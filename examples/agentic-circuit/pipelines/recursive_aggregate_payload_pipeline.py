"""Nominal enum and struct elements nested inside immutable aggregates."""

from __future__ import annotations

from enum import Enum

import agentic_circuit as ac


class Mode(Enum):
    IDLE = 0
    RUN = 1


@ac.struct
class Header:
    code: ac.bits[3]


@ac.struct
class Packet:
    tagged: tuple[Mode, ac.bits[3]]
    nested: tuple[Header, ac.bits[3]]
    modes: ac.array[2, Mode]
    flag: bool


@ac.rule
def update(item):
    return item.with_fields(
        tagged=(Mode.RUN, item.tagged[1]),
        nested=(
            item.nested[0].with_fields(code=item.nested[0].code + 1),
            item.nested[1],
        ),
        modes=(item.modes[1], item.modes[0]),
        flag=item.modes[0] == Mode.IDLE,
    )


@ac.system
def recursive_aggregate_payload_pipeline(incoming: Packet) -> Packet:
    updated = update(incoming)
    return updated
