# LinxISA CPU (pyCircuit example)

This folder contains:

- `examples/linx_cpu/programs/*.memh`: byte-addressed program images for the unified instruction/data memory
- `examples/linx_cpu/tb_linx_cpu_pyc.sv`: a small self-checking SystemVerilog testbench for the generated `linx_cpu_pyc` module

## Generate Verilog / C++

Generated outputs live under `examples/generated/linx_cpu_pyc/`.

Regenerate everything (all examples + linx CPU):

```bash
scripts/pyc regen
```

## Run the SV testbench (against generated Verilog)

Use any SystemVerilog simulator. The testbench expects plusargs:

- `+memh=<path>`: `$readmemh`-compatible byte image (default: `examples/linx_cpu/programs/test_or.memh`)
- `+expected=<hex>`: expected 32-bit value at address `0x100` when `ebreak` halts (default: `0x0000ff00`)
