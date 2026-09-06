# PYC primitive registries

This directory separates language semantics from replaceable backend
implementations:

- `semantic_registry.json` is the stable, vendor-neutral PYC contract.
- `pyc_ir_inventory.yaml` is the exact registered PYC operation/type surface;
  its generated ledger records producer, verifier/folder, backend, test, and
  Agentic Circuit mapping coverage.
- `pyc_ir_coverage.json` is the generated machine-readable per-op/type ledger,
  including stage, status, replacement, producers, verifier/folder and
  canonicalization, both emitters, qualified RTL selection, positive/negative
  MLIR, examples, tests, and end-to-end coverage.
- `library/verilog/rtl_catalog.json` contains qualified implementation choices.

Python and canonical PYC may reference only semantic IDs.  Vendor module,
parameter, port, source, digest, provenance, and license data enter IR only in
the Verilog-only `pyc-select-rtl-primitives` pass.
