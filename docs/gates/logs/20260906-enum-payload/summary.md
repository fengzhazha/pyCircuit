# Nominal enum payload gate

Decision 0214 maps standard Python `enum.Enum` to a verified nominal value
without adding a hardware-specific frontend constructor.

## Evidence

- Frontend enum members use contiguous declaration-order ordinals and retain
  `EnumType` identity inside nested structs.
- Canonical `ac.enum` and `ac.var.enum` verifiers reject unknown members,
  mismatched nominal result types, and non-equality comparisons.
- QueueGraph records exact enum name/member/width metadata and enum constants
  record matching member/ordinal identities.
- gfsim emits one compact C++ `enum class`; PYC uses the same exact-width
  ordinal. Both backends execute nested WAIT-to-RUN replacement and equality.

## Focused gates

- Frontend positive and negative enum tests: passed.
- ACIR enum value positive/negative lit: 2/2 passed.
- Generated gfsim enum execution: passed.
- Generated PYC C++/Verilator parity: passed.
- Full Agentic frontend: 188 passed, 2 optional skipped.
- Full Queue/gfsim integration: 31 passed, 1 optional skipped.
- Full PYC C++/Verilator integration: 15/15 passed.
- ACIR lit: 171/171; native ACIR/gfsim/CodeGen/E2E: 18/18.
- pyCircuit unit: 36/36; Agentic contract: 37/37; decision status: 214
  verified rows with zero deferred.

## Remaining boundary

Direct enum Queue payloads, enum switch/exhaustiveness, tuple/value-array
operations, bounded constraints, and the remaining descriptor migration remain
open.
