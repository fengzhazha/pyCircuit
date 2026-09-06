# Agentic Circuit

Agentic Circuit is a Python and MLIR-based architecture construction system
that generates a structured, pure C++ graph-flow simulator named `gfsim`.
It also emits canonical PYC IR for downstream C++ and Verilog generation.

The source tree is intentionally release-neutral: product versions belong to
Git tags and GitHub Releases, not directory names, filenames, symbols, or test
names. Serialized artifacts still carry an exact contract epoch because that
field is part of their wire-format compatibility contract.

## Development baseline

The repository is locked to LLVM/MLIR 22.1.8. On Apple Silicon, the default
prefix is `/opt/homebrew/opt/llvm`. On another supported host, pass the
equivalent package explicitly with
`-DMLIR_DIR=/path/to/llvm/lib/cmake/mlir`.

```sh
tools/agentic-circuit/bootstrap-dev.sh
source .venv/bin/activate
python -m unittest discover -s tests/python/agentic-circuit/contracts -p test_contracts.py -v
cmake --preset dev-llvm22
cmake --build --preset dev-llvm22
```

The bootstrap installs the repository-internal `pycircuit-semantic-core`
distribution before Agentic Circuit. It contains the immutable type/layout
semantics shared with the pyCircuit frontend; neither public namespace imports
the other.

Use `release-llvm22` for a release configuration. The exact upstream release,
commit, archive digest, supported host triples, and version policy are recorded
in [`toolchains/agentic-circuit/llvm.lock.json`](../../toolchains/agentic-circuit/llvm.lock.json).

## Documentation

- [Specification index](../../docs/acir/spec/README.md)
- [Agentic Circuit specification manual](../../docs/acir/spec/agentic-circuit.md)
- [Agentic Circuit 团队 Specification 手册](../../docs/acir/spec/agentic-circuit.zh-CN.md)
- [NDF release-layout decision](../../docs/rfcs/acir/D-RELEASE-LAYOUT-001.md)
- [Examples by semantic role](../../examples/agentic-circuit/README.md)

Canonical machine-readable schemas:

- [ACPy](../../schemas/agentic-circuit/acpy.schema.json)
- [Capabilities](../../schemas/agentic-circuit/capabilities.schema.json)
- [ComponentSchema](../../schemas/agentic-circuit/component.schema.json)
- [Official opcode catalog schema](../../schemas/agentic-circuit/opcode-catalog.schema.json)
- [Official Queue building-block catalog](../../schemas/agentic-circuit/opcodes.json)
- [PTO trace](../../schemas/agentic-circuit/pto-trace.schema.json)
- [Build manifest](../../schemas/agentic-circuit/build-manifest.schema.json)
- [Run manifest](../../schemas/agentic-circuit/run-manifest.schema.json)
- [Run result](../../schemas/agentic-circuit/run-result.schema.json)
- [Diagnostic](../../schemas/agentic-circuit/diagnostic.schema.json)
- [ACSim binding](../../schemas/agentic-circuit/acsim-binding.schema.json)
- [ACIR process-state plan](../../schemas/agentic-circuit/acir-process-state-plan.schema.json)

The repository uses a hard-break layout. Removed implementation-phase and
product-version paths have no aliases or compatibility symlinks. Historical
documents remain recoverable from the Git revision recorded by the NDF
historical reference.

## Project policies

Agentic Circuit is distributed as part of pyCircuit under the
[BSD 3-Clause License](LICENSE). See
[Contributing](../../CONTRIBUTING.md), the [Code of Conduct](../../CODE_OF_CONDUCT.md),
[Security policy](../../SECURITY.md) before opening a change or report.
