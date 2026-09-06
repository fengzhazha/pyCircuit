# Tuple and fixed value-array payload gate

Decision 0215 completes the issue #39 aggregate field slice without adding a
hardware container to Python. Ordinary tuple annotations, `ac.array[N, T]`,
tuple/list literals, and constant subscription lower through verified ACIR.

## Evidence

- `ac.var.tuple`, `ac.var.array`, and `ac.var.element` preserve exact recursive
  types and reject malformed shape, type, or index contracts.
- QueueGraph carries canonical aggregate kind, element, length, and packed
  width metadata. Forged tuple width, value-array shape, aggregate width over
  64 bits, and payload field width are rejected before code generation.
- Generated gfsim uses packed `UInt<8>` and `UInt<16>` fields and executes the
  tuple increment, four-lane rotation, and static selection correctly.
- Recursive type-aware gfsim conversion covers enum and nested nominal struct
  elements. The 13-bit recursive example executes packed `2962 -> 7125`.
- QueueGraph-to-PYC uses `pyc.concat` and `pyc.extract`; generated PYC C++ and
  Verilator produce the same 28-bit and recursive 13-bit transactions on the
  same cycles.
- TypeScope-aware nominal lookup preserves the earlier nested struct and enum
  paths while aggregate widths are computed.
- Checked width arithmetic and exact element-shape verification reject
  overflow, swapped tuple operands, wrong arity, and cross-element slices.

## Gate results

The archived official runner log intentionally retains the first full-run
failure: exact ACIR registry tests still named the old 121-operation set. The
golden was updated to the current exact 145-operation registry, and the same
G0/G1/G2 command then passed. `agentic_circuit_summary.json` is the final
machine-readable pass record; `supplemental_results.txt` records the broader
Queue and PYC suites that are outside the focused runner.

- Agentic frontend: 191 run, 2 optional skipped.
- Agentic CLI: 53/53 passed.
- Queue/gfsim integration: 32 passed, 1 optional skipped.
- PYC C++/Verilator integration: 17/17 passed.
- ACIR lit: 173/173 passed.
- Native ACIR/gfsim/CodeGen/E2E CTest: 20/20 passed.
- pyCircuit unit: 36/36 passed.
- Agentic contracts: 37/37 passed; repository contract schema check passed.
- IR coverage, API hygiene, strict docs, and decision status: passed.

## Remaining boundary

Direct frontend aggregate Queue payloads, dynamic aggregate indices, payloads
wider than 64 bits, the remaining QueueProgram descriptor migration, bounded static
constraints, masked bit matching, and enum exhaustiveness remain open.
