# Janus (pyCircuit)

This folder contains **Janus bring-up cores** written in **pyCircuit**:

`Python` → `*.pyc` (MLIR) → `pyc-compile` → `Verilog` / `C++`

Layout:

- Sources: `janus/pyc/janus/bcc/`
- Generated outputs (checked in): `janus/generated/`
- C++ testbenches: `janus/tb/`
- Program fixtures: `janus/programs/*.memh`

## Quickstart (C++ regressions)

From the repo root:

```bash
scripts/pyc build
scripts/pyc regen
scripts/pyc test
```

Or run individually:

```bash
bash janus/tools/run_janus_bcc_pyc_cpp.sh
bash janus/tools/run_janus_bcc_ooo_pyc_cpp.sh
```

## Emit `.pyc` / Verilog

```bash
PYTHONPATH=python:janus/pyc python3 -m pycircuit.cli emit janus/pyc/janus/bcc/janus_bcc_pyc.py -o /tmp/janus_bcc_pyc.pyc
./build/bin/pyc-compile /tmp/janus_bcc_pyc.pyc --emit=verilog -o /tmp/janus_bcc_pyc.v
```

## Tracing

Both C++ testbenches support:

- `PYC_TRACE=1` (log file)
- `PYC_VCD=1` (VCD waveform)
- `PYC_TRACE_DIR=/path/to/out` (override output dir; default is under `janus/generated/`)
