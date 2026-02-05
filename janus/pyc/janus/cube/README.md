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
| `0x10 - 0x20F` | 512 bytes | Matrix A (activations): 16×16 × 16-bit |
| `0x210 - 0x40F` | 512 bytes | Matrix W (weights): 16×16 × 16-bit |
| `0x410 - 0x80F` | 1024 bytes | Matrix C (results): 16×16 × 32-bit |

### Control Register (0x00)

| Bit | Name | Description |
|-----|------|-------------|
| 0 | START | Start computation (write 1) |
| 1 | RESET | Reset accelerator (write 1) |
| 2-7 | Reserved | - |

### Status Register (0x08)

| Bit | Name | Description |
|-----|------|-------------|
| 0 | DONE | Computation complete (read-only) |
| 1 | BUSY | Accelerator busy (read-only) |
| 2-7 | Reserved | - |

## File Structure

```
cube/
├── __init__.py          # Package marker
├── cube.py              # Top-level module (main build function)
├── cube_types.py        # Dataclasses for register groups
├── cube_consts.py       # Constants (states, addresses)
├── cube_pe.py           # Processing Element implementation
├── cube_array.py        # 16×16 systolic array instantiation
├── cube_control.py      # Control FSM (not used, logic inlined in cube.py)
├── cube_buffer.py       # Input/output buffer management
└── README.md            # This file
```

## Usage

### Building the Module

```python
from pycircuit import Circuit
from janus.cube.cube import build

m = Circuit()
build(m, base_addr=0x80000000)
```

### Emitting MLIR

```bash
PYTHONPATH="binding/python:janus/pyc" python3 -c "
import sys
sys.path.insert(0, 'binding/python')
from pycircuit.cli import main
sys.argv = ['pycircuit', 'emit', 'janus/pyc/janus/cube/cube.py', '-o', '/tmp/cube.pyc']
main()
"
```

### Operation Sequence

1. **Write weights** to addresses `0x210 - 0x40F` (256 × 16-bit values)
2. **Write activations** to addresses `0x10 - 0x20F` (256 × 16-bit values)
3. **Start computation** by writing `0x01` to control register at `0x00`
4. **Poll status** register at `0x08` until DONE bit is set
5. **Read results** from addresses `0x410 - 0x80F` (256 × 32-bit values)

### Timing

- **Load weights**: 1 cycle
- **Compute**: 16 cycles (streaming 16 rows of activations)
- **Drain**: 15 cycles (pipeline depth)
- **Total**: 32 cycles for 16×16 matrix multiplication

## FSM States

| State | Value | Description |
|-------|-------|-------------|
| IDLE | 0 | Waiting for start signal |
| LOAD_WEIGHTS | 1 | Loading weights into PEs |
| COMPUTE | 2 | Streaming activations, computing |
| DRAIN | 3 | Draining pipeline |
| DONE | 4 | Computation complete |

## Implementation Details

### JIT Compilation Constraints

The code is designed to work with pyCircuit's JIT compiler, which has specific constraints:

- **No for-loops with side effects**: All loops that create registers or append to lists are unrolled
- **No tuple unpacking**: Functions return tuples but access uses indexing (`result[0]`, `result[1]`)
- **No multiplication operator**: Currently using addition as placeholder (marked with TODO)
- **Pre-computed addresses**: All address calculations are done at module construction time

### Register Allocation

- **State registers**: 4 registers (state, cycle_count, done, busy)
- **PE registers**: 256 × 2 = 512 registers (weight + accumulator per PE)
- **Weight buffer**: 256 × 16-bit registers
- **Activation buffer**: 256 × 16-bit registers
- **Result buffer**: 256 × 32-bit registers

**Total**: ~1300 registers

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
3. **Fixed array size**: 16×16 array is hardcoded (not parameterizable due to JIT constraints)
4. **Simplified memory interface**: Real CPU integration would require proper bus protocol

## Future Enhancements

- [ ] Add multiplication operator support when available in pyCircuit
- [ ] Add overflow detection and saturation
- [ ] Implement proper AXI/AHB bus interface
- [ ] Add interrupt support for completion notification
- [ ] Support for different matrix sizes (8×8, 32×32)
- [ ] Add support for different data types (int8, float16)

## References

- [pyCircuit Documentation](../../../../docs/USAGE.md)
- [Systolic Array Architecture](https://en.wikipedia.org/wiki/Systolic_array)
- [Janus BCC CPU](../bcc/janus_bcc_pyc.py)

## License

Same as pyCircuit project.
