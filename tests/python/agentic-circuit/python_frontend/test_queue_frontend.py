from __future__ import annotations

import unittest

SOURCE = """
from agentic_circuit import sink, source, system

@system
def pipeline() -> None:
    input_queue = source(int, depth=4, latency=1)
    output_queue = input_queue.apply(lambda item: item, depth=8, latency=2)
    sink(output_queue)
"""

INFERRED_MODULE_SOURCE = """
import agentic_circuit as ac

@ac.module
def increment(value: ac.u8) -> ac.u8:
    return value + 1

@ac.system
def pipeline(left: ac.u8, right: ac.u8) -> tuple[ac.u8, ac.u8]:
    left_result = increment(left)
    right_result = increment(right)
    return left_result, right_result
"""

INFERRED_NESTED_MODULE_SOURCE = """
import agentic_circuit as ac

@ac.module
def increment(value: ac.u8) -> ac.u8:
    return value + 1

@ac.module
def wrapper(value: ac.u8) -> ac.u8:
    return increment(value)

@ac.system
def pipeline(left: ac.u8, right: ac.u8) -> tuple[ac.u8, ac.u8]:
    left_result = wrapper(left)
    right_result = wrapper(right)
    return left_result, right_result
"""

INFERRED_STATEFUL_MODULE_SOURCE = """
import agentic_circuit as ac

@ac.module
def accumulator(value: ac.u8) -> ac.u8:
    total: ac.u8 = 0
    total = total + value
    return total

@ac.system
def pipeline(left: ac.u8, right: ac.u8) -> tuple[ac.u8, ac.u8]:
    left_total = accumulator(left)
    right_total = accumulator(right)
    return left_total, right_total
"""

INFERRED_MULTI_STATE_MODULE_SOURCE = """
import agentic_circuit as ac

@ac.module
def tally(value: ac.u8) -> ac.u8:
    count: ac.u8 = 0
    total: ac.u8 = 0
    count = count + 1
    total = total + value
    return total + count

@ac.system
def pipeline(left: ac.u8, right: ac.u8) -> tuple[ac.u8, ac.u8]:
    left_result = tally(left)
    right_result = tally(right)
    return left_result, right_result
"""

POPCOUNT_SOURCE = """
import agentic_circuit as ac
from agentic_circuit import sink, source, struct, system

@struct
class Item:
    value: u8
    count: u4

@system
def pipeline() -> None:
    input_queue = source(Item)
    output_queue = input_queue.apply(
        lambda item: item.with_fields(count=ac.popcount(item.value)),
        depth=2,
        latency=1,
    )
    sink(output_queue)
"""

ZERO_COUNT_SOURCE = """
import agentic_circuit as ac
from agentic_circuit import sink, source, struct, system

@struct
class Item:
    value: u13
    leading: u4
    trailing: u4

@system
def pipeline() -> None:
    input_queue = source(Item)
    output_queue = input_queue.apply(
        lambda item: item.with_fields(
            leading=ac.count_leading_zeros(item.value),
            trailing=ac.count_trailing_zeros(item.value),
        ),
        depth=2,
        latency=1,
    )
    sink(output_queue)
"""

STRUCT_SOURCE = """
from agentic_circuit import sink, source, struct, system

@struct
class WorkItem:
    value: int
    remaining: int

@system
def pipeline() -> None:
    input_queue = source(WorkItem, depth=4, latency=1)
    output_queue = input_queue.apply(
        lambda item: item.with_fields(
            value=(item.value + 1) * 2,
            remaining=item.remaining - 1,
        ),
        depth=8,
        latency=2,
    )
    sink(output_queue)
"""

SCOPE_SOURCE = """
from agentic_circuit import scope, sink, source, system

@system
def pipeline() -> None:
    input_queue = source(int, depth=4, latency=1)
    with scope("frontend"):
        adjusted = input_queue.apply(lambda item: item + 1)
        with scope("inner"):
            completed = adjusted.apply(lambda item: item * 2)
    sink(completed)
"""

BROADCAST_SOURCE = """
from agentic_circuit import sink, source, system

@system
def pipeline() -> None:
    input_queue = source(int)
    left = input_queue.apply(lambda item: item + 1)
    right = input_queue.apply(lambda item: item * 2)
    sink(left)
    sink(right)
"""

CROSS_SCOPE_BROADCAST_SOURCE = """
from agentic_circuit import scope, sink, source, system

@system
def pipeline() -> None:
    input_queue = source(int)
    with scope("left"):
        left = input_queue.apply(lambda item: item + 1)
    with scope("right"):
        right = input_queue.apply(lambda item: item * 2)
    sink(left)
    sink(right)
"""

ROUTE_SOURCE = """
from agentic_circuit import sink, source, struct, system

@struct
class Item:
    value: int
    route: int

@system
def pipeline() -> None:
    input_queue = source(Item)
    left, right = input_queue.route(
        outputs=2,
        key=lambda item: item.route,
        depth=2,
        latency=1,
    )
    merged = left.merge(right, policy="round_robin", depth=3, latency=1)
    sink(merged)
"""

COLLECTION_SOURCE = """
import agentic_circuit as ac

@ac.system
def pipeline() -> None:
    lanes = ac.array(2, lambda lane: ac.source(int, depth=lane + 1))
    named = ac.map({"right": lanes[1], "left": lanes[0]})
    active = ac.set({named["right"], named["left"]})
    for lane in active:
        ac.sink(lane)
"""

NESTED_COLLECTION_SOURCE = """
import agentic_circuit as ac

@ac.system
def pipeline() -> None:
    grid = ac.array(
        2,
        lambda row: ac.array(
            2,
            lambda column: ac.source(int, depth=row + column + 1),
        ),
    )
    ac.sink(grid[1][0])
"""

FEEDBACK_SOURCE = """
from agentic_circuit import sink, source, struct, system

@struct
class Item:
    value: int
    remaining: int

@system
def pipeline() -> None:
    current = source(Item)
    while current.remaining > 0:
        current = current.apply(
            lambda item: item.with_fields(
                value=item.value + 1,
                remaining=item.remaining - 1,
            ),
            depth=2,
            latency=1,
        )
    sink(current)
"""

OBSERVE_SOURCE = """
import agentic_circuit as ac

@ac.system
def pipeline() -> None:
    input_queue = ac.source(int)
    ac.observe(input_queue)
    ac.sink(input_queue)
"""

ATOMIC_SOURCE = """
import agentic_circuit as ac

@ac.system
def pipeline() -> None:
    left = ac.source(int)
    right = ac.source(int)
    with ac.atomic():
        left_next = left.apply(lambda item: item + 1)
        right_next = right.apply(lambda item: item * 2)
    ac.sink(left_next)
    ac.sink(right_next)
"""

STATIC_CONTROL_SOURCE = """
import agentic_circuit as ac

@ac.system
def pipeline() -> None:
    input_queue = ac.source(int)
    if True:
        selected = input_queue.apply(lambda item: item + 1)
    else:
        unreachable = input_queue.apply(lambda item: item + 99)
    lanes = ac.array(2, lambda index: ac.source(int))
    for index in range(2):
        ac.sink(lanes[index])
    ac.sink(selected)
"""

CONST_KEY_MAP_SOURCE = """
import agentic_circuit as ac

@ac.system
def pipeline() -> None:
    one = ac.source(int)
    two = ac.source(int)
    lanes = ac.map({2: two, 1: one})
    ac.sink(lanes[1])
    ac.sink(lanes[2])
"""

WIDTH_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Header:
    value: ac.u32
    route: ac.u2
    remaining: ac.u16
    valid: bool

@ac.system
def pipeline() -> None:
    input_queue = ac.source(Header)
    output_queue = input_queue.apply(
        lambda item: item.with_fields(
            value=item.value + 1,
            remaining=item.remaining - 1,
        )
    )
    ac.sink(output_queue)
"""

BIT_WIDTH_SOURCE = """
import agentic_circuit as ac

@ac.struct
class BitBundle:
    left: ac.u3
    right: ac.u3
    anded: ac.u3
    ored: ac.u3
    xored: ac.u3
    inverted: ac.u3
    shifted_left: ac.u3
    shifted_right: ac.u3
    priority_index: ac.u2
    priority_valid: ac.u1
    tag: ac.u5
    payload: ac.u17
    sequence: ac.u63

@ac.system
def pipeline() -> None:
    input_queue = ac.source(BitBundle)
    output_queue = input_queue.apply(
        lambda item: item.with_fields(
            anded=item.left & item.right,
            ored=item.left | item.right,
            xored=item.left ^ item.right,
            inverted=~item.left,
            shifted_left=item.left << 1,
            shifted_right=item.right >> 1,
            priority_index=ac.priority_encode(item.left, order="low").index,
            priority_valid=ac.priority_encode(item.left, order="low").valid,
        )
    )
    ac.sink(output_queue)
"""

BIT_OPERATION_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Item:
    value: ac.bits[17]
    low: ac.bits[5]
    joined: ac.bits[8]
    updated: ac.bits[17]

@ac.rule
def transform(item):
    return item.with_fields(
        low=item.value[0:5],
        joined=ac.concat(item.value[5:8], item.value[0:5]),
        updated=ac.insert(item.value, item.value[5:8], lsb=9),
    )

@ac.system
def bits_pipeline(item: Item) -> Item:
    result = transform(item)
    return result
"""

MASKED_MATCH_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Item:
    opcode: ac.bits[4]
    enabled: bool
    matched: bool

@ac.rule
def decode(item):
    return item.with_fields(
        matched=ac.matches(item.opcode, "10x1"),
    )

@ac.system
def masked_decode_pipeline(item: Item) -> Item:
    result = decode(item)
    return result
"""

BITFIELD_SOURCE = """
import agentic_circuit as ac

INSTR = ac.BitfieldSpec(width=32, fields={
    "opcode": (31, 26),
    "rd": (25, 21),
    "imm17": (20, 4),
    "mode": (3, 1),
    "low25": (24, 0),
})

@ac.struct
class Item:
    word: ac.bits[32]
    opcode: ac.bits[6]
    opcode_rd: ac.bits[11]
    immediate: ac.bits[17]
    mode: ac.bits[3]
    rd: ac.bits[5]
    low25: ac.bits[25]
    updated: ac.bits[32]

@ac.rule
def decode(item):
    return item.with_fields(
        opcode=INSTR(item.word).opcode,
        opcode_rd=INSTR(item.word)["opcode", "rd"],
        immediate=INSTR.view(item.word).imm17,
        updated=INSTR.update(item.word, mode=item.mode, rd=item.rd),
    )

@ac.system
def bitfield_pipeline(incoming: Item) -> Item:
    result = decode(incoming)
    return result
"""

NESTED_STRUCT_SOURCE = """
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
def nested_struct_pipeline(incoming: Packet) -> Packet:
    updated = advance(incoming)
    return updated
"""

ENUM_PAYLOAD_SOURCE = """
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
"""

AGGREGATE_PAYLOAD_SOURCE = """
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
"""

BOOL_U1_SOURCE = """
from __future__ import annotations

import agentic_circuit as ac

@ac.struct
class LogicalBitPair:
    logical: bool
    bit: ac.u1

@ac.rule
def flip(item):
    return item.with_fields(
        logical=not item.logical,
        bit=~item.bit,
    )

@ac.system
def bool_u1_pipeline(incoming: LogicalBitPair) -> LogicalBitPair:
    updated = flip(incoming)
    return updated
"""

BOOL_U1_COMPARISON_SOURCE = """
import agentic_circuit as ac

@ac.struct
class LogicalBitPair:
    logical: bool
    bit: ac.u1
    matched: bool

@ac.system
def bool_u1_compare() -> None:
    incoming = ac.source(LogicalBitPair)
    outgoing = incoming.apply(
        lambda item: item.with_fields(matched=item.bit == item.logical)
    )
    ac.sink(outgoing)
"""

BOOL_U1_MODULE_SOURCE = """
import agentic_circuit as ac

@ac.module
def bit_identity(value: ac.u1) -> ac.u1:
    return value

@ac.module
def bit_state(value: bool) -> ac.u1:
    saved: ac.u1 = 0
    saved = value
    return saved

@ac.system
def bool_u1_modules(incoming: bool) -> bool:
    identity_result = bit_identity(incoming)
    state_result = bit_state(identity_result)
    return state_result
"""

SCALAR_BIT_SOURCE = """
import agentic_circuit as ac

@ac.system
def pipeline() -> None:
    incoming = ac.source(ac.u7)
    outgoing = incoming.apply(lambda value: (value + 1) ^ 3)
    flag = ac.source(ac.u1)
    toggled = flag.apply(lambda value: value + 1)
    ac.sink(outgoing)
    ac.sink(toggled)
"""

RULE_ROB_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Entry:
    sequence: ac.u4
    value: ac.u16
    done: bool

@ac.rule
def complete(entry):
    return entry.with_fields(done=True)

@ac.system
def rob() -> None:
    issued = ac.source(Entry)
    completed = complete(issued)
    retired = ac.reorder(
        completed,
        by=Entry.sequence,
        entries=8,
    )
    ac.sink(retired)
"""

RULE_PAIR_SOURCE = """
import agentic_circuit as ac

@ac.rule
def increment(value):
    return value + 1

@ac.system
def pair() -> None:
    left = ac.source(int)
    right = ac.source(int)
    left_next = increment(left)
    right_next = increment(right)
    ac.sink(left_next)
    ac.sink(right_next)
"""

MULTI_INPUT_RULE_SOURCE = """
import agentic_circuit as ac

@ac.rule
def add(left, right):
    return left + right

@ac.system
def pair() -> None:
    left = ac.source(int)
    right = ac.source(int)
    summed = add(left, right)
    ac.sink(summed)
"""

INFERRED_BOUNDARY_RULE_SOURCE = """
import agentic_circuit as ac

@ac.rule
def increment(value):
    return value + 1

@ac.system
def pipeline(value: ac.u8) -> ac.u8:
    result = increment(value)
    return result
"""

INFERRED_MULTI_BOUNDARY_SOURCE = """
import agentic_circuit as ac

@ac.rule
def combine(left, right):
    return left + right

@ac.rule
def forward(value):
    return value

@ac.system
def pipeline(left: ac.u8, right: ac.u8) -> tuple[ac.u8, ac.u8]:
    combined = combine(left, right)
    forwarded = forward(right)
    return combined, forwarded
"""

VARIABLE_RULE_SOURCE = """
import agentic_circuit as ac

@ac.rule
def accumulate(count, value):
    count = count + value
    return count

@ac.system
def accumulator() -> None:
    count: ac.u8 = 0
    incoming = ac.source(ac.u8, depth=2)
    outgoing = accumulate(count, incoming)
    ac.sink(outgoing)
"""

STRUCT_VARIABLE_RULE_SOURCE = """
import agentic_circuit as ac

@ac.struct
class State:
    value: ac.u8
    valid: bool

@ac.rule
def update(state, incoming):
    state = state.with_fields(value=state.value + incoming.value, valid=True)
    return state

@ac.system
def accumulator() -> None:
    state: State = 0
    incoming = ac.source(State, depth=2)
    outgoing = update(state, incoming)
    ac.sink(outgoing)
"""

INDEXED_VARIABLE_RULE_SOURCE = """
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
def indexed_state(incoming: Entry) -> Entry:
    entries: list[Entry] = [0] * 4
    outgoing = replace(entries, incoming)
    return outgoing
"""

LIST_FIND_RULE_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Entry:
    index: ac.u2
    age: ac.u8
    value: ac.u8
    valid: bool

@ac.rule
def issue(entries):
    selected = ac.find(
        entries,
        where=lambda entry: entry.valid,
        key=lambda entry: entry.age,
    )
    if selected.valid:
        entries[selected.index] = selected.value.with_fields(valid=False)
        return selected.value

@ac.system
def issue_queue() -> Entry:
    entries: list[Entry] = [0] * 4
    issued = issue(entries)
    return issued
"""

LIST_FIND_CAPTURE_SOURCE = """
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
            entry.valid
            and ready_tags[entry.src0_tag]
            and ready_tags[entry.src1_tag]
        ),
        key=lambda entry: entry.age,
    )
    if selected.valid:
        entries[selected.index] = selected.value.with_fields(valid=False)
        return selected.value

@ac.system
def issue_queue(wakeup: Wakeup) -> Entry:
    entries: list[Entry] = [0] * 4
    ready_tags: list[bool] = [False] * 64
    wake(ready_tags, wakeup)
    issued = issue(entries, ready_tags)
    return issued
"""

LIST_FIND_KEY_CAPTURE_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Entry:
    index: ac.u2
    tag: ac.u2
    valid: bool

@ac.rule
def issue(entries, priorities):
    selected = ac.find(
        entries,
        where=lambda entry: entry.valid,
        key=lambda entry: priorities[entry.tag],
    )
    if selected.valid:
        entries[selected.index] = selected.value.with_fields(valid=False)
        return selected.value

@ac.system
def issue_queue() -> Entry:
    entries: list[Entry] = [0] * 4
    priorities: list[ac.u2] = [0] * 4
    issued = issue(entries, priorities)
    return issued
"""

CONSUME_ONLY_RULE_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Entry:
    index: ac.u2
    generation: ac.u8
    value: ac.u8

@ac.rule
def complete(entries, completion):
    old = entries[completion.index]
    if old.generation != completion.generation:
        return
    entries[completion.index] = completion

@ac.system
def completion_port(completion: Entry) -> None:
    entries: list[Entry] = [0] * 4
    complete(entries, completion)
"""

READ_ONLY_SCALAR_CONDITIONAL_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Entry:
    index: ac.u1
    epoch: ac.u8

@ac.rule
def complete(epoch, entries, completion):
    old = entries[completion.index]
    if old.epoch != completion.epoch:
        return
    if old.epoch != epoch:
        return
    entries[completion.index] = completion

@ac.system
def completion_port(completion: Entry) -> None:
    epoch: ac.u8 = 0
    entries: list[Entry] = [0] * 2
    complete(epoch, entries, completion)
"""

STATE_DRIVEN_RULE_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Entry:
    index: ac.u1
    value: ac.u7
    valid: bool

@ac.rule
def retire(entries):
    old = entries[0]
    if old.valid:
        entries[0] = old.with_fields(valid=False)
        return old

@ac.system
def retirement_port() -> Entry:
    entries: list[Entry] = [0] * 2
    retired = retire(entries)
    return retired
"""

MULTI_STATE_RULE_SOURCE = """
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
"""

BRANCH_LOCAL_STATE_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Command:
    select_right: bool
    value: ac.u8

@ac.rule
def route(left, right, command):
    if command.select_right:
        right = command.value
    else:
        left = command.value

@ac.system
def branch_state(command: Command) -> None:
    left: ac.u8 = 0
    right: ac.u8 = 0
    route(left, right, command)
"""

INDEXED_BRANCH_JOIN_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Command:
    select_right: bool
    left_index: ac.u2
    right_index: ac.u2
    value: ac.u8

@ac.rule
def update(entries, command):
    if command.select_right:
        entries[command.right_index] = command.value
    else:
        entries[command.left_index] = command.value + 1

@ac.system
def indexed_branch_join(command: Command) -> None:
    entries: list[ac.u8] = [0] * 4
    update(entries, command)
"""

OPTIONAL_OUTPUT_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Event:
    emit: bool
    value: ac.u8

@ac.rule
def filter_event(count, event):
    count = count + 1
    if event.emit:
        return event
    return

@ac.system
def optional_output(event: Event) -> Event:
    count: ac.u8 = 0
    filtered = filter_event(count, event)
    return filtered
"""

STATEFUL_RULE_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Entry:
    index: ac.u1
    value: ac.u7

@ac.rule
def install(rob, entry):
    old = rob[entry.index]
    rob[entry.index] = entry
    return old

@ac.system
def table_rule() -> None:
    rob = ac.table[2, Entry](init=0)
    incoming = ac.source(Entry, depth=2)
    outgoing = install(rob, incoming)
    ac.sink(outgoing)
"""

STATEFUL_MULTI_INPUT_RULE_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Entry:
    index: ac.u1
    value: ac.u7

@ac.struct
class Delta:
    amount: ac.u7

@ac.rule
def install(rob, entry, delta):
    old = rob[entry.index]
    rob[entry.index] = entry.with_fields(value=entry.value + delta.amount)
    return old

@ac.system
def table_rule() -> None:
    rob = ac.table[2, Entry](init=0)
    incoming = ac.source(Entry, depth=2)
    deltas = ac.source(Delta, depth=2)
    outgoing = install(rob, incoming, deltas)
    ac.sink(outgoing)
"""

FORK_SOURCE = """
import agentic_circuit as ac

@ac.system
def pipeline() -> None:
    input_queue = ac.source(int)
    left, right = input_queue.fork(outputs=2, depth=2, latency=1)
    ac.sink(left)
    ac.sink(right)
"""

RUNTIME_IF_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Item:
    value: int
    route: int

@ac.system
def pipeline() -> None:
    input_queue = ac.source(Item)
    if input_queue.route == 0:
        output_queue = input_queue.apply(
            lambda item: item.with_fields(value=item.value + 10)
        )
    else:
        output_queue = input_queue.apply(
            lambda item: item.with_fields(value=item.value + 20)
        )
    ac.sink(output_queue)
"""

REORDER_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Token:
    sequence: ac.u64
    value: ac.u32

@ac.system
def pipeline() -> None:
    completed = ac.source(Token)
    retired = completed.reorder(
        key=lambda item: item.sequence,
        capacity=16,
        start=0,
        depth=4,
        latency=1,
    )
    ac.sink(retired)
"""

DEPENDENCY_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Token:
    sequence: ac.u8
    waits_for: ac.u8
    resource: ac.u2
    cycles: ac.u16

@ac.system
def pipeline() -> None:
    issued = ac.source(Token)
    completed = issued.depend(
        key=lambda item: item.sequence,
        waits_for=lambda item: item.waits_for,
        resource=lambda item: item.resource,
        cost=lambda item: item.cycles,
        capacity=16,
        resources=4,
        no_dependency=255,
        depth=8,
        latency=1,
    )
    ac.sink(completed)
"""

MEMORY_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Request:
    address: ac.u8
    write: bool
    data: ac.u16

@ac.system
def pipeline() -> None:
    sram = ac.memory(ac.u16, entries=16, init=0, latency=3)
    requests = ac.source(Request)
    responses = sram.request(
        requests,
        address=lambda item: item.address,
        write=lambda item: item.write,
        data=lambda item: item.data,
        result_field="data",
        depth=4,
    )
    ac.sink(responses)
"""

TABLE_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Entry:
    valid: bool
    done: bool
    result: ac.u16

@ac.struct
class Update:
    index: ac.u4
    enable: bool
    done: bool
    result: ac.u16

@ac.struct
class Request:
    index: ac.u4
    enable: bool

@ac.system
def pipeline() -> None:
    updates = ac.source(Update)
    requests = ac.source(Request)
    state = ac.table[16, Entry](init=0)
    state.view(lambda update: update.index).patch(
        updates,
        enable=lambda update: update.enable,
        valid=True,
        done=lambda update: update.done,
        result=lambda update: update.result,
    )
    responses = state.view(lambda request: request.index).read(
        requests,
        when=lambda request: request.enable,
        depth=1,
        latency=1,
    )
    entry = state.view(0)
    snapshots = entry.read(when=entry.valid, depth=1, latency=1)
    ac.sink(responses)
    ac.sink(snapshots)
"""

SLOT_TABLE_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Entry:
    valid: bool
    tag: ac.u8
    age: ac.u8

@ac.struct
class Request:
    tag: ac.u8

@ac.system
def pipeline() -> None:
    requests = ac.source(Request)
    pending = ac.slot(requests)
    issue = ac.table[4, Entry](init=0)
    ready = issue.match(
        lambda entry: pending.valid and entry.valid
        and entry.tag == pending.value.tag
    )
    grant = issue.choose(
        ready, count=1, policy="min", key=lambda entry: entry.age
    )
    issue.view(grant.index).patch(
        enable=pending.valid and grant.valid, valid=False
    )
    pending.release(when=pending.valid and grant.valid)
    snapshots = issue.view(grant.index).read(when=grant.valid)
    ac.sink(snapshots)
"""

MASKED_TABLE_WRITE_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Entry:
    valid: bool
    tag: ac.u8
    age: ac.u8

@ac.system
def pipeline() -> None:
    updates = ac.source(Entry)
    pending = ac.slot(updates)
    issue = ac.table[4, Entry](init=0)
    hits = issue.match(lambda entry: not entry.valid)
    issue.view(hits).write(enable=pending.valid, value=pending.value)
    pending.release(when=pending.valid)
    snapshots = issue.view(0).read()
    ac.sink(snapshots)
"""

MULTI_WRITER_TABLE_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Entry:
    valid: bool
    src0_ready: bool
    src1_ready: bool

@ac.system
def pipeline() -> None:
    issue = ac.table[4, Entry](init=0)
    hits = issue.match(lambda entry: entry.valid)
    issue.view(hits).patch(src0_ready=True, src1_ready=True)
    issue.view(0).patch(valid=False)
    snapshots = issue.view(0).read()
    ac.sink(snapshots)
"""

ALLOCATION_TABLE_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Entry:
    valid: bool
    ready: bool
    value: ac.u16

@ac.system
def pipeline() -> None:
    updates = ac.source(Entry)
    pending = ac.slot(updates)
    state = ac.table[4, Entry](init=0)
    state.view(0).patch(ready=True)
    state.view(0).allocate(
        enable=pending.valid, value=pending.value
    )
    pending.release(when=pending.valid)
    snapshots = state.view(0).read()
    ac.sink(snapshots)
"""

MEMORY_OWNED_SCOPE_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Request:
    address: ac.u8
    write: bool
    data: ac.u16

@ac.system
def pipeline() -> None:
    with ac.scope("owner"):
        sram = ac.memory(ac.u16, entries=16, init=0, latency=3)
        requests = ac.source(Request)
        responses = sram.request(
            requests,
            address=lambda item: item.address,
            write=lambda item: item.write,
            data=lambda item: item.data,
            result_field="data",
            depth=4,
        )
        ac.sink(responses)
"""

MEMORY_ARRAY_SOURCE = """
import agentic_circuit as ac

@ac.struct
class BankRequest:
    bank: ac.u2
    offset: ac.u4
    write: ac.u1
    data: ac.u16
    tag: ac.u8

@ac.system
def pipeline() -> None:
    requests = ac.source(BankRequest, depth=8, latency=1)
    with ac.scope("sram"):
        banks = ac.array(
            4,
            lambda _: ac.memory(ac.u16, entries=16, init=0, latency=2),
        )
        selected = banks.select(
            requests,
            key=lambda request: request.bank,
            depth=2,
            latency=1,
        )
        responses = selected.request(
            address=lambda request: request.offset,
            write=lambda request: request.write,
            data=lambda request: request.data,
            result_field="data",
            depth=2,
            merge_policy="priority",
            merge_depth=3,
            merge_latency=1,
        )
    ac.sink(responses)
"""

CREDIT_SOURCE = """
import agentic_circuit as ac

@ac.system
def pipeline() -> None:
    issued = ac.source(int)
    completed = issued.credit(
        cost=lambda item: item,
        credits=4,
        depth=4,
        latency=1,
    )
    ac.sink(completed)
"""

BARRIER_SOURCE = """
import agentic_circuit as ac

@ac.system
def pipeline() -> None:
    left = ac.source(int)
    right = ac.source(int)
    left_ready, right_ready = left.barrier(right, depth=2, latency=1)
    ac.sink(left_ready)
    ac.sink(right_ready)
"""

SELECT_SOURCE = """
import agentic_circuit as ac

@ac.struct
class Control:
    route: ac.u1

@ac.system
def pipeline() -> None:
    control = ac.source(Control)
    lanes = ac.array(2, lambda index: ac.source(int))
    selected = lanes.select(
        control,
        key=lambda item: item.route,
        depth=2,
        latency=1,
    )
    ac.sink(selected)
"""

FIRING_SOURCE = """
import agentic_circuit as ac

@ac.struct
class FiringItem:
    value: ac.u16

@ac.system
def pipeline() -> None:
    incoming = ac.source(FiringItem)
    outgoing = incoming.firing(
        lambda queue: queue.push(
            queue.pop().with_fields(value=queue.peek().value + 1)
        )
    )
    ac.sink(outgoing)
"""

LOOP_CONTROL_SOURCE = """
import agentic_circuit as ac

@ac.struct
class LoopItem:
    remaining: ac.u4
    stop: bool
    skip: bool

@ac.system
def pipeline() -> None:
    current = ac.source(LoopItem)
    while current.remaining > 0:
        if current.stop:
            break
        current = current.apply(
            lambda item: item.with_fields(remaining=item.remaining - 1)
        )
        if current.skip:
            continue
    ac.sink(current)
"""

RECURSION_SOURCE = """
import agentic_circuit as ac

def stages(queue, count):
    if count == 0:
        return queue
    return stages(
        queue.apply(lambda item: item + 1, depth=2, latency=1),
        count - 1,
    )

@ac.system
def pipeline() -> None:
    incoming = ac.source(int)
    outgoing = stages(incoming, 3)
    ac.sink(outgoing)
"""

EXPECT_SOURCE = """
import agentic_circuit as ac

@ac.struct
class ExpectedItem:
    value: ac.u16

@ac.system
def pipeline() -> None:
    incoming = ac.source(ExpectedItem)
    ac.expect(
        incoming,
        predicate=lambda item: item.value > 0,
        message="value must be positive",
    )
    ac.sink(incoming)
"""


class QueueFrontendTest(unittest.TestCase):
    def test_popcount_lowers_to_width_checked_var_operation(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(POPCOUNT_SOURCE, "pipeline")
        self.assertIn(
            "ac.var.popcount %v0 : !ac.var<i8> -> !ac.var<i4>",
            lowered,
        )

    def test_zero_counts_lower_to_one_parameterized_var_operation(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(ZERO_COUNT_SOURCE, "pipeline")
        self.assertIn(
            'ac.var.count_zeros %v0 direction "leading" : !ac.var<i13> -> !ac.var<i4>',
            lowered,
        )
        self.assertIn('direction "trailing" : !ac.var<i13> -> !ac.var<i4>', lowered)
        self.assertEqual(lowered.count("ac.var.count_zeros"), 2)

    def test_verification_expect_is_non_consuming_and_role_explicit(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        lowered = lower_queue_source(EXPECT_SOURCE, "pipeline")
        self.assertIn("ac.expect %incoming message", lowered)
        self.assertIn("ac.expect.yield", lowered)
        self.assertIn("ac.sink %incoming", lowered)
        with self.assertRaisesRegex(QueueFrontendError, "predicate must lower to bool"):
            lower_queue_source(
                EXPECT_SOURCE.replace("item.value > 0", "item.value"), "pipeline"
            )

    def test_compile_time_recursion_expands_to_frozen_queue_chain(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        lowered = lower_queue_source(RECURSION_SOURCE, "pipeline")
        self.assertEqual(3, lowered.count(" = ac.transform "))
        self.assertIn("%outgoing__rec0 = ac.transform %incoming", lowered)
        self.assertIn("%outgoing__rec1 = ac.transform %outgoing__rec0", lowered)
        self.assertIn("%outgoing = ac.transform %outgoing__rec1", lowered)
        self.assertEqual(lowered, lower_queue_source(RECURSION_SOURCE, "pipeline"))
        with self.assertRaisesRegex(QueueFrontendError, "recursion depth"):
            lower_queue_source(
                RECURSION_SOURCE.replace(
                    "stages(incoming, 3)", "stages(incoming, runtime)"
                ),
                "pipeline",
            )

    def test_bounded_loop_break_and_tail_continue_lower_to_feedback_edges(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(LOOP_CONTROL_SOURCE, "pipeline")
        self.assertIn("ac.feedback", lowered)
        self.assertIn('field "stop"', lowered)
        self.assertIn('field "skip"', lowered)
        self.assertIn('ac.var.cmp "eq"', lowered)
        self.assertIn("ac.var.mul", lowered)
        self.assertIn("ac.feedback.yield", lowered)

    def test_python_firing_surface_is_removed_at_epoch_0_5(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        with self.assertRaisesRegex(QueueFrontendError, "Queue.firing.*removed"):
            lower_queue_source(FIRING_SOURCE, "pipeline")

    def test_runtime_queue_collection_index_lowers_to_official_select(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(SELECT_SOURCE, "pipeline")
        self.assertIn(
            "%selected = ac.select %control, %lanes__0, %lanes__1 "
            "depth 2 latency 1 key",
            lowered,
        )
        self.assertIn('ac.var.get %item field "route"', lowered)
        self.assertIn("ac.select.yield", lowered)
        self.assertNotIn("dynamic", lowered)

    def test_barrier_lowers_multi_queue_atomic_synchronization(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        lowered = lower_queue_source(BARRIER_SOURCE, "pipeline")
        self.assertIn(
            "%left_ready, %right_ready = ac.barrier %left, %right "
            "depths [2, 2] latencies [1, 1]",
            lowered,
        )
        with self.assertRaisesRegex(QueueFrontendError, "inputs must be unique"):
            lower_queue_source(
                BARRIER_SOURCE.replace("left.barrier(right", "left.barrier(left"),
                "pipeline",
            )
        with self.assertRaisesRegex(QueueFrontendError, "matching input/output arity"):
            lower_queue_source(
                BARRIER_SOURCE.replace(
                    "left_ready, right_ready =", "left_ready, right_ready, extra ="
                ),
                "pipeline",
            )

    def test_credit_lowers_bounded_parallel_completion_window(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        lowered = lower_queue_source(CREDIT_SOURCE, "pipeline")
        self.assertIn(
            "%completed = ac.credit %issued credits 4 depth 4 latency 1 cost",
            lowered,
        )
        self.assertIn("ac.credit.yield %item : !ac.var<i64>", lowered)
        with self.assertRaisesRegex(QueueFrontendError, "credits must be positive"):
            lower_queue_source(
                CREDIT_SOURCE.replace("credits=4", "credits=0"), "pipeline"
            )

    def test_memory_lowers_old_data_request_response_contract(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        lowered = lower_queue_source(MEMORY_SOURCE, "pipeline")
        self.assertIn(
            "ac.memory.instance @sram data i16 entries 16 init 0 latency 3",
            lowered,
        )
        self.assertIn(
            "%responses = ac.memory.request @sram, %requests ordinal 0 "
            'result_field "data" depth 4 address',
            lowered,
        )
        self.assertIn("} write {", lowered)
        self.assertIn("} data {", lowered)
        self.assertEqual(3, lowered.count("ac.memory.yield"))
        with self.assertRaisesRegex(QueueFrontendError, "memory init must be zero"):
            lower_queue_source(MEMORY_SOURCE.replace("init=0", "init=1"), "pipeline")
        with self.assertRaisesRegex(QueueFrontendError, "latency must be positive"):
            lower_queue_source(
                MEMORY_SOURCE.replace("latency=3", "latency=0"), "pipeline"
            )
        with self.assertRaisesRegex(QueueFrontendError, "unsupported keyword"):
            lower_queue_source(
                MEMORY_SOURCE.replace("depth=4,", "depth=4,\n        latency=1,"),
                "pipeline",
            )

    def test_table_patch_and_two_read_modes_lower_to_frozen_primitives(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(TABLE_SOURCE, "pipeline")
        self.assertIn('ac.contract_epoch = "0.5"', lowered)
        self.assertIn(
            "ac.table @state entry !ac.struct<@types::@Entry> entries 16 init 0",
            lowered,
        )
        self.assertEqual(1, lowered.count("ac.table.write"))
        self.assertEqual(2, lowered.count("ac.table.read"))
        self.assertIn("ac.table.get @state", lowered)
        self.assertIn('field "valid"', lowered)
        self.assertIn('field "done"', lowered)
        self.assertIn('field "result"', lowered)
        self.assertIn('write_fields ["valid", "done", "result"]', lowered)
        self.assertNotIn("ac.table.patch", lowered)

    def test_table_hard_break_and_static_contracts_are_diagnosed(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        legacy = TABLE_SOURCE.replace(
            "state = ac.table[16, Entry](init=0)",
            "state = ac.table(updates, address=Update.index)",
        )
        with self.assertRaisesRegex(QueueFrontendError, "use ac.memory"):
            lower_queue_source(legacy, "pipeline")
        with self.assertRaisesRegex(
            QueueFrontendError, "table init must be exactly zero"
        ):
            lower_queue_source(
                TABLE_SOURCE.replace(
                    "ac.table[16, Entry](init=0)", "ac.table[16, Entry](init=1)"
                ),
                "pipeline",
            )
        with self.assertRaisesRegex(QueueFrontendError, "multiple endpoints"):
            lower_queue_source(
                TABLE_SOURCE.replace(
                    "    responses = state.view",
                    "    state.view(lambda update: update.index).write("
                    "updates, value=lambda update: update)\n"
                    "    responses = state.view",
                ),
                "pipeline",
            )

    def test_slot_match_choose_and_state_patch_lower(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        lowered = lower_queue_source(SLOT_TABLE_SOURCE, "pipeline")
        for operation in (
            "ac.slot @pending",
            "ac.slot.get @pending",
            "ac.table.match @issue",
            "ac.table.choose @issue",
            "ac.slot.release @pending",
        ):
            self.assertIn(operation, lowered)
        self.assertEqual(1, lowered.count("ac.table.match @issue"))
        self.assertEqual(1, lowered.count("ac.table.choose @issue"))
        choose_position = lowered.index("ac.table.choose @issue")
        write_position = lowered.index("ac.table.write @issue")
        self.assertLess(choose_position, write_position)
        self.assertIn("ac.table.yield %table_choose_", lowered)
        self.assertIn(
            'ac.table.write @issue mode "field" write_fields ["valid"] address',
            lowered,
        )
        first = SLOT_TABLE_SOURCE.replace(
            'ready, count=1, policy="min", key=lambda entry: entry.age',
            'ready, count=1, policy="first"',
        )
        first_lowered = lower_queue_source(first, "pipeline")
        self.assertIn('count 1 policy "first" key {}', first_lowered)
        self.assertEqual(1, first_lowered.count("ac.table.choose @issue"))
        self.assertIn(
            'count 1 policy "max"',
            lower_queue_source(
                SLOT_TABLE_SOURCE.replace('policy="min"', 'policy="max"'),
                "pipeline",
            ),
        )
        boundary = lower_queue_source(
            SLOT_TABLE_SOURCE.replace("ac.table[4, Entry]", "ac.table[64, Entry]"),
            "pipeline",
        )
        self.assertIn("-> !ac.var<i64>", boundary)
        with self.assertRaisesRegex(QueueFrontendError, "count=1 only"):
            lower_queue_source(
                SLOT_TABLE_SOURCE.replace("count=1", "count=2"), "pipeline"
            )
        with self.assertRaisesRegex(QueueFrontendError, "1..64 entries"):
            lower_queue_source(
                SLOT_TABLE_SOURCE.replace("ac.table[4, Entry]", "ac.table[65, Entry]"),
                "pipeline",
            )
        with self.assertRaisesRegex(QueueFrontendError, "requires key"):
            lower_queue_source(
                SLOT_TABLE_SOURCE.replace(", key=lambda entry: entry.age", ""),
                "pipeline",
            )
        with self.assertRaisesRegex(QueueFrontendError, "does not accept key"):
            lower_queue_source(
                SLOT_TABLE_SOURCE.replace('policy="min"', 'policy="first"'),
                "pipeline",
            )
        with self.assertRaisesRegex(QueueFrontendError, "different Table"):
            lower_queue_source(
                SLOT_TABLE_SOURCE.replace(
                    "grant = issue.choose(",
                    "other = ac.table[4, Entry](init=0)\n    grant = other.choose(",
                ),
                "pipeline",
            )

    def test_masked_table_write_and_entry_lambda_patch_lower(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        lowered = lower_queue_source(MASKED_TABLE_WRITE_SOURCE, "pipeline")
        self.assertIn("ac.table.match @issue", lowered)
        self.assertIn("ac.table.masked_write @issue", lowered)
        self.assertIn('write_fields ["valid", "tag", "age"]', lowered)
        self.assertIn("^value(%old: !ac.var<!ac.struct<@types::@Entry>>):", lowered)
        self.assertNotIn("ac.table.write @issue address", lowered)

        patched = MASKED_TABLE_WRITE_SOURCE.replace(
            "issue.view(hits).write(enable=pending.valid, value=pending.value)",
            "issue.view(hits).patch(\n"
            "        enable=pending.valid, valid=True,\n"
            "        age=lambda entry: entry.age + 1,\n"
            "    )",
        )
        patch_ir = lower_queue_source(patched, "pipeline")
        self.assertIn('field "valid"', patch_ir)
        self.assertIn('field "age"', patch_ir)
        self.assertIn("ac.var.get %old", patch_ir)

        with self.assertRaisesRegex(QueueFrontendError, "state-driven"):
            lower_queue_source(
                MASKED_TABLE_WRITE_SOURCE.replace(
                    "issue.view(hits).write(", "issue.view(hits).write(updates, "
                ),
                "pipeline",
            )
        with self.assertRaisesRegex(QueueFrontendError, "uniform expression"):
            lower_queue_source(
                MASKED_TABLE_WRITE_SOURCE.replace(
                    "value=pending.value", "value=lambda entry: entry"
                ),
                "pipeline",
            )
        with self.assertRaisesRegex(QueueFrontendError, "different Table"):
            lower_queue_source(
                MASKED_TABLE_WRITE_SOURCE.replace(
                    "issue = ac.table[4, Entry](init=0)",
                    "issue = ac.table[4, Entry](init=0)\n"
                    "    other = ac.table[4, Entry](init=0)",
                ).replace("issue.view(hits).write", "other.view(hits).write"),
                "pipeline",
            )
        with self.assertRaisesRegex(QueueFrontendError, "multiple endpoints"):
            lower_queue_source(
                patched.replace(
                    "pending.release(when=pending.valid)",
                    "issue.view(0).write(value=pending.value, enable=False)\n"
                    "    pending.release(when=pending.valid)",
                ),
                "pipeline",
            )

    def test_table_allows_disjoint_scalar_and_masked_writers(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        lowered = lower_queue_source(MULTI_WRITER_TABLE_SOURCE, "pipeline")
        self.assertIn(
            ': !ac.var<i4> mode "field" write_fields ["src0_ready", "src1_ready"]',
            lowered,
        )
        self.assertIn(
            'ac.table.write @issue mode "field" write_fields ["valid"] address',
            lowered,
        )
        with self.assertRaisesRegex(QueueFrontendError, "multiple endpoints"):
            lower_queue_source(
                MULTI_WRITER_TABLE_SOURCE.replace(
                    "issue.view(0).patch(valid=False)",
                    "issue.view(0).patch(src0_ready=False)",
                ),
                "pipeline",
            )
        with self.assertRaisesRegex(QueueFrontendError, "multiple endpoints"):
            lower_queue_source(
                MULTI_WRITER_TABLE_SOURCE.replace(
                    "issue.view(0).patch(valid=False)",
                    "issue.view(0).write(value=Entry(False, False, False))",
                ),
                "pipeline",
            )

    def test_scalar_allocation_lowers_as_unique_replace_writer(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        lowered = lower_queue_source(ALLOCATION_TABLE_SOURCE, "pipeline")
        self.assertIn(
            'ac.table.write @state mode "replace" '
            'write_fields ["valid", "ready", "value"]',
            lowered,
        )
        self.assertIn('ac.name = "state__allocate"', lowered)
        self.assertIn(
            'ac.table.write @state mode "field" write_fields ["ready"]',
            lowered,
        )
        with self.assertRaisesRegex(QueueFrontendError, "one allocation endpoint"):
            lower_queue_source(
                ALLOCATION_TABLE_SOURCE.replace(
                    "    snapshots = state.view(0).read()",
                    "    state.view(1).allocate(value=pending.value)\n"
                    "    snapshots = state.view(0).read()",
                ),
                "pipeline",
            )
        with self.assertRaisesRegex(QueueFrontendError, "requires one value"):
            lower_queue_source(
                ALLOCATION_TABLE_SOURCE.replace(
                    "enable=pending.valid, value=pending.value",
                    "enable=pending.valid",
                ),
                "pipeline",
            )
        with self.assertRaisesRegex(QueueFrontendError, "state-driven"):
            lower_queue_source(
                ALLOCATION_TABLE_SOURCE.replace(
                    "state.view(0).allocate(\n        enable=pending.valid, value=pending.value\n    )",
                    "state.view(lambda item: item.value).allocate(\n"
                    "        updates, enable=True, value=lambda item: item\n"
                    "    )",
                ),
                "pipeline",
            )
        masked = ALLOCATION_TABLE_SOURCE.replace(
            "    state.view(0).patch(ready=True)\n",
            "    free = state.match(lambda entry: not entry.valid)\n",
        ).replace("state.view(0).allocate(", "state.view(free).allocate(")
        with self.assertRaisesRegex(QueueFrontendError, "scalar view"):
            lower_queue_source(masked, "pipeline")

    def test_memory_instance_freezes_multiple_endpoint_priority(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        source = (
            MEMORY_SOURCE.replace(
                "requests = ac.source(Request)",
                "left = ac.source(Request)\n    right = ac.source(Request)",
            )
            .replace(
                "responses = sram.request(\n        requests,",
                "responses = sram.request(\n        left,",
            )
            .replace(
                "    ac.sink(responses)",
                "    other = sram.request(\n"
                "        right, address=lambda item: item.address,\n"
                "        write=lambda item: item.write, data=lambda item: item.data,\n"
                '        result_field="data", depth=2,\n'
                "    )\n"
                "    ac.sink(responses)\n    ac.sink(other)",
            )
        )
        lowered = lower_queue_source(source, "pipeline")
        self.assertEqual(1, lowered.count("ac.memory.instance"))
        self.assertEqual(2, lowered.count("ac.memory.request"))
        self.assertIn("@sram, %left ordinal 0", lowered)
        self.assertIn("@sram, %right ordinal 1", lowered)

    def test_memory_is_visible_in_its_declaration_scope(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(MEMORY_OWNED_SCOPE_SOURCE, "pipeline")
        self.assertIn(
            'ac.memory.instance @sram data i16 entries 16 init 0 latency 3 owner "/owner" '
            'stable_id "memory/owner/sram"',
            lowered,
        )
        self.assertIn("ac.scope @owner()", lowered)
        self.assertIn(
            'ac.endpoint_path = "/owner/responses", ac.name = "responses"',
            lowered,
        )

    def test_memory_array_select_statically_expands_existing_ops(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(MEMORY_ARRAY_SOURCE, "pipeline")
        self.assertEqual(4, lowered.count("ac.memory.instance"))
        self.assertEqual(4, lowered.count("ac.memory.request"))
        self.assertEqual(1, lowered.count("ac.route "))
        self.assertEqual(1, lowered.count("ac.merge "))
        for bank in range(4):
            self.assertIn(
                f"ac.memory.instance @banks__{bank} data i16 entries 16 init 0 latency 2 "
                f'owner "/sram" stable_id "memory/sram/banks__{bank}"',
                lowered,
            )
            self.assertIn(
                f"ac.memory.request @banks__{bank}, "
                f"%selected__bank{bank}_request__local ordinal 0",
                lowered,
            )
        self.assertIn("depths [2, 2, 2, 2] latencies [1, 1, 1, 1]", lowered)
        self.assertIn('ac.var.get %item field "bank"', lowered)
        self.assertIn(
            "%responses__local = ac.merge %responses__bank0__local, "
            "%responses__bank1__local, %responses__bank2__local, "
            '%responses__bank3__local policy "priority" depth 3 latency 1',
            lowered,
        )
        self.assertEqual(lowered, lower_queue_source(MEMORY_ARRAY_SOURCE, "pipeline"))

    def test_memory_array_select_rejects_invalid_elaboration(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        heterogeneous = MEMORY_ARRAY_SOURCE.replace("entries=16", "entries=_ + 1")
        with self.assertRaisesRegex(QueueFrontendError, "must be homogeneous"):
            lower_queue_source(heterogeneous, "pipeline")

        noninteger_key = MEMORY_ARRAY_SOURCE.replace(
            "key=lambda request: request.bank", "key=lambda request: request"
        )
        with self.assertRaisesRegex(QueueFrontendError, "route key must lower"):
            lower_queue_source(noninteger_key, "pipeline")

        request_start = MEMORY_ARRAY_SOURCE.index(
            "        responses = selected.request("
        )
        unused = MEMORY_ARRAY_SOURCE[:request_start] + "    ac.sink(requests)\n"
        with self.assertRaisesRegex(
            QueueFrontendError, "selected memory is not requested"
        ):
            lower_queue_source(unused, "pipeline")

        wrong_scope = MEMORY_ARRAY_SOURCE.replace(
            "        responses = selected.request(",
            "    responses = selected.request(",
        )
        with self.assertRaisesRegex(QueueFrontendError, "same lexical scope"):
            lower_queue_source(wrong_scope, "pipeline")

    def test_memory_rejects_legacy_unconnected_type_and_scope_forms(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        legacy = MEMORY_SOURCE.replace(
            "sram = ac.memory(ac.u16, entries=16, init=0, latency=3)\n    ", ""
        ).replace("sram.request(\n        requests,", "requests.memory(")
        with self.assertRaisesRegex(QueueFrontendError, "Queue.memory was removed"):
            lower_queue_source(legacy, "pipeline")
        unconnected = MEMORY_SOURCE.replace(
            "responses = sram.request(",
            "other = ac.memory(ac.u16)\n    responses = sram.request(",
        )
        with self.assertRaisesRegex(QueueFrontendError, "is not connected"):
            lower_queue_source(unconnected, "pipeline")
        mismatch = MEMORY_SOURCE.replace("ac.memory(ac.u16", "ac.memory(ac.u8")
        with self.assertRaisesRegex(QueueFrontendError, "must match instance"):
            lower_queue_source(mismatch, "pipeline")
        illegal_scope = MEMORY_SOURCE.replace(
            "sram = ac.memory(ac.u16, entries=16, init=0, latency=3)",
            'with ac.scope("owner"):\n        sram = ac.memory(ac.u16, entries=16, init=0, latency=3)',
        )
        with self.assertRaisesRegex(QueueFrontendError, "declaration scope"):
            lower_queue_source(illegal_scope, "pipeline")
        rebound = MEMORY_SOURCE.replace(
            "requests = ac.source(Request)",
            "sram = ac.source(Request)\n    requests = ac.source(Request)",
        )
        with self.assertRaisesRegex(QueueFrontendError, "cannot be rebound"):
            lower_queue_source(rebound, "pipeline")

    def test_dependency_lowers_four_pure_policies(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        lowered = lower_queue_source(DEPENDENCY_SOURCE, "pipeline")
        self.assertIn(
            "%completed = ac.dependency %issued capacity 16 resources 4 "
            "no_dependency 255 depth 8 latency 1 key",
            lowered,
        )
        self.assertIn("} waits_for {", lowered)
        self.assertIn("} resource {", lowered)
        self.assertIn("} cost {", lowered)
        self.assertEqual(4, lowered.count("ac.dependency.yield"))
        with self.assertRaisesRegex(QueueFrontendError, "requires one cost lambda"):
            lower_queue_source(
                DEPENDENCY_SOURCE.replace("cost=lambda item: item.cycles,", ""),
                "pipeline",
            )

    def test_reorder_lowers_sequence_key_and_static_capacity(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        lowered = lower_queue_source(REORDER_SOURCE, "pipeline")
        self.assertIn(
            "%retired = ac.reorder %completed capacity 16 start 0 depth 4 latency 1",
            lowered,
        )
        self.assertIn('ac.var.get %item field "sequence"', lowered)
        self.assertIn("ac.reorder.yield", lowered)
        with self.assertRaisesRegex(QueueFrontendError, "capacity must be positive"):
            lower_queue_source(
                REORDER_SOURCE.replace("capacity=16", "capacity=0"), "pipeline"
            )

    def test_simple_serial_python_lowers_to_typed_queue_graph(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        self.assertEqual(
            """module attributes {ac.contract_epoch = "0.5", ac.model_kind = "queue_graph", ac.queue_graph_domain = "cycle", ac.system = "pipeline"} {
  %input_queue = ac.source depth 4 latency 1 {ac.name = "input_queue"} : !ac.queue<i64>
  %output_queue = ac.transform %input_queue depths [8] latencies [2] {
  ^transform(%item: !ac.var<i64>):
    ac.transform.yield %item : !ac.var<i64>
  } {ac.name = "output_queue"} : (!ac.queue<i64>) -> !ac.queue<i64>
  ac.sink %output_queue {ac.name = "sink_2"} : !ac.queue<i64>
}
""",
            lower_queue_source(SOURCE, "pipeline"),
        )

    def test_repeated_lowering_is_byte_identical(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        self.assertEqual(
            lower_queue_source(SOURCE, "pipeline"),
            lower_queue_source(SOURCE, "pipeline"),
        )

    def test_struct_and_immutable_lambda_lower_to_var_operations(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(STRUCT_SOURCE, "pipeline")
        self.assertIn("ac.type_scope @types", lowered)
        self.assertIn("ac.struct @WorkItem", lowered)
        self.assertIn("!ac.queue<!ac.struct<@types::@WorkItem>>", lowered)
        self.assertIn('ac.var.get %item field "value"', lowered)
        self.assertIn("ac.var.constant 1 : i64", lowered)
        self.assertIn("ac.var.add", lowered)
        self.assertIn("ac.var.mul", lowered)
        self.assertIn("ac.var.sub", lowered)
        self.assertIn("ac.var.with", lowered)
        self.assertIn('field "remaining"', lowered)

    def test_nested_scope_infers_borrowed_local_and_exported_queues(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(SCOPE_SOURCE, "pipeline")
        self.assertIn("%completed = ac.scope @frontend(%input_queue)", lowered)
        self.assertIn("^body(%input_queue__in: !ac.queue<i64>):", lowered)
        self.assertIn(
            "%completed__inner = ac.scope @inner(%adjusted__local)",
            lowered,
        )
        self.assertIn("ac.scope.yield %completed__local", lowered)
        self.assertIn(
            'ac.sink %completed {ac.name = "sink_5"} : !ac.queue<i64>', lowered
        )

    def test_multiple_consumers_insert_strict_atomic_broadcast(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(BROADCAST_SOURCE, "pipeline")
        self.assertIn(
            "%input_queue__fanout0, %input_queue__fanout1 = ac.broadcast "
            "%input_queue depths [1, 1] latencies [1, 1]",
            lowered,
        )
        self.assertIn("ac.transform %input_queue__fanout0", lowered)
        self.assertIn("ac.transform %input_queue__fanout1", lowered)

    def test_cross_scope_broadcast_is_placed_at_lexical_lca(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(CROSS_SCOPE_BROADCAST_SOURCE, "pipeline")
        broadcast = lowered.index("ac.broadcast %input_queue")
        left_scope = lowered.index("ac.scope @left(%input_queue__fanout0)")
        right_scope = lowered.index("ac.scope @right(%input_queue__fanout1)")
        self.assertLess(broadcast, left_scope)
        self.assertLess(broadcast, right_scope)
        self.assertIn("^body(%input_queue__fanout0__in: !ac.queue<i64>):", lowered)

    def test_tuple_route_lowers_selector_to_var_region(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(ROUTE_SOURCE, "pipeline")
        self.assertIn("%left, %right = ac.route %input_queue", lowered)
        self.assertIn("depths [2, 2] latencies [1, 1]", lowered)
        self.assertIn('ac.var.get %item field "route"', lowered)
        self.assertIn("ac.route.yield", lowered)
        self.assertIn('%merged = ac.merge %left, %right policy "round_robin"', lowered)
        self.assertIn("depth 3 latency 1", lowered)
        self.assertIn("ac.sink %merged", lowered)

    def test_static_queue_collections_flatten_in_canonical_order(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(COLLECTION_SOURCE, "pipeline")
        self.assertIn("%lanes__0 = ac.source depth 1", lowered)
        self.assertIn("%lanes__1 = ac.source depth 2", lowered)
        self.assertLess(
            lowered.index("ac.sink %lanes__0"),
            lowered.index("ac.sink %lanes__1"),
        )
        self.assertNotIn("dynamic", lowered)
        reordered = COLLECTION_SOURCE.replace(
            '{named["right"], named["left"]}',
            '{named["left"], named["right"]}',
        )
        self.assertEqual(lowered, lower_queue_source(reordered, "pipeline"))

    def test_dynamic_or_duplicate_collection_shape_is_rejected(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        with self.assertRaisesRegex(QueueFrontendError, "positive compile-time extent"):
            lower_queue_source(
                COLLECTION_SOURCE.replace("ac.array(2", "ac.array(runtime"),
                "pipeline",
            )
        with self.assertRaisesRegex(QueueFrontendError, "members must be unique"):
            lower_queue_source(
                COLLECTION_SOURCE.replace(
                    '{named["right"], named["left"]}',
                    '{named["left"], named["left"]}',
                ),
                "pipeline",
            )

    def test_nested_collection_shape_is_statically_flattened(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(NESTED_COLLECTION_SOURCE, "pipeline")
        self.assertIn("%grid__0__0 = ac.source depth 1", lowered)
        self.assertIn("%grid__0__1 = ac.source depth 2", lowered)
        self.assertIn("%grid__1__0 = ac.source depth 2", lowered)
        self.assertIn("%grid__1__1 = ac.source depth 3", lowered)
        self.assertIn("ac.sink %grid__1__0", lowered)

    def test_serial_while_lowers_to_bounded_feedback(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(FEEDBACK_SOURCE, "pipeline")
        self.assertIn("ac.feedback %current depth 2 latency 1", lowered)
        self.assertIn("max_iterations 1024", lowered)
        self.assertIn('ac.var.cmp "ugt"', lowered)
        self.assertIn("ac.feedback.yield", lowered)
        self.assertIn("ac.sink %current__feedback0", lowered)

    def test_observation_only_use_does_not_insert_broadcast(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(OBSERVE_SOURCE, "pipeline")
        self.assertIn('ac.observe %input_queue name "observe_1"', lowered)
        self.assertIn("ac.sink %input_queue", lowered)
        self.assertNotIn("ac.broadcast", lowered)

    def test_explicit_atomic_surface_is_removed_at_epoch_0_5(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        with self.assertRaisesRegex(QueueFrontendError, "ac.atomic.*removed"):
            lower_queue_source(ATOMIC_SOURCE, "pipeline")

    def test_static_if_and_range_are_fully_expanded(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        lowered = lower_queue_source(
            STATIC_CONTROL_SOURCE.replace("range(2)", "range(1 + 1)"),
            "pipeline",
        )
        self.assertIn('ac.name = "selected"', lowered)
        self.assertNotIn("unreachable", lowered)
        self.assertIn("ac.sink %lanes__0", lowered)
        self.assertIn("ac.sink %lanes__1", lowered)
        with self.assertRaisesRegex(QueueFrontendError, "one result name"):
            lower_queue_source(
                STATIC_CONTROL_SOURCE.replace("if True:", "if input_queue:"),
                "pipeline",
            )

    def test_static_range_expansion_uses_the_shared_deterministic_cap(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        template = """
import agentic_circuit as ac

@ac.system
def pipeline() -> None:
    incoming = ac.source(int)
    for index in range(EXTENT):
        if False:
            ac.sink(incoming)
    ac.sink(incoming)
"""
        accepted = lower_queue_source(template.replace("EXTENT", "10000"), "pipeline")
        self.assertEqual(1, accepted.count("ac.sink %incoming"))
        with self.assertRaisesRegex(QueueFrontendError, r"\[0, 10000\]"):
            lower_queue_source(template.replace("EXTENT", "10001"), "pipeline")

    def test_user_opcode_definition_is_rejected(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        illegal = """
import agentic_circuit as ac

@ac.opcode
def private_block():
    pass

@ac.system
def pipeline() -> None:
    queue = ac.source(int)
    ac.sink(queue)
"""
        with self.assertRaisesRegex(QueueFrontendError, "providers are forbidden"):
            lower_queue_source(illegal, "pipeline")

    def test_static_map_accepts_canonical_integer_keys(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(CONST_KEY_MAP_SOURCE, "pipeline")
        self.assertLess(lowered.index("ac.sink %one"), lowered.index("ac.sink %two"))

    def test_explicit_integer_widths_freeze_payload_layout(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(WIDTH_SOURCE, "pipeline")
        self.assertIn('{name = "value", type = i32}', lowered)
        self.assertIn('{name = "route", type = i2}', lowered)
        self.assertIn('{name = "remaining", type = i16}', lowered)
        self.assertIn('{name = "valid", type = i1}', lowered)
        self.assertIn("size = 12 : i64", lowered)

    def test_u1_through_u64_fields_lower_to_exact_width_bit_operations(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(BIT_WIDTH_SOURCE, "pipeline")
        for field, width in (
            ("left", 3),
            ("tag", 5),
            ("payload", 17),
            ("sequence", 63),
        ):
            self.assertIn(f'{{name = "{field}", type = i{width}}}', lowered)
        for operation in ("and", "or", "xor", "not", "shl", "shr"):
            self.assertIn(f"ac.var.{operation}", lowered)
        self.assertEqual(lowered.count("ac.var.priority_encode"), 1)
        self.assertIn('order "low"', lowered)
        self.assertIn("ac.var.constant 1 : i3 as !ac.var<i3>", lowered)
        self.assertIn("size = 24 : i64", lowered)

    def test_payload_parser_retains_recursive_type_descriptors_before_mlir(
        self,
    ) -> None:
        from _pycircuit_semantics import BitsType, BoolType, StructType
        from agentic_circuit._queue_frontend import parse_queue_program

        program = parse_queue_program(BIT_WIDTH_SOURCE, "pipeline")
        payload = next(item for item in program.payloads if item.name == "BitBundle")

        self.assertIsInstance(payload.descriptor, StructType)
        self.assertIsInstance(payload.descriptor.field("left").type, BitsType)
        self.assertEqual(3, payload.descriptor.field("left").type.bit_width())
        self.assertIsInstance(payload.descriptor.field("priority_valid").type, BitsType)
        self.assertEqual(payload.acir_type, "!ac.struct<@types::@BitBundle>")

        rule_program = parse_queue_program(RULE_ROB_SOURCE, "rob")
        entry = next(item for item in rule_program.payloads if item.name == "Entry")
        self.assertIsInstance(entry.descriptor.field("done").type, BoolType)

    def test_queue_program_semantic_types_are_scope_independent_descriptors(
        self,
    ) -> None:
        from _pycircuit_semantics import BitsType, BoolType, StructType, ValueType
        from agentic_circuit._queue_frontend import parse_queue_program

        first = parse_queue_program(BIT_WIDTH_SOURCE, "pipeline")
        second = parse_queue_program(BIT_WIDTH_SOURCE, "pipeline")
        first_payload = next(
            item.descriptor for item in first.payloads if item.name == "BitBundle"
        )
        second_payload = next(
            item.descriptor for item in second.payloads if item.name == "BitBundle"
        )
        self.assertIsInstance(first_payload, StructType)
        self.assertEqual(first_payload, second_payload)
        self.assertIsNot(first_payload, second_payload)
        self.assertEqual(first_payload.fingerprint, second_payload.fingerprint)
        self.assertTrue(
            all(isinstance(queue.payload, ValueType) for queue in first.queues)
        )
        self.assertEqual(first_payload, first.queues[0].payload)

        logical = BoolType()
        bit = BitsType(1)
        self.assertNotEqual(logical, bit)
        self.assertEqual("i1", logical.mlir())
        self.assertEqual("i1", bit.mlir())

        memory = parse_queue_program(MEMORY_SOURCE, "pipeline")
        self.assertIsInstance(memory.memory_instances[0].data_type, BitsType)
        table = parse_queue_program(TABLE_SOURCE, "pipeline")
        self.assertIsInstance(table.tables[0].entry_type, StructType)
        slot = parse_queue_program(SLOT_TABLE_SOURCE, "pipeline")
        self.assertIsInstance(slot.slots[0].payload, StructType)
        state = parse_queue_program(VARIABLE_RULE_SOURCE, "accumulator")
        self.assertIsInstance(state.variables[0].value_type, BitsType)
        self.assertTrue(
            all(
                isinstance(owner.value_type, ValueType)
                for queue in state.queues
                for owner in queue.rule_state_owners
            )
        )

    def test_bool_and_u1_identity_survives_expression_lowering(self) -> None:
        from _pycircuit_semantics import BitsType, BoolType
        from agentic_circuit._queue_frontend import (
            lower_queue_source,
            parse_queue_program,
        )

        program = parse_queue_program(BOOL_U1_SOURCE, "bool_u1_pipeline")
        descriptor = program.payloads[0].descriptor
        logical = descriptor.field("logical").type
        bit = descriptor.field("bit").type
        self.assertIsInstance(logical, BoolType)
        self.assertIsInstance(bit, BitsType)
        self.assertNotEqual(logical, bit)
        self.assertEqual("i1", logical.mlir())
        self.assertEqual("i1", bit.mlir())

        lowered = lower_queue_source(BOOL_U1_SOURCE, "bool_u1_pipeline")
        self.assertEqual(1, lowered.count("ac.var.not"))
        self.assertEqual(1, lowered.count('ac.var.cmp "eq"'))

    def test_epoch_05_bool_u1_compatibility_remains_backend_consistent(self) -> None:
        from agentic_circuit._queue_codegen import lower_queue_program_to_cpp
        from agentic_circuit._queue_frontend import (
            lower_queue_source,
            parse_queue_program,
        )

        program = parse_queue_program(BOOL_U1_COMPARISON_SOURCE, "bool_u1_compare")
        lowered = lower_queue_source(BOOL_U1_COMPARISON_SOURCE, "bool_u1_compare")
        generated = lower_queue_program_to_cpp(program)
        self.assertIn('ac.var.cmp "eq"', lowered)
        self.assertIn("item.bit == item.logical", generated)

        modules = lower_queue_source(BOOL_U1_MODULE_SOURCE, "bool_u1_modules")
        self.assertIn("ac.module @bit_identity", modules)
        self.assertIn("ac.module @bit_state", modules)
        self.assertEqual(2, modules.count("ac.instance"))
        self.assertIn("ac.var.assign @saved", modules)

    def test_nested_struct_fields_lower_through_recursive_descriptors(self) -> None:
        from _pycircuit_semantics import BitsType, StructType
        from agentic_circuit._queue_frontend import (
            lower_queue_source,
            parse_queue_program,
        )

        program = parse_queue_program(NESTED_STRUCT_SOURCE, "nested_struct_pipeline")
        packet = next(item for item in program.payloads if item.name == "Packet")
        header = packet.descriptor.field("header").type

        self.assertIsInstance(header, StructType)
        self.assertIsInstance(header.field("mode").type, BitsType)
        self.assertEqual(26, packet.descriptor.bit_width())

        lowered = lower_queue_source(NESTED_STRUCT_SOURCE, "nested_struct_pipeline")
        self.assertIn('{name = "header", type = !ac.struct<@types::@Header>}', lowered)
        self.assertIn('ac.var.get %item field "header"', lowered)
        self.assertIn('field "mode"', lowered)
        self.assertIn("!ac.var<!ac.struct<@types::@Header>>", lowered)

    def test_recursive_struct_cycle_fails_before_acir(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        source = """
from __future__ import annotations
import agentic_circuit as ac

@ac.struct
class Left:
    right: Right

@ac.struct
class Right:
    left: Left

@ac.system
def cycle(incoming: Left) -> Left:
    return incoming
"""
        with self.assertRaisesRegex(QueueFrontendError, "recursive struct cycle"):
            lower_queue_source(source, "cycle")

    def test_standard_python_enum_lowers_as_nominal_nested_value(self) -> None:
        from _pycircuit_semantics import EnumType
        from agentic_circuit._queue_frontend import (
            lower_queue_source,
            parse_queue_program,
        )

        program = parse_queue_program(ENUM_PAYLOAD_SOURCE, "enum_payload_pipeline")
        mode = next(item for item in program.enums if item.name == "Mode")
        packet = next(item for item in program.payloads if item.name == "Packet")
        header = packet.descriptor.field("header").type

        self.assertIsInstance(mode.descriptor, EnumType)
        self.assertEqual(("IDLE", "RUN", "WAIT"), mode.descriptor.enumerants)
        self.assertEqual(mode.descriptor, header.field("mode").type)

        lowered = lower_queue_source(ENUM_PAYLOAD_SOURCE, "enum_payload_pipeline")
        self.assertIn("ac.enum @Mode", lowered)
        self.assertIn('enumerants ["IDLE", "RUN", "WAIT"]', lowered)
        self.assertIn('ac.var.enum @types::@Mode "RUN"', lowered)
        self.assertIn('ac.var.enum @types::@Mode "WAIT"', lowered)
        self.assertIn('ac.var.cmp "eq"', lowered)
        self.assertIn("!ac.var<!ac.enum<@types::@Mode>>", lowered)

    def test_enum_values_require_contiguous_encoding_and_equality_comparison(
        self,
    ) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        sparse = ENUM_PAYLOAD_SOURCE.replace("WAIT = 2", "WAIT = 3")
        with self.assertRaisesRegex(QueueFrontendError, "contiguous from zero"):
            lower_queue_source(sparse, "enum_payload_pipeline")

        ordered = ENUM_PAYLOAD_SOURCE.replace(
            "item.header.mode == Mode.WAIT", "item.header.mode < Mode.WAIT"
        )
        with self.assertRaisesRegex(QueueFrontendError, "only equality"):
            lower_queue_source(ordered, "enum_payload_pipeline")

    def test_tuple_and_value_array_lower_as_structural_aggregate_values(self) -> None:
        from _pycircuit_semantics import ArrayType, TupleType
        from agentic_circuit._queue_frontend import (
            lower_queue_source,
            parse_queue_program,
        )

        program = parse_queue_program(
            AGGREGATE_PAYLOAD_SOURCE, "aggregate_payload_pipeline"
        )
        packet = next(
            item for item in program.payloads if item.name == "AggregatePacket"
        )
        self.assertIsInstance(packet.descriptor.field("pair").type, TupleType)
        self.assertIsInstance(packet.descriptor.field("lanes").type, ArrayType)
        self.assertEqual(28, packet.descriptor.bit_width())

        lowered = lower_queue_source(
            AGGREGATE_PAYLOAD_SOURCE, "aggregate_payload_pipeline"
        )
        self.assertIn("type = tuple<i3, i5>", lowered)
        self.assertIn("type = !ac.value_array<4 x i4>", lowered)
        self.assertEqual(7, lowered.count("ac.var.element"))
        self.assertEqual(1, lowered.count("ac.var.tuple"))
        self.assertEqual(1, lowered.count("ac.var.array"))

    def test_tuple_and_value_array_require_static_exact_shapes(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        short = AGGREGATE_PAYLOAD_SOURCE.replace(
            "item.lanes[3], item.lanes[0]", "item.lanes[3]"
        )
        with self.assertRaisesRegex(QueueFrontendError, "literal arity"):
            lower_queue_source(short, "aggregate_payload_pipeline")

        out_of_range = AGGREGATE_PAYLOAD_SOURCE.replace(
            "selected=item.lanes[2]", "selected=item.lanes[4]"
        )
        with self.assertRaisesRegex(QueueFrontendError, "index is out of range"):
            lower_queue_source(out_of_range, "aggregate_payload_pipeline")

        dynamic = AGGREGATE_PAYLOAD_SOURCE.replace(
            "ac.array[4, ac.bits[4]]", "ac.array[WIDTH, ac.bits[4]]"
        )
        with self.assertRaisesRegex(QueueFrontendError, "static"):
            lower_queue_source(dynamic, "aggregate_payload_pipeline")

        overflowing = AGGREGATE_PAYLOAD_SOURCE.replace(
            "ac.array[4, ac.bits[4]]",
            "ac.array[2305843009213693953, ac.bits[8]]",
        )
        with self.assertRaisesRegex(QueueFrontendError, r"width must be in \[1, 64\]"):
            lower_queue_source(overflowing, "aggregate_payload_pipeline")

    def test_constraint_proven_shapes_render_concrete_acir_attributes(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        source = (
            AGGREGATE_PAYLOAD_SOURCE.replace(
                "tuple[ac.bits[3], ac.bits[5]]",
                "tuple[ac.bits[1 + 2], ac.bits[2 + 3]]",
            )
            .replace("ac.array[4, ac.bits[4]]", "ac.array[2 + 2, ac.bits[2 + 2]]")
            .replace("selected=item.lanes[2]", "selected=item.lanes[1 + 1]")
        )
        lowered = lower_queue_source(source, "aggregate_payload_pipeline")

        self.assertIn("type = tuple<i3, i5>", lowered)
        self.assertIn("type = !ac.value_array<4 x i4>", lowered)
        self.assertIn("ac.var.element %v", lowered)
        self.assertIn(" at 2 : !ac.var<!ac.value_array<4 x i4>>", lowered)

    def test_bit_operations_require_identical_widths(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        mismatched = BIT_WIDTH_SOURCE.replace("right: ac.u3", "right: ac.u5", 1)
        with self.assertRaisesRegex(QueueFrontendError, "operands must match"):
            lower_queue_source(mismatched, "pipeline")

    def test_exact_width_bit_value_can_be_a_scalar_queue_payload(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(SCALAR_BIT_SOURCE, "pipeline")
        self.assertIn("!ac.queue<i7>", lowered)
        self.assertIn("ac.var.constant 1 : i7 as !ac.var<i7>", lowered)
        self.assertIn("ac.var.constant 3 : i7 as !ac.var<i7>", lowered)
        self.assertIn("ac.var.constant 1 : i1 as !ac.var<i1>", lowered)
        self.assertIn("ac.var.add", lowered)
        self.assertIn("ac.var.xor", lowered)

    def test_bit_widths_outside_u1_through_u64_are_rejected(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        for invalid in ("u0", "u65"):
            source = BIT_WIDTH_SOURCE.replace("ac.u3", f"ac.{invalid}", 1)
            with self.subTest(invalid=invalid):
                with self.assertRaisesRegex(QueueFrontendError, r"\[1, 64\]"):
                    lower_queue_source(source, "pipeline")

    def test_static_bits_extract_concat_and_insert_lower_exact_widths(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        source = BIT_OPERATION_SOURCE.replace(
            "item.value[0:5]", "item.value[0 + 0:2 + 3]"
        ).replace("lsb=9", "lsb=4 + 5")
        lowered = lower_queue_source(source, "bits_pipeline")
        self.assertIn("ac.var.extract", lowered)
        self.assertIn("from 0 width 5", lowered)
        self.assertIn("ac.var.concat", lowered)
        self.assertIn("-> !ac.var<i8>", lowered)
        self.assertIn("ac.var.insert", lowered)
        self.assertIn("at 9", lowered)

    def test_static_bits_reject_dynamic_or_out_of_range_operations(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        dynamic = BIT_OPERATION_SOURCE.replace("item.value[0:5]", "item.value[0:]")
        with self.assertRaisesRegex(QueueFrontendError, "static integers"):
            lower_queue_source(dynamic, "bits_pipeline")

        out_of_range = BIT_OPERATION_SOURCE.replace("lsb=9", "lsb=16")
        with self.assertRaisesRegex(QueueFrontendError, "out of range"):
            lower_queue_source(out_of_range, "bits_pipeline")

    def test_masked_match_emits_exact_canonical_unsigned_attributes(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(MASKED_MATCH_SOURCE, "masked_decode_pipeline")
        self.assertIn(
            "ac.var.matches %v0 mask 13 value 9 : !ac.var<i4> -> !ac.var<i1>",
            lowered,
        )

    def test_masked_match_rejects_nonstatic_malformed_or_mistyped_patterns(
        self,
    ) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        invalid_sources = (
            (
                MASKED_MATCH_SOURCE.replace('"10x1"', '"10" + "x1"'),
                "compile-time str",
            ),
            (MASKED_MATCH_SOURCE.replace('"10x1"', "item.opcode"), "compile-time str"),
            (MASKED_MATCH_SOURCE.replace('"10x1"', '"10?1"'), "invalid"),
            (MASKED_MATCH_SOURCE.replace('"10x1"', '"10x"'), "width 3"),
            (
                MASKED_MATCH_SOURCE.replace(
                    'ac.matches(item.opcode, "10x1")',
                    'ac.matches(item.enabled, "x")',
                ),
                "requires a bits value",
            ),
            (
                MASKED_MATCH_SOURCE.replace(
                    'ac.matches(item.opcode, "10x1")',
                    'ac.matches(item.opcode, pattern="10x1")',
                ),
                "two positional arguments",
            ),
        )
        for source, diagnostic in invalid_sources:
            with self.subTest(diagnostic=diagnostic):
                with self.assertRaisesRegex(QueueFrontendError, diagnostic):
                    lower_queue_source(source, "masked_decode_pipeline")

        for extended in ("10X1", "10-1", "10 1", "10_1", "1(0)x"):
            with self.subTest(extended=extended):
                source = MASKED_MATCH_SOURCE.replace('"10x1"', repr(extended))
                with self.assertRaisesRegex(QueueFrontendError, "invalid"):
                    lower_queue_source(source, "masked_decode_pipeline")

    def test_bitfield_views_and_updates_lower_to_existing_bit_operations(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(BITFIELD_SOURCE, "bitfield_pipeline")

        self.assertIn("ac.bitfield @INSTR width 32", lowered)
        self.assertRegex(lowered, r'fingerprint "sha256:[0-9a-f]{64}"')
        self.assertIn("from 26 width 6", lowered)
        self.assertIn("from 21 width 5", lowered)
        self.assertIn("from 4 width 17", lowered)
        self.assertIn('ac.bitfield_fields = ["opcode", "rd"]', lowered)
        self.assertIn("at 1", lowered)
        self.assertIn("at 21", lowered)
        self.assertNotIn("ac.bitfield.read", lowered)
        self.assertNotIn("ac.bitfield.update", lowered)

    def test_bitfield_declaration_overlap_is_legal_but_update_overlap_rejects(
        self,
    ) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        invalid = BITFIELD_SOURCE.replace(
            "mode=item.mode, rd=item.rd",
            "rd=item.rd, low25=item.low25",
        )
        with self.assertRaisesRegex(QueueFrontendError, "overlap"):
            lower_queue_source(invalid, "bitfield_pipeline")

    def test_bitfield_schema_and_field_use_fail_closed(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        dynamic = BITFIELD_SOURCE.replace("width=32", "width=WIDTH", 1)
        with self.assertRaisesRegex(QueueFrontendError, "static literals"):
            lower_queue_source(dynamic, "bitfield_pipeline")

        unknown = BITFIELD_SOURCE.replace(
            "INSTR(item.word).opcode", "INSTR(item.word).missing", 1
        )
        with self.assertRaisesRegex(QueueFrontendError, "unknown bitfield"):
            lower_queue_source(unknown, "bitfield_pipeline")

        mismatched = BITFIELD_SOURCE.replace("word: ac.bits[32]", "word: ac.bits[31]")
        with self.assertRaisesRegex(QueueFrontendError, "does not match"):
            lower_queue_source(mismatched, "bitfield_pipeline")

    def test_rule_frontend_emits_typed_markers_before_mlir_lowering(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(RULE_ROB_SOURCE, "rob")
        self.assertIn("%completed = ac.rule %issued depths [1] latencies [1]", lowered)
        self.assertIn('name "complete"', lowered)
        self.assertIn('stable_id "completed" domain "cycle"', lowered)
        self.assertIn('stable_id "completed" domain "cycle" type exact {', lowered)
        self.assertNotIn("input_fact", lowered)
        self.assertNotIn("committed_input", lowered)
        self.assertIn(
            "ac.marker.obligation %v1 state pending resolver handshake",
            lowered,
        )
        self.assertIn("ac.rule.return", lowered)
        self.assertIn("ac.reorder %completed", lowered)
        self.assertNotIn("ac.firing", lowered)
        self.assertNotIn("ac.queue.peek", lowered)
        self.assertNotIn("ac.queue.pop", lowered)
        self.assertNotIn("ac.queue.push", lowered)

    def test_rule_frontend_rejects_unsupported_control_flow(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        invalid = RULE_ROB_SOURCE.replace(
            "    return entry.with_fields(done=True)",
            "    if entry.done:\n        return entry\n    return entry.with_fields(done=True)",
        )
        with self.assertRaisesRegex(QueueFrontendError, "one value-returning path"):
            lower_queue_source(invalid, "rob")

    def test_reused_rule_definition_gets_unique_stable_instance_ids(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(RULE_PAIR_SOURCE, "pair")
        self.assertEqual(2, lowered.count('name "increment" stable_id'))
        self.assertIn('stable_id "left_next"', lowered)
        self.assertIn('stable_id "right_next"', lowered)

    def test_multi_input_rule_keeps_queue_atomicity_below_python_surface(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(MULTI_INPUT_RULE_SOURCE, "pair")
        self.assertIn("%summed = ac.rule %left, %right", lowered)
        self.assertIn(
            "^rule(%item0: !ac.var<i64>, %item1: !ac.var<i64>):",
            lowered,
        )
        self.assertIn("ac.var.add %item0, %item1", lowered)
        self.assertNotIn(".pop", lowered)
        self.assertNotIn(".push", lowered)
        self.assertNotIn("ready_valid", lowered)
        self.assertNotIn("atomic", lowered)

    def test_system_parameters_and_return_infer_boundaries(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(INFERRED_BOUNDARY_RULE_SOURCE, "pipeline")
        self.assertIn("%value = ac.source", lowered)
        self.assertIn("%result = ac.rule %value", lowered)
        self.assertIn("ac.sink %result", lowered)
        self.assertNotIn("source(", INFERRED_BOUNDARY_RULE_SOURCE)
        self.assertNotIn("sink(", INFERRED_BOUNDARY_RULE_SOURCE)

    def test_system_tuple_return_infers_multiple_output_boundaries(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(INFERRED_MULTI_BOUNDARY_SOURCE, "pipeline")
        self.assertIn("%left = ac.source", lowered)
        self.assertIn("%right = ac.source", lowered)
        self.assertEqual(2, lowered.count("ac.sink %"))

    def test_inferred_system_boundary_checks_result_contract(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        wrong_arity = INFERRED_MULTI_BOUNDARY_SOURCE.replace(
            "return combined, forwarded", "return combined"
        )
        with self.assertRaisesRegex(QueueFrontendError, "return arity"):
            lower_queue_source(wrong_arity, "pipeline")

        wrong_type = INFERRED_BOUNDARY_RULE_SOURCE.replace("-> ac.u8", "-> ac.u7")
        with self.assertRaisesRegex(QueueFrontendError, "payload"):
            lower_queue_source(wrong_type, "pipeline")

    def test_lexical_scalar_state_lowers_to_generic_ac_var(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(VARIABLE_RULE_SOURCE, "accumulator")
        self.assertIn(
            'ac.var.decl @count type i8 init 0 : i8 owner "/" stable_id "var/count"',
            lowered,
        )
        self.assertIn("%outgoing = ac.rule %incoming", lowered)
        self.assertIn("ac.var.read @count", lowered)
        self.assertIn("ac.var.assign @count", lowered)
        self.assertNotIn("ac.table", lowered)
        self.assertNotIn("ready_valid", lowered)

    def test_lexical_struct_state_uses_the_same_ac_var_family(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(STRUCT_VARIABLE_RULE_SOURCE, "accumulator")
        self.assertIn("ac.var.decl @state type !ac.struct<@types::@State>", lowered)
        self.assertIn("init 0 : i64", lowered)
        self.assertIn("ac.var.read @state", lowered)
        self.assertIn("ac.var.assign @state", lowered)
        self.assertNotIn("ac.table", lowered)

    def test_persistent_list_uses_indexed_ac_var_operations(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(INDEXED_VARIABLE_RULE_SOURCE, "indexed_state")
        self.assertIn("ac.var.decl @entries type !ac.struct<@types::@Entry>", lowered)
        self.assertIn('stable_id "var/entries" shape [4]', lowered)
        self.assertIn("ac.var.read_element @entries", lowered)
        self.assertIn("ac.var.assign_element @entries", lowered)
        self.assertNotIn("ac.table @entries", lowered)

    def test_persistent_list_requires_static_zero_shape(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        invalid = INDEXED_VARIABLE_RULE_SOURCE.replace("[0] * 4", "[1] * 4")
        with self.assertRaisesRegex(QueueFrontendError, "persistent list"):
            lower_queue_source(invalid, "indexed_state")

    def test_persistent_list_find_lowers_to_generic_ac_var_selection(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(LIST_FIND_RULE_SOURCE, "issue_queue")
        self.assertIn("ac.var.match @entries predicate", lowered)
        self.assertIn("ac.var.match.yield", lowered)
        self.assertIn('count 1 policy "min"', lowered)
        self.assertIn("ac.var.choose.yield", lowered)
        self.assertIn("ac.var.read_element @entries", lowered)
        self.assertIn("ac.var.assign_element @entries", lowered)
        self.assertNotIn("ac.table", lowered)

    def test_persistent_list_find_rejects_non_list_state(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        invalid = LIST_FIND_RULE_SOURCE.replace(
            "entries: list[Entry] = [0] * 4", "entries: Entry = 0"
        )
        with self.assertRaisesRegex(QueueFrontendError, "find requires"):
            lower_queue_source(invalid, "issue_queue")

    def test_find_predicate_captures_a_read_only_persistent_list(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(LIST_FIND_CAPTURE_SOURCE, "issue_queue")
        self.assertIn("ac.var.decl @ready_tags type i1 init false", lowered)
        self.assertIn("ac.var.match @entries predicate", lowered)
        self.assertEqual(2, lowered.count("ac.var.read_element @ready_tags"))
        self.assertIn("ac.var.assign_element @ready_tags", lowered)
        self.assertIn("ac.var.assign_element @entries", lowered)
        self.assertNotIn("ac.table", lowered)

    def test_find_key_captures_a_read_only_persistent_list(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(LIST_FIND_KEY_CAPTURE_SOURCE, "issue_queue")
        self.assertIn("ac.var.decl @priorities type i2 init 0", lowered)
        self.assertIn("ac.var.choose @entries", lowered)
        self.assertIn('count 1 policy "min" key {', lowered)
        self.assertEqual(1, lowered.count("ac.var.read_element @priorities"))
        self.assertIn("ac.var.assign_element @entries", lowered)
        self.assertNotIn("ac.table", lowered)

    def test_outputless_rule_infers_consume_only_state_transaction(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(CONSUME_ONLY_RULE_SOURCE, "completion_port")
        self.assertIn("ac.rule %completion depths [] latencies []", lowered)
        self.assertIn("ac.var.read_element @entries", lowered)
        self.assertIn("ac.var.constant true as !ac.var<i1>", lowered)
        self.assertIn("ac.rule.condition", lowered)
        self.assertIn("ac.var.assign_element @entries", lowered)
        self.assertIn(" when %", lowered)
        self.assertIn("ac.rule.return", lowered)
        self.assertNotIn("ac.marker.obligation", lowered)
        self.assertNotIn("ac.sink", lowered)

    def test_outputless_rule_cannot_be_assigned(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        invalid = CONSUME_ONLY_RULE_SOURCE.replace(
            "    complete(entries, completion)",
            "    result = complete(entries, completion)",
        )
        with self.assertRaisesRegex(QueueFrontendError, "outputless rule"):
            lower_queue_source(invalid, "completion_port")

    def test_conditional_effect_infers_read_only_scalar_state(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(
            READ_ONLY_SCALAR_CONDITIONAL_SOURCE, "completion_port"
        )
        self.assertIn("ac.var.read @epoch", lowered)
        self.assertNotIn("ac.var.assign @epoch", lowered)
        self.assertIn("ac.var.assign_element @entries", lowered)
        self.assertIn(" when %", lowered)
        self.assertIn("ac.var.mul", lowered)
        self.assertIn(
            'ac.rule %completion depths [] latencies [] name "complete"',
            lowered,
        )

    def test_early_return_chain_must_remain_contiguous(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        invalid = READ_ONLY_SCALAR_CONDITIONAL_SOURCE.replace(
            "    if old.epoch != epoch:\n",
            "    checkpoint = old.epoch\n    if old.epoch != epoch:\n",
        )
        with self.assertRaisesRegex(QueueFrontendError, "contiguous serial guard"):
            lower_queue_source(invalid, "completion_port")

    def test_early_return_chain_must_precede_state_effects(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        invalid = CONSUME_ONLY_RULE_SOURCE.replace(
            "    if old.generation != completion.generation:\n",
            "    entries[completion.index] = completion\n"
            "    if old.generation != completion.generation:\n",
        )
        with self.assertRaisesRegex(QueueFrontendError, "precede state effects"):
            lower_queue_source(invalid, "completion_port")

    def test_state_driven_rule_infers_typed_functional_condition(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(STATE_DRIVEN_RULE_SOURCE, "retirement_port")
        self.assertIn("%retired = ac.rule  depths [1] latencies [1]", lowered)
        self.assertIn("ac.rule.condition", lowered)
        self.assertIn("ac.var.read_element @entries", lowered)
        self.assertIn("ac.var.assign_element @entries", lowered)
        self.assertNotIn("ac.source", lowered)

    def test_guarded_rule_requires_boolean_condition(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        invalid = STATE_DRIVEN_RULE_SOURCE.replace("if old.valid:", "if old.value:")
        with self.assertRaisesRegex(QueueFrontendError, "condition must lower to bool"):
            lower_queue_source(invalid, "retirement_port")

    def test_one_rule_can_propose_multiple_persistent_state_updates(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(MULTI_STATE_RULE_SOURCE, "multi_state_allocate")
        self.assertIn("ac.var.decl @tail type i2", lowered)
        self.assertIn("ac.var.decl @entries type !ac.struct<@types::@Entry>", lowered)
        self.assertIn("ac.var.read @tail", lowered)
        self.assertIn("ac.var.assign_element @entries", lowered)
        self.assertIn("ac.var.assign @tail", lowered)
        self.assertEqual(2, lowered.count("ac.var.assign"))

    def test_if_else_infers_complementary_branch_local_state_presence(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(BRANCH_LOCAL_STATE_SOURCE, "branch_state")
        self.assertIn("ac.var.assign @right", lowered)
        self.assertIn("ac.var.assign @left", lowered)
        self.assertEqual(2, lowered.count(" when %"))
        self.assertIn("ac.rule.condition", lowered)
        self.assertIn('ac.var.cmp "eq"', lowered)
        self.assertNotIn("ac.table", lowered)
        self.assertNotIn("atomic", lowered)

    def test_if_else_same_owner_uses_one_typed_value_join(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        source = BRANCH_LOCAL_STATE_SOURCE.replace(
            "        left = command.value", "        right = command.value"
        )
        lowered = lower_queue_source(source, "branch_state")
        self.assertIn("ac.var.select", lowered)
        self.assertEqual(1, lowered.count("ac.var.assign @right"))
        self.assertNotIn("ac.var.assign @left", lowered)

    def test_if_else_indexed_owner_joins_index_and_value(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(INDEXED_BRANCH_JOIN_SOURCE, "indexed_branch_join")
        self.assertEqual(2, lowered.count("ac.var.select"))
        self.assertEqual(1, lowered.count("ac.var.assign_element @entries"))
        self.assertNotIn(" when %", lowered)

    def test_optional_output_has_independent_ssa_presence(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(OPTIONAL_OUTPUT_SOURCE, "optional_output")
        self.assertIn("ac.rule.condition", lowered)
        self.assertIn("ac.var.assign @count", lowered)
        self.assertIn("ac.rule.output %item when %", lowered)
        self.assertIn("ac.rule.return %rule_ready", lowered)
        self.assertNotIn("ready_valid", lowered)

    def test_multi_state_rule_requires_persistent_arguments_first(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        invalid = MULTI_STATE_RULE_SOURCE.replace(
            "def allocate(tail, entries, incoming):",
            "def allocate(tail, incoming, entries):",
        )
        with self.assertRaisesRegex(QueueFrontendError, "must precede"):
            lower_queue_source(invalid, "multi_state_allocate")

    def test_multi_input_rule_requires_one_queue_per_parameter(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        invalid = MULTI_INPUT_RULE_SOURCE.replace(
            "summed = add(left, right)", "summed = add(left)"
        )
        with self.assertRaisesRegex(QueueFrontendError, "one Queue per"):
            lower_queue_source(invalid, "pair")

    def test_multi_input_rule_accepts_mixed_queue_payloads(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        source = MULTI_INPUT_RULE_SOURCE.replace(
            "return left + right", "return left"
        ).replace("right = ac.source(int)", "right = ac.source(ac.u7)")
        lowered = lower_queue_source(source, "pair")
        self.assertIn(
            "^rule(%item0: !ac.var<i64>, %item1: !ac.var<i7>):",
            lowered,
        )

    def test_multi_input_rule_rejects_duplicate_queue_arguments(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        invalid = MULTI_INPUT_RULE_SOURCE.replace(
            "summed = add(left, right)", "summed = add(left, left)"
        )
        with self.assertRaisesRegex(QueueFrontendError, "distinct Queue"):
            lower_queue_source(invalid, "pair")

    def test_stateful_rule_keeps_table_assignment_below_python_surface(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(STATEFUL_RULE_SOURCE, "table_rule")
        self.assertIn("%outgoing = ac.rule %incoming", lowered)
        self.assertIn('ac.var.get %item field "index"', lowered)
        self.assertIn("ac.table.get @rob", lowered)
        self.assertIn("ac.table.propose @rob", lowered)
        self.assertIn('mode "replace" write_fields ["index", "value"]', lowered)
        self.assertIn("ac.marker.obligation", lowered)
        self.assertNotIn("atomic", lowered)
        self.assertNotIn("ready_valid", lowered)
        self.assertNotIn("reserve", lowered)

        reused = STATEFUL_RULE_SOURCE.replace(
            "rob[entry.index] = entry",
            "rob[entry.index] = old.with_fields(value=entry.value)",
        )
        reused_lowered = lower_queue_source(reused, "table_rule")
        self.assertEqual(1, reused_lowered.count("ac.table.get @rob"))

    def test_stateful_multi_input_rule_emits_one_atomic_firing_intent(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(STATEFUL_MULTI_INPUT_RULE_SOURCE, "table_rule")
        self.assertIn("%outgoing = ac.rule %incoming, %deltas", lowered)
        self.assertIn(
            "^rule(%item0: !ac.var<!ac.struct<@types::@Entry>>, "
            "%item1: !ac.var<!ac.struct<@types::@Delta>>):",
            lowered,
        )
        self.assertIn("ac.table.propose @rob", lowered)
        self.assertNotIn("ready_valid", lowered)
        self.assertNotIn(".pop", lowered)
        self.assertNotIn(".push", lowered)

    def test_stateful_rule_defers_nonconstant_index_proof_to_mlir(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        u2_into_five = STATEFUL_RULE_SOURCE.replace(
            "index: ac.u1", "index: ac.u2"
        ).replace("ac.table[2, Entry]", "ac.table[5, Entry]")
        u3_into_five = u2_into_five.replace("index: ac.u2", "index: ac.u3")

        for source, index_type in ((u2_into_five, "i2"), (u3_into_five, "i3")):
            lowered = lower_queue_source(source, "table_rule")
            self.assertIn(f"ac.table.get @rob [%v1] : !ac.var<{index_type}>", lowered)
            self.assertIn("ac.table.propose @rob [%v0] = %item", lowered)

    def test_persistent_var_defers_nonconstant_index_proof_to_mlir(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        five_entries = INDEXED_VARIABLE_RULE_SOURCE.replace("[0] * 4", "[0] * 5")
        u3_into_five = five_entries.replace("index: ac.u2", "index: ac.u3")
        for source, index_type in ((five_entries, "i2"), (u3_into_five, "i3")):
            lowered = lower_queue_source(source, "indexed_state")
            self.assertIn(
                f"ac.var.read_element @entries[%v0] : !ac.var<{index_type}>",
                lowered,
            )
            self.assertIn(
                f"ac.var.assign_element @entries[%v2] = %item : !ac.var<{index_type}>",
                lowered,
            )

    def test_stateful_rule_rejects_only_disproven_constant_expression_index(
        self,
    ) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        constant = STATEFUL_RULE_SOURCE.replace(
            "rob[entry.index]", "rob[0 + 1]"
        ).replace("ac.table[2, Entry]", "ac.table[3, Entry]")
        lowered = lower_queue_source(constant, "table_rule")
        self.assertEqual(2, lowered.count("ac.var.add"))
        self.assertIn("ac.table.get @rob [%v5]", lowered)

        out_of_range = constant.replace("rob[0 + 1]", "rob[1 + 2]")
        with self.assertRaisesRegex(QueueFrontendError, "index is out of range"):
            lower_queue_source(out_of_range, "table_rule")

    def test_expression_facts_use_typed_bit_transfer_for_and_mask(self) -> None:
        import ast

        from _pycircuit_semantics import BitsType, StructType, ValueField, prove_within
        from agentic_circuit._queue_frontend import Payload, _ExpressionEmitter

        payload_type = StructType("Entry", (ValueField("index", BitsType(3)),))
        emitter = _ExpressionEmitter(
            {"Entry": Payload(payload_type)}, "item", payload_type
        )
        result, result_type = emitter.emit(
            ast.parse("item.index & 3", mode="eval").body
        )

        self.assertEqual(BitsType(3), result_type)
        self.assertTrue(
            prove_within(emitter.constraint_for_result(result, result_type), 0, 3)
        )
        shifted, shifted_type = emitter.emit(
            ast.parse("item.index >> 1", mode="eval").body
        )
        self.assertEqual(BitsType(3), shifted_type)
        self.assertTrue(
            prove_within(emitter.constraint_for_result(shifted, shifted_type), 0, 3)
        )

    def test_explicit_fork_lowers_to_decoupled_fanout(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(FORK_SOURCE, "pipeline")
        self.assertIn(
            "%left, %right = ac.fork %input_queue depths [2, 2] latencies [1, 1]",
            lowered,
        )

    def test_runtime_if_infers_route_branch_transforms_and_merge(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(RUNTIME_IF_SOURCE, "pipeline")
        self.assertEqual(lowered, lower_queue_source(RUNTIME_IF_SOURCE, "pipeline"))
        self.assertIn(
            "%output_queue__if_false0_in, %output_queue__if_true0_in = "
            "ac.route %input_queue",
            lowered,
        )
        self.assertIn('ac.var.cmp "eq"', lowered)
        self.assertIn("ac.route.yield", lowered)
        self.assertIn("ac.transform %output_queue__if_false0_in", lowered)
        self.assertIn("ac.transform %output_queue__if_true0_in", lowered)
        self.assertIn(
            "%output_queue = ac.merge %output_queue__if_false0, "
            '%output_queue__if_true0 policy "priority"',
            lowered,
        )

    def test_runtime_if_requires_symmetric_queue_assignment(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        with self.assertRaisesRegex(
            QueueFrontendError, "requires one apply assignment in each branch"
        ):
            lower_queue_source(
                RUNTIME_IF_SOURCE.replace("    else:\n", "    else:\n        pass\n"),
                "pipeline",
            )
        with self.assertRaisesRegex(QueueFrontendError, "one result name"):
            lower_queue_source(
                RUNTIME_IF_SOURCE.replace(
                    "        output_queue = input_queue.apply(\n"
                    "            lambda item: item.with_fields(value=item.value + 20)",
                    "        other_queue = input_queue.apply(\n"
                    "            lambda item: item.with_fields(value=item.value + 20)",
                ),
                "pipeline",
            )
        with self.assertRaisesRegex(QueueFrontendError, "must lower to bool"):
            lower_queue_source(
                RUNTIME_IF_SOURCE.replace(
                    "if input_queue.route == 0:", "if input_queue.route:"
                ),
                "pipeline",
            )

    def test_latency_zero_and_unsupported_lambda_are_rejected(self) -> None:
        from agentic_circuit._queue_frontend import (
            QueueFrontendError,
            lower_queue_source,
        )

        with self.assertRaisesRegex(QueueFrontendError, "latency must be positive"):
            lower_queue_source(SOURCE.replace("latency=2", "latency=0"), "pipeline")
        with self.assertRaisesRegex(QueueFrontendError, "unsupported lambda"):
            lower_queue_source(
                SOURCE.replace("lambda item: item", "lambda item: unknown(item)"),
                "pipeline",
            )

    def test_typed_module_calls_lower_to_structured_reusable_acir(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(INFERRED_MODULE_SOURCE, "pipeline")
        self.assertIn("ac.system @pipeline root @Top", lowered)
        self.assertIn("ac.module @increment", lowered)
        self.assertEqual(lowered.count("ac.instance"), 2)
        self.assertIn("ac.instance @left_result of @increment", lowered)
        self.assertIn("ac.instance @right_result of @increment", lowered)
        self.assertNotIn("ac.system =", lowered)
        self.assertNotIn("source(", lowered)
        self.assertNotIn("sink(", lowered)

    def test_host_result_mode_preserves_root_queue_returns(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(
            INFERRED_MODULE_SOURCE, "pipeline", host_results=True
        )
        self.assertIn("ac.module @Top() -> (!ac.queue<i8>, !ac.queue<i8>)", lowered)
        self.assertIn(
            "ac.return %left_result, %right_result : !ac.queue<i8>, !ac.queue<i8>",
            lowered,
        )
        self.assertNotIn("ac.sink", lowered)

    def test_nested_module_call_preserves_parent_child_structure(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(INFERRED_NESTED_MODULE_SOURCE, "pipeline")
        self.assertIn("ac.module @increment", lowered)
        self.assertIn("ac.module @wrapper", lowered)
        self.assertIn("ac.instance @result of @increment", lowered)
        self.assertEqual(2, lowered.count(" of @wrapper"))

    def test_stateful_module_uses_only_lexical_ac_var_ir(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(INFERRED_STATEFUL_MODULE_SOURCE, "pipeline")
        self.assertIn("ac.module @accumulator", lowered)
        self.assertIn("ac.var.decl @total", lowered)
        self.assertIn("ac.var.read @total", lowered)
        self.assertIn("ac.var.assign @total", lowered)
        self.assertIn('owner "/body" stable_id "var/body/total"', lowered)
        self.assertEqual(2, lowered.count(" of @accumulator"))
        self.assertNotIn("ac.table", lowered)

    def test_module_multiple_lexical_states_remain_one_atomic_rule(self) -> None:
        from agentic_circuit._queue_frontend import lower_queue_source

        lowered = lower_queue_source(INFERRED_MULTI_STATE_MODULE_SOURCE, "pipeline")
        self.assertEqual(2, lowered.count("ac.var.decl"))
        self.assertEqual(2, lowered.count("ac.var.read"))
        self.assertEqual(2, lowered.count("ac.var.assign"))
        self.assertIn("ac.var.assign @count", lowered)
        self.assertIn("ac.var.assign @total", lowered)
        self.assertEqual(1, lowered.count("ac.rule %borrowed"))
        self.assertEqual(2, lowered.count(" of @tally"))
        self.assertNotIn("ac.table", lowered)


if __name__ == "__main__":
    unittest.main()
