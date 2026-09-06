"""Storage-neutral list selection with one read-only persistent owner."""

import agentic_circuit as ac


@ac.struct
class Entry:
    index: ac.u2
    age: ac.u8
    src0_tag: ac.u6
    src1_tag: ac.u6
    valid: bool


@ac.struct
class Wakeup:
    tag: ac.u6


@ac.rule
def wake(ready_tags, wakeup):
    ready_tags[wakeup.tag] = True


@ac.rule
def issue(entries, ready_tags):
    selected = ac.find(
        entries,
        where=lambda entry: (
            entry.valid and ready_tags[entry.src0_tag] and ready_tags[entry.src1_tag]
        ),
        key=lambda entry: entry.age,
    )
    if selected.valid:
        entries[selected.index] = selected.value.with_fields(valid=False)
        return selected.value


@ac.module
def find_module(wakeup: Wakeup) -> Entry:
    entries: list[Entry] = [0] * 4
    ready_tags: list[bool] = [False] * 64
    wake(ready_tags, wakeup)
    issued = issue(entries, ready_tags)
    return issued


@ac.system
def list_find_capture(wakeup: Wakeup) -> Entry:
    issued = find_module(wakeup)
    return issued
