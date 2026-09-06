"""Recursive nominal payload update with compiler-derived queue mechanics."""

from __future__ import annotations

import agentic_circuit as ac


@ac.struct
class Packet:
    header: Header
    payload: ac.bits[17]


@ac.struct
class Header:
    opcode: ac.bits[6]
    mode: ac.bits[3]


@ac.rule
def advance(item):
    return item.with_fields(
        header=item.header.with_fields(mode=item.header.mode + 1),
    )


@ac.system
def nested_payload_pipeline(incoming: Packet) -> Packet:
    updated = advance(incoming)
    return updated
