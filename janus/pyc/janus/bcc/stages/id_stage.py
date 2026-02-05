from __future__ import annotations

from pycircuit import Circuit, Wire, jit_inline

from ..decode import decode_window
from ..pipeline import IdExRegs, IfIdRegs, RegFiles
from ..regfile import read_reg
from ..util import Consts


@jit_inline
def build_id_stage(m: Circuit, *, do_id: Wire, ifid: IfIdRegs, idex: IdExRegs, rf: RegFiles, consts: Consts) -> None:
    with m.scope("ID"):
        # Stage inputs.
        window = ifid.window.out()

        # Combinational decode.
        dec = decode_window(m, window)

        # Pipeline regs: ID/EX.
        op = dec.op
        len_bytes = dec.len_bytes
        regdst = dec.regdst
        srcl = dec.srcl
        srcr = dec.srcr
        srcp = dec.srcp
        imm = dec.imm

        idex.op.set(op, when=do_id)
        idex.len_bytes.set(len_bytes, when=do_id)
        idex.regdst.set(regdst, when=do_id)
        idex.srcl.set(srcl, when=do_id)
        idex.srcr.set(srcr, when=do_id)
        idex.srcp.set(srcp, when=do_id)
        idex.imm.set(imm, when=do_id)

        # Read register file values (mux-based, strict defaulting).
        srcl_val = read_reg(m, srcl, gpr=rf.gpr, t=rf.t, u=rf.u, default=consts.zero64)
        srcr_val = read_reg(m, srcr, gpr=rf.gpr, t=rf.t, u=rf.u, default=consts.zero64)
        srcp_val = read_reg(m, srcp, gpr=rf.gpr, t=rf.t, u=rf.u, default=consts.zero64)

        idex.srcl_val.set(srcl_val, when=do_id)
        idex.srcr_val.set(srcr_val, when=do_id)
        idex.srcp_val.set(srcp_val, when=do_id)

