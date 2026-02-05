from __future__ import annotations

from pycircuit import Circuit, Reg, Wire, jit_inline


@jit_inline
def build_if_stage(m: Circuit, *, do_if: Wire, ifid_window: Reg, mem_rdata: Wire) -> None:
    # IF stage: latch instruction window.
    with m.scope("IF"):
        ifid_window.set(mem_rdata, when=do_if)

