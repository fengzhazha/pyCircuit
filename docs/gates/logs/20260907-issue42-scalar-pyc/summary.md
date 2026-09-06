# Issue #42 scalar-PYC evidence

Decision 0220 removes the misinterpreted first-class PYC vector instruction
model while retaining recursive semantic-core/ACIR aggregates and their stable
MSB-first packed scalar lowering.

## Focused results

- PYC builds with 41 operations and two types; no `pyc.v_*`, vector operand
  constraint, vector pass, vector emitter dispatch, or vector runtime remains.
- Residual builtin vector function boundaries fail `pyc-check-flat-types`, and
  removed `pyc.v_get` fails as an unregistered operation.
- Non-Agentic Python tests pass 247 cases with three optional skips.
- Recursive 13-bit and 28-bit aggregate payloads lower to scalar PYC and pass
  C++/Verilog equivalence with no residual vector IR.
- Restored list-based scalar IssueQueue, RegisterFile, BypassUnit, examples,
  API hygiene, inventory, and repository contracts pass focused validation.

## Scope review

- Python `Vector`, `Wire[Vector]`, `shape=`, vector constants, lane iteration,
  broadcast/reduce/priority-mux, and vector-aware testbench packing are removed
  without compatibility aliases.
- `!ac.struct`, `!ac.enum`, builtin tuple, `!ac.value_array`, persistent Python
  lists, and topology `!ac.array` remain distinct high-level contracts.
- Repeated scalar hardware is authored with ordinary Python list/tuple and
  static loops. Aggregate payload ports use one exact-width packed scalar ABI,
  preserving reusable backend module definitions.

## Closure

- The full backend suite passed 18/18 cases against the refreshed local
  install tree after removing its stale generated `pyc_vec.hpp` artifact.
- Normal and nightly simulation lanes both passed, including IssueQueue,
  RegisterFile, BypassUnit, and trace DSL coverage.
- Fresh-toolchain AC G0/G1 passed 205 Python tests (two optional skips), 53
  additional Python tests, 182/182 lit tests, and 20/20 native/endpoint CTests.
- Fresh-toolchain AC G2 passed all seven scalar/aggregate PYC C++ and Verilog
  cases.
- Examples, V6 semantic regressions, strict decision status (220/220 verified),
  repository contracts, API hygiene, strict documentation, and the complete
  pre-commit suite pass. Results are recorded beside this summary.
