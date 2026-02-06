# Cube Matrix Multiplication Accelerator

A 16×16 systolic array matrix multiplication accelerator implemented in pyCircuit.

## Overview

The Cube module implements a weight-stationary systolic array for matrix multiplication. It features:

- **16×16 Processing Element (PE) array** (256 PEs total)
- **16-bit integer inputs** (weights and activations)
- **32-bit accumulator** (prevents overflow for 16-bit × 16-bit operations)
- **Weight-stationary dataflow** (weights loaded once, activations stream through)
- **Memory-mapped interface** for CPU integration

## Architecture

### Systolic Array Structure

```
        a0  a1  a2  ... a15  (activations flow down)
         ↓   ↓   ↓       ↓
    w0→[PE][PE][PE]...[PE]→ partial sums flow right
    w1→[PE][PE][PE]...[PE]→
    w2→[PE][PE][PE]...[PE]→
    ...
   w15→[PE][PE][PE]...[PE]→
         ↓   ↓   ↓       ↓
        c0  c1  c2  ... c15  (results accumulate)
```

### Processing Element (PE)

Each PE performs multiply-accumulate (MAC) operations:

```python
product = weight * activation  # 16-bit × 16-bit = 32-bit
acc_next = acc + product + partial_sum_in  # 32-bit accumulation
```

**Note**: Due to current pyCircuit limitations, multiplication is implemented as addition (placeholder). This will be updated when multiplication support is added.

### Dataflow

1. **Load Phase**: Weights (W matrix) are loaded into all 256 PEs
2. **Compute Phase**: Activations (A matrix) stream vertically through columns (16 cycles)
3. **Drain Phase**: Partial sums propagate through the array (15 cycles)
4. **Done**: Results are available in the result buffer

## Memory Map

Base address: `0x80000000` (configurable)

| Address Range | Size | Description |
|--------------|------|-------------|
| `0x00` | 8 bytes | Control register (start, reset) |
| `0x08` | 8 bytes | Status register (done, busy) |
| `0x10 - 0x2F` | 32 bytes | Matrix A (activations): 16 × 16-bit |
| `0x210 - 0x40F` | 512 bytes | Matrix W (weights): 16×16 × 16-bit |
| `0x410 - 0x80F` | 1024 bytes | Matrix C (results): 16×16 × 32-bit |

### Control Register (0x00)

| Bit | Name | Description |
|-----|------|-------------|
| 0 | START | Start computation (write 1) |
| 1 | RESET | Reset accelerator (write 1) |
| 2-63 | Reserved | - |

### Status Register (0x08)

| Bit | Name | Description |
|-----|------|-------------|
| 0 | DONE | Computation complete (read-only) |
| 1 | BUSY | Accelerator busy (read-only) |
| 2-63 | Reserved | - |

## File Structure

```
cube/
├── __init__.py          # Package marker
├── cube.py              # Top-level module (main build function)
├── cube_types.py        # Dataclasses (CubeState, PERegs, FsmResult, MmioWriteResult)
├── cube_consts.py       # Constants (states, addresses, array size)
├── util.py              # Utility functions (Consts dataclass)
└── README.md            # This file
```

### Key Components in cube.py

| Function | Decorator | Description |
|----------|-----------|-------------|
| `_make_pe_regs` | - | Create 16×16 PE register array |
| `_make_result_regs` | - | Create 256 result registers |
| `_make_weight_regs` | - | Create 256 weight registers |
| `_make_activation_regs` | - | Create 16 activation registers |
| `_build_pe` | `@jit_inline` | Build single PE (MAC operation) |
| `_build_array` | - | Build 16×16 systolic array |
| `_build_fsm` | `@jit_inline` | Build control FSM |
| `_build_mmio_read` | - | Build MMIO read logic |
| `_build_mmio_write` | - | Build MMIO write logic |
| `build` | - | Top-level build function |

## Usage

### Generating Outputs

```bash
# Generate Verilog and C++ from Python
bash janus/update_generated.sh
```

This produces:
- `janus/generated/janus_cube_pyc/janus_cube_pyc.v` - Verilog RTL
- `janus/generated/janus_cube_pyc/janus_cube_pyc_gen.hpp` - C++ simulation header

### Running Tests

```bash
# Run C++ testbench
bash janus/tools/run_janus_cube_pyc_cpp.sh

# With tracing enabled
PYC_TRACE=1 PYC_VCD=1 bash janus/tools/run_janus_cube_pyc_cpp.sh
```

### Operation Sequence

1. **Write weights** to addresses `0x210 - 0x40F` (256 × 16-bit values)
2. **Write activations** to addresses `0x10 - 0x2F` (16 × 16-bit values)
3. **Start computation** by writing `0x01` to control register at `0x00`
4. **Poll status** register at `0x08` until DONE bit is set
5. **Read results** from addresses `0x410 - 0x80F` (256 × 32-bit values)

### Timing

- **Load weights**: 1 cycle
- **Compute**: 16 cycles (streaming 16 rows of activations)
- **Drain**: 15 cycles (pipeline depth)
- **Total**: ~32 cycles for 16×16 matrix multiplication

## FSM States

| State | Value | Description |
|-------|-------|-------------|
| IDLE | 0 | Waiting for start signal |
| LOAD_WEIGHTS | 1 | Loading weights into PEs |
| COMPUTE | 2 | Streaming activations, computing |
| DRAIN | 3 | Draining pipeline |
| DONE | 4 | Computation complete |

## Implementation Details

### JIT Compilation Patterns

The code follows pyCircuit JIT compilation rules:

1. **Functions without `@jit_inline`** execute at Python time (before JIT compilation)
   - Used for register creation loops (`_make_pe_regs`, `_make_weight_regs`, etc.)
   - Can use Python for loops, list comprehensions, etc.

2. **Functions with `@jit_inline`** are compiled into hardware
   - Used for combinational logic (`_build_pe`, `_build_fsm`)
   - Must follow JIT rules: no tuple unpacking, return at top-level

3. **Dataclasses for return values** avoid tuple unpacking (not supported in JIT)
   - `FsmResult(load_weight, compute, done)`
   - `MmioWriteResult(start, reset_cube)`

4. **`Consts` dataclass** centralizes common constant values
   - `consts.zero32`, `consts.one1`, etc.

### Register Allocation

| Category | Count | Width | Total Bits |
|----------|-------|-------|------------|
| State registers | 4 | 1-8 bits | ~13 bits |
| PE weight registers | 256 | 16 bits | 4096 bits |
| PE accumulator registers | 256 | 32 bits | 8192 bits |
| Weight buffer | 256 | 16 bits | 4096 bits |
| Activation buffer | 16 | 16 bits | 256 bits |
| Result buffer | 256 | 32 bits | 8192 bits |

**Total**: ~24,845 bits (~3.1 KB)

## Testing

### C++ Testbench

Located at `janus/tb/tb_janus_cube_pyc.cpp`:

| Test | Description |
|------|-------------|
| `testIdentity` | Basic operation with zero weights |
| `testSimple2x2` | Simple 2×2 matrix operation |

### Debug Options

| Environment Variable | Description |
|---------------------|-------------|
| `PYC_TRACE=1` | Generate `.log` file with execution trace |
| `PYC_VCD=1` | Generate `.vcd` waveform file |
| `PYC_TRACE_DIR=<path>` | Output directory for trace files |

## Example: Matrix Multiplication

```python
# Matrix A (activations) - 16×16
A = [[a00, a01, ..., a0F],
     [a10, a11, ..., a1F],
     ...
     [aF0, aF1, ..., aFF]]

# Matrix W (weights) - 16×16
W = [[w00, w01, ..., w0F],
     [w10, w11, ..., w1F],
     ...
     [wF0, wF1, ..., wFF]]

# Result C = W × A (16×16)
C = [[c00, c01, ..., c0F],
     [c10, c11, ..., c1F],
     ...
     [cF0, cF1, ..., cFF]]

# Where: cij = Σ(wik * akj) for k=0 to 15
```

## Known Limitations

1. **Multiplication operator**: Currently using addition as placeholder due to pyCircuit limitations
2. **No overflow detection**: 32-bit accumulator can overflow for large values
3. **Fixed array size**: 16×16 array is hardcoded
4. **Simplified memory interface**: Real CPU integration would require proper bus protocol

## References

- [pyCircuit Usage Guide](../../../../docs/USAGE.md)
- [Janus BCC CPU](../bcc/janus_bcc_pyc.py)
- [Systolic Array Architecture](https://en.wikipedia.org/wiki/Systolic_array)
