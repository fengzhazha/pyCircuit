# Nested nominal payload gate

Decision 0213 closes the first recursive aggregate execution slice.

## Evidence

- The Python frontend resolves nested `@ac.struct` declarations independently
  of source order, preserves one recursive descriptor graph, and rejects
  cycles before ACIR.
- Nested attribute reads and immutable replacements lower to typed chained
  `ac.var.get` and `ac.var.with` operations.
- ACIR retains nominal nested declarations and recursively derived DLTI layout.
- QueueGraph validates nested payload references/cycles. gfsim emits dependency-
  ordered C++ structs and executes the nested update without flattening types.
- PYC recursively packs the example to 26 bits; generated C++ and Verilator
  agree on the output cycle and packed value.

## Focused gates

- Nested frontend positive/cycle-negative tests: passed.
- QueueGraph recursive-payload rejection C++ test: passed.
- Generated nested gfsim execution: passed.
- Generated PYC C++/Verilator parity: passed.
- Full Agentic frontend: 186 passed, 2 optional skipped.
- Full Queue/gfsim integration: 30 passed, 1 optional skipped.
- Full PYC C++/Verilator integration: 14/14 passed.
- ACIR lit: 169/169; native ACIR/gfsim/CodeGen/E2E: 17/17.

## Remaining boundary

Nominal enum values, tuple operations, fixed value-array operations, and the
remaining QueueProgram/expression descriptor migration are still open.
