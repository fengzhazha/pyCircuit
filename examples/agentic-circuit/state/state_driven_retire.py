"""A guarded state-driven retire rule with no Queue input."""

import agentic_circuit as ac


@ac.struct
class Entry:
    index: ac.u1
    value: ac.u7
    valid: bool


@ac.rule
def allocate(entries, incoming):
    entries[incoming.index] = incoming


@ac.rule
def retire(entries):
    old = entries[0]
    if old.valid:
        entries[0] = old.with_fields(valid=False)
        return old


@ac.system
def state_driven_retire(incoming: Entry) -> Entry:
    entries: list[Entry] = [0] * 2
    allocate(entries, incoming)
    retired = retire(entries)
    return retired
