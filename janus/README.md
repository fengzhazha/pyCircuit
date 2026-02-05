# JanusCore Hardware (pyCircuit)

This folder contains the initial **Janus BCC bring-up model** written in **pyCircuit** (Python → `.pyc` MLIR → `pyc-compile` → C++/SystemVerilog).

## Quickstart (C++ model regression)

Assumes you have `pyCircuit` checked out next to this repo:

- `../pyCircuit/binding/python` (Python frontend)
- `../pyCircuit/pyc/mlir/build/bin/pyc-compile` (compiler)

Run:

```bash
hw/tools/run_janus_bcc_pyc_cpp.sh
```

### OoO bring-up core

The repo also contains an **out-of-order** bring-up model (single-issue backend with ROB/rename/IQ) as a stepping stone toward the full BCC design in `plan.md`.

Run:

```bash
hw/tools/run_janus_bcc_ooo_pyc_cpp.sh
```

Trace (writes logs/VCDs under `hw/generated/`):

```bash
PYC_TRACE=1 hw/tools/run_janus_bcc_ooo_pyc_cpp.sh
PYC_TRACE=1 PYC_TRACE_VERBOSE=1 hw/tools/run_janus_bcc_ooo_pyc_cpp.sh
PYC_VCD=1 hw/tools/run_janus_bcc_ooo_pyc_cpp.sh
```

To point at a different pyCircuit checkout:

```bash
PYC_ROOT=/path/to/pyCircuit hw/tools/run_janus_bcc_pyc_cpp.sh
```

## Emit `.pyc` / Verilog

```bash
PYC_ROOT=../pyCircuit
PYTHONPATH="$PYC_ROOT/binding/python:hw/pyc" \
  python3 -m pycircuit.cli emit hw/pyc/janus/bcc/janus_bcc_pyc.py -o /tmp/janus_bcc_pyc.pyc

"$PYC_ROOT/pyc/mlir/build/bin/pyc-compile" /tmp/janus_bcc_pyc.pyc --emit=verilog -o /tmp/janus_bcc_pyc.sv
```

## Notes

- Current scope is a **working skeleton** (in-order bring-up) that matches the directory split we’ll evolve into the full BCC/IFU/OOO/IEX/LSU pipeline described in `plan.md`.
- Entry point design: `hw/pyc/janus/bcc/janus_bcc_pyc.py`
- OoO entry point design: `hw/pyc/janus/bcc/janus_bcc_ooo_pyc.py`
- C++ testbench: `hw/tb/tb_janus_bcc_pyc.cpp`
- OoO C++ testbench: `hw/tb/tb_janus_bcc_ooo_pyc.cpp`
- Program fixtures: `hw/programs/*.memh`
