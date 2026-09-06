# Static bitfield schema gate

Decision 0211 completes issue #39's first named-bitfield slice without adding
Queue, ready/full, transaction, or backend mechanics to Python.

## Evidence

- `pycircuit-semantic-core` provides the one immutable layout, closed-range
  validator, overlapping-write checker, field-slice metadata, and canonical
  SHA-256 implementation used by both Python frontends.
- Agentic `BitfieldSpec` lowers attribute/indexed reads, MSB-first multi-field
  selection, and immutable updates to existing `ac.var.extract`,
  `ac.var.concat`, and `ac.var.insert` operations.
- `ac.bitfield` independently verifies canonical field metadata and its
  fingerprint. Bit operations resolve their schema provenance and reject stale
  fingerprints, unknown fields, incorrect slices, or incorrect operand widths.
- The 32-bit decode example covers u3/u5/u17 fields and an overlapping `low25`
  read view. Generated gfsim executes the expected decode/update value.
- The scalar example is lowered through QueueGraph and PYC; generated C++ and
  Verilator both produce `2443373269` at cycle 2.

## Focused gates

- Shared semantic core plus existing pyCircuit bitfield suite: 49 passed.
- Agentic frontend suite: 183 passed, 2 optional skipped.
- ACIR lit: 169/169 passed, including schema/provenance positive and negative
  cases.
- Native ACIR/gfsim/CodeGen suites: 16/16 passed, including the registered
  bitfield decode E2E.
- Queue/gfsim integration: 29 passed, 1 optional skipped.
- Agentic contract suite: 37/37 passed; IR inventory/coverage ledger is current.
- Decision status: 211 rows, zero deferred.
- Repository contracts, API hygiene, repository management, strict docs, Ruff,
  Python compilation, and diff whitespace checks: passed.
- Generated gfsim decode execution: passed.
- PYC C++/Verilator cycle/value parity: passed.
- `acir-opt`, QueueGraph plan, gfsim generator, and PYC generator rebuild:
  passed.

## Remaining issue #39 work

The recursive value-type descriptor, bool/u1 hard-break decision, recursive
aggregates, bounded static constraint domain, masked matching, and full issue
closure remain active in `docs/pyc6-plan.md`.
