from __future__ import annotations

from pycircuit import Circuit

from janus.cube.cube_array import build_array
from janus.cube.cube_buffer import build_buffers
from janus.cube.cube_consts import ST_COMPUTE, ST_DONE, ST_DRAIN, ST_IDLE, ST_LOAD_WEIGHTS
from janus.cube.cube_types import CubeState, PERegs


def build(m: Circuit, *, base_addr: int = 0x80000000) -> None:
    """
    Build cube matrix multiplication accelerator (16x16 array, 16-bit inputs).

    Args:
        base_addr: Base address for memory-mapped interface
    """
    # --- Ports ---
    clk = m.clock("clk")
    rst = m.reset("rst")

    # Memory interface (simplified - connects to CPU memory bus)
    mem_wvalid = m.in_wire("mem_wvalid", width=1)
    mem_waddr = m.in_wire("mem_waddr", width=64)
    mem_wdata = m.in_wire("mem_wdata", width=64)
    mem_raddr = m.in_wire("mem_raddr", width=64)

    # Control registers
    with m.scope("ctrl"):
        ctrl_reg = m.out("control", clk=clk, rst=rst, width=8, init=0, en=mem_wvalid)
        ctrl_addr_match = mem_waddr == base_addr
        ctrl_reg.set(mem_wdata.trunc(width=8), when=ctrl_addr_match & mem_wvalid)

        start = ctrl_reg.out()[0]
        reset_cube = ctrl_reg.out()[1]

    # --- State registers ---
    with m.scope("state"):
        state = CubeState(
            state=m.out("state", clk=clk, rst=rst, width=3, init=ST_IDLE, en=m.const_wire(1, width=1)),
            cycle_count=m.out("cycle_count", clk=clk, rst=rst, width=8, init=0, en=m.const_wire(1, width=1)),
            done=m.out("done", clk=clk, rst=rst, width=1, init=0, en=m.const_wire(1, width=1)),
            busy=m.out("busy", clk=clk, rst=rst, width=1, init=0, en=m.const_wire(1, width=1)),
        )

    # --- PE array registers (16x16 = 256 PEs) ---
    pe_array = []

    # Row 0
    pe_row0 = []
    with m.scope("pe_r0_c0"):
        pe_row0.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r0_c1"):
        pe_row0.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r0_c2"):
        pe_row0.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r0_c3"):
        pe_row0.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r0_c4"):
        pe_row0.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r0_c5"):
        pe_row0.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r0_c6"):
        pe_row0.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r0_c7"):
        pe_row0.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r0_c8"):
        pe_row0.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r0_c9"):
        pe_row0.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r0_c10"):
        pe_row0.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r0_c11"):
        pe_row0.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r0_c12"):
        pe_row0.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r0_c13"):
        pe_row0.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r0_c14"):
        pe_row0.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r0_c15"):
        pe_row0.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    pe_array.append(pe_row0)

    # Row 1
    pe_row1 = []
    with m.scope("pe_r1_c0"):
        pe_row1.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r1_c1"):
        pe_row1.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r1_c2"):
        pe_row1.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r1_c3"):
        pe_row1.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r1_c4"):
        pe_row1.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r1_c5"):
        pe_row1.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r1_c6"):
        pe_row1.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r1_c7"):
        pe_row1.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r1_c8"):
        pe_row1.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r1_c9"):
        pe_row1.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r1_c10"):
        pe_row1.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r1_c11"):
        pe_row1.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r1_c12"):
        pe_row1.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r1_c13"):
        pe_row1.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r1_c14"):
        pe_row1.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r1_c15"):
        pe_row1.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    pe_array.append(pe_row1)

    # Row 2
    pe_row2 = []
    with m.scope("pe_r2_c0"):
        pe_row2.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r2_c1"):
        pe_row2.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r2_c2"):
        pe_row2.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r2_c3"):
        pe_row2.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r2_c4"):
        pe_row2.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r2_c5"):
        pe_row2.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r2_c6"):
        pe_row2.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r2_c7"):
        pe_row2.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r2_c8"):
        pe_row2.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r2_c9"):
        pe_row2.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r2_c10"):
        pe_row2.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r2_c11"):
        pe_row2.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r2_c12"):
        pe_row2.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r2_c13"):
        pe_row2.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r2_c14"):
        pe_row2.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r2_c15"):
        pe_row2.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    pe_array.append(pe_row2)

    # Row 3
    pe_row3 = []
    with m.scope("pe_r3_c0"):
        pe_row3.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r3_c1"):
        pe_row3.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r3_c2"):
        pe_row3.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r3_c3"):
        pe_row3.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r3_c4"):
        pe_row3.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r3_c5"):
        pe_row3.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r3_c6"):
        pe_row3.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r3_c7"):
        pe_row3.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r3_c8"):
        pe_row3.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r3_c9"):
        pe_row3.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r3_c10"):
        pe_row3.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r3_c11"):
        pe_row3.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r3_c12"):
        pe_row3.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r3_c13"):
        pe_row3.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r3_c14"):
        pe_row3.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r3_c15"):
        pe_row3.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    pe_array.append(pe_row3)

    # Row 4
    pe_row4 = []
    with m.scope("pe_r4_c0"):
        pe_row4.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r4_c1"):
        pe_row4.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r4_c2"):
        pe_row4.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r4_c3"):
        pe_row4.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r4_c4"):
        pe_row4.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r4_c5"):
        pe_row4.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r4_c6"):
        pe_row4.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r4_c7"):
        pe_row4.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r4_c8"):
        pe_row4.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r4_c9"):
        pe_row4.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r4_c10"):
        pe_row4.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r4_c11"):
        pe_row4.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r4_c12"):
        pe_row4.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r4_c13"):
        pe_row4.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r4_c14"):
        pe_row4.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r4_c15"):
        pe_row4.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    pe_array.append(pe_row4)

    # Row 5
    pe_row5 = []
    with m.scope("pe_r5_c0"):
        pe_row5.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r5_c1"):
        pe_row5.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r5_c2"):
        pe_row5.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r5_c3"):
        pe_row5.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r5_c4"):
        pe_row5.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r5_c5"):
        pe_row5.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r5_c6"):
        pe_row5.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r5_c7"):
        pe_row5.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r5_c8"):
        pe_row5.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r5_c9"):
        pe_row5.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r5_c10"):
        pe_row5.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r5_c11"):
        pe_row5.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r5_c12"):
        pe_row5.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r5_c13"):
        pe_row5.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r5_c14"):
        pe_row5.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r5_c15"):
        pe_row5.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    pe_array.append(pe_row5)

    # Row 6
    pe_row6 = []
    with m.scope("pe_r6_c0"):
        pe_row6.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r6_c1"):
        pe_row6.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r6_c2"):
        pe_row6.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r6_c3"):
        pe_row6.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r6_c4"):
        pe_row6.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r6_c5"):
        pe_row6.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r6_c6"):
        pe_row6.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r6_c7"):
        pe_row6.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r6_c8"):
        pe_row6.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r6_c9"):
        pe_row6.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r6_c10"):
        pe_row6.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r6_c11"):
        pe_row6.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r6_c12"):
        pe_row6.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r6_c13"):
        pe_row6.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r6_c14"):
        pe_row6.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r6_c15"):
        pe_row6.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    pe_array.append(pe_row6)

    # Row 7
    pe_row7 = []
    with m.scope("pe_r7_c0"):
        pe_row7.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r7_c1"):
        pe_row7.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r7_c2"):
        pe_row7.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r7_c3"):
        pe_row7.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r7_c4"):
        pe_row7.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r7_c5"):
        pe_row7.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r7_c6"):
        pe_row7.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r7_c7"):
        pe_row7.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r7_c8"):
        pe_row7.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r7_c9"):
        pe_row7.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r7_c10"):
        pe_row7.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r7_c11"):
        pe_row7.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r7_c12"):
        pe_row7.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r7_c13"):
        pe_row7.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r7_c14"):
        pe_row7.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r7_c15"):
        pe_row7.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    pe_array.append(pe_row7)

    # Row 8
    pe_row8 = []
    with m.scope("pe_r8_c0"):
        pe_row8.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r8_c1"):
        pe_row8.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r8_c2"):
        pe_row8.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r8_c3"):
        pe_row8.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r8_c4"):
        pe_row8.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r8_c5"):
        pe_row8.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r8_c6"):
        pe_row8.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r8_c7"):
        pe_row8.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r8_c8"):
        pe_row8.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r8_c9"):
        pe_row8.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r8_c10"):
        pe_row8.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r8_c11"):
        pe_row8.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r8_c12"):
        pe_row8.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r8_c13"):
        pe_row8.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r8_c14"):
        pe_row8.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r8_c15"):
        pe_row8.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    pe_array.append(pe_row8)

    # Row 9
    pe_row9 = []
    with m.scope("pe_r9_c0"):
        pe_row9.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r9_c1"):
        pe_row9.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r9_c2"):
        pe_row9.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r9_c3"):
        pe_row9.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r9_c4"):
        pe_row9.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r9_c5"):
        pe_row9.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r9_c6"):
        pe_row9.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r9_c7"):
        pe_row9.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r9_c8"):
        pe_row9.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r9_c9"):
        pe_row9.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r9_c10"):
        pe_row9.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r9_c11"):
        pe_row9.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r9_c12"):
        pe_row9.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r9_c13"):
        pe_row9.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r9_c14"):
        pe_row9.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r9_c15"):
        pe_row9.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    pe_array.append(pe_row9)

    # Row 10
    pe_row10 = []
    with m.scope("pe_r10_c0"):
        pe_row10.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r10_c1"):
        pe_row10.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r10_c2"):
        pe_row10.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r10_c3"):
        pe_row10.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r10_c4"):
        pe_row10.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r10_c5"):
        pe_row10.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r10_c6"):
        pe_row10.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r10_c7"):
        pe_row10.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r10_c8"):
        pe_row10.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r10_c9"):
        pe_row10.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r10_c10"):
        pe_row10.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r10_c11"):
        pe_row10.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r10_c12"):
        pe_row10.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r10_c13"):
        pe_row10.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r10_c14"):
        pe_row10.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r10_c15"):
        pe_row10.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    pe_array.append(pe_row10)

    # Row 11
    pe_row11 = []
    with m.scope("pe_r11_c0"):
        pe_row11.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r11_c1"):
        pe_row11.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r11_c2"):
        pe_row11.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r11_c3"):
        pe_row11.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r11_c4"):
        pe_row11.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r11_c5"):
        pe_row11.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r11_c6"):
        pe_row11.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r11_c7"):
        pe_row11.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r11_c8"):
        pe_row11.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r11_c9"):
        pe_row11.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r11_c10"):
        pe_row11.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r11_c11"):
        pe_row11.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r11_c12"):
        pe_row11.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r11_c13"):
        pe_row11.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r11_c14"):
        pe_row11.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r11_c15"):
        pe_row11.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    pe_array.append(pe_row11)

    # Row 12
    pe_row12 = []
    with m.scope("pe_r12_c0"):
        pe_row12.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r12_c1"):
        pe_row12.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r12_c2"):
        pe_row12.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r12_c3"):
        pe_row12.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r12_c4"):
        pe_row12.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r12_c5"):
        pe_row12.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r12_c6"):
        pe_row12.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r12_c7"):
        pe_row12.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r12_c8"):
        pe_row12.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r12_c9"):
        pe_row12.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r12_c10"):
        pe_row12.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r12_c11"):
        pe_row12.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r12_c12"):
        pe_row12.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r12_c13"):
        pe_row12.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r12_c14"):
        pe_row12.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r12_c15"):
        pe_row12.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    pe_array.append(pe_row12)

    # Row 13
    pe_row13 = []
    with m.scope("pe_r13_c0"):
        pe_row13.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r13_c1"):
        pe_row13.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r13_c2"):
        pe_row13.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r13_c3"):
        pe_row13.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r13_c4"):
        pe_row13.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r13_c5"):
        pe_row13.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r13_c6"):
        pe_row13.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r13_c7"):
        pe_row13.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r13_c8"):
        pe_row13.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r13_c9"):
        pe_row13.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r13_c10"):
        pe_row13.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r13_c11"):
        pe_row13.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r13_c12"):
        pe_row13.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r13_c13"):
        pe_row13.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r13_c14"):
        pe_row13.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r13_c15"):
        pe_row13.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    pe_array.append(pe_row13)

    # Row 14
    pe_row14 = []
    with m.scope("pe_r14_c0"):
        pe_row14.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r14_c1"):
        pe_row14.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r14_c2"):
        pe_row14.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r14_c3"):
        pe_row14.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r14_c4"):
        pe_row14.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r14_c5"):
        pe_row14.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r14_c6"):
        pe_row14.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r14_c7"):
        pe_row14.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r14_c8"):
        pe_row14.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r14_c9"):
        pe_row14.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r14_c10"):
        pe_row14.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r14_c11"):
        pe_row14.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r14_c12"):
        pe_row14.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r14_c13"):
        pe_row14.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r14_c14"):
        pe_row14.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r14_c15"):
        pe_row14.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    pe_array.append(pe_row14)

    # Row 15
    pe_row15 = []
    with m.scope("pe_r15_c0"):
        pe_row15.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r15_c1"):
        pe_row15.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r15_c2"):
        pe_row15.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r15_c3"):
        pe_row15.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r15_c4"):
        pe_row15.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r15_c5"):
        pe_row15.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r15_c6"):
        pe_row15.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r15_c7"):
        pe_row15.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r15_c8"):
        pe_row15.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r15_c9"):
        pe_row15.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r15_c10"):
        pe_row15.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r15_c11"):
        pe_row15.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r15_c12"):
        pe_row15.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r15_c13"):
        pe_row15.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r15_c14"):
        pe_row15.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    with m.scope("pe_r15_c15"):
        pe_row15.append(PERegs(
            weight=m.out("weight", clk=clk, rst=rst, width=16, init=0, en=m.const_wire(1, width=1)),
            acc=m.out("acc", clk=clk, rst=rst, width=32, init=0, en=m.const_wire(1, width=1)),
        ))
    pe_array.append(pe_row15)

    # --- Control FSM (inlined) ---
    with m.scope("CONTROL"):
        current_state = state.state.out()
        cycle_count = state.cycle_count.out()

        # State transitions
        state_is_idle = current_state == 0  # ST_IDLE
        state_is_load = current_state == ST_LOAD_WEIGHTS
        state_is_compute = current_state == ST_COMPUTE
        state_is_drain = current_state == ST_DRAIN
        state_is_done = current_state == ST_DONE

        # Next state logic
        next_state = current_state

        if state_is_idle:
            if start:
                next_state = m.const_wire(ST_LOAD_WEIGHTS, width=3)

        if state_is_load:
            if cycle_count == 0:
                next_state = m.const_wire(ST_COMPUTE, width=3)

        if state_is_compute:
            # Compute takes 16 cycles (streaming 16 rows)
            if cycle_count == 15:
                next_state = m.const_wire(ST_DRAIN, width=3)

        if state_is_drain:
            # Drain takes 15 cycles (pipeline depth)
            if cycle_count == 14:
                next_state = m.const_wire(ST_DONE, width=3)

        if state_is_done:
            if reset_cube:
                next_state = m.const_wire(0, width=3)  # ST_IDLE

        # Update state
        state.state.set(next_state)

        # Cycle counter
        counter_reset = (
            state_is_idle
            | (state_is_load & (cycle_count == 0))
            | (state_is_compute & (cycle_count == 15))
            | (state_is_drain & (cycle_count == 14))
        )

        next_count = counter_reset.select(m.const_wire(0, width=8), cycle_count + m.const_wire(1, width=8))
        state.cycle_count.set(next_count)

        # Control signals
        load_weight = state_is_load
        compute = state_is_compute | state_is_drain
        done = state_is_done

        # Status flags
        state.done.set(done)
        state.busy.set(~state_is_idle & ~state_is_done)

    # --- Buffers ---
    buffer_result = build_buffers(
        m,
        clk=clk,
        rst=rst,
        mem_wvalid=mem_wvalid,
        mem_waddr=mem_waddr,
        mem_wdata=mem_wdata,
        mem_raddr=mem_raddr,
        load_weight=load_weight,
        compute=compute,
        cycle_count=cycle_count,
    )
    weights = buffer_result[0]
    activations_buf = buffer_result[1]
    result_regs = buffer_result[2]

    # Select 16 activations based on cycle count (JIT-compiled mux logic)
    # Unrolled to avoid for-loop issues
    act0 = activations_buf[0]
    is_row1 = cycle_count == 1
    act0 = is_row1.select(activations_buf[16], act0)
    is_row2 = cycle_count == 2
    act0 = is_row2.select(activations_buf[32], act0)
    is_row3 = cycle_count == 3
    act0 = is_row3.select(activations_buf[48], act0)
    is_row4 = cycle_count == 4
    act0 = is_row4.select(activations_buf[64], act0)
    is_row5 = cycle_count == 5
    act0 = is_row5.select(activations_buf[80], act0)
    is_row6 = cycle_count == 6
    act0 = is_row6.select(activations_buf[96], act0)
    is_row7 = cycle_count == 7
    act0 = is_row7.select(activations_buf[112], act0)
    is_row8 = cycle_count == 8
    act0 = is_row8.select(activations_buf[128], act0)
    is_row9 = cycle_count == 9
    act0 = is_row9.select(activations_buf[144], act0)
    is_row10 = cycle_count == 10
    act0 = is_row10.select(activations_buf[160], act0)
    is_row11 = cycle_count == 11
    act0 = is_row11.select(activations_buf[176], act0)
    is_row12 = cycle_count == 12
    act0 = is_row12.select(activations_buf[192], act0)
    is_row13 = cycle_count == 13
    act0 = is_row13.select(activations_buf[208], act0)
    is_row14 = cycle_count == 14
    act0 = is_row14.select(activations_buf[224], act0)
    is_row15 = cycle_count == 15
    act0 = is_row15.select(activations_buf[240], act0)

    act1 = activations_buf[1]
    is_row1 = cycle_count == 1
    act1 = is_row1.select(activations_buf[17], act1)
    is_row2 = cycle_count == 2
    act1 = is_row2.select(activations_buf[33], act1)
    is_row3 = cycle_count == 3
    act1 = is_row3.select(activations_buf[49], act1)
    is_row4 = cycle_count == 4
    act1 = is_row4.select(activations_buf[65], act1)
    is_row5 = cycle_count == 5
    act1 = is_row5.select(activations_buf[81], act1)
    is_row6 = cycle_count == 6
    act1 = is_row6.select(activations_buf[97], act1)
    is_row7 = cycle_count == 7
    act1 = is_row7.select(activations_buf[113], act1)
    is_row8 = cycle_count == 8
    act1 = is_row8.select(activations_buf[129], act1)
    is_row9 = cycle_count == 9
    act1 = is_row9.select(activations_buf[145], act1)
    is_row10 = cycle_count == 10
    act1 = is_row10.select(activations_buf[161], act1)
    is_row11 = cycle_count == 11
    act1 = is_row11.select(activations_buf[177], act1)
    is_row12 = cycle_count == 12
    act1 = is_row12.select(activations_buf[193], act1)
    is_row13 = cycle_count == 13
    act1 = is_row13.select(activations_buf[209], act1)
    is_row14 = cycle_count == 14
    act1 = is_row14.select(activations_buf[225], act1)
    is_row15 = cycle_count == 15
    act1 = is_row15.select(activations_buf[241], act1)

    act2 = activations_buf[2]
    is_row1 = cycle_count == 1
    act2 = is_row1.select(activations_buf[18], act2)
    is_row2 = cycle_count == 2
    act2 = is_row2.select(activations_buf[34], act2)
    is_row3 = cycle_count == 3
    act2 = is_row3.select(activations_buf[50], act2)
    is_row4 = cycle_count == 4
    act2 = is_row4.select(activations_buf[66], act2)
    is_row5 = cycle_count == 5
    act2 = is_row5.select(activations_buf[82], act2)
    is_row6 = cycle_count == 6
    act2 = is_row6.select(activations_buf[98], act2)
    is_row7 = cycle_count == 7
    act2 = is_row7.select(activations_buf[114], act2)
    is_row8 = cycle_count == 8
    act2 = is_row8.select(activations_buf[130], act2)
    is_row9 = cycle_count == 9
    act2 = is_row9.select(activations_buf[146], act2)
    is_row10 = cycle_count == 10
    act2 = is_row10.select(activations_buf[162], act2)
    is_row11 = cycle_count == 11
    act2 = is_row11.select(activations_buf[178], act2)
    is_row12 = cycle_count == 12
    act2 = is_row12.select(activations_buf[194], act2)
    is_row13 = cycle_count == 13
    act2 = is_row13.select(activations_buf[210], act2)
    is_row14 = cycle_count == 14
    act2 = is_row14.select(activations_buf[226], act2)
    is_row15 = cycle_count == 15
    act2 = is_row15.select(activations_buf[242], act2)

    act3 = activations_buf[3]
    is_row1 = cycle_count == 1
    act3 = is_row1.select(activations_buf[19], act3)
    is_row2 = cycle_count == 2
    act3 = is_row2.select(activations_buf[35], act3)
    is_row3 = cycle_count == 3
    act3 = is_row3.select(activations_buf[51], act3)
    is_row4 = cycle_count == 4
    act3 = is_row4.select(activations_buf[67], act3)
    is_row5 = cycle_count == 5
    act3 = is_row5.select(activations_buf[83], act3)
    is_row6 = cycle_count == 6
    act3 = is_row6.select(activations_buf[99], act3)
    is_row7 = cycle_count == 7
    act3 = is_row7.select(activations_buf[115], act3)
    is_row8 = cycle_count == 8
    act3 = is_row8.select(activations_buf[131], act3)
    is_row9 = cycle_count == 9
    act3 = is_row9.select(activations_buf[147], act3)
    is_row10 = cycle_count == 10
    act3 = is_row10.select(activations_buf[163], act3)
    is_row11 = cycle_count == 11
    act3 = is_row11.select(activations_buf[179], act3)
    is_row12 = cycle_count == 12
    act3 = is_row12.select(activations_buf[195], act3)
    is_row13 = cycle_count == 13
    act3 = is_row13.select(activations_buf[211], act3)
    is_row14 = cycle_count == 14
    act3 = is_row14.select(activations_buf[227], act3)
    is_row15 = cycle_count == 15
    act3 = is_row15.select(activations_buf[243], act3)

    act4 = activations_buf[4]
    is_row1 = cycle_count == 1
    act4 = is_row1.select(activations_buf[20], act4)
    is_row2 = cycle_count == 2
    act4 = is_row2.select(activations_buf[36], act4)
    is_row3 = cycle_count == 3
    act4 = is_row3.select(activations_buf[52], act4)
    is_row4 = cycle_count == 4
    act4 = is_row4.select(activations_buf[68], act4)
    is_row5 = cycle_count == 5
    act4 = is_row5.select(activations_buf[84], act4)
    is_row6 = cycle_count == 6
    act4 = is_row6.select(activations_buf[100], act4)
    is_row7 = cycle_count == 7
    act4 = is_row7.select(activations_buf[116], act4)
    is_row8 = cycle_count == 8
    act4 = is_row8.select(activations_buf[132], act4)
    is_row9 = cycle_count == 9
    act4 = is_row9.select(activations_buf[148], act4)
    is_row10 = cycle_count == 10
    act4 = is_row10.select(activations_buf[164], act4)
    is_row11 = cycle_count == 11
    act4 = is_row11.select(activations_buf[180], act4)
    is_row12 = cycle_count == 12
    act4 = is_row12.select(activations_buf[196], act4)
    is_row13 = cycle_count == 13
    act4 = is_row13.select(activations_buf[212], act4)
    is_row14 = cycle_count == 14
    act4 = is_row14.select(activations_buf[228], act4)
    is_row15 = cycle_count == 15
    act4 = is_row15.select(activations_buf[244], act4)

    act5 = activations_buf[5]
    is_row1 = cycle_count == 1
    act5 = is_row1.select(activations_buf[21], act5)
    is_row2 = cycle_count == 2
    act5 = is_row2.select(activations_buf[37], act5)
    is_row3 = cycle_count == 3
    act5 = is_row3.select(activations_buf[53], act5)
    is_row4 = cycle_count == 4
    act5 = is_row4.select(activations_buf[69], act5)
    is_row5 = cycle_count == 5
    act5 = is_row5.select(activations_buf[85], act5)
    is_row6 = cycle_count == 6
    act5 = is_row6.select(activations_buf[101], act5)
    is_row7 = cycle_count == 7
    act5 = is_row7.select(activations_buf[117], act5)
    is_row8 = cycle_count == 8
    act5 = is_row8.select(activations_buf[133], act5)
    is_row9 = cycle_count == 9
    act5 = is_row9.select(activations_buf[149], act5)
    is_row10 = cycle_count == 10
    act5 = is_row10.select(activations_buf[165], act5)
    is_row11 = cycle_count == 11
    act5 = is_row11.select(activations_buf[181], act5)
    is_row12 = cycle_count == 12
    act5 = is_row12.select(activations_buf[197], act5)
    is_row13 = cycle_count == 13
    act5 = is_row13.select(activations_buf[213], act5)
    is_row14 = cycle_count == 14
    act5 = is_row14.select(activations_buf[229], act5)
    is_row15 = cycle_count == 15
    act5 = is_row15.select(activations_buf[245], act5)

    act6 = activations_buf[6]
    is_row1 = cycle_count == 1
    act6 = is_row1.select(activations_buf[22], act6)
    is_row2 = cycle_count == 2
    act6 = is_row2.select(activations_buf[38], act6)
    is_row3 = cycle_count == 3
    act6 = is_row3.select(activations_buf[54], act6)
    is_row4 = cycle_count == 4
    act6 = is_row4.select(activations_buf[70], act6)
    is_row5 = cycle_count == 5
    act6 = is_row5.select(activations_buf[86], act6)
    is_row6 = cycle_count == 6
    act6 = is_row6.select(activations_buf[102], act6)
    is_row7 = cycle_count == 7
    act6 = is_row7.select(activations_buf[118], act6)
    is_row8 = cycle_count == 8
    act6 = is_row8.select(activations_buf[134], act6)
    is_row9 = cycle_count == 9
    act6 = is_row9.select(activations_buf[150], act6)
    is_row10 = cycle_count == 10
    act6 = is_row10.select(activations_buf[166], act6)
    is_row11 = cycle_count == 11
    act6 = is_row11.select(activations_buf[182], act6)
    is_row12 = cycle_count == 12
    act6 = is_row12.select(activations_buf[198], act6)
    is_row13 = cycle_count == 13
    act6 = is_row13.select(activations_buf[214], act6)
    is_row14 = cycle_count == 14
    act6 = is_row14.select(activations_buf[230], act6)
    is_row15 = cycle_count == 15
    act6 = is_row15.select(activations_buf[246], act6)

    act7 = activations_buf[7]
    is_row1 = cycle_count == 1
    act7 = is_row1.select(activations_buf[23], act7)
    is_row2 = cycle_count == 2
    act7 = is_row2.select(activations_buf[39], act7)
    is_row3 = cycle_count == 3
    act7 = is_row3.select(activations_buf[55], act7)
    is_row4 = cycle_count == 4
    act7 = is_row4.select(activations_buf[71], act7)
    is_row5 = cycle_count == 5
    act7 = is_row5.select(activations_buf[87], act7)
    is_row6 = cycle_count == 6
    act7 = is_row6.select(activations_buf[103], act7)
    is_row7 = cycle_count == 7
    act7 = is_row7.select(activations_buf[119], act7)
    is_row8 = cycle_count == 8
    act7 = is_row8.select(activations_buf[135], act7)
    is_row9 = cycle_count == 9
    act7 = is_row9.select(activations_buf[151], act7)
    is_row10 = cycle_count == 10
    act7 = is_row10.select(activations_buf[167], act7)
    is_row11 = cycle_count == 11
    act7 = is_row11.select(activations_buf[183], act7)
    is_row12 = cycle_count == 12
    act7 = is_row12.select(activations_buf[199], act7)
    is_row13 = cycle_count == 13
    act7 = is_row13.select(activations_buf[215], act7)
    is_row14 = cycle_count == 14
    act7 = is_row14.select(activations_buf[231], act7)
    is_row15 = cycle_count == 15
    act7 = is_row15.select(activations_buf[247], act7)

    act8 = activations_buf[8]
    is_row1 = cycle_count == 1
    act8 = is_row1.select(activations_buf[24], act8)
    is_row2 = cycle_count == 2
    act8 = is_row2.select(activations_buf[40], act8)
    is_row3 = cycle_count == 3
    act8 = is_row3.select(activations_buf[56], act8)
    is_row4 = cycle_count == 4
    act8 = is_row4.select(activations_buf[72], act8)
    is_row5 = cycle_count == 5
    act8 = is_row5.select(activations_buf[88], act8)
    is_row6 = cycle_count == 6
    act8 = is_row6.select(activations_buf[104], act8)
    is_row7 = cycle_count == 7
    act8 = is_row7.select(activations_buf[120], act8)
    is_row8 = cycle_count == 8
    act8 = is_row8.select(activations_buf[136], act8)
    is_row9 = cycle_count == 9
    act8 = is_row9.select(activations_buf[152], act8)
    is_row10 = cycle_count == 10
    act8 = is_row10.select(activations_buf[168], act8)
    is_row11 = cycle_count == 11
    act8 = is_row11.select(activations_buf[184], act8)
    is_row12 = cycle_count == 12
    act8 = is_row12.select(activations_buf[200], act8)
    is_row13 = cycle_count == 13
    act8 = is_row13.select(activations_buf[216], act8)
    is_row14 = cycle_count == 14
    act8 = is_row14.select(activations_buf[232], act8)
    is_row15 = cycle_count == 15
    act8 = is_row15.select(activations_buf[248], act8)

    act9 = activations_buf[9]
    is_row1 = cycle_count == 1
    act9 = is_row1.select(activations_buf[25], act9)
    is_row2 = cycle_count == 2
    act9 = is_row2.select(activations_buf[41], act9)
    is_row3 = cycle_count == 3
    act9 = is_row3.select(activations_buf[57], act9)
    is_row4 = cycle_count == 4
    act9 = is_row4.select(activations_buf[73], act9)
    is_row5 = cycle_count == 5
    act9 = is_row5.select(activations_buf[89], act9)
    is_row6 = cycle_count == 6
    act9 = is_row6.select(activations_buf[105], act9)
    is_row7 = cycle_count == 7
    act9 = is_row7.select(activations_buf[121], act9)
    is_row8 = cycle_count == 8
    act9 = is_row8.select(activations_buf[137], act9)
    is_row9 = cycle_count == 9
    act9 = is_row9.select(activations_buf[153], act9)
    is_row10 = cycle_count == 10
    act9 = is_row10.select(activations_buf[169], act9)
    is_row11 = cycle_count == 11
    act9 = is_row11.select(activations_buf[185], act9)
    is_row12 = cycle_count == 12
    act9 = is_row12.select(activations_buf[201], act9)
    is_row13 = cycle_count == 13
    act9 = is_row13.select(activations_buf[217], act9)
    is_row14 = cycle_count == 14
    act9 = is_row14.select(activations_buf[233], act9)
    is_row15 = cycle_count == 15
    act9 = is_row15.select(activations_buf[249], act9)

    act10 = activations_buf[10]
    is_row1 = cycle_count == 1
    act10 = is_row1.select(activations_buf[26], act10)
    is_row2 = cycle_count == 2
    act10 = is_row2.select(activations_buf[42], act10)
    is_row3 = cycle_count == 3
    act10 = is_row3.select(activations_buf[58], act10)
    is_row4 = cycle_count == 4
    act10 = is_row4.select(activations_buf[74], act10)
    is_row5 = cycle_count == 5
    act10 = is_row5.select(activations_buf[90], act10)
    is_row6 = cycle_count == 6
    act10 = is_row6.select(activations_buf[106], act10)
    is_row7 = cycle_count == 7
    act10 = is_row7.select(activations_buf[122], act10)
    is_row8 = cycle_count == 8
    act10 = is_row8.select(activations_buf[138], act10)
    is_row9 = cycle_count == 9
    act10 = is_row9.select(activations_buf[154], act10)
    is_row10 = cycle_count == 10
    act10 = is_row10.select(activations_buf[170], act10)
    is_row11 = cycle_count == 11
    act10 = is_row11.select(activations_buf[186], act10)
    is_row12 = cycle_count == 12
    act10 = is_row12.select(activations_buf[202], act10)
    is_row13 = cycle_count == 13
    act10 = is_row13.select(activations_buf[218], act10)
    is_row14 = cycle_count == 14
    act10 = is_row14.select(activations_buf[234], act10)
    is_row15 = cycle_count == 15
    act10 = is_row15.select(activations_buf[250], act10)

    act11 = activations_buf[11]
    is_row1 = cycle_count == 1
    act11 = is_row1.select(activations_buf[27], act11)
    is_row2 = cycle_count == 2
    act11 = is_row2.select(activations_buf[43], act11)
    is_row3 = cycle_count == 3
    act11 = is_row3.select(activations_buf[59], act11)
    is_row4 = cycle_count == 4
    act11 = is_row4.select(activations_buf[75], act11)
    is_row5 = cycle_count == 5
    act11 = is_row5.select(activations_buf[91], act11)
    is_row6 = cycle_count == 6
    act11 = is_row6.select(activations_buf[107], act11)
    is_row7 = cycle_count == 7
    act11 = is_row7.select(activations_buf[123], act11)
    is_row8 = cycle_count == 8
    act11 = is_row8.select(activations_buf[139], act11)
    is_row9 = cycle_count == 9
    act11 = is_row9.select(activations_buf[155], act11)
    is_row10 = cycle_count == 10
    act11 = is_row10.select(activations_buf[171], act11)
    is_row11 = cycle_count == 11
    act11 = is_row11.select(activations_buf[187], act11)
    is_row12 = cycle_count == 12
    act11 = is_row12.select(activations_buf[203], act11)
    is_row13 = cycle_count == 13
    act11 = is_row13.select(activations_buf[219], act11)
    is_row14 = cycle_count == 14
    act11 = is_row14.select(activations_buf[235], act11)
    is_row15 = cycle_count == 15
    act11 = is_row15.select(activations_buf[251], act11)

    act12 = activations_buf[12]
    is_row1 = cycle_count == 1
    act12 = is_row1.select(activations_buf[28], act12)
    is_row2 = cycle_count == 2
    act12 = is_row2.select(activations_buf[44], act12)
    is_row3 = cycle_count == 3
    act12 = is_row3.select(activations_buf[60], act12)
    is_row4 = cycle_count == 4
    act12 = is_row4.select(activations_buf[76], act12)
    is_row5 = cycle_count == 5
    act12 = is_row5.select(activations_buf[92], act12)
    is_row6 = cycle_count == 6
    act12 = is_row6.select(activations_buf[108], act12)
    is_row7 = cycle_count == 7
    act12 = is_row7.select(activations_buf[124], act12)
    is_row8 = cycle_count == 8
    act12 = is_row8.select(activations_buf[140], act12)
    is_row9 = cycle_count == 9
    act12 = is_row9.select(activations_buf[156], act12)
    is_row10 = cycle_count == 10
    act12 = is_row10.select(activations_buf[172], act12)
    is_row11 = cycle_count == 11
    act12 = is_row11.select(activations_buf[188], act12)
    is_row12 = cycle_count == 12
    act12 = is_row12.select(activations_buf[204], act12)
    is_row13 = cycle_count == 13
    act12 = is_row13.select(activations_buf[220], act12)
    is_row14 = cycle_count == 14
    act12 = is_row14.select(activations_buf[236], act12)
    is_row15 = cycle_count == 15
    act12 = is_row15.select(activations_buf[252], act12)

    act13 = activations_buf[13]
    is_row1 = cycle_count == 1
    act13 = is_row1.select(activations_buf[29], act13)
    is_row2 = cycle_count == 2
    act13 = is_row2.select(activations_buf[45], act13)
    is_row3 = cycle_count == 3
    act13 = is_row3.select(activations_buf[61], act13)
    is_row4 = cycle_count == 4
    act13 = is_row4.select(activations_buf[77], act13)
    is_row5 = cycle_count == 5
    act13 = is_row5.select(activations_buf[93], act13)
    is_row6 = cycle_count == 6
    act13 = is_row6.select(activations_buf[109], act13)
    is_row7 = cycle_count == 7
    act13 = is_row7.select(activations_buf[125], act13)
    is_row8 = cycle_count == 8
    act13 = is_row8.select(activations_buf[141], act13)
    is_row9 = cycle_count == 9
    act13 = is_row9.select(activations_buf[157], act13)
    is_row10 = cycle_count == 10
    act13 = is_row10.select(activations_buf[173], act13)
    is_row11 = cycle_count == 11
    act13 = is_row11.select(activations_buf[189], act13)
    is_row12 = cycle_count == 12
    act13 = is_row12.select(activations_buf[205], act13)
    is_row13 = cycle_count == 13
    act13 = is_row13.select(activations_buf[221], act13)
    is_row14 = cycle_count == 14
    act13 = is_row14.select(activations_buf[237], act13)
    is_row15 = cycle_count == 15
    act13 = is_row15.select(activations_buf[253], act13)

    act14 = activations_buf[14]
    is_row1 = cycle_count == 1
    act14 = is_row1.select(activations_buf[30], act14)
    is_row2 = cycle_count == 2
    act14 = is_row2.select(activations_buf[46], act14)
    is_row3 = cycle_count == 3
    act14 = is_row3.select(activations_buf[62], act14)
    is_row4 = cycle_count == 4
    act14 = is_row4.select(activations_buf[78], act14)
    is_row5 = cycle_count == 5
    act14 = is_row5.select(activations_buf[94], act14)
    is_row6 = cycle_count == 6
    act14 = is_row6.select(activations_buf[110], act14)
    is_row7 = cycle_count == 7
    act14 = is_row7.select(activations_buf[126], act14)
    is_row8 = cycle_count == 8
    act14 = is_row8.select(activations_buf[142], act14)
    is_row9 = cycle_count == 9
    act14 = is_row9.select(activations_buf[158], act14)
    is_row10 = cycle_count == 10
    act14 = is_row10.select(activations_buf[174], act14)
    is_row11 = cycle_count == 11
    act14 = is_row11.select(activations_buf[190], act14)
    is_row12 = cycle_count == 12
    act14 = is_row12.select(activations_buf[206], act14)
    is_row13 = cycle_count == 13
    act14 = is_row13.select(activations_buf[222], act14)
    is_row14 = cycle_count == 14
    act14 = is_row14.select(activations_buf[238], act14)
    is_row15 = cycle_count == 15
    act14 = is_row15.select(activations_buf[254], act14)

    act15 = activations_buf[15]
    is_row1 = cycle_count == 1
    act15 = is_row1.select(activations_buf[31], act15)
    is_row2 = cycle_count == 2
    act15 = is_row2.select(activations_buf[47], act15)
    is_row3 = cycle_count == 3
    act15 = is_row3.select(activations_buf[63], act15)
    is_row4 = cycle_count == 4
    act15 = is_row4.select(activations_buf[79], act15)
    is_row5 = cycle_count == 5
    act15 = is_row5.select(activations_buf[95], act15)
    is_row6 = cycle_count == 6
    act15 = is_row6.select(activations_buf[111], act15)
    is_row7 = cycle_count == 7
    act15 = is_row7.select(activations_buf[127], act15)
    is_row8 = cycle_count == 8
    act15 = is_row8.select(activations_buf[143], act15)
    is_row9 = cycle_count == 9
    act15 = is_row9.select(activations_buf[159], act15)
    is_row10 = cycle_count == 10
    act15 = is_row10.select(activations_buf[175], act15)
    is_row11 = cycle_count == 11
    act15 = is_row11.select(activations_buf[191], act15)
    is_row12 = cycle_count == 12
    act15 = is_row12.select(activations_buf[207], act15)
    is_row13 = cycle_count == 13
    act15 = is_row13.select(activations_buf[223], act15)
    is_row14 = cycle_count == 14
    act15 = is_row14.select(activations_buf[239], act15)
    is_row15 = cycle_count == 15
    act15 = is_row15.select(activations_buf[255], act15)

    activations = [act0, act1, act2, act3, act4, act5, act6, act7, act8, act9, act10, act11, act12, act13, act14, act15]

    # --- Systolic array ---
    results = build_array(
        m,
        load_weight=load_weight,
        compute=compute,
        weights=weights,
        activations=activations,
        pe_array=pe_array,
    )

    # Write results to buffer (256 results)
    result_regs[0].set(results[0], when=compute)
    result_regs[1].set(results[1], when=compute)
    result_regs[2].set(results[2], when=compute)
    result_regs[3].set(results[3], when=compute)
    result_regs[4].set(results[4], when=compute)
    result_regs[5].set(results[5], when=compute)
    result_regs[6].set(results[6], when=compute)
    result_regs[7].set(results[7], when=compute)
    result_regs[8].set(results[8], when=compute)
    result_regs[9].set(results[9], when=compute)
    result_regs[10].set(results[10], when=compute)
    result_regs[11].set(results[11], when=compute)
    result_regs[12].set(results[12], when=compute)
    result_regs[13].set(results[13], when=compute)
    result_regs[14].set(results[14], when=compute)
    result_regs[15].set(results[15], when=compute)
    result_regs[16].set(results[16], when=compute)
    result_regs[17].set(results[17], when=compute)
    result_regs[18].set(results[18], when=compute)
    result_regs[19].set(results[19], when=compute)
    result_regs[20].set(results[20], when=compute)
    result_regs[21].set(results[21], when=compute)
    result_regs[22].set(results[22], when=compute)
    result_regs[23].set(results[23], when=compute)
    result_regs[24].set(results[24], when=compute)
    result_regs[25].set(results[25], when=compute)
    result_regs[26].set(results[26], when=compute)
    result_regs[27].set(results[27], when=compute)
    result_regs[28].set(results[28], when=compute)
    result_regs[29].set(results[29], when=compute)
    result_regs[30].set(results[30], when=compute)
    result_regs[31].set(results[31], when=compute)
    result_regs[32].set(results[32], when=compute)
    result_regs[33].set(results[33], when=compute)
    result_regs[34].set(results[34], when=compute)
    result_regs[35].set(results[35], when=compute)
    result_regs[36].set(results[36], when=compute)
    result_regs[37].set(results[37], when=compute)
    result_regs[38].set(results[38], when=compute)
    result_regs[39].set(results[39], when=compute)
    result_regs[40].set(results[40], when=compute)
    result_regs[41].set(results[41], when=compute)
    result_regs[42].set(results[42], when=compute)
    result_regs[43].set(results[43], when=compute)
    result_regs[44].set(results[44], when=compute)
    result_regs[45].set(results[45], when=compute)
    result_regs[46].set(results[46], when=compute)
    result_regs[47].set(results[47], when=compute)
    result_regs[48].set(results[48], when=compute)
    result_regs[49].set(results[49], when=compute)
    result_regs[50].set(results[50], when=compute)
    result_regs[51].set(results[51], when=compute)
    result_regs[52].set(results[52], when=compute)
    result_regs[53].set(results[53], when=compute)
    result_regs[54].set(results[54], when=compute)
    result_regs[55].set(results[55], when=compute)
    result_regs[56].set(results[56], when=compute)
    result_regs[57].set(results[57], when=compute)
    result_regs[58].set(results[58], when=compute)
    result_regs[59].set(results[59], when=compute)
    result_regs[60].set(results[60], when=compute)
    result_regs[61].set(results[61], when=compute)
    result_regs[62].set(results[62], when=compute)
    result_regs[63].set(results[63], when=compute)
    result_regs[64].set(results[64], when=compute)
    result_regs[65].set(results[65], when=compute)
    result_regs[66].set(results[66], when=compute)
    result_regs[67].set(results[67], when=compute)
    result_regs[68].set(results[68], when=compute)
    result_regs[69].set(results[69], when=compute)
    result_regs[70].set(results[70], when=compute)
    result_regs[71].set(results[71], when=compute)
    result_regs[72].set(results[72], when=compute)
    result_regs[73].set(results[73], when=compute)
    result_regs[74].set(results[74], when=compute)
    result_regs[75].set(results[75], when=compute)
    result_regs[76].set(results[76], when=compute)
    result_regs[77].set(results[77], when=compute)
    result_regs[78].set(results[78], when=compute)
    result_regs[79].set(results[79], when=compute)
    result_regs[80].set(results[80], when=compute)
    result_regs[81].set(results[81], when=compute)
    result_regs[82].set(results[82], when=compute)
    result_regs[83].set(results[83], when=compute)
    result_regs[84].set(results[84], when=compute)
    result_regs[85].set(results[85], when=compute)
    result_regs[86].set(results[86], when=compute)
    result_regs[87].set(results[87], when=compute)
    result_regs[88].set(results[88], when=compute)
    result_regs[89].set(results[89], when=compute)
    result_regs[90].set(results[90], when=compute)
    result_regs[91].set(results[91], when=compute)
    result_regs[92].set(results[92], when=compute)
    result_regs[93].set(results[93], when=compute)
    result_regs[94].set(results[94], when=compute)
    result_regs[95].set(results[95], when=compute)
    result_regs[96].set(results[96], when=compute)
    result_regs[97].set(results[97], when=compute)
    result_regs[98].set(results[98], when=compute)
    result_regs[99].set(results[99], when=compute)
    result_regs[100].set(results[100], when=compute)
    result_regs[101].set(results[101], when=compute)
    result_regs[102].set(results[102], when=compute)
    result_regs[103].set(results[103], when=compute)
    result_regs[104].set(results[104], when=compute)
    result_regs[105].set(results[105], when=compute)
    result_regs[106].set(results[106], when=compute)
    result_regs[107].set(results[107], when=compute)
    result_regs[108].set(results[108], when=compute)
    result_regs[109].set(results[109], when=compute)
    result_regs[110].set(results[110], when=compute)
    result_regs[111].set(results[111], when=compute)
    result_regs[112].set(results[112], when=compute)
    result_regs[113].set(results[113], when=compute)
    result_regs[114].set(results[114], when=compute)
    result_regs[115].set(results[115], when=compute)
    result_regs[116].set(results[116], when=compute)
    result_regs[117].set(results[117], when=compute)
    result_regs[118].set(results[118], when=compute)
    result_regs[119].set(results[119], when=compute)
    result_regs[120].set(results[120], when=compute)
    result_regs[121].set(results[121], when=compute)
    result_regs[122].set(results[122], when=compute)
    result_regs[123].set(results[123], when=compute)
    result_regs[124].set(results[124], when=compute)
    result_regs[125].set(results[125], when=compute)
    result_regs[126].set(results[126], when=compute)
    result_regs[127].set(results[127], when=compute)
    result_regs[128].set(results[128], when=compute)
    result_regs[129].set(results[129], when=compute)
    result_regs[130].set(results[130], when=compute)
    result_regs[131].set(results[131], when=compute)
    result_regs[132].set(results[132], when=compute)
    result_regs[133].set(results[133], when=compute)
    result_regs[134].set(results[134], when=compute)
    result_regs[135].set(results[135], when=compute)
    result_regs[136].set(results[136], when=compute)
    result_regs[137].set(results[137], when=compute)
    result_regs[138].set(results[138], when=compute)
    result_regs[139].set(results[139], when=compute)
    result_regs[140].set(results[140], when=compute)
    result_regs[141].set(results[141], when=compute)
    result_regs[142].set(results[142], when=compute)
    result_regs[143].set(results[143], when=compute)
    result_regs[144].set(results[144], when=compute)
    result_regs[145].set(results[145], when=compute)
    result_regs[146].set(results[146], when=compute)
    result_regs[147].set(results[147], when=compute)
    result_regs[148].set(results[148], when=compute)
    result_regs[149].set(results[149], when=compute)
    result_regs[150].set(results[150], when=compute)
    result_regs[151].set(results[151], when=compute)
    result_regs[152].set(results[152], when=compute)
    result_regs[153].set(results[153], when=compute)
    result_regs[154].set(results[154], when=compute)
    result_regs[155].set(results[155], when=compute)
    result_regs[156].set(results[156], when=compute)
    result_regs[157].set(results[157], when=compute)
    result_regs[158].set(results[158], when=compute)
    result_regs[159].set(results[159], when=compute)
    result_regs[160].set(results[160], when=compute)
    result_regs[161].set(results[161], when=compute)
    result_regs[162].set(results[162], when=compute)
    result_regs[163].set(results[163], when=compute)
    result_regs[164].set(results[164], when=compute)
    result_regs[165].set(results[165], when=compute)
    result_regs[166].set(results[166], when=compute)
    result_regs[167].set(results[167], when=compute)
    result_regs[168].set(results[168], when=compute)
    result_regs[169].set(results[169], when=compute)
    result_regs[170].set(results[170], when=compute)
    result_regs[171].set(results[171], when=compute)
    result_regs[172].set(results[172], when=compute)
    result_regs[173].set(results[173], when=compute)
    result_regs[174].set(results[174], when=compute)
    result_regs[175].set(results[175], when=compute)
    result_regs[176].set(results[176], when=compute)
    result_regs[177].set(results[177], when=compute)
    result_regs[178].set(results[178], when=compute)
    result_regs[179].set(results[179], when=compute)
    result_regs[180].set(results[180], when=compute)
    result_regs[181].set(results[181], when=compute)
    result_regs[182].set(results[182], when=compute)
    result_regs[183].set(results[183], when=compute)
    result_regs[184].set(results[184], when=compute)
    result_regs[185].set(results[185], when=compute)
    result_regs[186].set(results[186], when=compute)
    result_regs[187].set(results[187], when=compute)
    result_regs[188].set(results[188], when=compute)
    result_regs[189].set(results[189], when=compute)
    result_regs[190].set(results[190], when=compute)
    result_regs[191].set(results[191], when=compute)
    result_regs[192].set(results[192], when=compute)
    result_regs[193].set(results[193], when=compute)
    result_regs[194].set(results[194], when=compute)
    result_regs[195].set(results[195], when=compute)
    result_regs[196].set(results[196], when=compute)
    result_regs[197].set(results[197], when=compute)
    result_regs[198].set(results[198], when=compute)
    result_regs[199].set(results[199], when=compute)
    result_regs[200].set(results[200], when=compute)
    result_regs[201].set(results[201], when=compute)
    result_regs[202].set(results[202], when=compute)
    result_regs[203].set(results[203], when=compute)
    result_regs[204].set(results[204], when=compute)
    result_regs[205].set(results[205], when=compute)
    result_regs[206].set(results[206], when=compute)
    result_regs[207].set(results[207], when=compute)
    result_regs[208].set(results[208], when=compute)
    result_regs[209].set(results[209], when=compute)
    result_regs[210].set(results[210], when=compute)
    result_regs[211].set(results[211], when=compute)
    result_regs[212].set(results[212], when=compute)
    result_regs[213].set(results[213], when=compute)
    result_regs[214].set(results[214], when=compute)
    result_regs[215].set(results[215], when=compute)
    result_regs[216].set(results[216], when=compute)
    result_regs[217].set(results[217], when=compute)
    result_regs[218].set(results[218], when=compute)
    result_regs[219].set(results[219], when=compute)
    result_regs[220].set(results[220], when=compute)
    result_regs[221].set(results[221], when=compute)
    result_regs[222].set(results[222], when=compute)
    result_regs[223].set(results[223], when=compute)
    result_regs[224].set(results[224], when=compute)
    result_regs[225].set(results[225], when=compute)
    result_regs[226].set(results[226], when=compute)
    result_regs[227].set(results[227], when=compute)
    result_regs[228].set(results[228], when=compute)
    result_regs[229].set(results[229], when=compute)
    result_regs[230].set(results[230], when=compute)
    result_regs[231].set(results[231], when=compute)
    result_regs[232].set(results[232], when=compute)
    result_regs[233].set(results[233], when=compute)
    result_regs[234].set(results[234], when=compute)
    result_regs[235].set(results[235], when=compute)
    result_regs[236].set(results[236], when=compute)
    result_regs[237].set(results[237], when=compute)
    result_regs[238].set(results[238], when=compute)
    result_regs[239].set(results[239], when=compute)
    result_regs[240].set(results[240], when=compute)
    result_regs[241].set(results[241], when=compute)
    result_regs[242].set(results[242], when=compute)
    result_regs[243].set(results[243], when=compute)
    result_regs[244].set(results[244], when=compute)
    result_regs[245].set(results[245], when=compute)
    result_regs[246].set(results[246], when=compute)
    result_regs[247].set(results[247], when=compute)
    result_regs[248].set(results[248], when=compute)
    result_regs[249].set(results[249], when=compute)
    result_regs[250].set(results[250], when=compute)
    result_regs[251].set(results[251], when=compute)
    result_regs[252].set(results[252], when=compute)
    result_regs[253].set(results[253], when=compute)
    result_regs[254].set(results[254], when=compute)
    result_regs[255].set(results[255], when=compute)

    # --- Outputs ---
    m.output("done", state.done.out())
    m.output("busy", state.busy.out())
    m.output("state", state.state.out())

    # Memory read interface (simplified)
    # In real integration, this would connect to CPU memory bus
    rdata = m.const_wire(0, width=64)
    # Unrolled with pre-computed addresses (256 results, 4 bytes each)
    addr_match = mem_raddr == (base_addr + 0x410 + 0)
    rdata = addr_match.select(result_regs[0].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 4)
    rdata = addr_match.select(result_regs[1].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 8)
    rdata = addr_match.select(result_regs[2].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 12)
    rdata = addr_match.select(result_regs[3].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 16)
    rdata = addr_match.select(result_regs[4].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 20)
    rdata = addr_match.select(result_regs[5].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 24)
    rdata = addr_match.select(result_regs[6].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 28)
    rdata = addr_match.select(result_regs[7].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 32)
    rdata = addr_match.select(result_regs[8].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 36)
    rdata = addr_match.select(result_regs[9].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 40)
    rdata = addr_match.select(result_regs[10].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 44)
    rdata = addr_match.select(result_regs[11].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 48)
    rdata = addr_match.select(result_regs[12].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 52)
    rdata = addr_match.select(result_regs[13].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 56)
    rdata = addr_match.select(result_regs[14].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 60)
    rdata = addr_match.select(result_regs[15].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 64)
    rdata = addr_match.select(result_regs[16].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 68)
    rdata = addr_match.select(result_regs[17].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 72)
    rdata = addr_match.select(result_regs[18].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 76)
    rdata = addr_match.select(result_regs[19].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 80)
    rdata = addr_match.select(result_regs[20].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 84)
    rdata = addr_match.select(result_regs[21].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 88)
    rdata = addr_match.select(result_regs[22].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 92)
    rdata = addr_match.select(result_regs[23].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 96)
    rdata = addr_match.select(result_regs[24].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 100)
    rdata = addr_match.select(result_regs[25].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 104)
    rdata = addr_match.select(result_regs[26].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 108)
    rdata = addr_match.select(result_regs[27].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 112)
    rdata = addr_match.select(result_regs[28].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 116)
    rdata = addr_match.select(result_regs[29].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 120)
    rdata = addr_match.select(result_regs[30].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 124)
    rdata = addr_match.select(result_regs[31].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 128)
    rdata = addr_match.select(result_regs[32].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 132)
    rdata = addr_match.select(result_regs[33].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 136)
    rdata = addr_match.select(result_regs[34].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 140)
    rdata = addr_match.select(result_regs[35].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 144)
    rdata = addr_match.select(result_regs[36].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 148)
    rdata = addr_match.select(result_regs[37].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 152)
    rdata = addr_match.select(result_regs[38].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 156)
    rdata = addr_match.select(result_regs[39].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 160)
    rdata = addr_match.select(result_regs[40].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 164)
    rdata = addr_match.select(result_regs[41].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 168)
    rdata = addr_match.select(result_regs[42].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 172)
    rdata = addr_match.select(result_regs[43].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 176)
    rdata = addr_match.select(result_regs[44].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 180)
    rdata = addr_match.select(result_regs[45].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 184)
    rdata = addr_match.select(result_regs[46].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 188)
    rdata = addr_match.select(result_regs[47].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 192)
    rdata = addr_match.select(result_regs[48].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 196)
    rdata = addr_match.select(result_regs[49].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 200)
    rdata = addr_match.select(result_regs[50].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 204)
    rdata = addr_match.select(result_regs[51].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 208)
    rdata = addr_match.select(result_regs[52].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 212)
    rdata = addr_match.select(result_regs[53].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 216)
    rdata = addr_match.select(result_regs[54].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 220)
    rdata = addr_match.select(result_regs[55].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 224)
    rdata = addr_match.select(result_regs[56].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 228)
    rdata = addr_match.select(result_regs[57].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 232)
    rdata = addr_match.select(result_regs[58].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 236)
    rdata = addr_match.select(result_regs[59].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 240)
    rdata = addr_match.select(result_regs[60].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 244)
    rdata = addr_match.select(result_regs[61].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 248)
    rdata = addr_match.select(result_regs[62].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 252)
    rdata = addr_match.select(result_regs[63].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 256)
    rdata = addr_match.select(result_regs[64].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 260)
    rdata = addr_match.select(result_regs[65].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 264)
    rdata = addr_match.select(result_regs[66].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 268)
    rdata = addr_match.select(result_regs[67].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 272)
    rdata = addr_match.select(result_regs[68].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 276)
    rdata = addr_match.select(result_regs[69].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 280)
    rdata = addr_match.select(result_regs[70].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 284)
    rdata = addr_match.select(result_regs[71].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 288)
    rdata = addr_match.select(result_regs[72].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 292)
    rdata = addr_match.select(result_regs[73].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 296)
    rdata = addr_match.select(result_regs[74].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 300)
    rdata = addr_match.select(result_regs[75].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 304)
    rdata = addr_match.select(result_regs[76].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 308)
    rdata = addr_match.select(result_regs[77].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 312)
    rdata = addr_match.select(result_regs[78].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 316)
    rdata = addr_match.select(result_regs[79].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 320)
    rdata = addr_match.select(result_regs[80].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 324)
    rdata = addr_match.select(result_regs[81].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 328)
    rdata = addr_match.select(result_regs[82].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 332)
    rdata = addr_match.select(result_regs[83].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 336)
    rdata = addr_match.select(result_regs[84].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 340)
    rdata = addr_match.select(result_regs[85].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 344)
    rdata = addr_match.select(result_regs[86].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 348)
    rdata = addr_match.select(result_regs[87].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 352)
    rdata = addr_match.select(result_regs[88].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 356)
    rdata = addr_match.select(result_regs[89].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 360)
    rdata = addr_match.select(result_regs[90].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 364)
    rdata = addr_match.select(result_regs[91].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 368)
    rdata = addr_match.select(result_regs[92].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 372)
    rdata = addr_match.select(result_regs[93].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 376)
    rdata = addr_match.select(result_regs[94].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 380)
    rdata = addr_match.select(result_regs[95].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 384)
    rdata = addr_match.select(result_regs[96].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 388)
    rdata = addr_match.select(result_regs[97].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 392)
    rdata = addr_match.select(result_regs[98].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 396)
    rdata = addr_match.select(result_regs[99].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 400)
    rdata = addr_match.select(result_regs[100].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 404)
    rdata = addr_match.select(result_regs[101].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 408)
    rdata = addr_match.select(result_regs[102].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 412)
    rdata = addr_match.select(result_regs[103].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 416)
    rdata = addr_match.select(result_regs[104].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 420)
    rdata = addr_match.select(result_regs[105].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 424)
    rdata = addr_match.select(result_regs[106].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 428)
    rdata = addr_match.select(result_regs[107].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 432)
    rdata = addr_match.select(result_regs[108].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 436)
    rdata = addr_match.select(result_regs[109].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 440)
    rdata = addr_match.select(result_regs[110].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 444)
    rdata = addr_match.select(result_regs[111].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 448)
    rdata = addr_match.select(result_regs[112].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 452)
    rdata = addr_match.select(result_regs[113].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 456)
    rdata = addr_match.select(result_regs[114].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 460)
    rdata = addr_match.select(result_regs[115].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 464)
    rdata = addr_match.select(result_regs[116].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 468)
    rdata = addr_match.select(result_regs[117].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 472)
    rdata = addr_match.select(result_regs[118].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 476)
    rdata = addr_match.select(result_regs[119].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 480)
    rdata = addr_match.select(result_regs[120].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 484)
    rdata = addr_match.select(result_regs[121].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 488)
    rdata = addr_match.select(result_regs[122].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 492)
    rdata = addr_match.select(result_regs[123].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 496)
    rdata = addr_match.select(result_regs[124].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 500)
    rdata = addr_match.select(result_regs[125].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 504)
    rdata = addr_match.select(result_regs[126].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 508)
    rdata = addr_match.select(result_regs[127].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 512)
    rdata = addr_match.select(result_regs[128].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 516)
    rdata = addr_match.select(result_regs[129].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 520)
    rdata = addr_match.select(result_regs[130].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 524)
    rdata = addr_match.select(result_regs[131].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 528)
    rdata = addr_match.select(result_regs[132].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 532)
    rdata = addr_match.select(result_regs[133].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 536)
    rdata = addr_match.select(result_regs[134].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 540)
    rdata = addr_match.select(result_regs[135].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 544)
    rdata = addr_match.select(result_regs[136].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 548)
    rdata = addr_match.select(result_regs[137].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 552)
    rdata = addr_match.select(result_regs[138].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 556)
    rdata = addr_match.select(result_regs[139].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 560)
    rdata = addr_match.select(result_regs[140].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 564)
    rdata = addr_match.select(result_regs[141].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 568)
    rdata = addr_match.select(result_regs[142].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 572)
    rdata = addr_match.select(result_regs[143].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 576)
    rdata = addr_match.select(result_regs[144].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 580)
    rdata = addr_match.select(result_regs[145].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 584)
    rdata = addr_match.select(result_regs[146].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 588)
    rdata = addr_match.select(result_regs[147].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 592)
    rdata = addr_match.select(result_regs[148].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 596)
    rdata = addr_match.select(result_regs[149].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 600)
    rdata = addr_match.select(result_regs[150].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 604)
    rdata = addr_match.select(result_regs[151].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 608)
    rdata = addr_match.select(result_regs[152].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 612)
    rdata = addr_match.select(result_regs[153].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 616)
    rdata = addr_match.select(result_regs[154].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 620)
    rdata = addr_match.select(result_regs[155].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 624)
    rdata = addr_match.select(result_regs[156].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 628)
    rdata = addr_match.select(result_regs[157].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 632)
    rdata = addr_match.select(result_regs[158].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 636)
    rdata = addr_match.select(result_regs[159].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 640)
    rdata = addr_match.select(result_regs[160].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 644)
    rdata = addr_match.select(result_regs[161].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 648)
    rdata = addr_match.select(result_regs[162].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 652)
    rdata = addr_match.select(result_regs[163].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 656)
    rdata = addr_match.select(result_regs[164].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 660)
    rdata = addr_match.select(result_regs[165].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 664)
    rdata = addr_match.select(result_regs[166].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 668)
    rdata = addr_match.select(result_regs[167].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 672)
    rdata = addr_match.select(result_regs[168].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 676)
    rdata = addr_match.select(result_regs[169].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 680)
    rdata = addr_match.select(result_regs[170].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 684)
    rdata = addr_match.select(result_regs[171].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 688)
    rdata = addr_match.select(result_regs[172].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 692)
    rdata = addr_match.select(result_regs[173].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 696)
    rdata = addr_match.select(result_regs[174].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 700)
    rdata = addr_match.select(result_regs[175].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 704)
    rdata = addr_match.select(result_regs[176].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 708)
    rdata = addr_match.select(result_regs[177].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 712)
    rdata = addr_match.select(result_regs[178].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 716)
    rdata = addr_match.select(result_regs[179].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 720)
    rdata = addr_match.select(result_regs[180].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 724)
    rdata = addr_match.select(result_regs[181].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 728)
    rdata = addr_match.select(result_regs[182].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 732)
    rdata = addr_match.select(result_regs[183].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 736)
    rdata = addr_match.select(result_regs[184].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 740)
    rdata = addr_match.select(result_regs[185].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 744)
    rdata = addr_match.select(result_regs[186].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 748)
    rdata = addr_match.select(result_regs[187].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 752)
    rdata = addr_match.select(result_regs[188].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 756)
    rdata = addr_match.select(result_regs[189].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 760)
    rdata = addr_match.select(result_regs[190].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 764)
    rdata = addr_match.select(result_regs[191].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 768)
    rdata = addr_match.select(result_regs[192].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 772)
    rdata = addr_match.select(result_regs[193].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 776)
    rdata = addr_match.select(result_regs[194].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 780)
    rdata = addr_match.select(result_regs[195].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 784)
    rdata = addr_match.select(result_regs[196].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 788)
    rdata = addr_match.select(result_regs[197].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 792)
    rdata = addr_match.select(result_regs[198].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 796)
    rdata = addr_match.select(result_regs[199].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 800)
    rdata = addr_match.select(result_regs[200].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 804)
    rdata = addr_match.select(result_regs[201].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 808)
    rdata = addr_match.select(result_regs[202].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 812)
    rdata = addr_match.select(result_regs[203].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 816)
    rdata = addr_match.select(result_regs[204].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 820)
    rdata = addr_match.select(result_regs[205].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 824)
    rdata = addr_match.select(result_regs[206].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 828)
    rdata = addr_match.select(result_regs[207].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 832)
    rdata = addr_match.select(result_regs[208].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 836)
    rdata = addr_match.select(result_regs[209].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 840)
    rdata = addr_match.select(result_regs[210].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 844)
    rdata = addr_match.select(result_regs[211].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 848)
    rdata = addr_match.select(result_regs[212].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 852)
    rdata = addr_match.select(result_regs[213].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 856)
    rdata = addr_match.select(result_regs[214].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 860)
    rdata = addr_match.select(result_regs[215].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 864)
    rdata = addr_match.select(result_regs[216].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 868)
    rdata = addr_match.select(result_regs[217].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 872)
    rdata = addr_match.select(result_regs[218].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 876)
    rdata = addr_match.select(result_regs[219].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 880)
    rdata = addr_match.select(result_regs[220].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 884)
    rdata = addr_match.select(result_regs[221].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 888)
    rdata = addr_match.select(result_regs[222].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 892)
    rdata = addr_match.select(result_regs[223].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 896)
    rdata = addr_match.select(result_regs[224].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 900)
    rdata = addr_match.select(result_regs[225].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 904)
    rdata = addr_match.select(result_regs[226].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 908)
    rdata = addr_match.select(result_regs[227].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 912)
    rdata = addr_match.select(result_regs[228].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 916)
    rdata = addr_match.select(result_regs[229].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 920)
    rdata = addr_match.select(result_regs[230].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 924)
    rdata = addr_match.select(result_regs[231].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 928)
    rdata = addr_match.select(result_regs[232].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 932)
    rdata = addr_match.select(result_regs[233].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 936)
    rdata = addr_match.select(result_regs[234].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 940)
    rdata = addr_match.select(result_regs[235].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 944)
    rdata = addr_match.select(result_regs[236].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 948)
    rdata = addr_match.select(result_regs[237].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 952)
    rdata = addr_match.select(result_regs[238].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 956)
    rdata = addr_match.select(result_regs[239].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 960)
    rdata = addr_match.select(result_regs[240].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 964)
    rdata = addr_match.select(result_regs[241].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 968)
    rdata = addr_match.select(result_regs[242].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 972)
    rdata = addr_match.select(result_regs[243].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 976)
    rdata = addr_match.select(result_regs[244].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 980)
    rdata = addr_match.select(result_regs[245].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 984)
    rdata = addr_match.select(result_regs[246].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 988)
    rdata = addr_match.select(result_regs[247].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 992)
    rdata = addr_match.select(result_regs[248].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 996)
    rdata = addr_match.select(result_regs[249].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 1000)
    rdata = addr_match.select(result_regs[250].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 1004)
    rdata = addr_match.select(result_regs[251].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 1008)
    rdata = addr_match.select(result_regs[252].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 1012)
    rdata = addr_match.select(result_regs[253].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 1016)
    rdata = addr_match.select(result_regs[254].out().zext(width=64), rdata)
    addr_match = mem_raddr == (base_addr + 0x410 + 1020)
    rdata = addr_match.select(result_regs[255].out().zext(width=64), rdata)

    m.output("mem_rdata", rdata)


build.__pycircuit_name__ = "janus_cube_pyc"
