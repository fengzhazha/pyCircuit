from __future__ import annotations

from pycircuit import Circuit, Reg, Wire
from pycircuit.dsl import Signal


def build_byte_mem(
    m: Circuit,
    clk: Signal,
    rst: Signal,
    *,
    raddr: Wire,
    wvalid: Wire,
    waddr: Reg,
    wdata: Reg,
    wstrb: Wire,
    depth_bytes: int,
    name: str,
) -> Wire:
    return m.byte_mem(clk, rst, raddr=raddr, wvalid=wvalid, waddr=waddr, wdata=wdata, wstrb=wstrb, depth=depth_bytes, name=name)

