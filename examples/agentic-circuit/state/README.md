# Stateful Issue Table

`issue.py` is the stateful Table example in this directory. It combines:

- two field-disjoint operand wakeup writers;
- minimum-age selection and grant-driven read;
- scalar `valid` clear after issue;
- explicit empty-slot match/choose followed by complete Entry allocation;
- allocation backpressure while no old-state empty slot is available.

All Table expressions observe the old committed image. Consequently a wakeup
becomes selectable on the following tick, and a slot cleared by issue becomes
visible to the empty-slot selector on the following tick. Allocation remains a
state-driven scalar endpoint and replaces the complete selected Entry. The
example intentionally updates only entries resident when a wakeup is consumed.
It does not persist global tag-ready state or re-query later allocations, so it
is not the lost-wakeup solution tracked by issue #11.

The checked multi-writer Frozen ACIR fixture remains
`table_multi_writer_issue.mlir`. Additional Python inputs used only for E2E
regression coverage live under `tests/e2e/fixtures/table_examples/` and are not
public examples.

`rob.py` is the first epoch 0.5 rule-lowering example. It is intentionally a
bounded, non-wrapping retirement demo rather than a complete circular ROB: a
pure `@ac.rule` marks incoming entries done, and `ac.reorder` retires sequence
numbers in order. The example exercises typed obligations, internal
`ac.firing`, proof-driven `ac.transform` canonicalization, QueueGraph C++
generation, and gfsim execution. Allocation/completion Table rules and wrapped
sequence arithmetic remain follow-up stateful slices.

`table_multi_input_rule.py` extends the inferred state transition to
heterogeneous Queue inputs. One Table replacement, every input consumption,
and the returned old Entry share the compiler-generated
`ready_valid_Nx1_table` commit group. Python still contains no Queue readiness,
pop, push, reservation, or commit operations.

`variable_accumulator.py` is the first generic persistent-field example. A
normal annotated lexical variable and ordinary assignment lower through
`ac.var.decl/read/assign`; MLIR then selects committed storage before rule and
gfsim lowering. Python names no register or Table primitive.

`branch_local_state.py` uses one plain Python `if/else` to select exactly one of
two lexical state owners. MLIR proves complementary SSA presence, and generated
gfsim commits the input plus only the selected owner in one transaction.

`branch_join_state.py` assigns the same scalar lexical owner in both arms.
Compiler-owned `ac.var.select` joins the values before storage selection, so
the frozen plan contains one owner write; gfsim uses a ternary and PYC uses the
same `pyc.mux` semantics.

`indexed_branch_join.py` extends the same rule to one persistent list. The
compiler joins both branch indices and both values, preserving one dynamic
Table proposal; the unselected index receives no write.

`optional_output_state.py` always increments lexical state but returns its input
only when a normal Python condition is true. MLIR qualifies output-capacity
checks with SSA presence: absent output ignores backpressure, while present
output stalls input and state together.

`inferred_stateful_module.py` moves the same variable contract inside a normal
typed `@ac.module`. Two ordinary calls reuse one generated specialization class
while constructing independent persistent state for each instance. Python
still names only the lexical `total` variable; MLIR inserts storage, handshake,
backpressure, and the atomic Queue-plus-state transaction.

`inferred_multi_state_module.py` extends that module to correlated `count` and
`total` variables. MLIR retains both proposals in one multi-owner commit group;
an output-full stall cannot consume the input or update either value in
isolation.

`indexed_variable_array.py` extends the same family to a normal persistent
Python `list`. MLIR sees shaped `ac.var` element reads and assignments, then
selects the committed indexed storage without exposing Table syntax in Python.

`shared_indexed_rules.py` has two lexical rule instances targeting that same
list. MLIR emits ordered state footprints; gfsim evaluates both candidates in
Work and performs conflicting reservation/publication only in stable Arbitrate
order.

`consume_only_completion.py` demonstrates a state update with no returned
value. A normal early `return` discards a stale completion: MLIR infers a
constant-true input candidate and a separate state-write presence. The input
commits even when the write is absent, while a fresh completion reserves and
updates the indexed state without a dummy output or sink.

`state_driven_retire.py` demonstrates a zero-input rule guarded by committed
state. The inferred output-capacity check is part of the same transaction as
clearing the entry, so output backpressure preserves the old state.

`multi_state_allocate.py` demonstrates one rule atomically advancing a scalar
tail and updating a dynamically indexed entry list. Both logical variables
remain `ac.var` until MLIR selects heterogeneous state owners and one generated
multi-state commit group.

`circular_rob.py` composes the completed rule flow into a four-entry circular
ROB. It has typed flush/allocation/completion inputs and allocation/retirement
results, while ordinary scalar/list variables hold head, tail, occupancy,
recovery epoch, and per-slot generation. Python contains no explicit Queue,
Table, source/sink, ready/full, pop/push, reservation, or commit mechanics.

`reusable_circular_rob.py` moves the same recover/allocate/complete/retire flow
behind an ordinary typed 3-input/2-output `@ac.module`. The root places it twice
with tuple assignment. Frozen ACIR and generated C++ retain one specialization
body/class, while each placement owns independent head, tail, count, epoch, and
entry state. Completion uses a serial two-step early-return guard chain for
generation and epoch rejection: stale tags consume without state commit, while
an overlapping same-epoch allocation forces snapshot revalidation before the
completion is classified.
`ACDataFlowAnalyzer` derives exact reservations for the entry index and scalar
epoch read, so the Python rule contains no dummy `epoch = epoch` write.
The same proof records only the `generation` and `epoch` Entry fields consulted
by completion; unrelated Entry fields are not part of its snapshot conflict
set.

`reusable_oldest_ready_isq.py` uses the same Pythonic state contract for a
four-entry issue queue. `ac.find(entries, where=..., key=...)` is an algorithmic
query over an ordinary persistent `list`, returning `valid`, `index`, and
`value`; it is not a Queue or Table object. MLIR lowers the generic
`ac.var.match/choose` query only after storage selection. A separate persistent
boolean list tracks source-tag readiness, including false updates on tag reuse.
The state-driven issue rule reads that list without adding it to the atomic
write closure. `ACDataFlowAnalyzer` derives an `ac.state.snapshot_set` from the
readiness indices actually evaluated by the entry match; generated gfsim builds
that set in the original scan rather than scanning readiness a second time.
Readiness changes therefore trigger a new oldest-ready query without bulk
rewriting every resident entry. Two placements share one generated class and
own independent entries and readiness state.

The same inference applies when a normal `ac.find(..., key=...)` lambda indexes
another persistent list. The choose index is compiler-owned evaluation
provenance, and only foreign indices read for candidate keys join the snapshot
reservation mask.
