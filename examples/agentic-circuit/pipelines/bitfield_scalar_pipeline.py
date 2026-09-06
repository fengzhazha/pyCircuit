"""Scalar bitfield permutation used for C++/Verilog parity."""

from __future__ import annotations

import agentic_circuit as ac

WORD = ac.BitfieldSpec(
    width=32,
    fields={
        "high15": (31, 17),
        "low17": (16, 0),
        "mode": (2, 0),
    },
)


@ac.rule
def permute(word):
    return WORD.update(
        ac.concat(WORD(word).low17, WORD(word).high15),
        mode=WORD(word).mode,
    )


@ac.system
def bitfield_scalar_pipeline(incoming: ac.bits[32]) -> ac.bits[32]:
    updated = permute(incoming)
    return updated
