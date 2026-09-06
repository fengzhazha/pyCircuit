"""Standard Python enum carried through a nested nominal payload."""

from __future__ import annotations

from enum import Enum

import agentic_circuit as ac


class Mode(Enum):
    IDLE = 0
    RUN = 1
    WAIT = 2


@ac.struct
class Header:
    opcode: ac.bits[6]
    mode: Mode


@ac.struct
class Packet:
    header: Header
    payload: ac.bits[17]
    matched: bool


@ac.rule
def classify(item):
    return item.with_fields(
        header=item.header.with_fields(mode=Mode.RUN),
        matched=item.header.mode == Mode.WAIT,
    )


@ac.system
def enum_payload_pipeline(incoming: Packet) -> Packet:
    updated = classify(incoming)
    return updated
