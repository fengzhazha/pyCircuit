"""Two reusable oldest-ready issue queues using ordinary persistent lists."""

import agentic_circuit as ac


@ac.struct
class IssueEntry:
    index: ac.u2
    age: ac.u8
    src0_tag: ac.u6
    src1_tag: ac.u6
    value: ac.u16
    valid: bool


@ac.struct
class Readiness:
    tag: ac.u6
    ready: bool


@ac.rule
def update_ready(ready_tags, event):
    ready_tags[event.tag] = event.ready


@ac.rule
def dispatch(entries, request):
    free = ac.find(entries, where=lambda entry: not entry.valid)
    if free.valid:
        installed = request.with_fields(index=free.index, valid=True)
        entries[free.index] = installed


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
def isq(request: IssueEntry, readiness: Readiness) -> IssueEntry:
    entries: list[IssueEntry] = [0] * 4
    ready_tags: list[bool] = [False] * 64

    update_ready(ready_tags, readiness)
    dispatch(entries, request)
    issued = issue(entries, ready_tags)
    return issued


@ac.system
def reusable_oldest_ready_isq(
    left_request: IssueEntry,
    left_readiness: Readiness,
    right_request: IssueEntry,
    right_readiness: Readiness,
) -> tuple[IssueEntry, IssueEntry]:
    left_issued = isq(left_request, left_readiness)
    right_issued = isq(right_request, right_readiness)
    return left_issued, right_issued
