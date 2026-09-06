# Bounded constraints and AC dataflow analysis gate

Decision 0217 adds one bounded value-domain contract shared by Python static
evaluation and MLIR verification. The frontend remains ordinary Python and
does not expose constraint, Queue transaction, or range markers.

## Evidence

- The semantic core provides typed `Constant`, `FiniteSet`, `ClosedInterval`,
  and `Unknown` facts with deterministic caps and exact-width bit transfers.
- `ac.var` remains the only variable family; `agentic_circuit.variable` is not
  a public alias or constructor.
- `ACDataFlowAnalyzer` is the public C++ analysis API. MLIR `DataFlowSolver`
  appears only in the analyzer's private implementation.
- `ac-verify-value-constraints` proves dynamic persistent-list and Table
  indices before rule lowering and freeze; QueueGraph independently recomputes
  the obligation from the expression plan.
- The indexed persistent-list example uses five entries with a `u2` index and
  executes through generated gfsim without a frontend bounds check.

## Focused results

- Agentic frontend: 200 run, 2 optional skipped.
- Agentic CLI: 53/53 passed.
- Semantic-core plus public API: 36/36 passed.
- `ValueConstraintTest` and `ACDataFlowAnalyzerTest`: 6/6 passed.
- QueueGraph plan tests: 53/53 passed.
- Five-entry generated gfsim regression: 1/1 passed.
- ACIR lit: 176/176 passed.
- Native ACIR/gfsim/CodeGen/E2E CTest: 20/20 passed.
- Fresh integrated pyc6/AC toolchain build: passed.
- PYC C++/Verilator integration: 6/6 passed; the `arbiter`,
  `atomic-transform`, `bit-widths`, and `popcount` generated cases also built
  through both backends.
- pyCircuit unit: 43/43 passed.
- Agentic contracts: 42/42 passed after regenerating the IR coverage ledger.
- API hygiene, strict documentation, and decision status: passed with 217
  verified rows and zero deferred.

The authoritative release-style run is
`agentic_circuit_summary.json`; bounded stdout/stderr and the exact generated
command list are retained beside this summary.

## Deliberate boundary

Dynamic aggregate indexing/extract/insert, path-sensitive guard refinement,
runtime loop termination proofs, general enum control-flow exhaustiveness, and
masked matching remain separate follow-up work.
