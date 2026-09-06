from __future__ import annotations

import sys
from pathlib import Path

from pycircuit import (
    CycleAwareTb,
    Tb,
    compile_cycle_aware,
    testbench,
)
from pycircuit.tb import sva

_THIS_DIR = Path(__file__).resolve().parent
if str(_THIS_DIR) not in sys.path:
    sys.path.insert(0, str(_THIS_DIR))

from bypass_unit import PTYPE_C, PTYPE_P, PTYPE_T, PTYPE_U, build  # noqa: E402

_STAGES = ("w1", "w2", "w3")
_SRCS = ("srcL", "srcR")


def _base_cycle(cyc: int, *, lanes: int, ptag_count: int) -> dict:
    mask64 = (1 << 64) - 1
    wb = {
        stage: [{"valid": 0, "ptag": 0, "ptype": 0, "data": 0} for _ in range(lanes)]
        for stage in _STAGES
    }
    i2 = []
    for i in range(lanes):
        tag_l = (32 + cyc * 19 + i * 2) % ptag_count
        tag_r = (33 + cyc * 19 + i * 2) % ptag_count
        rf_l = (((cyc + 1) << 48) | (i << 8) | 0x4C) & mask64
        rf_r = (((cyc + 1) << 48) | (i << 8) | 0x52) & mask64
        i2.append(
            {
                "srcL": {"valid": 1, "ptag": tag_l, "ptype": PTYPE_P, "rf_data": rf_l},
                "srcR": {"valid": 1, "ptag": tag_r, "ptype": PTYPE_T, "rf_data": rf_r},
            }
        )
    return {"wb": wb, "i2": i2}


def _resolve_expected(src: dict, wb: dict, *, lanes: int) -> tuple[int, int, int, int]:
    src_valid = int(src["valid"])
    src_ptag = int(src["ptag"])
    src_ptype = int(src["ptype"])
    src_rf_data = int(src["rf_data"])

    if src_valid == 0:
        return src_rf_data, 0, 0, 0

    for stage_id, stage in ((1, "w1"), (2, "w2"), (3, "w3")):
        for lane in range(lanes):
            w = wb[stage][lane]
            if int(w["valid"]) == 0:
                continue
            if int(w["ptag"]) != src_ptag:
                continue
            if int(w["ptype"]) != src_ptype:
                continue
            return int(w["data"]), 1, stage_id, lane

    return src_rf_data, 0, 0, 0


def _drive_cycle(tb: CycleAwareTb, spec: dict, *, lanes: int) -> None:
    wb = spec["wb"]
    i2 = spec["i2"]

    for stage in _STAGES:
        for lane in range(lanes):
            w = wb[stage][lane]
            tb.drive(f"{stage}{lane}_valid", int(w["valid"]))
            tb.drive(f"{stage}{lane}_ptag", int(w["ptag"]))
            tb.drive(f"{stage}{lane}_ptype", int(w["ptype"]))
            tb.drive(f"{stage}{lane}_data", int(w["data"]))

    for i in range(lanes):
        for src in _SRCS:
            s = i2[i][src]
            tb.drive(f"i2{i}_{src}_valid", int(s["valid"]))
            tb.drive(f"i2{i}_{src}_ptag", int(s["ptag"]))
            tb.drive(f"i2{i}_{src}_ptype", int(s["ptype"]))
            tb.drive(f"i2{i}_{src}_rf_data", int(s["rf_data"]))


def _expect_cycle(tb: CycleAwareTb, cyc: int, spec: dict, *, lanes: int) -> None:
    wb = spec["wb"]
    i2 = spec["i2"]
    for i in range(lanes):
        for src in _SRCS:
            exp_data, exp_hit, exp_stage, exp_lane = _resolve_expected(
                i2[i][src], wb, lanes=lanes
            )
            tb.expect(
                f"i2{i}_{src}_data",
                exp_data,
                msg=f"data mismatch lane={i} src={src} cycle={cyc}",
            )
            tb.expect(
                f"i2{i}_{src}_hit",
                exp_hit,
                msg=f"hit mismatch lane={i} src={src} cycle={cyc}",
            )
            tb.expect(
                f"i2{i}_{src}_sel_stage",
                exp_stage,
                msg=f"sel_stage mismatch lane={i} src={src} cycle={cyc}",
            )
            tb.expect(
                f"i2{i}_{src}_sel_lane",
                exp_lane,
                msg=f"sel_lane mismatch lane={i} src={src} cycle={cyc}",
            )


def _match_expr(stage: str, lane: int, i2_lane: int, src: str):
    return (
        sva.id(f"i2{i2_lane}_{src}_valid")
        & sva.id(f"{stage}{lane}_valid")
        & (sva.id(f"{stage}{lane}_ptag") == sva.id(f"i2{i2_lane}_{src}_ptag"))
        & (sva.id(f"{stage}{lane}_ptype") == sva.id(f"i2{i2_lane}_{src}_ptype"))
    )


def _single_hit_case(
    case_id: int,
    *,
    lanes: int,
    ptag_count: int,
    i2_lane: int,
    src: str,
    stage: str,
    wb_lane: int,
) -> dict:
    spec = _base_cycle(100 + case_id, lanes=lanes, ptag_count=ptag_count)
    src_idx = 0 if src == "srcL" else 1
    stage_idx = _STAGES.index(stage)
    ptag = (17 + case_id * 11 + i2_lane * 7 + wb_lane * 5) % ptag_count
    ptype = (case_id + src_idx + stage_idx) & 0x3
    data = (
        ((case_id + 1) << 24)
        ^ (stage_idx << 20)
        ^ (wb_lane << 12)
        ^ (i2_lane << 4)
        ^ src_idx
    )
    data &= (1 << 64) - 1

    spec["i2"][i2_lane][src]["valid"] = 1
    spec["i2"][i2_lane][src]["ptag"] = ptag
    spec["i2"][i2_lane][src]["ptype"] = ptype
    spec["wb"][stage][wb_lane] = {
        "valid": 1,
        "ptag": ptag,
        "ptype": ptype,
        "data": data,
    }

    # Distractors: same tag with different type on non-selected stages.
    for os_idx, other_stage in enumerate(_STAGES):
        if other_stage == stage:
            continue
        lane = (wb_lane + 1 + os_idx) % lanes
        spec["wb"][other_stage][lane] = {
            "valid": 1,
            "ptag": ptag,
            "ptype": (ptype + 1) & 0x3,
            "data": data ^ (0x55 << (8 * os_idx)),
        }

    return spec


def _gen_single_hit_sweep(*, lanes: int, ptag_count: int) -> list[dict]:
    out: list[dict] = []
    case_id = 0
    for i2_lane in range(lanes):
        for src in _SRCS:
            for stage_idx, stage in enumerate(_STAGES):
                wb_lane_a = (i2_lane + stage_idx + (0 if src == "srcL" else 3)) % lanes
                wb_lane_b = (
                    i2_lane * 3 + stage_idx + (1 if src == "srcL" else 5)
                ) % lanes
                if wb_lane_b == wb_lane_a:
                    wb_lane_b = (wb_lane_b + 1) % lanes
                out.append(
                    _single_hit_case(
                        case_id,
                        lanes=lanes,
                        ptag_count=ptag_count,
                        i2_lane=i2_lane,
                        src=src,
                        stage=stage,
                        wb_lane=wb_lane_a,
                    )
                )
                case_id += 1
                out.append(
                    _single_hit_case(
                        case_id,
                        lanes=lanes,
                        ptag_count=ptag_count,
                        i2_lane=i2_lane,
                        src=src,
                        stage=stage,
                        wb_lane=wb_lane_b,
                    )
                )
                case_id += 1
    return out


def _gen_priority_sweep(*, lanes: int, ptag_count: int) -> list[dict]:
    out: list[dict] = []
    case_id = 0
    for i2_lane in range(lanes):
        for src_idx, src in enumerate(_SRCS):
            ptag = (200 + case_id * 9 + i2_lane * 5 + src_idx) % ptag_count
            ptype = (case_id + src_idx) & 0x3
            w1_lane = (i2_lane + 1) % lanes
            w2_lane = (i2_lane + 3) % lanes
            w3_lane = (i2_lane + 5) % lanes

            # W1 vs W2 vs W3 all match: W1 must win.
            spec = _base_cycle(1000 + case_id, lanes=lanes, ptag_count=ptag_count)
            spec["i2"][i2_lane][src]["valid"] = 1
            spec["i2"][i2_lane][src]["ptag"] = ptag
            spec["i2"][i2_lane][src]["ptype"] = ptype
            spec["wb"]["w1"][w1_lane] = {
                "valid": 1,
                "ptag": ptag,
                "ptype": ptype,
                "data": 0x1111_0000_0000_0000 | case_id,
            }
            spec["wb"]["w2"][w2_lane] = {
                "valid": 1,
                "ptag": ptag,
                "ptype": ptype,
                "data": 0x2222_0000_0000_0000 | case_id,
            }
            spec["wb"]["w3"][w3_lane] = {
                "valid": 1,
                "ptag": ptag,
                "ptype": ptype,
                "data": 0x3333_0000_0000_0000 | case_id,
            }
            out.append(spec)
            case_id += 1

            # W2 vs W3 match with W1 mismatch: W2 must win.
            spec = _base_cycle(2000 + case_id, lanes=lanes, ptag_count=ptag_count)
            spec["i2"][i2_lane][src]["valid"] = 1
            spec["i2"][i2_lane][src]["ptag"] = ptag
            spec["i2"][i2_lane][src]["ptype"] = ptype
            spec["wb"]["w1"][w1_lane] = {
                "valid": 1,
                "ptag": ptag,
                "ptype": (ptype + 1) & 0x3,
                "data": 0xAAAA_0000_0000_0000 | case_id,
            }
            spec["wb"]["w2"][w2_lane] = {
                "valid": 1,
                "ptag": ptag,
                "ptype": ptype,
                "data": 0xBBBB_0000_0000_0000 | case_id,
            }
            spec["wb"]["w3"][w3_lane] = {
                "valid": 1,
                "ptag": ptag,
                "ptype": ptype,
                "data": 0xCCCC_0000_0000_0000 | case_id,
            }
            out.append(spec)
            case_id += 1
    return out


def _gen_invalid_source_sweep(*, lanes: int, ptag_count: int) -> list[dict]:
    out: list[dict] = []
    case_id = 0
    for i2_lane in range(lanes):
        for src_idx, src in enumerate(_SRCS):
            spec = _base_cycle(3000 + case_id, lanes=lanes, ptag_count=ptag_count)
            ptag = (63 + case_id * 13 + i2_lane * 7) % ptag_count
            ptype = (case_id + src_idx + 2) & 0x3
            spec["i2"][i2_lane][src]["valid"] = 0
            spec["i2"][i2_lane][src]["ptag"] = ptag
            spec["i2"][i2_lane][src]["ptype"] = ptype
            spec["wb"]["w1"][(i2_lane + 0) % lanes] = {
                "valid": 1,
                "ptag": ptag,
                "ptype": ptype,
                "data": 0x9000_0000_0000_0000 | case_id,
            }
            spec["wb"]["w2"][(i2_lane + 2) % lanes] = {
                "valid": 1,
                "ptag": ptag,
                "ptype": ptype,
                "data": 0xA000_0000_0000_0000 | case_id,
            }
            spec["wb"]["w3"][(i2_lane + 4) % lanes] = {
                "valid": 1,
                "ptag": ptag,
                "ptype": ptype,
                "data": 0xB000_0000_0000_0000 | case_id,
            }
            out.append(spec)
            case_id += 1
    return out


def _lcg64(state: int) -> int:
    return (state * 6364136223846793005 + 1) & ((1 << 64) - 1)


def _pick_nomatch_pair(
    *, state: int, used_pairs: set[tuple[int, int]], ptag_count: int
) -> tuple[int, int, int]:
    for _ in range(ptag_count * 4 + 8):
        state = _lcg64(state)
        tag = state % ptag_count
        state = _lcg64(state)
        ptype = state & 0x3
        if (tag, ptype) not in used_pairs:
            return state, tag, ptype
    return state, 0, 0


def _gen_random_stress(
    *, lanes: int, ptag_count: int, count: int, seed: int
) -> list[dict]:
    out: list[dict] = []
    state = int(seed) & ((1 << 64) - 1)
    mask64 = (1 << 64) - 1

    for idx in range(count):
        spec = _base_cycle(4000 + idx, lanes=lanes, ptag_count=ptag_count)
        stage_entries: dict[str, list[tuple[int, int, int, int]]] = {
            s: [] for s in _STAGES
        }
        pair_sets: dict[str, set[tuple[int, int]]] = {s: set() for s in _STAGES}
        all_pairs: set[tuple[int, int]] = set()

        for s_idx, stage in enumerate(_STAGES):
            for lane in range(lanes):
                state = _lcg64(state)
                valid = 1 if (state & 0x3) != 0 else 0
                if valid == 0:
                    continue
                for _ in range(ptag_count * 4 + 8):
                    state = _lcg64(state)
                    tag = state % ptag_count
                    state = _lcg64(state)
                    ptype = state & 0x3
                    pair = (tag, ptype)
                    if pair not in pair_sets[stage]:
                        break
                pair_sets[stage].add(pair)
                all_pairs.add(pair)
                state = _lcg64(state)
                data = (
                    (state << 1) ^ (idx << 24) ^ (s_idx << 20) ^ (lane << 7)
                ) & mask64
                spec["wb"][stage][lane] = {
                    "valid": 1,
                    "ptag": tag,
                    "ptype": ptype,
                    "data": data,
                }
                stage_entries[stage].append((lane, tag, ptype, data))

        inter12 = sorted(pair_sets["w1"] & pair_sets["w2"])
        inter123 = sorted(pair_sets["w1"] & pair_sets["w2"] & pair_sets["w3"])

        for i2_lane in range(lanes):
            for src in _SRCS:
                s = spec["i2"][i2_lane][src]
                state = _lcg64(state)
                s["rf_data"] = (
                    (state << 9)
                    ^ (idx << 16)
                    ^ (i2_lane << 3)
                    ^ (0 if src == "srcL" else 1)
                ) & mask64
                mode = state % 6

                if mode == 0:
                    s["valid"] = 0
                    if all_pairs:
                        pairs = sorted(all_pairs)
                        pair = pairs[state % len(pairs)]
                        s["ptag"], s["ptype"] = pair
                    else:
                        state, tag, ptype = _pick_nomatch_pair(
                            state=state, used_pairs=all_pairs, ptag_count=ptag_count
                        )
                        s["ptag"] = tag
                        s["ptype"] = ptype
                    continue

                s["valid"] = 1
                if mode == 1 and stage_entries["w1"]:
                    pick = stage_entries["w1"][state % len(stage_entries["w1"])]
                    s["ptag"], s["ptype"] = pick[1], pick[2]
                elif mode == 2 and stage_entries["w2"]:
                    pick = stage_entries["w2"][state % len(stage_entries["w2"])]
                    s["ptag"], s["ptype"] = pick[1], pick[2]
                elif mode == 3 and stage_entries["w3"]:
                    pick = stage_entries["w3"][state % len(stage_entries["w3"])]
                    s["ptag"], s["ptype"] = pick[1], pick[2]
                elif mode == 4 and inter12:
                    pair = (
                        inter123[state % len(inter123)]
                        if inter123
                        else inter12[state % len(inter12)]
                    )
                    s["ptag"], s["ptype"] = pair
                else:
                    state, tag, ptype = _pick_nomatch_pair(
                        state=state, used_pairs=all_pairs, ptag_count=ptag_count
                    )
                    s["ptag"] = tag
                    s["ptype"] = ptype

        out.append(spec)

    return out


@testbench
def tb(t: Tb) -> None:
    tb = CycleAwareTb(t)
    lanes = 8
    ptag_count = 256

    cycles = [_base_cycle(cyc, lanes=lanes, ptag_count=ptag_count) for cyc in range(8)]

    # c1: single W1 hit for srcL.
    cycles[1]["i2"][5]["srcL"]["ptag"] = 41
    cycles[1]["i2"][5]["srcL"]["ptype"] = PTYPE_T
    cycles[1]["wb"]["w1"][2] = {
        "valid": 1,
        "ptag": 41,
        "ptype": PTYPE_T,
        "data": 0x1111_0000_0000_0001,
    }

    # c2: single W2 hit for srcR.
    cycles[2]["i2"][1]["srcR"]["ptag"] = 55
    cycles[2]["i2"][1]["srcR"]["ptype"] = PTYPE_U
    cycles[2]["wb"]["w2"][7] = {
        "valid": 1,
        "ptag": 55,
        "ptype": PTYPE_U,
        "data": 0x2222_0000_0000_0002,
    }

    # c3: single W3 hit for srcL.
    cycles[3]["i2"][0]["srcL"]["ptag"] = 66
    cycles[3]["i2"][0]["srcL"]["ptype"] = PTYPE_P
    cycles[3]["wb"]["w3"][4] = {
        "valid": 1,
        "ptag": 66,
        "ptype": PTYPE_P,
        "data": 0x3333_0000_0000_0003,
    }

    # c4: simultaneous cross-stage hits; W1 must win over W2/W3.
    cycles[4]["i2"][3]["srcR"]["ptag"] = 77
    cycles[4]["i2"][3]["srcR"]["ptype"] = PTYPE_P
    cycles[4]["wb"]["w3"][6] = {
        "valid": 1,
        "ptag": 77,
        "ptype": PTYPE_P,
        "data": 0xBBBB_0000_0000_0003,
    }
    cycles[4]["wb"]["w2"][2] = {
        "valid": 1,
        "ptag": 77,
        "ptype": PTYPE_P,
        "data": 0xAAAA_0000_0000_0002,
    }
    cycles[4]["wb"]["w1"][5] = {
        "valid": 1,
        "ptag": 77,
        "ptype": PTYPE_P,
        "data": 0x9999_0000_0000_0001,
    }

    # c5: same ptag but type mismatch; no bypass.
    cycles[5]["i2"][2]["srcL"]["ptag"] = 88
    cycles[5]["i2"][2]["srcL"]["ptype"] = PTYPE_T
    cycles[5]["wb"]["w1"][1] = {
        "valid": 1,
        "ptag": 88,
        "ptype": PTYPE_P,
        "data": 0xDEAD_BEEF_DEAD_BEEF,
    }
    cycles[5]["wb"]["w2"][0] = {
        "valid": 1,
        "ptag": 88,
        "ptype": PTYPE_U,
        "data": 0xFEED_FACE_FEED_FACE,
    }

    # c6: src invalid, match exists, bypass must stay disabled.
    cycles[6]["i2"][6]["srcR"]["valid"] = 0
    cycles[6]["i2"][6]["srcR"]["ptag"] = 99
    cycles[6]["i2"][6]["srcR"]["ptype"] = PTYPE_U
    cycles[6]["wb"]["w1"][0] = {
        "valid": 1,
        "ptag": 99,
        "ptype": PTYPE_U,
        "data": 0xCCCC_0000_0000_0006,
    }

    # c7: source-id checks for selected stage and lane on both srcs.
    cycles[7]["i2"][4]["srcL"]["ptag"] = 120
    cycles[7]["i2"][4]["srcL"]["ptype"] = PTYPE_C
    cycles[7]["wb"]["w2"][6] = {
        "valid": 1,
        "ptag": 120,
        "ptype": PTYPE_C,
        "data": 0xAAAA_BBBB_CCCC_DDDD,
    }
    cycles[7]["i2"][4]["srcR"]["ptag"] = 121
    cycles[7]["i2"][4]["srcR"]["ptype"] = PTYPE_U
    cycles[7]["wb"]["w3"][3] = {
        "valid": 1,
        "ptag": 121,
        "ptype": PTYPE_U,
        "data": 0x1111_2222_3333_4444,
    }

    # Systematic stress suites.
    cycles.extend(_gen_single_hit_sweep(lanes=lanes, ptag_count=ptag_count))
    cycles.extend(_gen_priority_sweep(lanes=lanes, ptag_count=ptag_count))
    cycles.extend(_gen_invalid_source_sweep(lanes=lanes, ptag_count=ptag_count))
    cycles.extend(
        _gen_random_stress(
            lanes=lanes, ptag_count=ptag_count, count=32, seed=0xD1CE_BA5E_F00D_CAFE
        )
    )

    tb.clock("clk")
    tb.reset("rst", cycles_asserted=2, cycles_deasserted=1)
    tb.timeout(len(cycles) + 64)
    tb.print_every("bypass", start=0, every=32, ports=["i20_srcL_hit", "i20_srcR_hit"])

    for i in range(lanes):
        for src in _SRCS:
            for stage in _STAGES:
                for a in range(lanes):
                    for b in range(a + 1, lanes):
                        match_a = _match_expr(stage, a, i, src)
                        match_b = _match_expr(stage, b, i, src)
                        tb.sva_assert(
                            ~(match_a & match_b),
                            clock="clk",
                            reset="rst",
                            name=f"no_conflict_{stage}_{src}_{i}_{a}_{b}",
                            msg=f"illegal same-stage multihit stage={stage} src={src} lane={i}",
                        )

    # --- cycle 0 ---
    for cyc, spec in enumerate(cycles):
        if cyc > 0:
            tb.next()  # --- advance to next cycle ---
        _drive_cycle(tb, spec, lanes=lanes)
        _expect_cycle(tb, cyc, spec, lanes=lanes)

    tb.finish()


if __name__ == "__main__":
    print(
        compile_cycle_aware(
            build,
            name="tb_bypass_unit_top",
            lanes=8,
            data_width=64,
            ptag_count=256,
            ptype_count=4,
        ).emit_mlir()
    )
