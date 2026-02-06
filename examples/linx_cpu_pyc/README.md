# LinxISA CPU (pyCircuit prototype)

`examples/linx_cpu_pyc/linx_cpu_pyc.py` builds a small LinxISA bring-up CPU using
the pyCircuit Python DSL.

The design is split across multiple Python files:

- `examples/linx_cpu_pyc/isa.py`: opcode + state encodings
- `examples/linx_cpu_pyc/decode.py`: instruction decode
- `examples/linx_cpu_pyc/regfile.py`: GPR + T/U stacks
- `examples/linx_cpu_pyc/pipeline.py`: pipeline reg bundles / state containers
- `examples/linx_cpu_pyc/stages/*.py`: IF/ID/EX/MEM/WB stage logic
- `examples/linx_cpu_pyc/memory.py`: internal byte-addressed memory instantiation
- `examples/linx_cpu_pyc/util.py`: small helpers used across the design

The CPU includes an internal unified byte-addressed memory (instruction + data)
implemented via the `pyc.byte_mem` op (C++ + Verilog backends).

End-to-end flow:

`Python` → `*.pyc` (MLIR) → `pyc-compile` → `Verilog` / `C++`

## C++ self-checking testbench

This folder includes `examples/linx_cpu_pyc/tb_linx_cpu_pyc.cpp`, which is meant
to be compiled together with the generated C++ output from `pyc-compile`.

Run:

```sh
bash tools/run_linx_cpu_pyc_cpp.sh
```

Run a specific ELF / object file (loads `.text` at `0x10000` by converting to a
byte-oriented `memh` image first):

```sh
bash tools/run_linx_cpu_pyc_cpp.sh --elf /Users/zhoubot/linxisa/linx-test/test_or.o --expected 0x0000ff00
```

You can also run with an explicit memh file:

```sh
bash tools/run_linx_cpu_pyc_cpp.sh --memh examples/linx_cpu/programs/test_or.memh --expected 0x0000ff00
```
