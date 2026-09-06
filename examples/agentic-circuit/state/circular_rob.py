"""A compiler-inferred four-entry circular reorder buffer.

The environment must not retain a completion across 2^16 reuses of one slot or
2^16 recovery epochs; this is the explicit finite-tag anti-ABA contract.
"""

import agentic_circuit as ac


@ac.struct
class RobEvent:
    index: ac.u2
    generation: ac.u16
    epoch: ac.u16
    value: ac.u16
    done: bool


@ac.rule
def recover(head, tail, count, epoch, request):
    head = tail
    tail = tail
    count = 0
    epoch = epoch + 1


@ac.rule
def allocate(tail, count, epoch, entries, request):
    old = entries[tail]
    if count != 4:
        allocated = request.with_fields(
            index=tail,
            generation=old.generation + 1,
            epoch=epoch,
            done=False,
        )
        entries[tail] = allocated
        tail = tail + 1
        count = count + 1
        epoch = epoch
        return allocated


@ac.rule
def complete(epoch, entries, completion):
    old = entries[completion.index]
    entries[completion.index] = old.with_fields(
        done=(old.done == True)
        | (
            (old.generation == completion.generation)
            & (old.epoch == completion.epoch)
            & (old.epoch == epoch)
        )
    )
    epoch = epoch


@ac.rule
def retire(head, count, entries):
    old = entries[head]
    if (count != 0) & (old.done == True):
        entries[head] = old.with_fields(done=False)
        head = head + 1
        count = count - 1
        return old


@ac.system
def circular_rob(
    flush_request: RobEvent,
    allocate_request: RobEvent,
    completion: RobEvent,
) -> tuple[RobEvent, RobEvent]:
    head: ac.u2 = 0
    tail: ac.u2 = 0
    count: ac.u3 = 0
    epoch: ac.u16 = 0
    entries: list[RobEvent] = [0] * 4

    recover(head, tail, count, epoch, flush_request)
    allocated = allocate(tail, count, epoch, entries, allocate_request)
    complete(epoch, entries, completion)
    retired = retire(head, count, entries)
    return allocated, retired
