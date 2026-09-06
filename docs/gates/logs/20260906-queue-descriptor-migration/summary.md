# QueueProgram recursive descriptor migration gate

Decision 0216 removes MLIR type spelling from Python QueueProgram and
expression-lowering semantics. Recursive `ValueType` objects now remain intact
until the ACIR text renderer.

## Evidence

- Queue, rule state, persistent state, Table, memory, slot, fanout, and module
  signature records carry `ValueType` descriptors.
- ExpressionEmitter carries descriptors through all value maps and uses class,
  equality, field, and bit-width APIs for analysis.
- One `_render_type` helper owns the only frontend call to `ValueType.mlir()`;
  AST contract tests fail if MLIR spelling re-enters semantic records.
- Bool and u1 retain distinct descriptors/fingerprints while explicit
  epoch-0.5 equality and integer-width helpers preserve current `i1` behavior.
- The direct Python gfsim adapter consumes descriptors; C++ QueueGraph strings
  begin only after verified ACIR parsing.

## Gate results

- Agentic frontend: 194 run, 2 optional skipped.
- Agentic CLI: 53/53 passed.
- Direct Python gfsim codegen: 11/11 passed.
- Queue/gfsim integration: 32 passed, 1 optional skipped.
- PYC C++/Verilator integration: 17/17 passed.
- ACIR lit: 173/173 passed.
- Native ACIR/gfsim/CodeGen/E2E CTest: 20/20 passed.
- pyCircuit unit: 37/37 passed.
- Agentic contracts: 42/42 passed.
- API hygiene, IR coverage, strict docs, and decision status: passed.

## Remaining boundary

Logical bool/u1 separation remains an explicit future hard break. Issue #39
still requires the Constant/FiniteSet/ClosedInterval/Unknown constraint domain,
enum exhaustiveness, masked bit matching, final closure, and upstream merge.
