"""One rule atomically updates scalar and indexed persistent variables."""

import agentic_circuit as ac


@ac.struct
class Entry:
    index: ac.u2
    value: ac.u8


@ac.rule
def allocate(tail, entries, incoming):
    entries[tail] = incoming
    tail = tail + 1
    return incoming


@ac.system
def multi_state_allocate(incoming: Entry) -> Entry:
    tail: ac.u2 = 0
    entries: list[Entry] = [0] * 4
    allocated = allocate(tail, entries, incoming)
    return allocated
