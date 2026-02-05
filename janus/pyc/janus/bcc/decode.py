from __future__ import annotations

from dataclasses import dataclass

from pycircuit import Circuit, Wire

from .isa import (
    OP_ADDTPC,
    OP_ADDI,
    OP_ADDIW,
    OP_ADDW,
    OP_ANDW,
    OP_BSTART_STD_CALL,
    OP_CMP_EQ,
    OP_CSEL,
    OP_C_BSTOP,
    OP_C_BSTART_COND,
    OP_C_BSTART_STD,
    OP_C_LWI,
    OP_C_MOVI,
    OP_C_MOVR,
    OP_C_SETC_EQ,
    OP_C_SETC_TGT,
    OP_C_SETRET,
    OP_C_SWI,
    OP_EBREAK,
    OP_HL_LUI,
    OP_INVALID,
    OP_LWI,
    OP_ORW,
    OP_SDI,
    OP_SUBI,
    OP_SWI,
    OP_XORW,
    REG_INVALID,
)
from .util import masked_eq


@dataclass(frozen=True)
class Decode:
    op: Wire
    len_bytes: Wire
    regdst: Wire
    srcl: Wire
    srcr: Wire
    srcp: Wire
    imm: Wire


@dataclass(frozen=True)
class DecodeBundle:
    """Up to 4 sequential decodes out of an 8-byte (64-bit) window.

    `valid[i]` indicates that decode `dec[i]` is present in the window and does
    not cross the 8-byte boundary. Invalid/unknown instructions are still
    treated as present (op=OP_INVALID) and retain an encoded `len_bytes` so that
    fetch alignment stays correct at window boundaries.
    """

    valid: list[Wire]
    off_bytes: list[Wire]
    dec: list[Decode]
    count: Wire
    total_len_bytes: Wire


def decode_window(m: Circuit, window: Wire) -> Decode:
    c = m.const_wire

    zero3 = c(0, width=3)
    zero64 = c(0, width=64)

    insn16 = window.trunc(width=16)
    insn32 = window.trunc(width=32)
    insn48 = window.trunc(width=48)

    low4 = insn16[0:4]
    is_hl = low4.eq(c(0xE, width=4))

    is32 = insn16[0]
    in32 = (~is_hl) & is32
    in16 = (~is_hl) & (~is32)

    rd32 = insn32[7:12].zext(width=6)
    rs1_32 = insn32[15:20].zext(width=6)
    rs2_32 = insn32[20:25].zext(width=6)
    srcp_32 = insn32[27:32].zext(width=6)

    imm12_u64 = insn32[20:32].zext(width=64)
    imm12_s64 = insn32[20:32].sext(width=64)

    imm20_s64 = insn32[12:32].sext(width=64)

    # SWI simm12 is split: {insn32[11:7], insn32[31:25]}.
    swi_lo5 = insn32[7:12]
    swi_hi7 = insn32[25:32]
    simm12_raw = swi_lo5.zext(width=12).shl(amount=7) | swi_hi7.zext(width=12)
    simm12_s64 = simm12_raw.sext(width=64)
    simm17_s64 = insn32[15:32].sext(width=64)

    # HL.LUI immediate packing (48-bit):
    # pfx = insn48[15:0]; main = insn48[47:16]
    pfx16 = insn48.trunc(width=16)
    main32 = insn48[16:48]
    imm_hi12 = pfx16[4:16]
    imm_lo20 = main32[12:32]
    imm32 = imm_hi12.zext(width=32).shl(amount=20) | imm_lo20.zext(width=32)
    imm_hl_lui = imm32.sext(width=64)

    rd_hl = main32[7:12].zext(width=6)

    # 16-bit fields.
    rd16 = insn16[11:16].zext(width=6)
    rs16 = insn16[6:11].zext(width=6)
    # Immediate fields:
    # - simm5_11_s5: bits[15:11] (used by loads/stores)
    # - simm5_6_s5: bits[10:6] (used by C.MOVI / C.SETRET)
    simm5_11_s64 = insn16[11:16].sext(width=64)
    simm5_6_s64 = insn16[6:11].sext(width=64)
    simm12_s64_c = insn16[4:16].sext(width=64)
    uimm5_u64 = insn16[6:11].zext(width=64)
    brtype_u64 = insn16[11:14].zext(width=64)

    op = c(OP_INVALID, width=6)

    # Instruction length is encoded by the prefix bits (independent of opcode
    # validity). Keep `len_bytes` meaningful even for OP_INVALID so that fetch
    # alignment and bundle packing behave correctly at window boundaries.
    ln = c(2, width=3)
    ln = in32.select(c(4, width=3), ln)
    ln = is_hl.select(c(6, width=3), ln)
    regdst = c(REG_INVALID, width=6)
    srcl = c(REG_INVALID, width=6)
    srcr = c(REG_INVALID, width=6)
    srcp = c(REG_INVALID, width=6)
    imm = zero64

    def apply(cond: Wire, *, opv: int, lnv: int, regdstv: Wire, srclv: Wire, srcrv: Wire, srcpv: Wire, immv: Wire) -> None:
        nonlocal op, ln, regdst, srcl, srcr, srcp, imm
        op = cond.select(c(opv, width=6), op)
        ln = cond.select(c(lnv, width=3), ln)
        regdst = cond.select(regdstv, regdst)
        srcl = cond.select(srclv, srcl)
        srcr = cond.select(srcrv, srcr)
        srcp = cond.select(srcpv, srcp)
        imm = cond.select(immv, imm)

    def apply32(cond: Wire, *, opv: int, regdstv: Wire, srclv: Wire, srcrv: Wire, srcpv: Wire, immv: Wire) -> None:
        apply(cond, opv=opv, lnv=4, regdstv=regdstv, srclv=srclv, srcrv=srcrv, srcpv=srcpv, immv=immv)

    # --- 16-bit decode (reverse priority; C.BSTOP highest) ---
    apply(
        in16 & masked_eq(m, insn16, width=16, mask=0x003F, match=0x0016),
        opv=OP_C_MOVI,
        lnv=2,
        regdstv=rd16,
        srclv=c(REG_INVALID, width=6),
        srcrv=c(REG_INVALID, width=6),
        srcpv=c(REG_INVALID, width=6),
        immv=simm5_6_s64,
    )
    apply(
        in16 & masked_eq(m, insn16, width=16, mask=0xF83F, match=0x5016),
        opv=OP_C_SETRET,
        lnv=2,
        regdstv=c(10, width=6),  # ra
        srclv=c(REG_INVALID, width=6),
        srcrv=c(REG_INVALID, width=6),
        srcpv=c(REG_INVALID, width=6),
        immv=uimm5_u64.shl(amount=1),
    )
    apply(
        in16 & masked_eq(m, insn16, width=16, mask=0x003F, match=0x002A),
        opv=OP_C_SWI,
        lnv=2,
        regdstv=c(REG_INVALID, width=6),
        srclv=rs16,
        srcrv=c(24, width=6),
        srcpv=c(REG_INVALID, width=6),
        immv=simm5_11_s64,
    )
    apply(
        in16 & masked_eq(m, insn16, width=16, mask=0x003F, match=0x000A),
        opv=OP_C_LWI,
        lnv=2,
        regdstv=c(REG_INVALID, width=6),
        srclv=rs16,
        srcrv=c(REG_INVALID, width=6),
        srcpv=c(REG_INVALID, width=6),
        immv=simm5_11_s64,
    )
    apply(
        in16 & masked_eq(m, insn16, width=16, mask=0x003F, match=0x0006),
        opv=OP_C_MOVR,
        lnv=2,
        regdstv=rd16,
        srclv=rs16,
        srcrv=c(REG_INVALID, width=6),
        srcpv=c(REG_INVALID, width=6),
        immv=zero64,
    )
    apply(
        in16 & masked_eq(m, insn16, width=16, mask=0x003F, match=0x0026),
        opv=OP_C_SETC_EQ,
        lnv=2,
        regdstv=c(REG_INVALID, width=6),
        srclv=rs16,
        srcrv=rd16,
        srcpv=c(REG_INVALID, width=6),
        immv=zero64,
    )
    apply(
        in16 & masked_eq(m, insn16, width=16, mask=0xF83F, match=0x001C),
        opv=OP_C_SETC_TGT,
        lnv=2,
        regdstv=c(REG_INVALID, width=6),
        srclv=rs16,
        srcrv=c(REG_INVALID, width=6),
        srcpv=c(REG_INVALID, width=6),
        immv=zero64,
    )
    apply(
        in16 & masked_eq(m, insn16, width=16, mask=0x000F, match=0x0004),
        opv=OP_C_BSTART_COND,
        lnv=2,
        regdstv=c(REG_INVALID, width=6),
        srclv=c(REG_INVALID, width=6),
        srcrv=c(REG_INVALID, width=6),
        srcpv=c(REG_INVALID, width=6),
        immv=simm12_s64_c.shl(amount=1),
    )
    apply(
        in16 & masked_eq(m, insn16, width=16, mask=0xC7FF, match=0x0000),
        opv=OP_C_BSTART_STD,
        lnv=2,
        regdstv=c(REG_INVALID, width=6),
        srclv=c(REG_INVALID, width=6),
        srcrv=c(REG_INVALID, width=6),
        srcpv=c(REG_INVALID, width=6),
        immv=brtype_u64,
    )
    apply(
        in16 & masked_eq(m, insn16, width=16, mask=0xFFFF, match=0x0000),
        opv=OP_C_BSTOP,
        lnv=2,
        regdstv=c(REG_INVALID, width=6),
        srclv=c(REG_INVALID, width=6),
        srcrv=c(REG_INVALID, width=6),
        srcpv=c(REG_INVALID, width=6),
        immv=zero64,
    )

    # --- 32-bit decode (reverse priority; EBREAK highest) ---
    apply32(
        in32 & masked_eq(m, insn32, width=32, mask=0x0000707F, match=0x00004025),
        opv=OP_XORW,
        regdstv=rd32,
        srclv=rs1_32,
        srcrv=rs2_32,
        srcpv=c(REG_INVALID, width=6),
        immv=zero64,
    )
    apply32(
        in32 & masked_eq(m, insn32, width=32, mask=0x0000707F, match=0x00002025),
        opv=OP_ANDW,
        regdstv=rd32,
        srclv=rs1_32,
        srcrv=rs2_32,
        srcpv=c(REG_INVALID, width=6),
        immv=zero64,
    )
    apply32(
        in32 & masked_eq(m, insn32, width=32, mask=0x0000707F, match=0x00003025),
        opv=OP_ORW,
        regdstv=rd32,
        srclv=rs1_32,
        srcrv=rs2_32,
        srcpv=c(REG_INVALID, width=6),
        immv=zero64,
    )
    apply32(
        in32 & masked_eq(m, insn32, width=32, mask=0x0000707F, match=0x00000025),
        opv=OP_ADDW,
        regdstv=rd32,
        srclv=rs1_32,
        srcrv=rs2_32,
        srcpv=c(REG_INVALID, width=6),
        immv=zero64,
    )
    apply32(
        in32 & masked_eq(m, insn32, width=32, mask=0x0000707F, match=0x00000077),
        opv=OP_CSEL,
        regdstv=rd32,
        srclv=rs1_32,
        srcrv=rs2_32,
        srcpv=srcp_32,
        immv=zero64,
    )
    apply32(
        in32 & masked_eq(m, insn32, width=32, mask=0x0000707F, match=0x00002059),
        opv=OP_SWI,
        regdstv=c(REG_INVALID, width=6),
        srclv=rs1_32,
        srcrv=rs2_32,
        srcpv=c(REG_INVALID, width=6),
        immv=simm12_s64,
    )
    apply32(
        in32 & masked_eq(m, insn32, width=32, mask=0x0000707F, match=0x00003059),
        opv=OP_SDI,
        regdstv=c(REG_INVALID, width=6),
        srclv=rs1_32,
        srcrv=rs2_32,
        srcpv=c(REG_INVALID, width=6),
        immv=simm12_s64,
    )
    apply32(
        in32 & masked_eq(m, insn32, width=32, mask=0x0000707F, match=0x00002019),
        opv=OP_LWI,
        regdstv=rd32,
        srclv=rs1_32,
        srcrv=c(REG_INVALID, width=6),
        srcpv=c(REG_INVALID, width=6),
        immv=imm12_s64,
    )
    apply32(
        in32 & masked_eq(m, insn32, width=32, mask=0x0000707F, match=0x00000035),
        opv=OP_ADDIW,
        regdstv=rd32,
        srclv=rs1_32,
        srcrv=c(REG_INVALID, width=6),
        srcpv=c(REG_INVALID, width=6),
        immv=imm12_u64,
    )
    apply32(
        in32 & masked_eq(m, insn32, width=32, mask=0x0000707F, match=0x00000015),
        opv=OP_ADDI,
        regdstv=rd32,
        srclv=rs1_32,
        srcrv=c(REG_INVALID, width=6),
        srcpv=c(REG_INVALID, width=6),
        immv=imm12_u64,
    )
    apply32(
        in32 & masked_eq(m, insn32, width=32, mask=0x0000707F, match=0x00001015),
        opv=OP_SUBI,
        regdstv=rd32,
        srclv=rs1_32,
        srcrv=c(REG_INVALID, width=6),
        srcpv=c(REG_INVALID, width=6),
        immv=imm12_u64,
    )
    apply32(
        in32 & masked_eq(m, insn32, width=32, mask=0xF800707F, match=0x00000045),
        opv=OP_CMP_EQ,
        regdstv=rd32,
        srclv=rs1_32,
        srcrv=rs2_32,
        srcpv=c(REG_INVALID, width=6),
        immv=zero64,
    )
    apply32(
        in32 & masked_eq(m, insn32, width=32, mask=0xF0FFFFFF, match=0x0010102B),
        opv=OP_EBREAK,
        regdstv=c(REG_INVALID, width=6),
        srclv=c(REG_INVALID, width=6),
        srcrv=c(REG_INVALID, width=6),
        srcpv=c(REG_INVALID, width=6),
        immv=zero64,
    )
    apply32(
        in32 & masked_eq(m, insn32, width=32, mask=0x0000007F, match=0x00000007),
        opv=OP_ADDTPC,
        regdstv=rd32,
        srclv=c(REG_INVALID, width=6),
        srcrv=c(REG_INVALID, width=6),
        srcpv=c(REG_INVALID, width=6),
        immv=imm20_s64.shl(amount=12),
    )
    apply32(
        in32 & masked_eq(m, insn32, width=32, mask=0x00007FFF, match=0x00004001),
        opv=OP_BSTART_STD_CALL,
        regdstv=c(REG_INVALID, width=6),
        srclv=c(REG_INVALID, width=6),
        srcrv=c(REG_INVALID, width=6),
        srcpv=c(REG_INVALID, width=6),
        immv=simm17_s64.shl(amount=1),
    )

    # --- 48-bit HL.LUI (highest priority overall) ---
    apply(
        is_hl & masked_eq(m, insn48, width=48, mask=0x0000007F000F, match=0x00000017000E),
        opv=OP_HL_LUI,
        lnv=6,
        regdstv=rd_hl,
        srclv=c(REG_INVALID, width=6),
        srcrv=c(REG_INVALID, width=6),
        srcpv=c(REG_INVALID, width=6),
        immv=imm_hl_lui,
    )

    return Decode(op=op, len_bytes=ln, regdst=regdst, srcl=srcl, srcr=srcr, srcp=srcp, imm=imm)


def decode_bundle_8B(m: Circuit, window: Wire) -> DecodeBundle:
    """Decode up to 4 instructions out of a 64-bit window (LSB = first byte)."""
    c = m.const_wire

    # Pre-shifted windows (byte offsets: 0/2/4/6).
    win2 = window.lshr(amount=16)
    win4 = window.lshr(amount=32)
    win6 = window.lshr(amount=48)

    d0 = decode_window(m, window)
    len0 = d0.len_bytes
    len0_nz = ~len0.eq(c(0, width=3))

    # Lane 1 window select based on len0.
    w1 = window
    w1 = len0.eq(c(2, width=3)).select(win2, w1)
    w1 = len0.eq(c(4, width=3)).select(win4, w1)
    w1 = len0.eq(c(6, width=3)).select(win6, w1)
    d1 = decode_window(m, w1)

    # Offsets and fit checks are in bytes, use a wider accumulator to avoid wrap.
    len0_u = len0.zext(width=5)
    len1_u = d1.len_bytes.zext(width=5)
    off0 = c(0, width=5)
    off1 = len0_u
    end1 = off1 + len1_u
    fit1 = end1.ult(c(9, width=5))  # end <= 8
    v0 = c(1, width=1)
    v1 = len0_nz & fit1

    # Lane 2 select based on (len0+len1).
    len1 = d1.len_bytes
    len1_nz = ~len1.eq(c(0, width=3))
    off2 = end1
    w2 = window
    w2 = off2.eq(c(2, width=5)).select(win2, w2)
    w2 = off2.eq(c(4, width=5)).select(win4, w2)
    w2 = off2.eq(c(6, width=5)).select(win6, w2)
    d2 = decode_window(m, w2)
    len2_u = d2.len_bytes.zext(width=5)
    end2 = off2 + len2_u
    fit2 = end2.ult(c(9, width=5))
    v2 = v1 & len1_nz & fit2

    # Lane 3 select based on (len0+len1+len2).
    len2 = d2.len_bytes
    len2_nz = ~len2.eq(c(0, width=3))
    off3 = end2
    w3 = window
    w3 = off3.eq(c(2, width=5)).select(win2, w3)
    w3 = off3.eq(c(4, width=5)).select(win4, w3)
    w3 = off3.eq(c(6, width=5)).select(win6, w3)
    d3 = decode_window(m, w3)
    len3_u = d3.len_bytes.zext(width=5)
    end3 = off3 + len3_u
    fit3 = end3.ult(c(9, width=5))
    v3 = v2 & len2_nz & fit3

    cnt = v0.zext(width=3) + v1.zext(width=3) + v2.zext(width=3) + v3.zext(width=3)

    total = len0_u
    total = v1.select(total + len1_u, total)
    total = v2.select(total + len2_u, total)
    total = v3.select(total + len3_u, total)

    return DecodeBundle(
        valid=[v0, v1, v2, v3],
        off_bytes=[off0, off1, off2, off3],
        dec=[d0, d1, d2, d3],
        count=cnt,
        total_len_bytes=total,
    )
