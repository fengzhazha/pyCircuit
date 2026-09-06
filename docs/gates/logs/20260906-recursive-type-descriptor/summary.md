# Recursive value descriptor gate

Decision 0212 introduces one immutable recursive type tree before admitting
nested aggregate lowering.

## Evidence

- Shared semantic types cover logical bool, exact bits, nominal enum/struct,
  structural tuple, and fixed value array.
- Every descriptor has canonical identity, stable SHA-256, recursive bit width,
  and explicit MLIR rendering. Bool and u1 retain different compiler identity
  while both continue to render as `i1` under epoch `0.5`.
- Agentic scalar annotations and flat `@ac.struct` payload discovery now create
  descriptors before rendering ACIR type syntax.
- ACIR uses `!ac.value_array<N x T>` for fixed payload values while preserving
  `!ac.array` for static Queue/Var topology collections. Tuple and value-array
  payloads are accepted only when every nested element is immutable.
- The repository C++ generator locates the shared semantic package explicitly;
  CMake build/install, isolated capture, CI, wheel packaging, and source tools
  no longer depend on an ambient Python installation.
- Broad PYC tests now run the real topology freeze, compile split C++ units,
  avoid a removed consumer trace dependency, and print exact-width gfsim
  values through their explicit storage accessor.

## Gates

- Shared descriptor and pyCircuit bitfield tests: 56 passed.
- Agentic frontend: 184 passed, 2 optional skipped.
- Queue/gfsim integration: 29 passed, 1 optional skipped.
- PYC C++/Verilator integration: 13/13 passed.
- ACIR type C++ suite: 7/7 passed; tuple/value-array positive and negative lit:
  2/2 passed.
- Full ACIR lit: 169/169 passed; native ACIR/gfsim/CodeGen/E2E: 16/16.
- pyCircuit unit: 36/36; Agentic contract: 37/37.

## Remaining boundary

QueueProgram and expression records still carry rendered MLIR strings. Nested
struct/enum/tuple/fixed-array values are descriptor-valid but are not admitted
to ACIR/QueueGraph/backends until the remaining string migration and recursive
aggregate verifier/lowering slices are complete.
