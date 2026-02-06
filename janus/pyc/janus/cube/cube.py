from __future__ import annotations

from pycircuit import Circuit, Wire, jit_inline

from janus.cube.cube_consts import (
    ADDR_CONTROL,
    ADDR_MATRIX_A,
    ADDR_MATRIX_C,
    ADDR_MATRIX_W,
    ADDR_STATUS,
    ARRAY_SIZE,
    ST_COMPUTE,
    ST_DONE,
    ST_DRAIN,
    ST_IDLE,
    ST_LOAD_WEIGHTS,
)
from janus.cube.cube_types import CubeState, FsmResult, MmioWriteResult, PERegs
from janus.cube.util import Consts, make_consts


def _make_pe_regs(m: Circuit, clk: Wire, rst: Wire, consts: Consts) -> list[list[PERegs]]:
    """Create 16×16 PE register array using JIT-unrolled loops."""
    pe_array = []
    for row in range(ARRAY_SIZE):
        pe_row = []
        for col in range(ARRAY_SIZE):
            with m.scope(f"pe_r{row}_c{col}"):
                pe_row.append(PERegs(
                    weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=consts.one1),
                    acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=consts.one1),
                ))
        pe_array.append(pe_row)
    return pe_array


def _make_result_regs(m: Circuit, clk: Wire, rst: Wire, consts: Consts) -> list:
    """Create 256 result registers."""
    result_regs = []
    for i in range(ARRAY_SIZE * ARRAY_SIZE):
        with m.scope(f"result_{i}"):
            result_regs.append(m.out("value", clk=clk, rst=rst, width=32, init=0, en=consts.one1))
    return result_regs


def _make_weight_regs(m: Circuit, clk: Wire, rst: Wire, consts: Consts) -> list:
    """Create 256 weight registers."""
    weights = []
    for i in range(ARRAY_SIZE * ARRAY_SIZE):
        with m.scope(f"weight_{i}"):
            weights.append(m.out("value", clk=clk, rst=rst, width=16, init=0, en=consts.one1))
    return weights


def _make_activation_regs(m: Circuit, clk: Wire, rst: Wire, consts: Consts) -> list:
    """Create 16 activation registers."""
    activations = []
    for i in range(ARRAY_SIZE):
        with m.scope(f"activation_{i}"):
            activations.append(m.out("value", clk=clk, rst=rst, width=16, init=0, en=consts.one1))
    return activations


def _get_reg_outputs(regs: list) -> list[Wire]:
    """Get output wires from a list of registers."""
    return [r.out() for r in regs]


def _store_results(result_regs: list, results: list[Wire], done: Wire) -> None:
    """Store computation results into result registers."""
    for i in range(ARRAY_SIZE * ARRAY_SIZE):
        result_regs[i].set(results[i], when=done)


@jit_inline
def _build_pe(
    m: Circuit,
    *,
    load_weight: Wire,
    compute: Wire,
    weight_in: Wire,
    activation: Wire,
    partial_sum_in: Wire,
    pe_regs: PERegs,
) -> tuple[Wire, Wire]:
    """Build a single processing element."""
    pe_regs.weight.set(weight_in, when=load_weight)

    weight = pe_regs.weight.out()
    # NOTE: Using addition as placeholder for multiplication
    # TODO: Replace with proper multiplication when pyCircuit supports it
    product = weight.zext(width=32) + activation.zext(width=32)
    acc_next = pe_regs.acc.out() + product + partial_sum_in

    pe_regs.acc.set(acc_next, when=compute)

    partial_sum_out = acc_next
    result = pe_regs.acc.out()
    return partial_sum_out, result


def _build_array(
    m: Circuit,
    *,
    load_weight: Wire,
    compute: Wire,
    weights: list[Wire],
    activations: list[Wire],
    pe_array: list[list[PERegs]],
    consts: Consts,
) -> list[Wire]:
    """Build 16×16 systolic array."""
    with m.scope("ARRAY"):
        results = []
        for row in range(ARRAY_SIZE):
            partial_sum = consts.zero32
            for col in range(ARRAY_SIZE):
                pe_idx = row * ARRAY_SIZE + col
                with m.scope(f"r{row}_c{col}"):
                    partial_sum, result = _build_pe(
                        m,
                        load_weight=load_weight,
                        compute=compute,
                        weight_in=weights[pe_idx],
                        activation=activations[col],
                        partial_sum_in=partial_sum,
                        pe_regs=pe_array[row][col],
                    )
                    results.append(result)
        return results


@jit_inline
def _build_fsm(
    m: Circuit,
    *,
    start: Wire,
    reset_cube: Wire,
    state: CubeState,
    consts: Consts,
) -> FsmResult:
    """Build control FSM. Returns FsmResult(load_weight, compute, done)."""
    with m.scope("FSM"):
        c = m.const
        current_state = state.state.out()
        cycle_count = state.cycle_count.out()

        # State decode
        state_is_idle = current_state.eq(c(ST_IDLE, width=3))
        state_is_load = current_state.eq(c(ST_LOAD_WEIGHTS, width=3))
        state_is_compute = current_state.eq(c(ST_COMPUTE, width=3))
        state_is_drain = current_state.eq(c(ST_DRAIN, width=3))
        state_is_done = current_state.eq(c(ST_DONE, width=3))

        # Next state logic
        next_state = current_state

        if state_is_idle:
            if start:
                next_state = c(ST_LOAD_WEIGHTS, width=3)

        if state_is_load:
            if cycle_count == 0:
                next_state = c(ST_COMPUTE, width=3)

        if state_is_compute:
            if cycle_count == (ARRAY_SIZE - 1):
                next_state = c(ST_DRAIN, width=3)

        if state_is_drain:
            if cycle_count == (ARRAY_SIZE - 2):
                next_state = c(ST_DONE, width=3)

        if state_is_done:
            if reset_cube:
                next_state = c(ST_IDLE, width=3)

        state.state.set(next_state)

        # Cycle counter
        counter_reset = (
            state_is_idle
            | (state_is_load & cycle_count.eq(c(0, width=8)))
            | (state_is_compute & cycle_count.eq(c(ARRAY_SIZE - 1, width=8)))
            | (state_is_drain & cycle_count.eq(c(ARRAY_SIZE - 2, width=8)))
        )
        next_count = counter_reset.select(consts.zero8, cycle_count + consts.one8)
        state.cycle_count.set(next_count)

        # Control signals
        load_weight = state_is_load
        compute = state_is_compute | state_is_drain
        done = state_is_done

        # Status flags
        state.done.set(done)
        state.busy.set(~state_is_idle & ~state_is_done)

    return FsmResult(load_weight=load_weight, compute=compute, done=done)


def _build_mmio_read(
    m: Circuit,
    *,
    mem_raddr: Wire,
    base_addr: int,
    state: CubeState,
    result_regs: list,
    consts: Consts,
) -> Wire:
    """Build memory-mapped read logic."""
    with m.scope("MMIO_RD"):
        c = m.const
        rdata = consts.zero64

        # Status register
        addr_match = mem_raddr.eq(c(base_addr + ADDR_STATUS, width=64))
        status = state.done.out().zext(width=64) | (state.busy.out().zext(width=64) << 1)
        rdata = addr_match.select(status, rdata)

        # Result matrix (256 × 32-bit values)
        for i in range(ARRAY_SIZE * ARRAY_SIZE):
            addr_match = mem_raddr.eq(c(base_addr + ADDR_MATRIX_C + i * 4, width=64))
            rdata = addr_match.select(result_regs[i].out().zext(width=64), rdata)

        return rdata


def _build_mmio_write(
    m: Circuit,
    *,
    mem_wvalid: Wire,
    mem_waddr: Wire,
    mem_wdata: Wire,
    base_addr: int,
    weights: list,
    activations: list,
    consts: Consts,
) -> MmioWriteResult:
    """Build memory-mapped write logic. Returns MmioWriteResult(start, reset_cube)."""
    with m.scope("MMIO_WR"):
        c = m.const

        # Control register
        ctrl_match = mem_waddr.eq(c(base_addr + ADDR_CONTROL, width=64)) & mem_wvalid
        start = ctrl_match & mem_wdata[0]
        reset_cube = ctrl_match & mem_wdata[1]

        # Weight matrix (256 × 16-bit values)
        for i in range(ARRAY_SIZE * ARRAY_SIZE):
            addr_match = mem_waddr.eq(c(base_addr + ADDR_MATRIX_W + i * 2, width=64)) & mem_wvalid
            weights[i].set(mem_wdata.trunc(width=16), when=addr_match)

        # Activation matrix (16 × 16-bit values per row)
        for i in range(ARRAY_SIZE):
            addr_match = mem_waddr.eq(c(base_addr + ADDR_MATRIX_A + i * 2, width=64)) & mem_wvalid
            activations[i].set(mem_wdata.trunc(width=16), when=addr_match)

        return MmioWriteResult(start=start, reset_cube=reset_cube)


def build(m: Circuit, *, base_addr: int = 0x80000000) -> None:
    """
    Build cube matrix multiplication accelerator (16×16 systolic array).

    Memory-mapped interface:
        base_addr + 0x00: Control (write: bit0=start, bit1=reset)
        base_addr + 0x08: Status (read: bit0=done, bit1=busy)
        base_addr + 0x10: Matrix A (activations, 16×16 × 16-bit)
        base_addr + 0x210: Matrix W (weights, 16×16 × 16-bit)
        base_addr + 0x410: Matrix C (results, 16×16 × 32-bit)
    """
    # --- Ports ---
    clk = m.clock("clk")
    rst = m.reset("rst")

    mem_wvalid = m.input("mem_wvalid", width=1)
    mem_waddr = m.input("mem_waddr", width=64)
    mem_wdata = m.input("mem_wdata", width=64)
    mem_raddr = m.input("mem_raddr", width=64)

    consts = make_consts(m)

    # --- State registers ---
    with m.scope("state"):
        state = CubeState(
            state=m.out("state", clk=clk, rst=rst, width=3, init=ST_IDLE, en=consts.one1),
            cycle_count=m.out("cycle_count", clk=clk, rst=rst, width=8, init=0, en=consts.one1),
            done=m.out("done", clk=clk, rst=rst, width=1, init=0, en=consts.one1),
            busy=m.out("busy", clk=clk, rst=rst, width=1, init=0, en=consts.one1),
        )

    # --- PE array registers ---
    pe_array = _make_pe_regs(m, clk, rst, consts)

    # --- Result registers ---
    result_regs = _make_result_regs(m, clk, rst, consts)

    # --- Weight and activation buffers ---
    weights = _make_weight_regs(m, clk, rst, consts)
    activations = _make_activation_regs(m, clk, rst, consts)

    # --- MMIO write (control, weights, activations) ---
    mmio_wr = _build_mmio_write(
        m,
        mem_wvalid=mem_wvalid,
        mem_waddr=mem_waddr,
        mem_wdata=mem_wdata,
        base_addr=base_addr,
        weights=weights,
        activations=activations,
        consts=consts,
    )

    # --- FSM ---
    fsm = _build_fsm(
        m,
        start=mmio_wr.start,
        reset_cube=mmio_wr.reset_cube,
        state=state,
        consts=consts,
    )

    # --- Systolic array ---
    weight_wires = _get_reg_outputs(weights)
    activation_wires = _get_reg_outputs(activations)

    results = _build_array(
        m,
        load_weight=fsm.load_weight,
        compute=fsm.compute,
        weights=weight_wires,
        activations=activation_wires,
        pe_array=pe_array,
        consts=consts,
    )

    # --- Store results ---
    _store_results(result_regs, results, fsm.done)

    # --- MMIO read (status, results) ---
    rdata = _build_mmio_read(
        m,
        mem_raddr=mem_raddr,
        base_addr=base_addr,
        state=state,
        result_regs=result_regs,
        consts=consts,
    )

    # --- Outputs ---
    m.output("mem_rdata", rdata)
    m.output("done", state.done.out())
    m.output("busy", state.busy.out())


build.__pycircuit_name__ = "janus_cube_pyc"
