# pyCircuit 6

<p align="center">
  <img src="https://img.shields.io/badge/License-BSD--3--Clause-blue.svg" alt="License">
  <img src="https://img.shields.io/badge/pycircuit-Python%203.10%2B-green.svg" alt="pyCircuit Python 3.10 or later">
  <img src="https://img.shields.io/badge/Agentic-Python%203.11%2B-green.svg" alt="Agentic Circuit Python 3.11 or later">
  <img src="https://img.shields.io/badge/MLIR-22-orange.svg" alt="MLIR">
  <a href="https://github.com/PTO-ISA/pyCircuit/actions"><img src="https://github.com/PTO-ISA/pyCircuit/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://github.com/PTO-ISA/pyCircuit/actions/workflows/release.yml"><img src="https://github.com/PTO-ISA/pyCircuit/actions/workflows/release.yml/badge.svg" alt="Release"></a>
  <a href="https://github.com/PTO-ISA/pyCircuit/releases"><img src="https://img.shields.io/github/v/release/PTO-ISA/pyCircuit?display_name=tag" alt="Latest release"></a>
</p>

pyCircuit is a Python hardware construction and architecture-modeling
repository. The `pycircuit` frontend lowers cycle-aware designs to verified PYC
MLIR and emits synthesizable Verilog and a C++ cycle model. The retained
`agentic_circuit` frontend lowers architecture/process/queue descriptions to
ACIR, then targets either ACSim/gfsim or the pyCircuit 6 hardware flow.

[`PTO-ISA/pyCircuit`](https://github.com/PTO-ISA/pyCircuit) is the canonical
repository, release authority, and only active source of truth for both
pyCircuit and Agentic Circuit.
[`LinxISA/pyCircuit`](https://github.com/LinxISA/pyCircuit) is its downstream
fork for downstream compatibility validation.

The former standalone `PTO-ISA/agentic-circuit` repository is a private,
archived provenance record. Its final tombstone points to pyCircuit merge
[`cba1d938`](https://github.com/PTO-ISA/pyCircuit/commit/cba1d938ddcfaadf021bbff5a91553869028e124).
New AC source, issues, releases, and packages belong only in this repository.

## Why pyCircuit 6

- **Cycle-aware signals:** `CycleAwareSignal` carries logical-cycle provenance.
- **Automatic pipeline balancing:** mixed-cycle expressions lower to explicit
  delay registers.
- **Inferred state:** `domain.signal()` plus `<<=` or `.assign()` derives the
  required register structure.
- **One semantic IR:** C++ and Verilog consume the same verified `pyc` MLIR.
- **Preserved hierarchy:** module instances remain visible to simulation, DFX,
  and emitted RTL.
- **Scalable validation:** legality, cycle, depth, clock-domain, trace, and
  backend-equivalence gates are part of the repository workflow.

## Choose a frontend

| Goal | Distribution | Python import | Primary IR and runtime |
| --- | --- | --- | --- |
| Construct cycle-aware hardware | `pycircuit-hisi` | `pycircuit` | PYC → `pycc` → `libpyc6_runtime` / Verilog |
| Model architecture, processes, resources, and queues | `agentic-circuit` | `agentic_circuit` | ACPy/ACIR → ACSim/gfsim, or ACIR → PYC |

Use `pycircuit` when the design contract is signals, registers, memories,
pipeline timing, and synthesizable hardware. Use `agentic_circuit` when the
source model describes architectural processes, queues, resources, scheduling,
or workloads. The namespaces remain separate even when both frontends converge
on verified PYC for hardware generation.

See [Choose a frontend](docs/getting-started/choose-a-frontend.md) for the
supported entrypoints and first commands.

## Install

The canonical pyCircuit 6 source installation is:

```bash
git clone https://github.com/PTO-ISA/pyCircuit.git
cd pyCircuit
python3 -m pip install -e "python/semantic-core"
python3 -m pip install -e ".[dev,docs]"
pre-commit install
bash flows/scripts/pyc build
```

Install the Agentic Circuit distribution from the same checkout when using
ACPy, ACIR, ACSim, or gfsim:

```bash
python3 -m pip install -e "python/semantic-core"
python3 -m pip install -e "python/agentic-circuit[test]"
agentic-circuit --help
```

The source CLI's schema-backed commands also need the generated AC Python
resource tree. The canonical `run_agentic_circuit.sh` gate configures that tree
and supplies the required `PYTHONPATH`; installation alone is sufficient for
imports and `--help`, not for compiling a workspace.

Release wheels, once published, use the distribution name `pycircuit-hisi`;
the Python import remains `pycircuit`. The repository does not claim a PyPI
release until the corresponding PTO-ISA release workflow has completed.

The staged compiler is installed under
`.pycircuit_out/toolchain/install/`. Set `PYC_TOOLCHAIN_ROOT` to that directory
when running end-to-end builds from a source checkout.

## Agentic Circuit and ACIR

ACIR remains an independent, upper-level MLIR dialect. It is not folded into
the PYC dialect and does not replace the Cycle-Aware Signal model:

```text
agentic_circuit frontend -> ACPy 0.5 -> ACIR
                                         |-> ACSim -> gfsim
                                         `-> PYC -> pycc -> pyc6 C++ / Verilog

pycircuit frontend -> Cycle-Aware Signal -> PYC -> pycc -> pyc6 C++ / Verilog
```

The public `agentic_circuit` import and `agentic-circuit` CLI remain distinct
from `pycircuit`. AC symbols are not re-exported from `pycircuit.__init__`.
See the [ACIR architecture overview](docs/acir/index.md) and
[migration record](docs/acir/migration.md).

Exact unsigned circuit fields are available at every width from `ac.u1`
through `ac.u64`:

```python
@ac.struct
class Tag:
    value: ac.u13
    mask: ac.u13

masked = stream.apply(
    lambda item: item.with_fields(value=(item.value & item.mask) ^ 1)
)
```

Bit arithmetic and `& | ^ ~ << >>` preserve the declared width and reject
mixed-width operands.

Semantic primitives keep implementation choice out of Python:

```python
encoded = stream.apply(
    lambda item: item.with_fields(
        index=ac.priority_encode(item.mask, order="low").index,
        valid=ac.priority_encode(item.mask, order="low").valid,
    )
)
```

The same operation is available as `pycircuit.priority_encode(signal)`. PYC
keeps `pyc.priority_encode` for C++ reference simulation; only the Verilog
selection pass introduces internal `pyc.rtl.comb` implementation metadata and
a digest-verified BSD source closure.

Population count follows the same semantic/implementation split:

```python
count = signal.popcount()          # Cycle-Aware Signal
count = pycircuit.popcount(signal)
count = circuit.popcount(wire)     # structural frontend
```

The result width is `max(1, ceil(log2(N + 1)))`. Agentic
`ac.popcount(value)` lowers to the same `pyc.popcount`; the Verilog backend may
select the qualified BSD implementation without exposing its module name. See
the runnable [Agentic popcount example](examples/agentic-circuit/blocks/popcount.py).

Leading-zero count is also semantic and parameterized by its input type:

```python
leading = signal.count_leading_zeros()
trailing = signal.count_trailing_zeros()
leading = pycircuit.count_leading_zeros(signal)
trailing = ac.count_trailing_zeros(value)
```

Both return a value in `[0, N]`; an all-zero `N`-bit input returns `N`. PYC
keeps one `pyc.count_zeros` operation with a static `direction` parameter,
while the Verilog backend selects one shared digest-verified BSD balanced-tree
implementation. See the runnable
[Agentic example](examples/agentic-circuit/blocks/count_leading_zeros.py).

The epoch 0.5 rule frontend keeps scheduling mechanics out of Python:

```python
@ac.rule
def complete(entry):
    return entry.with_fields(done=True)

completed = complete(issued)
```

MLIR passes infer effects, establish the phase-one empty-check contract,
materialize handshake, resolve scheduling, and lower the transient rule to
marker-free internal firing IR. Dynamic checks remain fail-closed in this first
slice. See the
[bounded retirement example](examples/agentic-circuit/state/rob.py) for a
runnable frontend-to-gfsim slice.

The first stateful slice uses ordinary Table observation and assignment:

```python
@ac.rule
def install(rob, entry):
    old = rob[entry.index]
    rob[entry.index] = entry
    return old

outgoing = install(rob, incoming)
```

MLIR turns the assignment into a verified Table proposal and groups its Table
replace with Queue consumption and production. This initial slice accepts one
Table, one Queue input/output, and one complete Entry replace; unsupported
shapes fail closed. The runnable example is
[table_rule.py](examples/agentic-circuit/state/table_rule.py). PYC/RTL still
reject provisional Table graphs while gfsim executes the grouped transition.

## First cycle-aware design

```python
from pycircuit import (
    CycleAwareCircuit,
    CycleAwareDomain,
    cas,
    compile_cycle_aware,
    wire_of,
)


def counter(
    m: CycleAwareCircuit,
    domain: CycleAwareDomain,
    width: int = 8,
) -> None:
    enable = cas(domain, m.input("enable", width=1), cycle=0)
    count = domain.signal(width=width, reset_value=0, name="count")

    m.output("count", wire_of(count))
    domain.next()
    count.assign(count + 1, when=enable)


if __name__ == "__main__":
    design = compile_cycle_aware(counter, name="counter", eager=True)
    print(design.emit_mlir())
```

`domain.next()` advances the authoring-time logical cycle. The assignment to
`count` therefore creates a one-stage state update. When values from different
logical cycles meet, the compiler inserts the delay chain needed to align them.

## Build and test

Build the repository counter example for both backends:

```bash
export PYC_TOOLCHAIN_ROOT="$PWD/.pycircuit_out/toolchain/install"
PYTHONPATH=python/pycircuit/src \
python3 -m pycircuit.cli build \
  examples/pycircuit/counter/tb_counter.py \
  --out-dir /tmp/pyc_counter \
  --target both \
  --jobs 8
```

Pull requests use two lightweight required checks: pyCircuit Python/repository
hygiene and Agentic Circuit contract/frontend/CLI-inventory tests. Before
opening a PR,
run the matching local commands:

```bash
pre-commit run --files <changed-file> [<changed-file> ...]
pytest tests/unit -m unit
python3 tools/agentic-circuit/check-contracts.py
```

For native, MLIR, lowering, runtime, or backend changes, add the narrowest
affected local test to the PR evidence. Full AC/PYC closure is intentionally
reserved for the release workflow and blocks package publication.

To reproduce the complete Agentic Circuit G0/G1/G2 release lane locally:

```bash
PYC_GATE_RUN_ID=local-ac-$(date +%Y%m%d-%H%M%S) \
bash flows/scripts/run_agentic_circuit.sh
```

The script installs the current AC frontend, builds ACIR/ACSim/gfsim, runs the
MLIR and C++ suites, and validates canonical ACIR-to-PYC-to-C++/Verilog cases.

System tests require a built toolchain and Verilator:

```bash
pytest tests/system -m system
```

## External designs

pyCircuit provides language frontends, MLIR dialects and passes, runtimes,
backend libraries, generic examples, and verification contracts. Complete CPU,
NPU, SoC, board, and product-specific testbench sources are consumer-owned and
live outside this repository.

Linx, Janus, XiangShan, QEMU comparison, and FPGA flows consume a released or
pinned pyCircuit toolchain from their own repositories. The framework does not
carry consumer path allowlists, consumer-specific runtime headers, or in-tree
integration scripts.

## Documentation

- [V6 language specification](docs/v6_PyCircuit_Specification.md)
- [V6 tutorial](docs/v6_PyCircuit_Tutorial.md)
- [V6 software architecture](docs/v6_PyCircuit_Software_Architecture.md)
- [Choose a frontend](docs/getting-started/choose-a-frontend.md)
- [Frontend API](docs/FRONTEND_API.md)
- [Testbench API](docs/TESTBENCH.md)
- [IR specification](docs/IR_SPEC.md)
- [pyCircuit 6 decisions](docs/rfcs/pyc6-decisions.md)
- [pyCircuit 6 evolution plan](docs/pyc6-plan.md)
- [ACIR architecture and frontend](docs/acir/index.md)
- [Agentic Circuit migration](docs/acir/migration.md)

## Repository governance

PTO-ISA owns product decisions, both Python distributions, releases, package
publication, and the default branch. Consumer repositories pin a released or
reviewed pyCircuit revision and own their design-specific integration gates.
The LinxISA fork follows the upstream default branch and does not define a
second framework API. The standalone Agentic Circuit repository remains a
public migration record until its operational cutover checklist passes; it is
not an active development or publishing source.

- [Contribution workflow](docs/development/contributing-workflow.md)
- [Testing and gates](docs/development/testing-and-gates.md)
- [Review and merge](docs/development/review-and-merge.md)
- [Repository management](docs/development/repository-management.md)

Historical gate logs retain their original directory names. Active runtime,
trace, and gate contracts use `libpyc6_runtime`, `PYC6TRC3`, and
`run_semantic_regressions_v6.sh`.

## Repository layout

```text
pyCircuit/
├── python/semantic-core/        # Shared immutable value/layout semantics
├── python/pycircuit/src/pycircuit/  # Python language frontend
├── python/agentic-circuit/       # Agentic Circuit Python distribution
├── compiler/mlir/                # pyc dialect, passes, pycc, and emitters
├── compiler/acir/                # ACIR/ACSim dialects, passes, and tools
├── library/                      # pyCircuit C++ and Verilog libraries
├── simulator/gfsim/             # Agentic Circuit architecture simulator
├── examples/                     # pyCircuit and Agentic Circuit examples
├── flows/                        # Build and validation orchestration
├── tests/                        # Language- and layer-classified tests
└── docs/                         # Product and contributor documentation
```

## License

pyCircuit, including the integrated Agentic Circuit sources, is licensed under
the BSD 3-Clause License. See [LICENSE](LICENSE) and the
[relicensing record](docs/legal/AC-RELICENSE-BSD-3-CLAUSE.md).
