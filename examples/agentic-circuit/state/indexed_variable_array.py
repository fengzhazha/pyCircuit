"""Persistent Python list storage selected by MLIR from generic ac.var IR."""

import agentic_circuit as ac


@ac.struct
class Entry:
    index: ac.u2
    value: ac.u8


@ac.rule
def replace(entries, incoming):
    old = entries[incoming.index]
    entries[incoming.index] = incoming
    return old


@ac.system
def indexed_variable_array(incoming: Entry) -> Entry:
    entries: list[Entry] = [0] * 5
    outgoing = replace(entries, incoming)
    return outgoing
