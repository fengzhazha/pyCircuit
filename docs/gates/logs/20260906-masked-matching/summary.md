# Verified masked matching gate

Decision 0218 adds one decode-oriented pure operation while keeping the Python
frontend free of mask objects, hardware types, and transaction mechanics.

## Evidence

- `ac.matches(value, "10x1")` accepts only an exact-width AST string literal
  using lowercase `0`, `1`, and `x` in MSB-first order.
- pyCircuit and Agentic share one semantic-core parser implementation while
  selecting separate extended/basic grammar policies.
- ACIR preserves `ac.var.matches` and verifies types, bounds, and canonical
  mask/value consistency.
- QueueGraph preserves `masked_match` with exact-width lowercase hexadecimal
  constants, including u64 bit 63, and independently rejects forged metadata.
- gfsim executes `(input & mask) == value`; PYC uses only existing
  vendor-neutral constants, bitwise AND, and equality.
- The public masked decode example has matching PYC C++ and Verilator
  cycle/value output; the direct gfsim adapter executes matching and
  non-matching inputs.

## Focused results

- Shared semantic parser and existing pyCircuit bitmask tests: 50/50 passed.
- Agentic frontend: 203 run, 2 optional skipped.
- Legacy direct gfsim adapter: 12/12 passed.
- ACDataFlow analysis: 23/23 passed.
- QueueGraph CodeGen: 116/116 passed.
- ACIR lit: 179/179 passed.
- PYC C++/Verilator masked decode parity: 1/1 passed.

## Remaining issue gate

Issue #39 is not complete until full closure, review, merge, and confirmed issue
closure. Issue #41, issue #42, and DavinciOO consumer implementation remain
strictly blocked behind that sequence.
