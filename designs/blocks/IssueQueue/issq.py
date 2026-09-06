from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

from pycircuit import (
    Circuit,
    CycleAwareCircuit,
    CycleAwareDomain,
    compile_cycle_aware,
    function,
    u,
)

_THIS_DIR = Path(__file__).resolve().parent
if str(_THIS_DIR) not in sys.path:
    sys.path.insert(0, str(_THIS_DIR))

from issq_config import (  # noqa: E402
    _alloc_field,
    _count_ones,
    _derive_cfg,
    _entry_spec,
    _lane_lt,
    _not1,
    _onehot_mux,
    _ready_lookup,
    _slot_select,
    _uop_spec,
    _wake_hit,
)


@function
def _snapshot_entries(
    m: Circuit, entry_state: list, entries: int
) -> list[dict[str, Any]]:
    _ = m
    cur: list[dict[str, Any]] = []
    for i in range(int(entries)):
        s = entry_state[i]
        cur.append(
            {
                "valid": s["valid"].read(),
                "src0_valid": s["uop.src0.valid"].read(),
                "src0_ptag": s["uop.src0.ptag"].read(),
                "src0_ready": s["uop.src0.ready"].read(),
                "src1_valid": s["uop.src1.valid"].read(),
                "src1_ptag": s["uop.src1.ptag"].read(),
                "src1_ready": s["uop.src1.ready"].read(),
                "dst_valid": s["uop.dst.valid"].read(),
                "dst_ptag": s["uop.dst.ptag"].read(),
                "dst_ready": s["uop.dst.ready"].read(),
                "payload": s["uop.payload"].read(),
            }
        )
    return cur


@function
def _select_oldest_ready(
    m: Circuit,
    *,
    cur: list[dict[str, Any]],
    age_state: list[list],
    entries: int,
    issue_ports: int,
) -> tuple[list, list[list], list, list, list]:
    entry_ready = [
        cur[i]["valid"] & cur[i]["src0_ready"] & cur[i]["src1_ready"]
        for i in range(int(entries))
    ]

    issue_sel = [
        [u(1, 0) for _ in range(int(entries))] for _ in range(int(issue_ports))
    ]
    issue_valid = [u(1, 0) for _ in range(int(issue_ports))]

    remaining = list(entry_ready)
    for k in range(int(issue_ports)):
        oldest = []
        for i in range(int(entries)):
            older_exists = u(1, 0)
            for j in range(int(entries)):
                older_exists = older_exists | (remaining[j] & age_state[j][i].out())
            oldest_i = remaining[i] & _not1(m, older_exists)
            oldest.append(oldest_i)
        issue_sel[k] = oldest

        valid_k = u(1, 0)
        for i in range(int(entries)):
            valid_k = valid_k | oldest[i]
        issue_valid[k] = valid_k
        remaining = [remaining[i] & _not1(m, oldest[i]) for i in range(int(entries))]

    issue_win = [u(1, 0) for _ in range(int(entries))]
    for i in range(int(entries)):
        win_i = u(1, 0)
        for k in range(int(issue_ports)):
            win_i = win_i | issue_sel[k][i]
        issue_win[i] = win_i

    keep_valid = [cur[i]["valid"] & _not1(m, issue_win[i]) for i in range(int(entries))]
    return entry_ready, issue_sel, issue_valid, issue_win, keep_valid


@function
def _allocate_enqueue_lanes(
    m: Circuit,
    *,
    enq_valid: list,
    keep_valid: list,
    entries: int,
    enq_ports: int,
) -> tuple[list[list], list, list, list]:
    free_slot = [_not1(m, keep_valid[i]) for i in range(int(entries))]
    alloc_lane = [[u(1, 0) for _ in range(int(entries))] for _ in range(int(enq_ports))]
    enq_ready = [u(1, 0) for _ in range(int(enq_ports))]

    free_avail = list(free_slot)
    for k in range(int(enq_ports)):
        any_free = u(1, 0)
        for i in range(int(entries)):
            any_free = any_free | free_avail[i]
        enq_ready[k] = any_free

        first = [u(1, 0) for _ in range(int(entries))]
        lower_seen = u(1, 0)
        for i in range(int(entries)):
            first_i = free_avail[i] & _not1(m, lower_seen)
            first[i] = first_i
            lower_seen = lower_seen | free_avail[i]

        accept_k = enq_valid[k] & any_free
        for i in range(int(entries)):
            alloc_lane[k][i] = first[i] & accept_k
            free_avail[i] = free_avail[i] & _not1(m, alloc_lane[k][i])

    new_alloc = [u(1, 0) for _ in range(int(entries))]
    for i in range(int(entries)):
        slot_new = u(1, 0)
        for k in range(int(enq_ports)):
            slot_new = slot_new | alloc_lane[k][i]
        new_alloc[i] = slot_new

    next_valid = [keep_valid[i] | new_alloc[i] for i in range(int(entries))]
    return alloc_lane, enq_ready, new_alloc, next_valid


@function
def _issue_field_lists(
    m: Circuit, cur: list[dict[str, Any]], entries: int
) -> dict[str, list]:
    _ = m
    return {
        "src0.valid": [cur[i]["src0_valid"] for i in range(int(entries))],
        "src0.ptag": [cur[i]["src0_ptag"] for i in range(int(entries))],
        "src0.ready": [cur[i]["src0_ready"] for i in range(int(entries))],
        "src1.valid": [cur[i]["src1_valid"] for i in range(int(entries))],
        "src1.ptag": [cur[i]["src1_ptag"] for i in range(int(entries))],
        "src1.ready": [cur[i]["src1_ready"] for i in range(int(entries))],
        "dst.valid": [cur[i]["dst_valid"] for i in range(int(entries))],
        "dst.ptag": [cur[i]["dst_ptag"] for i in range(int(entries))],
        "dst.ready": [cur[i]["dst_ready"] for i in range(int(entries))],
        "payload": [cur[i]["payload"] for i in range(int(entries))],
    }


@function
def _emit_issue_ports(
    m: Circuit,
    *,
    uop_spec,
    issue_sel: list[list],
    issue_valid: list,
    field_lists: dict[str, list],
    ptag_width: int,
    payload_width: int,
    issue_ports: int,
) -> list[dict[str, Any]]:
    issue_uops: list[dict[str, Any]] = []
    for k in range(int(issue_ports)):
        vals = {
            "src0.valid": _onehot_mux(m, issue_sel[k], field_lists["src0.valid"], 1),
            "src0.ptag": _onehot_mux(
                m, issue_sel[k], field_lists["src0.ptag"], int(ptag_width)
            ),
            "src0.ready": _onehot_mux(m, issue_sel[k], field_lists["src0.ready"], 1),
            "src1.valid": _onehot_mux(m, issue_sel[k], field_lists["src1.valid"], 1),
            "src1.ptag": _onehot_mux(
                m, issue_sel[k], field_lists["src1.ptag"], int(ptag_width)
            ),
            "src1.ready": _onehot_mux(m, issue_sel[k], field_lists["src1.ready"], 1),
            "dst.valid": _onehot_mux(m, issue_sel[k], field_lists["dst.valid"], 1),
            "dst.ptag": _onehot_mux(
                m, issue_sel[k], field_lists["dst.ptag"], int(ptag_width)
            ),
            "dst.ready": _onehot_mux(m, issue_sel[k], field_lists["dst.ready"], 1),
            "payload": _onehot_mux(
                m, issue_sel[k], field_lists["payload"], int(payload_width)
            ),
        }
        issue_uops.append(vals)
        m.output(f"iss{k}_valid", issue_valid[k])
        m.outputs(uop_spec, vals, prefix=f"iss{k}_")
    return issue_uops


@function
def _issue_wake_vectors(
    m: Circuit, issue_valid: list, issue_uops: list[dict[str, Any]], issue_ports: int
) -> tuple[list, list]:
    _ = m
    wake_valid = [
        issue_valid[k] & issue_uops[k]["dst.valid"] for k in range(int(issue_ports))
    ]
    wake_ptag = [issue_uops[k]["dst.ptag"] for k in range(int(issue_ports))]
    return wake_valid, wake_ptag


@function
def _write_entry_next_state(
    m: Circuit,
    *,
    entry_state: list,
    cur: list[dict[str, Any]],
    enq_uops: list,
    alloc_lane: list[list],
    keep_valid: list,
    new_alloc: list,
    next_valid: list,
    wake_valid: list,
    wake_ptag: list,
    ready_state: list,
    entries: int,
    enq_ports: int,
    ptag_width: int,
    payload_width: int,
    ptag_count: int,
    issue_ports: int,
) -> None:
    for i in range(int(entries)):
        new_src0_valid = _alloc_field(
            m, enq_uops, alloc_lane, i, "src0.valid", 1, int(enq_ports)
        )
        new_src0_ptag = _alloc_field(
            m, enq_uops, alloc_lane, i, "src0.ptag", int(ptag_width), int(enq_ports)
        )
        new_src0_ready_in = _alloc_field(
            m, enq_uops, alloc_lane, i, "src0.ready", 1, int(enq_ports)
        )

        new_src1_valid = _alloc_field(
            m, enq_uops, alloc_lane, i, "src1.valid", 1, int(enq_ports)
        )
        new_src1_ptag = _alloc_field(
            m, enq_uops, alloc_lane, i, "src1.ptag", int(ptag_width), int(enq_ports)
        )
        new_src1_ready_in = _alloc_field(
            m, enq_uops, alloc_lane, i, "src1.ready", 1, int(enq_ports)
        )

        new_dst_valid = _alloc_field(
            m, enq_uops, alloc_lane, i, "dst.valid", 1, int(enq_ports)
        )
        new_dst_ptag = _alloc_field(
            m, enq_uops, alloc_lane, i, "dst.ptag", int(ptag_width), int(enq_ports)
        )
        new_dst_ready = _alloc_field(
            m, enq_uops, alloc_lane, i, "dst.ready", 1, int(enq_ports)
        )
        new_payload = _alloc_field(
            m, enq_uops, alloc_lane, i, "payload", int(payload_width), int(enq_ports)
        )

        keep_src0_ready = (
            cur[i]["src0_ready"]
            | _not1(m, cur[i]["src0_valid"])
            | _ready_lookup(
                m, ready_state, cur[i]["src0_ptag"], int(ptag_width), int(ptag_count)
            )
            | (
                cur[i]["src0_valid"]
                & _wake_hit(
                    m, wake_valid, wake_ptag, cur[i]["src0_ptag"], int(issue_ports)
                )
            )
        )
        keep_src1_ready = (
            cur[i]["src1_ready"]
            | _not1(m, cur[i]["src1_valid"])
            | _ready_lookup(
                m, ready_state, cur[i]["src1_ptag"], int(ptag_width), int(ptag_count)
            )
            | (
                cur[i]["src1_valid"]
                & _wake_hit(
                    m, wake_valid, wake_ptag, cur[i]["src1_ptag"], int(issue_ports)
                )
            )
        )

        new_src0_ready = (
            _not1(m, new_src0_valid)
            | new_src0_ready_in
            | _ready_lookup(
                m, ready_state, new_src0_ptag, int(ptag_width), int(ptag_count)
            )
            | (
                new_src0_valid
                & _wake_hit(m, wake_valid, wake_ptag, new_src0_ptag, int(issue_ports))
            )
        )
        new_src1_ready = (
            _not1(m, new_src1_valid)
            | new_src1_ready_in
            | _ready_lookup(
                m, ready_state, new_src1_ptag, int(ptag_width), int(ptag_count)
            )
            | (
                new_src1_valid
                & _wake_hit(m, wake_valid, wake_ptag, new_src1_ptag, int(issue_ports))
            )
        )

        st = entry_state[i]
        st["valid"].set(next_valid[i])
        st["uop.src0.valid"].set(
            _slot_select(
                m, keep_valid[i], new_alloc[i], cur[i]["src0_valid"], new_src0_valid, 1
            )
        )
        st["uop.src0.ptag"].set(
            _slot_select(
                m,
                keep_valid[i],
                new_alloc[i],
                cur[i]["src0_ptag"],
                new_src0_ptag,
                int(ptag_width),
            )
        )
        st["uop.src0.ready"].set(
            _slot_select(
                m, keep_valid[i], new_alloc[i], keep_src0_ready, new_src0_ready, 1
            )
        )

        st["uop.src1.valid"].set(
            _slot_select(
                m, keep_valid[i], new_alloc[i], cur[i]["src1_valid"], new_src1_valid, 1
            )
        )
        st["uop.src1.ptag"].set(
            _slot_select(
                m,
                keep_valid[i],
                new_alloc[i],
                cur[i]["src1_ptag"],
                new_src1_ptag,
                int(ptag_width),
            )
        )
        st["uop.src1.ready"].set(
            _slot_select(
                m, keep_valid[i], new_alloc[i], keep_src1_ready, new_src1_ready, 1
            )
        )

        st["uop.dst.valid"].set(
            _slot_select(
                m, keep_valid[i], new_alloc[i], cur[i]["dst_valid"], new_dst_valid, 1
            )
        )
        st["uop.dst.ptag"].set(
            _slot_select(
                m,
                keep_valid[i],
                new_alloc[i],
                cur[i]["dst_ptag"],
                new_dst_ptag,
                int(ptag_width),
            )
        )
        st["uop.dst.ready"].set(
            _slot_select(
                m, keep_valid[i], new_alloc[i], cur[i]["dst_ready"], new_dst_ready, 1
            )
        )

        st["uop.payload"].set(
            _slot_select(
                m,
                keep_valid[i],
                new_alloc[i],
                cur[i]["payload"],
                new_payload,
                int(payload_width),
            )
        )


@function
def _update_age_state(
    m: Circuit,
    *,
    age_state: list[list],
    keep_valid: list,
    new_alloc: list,
    next_valid: list,
    alloc_lane: list[list],
    entries: int,
    enq_ports: int,
) -> None:
    for i in range(int(entries)):
        for j in range(int(entries)):
            if i == j:
                age_state[i][j].set(u(1, 0))
            else:
                keep_keep = keep_valid[i] & keep_valid[j] & age_state[i][j].out()
                keep_new = keep_valid[i] & new_alloc[j]
                new_new = (
                    new_alloc[i]
                    & new_alloc[j]
                    & _lane_lt(m, alloc_lane, i, j, int(enq_ports))
                )
                rel = keep_keep | keep_new | new_new
                age_state[i][j].set(next_valid[i] & next_valid[j] & rel)


@function
def _update_ready_table(
    m: Circuit,
    *,
    ready_state: list,
    wake_valid: list,
    wake_ptag: list,
    ptag_count: int,
    ptag_width: int,
    issue_ports: int,
) -> None:
    _ = m
    for t in range(int(ptag_count)):
        wake_t = u(1, 0)
        t_const = u(int(ptag_width), t)
        for k in range(int(issue_ports)):
            wake_t = wake_t | (wake_valid[k] & (wake_ptag[k] == t_const))
        ready_state[t].set(ready_state[t].out() | wake_t)


@function
def _emit_debug_and_ready(
    m: Circuit,
    *,
    cur: list[dict[str, Any]],
    enq_ready: list,
    issue_valid: list,
    issued_total_q,
    entries: int,
    enq_ports: int,
    occupancy_width: int,
    issue_count_width: int,
    issued_total_width: int,
) -> None:
    occupancy = _count_ones(
        m, [cur[i]["valid"] for i in range(int(entries))], int(occupancy_width)
    )
    issued_this = _count_ones(m, issue_valid, int(issue_count_width))
    issued_total_q.set(
        (issued_total_q.out() + issued_this)[0 : int(issued_total_width)]
    )

    for k in range(int(enq_ports)):
        m.output(f"enq{k}_ready", enq_ready[k])
    m.output("occupancy", occupancy)
    m.output("issued_total", issued_total_q.out())


def build(
    m: CycleAwareCircuit,
    domain: CycleAwareDomain,
    *,
    entries: int = 16,
    ptag_count: int = 64,
    payload_width: int = 32,
    enq_ports: int = 2,
    issue_ports: int = 2,
    init_ready_mask: int = 0,
):
    cfg = _derive_cfg(
        m,
        entries=entries,
        ptag_count=ptag_count,
        payload_width=payload_width,
        enq_ports=enq_ports,
        issue_ports=issue_ports,
        init_ready_mask=init_ready_mask,
    )

    e = int(cfg.entries)
    p = int(cfg.ptag_count)
    ptag_w = int(cfg.ptag_width)
    payload_w = int(cfg.payload_width)
    n_enq = int(cfg.enq_ports)
    n_issue = int(cfg.issue_ports)
    occ_w = int(cfg.occupancy_width)
    issue_cnt_w = int(cfg.issue_count_width)
    issued_total_w = int(cfg.issued_total_width)
    cd = domain.clock_domain
    uop_spec = _uop_spec(m, cfg)
    entry_spec = _entry_spec(m, cfg)

    enq_valid = [m.input(f"enq{k}_valid", width=1) for k in range(n_enq)]
    enq_uops = [m.inputs(uop_spec, prefix=f"enq{k}_") for k in range(n_enq)]

    entry_state = [
        m.state(entry_spec, clk=cd.clk, rst=cd.rst, prefix=f"ent{i}_", init=0)
        for i in range(e)
    ]

    age_state = [
        [m.out(f"age_{i}_{j}", domain=cd, width=1, init=u(1, 0)) for j in range(e)]
        for i in range(e)
    ]

    ready_state = [
        m.out(
            f"ready_ptag_{t}",
            domain=cd,
            width=1,
            init=u(1, (int(cfg.init_ready_mask) >> t) & 1),
        )
        for t in range(p)
    ]

    issued_total_q = m.out(
        "issued_total_q", domain=cd, width=issued_total_w, init=u(issued_total_w, 0)
    )

    cur = _snapshot_entries(m, entry_state, e)

    _entry_ready, issue_sel, issue_valid, _issue_win, keep_valid = _select_oldest_ready(
        m,
        cur=cur,
        age_state=age_state,
        entries=e,
        issue_ports=n_issue,
    )

    alloc_lane, enq_ready, new_alloc, next_valid = _allocate_enqueue_lanes(
        m,
        enq_valid=enq_valid,
        keep_valid=keep_valid,
        entries=e,
        enq_ports=n_enq,
    )

    issue_uops = _emit_issue_ports(
        m,
        uop_spec=uop_spec,
        issue_sel=issue_sel,
        issue_valid=issue_valid,
        field_lists=_issue_field_lists(m, cur, e),
        ptag_width=ptag_w,
        payload_width=payload_w,
        issue_ports=n_issue,
    )
    wake_valid, wake_ptag = _issue_wake_vectors(m, issue_valid, issue_uops, n_issue)

    _write_entry_next_state(
        m,
        entry_state=entry_state,
        cur=cur,
        enq_uops=enq_uops,
        alloc_lane=alloc_lane,
        keep_valid=keep_valid,
        new_alloc=new_alloc,
        next_valid=next_valid,
        wake_valid=wake_valid,
        wake_ptag=wake_ptag,
        ready_state=ready_state,
        entries=e,
        enq_ports=n_enq,
        ptag_width=ptag_w,
        payload_width=payload_w,
        ptag_count=p,
        issue_ports=n_issue,
    )

    _update_age_state(
        m,
        age_state=age_state,
        keep_valid=keep_valid,
        new_alloc=new_alloc,
        next_valid=next_valid,
        alloc_lane=alloc_lane,
        entries=e,
        enq_ports=n_enq,
    )
    _update_ready_table(
        m,
        ready_state=ready_state,
        wake_valid=wake_valid,
        wake_ptag=wake_ptag,
        ptag_count=p,
        ptag_width=ptag_w,
        issue_ports=n_issue,
    )

    _emit_debug_and_ready(
        m,
        cur=cur,
        enq_ready=enq_ready,
        issue_valid=issue_valid,
        issued_total_q=issued_total_q,
        entries=e,
        enq_ports=n_enq,
        occupancy_width=occ_w,
        issue_count_width=issue_cnt_w,
        issued_total_width=issued_total_w,
    )


if __name__ == "__main__":
    print(
        compile_cycle_aware(
            build,
            name="issq",
            entries=16,
            ptag_count=64,
            payload_width=32,
            enq_ports=2,
            issue_ports=2,
            init_ready_mask=0,
        ).emit_mlir()
    )
