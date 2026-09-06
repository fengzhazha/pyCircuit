# Issue #39 final local closure

This is the final local closure for PTO-ISA/pyCircuit issue #39 after resolving
all three high-severity independent review findings.

## Implemented scope

- Exact bits, bit extract/concat/insert, shared bitfield schemas, recursive
  immutable value descriptors, nested nominal structs, enums, tuples, fixed
  value arrays, bounded constraints, and masked decode matching.
- Python remains serial and variable-oriented. `ac.var` is the only variable
  family; no `ac.variable`, Queue transaction checks, range markers, or
  backend storage types enter the public frontend.
- `ACDataFlowAnalyzer` is the public MLIR analysis boundary. It proves dynamic
  persistent state and all Table get/read/write indices; QueueGraph
  independently recomputes the same obligations.
- gfsim and PYC C++/Verilator preserve exact widths, aggregate packing, and
  decode results. Repeated module specializations remain shared while instance
  state stays independent.

## Review fixes

- Unary `bitConcat(UInt<64>)` no longer shifts by the storage width; constexpr
  and UBSan regressions pass.
- The direct gfsim adapter derives the actual operand `ValueType` for
  `ac.matches`, rejecting short, long, and Bool patterns.
- Every `TableGet`, `TableRead` address, and `TableWrite` address is checked by
  MLIR and independently by QueueGraph. u2-to-five endpoint indices pass;
  unsafe u3-to-five cases fail in raw verification, freeze, and forged plans.
- Independent re-review verdict: APPROVE, zero remaining findings in the three
  previously rejected areas.

## Formal G0/G1/G2 result

- Agentic frontend: 204 run, 2 optional skipped.
- Agentic CLI: 53/53 passed.
- ACIR lit: 181/181 passed.
- Native CTest: 20/20 passed.
- Fresh integrated pyc6/AC toolchain build: passed.
- Selected PYC C++/Verilator runtime integration: 7/7 passed.
- Full QueueGraph/gfsim integration: 32 passed, 1 optional skipped.
- Full PYC backend integration: 18/18 passed.
- G2 generated C++/Verilog cases: arbiter, atomic-transform, bit-widths,
  masked-match, and popcount passed.
- pyCircuit unit: 48/48 passed.
- Agentic contracts: 42/42 passed.
- API hygiene, strict docs, and decision status: passed with 218 rows and zero
  deferred.

The generated `agentic_circuit_summary.json`, bounded stdout/stderr, and exact
script command list are retained beside this summary.

## Remaining external gate

The code is not yet issue-complete: it still requires a `Closes #39` PR,
required checks, review, merge, and confirmed GitHub issue closure. Issue #41,
issue #42, and DavinciOO remain blocked until their required serial gates.
