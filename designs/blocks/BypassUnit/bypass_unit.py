from __future__ import annotations

from pycircuit import (
    CycleAwareCircuit,
    CycleAwareDomain,
    Tb,
    compile_cycle_aware,
    mux,
    testbench,
)

PTYPE_C = 0
PTYPE_P = 1
PTYPE_T = 2
PTYPE_U = 3


def _not1(m, x):
    return m.const(1, width=1) ^ x


def _select_stage(
    m,
    *,
    src_valid,
    src_ptag,
    src_ptype,
    lane_valid: list,
    lane_ptag: list,
    lane_ptype: list,
    lane_data: list,
    lanes: int,
    lane_w: int,
    data_w: int,
):
    has = m.const(0, width=1)
    sel_lane = m.const(0, width=int(lane_w))
    sel_data = m.const(0, width=int(data_w))

    for j in range(int(lanes)):
        match = (
            src_valid
            & lane_valid[j]
            & (lane_ptag[j] == src_ptag)
            & (lane_ptype[j] == src_ptype)
        )
        take = match & _not1(m, has)
        sel_lane = mux(take, m.const(j, width=int(lane_w)), sel_lane)
        sel_data = mux(take, lane_data[j], sel_data)
        has = has | match

    return has, sel_lane, sel_data


def _resolve_src(
    m,
    *,
    src_valid,
    src_ptag,
    src_ptype,
    src_rf_data,
    w1_valid: list,
    w1_ptag: list,
    w1_ptype: list,
    w1_data: list,
    w2_valid: list,
    w2_ptag: list,
    w2_ptype: list,
    w2_data: list,
    w3_valid: list,
    w3_ptag: list,
    w3_ptype: list,
    w3_data: list,
    lanes: int,
    lane_w: int,
    data_w: int,
):
    has_w1, lane_w1, data_w1 = _select_stage(
        m,
        src_valid=src_valid,
        src_ptag=src_ptag,
        src_ptype=src_ptype,
        lane_valid=w1_valid,
        lane_ptag=w1_ptag,
        lane_ptype=w1_ptype,
        lane_data=w1_data,
        lanes=lanes,
        lane_w=lane_w,
        data_w=data_w,
    )
    has_w2, lane_w2, data_w2 = _select_stage(
        m,
        src_valid=src_valid,
        src_ptag=src_ptag,
        src_ptype=src_ptype,
        lane_valid=w2_valid,
        lane_ptag=w2_ptag,
        lane_ptype=w2_ptype,
        lane_data=w2_data,
        lanes=lanes,
        lane_w=lane_w,
        data_w=data_w,
    )
    has_w3, lane_w3, data_w3 = _select_stage(
        m,
        src_valid=src_valid,
        src_ptag=src_ptag,
        src_ptype=src_ptype,
        lane_valid=w3_valid,
        lane_ptag=w3_ptag,
        lane_ptype=w3_ptype,
        lane_data=w3_data,
        lanes=lanes,
        lane_w=lane_w,
        data_w=data_w,
    )

    out_data = mux(has_w3, data_w3, src_rf_data)
    out_hit = mux(has_w3, m.const(1, width=1), m.const(0, width=1))
    out_stage = mux(has_w3, m.const(3, width=2), m.const(0, width=2))
    out_lane = mux(has_w3, lane_w3, m.const(0, width=int(lane_w)))

    out_data = mux(has_w2, data_w2, out_data)
    out_hit = mux(has_w2, m.const(1, width=1), out_hit)
    out_stage = mux(has_w2, m.const(2, width=2), out_stage)
    out_lane = mux(has_w2, lane_w2, out_lane)

    out_data = mux(has_w1, data_w1, out_data)
    out_hit = mux(has_w1, m.const(1, width=1), out_hit)
    out_stage = mux(has_w1, m.const(1, width=2), out_stage)
    out_lane = mux(has_w1, lane_w1, out_lane)

    return out_data, out_hit, out_stage, out_lane


def build(
    m: CycleAwareCircuit,
    domain: CycleAwareDomain,
    *,
    lanes: int = 8,
    data_width: int = 64,
    ptag_count: int = 256,
    ptype_count: int = 4,
) -> None:
    lanes_n = int(lanes)
    data_w = int(data_width)
    ptag_n = int(ptag_count)
    ptype_n = int(ptype_count)

    if lanes_n <= 0:
        raise ValueError("bypass_unit lanes must be > 0")
    if data_w <= 0:
        raise ValueError("bypass_unit data_width must be > 0")
    if ptag_n <= 0:
        raise ValueError("bypass_unit ptag_count must be > 0")
    if ptype_n <= 0:
        raise ValueError("bypass_unit ptype_count must be > 0")
    if ptype_n <= PTYPE_U:
        raise ValueError("bypass_unit ptype_count must be >= 4 to represent C/P/T/U")

    ptag_w = max(1, (ptag_n - 1).bit_length())
    ptype_w = max(1, (ptype_n - 1).bit_length())
    lane_w = max(1, (lanes_n - 1).bit_length())

    w_valid: dict[str, list] = {}
    w_ptag: dict[str, list] = {}
    w_ptype: dict[str, list] = {}
    w_data: dict[str, list] = {}
    for stage in ("w1", "w2", "w3"):
        w_valid[stage] = [m.input(f"{stage}{k}_valid", width=1) for k in range(lanes_n)]
        w_ptag[stage] = [
            m.input(f"{stage}{k}_ptag", width=ptag_w) for k in range(lanes_n)
        ]
        w_ptype[stage] = [
            m.input(f"{stage}{k}_ptype", width=ptype_w) for k in range(lanes_n)
        ]
        w_data[stage] = [
            m.input(f"{stage}{k}_data", width=data_w) for k in range(lanes_n)
        ]

    for i in range(lanes_n):
        for src in ("srcL", "srcR"):
            src_valid = m.input(f"i2{i}_{src}_valid", width=1)
            src_ptag = m.input(f"i2{i}_{src}_ptag", width=ptag_w)
            src_ptype = m.input(f"i2{i}_{src}_ptype", width=ptype_w)
            src_rf_data = m.input(f"i2{i}_{src}_rf_data", width=data_w)

            out_data, out_hit, out_stage, out_lane = _resolve_src(
                m,
                src_valid=src_valid,
                src_ptag=src_ptag,
                src_ptype=src_ptype,
                src_rf_data=src_rf_data,
                w1_valid=w_valid["w1"],
                w1_ptag=w_ptag["w1"],
                w1_ptype=w_ptype["w1"],
                w1_data=w_data["w1"],
                w2_valid=w_valid["w2"],
                w2_ptag=w_ptag["w2"],
                w2_ptype=w_ptype["w2"],
                w2_data=w_data["w2"],
                w3_valid=w_valid["w3"],
                w3_ptag=w_ptag["w3"],
                w3_ptype=w_ptype["w3"],
                w3_data=w_data["w3"],
                lanes=lanes_n,
                lane_w=lane_w,
                data_w=data_w,
            )

            m.output(f"i2{i}_{src}_data", out_data)
            m.output(f"i2{i}_{src}_hit", out_hit)
            m.output(f"i2{i}_{src}_sel_stage", out_stage)
            m.output(f"i2{i}_{src}_sel_lane", out_lane)


build.__pycircuit_name__ = "bypass_unit"


@testbench
def tb(t: Tb) -> None:
    t.clock("clk")
    t.reset("rst", cycles_asserted=1, cycles_deasserted=1)
    t.timeout(4)
    t.finish(at=0)


if __name__ == "__main__":
    print(
        compile_cycle_aware(
            build,
            name="bypass_unit",
            eager=True,
            lanes=8,
            data_width=64,
            ptag_count=256,
            ptype_count=4,
        ).emit_mlir()[:500]
    )
