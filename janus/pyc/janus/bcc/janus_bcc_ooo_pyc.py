from __future__ import annotations

from pycircuit import Circuit

from janus.bcc.ooo.core import build_bcc_ooo


def build(m: Circuit, *, mem_bytes: int = (1 << 20)) -> None:
    # `build(m, ...)` is compiled by the pyCircuit AST/JIT frontend.
    #
    # Most structural construction (arrays of regs, loops, etc.) is done in
    # normal Python helpers called from here.
    build_bcc_ooo(m, mem_bytes=mem_bytes)


build.__pycircuit_name__ = "janus_bcc_ooo_pyc"

