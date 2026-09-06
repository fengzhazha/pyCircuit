# Queue/Var DavinciOO-like model

The [Agentic Circuit Specification Manual](../../../docs/acir/spec/agentic-circuit.md)
defines the authoring, ACIR, runtime, backend, and refinement contracts exercised
by these examples.

`bitfield_decode_pipeline.py` demonstrates a static 32-bit `BitfieldSpec` with
overlapping read views, u3/u5/u17 fields, MSB-first multi-field selection, and
an immutable two-field update. The frontend emits only verified
`ac.var.extract`, `ac.var.concat`, and `ac.var.insert` operations; Queue and
backpressure mechanics remain compiler-owned.
`bitfield_scalar_pipeline.py` keeps the boundary at 32 bits so the same named
extract/concat/insert behavior can be compared directly in generated C++ and
Verilog simulations.
`nested_payload_pipeline.py` defines its outer nominal struct before the nested
struct, proving descriptor resolution and generated C++ declaration order are
dependency-driven rather than source-order dependent. Its rule performs an
immutable nested field replacement; Queue checks remain compiler-inferred.
`enum_payload_pipeline.py` uses the standard Python `enum.Enum` class rather
than an Agentic hardware constructor. Declaration-order ordinals become the
explicit nominal encoding; nested equality and immutable replacement lower
through verified ACIR and the shared gfsim/PYC backends.
`aggregate_payload_pipeline.py` uses ordinary typed tuples and `ac.array[N, T]`
value annotations. Tuple/list literals construct immutable aggregate values,
constant indexing stays structural, and the compiler packs the 28-bit payload
without exposing Queue operations or hardware container classes in Python.
`recursive_aggregate_payload_pipeline.py` places standard Python enum and
nominal struct values inside those aggregates. The same compiler-owned layout
recursively packs and restores the nominal values in gfsim, PYC C++, and
Verilog.
`masked_decode_pipeline.py` uses the pure `ac.matches(value, "1xx0")`
intrinsic for an MSB-first four-bit decode. Python supplies only the compact
lowercase `0`/`1`/`x` pattern; ACIR verifies its canonical mask/value operation
and the backends implement the same boolean result without exposing decode
hardware or transaction checks in the frontend.

`davincioo_queue_model.py` is the first executable topology generated
from serial Python. It uses only repository-owned common building blocks:
`ac.source`, `ac.transform`, `ac.dependency`, `ac.route`, `ac.merge`,
`ac.observe`, `ac.reorder`, and `ac.sink`, connected by typed `ac.queue` values.

Generate one canonical typed C++ model:

```bash
PYTHONPATH=src tools/ac-queue-cxxgen.py \
  examples/pipelines/davincioo_queue_model.py \
  --system davincioo_queue_model \
  --acir-output build/davincioo_queue_model.ac.mlir \
  --plan-output build/davincioo_queue_model.queue-plan.json \
  --acir-opt build/dev-llvm22/bin/acir-opt \
  --queue-plan-tool build/dev-llvm22/bin/acir-queue-plan \
  --queue-cxxgen-tool build/dev-llvm22/bin/acir-queue-cxxgen \
  -o build/davincioo_queue_model.cpp
```

The generated class owns every interconnect as a typed `gfsim::SimQueue<T>`.
Lexical scopes become `gfsim::Module` hierarchy nodes. Engine blocks borrow
Queue references; they do not allocate or own sibling interconnect.

The example models the reference shape at building-block level:

```text
trace -> frontend -> dependency window -> 4-way dispatch
                                              | scalar
                                              | vector
                                              | cube
                                              | tma
                                         merge -> reorder -> retire -> sink
```

The checked-in
[`softmax-projection.json`](../../../tests/goldens/agentic-circuit/davincioo/softmax-projection.json) binds
this generated topology to the provenance-locked 15-record softmax trace. It
records opcode identities, engine routes, reference execution costs, explicit
predecessors, fixed boundary-cycle compensation, out-of-order completion order,
in-order retirement, architectural values, and the 453-cycle oracle.

The generated model uses the official bounded `ac.dependency` window with four
reserved resource classes, round-robin merge, committed observations, and the
official `ac.reorder` block. The same serial Python and frozen ACIR now pass all
of these gates:

- typed gfsim consumes all 15 projected records and finishes in 453 cycles;
- opcode counts and completion/retirement order match the reference projection;
- copied-source generation remains byte-identical across unrelated roots;
- the same frozen ACIR builds with pinned `pycc` as PYC C++ and Verilog;
- PYC C++ and Verilator produce cycle-identical ready/valid/data observations;
- gfsim, PYC C++, and Verilog produce the same projected output transactions.

Dependency wait is no longer folded into token latency. `ac.dependency` tracks
predecessor completion explicitly and counts the reference execution cost. The
projection applies only a documented 5-cycle ingress and 4-cycle drain
compensation for the different Queue boundaries. It checks dependency-window
peak occupancy, per-resource executing peaks, and reorder-window peak occupancy
through stable generated-model accessors; raw reference rename-table structure
remains outside the declared observation projection.

## PYC and Verilog slice

`pyc_queue_pipeline.py` exercises the initial scalar hardware lowering. Generate
frozen ACIR first, then run `acir-queue-pycgen` or the bundled
`tools/ac-queue-pyc-build.py` command. The bundle command validates the pinned
toolchain lock, invokes external `pycc` for C++ and Verilog, compiles the C++
source, runs Verilator lint, and writes a canonical hash manifest.

The repo-local pyCircuit 6 toolchain contract is recorded in
`toolchains/agentic-circuit/pyc.lock.json`. Build it with the repository's
pinned LLVM 22 toolchain before running the PYC gate.

`pyc_struct_pipeline.py` verifies deterministic packed struct layout.
`pyc_route_merge_pipeline.py` verifies static selector demux and priority merge
logic, including forward valid and backward ready paths.
`pyc_select_pipeline.py` verifies runtime selection from a statically shaped
Queue collection without dynamic Queue pointers.
`pyc_rule_pair_pipeline.py` verifies two independent rules without introducing
an implicit global transaction or priority.
`pyc_multi_input_rule_pipeline.py` verifies that one serial `@ac.rule` consumes
two Queue tokens and produces one result atomically, while MLIR supplies the
input-availability, output-backpressure, and commit-group mechanics.
`inferred_boundary_pipeline.py` verifies that ordinary typed system parameters
and returns are enough for MLIR to create the source/sink boundaries.
`inferred_module_pipeline.py` defines a pure typed `@ac.module` and invokes it
twice with ordinary Python calls; MLIR creates the structured instances and
gfsim emits one reusable specialization class.
`inferred_nested_module_pipeline.py` returns one module call from another module
and proves the child and wrapper classes are each emitted once.
`pyc_rule_pipeline.py` verifies that the simple `@ac.rule` surface lowers
through typed obligations and internal firing IR to the standard transform.
`gfsim_expect_pipeline.py` verifies a verification-role leaf executes in gfsim
and is rejected from PYC design hierarchy with testbench-boundary guidance.
`pyc_fork_pipeline.py` verifies decoupled fanout with per-output delivered state.
`pyc_conditional_pipeline.py` verifies that a serial runtime `if` becomes an
official route, two branch transforms, and a mutually exclusive priority merge.
`pyc_feedback_pipeline.py` verifies a bounded serial `while` as sequential
feedback data, valid, and iteration state shared by PYC C++ and Verilog.
`pyc_loop_control_pipeline.py` verifies a leading runtime `break` and tail
`continue` normalize to explicit bounded feedback conditions.
`pyc_recursive_pipeline.py` verifies bounded compile-time recursion expands to
a frozen three-stage Queue chain before ACIR publication.
`pyc_reorder_pipeline.py` verifies bounded key-ordered retirement with the same
register-bank and handshake semantics in typed gfsim, PYC C++, and Verilog.
`pyc_dependency_pipeline.py` verifies predecessor wakeup, execution countdown,
out-of-order completion, and PYC C++/Verilator cycle equivalence.
`pyc_barrier_pipeline.py` verifies heterogeneous positional payloads and an
all-input/all-output atomic synchronization firing shared by typed gfsim, PYC
C++, and Verilog.
`pyc_credit_pipeline.py` verifies bounded parallel in-flight slots, independent
cost countdown, deterministic completion selection, automatic credit return,
and PYC C++/Verilator cycle equivalence.
`pyc_memory_pipeline.py` verifies an explicit typed memory instance, old-data
read-during-write behavior, aligned request/response state, and exactly one
`pyc.sync_mem` realization per instance in PYC C++ and Verilog.
