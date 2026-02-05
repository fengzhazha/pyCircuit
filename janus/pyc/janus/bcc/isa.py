from __future__ import annotations

# NOTE: These op IDs are an internal encoding for the Janus BCC bring-up model.
# Keep them stable across backends/tests (C++ + SystemVerilog).

OP_INVALID = 0
OP_C_BSTART_STD = 1
OP_C_BSTOP = 2
OP_C_MOVR = 3
OP_C_LWI = 4
OP_C_SWI = 5
OP_SUBI = 6
OP_ADDI = 7
OP_ADDIW = 8
OP_LWI = 9
OP_SWI = 10
OP_ADDW = 11
OP_ORW = 12
OP_ANDW = 13
OP_XORW = 14
OP_CMP_EQ = 15
OP_CSEL = 16
OP_HL_LUI = 17
OP_EBREAK = 18

# --- extended ops for BlockISA control flow + PC-relative + wider memory ---
OP_C_BSTART_COND = 19
OP_BSTART_STD_CALL = 20
OP_C_MOVI = 21
OP_C_SETRET = 22
OP_C_SETC_EQ = 23
OP_C_SETC_TGT = 24
OP_ADDTPC = 25
OP_SDI = 26

REG_INVALID = 0x3F

ST_IF = 0
ST_ID = 1
ST_EX = 2
ST_MEM = 3
ST_WB = 4

# Block transition kinds (internal control state, not ISA encodings).
BK_FALL = 0
BK_COND = 1
BK_CALL = 2
BK_RET = 3

