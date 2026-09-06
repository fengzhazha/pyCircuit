"""Named 32-bit decode views lowered to verified generic bit operations."""

from __future__ import annotations

import agentic_circuit as ac

INSTRUCTION = ac.BitfieldSpec(
    width=32,
    fields={
        "opcode": (31, 26),
        "rd": (25, 21),
        "imm17": (20, 4),
        "mode": (3, 1),
        "low25": (24, 0),
    },
)


@ac.struct
class DecodeItem:
    word: ac.bits[32]
    opcode: ac.bits[6]
    opcode_rd: ac.bits[11]
    immediate: ac.bits[17]
    mode: ac.bits[3]
    rd: ac.bits[5]
    low25: ac.bits[25]
    updated: ac.bits[32]


@ac.rule
def decode(item):
    return item.with_fields(
        opcode=INSTRUCTION(item.word).opcode,
        opcode_rd=INSTRUCTION(item.word)["opcode", "rd"],
        immediate=INSTRUCTION.view(item.word).imm17,
        updated=INSTRUCTION.update(item.word, mode=item.mode, rd=item.rd),
    )


@ac.system
def bitfield_decode_pipeline(incoming: DecodeItem) -> DecodeItem:
    decoded = decode(incoming)
    return decoded
