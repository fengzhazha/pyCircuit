from __future__ import annotations

from pycircuit import Circuit, Wire, jit_inline

from ..isa import (
    OP_ADDTPC,
    OP_ADDI,
    OP_ADDIW,
    OP_ADD,
    OP_ANDI,
    OP_ANDIW,
    OP_BSTART_STD_COND,
    OP_BSTART_STD_DIRECT,
    OP_BSTART_STD_FALL,
    OP_ADDW,
    OP_ANDW,
    OP_BXU,
    OP_BSTART_STD_CALL,
    OP_CMP_EQ,
    OP_CMP_LTUI,
    OP_CMP_LTU,
    OP_C_ADD,
    OP_C_ADDI,
    OP_C_BSTART_COND,
    OP_C_BSTART_DIRECT,
    OP_C_BSTART_STD,
    OP_C_OR,
    OP_CSEL,
    OP_C_LDI,
    OP_C_LWI,
    OP_C_MOVI,
    OP_C_MOVR,
    OP_C_SETC_EQ,
    OP_C_SETC_NE,
    OP_C_SETC_TGT,
    OP_C_SDI,
    OP_C_SEXT_W,
    OP_C_SETRET,
    OP_C_SWI,
    OP_C_ZEXT_W,
    OP_HL_LB_PCR,
    OP_HL_LBU_PCR,
    OP_HL_LD_PCR,
    OP_HL_LH_PCR,
    OP_HL_LHU_PCR,
    OP_HL_LUI,
    OP_HL_LW_PCR,
    OP_HL_LWU_PCR,
    OP_HL_SB_PCR,
    OP_HL_SD_PCR,
    OP_HL_SH_PCR,
    OP_HL_SW_PCR,
    OP_LB,
    OP_LBUI,
    OP_LDI,
    OP_LUI,
    OP_LW,
    OP_LWI,
    OP_OR,
    OP_ORI,
    OP_ORIW,
    OP_ORW,
    OP_SETC_GEUI,
    OP_SETRET,
    OP_SLL,
    OP_SLLI,
    OP_SDI,
    OP_SRL,
    OP_SRLIW,
    OP_SUB,
    OP_SUBI,
    OP_SWI,
    OP_XORW,
    REG_INVALID,
)
from ..pipeline import ExMemRegs, IdExRegs
from ..util import Consts, ashr_var, lshr_var, shl_var


@jit_inline
def build_ex_stage(
    m: Circuit,
    *,
    do_ex: Wire,
    idex: IdExRegs,
    exmem: ExMemRegs,
    consts: Consts,
    # Forwarding sources (from MEM and WB stages, both lanes).
    mem0_fwd_valid: Wire,
    mem0_fwd_regdst: Wire,
    mem0_fwd_value: Wire,
    mem1_fwd_valid: Wire,
    mem1_fwd_regdst: Wire,
    mem1_fwd_value: Wire,
    wb0_fwd_valid: Wire,
    wb0_fwd_regdst: Wire,
    wb0_fwd_value: Wire,
    wb1_fwd_valid: Wire,
    wb1_fwd_regdst: Wire,
    wb1_fwd_value: Wire,
    # Forwarded T/U stack views for this EX stage (after applying older in-flight
    # pushes/clears from WB+MEM).
    t0_fwd: Wire,
    t1_fwd: Wire,
    t2_fwd: Wire,
    t3_fwd: Wire,
    u0_fwd: Wire,
    u1_fwd: Wire,
    u2_fwd: Wire,
    u3_fwd: Wire,
) -> None:
    with m.scope("EX"):
        z1 = consts.zero1
        z4 = consts.zero4
        z64 = consts.zero64

        # Stage inputs.
        pc = idex.pc.out()
        window = idex.window.out()
        pred_next_pc = idex.pred_next_pc.out()
        op = idex.op.out()
        len_bytes = idex.len_bytes.out()
        regdst = idex.regdst.out()
        srcl = idex.srcl.out()
        srcr = idex.srcr.out()
        srcr_type = idex.srcr_type.out()
        shamt = idex.shamt.out()
        srcp = idex.srcp.out()
        srcl_val = idex.srcl_val.out()
        srcr_val = idex.srcr_val.out()
        srcp_val = idex.srcp_val.out()
        imm = idex.imm.out()

        # Operand forwarding (priority: younger > older; MEM stage > WB stage; lane1 > lane0).
        can_fwd_mem0 = mem0_fwd_valid & (mem0_fwd_regdst != REG_INVALID) & (mem0_fwd_regdst != 0)
        can_fwd_mem1 = mem1_fwd_valid & (mem1_fwd_regdst != REG_INVALID) & (mem1_fwd_regdst != 0)
        can_fwd_wb0 = wb0_fwd_valid & (wb0_fwd_regdst != REG_INVALID) & (wb0_fwd_regdst != 0)
        can_fwd_wb1 = wb1_fwd_valid & (wb1_fwd_regdst != REG_INVALID) & (wb1_fwd_regdst != 0)

        # Apply sources from oldest -> youngest so later matches override.
        if can_fwd_wb0 & (wb0_fwd_regdst == srcl):
            srcl_val = wb0_fwd_value
        if can_fwd_wb1 & (wb1_fwd_regdst == srcl):
            srcl_val = wb1_fwd_value
        if can_fwd_mem0 & (mem0_fwd_regdst == srcl):
            srcl_val = mem0_fwd_value
        if can_fwd_mem1 & (mem1_fwd_regdst == srcl):
            srcl_val = mem1_fwd_value

        if can_fwd_wb0 & (wb0_fwd_regdst == srcr):
            srcr_val = wb0_fwd_value
        if can_fwd_wb1 & (wb1_fwd_regdst == srcr):
            srcr_val = wb1_fwd_value
        if can_fwd_mem0 & (mem0_fwd_regdst == srcr):
            srcr_val = mem0_fwd_value
        if can_fwd_mem1 & (mem1_fwd_regdst == srcr):
            srcr_val = mem1_fwd_value

        if can_fwd_wb0 & (wb0_fwd_regdst == srcp):
            srcp_val = wb0_fwd_value
        if can_fwd_wb1 & (wb1_fwd_regdst == srcp):
            srcp_val = wb1_fwd_value
        if can_fwd_mem0 & (mem0_fwd_regdst == srcp):
            srcp_val = mem0_fwd_value
        if can_fwd_mem1 & (mem1_fwd_regdst == srcp):
            srcp_val = mem1_fwd_value

        # T/U stack bypass (codes: T0..T3 = 24..27, U0..U3 = 28..31).
        if srcl == 24:
            srcl_val = t0_fwd
        if srcl == 25:
            srcl_val = t1_fwd
        if srcl == 26:
            srcl_val = t2_fwd
        if srcl == 27:
            srcl_val = t3_fwd
        if srcl == 28:
            srcl_val = u0_fwd
        if srcl == 29:
            srcl_val = u1_fwd
        if srcl == 30:
            srcl_val = u2_fwd
        if srcl == 31:
            srcl_val = u3_fwd

        if srcr == 24:
            srcr_val = t0_fwd
        if srcr == 25:
            srcr_val = t1_fwd
        if srcr == 26:
            srcr_val = t2_fwd
        if srcr == 27:
            srcr_val = t3_fwd
        if srcr == 28:
            srcr_val = u0_fwd
        if srcr == 29:
            srcr_val = u1_fwd
        if srcr == 30:
            srcr_val = u2_fwd
        if srcr == 31:
            srcr_val = u3_fwd

        if srcp == 24:
            srcp_val = t0_fwd
        if srcp == 25:
            srcp_val = t1_fwd
        if srcp == 26:
            srcp_val = t2_fwd
        if srcp == 27:
            srcp_val = t3_fwd
        if srcp == 28:
            srcp_val = u0_fwd
        if srcp == 29:
            srcp_val = u1_fwd
        if srcp == 30:
            srcp_val = u2_fwd
        if srcp == 31:
            srcp_val = u3_fwd

        op_c_bstart_std = op == OP_C_BSTART_STD
        op_c_bstart_cond = op == OP_C_BSTART_COND
        op_c_bstart_direct = op == OP_C_BSTART_DIRECT
        op_bstart_std_fall = op == OP_BSTART_STD_FALL
        op_bstart_std_direct = op == OP_BSTART_STD_DIRECT
        op_bstart_std_cond = op == OP_BSTART_STD_COND
        op_bstart_std_call = op == OP_BSTART_STD_CALL
        op_c_movr = op == OP_C_MOVR
        op_c_movi = op == OP_C_MOVI
        op_c_setret = op == OP_C_SETRET
        op_c_setc_eq = op == OP_C_SETC_EQ
        op_c_setc_ne = op == OP_C_SETC_NE
        op_c_setc_tgt = op == OP_C_SETC_TGT
        op_setret = op == OP_SETRET
        op_addtpc = op == OP_ADDTPC
        op_lui = op == OP_LUI
        op_add = op == OP_ADD
        op_sub = op == OP_SUB
        op_or = op == OP_OR
        op_addi = op == OP_ADDI
        op_subi = op == OP_SUBI
        op_andi = op == OP_ANDI
        op_ori = op == OP_ORI
        op_addiw = op == OP_ADDIW
        op_andiw = op == OP_ANDIW
        op_oriw = op == OP_ORIW
        op_sll = op == OP_SLL
        op_srl = op == OP_SRL
        op_slli = op == OP_SLLI
        op_srliw = op == OP_SRLIW
        op_bxu = op == OP_BXU
        op_addw = op == OP_ADDW
        op_orw = op == OP_ORW
        op_andw = op == OP_ANDW
        op_xorw = op == OP_XORW
        op_cmp_eq = op == OP_CMP_EQ
        op_cmp_ltu = op == OP_CMP_LTU
        op_cmp_ltui = op == OP_CMP_LTUI
        op_setc_geui = op == OP_SETC_GEUI
        op_csel = op == OP_CSEL
        op_hl_lui = op == OP_HL_LUI
        op_hl_lb_pcr = op == OP_HL_LB_PCR
        op_hl_lbu_pcr = op == OP_HL_LBU_PCR
        op_hl_lh_pcr = op == OP_HL_LH_PCR
        op_hl_lhu_pcr = op == OP_HL_LHU_PCR
        op_hl_lw_pcr = op == OP_HL_LW_PCR
        op_hl_lwu_pcr = op == OP_HL_LWU_PCR
        op_hl_ld_pcr = op == OP_HL_LD_PCR
        op_hl_sb_pcr = op == OP_HL_SB_PCR
        op_hl_sh_pcr = op == OP_HL_SH_PCR
        op_hl_sw_pcr = op == OP_HL_SW_PCR
        op_hl_sd_pcr = op == OP_HL_SD_PCR
        op_lwi = op == OP_LWI
        op_c_lwi = op == OP_C_LWI
        op_lbui = op == OP_LBUI
        op_lb = op == OP_LB
        op_lw = op == OP_LW
        op_ldi = op == OP_LDI
        op_c_add = op == OP_C_ADD
        op_c_addi = op == OP_C_ADDI
        op_c_or = op == OP_C_OR
        op_c_ldi = op == OP_C_LDI
        op_swi = op == OP_SWI
        op_c_swi = op == OP_C_SWI
        op_c_sdi = op == OP_C_SDI
        op_c_sext_w = op == OP_C_SEXT_W
        op_c_zext_w = op == OP_C_ZEXT_W
        op_sdi = op == OP_SDI

        off = imm.shl(amount=2)

        alu = z64
        is_load = z1
        is_store = z1
        size = z4
        addr = z64
        wdata = z64

        # --- SrcR modifiers (QEMU reference: linx_srcR_addsub / linx_srcR_logic) ---
        srcr_addsub = srcr_val
        if srcr_type == 0:
            srcr_addsub = srcr_val.trunc(width=32).sext(width=64)
        if srcr_type == 1:
            srcr_addsub = srcr_val.trunc(width=32).zext(width=64)
        if srcr_type == 2:
            srcr_addsub = (~srcr_val) + 1

        srcr_logic = srcr_val
        if srcr_type == 0:
            srcr_logic = srcr_val.trunc(width=32).sext(width=64)
        if srcr_type == 1:
            srcr_logic = srcr_val.trunc(width=32).zext(width=64)
        if srcr_type == 2:
            srcr_logic = ~srcr_val

        srcr_addsub_shl = shl_var(m, srcr_addsub, shamt)
        srcr_logic_shl = shl_var(m, srcr_logic, shamt)

        # Address-index modifier (QEMU reference: linx_addr_add_reg): idx_type 0 => .sw else .uw.
        idx_mod = srcr_val.trunc(width=32).zext(width=64)
        if srcr_type == 0:
            idx_mod = srcr_val.trunc(width=32).sext(width=64)
        idx_mod_shl = shl_var(m, idx_mod, shamt)

        # Block markers: forward imm through ALU (used for control-state updates in WB).
        if (
            op_c_bstart_std
            | op_c_bstart_cond
            | op_c_bstart_direct
            | op_bstart_std_fall
            | op_bstart_std_direct
            | op_bstart_std_cond
            | op_bstart_std_call
        ):
            alu = imm
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # MOVR: pass-through.
        if op_c_movr:
            alu = srcl_val
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # MOVI: immediate.
        if op_c_movi:
            alu = imm
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # SETRET: ra = PC + off (off already shifted by 1 in decode).
        if op_c_setret:
            alu = pc + imm
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # SETC.EQ / SETC.TGT: internal control state updates (committed in WB).
        setc_eq = z64
        if srcl_val == srcr_val:
            setc_eq = 1
        if op_c_setc_eq:
            alu = setc_eq
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64
        if op_c_setc_tgt:
            alu = srcl_val
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # ADDTPC: PC + (imm<<12) (imm already shifted by 12 in decode).
        pc_page = pc & 0xFFFF_FFFF_FFFF_F000
        if op_addtpc:
            alu = pc_page + imm
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # ADDI/SUBI/ADDIW: srcl +/- imm.
        if op_addi:
            alu = srcl_val + imm
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64
        subi = srcl_val + ((~imm) + 1)
        if op_subi:
            alu = subi
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64
        addiw = (srcl_val.trunc(width=32) + imm.trunc(width=32)).sext(width=64)
        if op_addiw:
            alu = addiw
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # LUI: immediate (already shifted by 12 in decode).
        if op_lui:
            alu = imm
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # SETRET: ra = pc + (imm<<1) (imm already shifted by 1 in decode).
        if op_setret:
            alu = pc + imm
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # ADD/SUB/OR: 64-bit ops with SrcR modifiers + optional shift.
        if op_add:
            alu = srcl_val + srcr_addsub_shl
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64
        if op_sub:
            alu = srcl_val - srcr_addsub_shl
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64
        if op_or:
            alu = srcl_val | srcr_logic_shl
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # ANDI/ORI: immediate logic ops.
        if op_andi:
            alu = srcl_val & imm
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64
        if op_ori:
            alu = srcl_val | imm
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # ANDIW/ORIW: word immediate ops (sign-extend low32).
        if op_andiw:
            alu = (srcl_val & imm).trunc(width=32).sext(width=64)
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64
        if op_oriw:
            alu = (srcl_val | imm).trunc(width=32).sext(width=64)
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # Shifts.
        if op_sll:
            alu = shl_var(m, srcl_val, srcr_val)
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64
        if op_srl:
            alu = lshr_var(m, srcl_val, srcr_val)
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64
        if op_slli:
            alu = shl_var(m, srcl_val, shamt)
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64
        if op_srliw:
            l32 = srcl_val.trunc(width=32).zext(width=64)
            sh5 = shamt & 0x1F
            shifted = lshr_var(m, l32, sh5)
            alu = shifted.trunc(width=32).sext(width=64)
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # BXU: unsigned bit-field extract.
        if op_bxu:
            imms = srcr
            imml = srcp
            shifted = lshr_var(m, srcl_val, imms)
            sh_mask_amt = m.const(63, width=64) - imml.zext(width=64)
            mask = lshr_var(m, m.const(0xFFFF_FFFF_FFFF_FFFF, width=64), sh_mask_amt)
            extracted = shifted & mask
            valid = (imms.zext(width=64) + imml.zext(width=64)).ule(63)
            alu = valid.select(extracted, z64)
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # ADDW/ORW/ANDW/XORW: word ops with SrcR modifiers + optional shift.
        addw = (srcl_val + srcr_addsub_shl).trunc(width=32).sext(width=64)
        orw = (srcl_val | srcr_logic_shl).trunc(width=32).sext(width=64)
        andw = (srcl_val & srcr_logic_shl).trunc(width=32).sext(width=64)
        xorw = (srcl_val ^ srcr_logic_shl).trunc(width=32).sext(width=64)
        if op_addw:
            alu = addw
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64
        if op_orw:
            alu = orw
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64
        if op_andw:
            alu = andw
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64
        if op_xorw:
            alu = xorw
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # CMP_EQ: (srcl == SrcR_mod) ? 1 : 0 (SrcR_mod uses addsub modifiers, no shift).
        srcr_addsub_nosh = srcr_addsub
        cmp = z64
        if srcl_val == srcr_addsub_nosh:
            cmp = 1
        if op_cmp_eq:
            alu = cmp
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # CMP.LTU: unsigned compare against modified SrcR.
        cmp_ltu = z64
        if srcl_val.ult(srcr_addsub_nosh):
            cmp_ltu = 1
        if op_cmp_ltu:
            alu = cmp_ltu
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # CMP.LTUI: unsigned compare against immediate.
        cmp_ltui = z64
        if srcl_val.ult(imm):
            cmp_ltui = 1
        if op_cmp_ltui:
            alu = cmp_ltui
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # SETC.GEUI / C.SETC.NE: update commit_cond (committed in WB).
        setc_bit = z64
        if op_setc_geui:
            uimm = shl_var(m, imm, shamt)
            if srcl_val.uge(uimm):
                setc_bit = 1
            alu = setc_bit
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64
        if op_c_setc_ne:
            if srcl_val != srcr_val:
                setc_bit = 1
            alu = setc_bit
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # HL.LUI: imm.
        if op_hl_lui:
            alu = imm
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # CSEL: (srcp != 0) ? srcr : srcl.
        csel_srcr = srcr_addsub_nosh
        csel_val = srcl_val
        if srcp_val != 0:
            csel_val = csel_srcr
        if op_csel:
            alu = csel_val
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # LWI / C.LWI: load word, address = srcl + (imm << 2)
        is_lwi = op_lwi | op_c_lwi
        lwi_addr = srcl_val + off
        if is_lwi:
            alu = z64
            is_load = 1
            is_store = z1
            size = 4
            addr = lwi_addr
            wdata = z64

        # LBUI: load unsigned byte.
        if op_lbui:
            alu = z64
            is_load = 1
            is_store = z1
            size = 1
            addr = srcl_val + imm
            wdata = z64

        # LB/LW: indexed loads.
        idx_addr = srcl_val + idx_mod_shl
        if op_lb:
            alu = z64
            is_load = 1
            is_store = z1
            size = 1
            addr = idx_addr
            wdata = z64
        if op_lw:
            alu = z64
            is_load = 1
            is_store = z1
            size = 4
            addr = idx_addr
            wdata = z64

        # LDI / C.LDI: load 8B (unsigned).
        ldi_off = imm.shl(amount=3)
        if op_ldi | op_c_ldi:
            alu = z64
            is_load = 1
            is_store = z1
            size = 8
            addr = srcl_val + ldi_off
            wdata = z64

        # SWI / C.SWI: store word (4 bytes)
        store_addr = srcl_val + off
        store_data = srcr_val
        if op_swi:
            store_addr = srcr_val + off
            store_data = srcl_val
        if op_swi | op_c_swi:
            alu = z64
            is_load = z1
            is_store = 1
            size = 4
            addr = store_addr
            wdata = store_data

        sdi_off = imm.shl(amount=3)

        # C.SDI: store double word from T0 at base SrcL + simm5*8.
        if op_c_sdi:
            alu = z64
            is_load = z1
            is_store = 1
            size = 8
            addr = srcl_val + sdi_off
            wdata = srcr_val

        # SDI: store double word (8 bytes), addr = SrcR + (simm12<<3)
        sdi_addr = srcr_val + sdi_off
        if op_sdi:
            alu = z64
            is_load = z1
            is_store = 1
            size = 8
            addr = sdi_addr
            wdata = srcl_val

        # HL.<load>.PCR: PC-relative loads (imm already sign-extended in decode).
        if op_hl_lb_pcr | op_hl_lbu_pcr:
            alu = z64
            is_load = 1
            is_store = z1
            size = 1
            addr = pc + imm
            wdata = z64
        if op_hl_lh_pcr | op_hl_lhu_pcr:
            alu = z64
            is_load = 1
            is_store = z1
            size = 2
            addr = pc + imm
            wdata = z64
        if op_hl_lw_pcr | op_hl_lwu_pcr:
            alu = z64
            is_load = 1
            is_store = z1
            size = 4
            addr = pc + imm
            wdata = z64
        if op_hl_ld_pcr:
            alu = z64
            is_load = 1
            is_store = z1
            size = 8
            addr = pc + imm
            wdata = z64

        # HL.<store>.PCR: PC-relative stores.
        if op_hl_sb_pcr:
            alu = z64
            is_load = z1
            is_store = 1
            size = 1
            addr = pc + imm
            wdata = srcl_val
        if op_hl_sh_pcr:
            alu = z64
            is_load = z1
            is_store = 1
            size = 2
            addr = pc + imm
            wdata = srcl_val
        if op_hl_sw_pcr:
            alu = z64
            is_load = z1
            is_store = 1
            size = 4
            addr = pc + imm
            wdata = srcl_val
        if op_hl_sd_pcr:
            alu = z64
            is_load = z1
            is_store = 1
            size = 8
            addr = pc + imm
            wdata = srcl_val

        # C.ADDI / C.ADD / C.OR: push to T-hand.
        if op_c_addi:
            alu = srcl_val + imm
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64
        if op_c_add:
            alu = srcl_val + srcr_val
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64
        if op_c_or:
            alu = srcl_val | srcr_val
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # C.SEXT.W / C.ZEXT.W: push word-extended to T-hand.
        if op_c_sext_w:
            alu = srcl_val.trunc(width=32).sext(width=64)
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64
        if op_c_zext_w:
            alu = srcl_val.trunc(width=32).zext(width=64)
            is_load = z1
            is_store = z1
            size = z4
            addr = z64
            wdata = z64

        # Pipeline regs: EX/MEM.
        exmem.pc.set(pc, when=do_ex)
        exmem.window.set(window, when=do_ex)
        exmem.pred_next_pc.set(pred_next_pc, when=do_ex)
        exmem.op.set(op, when=do_ex)
        exmem.len_bytes.set(len_bytes, when=do_ex)
        exmem.regdst.set(regdst, when=do_ex)
        exmem.srcl.set(srcl, when=do_ex)
        exmem.srcr.set(srcr, when=do_ex)
        exmem.imm.set(imm, when=do_ex)
        exmem.alu.set(alu, when=do_ex)
        exmem.is_load.set(is_load, when=do_ex)
        exmem.is_store.set(is_store, when=do_ex)
        exmem.size.set(size, when=do_ex)
        exmem.addr.set(addr, when=do_ex)
        exmem.wdata.set(wdata, when=do_ex)
