# pyCircuit (prototype)

`pyCircuit` is a growing hardware construction + compilation project:

`Python frontend` → emits `PYC` MLIR (`*.pyc`) → MLIR passes → emit backends:

- `include/pyc/cpp/`: C++ templates for cycle-accurate models
- `include/pyc/verilog/`: Verilog/SystemVerilog templates for RTL simulation

## Status

This repository is an early prototype. The current goal is to lock down:

- A stable, extensible `pyc` MLIR dialect for common circuit components
- A strict **ready/valid** handshake model for streaming components (FIFO)
- Multi-clock domain modeling from day 1 (`!pyc.clock` / `!pyc.reset`)

## Quickstart (macOS/Linux)

### 1) Build MLIR (`mlir-opt`) from `~/llvm-project`

```bash
cmake -G Ninja -S ~/llvm-project/llvm -B ~/llvm-project/build-mlir \
  -DLLVM_ENABLE_PROJECTS=mlir \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DCMAKE_BUILD_TYPE=Release

ninja -C ~/llvm-project/build-mlir mlir-opt
```

### 2) Build `pyc-opt` / `pyc-compile`

```bash
cmake -G Ninja -S pyc/mlir -B pyc/mlir/build \
  -DMLIR_DIR=$HOME/llvm-project/build-mlir/lib/cmake/mlir \
  -DLLVM_DIR=$HOME/llvm-project/build-mlir/lib/cmake/llvm

ninja -C pyc/mlir/build pyc-opt pyc-compile
```

### 3) Emit `.pyc` from Python and compile to Verilog

```bash
PYTHONPATH=binding/python python3 -m pycircuit.cli emit examples/counter.py -o /tmp/counter.pyc
./pyc/mlir/build/bin/pyc-compile /tmp/counter.pyc --emit=verilog -o /tmp/counter.sv
```

## Python AST/JIT control flow (SCF prototype)

The repo includes an AST-based frontend that emits `scf.if` / `scf.for` and then lowers them to static hardware.

In `pycircuit.cli emit`, **JIT mode is enabled by default** when your design defines:

- `build(m: Circuit, ...)` (builder argument + optional defaulted params)

```bash
bash examples/update_generated.sh
```

### Multi-file designs: `@jit_inline`

When splitting a design across multiple Python files (pipeline stages, helper modules), mark helper functions with
`@pycircuit.jit_inline` so calls from inside `build(...)` are **compiled** (AST/SCF) into the current circuit rather than
executed as Python at JIT time. This preserves consistent name-mangling with file/line provenance in generated C++/SV.

## Layout

- `binding/python/pycircuit/`: Python DSL + CLI (emits `.pyc` MLIR)
- `pyc/mlir/`: MLIR dialect, passes, tools (`pyc-opt`, `pyc-compile`)
- `include/pyc/`: backend template headers (C++ + Verilog)
- `examples/`: small designs and golden outputs
- `docs/IR_SPEC.md`: current PYC IR contract (prototype)

## LinxISA CPU bring-up (prototype)

This repo includes a LinxISA 5-stage (multi-cycle) CPU bring-up model written in pyCircuit:

- pyCircuit source: `examples/linx_cpu_pyc/`
- Program images + SV testbench: `examples/linx_cpu/`
- Generated outputs (checked in): `examples/generated/linx_cpu_pyc/`

### pyCircuit CPU (C++ backend)

After building `pyc-compile`, run the self-checking C++ testbench:

```bash
bash tools/run_linx_cpu_pyc_cpp.sh
```

Run a LinxISA relocatable ELF (`.o`) directly (loads `.text/.data/.bss`, applies a small set of relocations, boots at `_start`):

```bash
bash tools/run_linx_cpu_pyc_cpp.sh --elf ../linxisa/linx-test/test_branch2.o
```

Debug trace (prints one line per instruction at WB):

```bash
PYC_TRACE=1 bash tools/run_linx_cpu_pyc_cpp.sh --elf ../linxisa/linx-test/test_branch2.o
```

Override boot PC manually if needed:

```bash
PYC_BOOT_PC=0x10000 bash tools/run_linx_cpu_pyc_cpp.sh --memh examples/linx_cpu/programs/test_or.memh
```
