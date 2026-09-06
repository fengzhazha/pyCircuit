"""Two reusable four-entry ROB instances authored as ordinary typed Python."""

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
    head = tail  # noqa: F841 - captured as a compiler-visible state proposal
    tail = tail
    count = 0  # noqa: F841 - captured as a compiler-visible state proposal
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
    if old.generation != completion.generation:
        return
    if (old.epoch != completion.epoch) | (old.epoch != epoch):
        return
    entries[completion.index] = old.with_fields(done=True)


@ac.rule
def retire(head, count, entries):
    old = entries[head]
    if (count != 0) & (old.done == True):  # noqa: E712 - width-exact comparison
        entries[head] = old.with_fields(done=False)
        head = head + 1
        count = count - 1
        return old


@ac.module
def rob(
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


@ac.system
def reusable_circular_rob(
    left_flush: RobEvent,
    left_allocate: RobEvent,
    left_completion: RobEvent,
    right_flush: RobEvent,
    right_allocate: RobEvent,
    right_completion: RobEvent,
) -> tuple[RobEvent, RobEvent, RobEvent, RobEvent]:
    left_allocated, left_retired = rob(left_flush, left_allocate, left_completion)
    right_allocated, right_retired = rob(right_flush, right_allocate, right_completion)
    return left_allocated, left_retired, right_allocated, right_retired
