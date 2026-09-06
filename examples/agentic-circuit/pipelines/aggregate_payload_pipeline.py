"""Tuple and fixed value-array payloads with immutable structural updates."""

from __future__ import annotations

import agentic_circuit as ac


@ac.struct
class AggregatePacket:
    pair: tuple[ac.bits[3], ac.bits[5]]
    lanes: ac.array[4, ac.bits[4]]
    selected: ac.bits[4]


@ac.rule
def rotate(item):
    return item.with_fields(
        pair=(item.pair[0] + 1, item.pair[1] + 1),
        lanes=(item.lanes[1], item.lanes[2], item.lanes[3], item.lanes[0]),
        selected=item.lanes[2],
    )


@ac.system
def aggregate_payload_pipeline(incoming: AggregatePacket) -> AggregatePacket:
    updated = rotate(incoming)
    return updated
