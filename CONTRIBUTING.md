# Contributing to pyCircuit

This repo is a prototype, but it is intended to grow into a serious hardware construction + compilation toolchain.
Small, focused changes with good tests/examples are preferred.

## Development setup

### Build `pyc-compile`

You need an LLVM+MLIR installation/build that provides `LLVMConfig.cmake` and `MLIRConfig.cmake`.

```bash
LLVM_DIR="$(llvm-config --cmakedir)"
MLIR_DIR="$(dirname "$LLVM_DIR")/mlir"

cmake -G Ninja -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR="$LLVM_DIR" \
  -DMLIR_DIR="$MLIR_DIR"

ninja -C build pyc-compile pyc-opt
```

### Regenerate checked-in outputs

This repo checks in golden Verilog/C++ outputs under `examples/generated/`.

```bash
scripts/pyc regen
git diff
```

## What to include in a PR

- A clear description of the change and why it is needed.
- If you changed codegen or the frontend, update at least one example and regenerate `examples/generated/`.
- For dialect or lowering changes: update `docs/IR_SPEC.md` as needed.
- Prefer minimal surface area changes; avoid broad refactors unless they remove real complexity.

## Testing

The default “smoke test” is the LinxISA CPU C++ regression:

```bash
bash tools/run_linx_cpu_pyc_cpp.sh
```

CI also regenerates goldens and checks that no diffs are produced.
