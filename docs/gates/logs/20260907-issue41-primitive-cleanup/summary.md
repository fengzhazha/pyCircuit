# Issue #41 primitive-cleanup evidence

Decision 0219 removes dormant ACIR declarations, retires Decision 0199's
legacy string summaries, canonicalizes scalar PYC operations, and adds an exact
machine-readable PYC inventory. Issue #42 remains the sole owner of first-class
PYC vector removal.

## Focused results

- Python/PYC/vector compatibility: 151 passed, 2 optional skips.
- QueueGraph/gfsim integration: 32 passed, 1 optional skip.
- ACIR-to-PYC C++/Verilog integration: 18 of 18 passed.
- PYC inventory: exact 48 operations and 2 types; only seven `pyc.v_*`
  operations are marked `pending-removal` for issue #42.
- Agentic repository contracts: 12 public schemas, 36 standard-library
  components, epoch 0.5, LLVM 22.1.8.
- Canonical PYC verifier checks accept the positive fixture and reject an
  unknown `pyc.cmp` predicate and a mismatched `pyc.select` result type.

## Full closure results

- AC G0 frontend: 205 passed with 2 optional skips; CLI: 53 of 53 passed.
- AC G1: ACIR lit 182 of 182 passed; native CTest 20 of 20 passed.
- AC G2: fresh integrated LLVM/MLIR 22 toolchain build, five generated
  primitive cases, and seven focused ACIR-to-PYC C++/Verilog cases passed.
- pyCircuit root closure: 51 unit tests passed inside the closure script; the
  final flow-fix sweep passed all 52 unit tests. API hygiene, strict
  documentation, and strict decision status with 219 rows and zero deferred
  also passed.
- Normal simulation passed `trace_dsl_smoke`, IssueQueue, RegisterFile, and
  BypassUnit.
- V6 semantic regressions passed C++/Verilator parity for X/Z values,
  reset/invalidate ordering, and net-resolution depth.
- The examples gate passed API hygiene, build/run smoke, value-class
  canonicalization, cache, runtime ownership, phase API, probe, memory
  observability, negative legality, and strict decision-status checks.

## Review findings

- Production sources contain no parser alias or producer for `pyc.mux`,
  `pyc.eq`, `pyc.ult`, `pyc.slt`, `pyc.shli`, `pyc.lshri`, or `pyc.ashri`.
- Production ACIR contains no removed dormant op/type or legacy rule/firing
  summary producer. Remaining spellings are explicit hard-break tests or
  decision documentation.
- `DataFlowSolver` remains confined to `ACDataFlowAnalyzer`'s private
  implementation, and `ac.var` remains the only variable family.
- Vector ODS operations, emitters, runtime, and semantics are unchanged. The
  issue #41 diff only updates shared scalar operation class names and canonical
  spelling expectations used by existing vector tests.
- The shared repository flow `PYTHONPATH` now includes the semantic-core source
  required by every in-tree pyCircuit build; a focused unit test locks this
  current-checkout behavior.

## Closure

The full AC G0/G1/G2 output, simulation case logs, semantic summaries, and
strict decision-status report are recorded beside this summary.
