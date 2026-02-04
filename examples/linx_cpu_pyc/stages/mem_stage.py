from __future__ import annotations

from pycircuit import Circuit, Wire, jit_inline

from ..pipeline import ExMemRegs, MemWbRegs
from ..util import Consts


@jit_inline
def build_mem_stage(m: Circuit, *, do_mem: Wire, exmem: ExMemRegs, memwb: MemWbRegs, mem_rdata: Wire, consts: Consts) -> None:
    with m.scope("MEM"):
        # Stage inputs.
        op = exmem.op.out()
        len_bytes = exmem.len_bytes.out()
        regdst = exmem.regdst.out()
        alu = exmem.alu.out()
        is_load = exmem.is_load.out()
        is_store = exmem.is_store.out()

        # Combinational.
        load32 = mem_rdata.trunc(width=32)
        load64 = load32.sext(width=64)
        mem_val = is_load.select(load64, alu)
        mem_val = is_store.select(consts.zero64, mem_val)

        # Pipeline regs: MEM/WB.
        memwb.op.set(op, when=do_mem)
        memwb.len_bytes.set(len_bytes, when=do_mem)
        memwb.regdst.set(regdst, when=do_mem)
        memwb.value.set(mem_val, when=do_mem)
