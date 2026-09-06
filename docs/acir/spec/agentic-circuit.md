# Agentic Circuit Specification Manual

| Field | Value |
| --- | --- |
| Specification | Serial Python, Queue/Var ACIR, typed gfsim, and PYC refinement |
| Target contract epoch | `0.5` |
| Status | Current implementation contract; serialized epoch `0.5` is active on `main` |
| Public namespace | `ac` |
| Audience | Frontend, compiler, simulator, and RTL contributors |
| Design background | [NDF block-model decision](../../rfcs/acir/D-BLOCK-MODEL-001.md) |
| Executable examples | Pipeline examples |

## Purpose

Agentic Circuit lets an author describe a static circuit as serial-looking
Python. The author names values and lexical scopes; the compiler infers queue
connections, scope boundaries, typed payloads, and common hardware building
blocks. The same frozen ACIR graph can generate:

- a deterministic typed gfsim C++ model built around `SimQueue<T>`; and
- canonical PYC IR that external pinned `pycc` lowers to PYC C++ and Verilog.

This manual is the implementation-facing specification for teammates. It
defines the supported programming model, ACIR contracts, backend obligations,
examples, and current limitations. The NDF
[block-model decision](../../rfcs/acir/D-BLOCK-MODEL-001.md) records the architectural
rationale; this document records the executable contract.

![Agentic Circuit compilation and refinement](images/agentic-circuit-pipeline.svg)

The editable diagram source is
[`agentic-circuit-pipeline.drawio`](images/agentic-circuit-pipeline.drawio).

## Status and authority

The words **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY** are
normative requirements for the current contract.

Producers emit exact serialized epoch `0.5`; consumers reject other epochs
before interpreting the artifact. The toolchain provides no compatibility
alias or best-effort conversion.

When this manual and implementation disagree, use the following authority
order:

1. machine-readable schema and MLIR ODS definitions;
2. verifier and conformance tests;
3. this manual;
4. NDF decisions and references.

The principal machine-readable and executable sources are:

- `ACIRTypes.td` for Queue, Var,
  and collection types;
- `ACIROps.td` for operation
  signatures;
- `ACIROps.cpp` for semantic verification;
- `QueueGraphPlan.cpp` for the frozen
  backend plan;
- `opcodes.json` for the closed official
  building-block catalog and backend availability;
- `queue.h` and
  `queue_blocks.h` for gfsim behavior;
- `test_queue_frontend.py`
  for accepted and rejected Python syntax;
- `test/ACIR` for ACIR conformance tests.

The executable conformance suites and generated
[IR coverage ledger](../../development/acir/verification/ir-coverage.md) track the live
requirement-by-requirement status.

## Core mental model

### Serial Python elaborates a static graph

An `@ac.system` body is not an imperative program that runs once per simulated
cycle. The frontend parses its Python AST and treats statements as graph
construction in source order.

```python
@ac.system
def pipeline() -> None:
    incoming = ac.source(int)
    adjusted = incoming.apply(lambda item: item + 1)
    ac.sink(adjusted)
```

This source creates two Queue values and one persistent transform block:

```text
incoming Queue -> transform(item + 1) -> adjusted Queue -> sink
```

The frontend does not require explicit module input or output declarations.
`ac.source(...)` and `ac.sink(...)` define the current executable boundary, and
lexical uses determine scope inputs and outputs.

### Queue is state; Var is combinational value

`!ac.queue<T>` is a finite, typed, stateful FIFO channel. It has positive depth,
positive latency, occupancy, backpressure, stable identity, and commit-time
effects.

`!ac.var<T>` is an immutable, zero-latency value. It has no occupancy, capacity,
push, pop, or independent runtime identity. Lambda arguments, constants,
arithmetic results, comparisons, field projections, and immutable field updates
are Vars.

```text
!ac.queue<i64>                         stateful channel
!ac.var<i64>                           combinational scalar
!ac.queue<!ac.struct<@types::@Item>>   stateful typed channel
!ac.var<!ac.struct<@types::@Item>>     immutable token value
```

A Queue MUST have latency of at least one. A zero-latency Queue is invalid;
zero-latency logic belongs in a Var region.

### Mutable channel, immutable token

Queue state changes at commit. Token payloads do not mutate in place.

```python
# Valid: creates a new immutable token value.
next_item = item.with_fields(remaining=item.remaining - 1)

# Invalid: mutates the input object.
item.remaining -= 1
```

The frontend and backends MAY copy or move an immutable token internally, but
they MUST NOT expose mutable aliases that change a token already stored in a
Queue.

### Opcodes are common building blocks

The public `ac.*` inventory is closed and repository-owned. Users compose
common transport, computation, state, boundary, and observation blocks. They
MUST NOT define private opcodes, C++ providers, PYC providers, or raw Verilog
providers.

Application stages such as `decode`, `rename`, `dispatch`, and `retire` are
scope names or compositions. They are not generic ACIR opcodes.

Generate the canonical catalog directly from the shared backend contract table:

```sh
build/dev-llvm22/bin/acir-opcode-catalog
agentic-circuit schema opcode ac.transform
```

## Python authoring contract

### System declaration

A Queue/Var system uses `@ac.system` and takes no parameters. Inputs and outputs
are inferred from the body.

```python
import agentic_circuit as ac


@ac.system
def pipeline() -> None:
    value = ac.source(int)
    ac.sink(value)
```

The source file is compiled through AST capture. The queue primitives inside
the system body are syntax markers; ordinary Python execution of the body is
not the compilation path.

### Payload structures

Use `@ac.struct` to define a compile-time token layout.

```python
@ac.struct
class WorkItem:
    value: ac.u32
    route: ac.u2
    remaining: ac.u16
    valid: bool
```

The current frontend accepts these scalar field spellings:

| Python spelling | ACIR element type |
| --- | --- |
| `bool` | `i1` |
| `int` | `i64` |
| `ac.u1` through `ac.u64` | exact-width unsigned bit value, lowered to `i1` through `i64` |
| `ac.s8`, `ac.s16`, `ac.s32`, `ac.s64` | corresponding integer width |

Field order is declaration order. Fields MUST be unique and annotated. The
operators `+`, `-`, `*`, `&`, `|`, `^`, `~`, `<<`, and `>>`
preserve the declared width. Binary bit operands MUST have the same width; a
right-side integer literal is typed from the left operand. Results wrap modulo
(2^N), and shifts by an amount greater than or equal to (N) produce zero.
Equality is width-exact; relational comparisons on `ac.uN` are unsigned.

An `ac.uN` value may be used directly as a queue payload or as an
`@ac.struct` field. The current ACIR integer type freezes width but not
signedness as a distinct type. The retained `ac.s8/s16/s32/s64` names therefore
also use signless unsigned relational lowering for now; signed comparison
semantics require a future type-system decision.

### Static bits and named bitfield views

`ac.bits[N]` is the static-width spelling of the same exact unsigned value as
`ac.uN`; `N` must resolve from a deterministic static expression to an integer
in `[1, 64]`. Raw Python slicing is half-open:
`word[4:21]` extracts 17 bits starting at bit 4. `ac.concat(a, b)` places `a`
above `b`, and `ac.insert(base, value, lsb=N)` returns a new value without
mutating `base`.

`ac.BitfieldSpec(width=N, fields={name: (msb, lsb)})` adds names to closed bit
ranges. Different fields may overlap as alternate read views. A single update
must select disjoint fields:

```python
INSTRUCTION = ac.BitfieldSpec(
    width=32,
    fields={
        "opcode": (31, 26),
        "rd": (25, 21),
        "imm17": (20, 4),
        "low25": (24, 0),  # overlapping read view
    },
)

opcode = INSTRUCTION(word).opcode
opcode_rd = INSTRUCTION(word)["opcode", "rd"]
updated = INSTRUCTION.update(word, rd=replacement)
```

Named and multi-field reads lower to `ac.var.extract` and MSB-first
`ac.var.concat`; updates lower to immutable `ac.var.insert`. `ac.bitfield`
retains canonical width/range metadata and a stable SHA-256 in ACIR. Verifiers
resolve every field-qualified operation back to that declaration before
topology freeze. The Python frontend contains no ready/full/Queue transaction
logic for these values.

### Bounded value constraints

The compiler uses a small deterministic abstract domain with four facts:
`Constant`, `FiniteSet`, `ClosedInterval`, and `Unknown`. Constraints are
separate from `ValueType`: they do not change type identity or specialization
fingerprints, and the frontend does not serialize range attributes or markers
into ACIR. Widths, fixed shapes, aggregate indices, slice/insert bounds, and
topology loop counts must still become concrete before ACIR is emitted.

Typed bit transfers follow the exact `ac.var` semantics: arithmetic wraps
modulo (2^N), and a logical shift by at least (N) yields zero. Finite-set and
Cartesian propagation are capped at 64 values; topology expansion is capped at
10,000 iterations. When a fact exceeds a cap or cannot be represented safely,
analysis widens conservatively and proof sites fail closed.

`ACDataFlowAnalyzer` is the public compiler analysis. It recomputes constraints
from ACIR SSA through MLIR dataflow and proves every dynamic persistent-list or
Table index is within `[0, entries - 1]` before rule lowering, topology freeze,
and QueueGraph planning. MLIR's generic `DataFlowSolver` remains private to the
analyzer implementation. For example, a `u2` index is safe for five entries,
while an unconstrained `u3` index is rejected unless preceding operations
produce a narrower proven fact. QueueGraph independently recomputes the same
obligations so forged Frozen ACIR cannot bypass the verifier.

This slice is intentionally path-insensitive. Dynamic aggregate extract/insert,
guard-derived refinement, runtime-loop termination proofs, and general enum
branch exhaustiveness remain separate extensions.

### Masked bit matching

`ac.matches(value, pattern)` is the decode-oriented masked comparison for an
exact-width bits value. `pattern` must be a Python string literal containing
only lowercase `0`, `1`, and `x`, and its length must equal the value width.
The first character names the most-significant bit. `x` is a compile-time
don't-care position; it does not introduce runtime X/Z semantics.

```python
is_compute = ac.matches(opcode, "1xx0")
```

The frontend converts the pattern to one canonical `mask` and `value` and
emits `ac.var.matches`. ACIR verifies input/result types, width bounds, and
that `value` sets no bit outside `mask`. QueueGraph preserves the operation as
`masked_match`, serializing both constants as exact-width lowercase hex strings
so bit 63 is lossless. gfsim evaluates `(input & mask) == value`; PYC lowering
uses only vendor-neutral `pyc.constant`, `pyc.and`, and `pyc.eq`.

The parser implementation is shared through the semantic core, but the Agentic
surface deliberately enables only the basic grammar above. pyCircuit's existing
extended syntax remains a separate frontend policy. Agentic Circuit does not
add pattern objects, alternation, captures, runtime patterns, Python
`match/case`, or general ASL pattern matching.

### Variable properties

Python exposes ordinary values, module/class fields, and lexical scopes rather
than hardware-named variable annotations. MLIR infers two independent
properties: lifetime is static, temporary, or persistent; update permission is
immutable or assignable. `const` is static immutable. Rule parameters, returns,
and local expressions are temporary immutable SSA snapshots. Scope-owned state
is persistent assignable state, but every committed value read by one rule
activation remains immutable; assignment proposes the next committed value.

These properties are compiler facts, not Python markers. Subsequent analysis
selects storage and transport from type, access pattern, def-use, scheduling,
and target/NDF constraints. `ac.var` is the single ACIR variable-value
concept across expressions and inferred state.

Persistent class/module fields lower to the same `ac.var` family. Internal
`ac.var.decl` names the lexical state, `ac.var.read` produces an immutable
committed snapshot, and `ac.var.assign` proposes the next value within one
rule. Storage selection eliminates these operations before rule closure. The
first executable slice accepts a zero-initialized scalar integer or flat struct
and selects a single-entry committed implementation; the Python frontend does
not expose that choice.

A fixed persistent list uses ordinary Python syntax such as
`entries: list[Entry] = [0] * 8`. The frontend emits a shaped `ac.var.decl`
plus `ac.var.read_element`/`ac.var.assign_element`; storage selection maps that
logical variable to touched-entry committed storage. Dynamic indices currently
are accepted only when `ACDataFlowAnalyzer` proves their value domain is within
the list shape; authors do not write a frontend range check or marker.

`ac.find(values, where=predicate, key=key)` is the storage-neutral collection
query for a persistent Python list. It returns an intrinsic value with
`.valid`, `.index`, and `.value`; omitting `key` selects the first matching
index, while providing a fixed-width integer key selects the minimum key with
stable index tie-breaking. Raw ACIR uses `ac.var.match` and `ac.var.choose`.
Storage selection rewrites them to the existing committed Table query without
changing the Python variable model. The selected index/value may affect state
only under the corresponding `.valid` condition.

A predicate may read another persistent list. Such an owner is an activation
source but not a transaction resource unless the rule writes it. Generated
policies capture read-only committed storage through const references; only
writable owners participate in prepare/publish/commit. This supports an ISQ
whose readiness table update wakes one oldest-ready query instead of bulk
rewriting every resident entry.

The current single-condition rule subset also carries verifier-derived typed
summary attributes for guard kind, Queue availability/capacity checks, effects,
output-presence kind, state accesses, schedule kind, and lexical arbitration
membership. These closed enums replace strings as the analysis vocabulary for
the supported 0/1-output subset, while the old strings remain derived readable
summaries. A `predicate` summary is not a path identity: general CFG and
selected multi-output semantics require an SSA `!ac.var<i1>` presence value on
each output and state proposal and remain a later contract.

`ac.priority_encode(value, order="low")` is a semantic combinational helper.
Its `.index` and `.valid` projections share one `ac.var.priority_encode` in
ACIR. `order="low"` selects the least-significant asserted bit and
`order="high"` selects the most-significant asserted bit; an all-zero input
returns `valid=0,index=0`. QueueGraph uses the gfsim reference model and lowers
the same operation to vendor-neutral `pyc.priority_encode`.

`ac.popcount(value)` returns the number of asserted bits using exactly
`max(1, ceil(log2(N+1)))` result bits. ACIR preserves it as
`ac.var.popcount`; QueueGraph C++ uses the typed gfsim reference and
QueueGraph-to-PYC emits one vendor-neutral `pyc.popcount`. The Verilog-only
selection pass may choose the qualified BSD implementation.

`ac.count_leading_zeros(value)` and `ac.count_trailing_zeros(value)` count
consecutive zero bits from the most- or least-significant end. The result range
is `[0,N]`, so an all-zero `N`-bit input returns `N`; its result width is
`max(1, ceil(log2(N+1)))`. Both lower to `ac.var.count_zeros` with a static
direction marker, then to one vendor-neutral `pyc.count_zeros` family.
QueueGraph C++ uses the corresponding typed gfsim helper.

### Source and sink

`ac.source(T, depth=N, latency=L)` creates a Queue boundary with payload `T`.
`depth` and `latency` default to one and MUST be positive compile-time integers.

```python
incoming = ac.source(WorkItem, depth=8, latency=1)
ac.sink(incoming)
```

`ac.sink(queue)` consumes tokens from a Queue. A system MUST contain at least
one source Queue and at least one sink.

### Transform with `apply`

`queue.apply(lambda item: expression, depth=N, latency=L)` creates an
`ac.transform` block and one output Queue.

```python
updated = incoming.apply(
    lambda item: item.with_fields(
        value=(item.value + 1) * 2,
        remaining=item.remaining - 1,
    ),
    depth=4,
    latency=2,
)
```

The current lambda subset supports:

- the lambda parameter itself;
- integer and Boolean constants;
- structure field reads;
- `+`, `-`, and `*` over identical Var types;
- `==`, `!=`, `<`, `<=`, `>`, and `>=`;
- immutable `with_fields(...)` updates.

The lambda MUST take exactly one argument and MUST return the Queue payload
type. Function calls other than `with_fields`, mutation, I/O, allocation,
ambient state access, and arbitrary Python expressions are rejected.

### Lexical scope and inferred boundaries

`with ac.scope("name"):` defines ownership and hierarchy. It does not declare
ports.

```python
incoming = ac.source(int)

with ac.scope("frontend"):
    adjusted = incoming.apply(lambda item: item + 1)
    with ac.scope("inner"):
        completed = adjusted.apply(lambda item: item * 2)

ac.sink(completed)
```

The compiler infers:

- `incoming` as a borrowed input of `/frontend`;
- `adjusted` as a Queue owned inside `/frontend`;
- `completed` as an exported output of `/frontend/inner` and `/frontend`;
- parent ownership for an interconnect at the lowest common lexical ancestor.

Scope names MUST be non-empty, and one lexical path MUST NOT be declared twice.

### Multiple consuming uses

Queue consumption is destructive. If one Queue variable feeds multiple
`apply` statements, the frontend inserts `ac.broadcast` at the lexical lowest
common ancestor.

```python
incoming = ac.source(int)
left = incoming.apply(lambda item: item + 1)
right = incoming.apply(lambda item: item + 2)
```

The inserted broadcast is strict and atomic: it pops the input only when every
output can accept the token. It has no hidden per-output progress state.

### Explicit decoupled fork

Use `fork` when outputs may accept the token on different cycles.

```python
left, right = incoming.fork(outputs=2, depth=2, latency=1)
```

`ac.fork` retains one token and a per-output delivered mask until every output
has accepted that token. Each output receives the token exactly once. The input
is popped only after delivery to all outputs completes.

This distinction is normative:

| Block | Acceptance rule | Hidden progress state |
| --- | --- | --- |
| `ac.broadcast` | all outputs accept in one atomic firing | none |
| `ac.fork` | outputs may accept independently | per-token delivered mask |

### Route

`route` sends one token to exactly one statically declared output.

```python
scalar, vector, cube, tma = prepared.route(
    outputs=4,
    key=lambda item: item.route,
    depth=2,
    latency=1,
)
```

The output tuple arity MUST equal `outputs`. The selector lambda returns an
integer or enum-like Var. A selector outside `[0, outputs)` is a deterministic
runtime failure named `route_selector_out_of_range`; it is not wrapped or
clamped.

### Merge

`merge` combines two or more Queues with identical payload types.

```python
completed = scalar_done.merge(
    vector_done,
    cube_done,
    tma_done,
    policy="round_robin",
    depth=8,
    latency=1,
)
```

Supported policies are:

- `priority`: select the first ready input in source order;
- `round_robin`: begin from a committed cursor and advance the cursor after a
  successful transfer.

The output Queue applies ordinary capacity and latency rules.

### Dependency scheduling

`depend` is the generic bounded dependency window. It admits typed tokens,
starts a token when its predecessor is complete, counts its declared execution
cost, and emits tokens in completion order.

```python
completed = issued.depend(
    key=lambda item: item.sequence_id,
    waits_for=lambda item: item.waits_for,
    resource=lambda item: item.route,
    cost=lambda item: item.cycles,
    capacity=8,
    resources=4,
    no_dependency=255,
    depth=8,
    latency=1,
)
```

The three lambdas are pure Var regions. `key` and `waits_for` MUST return the
same integer Var type, no wider than 64 bits. `no_dependency` MUST fit that
type. `resource` selects one of the statically declared resources; each resource
admits at most one executing token at a time. `cost` MUST return a positive
integer at runtime. Dependencies refer to tokens retained in the bounded
window; a missing predecessor blocks the token and can participate in deadlock
diagnostics.

### Credit scheduling

`credit` is a bounded parallel completion window. It admits at most `credits`
tokens, evaluates a pure per-token cost, advances every occupied slot once per
epoch, and returns the slot when the completed token transfers to the output
Queue.

```python
completed = issued.credit(
    cost=lambda item: item.cycles,
    credits=2,
    depth=4,
    latency=1,
)
```

`credits`, output `depth`, and output `latency` are positive compile-time
constants. The cost lambda MUST return an integer Var no wider than 64 bits.
Every accepted runtime cost MUST be positive; zero or negative cost produces
the deterministic `credit_nonpositive_cost` failure/assertion.

Each slot counts down independently, so completion order may differ from input
order. When multiple slots are complete, the block chooses the lowest canonical
slot index and emits at most one token per epoch. Admission and retirement may
occur in the same epoch when they use different committed slots. A slot retired
in the current Xfer is not reused until a later epoch.

### Barrier synchronization

`barrier` synchronizes two or more Queue heads and publishes a positionally
matching output tuple as one atomic firing. Input payload types may differ.

```python
left_ready, right_ready = left.barrier(
    right,
    depth=2,
    latency=1,
)
```

All input Queues MUST be distinct. The output count MUST equal the input count,
and each output payload type MUST match its corresponding input. The barrier
waits until every input can pop and every output can accept; then all pops and
pushes commit together. Before that Xfer, it publishes no partial result.

The output Queues are ordinary independent Queues after the atomic transfer.
Downstream consumers may therefore drain them on different later epochs without
changing the barrier firing contract.

### Typed memory

`memory` declares a physical single-read/single-write state instance. One or
more logical request endpoints connect to it; requests and responses use one
shared structure type. Three pure lambdas select the address, write enable, and
write data; `result_field` names the response field replaced with old data.

```python
@ac.struct
class MemoryRequest:
    address: ac.u4
    write: ac.u1
    data: ac.u16
    tag: ac.u8


sram = ac.memory(ac.u16, entries=16, init=0, latency=3)
requests = ac.source(MemoryRequest, depth=4, latency=1)
responses = sram.request(
    requests,
    address=lambda item: item.address,
    write=lambda item: item.write,
    data=lambda item: item.data,
    result_field="data",
    depth=4,
)
ac.sink(responses)
```

`entries`, `init`, instance `latency`, `result_field`, and `depth` are
compile-time constants. `entries`, `depth`, and instance `latency` MUST be
positive. The response Queue has fixed latency one. The address MUST
be an integer Var no wider than 64 bits and wide enough to represent every
entry. The write policy MUST return `!ac.var<i1>`. The data policy and result
field MUST have the same integer type, no wider than 64 bits. The current contract supports
deterministic zero initialization only, so `init` MUST equal zero.

Every accepted request performs a read. A request accepted in cycle `T` can
offer its response no earlier than `T + latency`; the instance remains busy
throughout that interval and while the response Queue is blocked. A response
preserves the request's other fields and replaces `result_field` with the
pre-transfer memory value. A
write commits at Xfer. Therefore a read and write to the same address in one
request returns old data and makes the new data visible to a later request.

### Stateful Table prototype

Epoch `0.5` separates locally owned state from request/response memory:

```python
Table16 = ac.table[16, Entry]
table = Table16(init=0)
entry = table.view(0)
snapshots = entry.read(when=entry.valid, depth=1, latency=1)

table.view(lambda update: update.index).patch(
    updates,
    enable=lambda update: update.enable,
    done=True,
    result=lambda update: update.result,
)
responses = table.view(lambda request: request.index).read(
    requests,
    when=lambda request: request.enable,
    depth=1,
    latency=1,
)

pending = table.match(lambda entry: not entry.valid)
table.view(pending).patch(
    enable=request_slot.valid,
    valid=True,
    age=lambda entry: entry.age + 1,
)

table.view(tail).allocate(
    enable=allocation.valid,
    value=allocation.value,
)
```

`Entry` is a boolean, a fixed-width integer, or a flat struct of those scalar
types. The Table is one-dimensional and has an all-zero initial image. It may
have multiple `write` or `patch` endpoints when their statically declared
top-level field sets are pairwise disjoint. `read` always returns `Queue<Entry>`.
Queue-driven read with `when=false` preserves its input; disabled write consumes
its input without proposing state. Same-tick reads observe old committed data,
and a write becomes visible at tick commit. Dynamic bounds failures use
`table_index_out_of_range`.

`Table.view(candidates)` accepts a same-Table `CandidateSet` from `match` for a
state-driven masked update. Masked `write` assigns one uniform complete value;
masked `patch` assigns uniform fields or evaluates a pure `lambda entry` from
each selected old Entry. A false enable does not evaluate the mask or value,
an empty mask is a no-op, and all selected Entries commit atomically. Scalar
and masked endpoints may coexist under the same disjoint-field rule. Dynamic
address, mask, enable, and predicate mutual exclusion is not analyzed; two
endpoints that declare the same field are rejected.

One state-driven scalar `allocate` endpoint may coexist with those ordinary
field writers. It installs one complete Entry at the caller-supplied index; it
does not search for a free slot, check occupancy, or change `valid`
automatically. Queue-driven and CandidateSet-masked allocation are rejected.
All policies read the old committed image. Commit merges ordinary field
proposals first and applies allocation last, so allocation wins when both target
the same Entry and unrelated Entries remain independent.

Each authored `match` and `choose` is a shared value, not endpoint-local sugar.
Frozen ACIR emits one dominating `ac.table.match` or `ac.table.choose`, and
every read/write policy captures that SSA result. The QueueGraph and typed
gfsim implementations preserve the sharing: evaluation is lazy and cached by
the complete Epoch, so multiple consumers cause one Table scan per Epoch.
Advancing the Epoch or resetting the model invalidates that result. A choose
mask must come from a match on the same Table. `policy="first"` has an empty key
region; min/max retain one typed key region.

`EntryView` is elaboration-only. `patch` lowers before Frozen ACIR to
`ac.table.get`, immutable `ac.var.with` updates, and `ac.table.write` or
`ac.table.masked_write`; there is no `ac.table.patch` operation. Both Frozen
write operations carry required, normalized, non-empty `write_fields` and a
required `mode`. Ordinary writes use `mode "field"`; scalar allocation uses
`mode "replace"`; masked writes accept only `field`.
Struct full writes list every declared field; scalar Entries use `$entry`.
Every value region still returns a complete Entry, but commit copies only the
declared fields. All endpoints evaluate from one old committed image and their
disjoint proposals are merged once at the tick edge. Table is a
typed gfsim C++ prototype. PYC/RTL
lowering is deferred and rejects the graph with `unsupported provisional
Table`. Request/response storage remains `ac.memory`; legacy `ac.table(...)`
has been removed. The single public Python example is
`issue.py`. It combines two field-disjoint
operand wakeups, next-tick minimum-age selection, grant-driven removal, and a
complete Entry allocation after explicitly matching and choosing an old-state
empty slot. A full Table retains the allocation request until a slot is
selectable. Additional focused sources are internal E2E fixtures rather than
public examples.

### Reorder

`reorder` accepts out-of-order completions and releases them in monotonically
increasing key order. It is the generic ordering primitive used to compose a
ROB-like retirement path; `retire` itself remains an application scope.

```python
retired = completed.reorder(
    key=lambda item: item.sequence_id,
    capacity=64,
    start=0,
    depth=8,
    latency=1,
)
```

The key lambda MUST return an integer Var no wider than 64 bits. `capacity`,
`start`, output `depth`, and output `latency` are compile-time constants. The
non-negative `start` value MUST fit the key width. The
block backpressures when every entry is occupied and emits only the token whose
key equals the committed next key. Duplicate, negative, or already retired keys
are invalid.

### Observation

`ac.observe(queue)` reads the committed Queue head without consuming it and
without participating in backpressure.

```python
ac.observe(completed)
ac.sink(completed)
```

An observation-only use does not cause broadcast insertion. Observations may
record a new head when the token or committed pop count changes, but MUST NOT
alter functional state.

### Verification expectation

`ac.expect` is a non-consuming verification leaf for gfsim and PYC testbench
boundaries.

```python
ac.expect(
    completed,
    predicate=lambda item: item.value > 0,
    message="value must be positive",
)
```

The predicate MUST be pure and return bool. gfsim evaluates each new committed
head and reports `expectation_failed` without consuming or backpressuring the
Queue. `ac.expect` is verification-role, not design-role: PYC design emission
rejects it with an explicit instruction to place the check at the testbench
boundary. `ac.observe` remains observation-role and may enter design lowering
because it cannot change functional state.

### Rule authoring

`@ac.rule` is the only explicit Python scheduling boundary. A rule receives
immutable payload values and returns output payload values; users do not spell
Queue effects, checks, ready/valid handshake, scheduling, commit, or rollback.

```python
@ac.rule
def increment(item):
    return item.with_fields(value=item.value + 1)

@ac.system
def pipeline(incoming: Item) -> Item:
    outgoing = increment(incoming)
    return outgoing
```

Non-`const` system parameters and typed returns are the preferred external
boundary surface. The compiler inserts internal `ac.source` and `ac.sink`
nodes; multiple outputs use an ordered `tuple[...]` annotation and tuple
return. Explicit Python `source(...)` and `sink(...)` remain transitional.

The epoch 0.5 pure-rule frontend accepts one or more Queue inputs and one total
return path. Every argument is the immutable committed head payload of its
corresponding Queue. It emits transient variadic `ac.rule` IR and one typed
pending output-handshake obligation. MLIR passes infer all input-consume and
output-produce effects, establish an explicit empty-check contract, materialize
the `ready_valid_Nx1` handshake, resolve scheduling, and lower to marker-free
`ac.firing`. Dynamic-check obligations are rejected in this slice because no
executable checked IR exists yet. A proof pass canonicalizes the closed pure
firing to a variadic `ac.transform`; QueueGraph/gfsim then realizes it as one
atomic transform, so output backpressure never consumes only a subset of the
inputs. Input Queue payloads may differ; the single result currently preserves
the primary input payload type.

A stateful rule may return no value. In that form the compiler infers an
`Nx0` consume-only transaction: selected inputs and state proposals commit
together and no dummy Queue or sink is created. A stateful rule may also have
no Queue input and return one value. A single Python `if` around its state
assignment/return lowers to typed `ac.rule.condition` and then
`ac.firing.condition`; a false condition forms no candidate transaction.
Output capacity remains part of the inferred transaction, so a state-driven
retire cannot clear its entry while its result Queue is backpressured.

An outputless rule with exactly one Queue payload may explicitly finish a token
without selecting its state effects by using one or more ordinary serial early
returns:

```python
@ac.rule
def complete(entries, completion):
    old = entries[completion.index]
    if old.generation != completion.generation:
        return
    if old.epoch != completion.epoch:
        return
    entries[completion.index] = completion
```

This is distinct from the blocking trailing `if` above. The compiler emits a
constant-true candidate condition for input consumption and combines the
inverted early-return predicates into one SSA conjunction attached as `when` on
generic `ac.var.assign` operations. Storage selection preserves that SSA
presence on `ac.table.propose`. The early returns must be contiguous and precede
all state effects, so flattening pure, total predicate evaluation does not move
a write across a return. All conditional state effects in this restricted form
share the resulting predicate; it cannot be combined with a blocking guard,
multiple Queue payloads, or a selected output yet.

An outputless one-input rule may also use one ordinary `if/else` whose branches
assign distinct persistent owners. The frontend preserves the branch test and
its Boolean complement as separate SSA presence values. Rule, Firing, and
QueueGraph verification independently require exactly one shared predicate or
one structurally proven complementary pair. Generated gfsim evaluates one Work
candidate and prepares only the selected owner; the input and selected state
still publish through one atomic group. If both arms assign the same scalar
owner, the compiler emits one `ac.var.select` value join followed by one
unconditional state proposal. QueueGraph/gfsim use a ternary expression and PYC
uses `pyc.mux`; the owner therefore retains one write slot and one commit.
If both arms assign the same persistent list, the compiler joins both value and
index with typed `ac.var.select` operations, then emits one unconditional
`ac.var.assign_element`. Each authored index must retain the existing exact
width/full-domain safety proof. A branch value that depends on another
branch-written owner remains rejected until general state joins are available.

One stateful output may be optional. A trailing Python
`if condition: return value` followed by `return` means the input and preceding
state effects are always selected, while the output is selected only by the
condition. The frontend emits `ac.rule.output ... when`, and MLIR independently
derives predicate-qualified output capacity/effect summaries. A full output
Queue therefore blocks the complete transaction only when output presence is
true; the absent-output path consumes input and commits state without requiring
capacity. Rule/Firing/QueueGraph verifiers require one input and a constant-true
candidate for this differing output presence.

`ACDataFlowAnalyzer` walks backward from candidate, output-presence, and
state-effect presence values and materializes compiler-owned state-snapshot
proof. A top-level `ac.table.get` becomes `ac.state.snapshot` with an exact
static or proven full-domain dynamic index. A source `ac.table.match` reserves
its complete scanned owner; a foreign Table read inside that match predicate
becomes `ac.state.snapshot_set`, keyed by the match mask, so only indices
actually read during the same scan are reserved. A foreign Table read inside a
`table.choose` key region uses the choose index result as canonical evaluation
provenance. Its dependency mask is updated only after the candidate-mask test
and immediately before that candidate's key evaluation. The current
snapshot-set contract supports at most 64 entries. Table reads in a shared,
non-transactional choose key are rejected rather than lowered without snapshot
closure.

The closure verifier independently recomputes the complete proof set before
freeze. QueueGraph preserves scalar/all/set reservations independently from
writes, activation sources, and transaction resources. Generated gfsim folds a
set reservation into a `uint64_t` mask during the original match scan, without
a second state traversal or heap allocation. A scalar rule parameter bound to
lexical persistent state is inferred from the state-prefix call binding and
lowered to `ac.var.read` even when it is read-only, so authors do not need a
self-assignment to force serialization.

Snapshot proof also carries ordered `read_fields`. A direct `ac.var.get` from a
Table result narrows the reservation to that field; consuming the complete
Entry records every declared field, and scalar entries use `$entry`.
QueueGraph verifies the field names against the Entry declaration. Generated
gfsim uses a compact `StateReservation`: complete Entry reads retain an entry
mask, while partial reads use one 64-bit relation whose bit position encodes an
exact `(entry, field)` pair. Same-entry field-merge writes conflict only when
that pair is present, while replace writes still conflict with every read of
the entry. Relation union preserves heterogeneous clauses without a
cross-product or heap allocation. Partial relations currently require
`entries * declared_fields <= 64`; complete Entry and scalar masks retain the
existing 64-entry limit.

One rule activation may update multiple lexical persistent values. Scalar and
fixed-list assignments remain generic `ac.var` operations until storage
selection; the resulting heterogeneous state owners are carried by one
`gfsim::QueueStateTransition`. Work computes one immutable candidate containing
all owner writes. Arbitrate either reserves and publishes every owner and
selected Queue, or publishes none.

`examples/agentic-circuit/state/circular_rob.py` exercises this path as a real
four-entry circular ROB. Ordinary scalar variables hold head, tail, occupancy,
and recovery epoch; a normal `list[RobEvent]` holds entries. Its four rules
implement recovery, allocation, completion, and state-driven retirement. The
generated test proves full/empty distinction, fixed-width head/tail wrap,
per-slot generation rejection, recovery-epoch rejection, out-of-order
completion, in-order retirement, and allocation/retirement output
backpressure. The Python source contains no Queue/Table/source/sink/readiness or
commit operations. Its generation and recovery epoch are 16-bit finite tags;
the environment must not retain a completion across `2^16` same-slot reuses or
recovery epochs.

Python `ac.atomic()` and `Queue.firing()` are removed and produce migration
diagnostics directing authors to `@ac.rule`. The lower-level words remain
compiler implementation concepts, not Python APIs.

The first stateful rule subset adds one Table parameter followed by one or more
Queue payload parameters without exposing the transaction machinery:

```python
@ac.rule
def install(rob, entry):
    old = rob[entry.index]
    rob[entry.index] = entry
    return old

outgoing = install(rob, incoming, metadata)
```

The Table Entry, primary input, and any output Queue payload types MUST match;
additional input Queue payloads may differ. The body MAY bind one committed
Table Entry observation, MUST perform exactly one complete Entry replacement,
and MAY return zero or one payload. A dynamic `ac.uN` index is
accepted only for a `2^N`-entry Table; a constant index must be in range. This
statically discharges bounds while executable dynamic checked IR remains
pending.

The frontend emits firing-local `ac.table.propose`. Separate MLIR passes infer
every input consume, the output produce, and the Table replace effect;
materialize `ready_valid_Nx1_table`; infer lexical priority and typed state
footprints; discharge every marker; and retain the result as stateful
`ac.firing`. QueueGraph lowers the
closed firing to `gfsim::QueueTableTransition`. It is not canonicalized to
`ac.transform`, and PYC continues to reject the provisional Table boundary.
Field or masked updates, optional or multiple outputs, multiple state
proposals, CFG branches, Reg effects, and arbitration are not part of this
subset.

### Bounded feedback

The current runtime-loop form is one Queue rebinding through one `apply`.

```python
current = ac.source(WorkItem)

while current.remaining > 0:
    current = current.apply(
        lambda item: item.with_fields(
            value=item.value + 1,
            remaining=item.remaining - 1,
        ),
        depth=2,
        latency=1,
    )

ac.sink(current)
```

The frontend lowers this form to `ac.feedback` with a stateful feedback Queue.
The current compiler freezes `max_iterations = 1024`. When the condition is
false, the current token exits unchanged. When it is true, the immutable update
is recirculated. Exceeding the bound reports `feedback_iteration_limit`.

A bounded loop may place one runtime `break` guard before its Queue update and
one runtime `continue` guard at the tail:

```python
while current.remaining > 0:
    if current.stop:
        break
    current = current.apply(step)
    if current.skip:
        continue
```

The leading `break` becomes part of the explicit feedback continuation
condition; a matching token exits unchanged. A tail `continue` targets the same
feedback edge as normal loop fallthrough and is normalized to that edge. Other
statement placement, loop `else`, and more than one Queue update remain
deterministic errors.

### Static collections

Queue collections have compile-time shape and membership.

```python
lanes = ac.array(
    2,
    lambda lane: ac.source(int, depth=lane + 1),
)
named = ac.map({"right": lanes[1], "left": lanes[0]})
active = ac.set({named["right"], named["left"]})

for lane in active:
    ac.sink(lane)
```

Runtime selection from a flat Queue collection uses one explicit control Queue
and lowers to the official `ac.select` mux. It never creates a runtime Queue
handle.

```python
control = ac.source(SelectControl)
lanes = ac.array(2, lambda index: ac.source(int))
selected = lanes.select(
    control,
    key=lambda item: item.route,
)
ac.sink(selected)
```

The control token and exactly one selected data token transfer atomically. An
out-of-range selector produces `select_selector_out_of_range`. Nested
collections MUST first be statically flattened to a flat collection.

The current frontend supports:

- `ac.array(extent, lambda index: ...)` with positive static extent;
- `ac.map({...})` with unique compile-time `bool`, `int`, or non-empty `str`
  keys;
- `ac.set({...})` over unique Queue or nested collection members;
- nested collections;
- static indexing;
- compile-time iteration over a collection.

Map keys and set members are canonicalized. Frozen QueueGraph planning flattens
collections into statically named Queue members; it never creates a runtime
Queue pointer or host-order container dependency.

### Static control

`if True` and `if False` are elaborated statically. `for` over
`range(constant)` or a static Queue collection is expanded at compile time.

```python
if True:
    selected = incoming.apply(lambda item: item + 1)

for index in range(2):
    ac.sink(lanes[index])
```

One structurally decreasing Queue helper may recurse at compile time:

```python
def add_stages(queue, count):
    if count == 0:
        return queue
    return add_stages(
        queue.apply(lambda item: item + 1),
        count - 1,
    )

outgoing = add_stages(incoming, 3)
```

The helper MUST have exactly one Queue parameter and one integer count, a
`count == 0` identity base case, and one self-call whose count is `count - 1`.
The call-site depth MUST be a compile-time integer in `[0, 1024]`. The frontend
expands the helper before ACIR publication; no recursion, call stack, or dynamic
module creation remains in either backend.

A runtime Queue condition may use the symmetric form below. The condition MUST
lower to `ac.var<i1>`, both branches MUST consume the same Queue through one
`apply`, and both branches MUST assign the same fresh result name.

```python
if incoming.route == 0:
    selected = incoming.apply(
        lambda item: item.with_fields(value=item.value + 10)
    )
else:
    selected = incoming.apply(
        lambda item: item.with_fields(value=item.value + 20)
    )
```

The frontend lowers this statement to an official two-way `ac.route`, two
branch transforms, and a mutually exclusive priority `ac.merge`. More complex
runtime Queue control remains explicit through `route`/`merge`. Runtime topology
allocation is forbidden.

## ACIR type contract

### Immutable payload types

`!ac.var<T>` and `!ac.queue<T>` require an immutable ACIR payload type. They
MUST NOT recursively carry Queue, mutable list, function, channel, endpoint, or
other runtime-reference types.

Valid examples:

```mlir
!ac.var<i32>
!ac.queue<i32>
!ac.var<!ac.struct<@types::@WorkItem>>
!ac.queue<!ac.struct<@types::@WorkItem>>
```

Before MLIR rendering, the frontend represents values with immutable recursive
descriptors: logical bool, exact bits, nominal enum/struct, structural tuple,
and fixed value array. Each descriptor has canonical identity, stable SHA-256,
and recursive bit width. `BoolType()` and `BitsType(1)` are deliberately
different compiler facts even though both currently render as `i1`. Persistent
Python lists are state containers and are not `ArrayType` values. Fixed payload
arrays render as `!ac.value_array<N x T>`; `!ac.array` remains a static
Queue/Var topology collection. Tuple and value-array elements must be
recursively immutable. The executable frontend admits acyclic nested structs,
standard Python enum values, structural tuple construction, fixed value-array
construction, and constant aggregate indexing.

Tuple and fixed-array payloads use ordinary Python annotations and values:

```python
@ac.struct
class Packet:
    pair: tuple[ac.bits[3], ac.bits[5]]
    lanes: ac.array[4, ac.bits[4]]

updated = item.with_fields(
    pair=(item.pair[0] + 1, item.pair[1] + 1),
    lanes=(item.lanes[1], item.lanes[2], item.lanes[3], item.lanes[0]),
)
```

Tuple/list literals must have the exact statically known arity, and aggregate
indices must be static and in range before Frozen ACIR. The compiler lowers
them through typed `ac.var.tuple`, `ac.var.array`, and `ac.var.element`, keeps
aggregate identity and width in QueueGraph, and uses one packed value in gfsim
and PYC rather than expanding a hardware container object in Python.
Enum and nominal struct elements are recursively packed before construction and
restored after selection, using the same MSB-first field order as PYC. Width
addition/multiplication is checked; a field wider than 64 bits or a malformed
element boundary is rejected before backend generation.

QueueProgram retains these descriptors on Queue payloads, persistent values,
Table entries, memories, slots, rule state effects, and reusable module
signatures. Expression lowering returns `(SSA name, ValueType)` and performs
identity, width, enum, aggregate, and field checks on descriptors. MLIR spelling
is produced only by the ACIR text renderer; the C++ QueueGraph begins its own
string representation only after parsing that verified ACIR boundary.
`BoolType()` and `BitsType(1)` remain distinct inside the frontend while a
small explicit pair of epoch-0.5 compatibility helpers preserves the accepted
`i1` equality and integer-width boundaries until a separate hard-break
decision.

One nominal struct may now contain another nominal struct. Declarations may
appear in either source order; cycles are rejected. Nested access and update
use ordinary Python values:

```python
@ac.struct
class Packet:
    header: Header
    payload: ac.bits[17]

updated = item.with_fields(
    header=item.header.with_fields(mode=item.header.mode + 1)
)
```

The compiler emits typed chained `ac.var.get`/`ac.var.with`, orders generated
C++ declarations by dependency, and recursively packs the same value for PYC
C++ and Verilog. It does not flatten the Python struct or duplicate nested
types per instance.

Nominal values use the standard Python enum class:

```python
from enum import Enum

class Mode(Enum):
    IDLE = 0
    RUN = 1
    WAIT = 2
```

Members must be contiguous from zero in declaration order. A nested struct
field may use `Mode`; `Mode.RUN` lowers to a verified `ac.var.enum` value.
Enums support equality and inequality only in the current slice. QueueGraph
retains the member list and encoding width, gfsim emits one compact C++ enum,
and PYC/Verilog use the same exact-width ordinal.

Invalid examples:

```mlir
!ac.queue<!ac.var<i32>>
!ac.var<!ac.queue<i32>>
!ac.queue<!ac.list<i32>>
!ac.queue<(i32) -> i32>
```

### Static collection types

ACIR provides statically shaped collection types:

```mlir
!ac.array<4 x !ac.queue<i32>>
!ac.map<["cube", "scalar", "vector"], !ac.queue<i32>>
!ac.set<4 x !ac.var<i16>>
```

Array and set lengths MUST be positive. ACIR map keys are non-empty unique
strings in strict lexicographic order. Collection elements MUST be Queue, Var,
or another supported static collection with a valid fixed shape.

## ACIR operation contract

### Implemented common building blocks

The official graph-level catalog contains exactly these operations. Every
non-provisional design entry has both a typed gfsim realization and a PYC
realization; Table entries explicitly declare their gfsim-only boundary.

| Operation | Role | Queue arity | Static parameters | Core behavior |
| --- | --- | --- | --- | --- |
| `ac.source` | design | none to one | `depth`, `latency` | boundary producer |
| `ac.sink` | design | one to none | none | consuming boundary |
| `ac.observe` | observation | one to none | `name` | non-consuming, non-backpressuring probe |
| `ac.expect` | verification | one to none | `message` | non-consuming predicate check; PYC testbench only |
| `ac.transform` | design | one or more to one or more | output depths and latencies | pure Var region plus atomic Queue transfer |
| `ac.broadcast` | design | one to two or more | output depths and latencies | strict atomic fanout |
| `ac.fork` | design | one to two or more | output depths and latencies | decoupled exactly-once fanout |
| `ac.route` | design | one to two or more | output depths and latencies | selector-controlled demultiplexing |
| `ac.select` | design | one control plus two or more data inputs to one | `depth`, `latency` | selector-controlled data Queue mux |
| `ac.merge` | design | two or more to one | `policy`, `depth`, `latency` | priority or round-robin arbitration |
| `ac.barrier` | design | two or more to the same count | output depths and latencies | positionally typed atomic synchronization |
| `ac.credit` | design | one to one | `credits`, `depth`, `latency` | bounded parallel cost countdown and completion |
| `ac.memory.instance` / `ac.memory.request` | design | shared instance, one-to-one endpoint | instance identity, ordinal, `entries`, `init`, instance `latency`, `result_field`, `depth` | fixed-priority single-outstanding old-data memory |
| `ac.table` | design | state owner | Entry type, `entries`, `init`, owner, stable identity | committed zero-initialized state image; gfsim-only prototype |
| `ac.table.read` | design | optional request to one | Table identity, `depth`, `latency` | state- or Queue-driven old-data capture |
| `ac.table.write` | design | optional update to none | Table identity, `mode`, `write_fields` | Queue-driven consumption, state-driven field proposal, or scalar replace allocation |
| `ac.table.masked_write` | design | committed mask to none | Table identity, `mode="field"`, `write_fields` | atomic state-driven field update of every Entry selected by a same-Table match |
| `ac.table.propose` | internal rule IR | firing-local Var operands | Table identity, index, value, `mode`, `write_fields` | next-state intent committed only with the owning `ac.firing` Queue effects |
| `ac.table.match` / `ac.table.choose` | design | committed state to Vars | Table identity, `count=1`, `policy` | 1..64-entry candidate mask and deterministic first/min/max selection |
| `ac.slot` | design | one to none | owner, stable identity | one committed request with backpressure, retained payload, and explicit release |
| `ac.dependency` | design | one to one | `capacity`, `resources`, `no_dependency`, `depth`, `latency` | bounded predecessor tracking, resource reservation, and execution countdown |
| `ac.reorder` | design | one to one | `capacity`, `start`, `depth`, `latency` | bounded key-ordered retirement |
| `ac.feedback` | design | one to one | `depth`, `latency`, `max_iterations` | bounded stateful loop |
| `ac.scope` | design | variadic to variadic | symbol name | hierarchy boundary; PYC elaboration flattens it |

`ac.rule` and the three typed marker operations are transient pre-freeze IR.
Marker-free `ac.firing` is the internal transaction contract. These operations
are not independent QueueGraph building blocks; a proven pure firing becomes
`ac.transform` before QueueGraph extraction. The epoch 0.4
`ac.queue.peek/pop/push` operations are removed.

The closed inventory will grow with other common hardware blocks. New
application-specific opcodes and private provider identities are not an
extension mechanism.

### Transform example

The following excerpt is the canonical shape produced for a structure update:

```mlir
%output = ac.transform %input depths [2] latencies [1] {
^transform(%item: !ac.var<!ac.struct<@types::@Item>>):
  %value = ac.var.get %item field "value"
    : !ac.var<!ac.struct<@types::@Item>> -> !ac.var<i64>
  %one = ac.var.constant 1 : i64 as !ac.var<i64>
  %next_value = ac.var.add %value, %one : !ac.var<i64>
  %next = ac.var.with %item, %next_value field "value"
    : !ac.var<!ac.struct<@types::@Item>>, !ac.var<i64>
      -> !ac.var<!ac.struct<@types::@Item>>
  ac.transform.yield %next : !ac.var<!ac.struct<@types::@Item>>
} {ac.name = "output"}
  : (!ac.queue<!ac.struct<@types::@Item>>)
    -> !ac.queue<!ac.struct<@types::@Item>>
```

The region MUST have one Var block argument for each input Queue. All body
operations before `ac.transform.yield` MUST be pure. Yielded Var types MUST
match the payload types of the corresponding output Queues.

Input and output arity are independent. This two-input, one-output transform
consumes both heads and publishes the sum as one atomic transaction:

```mlir
%sum = ac.transform %left, %right depths [2] latencies [1] {
^transform(%left_item: !ac.var<i64>, %right_item: !ac.var<i64>):
  %value = ac.var.add %left_item, %right_item : !ac.var<i64>
  ac.transform.yield %value : !ac.var<i64>
} {ac.output_names = ["sum"]}
  : (!ac.queue<i64>, !ac.queue<i64>) -> !ac.queue<i64>
```

Neither backend may consume only one input or publish a partial output set.
The transform fires only when every input is valid and every output can accept
its corresponding result.

### Credit example

The Python credit call lowers to one stateful `ac.credit` and one pure cost
region.

```mlir
%completed = ac.credit %issued credits 2 depth 4 latency 1 cost {
^cost(%item: !ac.var<!ac.struct<@types::@CreditToken>>):
  %cycles = ac.var.get %item field "cycles"
    : !ac.var<!ac.struct<@types::@CreditToken>> -> !ac.var<i4>
  ac.credit.yield %cycles : !ac.var<i4>
} : !ac.queue<!ac.struct<@types::@CreditToken>>
    -> !ac.queue<!ac.struct<@types::@CreditToken>>
```

The cost region MUST contain one argument matching the Queue payload, contain
only pure Var operations, and terminate with exactly one `ac.credit.yield`.

### Barrier example

The barrier has no Var policy region. Its positional Queue types and static
output parameters completely define the operation.

```mlir
%left_ready, %right_ready = ac.barrier %left, %right
    depths [2, 2] latencies [1, 1]
    : (!ac.queue<i16>, !ac.queue<i32>)
      -> (!ac.queue<i16>, !ac.queue<i32>)
```

The input operands MUST be unique. Output depth and latency arrays MUST match
the output count and contain only positive values.

### Memory example

The declaration lowers to `ac.memory.instance`; every endpoint lowers to an
`ac.memory.request` with a frozen ordinal and three pure policy regions.

```mlir
ac.memory.instance @sram data i16 entries 16 init 0 latency 3
    owner "/" stable_id "memory/sram"
%response = ac.memory.request @sram, %request ordinal 0
    result_field "data" depth 4
    address {
  ^address(%item: !ac.var<!ac.struct<@types::@MemoryRequest>>):
    %address = ac.var.get %item field "address"
      : !ac.var<!ac.struct<@types::@MemoryRequest>> -> !ac.var<i4>
    ac.memory.yield %address : !ac.var<i4>
} write {
  ^write(%item: !ac.var<!ac.struct<@types::@MemoryRequest>>):
    %write = ac.var.get %item field "write"
      : !ac.var<!ac.struct<@types::@MemoryRequest>> -> !ac.var<i1>
    ac.memory.yield %write : !ac.var<i1>
} data {
  ^data(%item: !ac.var<!ac.struct<@types::@MemoryRequest>>):
    %data = ac.var.get %item field "data"
      : !ac.var<!ac.struct<@types::@MemoryRequest>> -> !ac.var<i16>
    ac.memory.yield %data : !ac.var<i16>
} {ac.endpoint_path = "/response", ac.name = "response"}
    : !ac.queue<!ac.struct<@types::@MemoryRequest>>
    -> !ac.queue<!ac.struct<@types::@MemoryRequest>>
```

The three regions MUST each contain one block argument matching the request
payload, contain only pure Var operations, and terminate with exactly one
`ac.memory.yield`.

An instance is visible only from its declaration scope and descendants. Its
endpoints use fixed ordinal priority. While one transaction is outstanding all
request endpoints are backpressured; `busy` is released only when the selected
response Queue accepts the response, and no request is reaccepted in that same
epoch. Every backend realizes exactly one physical memory per instance.

The Python frontend may statically elaborate a homogeneous memory array without
adding a frozen operation. `banks.select(requests, key=...).request(...)`
lowers to one `ac.route`, one ordinary memory instance and request per bank,
and one response `ac.merge`. The route key selects exactly one bank. Banks have
independent outstanding state, so responses from different banks may be
reordered; callers that require request order retain a tag and use `reorder`.
Memory arrays are one-dimensional in epoch 0.5 and require identical data type,
entry count, and initialization across all banks.

### Internal firing example

After marker discharge, every scheduling contract remains distinct on the
internal firing operation.

```mlir
%output = ac.firing %input depths [1] latencies [1]
    stable_id "top/increment" domain "cycle" guard "true" checks []
    handshake "ready_valid_1x1" schedule "independent"
    effects ["input.consume", "output.produce"] {
^body(%value: !ac.var<i32>):
  ac.firing.yield %value : !ac.var<i32>
} : (!ac.queue<i32>) -> !ac.queue<i32>
```

The stable identity, exact domain, functional guard, checks, handshake,
schedule, and effect summary are all mandatory. No typed marker may survive
into Frozen ACIR. Pure firings may become `ac.transform` only after the
canonicalization pass proves that this complete contract is preserved.

### Frozen logical identity

Every QueueGraph representation carries `ac.model_kind = "queue_graph"` and
the exact singleton-domain declaration `ac.queue_graph_domain = "cycle"`.
Every phase-one rule domain must equal that declaration. QueueGraph planning
and generation accept only verified epoch 0.5 frozen input with a matching
owner manifest and topology digest. Raw or forged models are rejected rather
than frozen implicitly by a backend.

The legacy flat representation additionally carries the string attribute
`ac.system`; it contains no structured `ac.system` or `ac.module*`
declarations and freezes with an empty owner manifest. The module-preserving
representation instead uses one selected `ac.system`, materialized
`ac.module` definitions, `ac.instance` placements, and module-local
`ac.scope` Queue graphs. The freeze pass computes, inserts, and verifies:

- one `ac.definition_fingerprint` for each reusable module definition;
- one `ac.specialization` fingerprint for the root and for every instance,
  derived from the definition fingerprint plus canonical static arguments;
- identical specialization identities for repeated instances of the same
  definition and argument set; and
- the selected system and elaborated instance-owner manifest in the global
  topology seal.

Backends must key generated implementation classes by specialization and bind
instances to independently owned ports and state. They must not flatten a
repeated module merely because its placements have different hierarchy paths.
For a stateful specialization, the implementation class owns the Table and
transition member layout, while each constructed instance owns a distinct
runtime Table object and dense object IDs. Reusing a specialization therefore
shares executable structure, never committed state or transaction ownership.
Within one specialization, multiple firing rules may bind different subsets of
the module's typed Queue interface while sharing one local state owner. Their
firing IDs and generated dispatch rows preserve frozen lexical priority. A
losing same-owner rule retains all of its inputs and recomputes from the next
committed snapshot; another module instance arbitrates against its own state,
not against the first instance.
One firing may also propose writes to multiple module-local state owners. The
specialization class stores the `QueueStateTransition` policy and member layout
once, while each instance constructs every Table independently. All selected
Queue effects and owner writes remain one prepare/publish/no-fail commit group;
failure to reserve any instance-local owner leaves every input and owner
unchanged.
Different rules in one module may touch different ordered subsets of the
module's owner set. Each generated transition receives only its own Queue and
owner subset, while the class constructs the union of all declared owners once
per instance. Lexical arbitration therefore remains rule-local without adding
unrelated Tables to a transaction.
Specializations may instantiate other specializations. Planning constructs the
acyclic specialization dependency graph in child-before-parent order. A parent
plan stores one direct child specialization body and instance bindings; codegen
emits the child class once, then the parent class, and recursively partitions
the parent's dense runtime-ID range across child instances. Repeating the parent
therefore repeats only object construction and bindings, not either class body.
A parent specialization may own internal Queues connecting local blocks to
child instances. Internal Queues belong to the parent instance's runtime-ID
interval and are constructed once per parent object; exported child results bind
directly to the parent interface. Internal storage is never promoted to the root
or shared between repeated parent instances.

Python authors declare reusable behavior as an ordinary typed `@ac.module`
function and invoke it with ordinary calls from `@ac.system`. The frontend does
not expose Queue ports, instance objects, specialization fingerprints, source,
sink, readiness, or backpressure. The first lowering slice accepts a pure 1x1
module whose expression return becomes a module-local transform; typed system
parameters and results become internal boundaries, and calls become
`ac.instance` placements.
A module expression return may directly call another typed module. The frontend
emits a parent `ac.module` containing a child `ac.instance`; existing
specialization planning and codegen preserve child-before-parent reuse. The
Python function still returns an ordinary value and names no hierarchy object.
The stateful module slice accepts one or more zero-initialized scalar lexical
variables, one serial assignment per variable, and a typed result expression:

```python
@ac.module
def accumulator(value: ac.u8) -> ac.u8:
    total: ac.u8 = 0
    total = total + value
    return total
```

All committed variables are read at activation start. Assignments update the
local immutable value environment in source order, then the raw IR records one
`ac.var.assign` proposal per owner. MLIR selects storage and infers one
Queue-plus-state atomic transaction; repeated calls reuse one implementation
class while each instance constructs independent committed state. Arbitrary
arity, conditional updates, shaped module state, static parameters, and
inferred repeated-value fanout remain follow-up slices.

Rule-backed modules reuse the same callable-body parser and QueueProgram event
renderer as systems. Such a module may expose multiple typed inputs and outputs
and invoke multiple ordinary rules; each rule still returns zero or one Queue.
Module arguments bind borrowed Queue values internally, and typed Python return
names become `ac.return` operands. A root system may place the module with an
ordinary tuple assignment. The compiler, not Python, materializes
`ac.instance`, interface bindings, specialization identity, and per-instance
state.

`reusable_circular_rob.py` exercises this path with one 3-input/2-output ROB,
five lexical state owners, four rules, and two independent placements. Its
specialization body and generated class occur once. Direct interface-to-rule
graphs are supported; arbitrary internal Queue graphs and repeated-input
fanout inside one module remain follow-up work.

For host-integrated simulation, compiler option `--host-results` preserves
typed system returns as Top module Queue results instead of inserting automatic
sinks. Generated `result_N()` accessors expose committed occupancy and
`try_take_result_N(system)` enrolls one dequeue in the external-Xfer frontier.
The same Python source is used in standalone and host modes; Python never names
the boundary Queue or sink.

Every Queue-producing operation MUST carry exact frozen logical output names
before QueueGraph extraction:

- one result uses non-empty `ac.name`;
- multiple results use exact `ac.output_names` in result order;
- names are unique across the system;
- each Queue records payload type, scope path, depth, and latency.

The flat canonical QueueGraph JSON uses schema
`agentic-circuit-queue-graph-plan`, version `0.5`. The module-preserving plan
extends this into definition-, specialization-, and instance-indexed records.
Its ordering and bytes MUST not depend on host addresses, hash iteration,
allocation order, or checkout path.

### Queue graph verification

A backend-ready Queue graph MUST satisfy:

- every Queue has exactly one producer;
- every Queue has exactly one consuming block;
- `ac.observe` does not count as a consuming block;
- fanout is represented by `ac.broadcast` or `ac.fork`;
- merge is represented by `ac.merge`, not multiple producers on one Queue;
- key-ordered retirement is represented by bounded `ac.reorder` state;
- every Queue depth and latency is positive;
- Queue logical identities are non-empty and unique;
- every block input and output references a known Queue identity;
- raw QueueGraph cycles are rejected; a stateful loop MUST use `ac.feedback`;
- every Var region uses only supported pure operations and structured yields;
- topology and collection shape are compile-time fixed.

An otherwise unused Queue is a static no-progress risk and is rejected with an
actionable `connect ac.sink` diagnostic. The same verifier runs after ACIR
extraction and again at both backend entry points, so hand-constructed plans
cannot bypass the producer, consumer, reference, or cycle checks.

## Runtime execution contract

### Snapshot, proposal, arbitration, and Xfer

At one active epoch, gfsim follows this state discipline:

1. blocks read committed Queue state;
2. blocks propose pushes and pops without publishing them;
3. Queue-local arbitration resolves deterministic FIFO proposals;
4. Xfer commits accepted changes;
5. consumers observe committed results no earlier than the required later
   epoch.

One block MUST NOT observe another block's uncommitted proposal in the same
epoch. Independent Work order MUST NOT change architectural results,
diagnostics, committed statistics, or refinement observations.

### Incremental activation

QueueGraph plans derive activation from typed Queue endpoints and Table
footprints. A committed input/output Queue or semantically changed referenced
Table wakes every subscribed block at the next epoch. A Table proposal that
commits the same final values still completes its atomic transaction, progress,
and observation bookkeeping, but it does not propagate activation. A separate
Work-closure relation adds all consumed/produced Queues and writable Tables to
that block's same-epoch Arbitrate/Probe/Commit barrier without invoking their
no-op Work methods.

For rule-backed blocks, `ac-infer-rule-activation` freezes enum-typed input,
output, and state resource records after `ACDataFlowAnalyzer` has produced
state footprints. Firing verification re-derives those records before
QueueGraph extraction. Generated system inputs expose `offer_<name>(system,
value)`, which schedules the external Queue proposal without adding any
readiness or scheduling operation to Python authoring.

The runtime evaluates Work blocks first, arbitrates those owners before
closure-only resources, probes the complete closure, and only then begins the
Commit loop. External Queue offers use a distinct Xfer frontier rather than
pretending the Queue is a Work block.
Host-result dequeue uses the same frontier. A returned value is only accepted
after the following system step commits its Queue pop; that commit wakes any
producer stalled by the previously full result Queue.

The validated runtime profile records each committed ObjectId, epoch, and
semantic-change bit after the global Probe barrier. This commit timeline is an
equivalence/debugging surface; the fast profile does not allocate or append
these records on the commit hot path.

Zero-input rules receive one initial activation and then sleep until a
referenced owner changes or an output dequeue removes backpressure. Extracted
activation edges and the initial frontier are canonical compiler evidence;
backends MUST NOT guess them from generated C++ text. Physical binding recurses
through the supported specialization hierarchy, mapping borrowed interfaces,
parent-local Queues, blocks, Tables, and child ObjectId intervals without
flattening classes. Generated `activation_complete()` states whether the whole
reachable hierarchy is covered.

### Capacity and latency

`SimQueue<T>` counts committed entries, delayed entries, and push proposals
against capacity. It rejects a push proposal that would exceed entry or byte
capacity.

Latency is exact and positive. A token accepted at epoch `t` by a Queue with
latency `L` becomes visible to downstream committed-state reads no earlier than
the boundary corresponding to `t + L`. Latency one is still stateful; it is not
a combinational wire.

### Atomic transfer

A transform fires only when all required input pops and output pushes can be
proposed. It computes output Vars from immutable input heads, proposes every
output, proposes every input pop, and commits the complete transaction through
Xfer.

No legal lowering may commit an input pop while a required output push is
rejected.

For the current single-condition, zero-or-one-output rule subset, path
selection is explicit SSA evidence rather than only a summary category.
`ac.rule.output` and `ac.firing.output` bind each returned value and ordinal to
one `!ac.var<i1>` presence value, and each firing-local `ac.table.propose`
carries its presence value through storage selection. Rule and Firing
verification independently require exactly one candidate condition, complete
output ordinal coverage, returned-value identity, and each effect presence to
imply the candidate. The narrow conditional-effect form allows presence to
differ only when the candidate is constant true, there is exactly one input,
and all differing effects share one predicate. QueueGraph retains candidate and
effect presence separately. Generated gfsim distinguishes `nullopt` (stall and
retain input) from an engaged plan with absent writes (consume input without a
state commit). It reserves analyzer-derived snapshot indices long enough to
validate the committed decision against overlapping lexical writers, then
cancels unselected reservations without publishing a Table proposal. Snapshot
readers remain mutually compatible; snapshot/write overlap conflicts, while
disjoint indices proceed independently. Python exposes
none of these proof or reservation operations. General predicate read-set
inference for candidate/output and match/choose index sets, CFG joins, and
multiple selected outputs remain outside this subset.

### Credit transfer

Credit admission pops one input token into a free committed slot. Active slots
decrement at Xfer, and a zero-remaining slot may propose one output token. The
slot becomes free only when that output push commits. Output backpressure keeps
the completed token and its credit occupied.

The gfsim and PYC implementations use the same lowest-slot tie break, the same
one-admission/one-completion bandwidth, and the same no-same-Xfer slot reuse
rule. Refinement compares accepted and completed transactions and derives the
active credit count from their committed difference.

### Barrier transfer

A barrier reads every committed input head, checks every output proposal slot,
and proposes the complete positional transfer. If any precondition fails, it
proposes no pop or push. Xfer commits all accepted Queue effects together.

gfsim implements this as `QueueBarrier<std::tuple<Ts...>>`. PYC implements the
same contract with an all-input-valid conjunction, per-input ready gating, and
per-output valid gating; no backend retains a dynamic Queue pointer.

### Memory transfer

Memory reads observe committed state before the current Xfer. The memory block
stages an optional write only after its response push and request pop are both
accepted. Xfer commits the staged write and the Queue effects together. A
rejected response push MUST leave the request and memory unchanged.

Model construction and replay start from the deterministic zero image.
gfsim `reset()` restores that image so the same model instance can replay
deterministically. The raw PYC primitive does not clear memory on its reset
port, so mid-run reset of memory contents is outside the shared refinement
contract; a PYC replay MUST instantiate a fresh model.

### Table transfer

`SimTable<Entry>` exposes only committed state to read and value policies. A
Queue-driven read proposes its input pop and output push together. A
state-driven read may capture once per tick while `when` is true and the output
has capacity. A Queue-driven writer consumes a disabled token without a
proposal; an enabled endpoint proposes one complete value plus its declared
field set. An output Queue
owns its captured Entry, so later writes cannot change a backpressured output.
For a masked endpoint, enable is evaluated first; when enabled, the match mask
and every selected value are evaluated from the old committed image. The whole
set is staged before transfer. All enabled endpoint proposals are evaluated
from the same committed image, validated as one batch, merged by disjoint
fields, and published together. A zero mask transfers as a no-op.
Shared match and choose caches are evaluated on first demand and reused by all
endpoint policies in the same Epoch; they only inspect committed state and are
invalidated by model reset.

## Typed gfsim C++ lowering

The C++ backend MUST generate statically typed, queue-wired code.

```cpp
struct WorkItem {
  std::uint32_t value;
  std::uint8_t route;
  std::uint16_t remaining;
};

gfsim::SimQueue<WorkItem> input_queue_;
gfsim::SimQueue<WorkItem> output_queue_;
```

The generated system owns interconnect Queues. Child scope modules and common
blocks borrow typed Queue references. Sibling blocks MUST NOT own duplicate
instances of the same interconnect.

The implementation currently provides reusable templates for transform,
atomic transform, sink, observe, broadcast, fork, route, merge, barrier, credit,
memory, dependency, reorder, and feedback.
Generated dispatch is static; generated runtime code MUST NOT discover opcodes
by strings, walk schemas, construct topology dynamically, or depend on Python
or MLIR libraries.

## PYC and Verilog lowering

Agentic Circuit owns `frozen ACIR -> canonical PYC IR`. A pinned external
`pycc` owns PYC verification, C++ emission, and Verilog emission. The pin is
recorded in `pyc.lock.json`.

The current hardware lowering maps:

| ACIR concept | PYC/RTL realization |
| --- | --- |
| scalar or structure Var | combinational value or packed bundle |
| Queue | valid/data/ready channel with fixed storage |
| Queue depth and latency | fixed register/FIFO stages |
| transform | combinational data logic plus atomic handshakes |
| broadcast | all-output ready conjunction |
| fork | delivered-mask registers and independent output handshakes |
| route | selector decoder, valid demultiplexing, ready multiplexing |
| select | selector mux, selected-input ready, and control/data atomic handshake |
| priority merge | fixed-priority selection |
| round-robin merge | selection plus committed cursor register |
| barrier | all-input-valid/all-output-ready atomic handshake |
| credit | fixed slot register bank, parallel countdown, and deterministic completion selection |
| memory | `pyc.sync_mem` plus an aligned pending request/valid register |
| dependency | fixed register window, predecessor wakeup, countdown, and resource grants |
| reorder | fixed register window and committed next-key register |
| bounded feedback | committed valid/data/iteration registers and limit assertion |
| scope | static module hierarchy |
| observe | non-functional probe boundary |
| expect | rejected in design hierarchy; permitted only at the PYC testbench boundary |

PYC C++ and Verilog generated from the same PYC IR MUST be cycle equivalent.
Memory uses the PYC synchronous 1R1W primitive. The lowering retains the
request until its registered read data is aligned and accepted, drives writes
only when the request handshake fires, and enables every byte lane of the
integer data word. The primitive's read-during-write rule is old data.
Feedback uses explicit sequential state in PYC IR; it is not a combinational
unroll or a backend-specific loop.

Before lowering, role placement is checked against the shared opcode catalog.
A verification-only leaf in the design graph fails deterministically; an
observation leaf remains non-state-changing and cannot affect ready/valid.

## Cross-backend refinement

Typed gfsim and PYC/Verilog have different internal IR and may have different
internal cycle structures. Cross-backend validation compares a declared
semantic projection, including:

- input transaction sequence;
- accepted and completed transaction identities;
- output transaction sequence;
- architectural state and memory-visible effects when present;
- declared assertions and runtime failures.

Cross-backend refinement does not require equality of:

- internal Queue implementation;
- gfsim deltas;
- PYC registers and wires;
- every internal stage cycle;
- abstract versus detailed pipeline latency that is outside the declared
  observation contract.

## End-to-end example

The executable
`davincioo_queue_model.py`
uses only serial Python and common building blocks.

```python
import agentic_circuit as ac


@ac.struct
class WorkItem:
    value: int
    route: int
    remaining: int


@ac.system
def davincioo_queue_model() -> None:
    trace = ac.source(WorkItem, depth=8, latency=1)

    with ac.scope("frontend"):
        prepared = trace.apply(
            lambda item: item.with_fields(value=item.value + 1),
            depth=4,
            latency=1,
        )

    with ac.scope("dispatch"):
        scalar, vector, cube, tma = prepared.route(
            outputs=4,
            key=lambda item: item.route,
            depth=2,
            latency=1,
        )

    scalar_done = scalar.apply(lambda item: item.with_fields(value=item.value + 1))
    vector_done = vector.apply(lambda item: item.with_fields(value=item.value + 2))
    cube_done = cube.apply(lambda item: item.with_fields(value=item.value + 3))
    tma_done = tma.apply(lambda item: item.with_fields(value=item.value + 4))

    completed = scalar_done.merge(
        vector_done,
        cube_done,
        tma_done,
        policy="round_robin",
        depth=8,
        latency=1,
    )

    ac.sink(completed)
```

The checked-in executable adds explicit engine and retirement scopes. The
inferred graph is:

```text
trace -> frontend transform -> four-way route
                                  | scalar engine
                                  | vector engine
                                  | cube engine
                                  | tma engine
                             round-robin merge -> retire -> sink
```

### Generate canonical ACIR, QueueGraph JSON, and gfsim C++

Configure and build the native tools first:

```sh
scripts/bootstrap-dev.sh
cmake --preset dev-llvm22
cmake --build --preset dev-llvm22
```

Generate all canonical Queue artifacts:

```sh
PYTHONPATH=src .venv/bin/python tools/ac-queue-cxxgen.py \
  examples/pipelines/davincioo_queue_model.py \
  --system davincioo_queue_model \
  --acir-output build/davincioo_queue_model.ac.mlir \
  --plan-output build/davincioo_queue_model.queue-plan.json \
  --acir-opt build/dev-llvm22/bin/acir-opt \
  --queue-plan-tool build/dev-llvm22/bin/acir-queue-plan \
  --queue-cxxgen-tool build/dev-llvm22/bin/acir-queue-cxxgen \
  --output build/davincioo_queue_model.cpp
```

Check that the generated C++ is valid for the local compiler:

```sh
c++ -std=c++20 -I include -fsyntax-only build/davincioo_queue_model.cpp
```

### Generate PYC, PYC C++, and Verilog

Use the exact pyCircuit commit and LLVM version in the toolchain lock. Given a
matching local pyCircuit installation, run the canonical bundle command:

```sh
PYC_TOOLCHAIN_ROOT=/path/to/pycircuit/toolchain/install

.venv/bin/python tools/ac-queue-pyc-build.py \
  build/davincioo_queue_model.ac.mlir \
  --pycgen-tool build/dev-llvm22/bin/acir-queue-pycgen \
  --pycc "$PYC_TOOLCHAIN_ROOT/bin/pycc" \
  --toolchain-lock toolchains/pyc.lock.json \
  --toolchain-metadata \
    "$PYC_TOOLCHAIN_ROOT/share/pycircuit/toolchain-metadata.json" \
  --cxx "$(command -v c++)" \
  --verilator "$(command -v verilator)" \
  --pyc-output build/davincioo_queue_model.pyc \
  --cpp-output-dir build/davincioo_queue_model-pyc-cpp \
  --verilog-output-dir build/davincioo_queue_model-verilog \
  --manifest build/davincioo_queue_model-pyc-manifest.json
```

The command validates the toolchain lock, emits PYC C++ and Verilog, runs C++
syntax checking and Verilator lint, and records deterministic artifact hashes.
Output paths MUST not already exist.

## Rejected examples

### Explicit system ports

```python
@ac.system
def illegal(input_queue):
    ...
```

Queue/Var system boundaries are inferred. A system with parameters is rejected.

### Zero-latency Queue

```python
incoming = ac.source(int, latency=0)
```

Use Var computation inside a lambda for latency-zero logic.

### Runtime topology

```python
for _ in range(item.value):
    queues.append(ac.source(int))
```

Queue count and collection shape MUST be known during AST elaboration.

### Runtime Queue condition

```python
if incoming:
    selected = incoming.apply(lambda item: item + 1)
```

Use `route` for runtime token selection.

### Dynamic Queue handle

```python
selected_queue = queues[item.route]
```

Runtime selection MUST lower to `route`, select, or arbitration logic. It MUST
NOT materialize a runtime Queue pointer.

### Private opcode or backend

```python
@ac.opcode
def private_scheduler(...):
    ...

ac.raw_verilog("assign ...")
```

Both forms are forbidden. Add a reusable common building block to the
repository-owned inventory with ACIR, gfsim, PYC, and conformance definitions.

## Diagnostics

Frontend Queue diagnostics use the `ACPY-QUEUE-*` family. They SHOULD identify
the source construct, violated static rule, and repair. Important current codes
include:

| Code | Meaning |
| --- | --- |
| `ACPY-QUEUE-001` | invalid system, assignment, statement, or positive constant |
| `ACPY-QUEUE-002` | unsupported payload or structure declaration |
| `ACPY-QUEUE-003` | invalid lambda or Var expression |
| `ACPY-QUEUE-004` | duplicate scope path |
| `ACPY-QUEUE-005` | invalid static collection or reference |
| `ACPY-QUEUE-006` | invalid route declaration |
| `ACPY-QUEUE-007` | invalid bounded feedback loop |
| `ACPY-QUEUE-008` | invalid merge |
| `ACPY-QUEUE-010` | forbidden user opcode or backend provider |
| `ACPY-QUEUE-011` | runtime `if` is not a symmetric Boolean Queue branch |
| `ACPY-QUEUE-012` | invalid fork |

Rule diagnostics use `ACPY-RULE-001` through `ACPY-RULE-005` for invalid rule
definitions, unsupported control flow, invalid Queue invocation, result-type
mismatch, and removed epoch 0.4 `atomic`/`.firing()` spellings respectively.

Native QueueGraph/backend diagnostics use the `ACLOWER-QUEUE-*` family and
MUST reject an invalid graph before emitting partial backend artifacts.

## Determinism requirements

Canonical ACIR, QueueGraph JSON, generated C++, PYC IR, manifests, and
observations MUST NOT depend on:

- Python hash iteration order;
- host pointer values or allocation order;
- unordered C++ traversal order;
- ambient checkout path;
- process ID or wall-clock time;
- runtime plugin discovery;
- arbitrary Python or Verilog execution.

Canonical ordering uses source occurrence, static collection order, frozen
logical identity, and declared arbitration policy.

## Current implementation boundary

The following slices are implemented and tested:

- AST-based serial Python capture;
- immutable scalar and structure payloads;
- Queue/Var types and pure Var expressions;
- scopes with inferred Queue boundaries;
- transform, strict broadcast, decoupled fork, route, merge, atomic barrier,
  bounded credit, typed memory, dependency, reorder, observe, sink, explicit
  atomic transform, and bounded feedback;
- static arrays, maps, sets, runtime flat-collection selection, static `if`,
  static loops, and symmetric runtime Queue `if` lowering through
  route/transform/merge;
- canonical QueueGraph extraction;
- typed gfsim C++ generation, including the provisional one-dimensional Table;
- PYC/Verilog lowering for transform, broadcast, fork, route, select, merge,
  atomic barrier, bounded credit, typed synchronous memory, dependency,
  reorder, bounded feedback, elaboration-time scope flattening, packed
  structures, atomic handshakes, and exact Queue latency;
- stable PYC rejection of provisional Table graphs;
- PYC C++ versus Verilog cycle equivalence and gfsim/PYC projected transaction
  comparison.

Issues [#9](https://github.com/PTO-ISA/agentic-circuit/issues/9),
[#10](https://github.com/PTO-ISA/agentic-circuit/issues/10), and
[#11](https://github.com/PTO-ISA/agentic-circuit/issues/11) are closed. The
requirement matrix contains no partial or missing rows. Future public
building blocks, richer signedness semantics, and additional refinement
projections are new contract work rather than incomplete requirements of these
issues.

The checked-in DavinciOO-like model now proves topology, typed payloads, finite
Queues, backpressure, deterministic C++ generation, the 15-record softmax
opcode/completion/retirement projection, and the 453-cycle bounded oracle. The
same frozen ACIR produces PYC C++ and Verilog with cycle-identical hardware
observations and the same projected output transactions. Dependency readiness
resource reservation, and execution countdown are now explicit `ac.dependency`
state, while bounded parallel in-flight work is explicit `ac.credit` state. The
projection
retains only a fixed 5-cycle ingress and 4-cycle drain compensation for the
different model boundaries. The checked occupancy projection records dependency
window peak 8, per-resource executing peaks `[1, 1, 0, 1]`, and reorder window
peak 8 through stable generated-model accessors.

## Contributor checklist

A change to the public contract is complete only when it updates all
affected layers:

- Python accepted and rejected syntax;
- ACIR ODS type or operation definition;
- verifier and diagnostic;
- QueueGraph canonical plan;
- gfsim runtime semantics and typed C++ emission;
- PYC lowering or an explicit backend rejection;
- positive, negative, determinism, and round-trip tests;
- this manual and the relevant machine-readable schema;
- exact contract epoch and capability declarations when public syntax changes.

Do not document a backend-specific behavior as shared ACIR semantics. Do not
add a compatibility alias for removed public source surfaces. Git history,
release tags, and [`REF-HISTORY-001`](refs/history.md) preserve prior contracts.
