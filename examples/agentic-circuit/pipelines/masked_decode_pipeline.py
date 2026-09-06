"""Decode a compact opcode with one compiler-verified masked match."""

import agentic_circuit as ac


@ac.struct
class Instruction:
    opcode: ac.bits[4]
    is_compute: bool


@ac.rule
def decode(instruction):
    return instruction.with_fields(
        is_compute=ac.matches(instruction.opcode, "1xx0"),
    )


@ac.system
def masked_decode_pipeline(instruction: Instruction) -> Instruction:
    decoded = decode(instruction)
    return decoded
