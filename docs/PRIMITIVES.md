# PYC Primitive API (prototype)

This file documents the *single-source* “contract” we want to keep stable as
`pyCircuit` grows: every primitive has a **matching C++ template** and a
**matching Verilog module** with the same name and port names.

The MLIR dialect (`pyc.*` ops) should lower to this primitive layer.

## 1) Common data structures

### 1.1 `Wire<W>`

Represents a combinational value of width `W` bits.

- C++: `pyc::cpp::Wire<W>` (see `library/cpp/pyc_bits.hpp`)
- Verilog: `wire [W-1:0]` (or just `[W-1:0]` ports)

### 1.2 `Reg<W>` (module-like)

Represents a clocked storage element with synchronous reset and clock enable.

- Verilog module: `pyc_reg` (`library/verilog/pyc_reg.v`)
- C++ class: `pyc::cpp::pyc_reg<W>` (`library/cpp/pyc_primitives.hpp`)

Note: although the testbenches in `examples/pycircuit/` are written in SystemVerilog for convenience,
the *design* backend is plain Verilog.

Ports:

- `clk` (i1 / wire)
- `rst` (i1 / wire)
- `en` (i1 / wire)
- `d` (W-bit)
- `init` (W-bit)
- `q` (W-bit)

Semantics (posedge `clk`):

- if `rst`: `q <= init`
- else if `en`: `q <= d`

Repeated structure is authored with ordinary Python list/tuple iteration and
emitted as explicit scalar operations. Typed ACIR aggregates use an exact-width
packed scalar ABI before reaching this primitive layer; the runtime does not
provide a vector-instruction container.

## 2) Primitive operations (combinational)

All combinational primitives have an `eval()` method in C++ and continuous
assign semantics in Verilog.

Note: the current `pycc` emitters typically **inline** these operations
as expressions (Verilog `assign` / C++ `Wire<>` operators) to keep generated
code netlist-like. The `pyc_*` combinational wrappers remain available as a
stable “template library” layer for hand-written designs or future lowering.

### 2.1 `pyc_add` (W-bit)

- Verilog: `module pyc_add #(WIDTH) (a, b, y)`
- C++: `pyc::cpp::pyc_add<W> { a, b, y; eval(); }`

### 2.2 `pyc_mux` (W-bit)

- Verilog: `module pyc_mux #(WIDTH) (sel, a, b, y)`
- C++: `pyc::cpp::pyc_mux<W> { sel, a, b, y; eval(); }`

### 2.3 Bitwise ops (W-bit)

- `pyc_and`: `(a, b) -> y`
- `pyc_or`: `(a, b) -> y`
- `pyc_xor`: `(a, b) -> y`
- `pyc_not`: `(a) -> y`

## 3) Primitive operations (ready/valid)

### 3.1 `pyc_fifo` (single-clock, strict ready/valid)

- Verilog: `module pyc_fifo #(WIDTH, DEPTH) (...)`
- C++: `pyc::cpp::pyc_fifo<Width, Depth>` (`library/cpp/pyc_primitives.hpp`)

Ports (explicit, for simple codegen):

- `clk`, `rst`
- input: `in_valid`, `in_ready`, `in_data`
- output: `out_valid`, `out_ready`, `out_data`

Handshake:

- push when `in_valid && in_ready`
- pop when `out_valid && out_ready`

Notes (prototype):

- `out_data` is defined as 0 when `out_valid == 0` (empty). This keeps
  C++/Verilog equivalence deterministic without requiring the internal storage
  array to be cleared on reset.

Note: this is currently **single-clock**; async FIFO should be a separate
primitive. See `pyc_async_fifo` below.

### 3.2 `pyc_async_fifo` (dual-clock, strict ready/valid)

- Verilog: `module pyc_async_fifo #(WIDTH, DEPTH) (...)` (`library/verilog/pyc_async_fifo.v`)
- C++: `pyc::cpp::pyc_async_fifo<Width, Depth>` (`library/cpp/pyc_async_fifo.hpp`)

Ports:

- write domain: `in_clk`, `in_rst`, `in_valid`, `in_ready`, `in_data`
- read domain: `out_clk`, `out_rst`, `out_valid`, `out_ready`, `out_data`

Notes (prototype constraints):

- `DEPTH` must be a power of two and `>= 2`.
- No combinational cross-domain paths (strict handshake).

## 4) Memory

### 4.1 `pyc_byte_mem` (byte-addressed, prototype)

- Verilog: `module pyc_byte_mem #(ADDR_WIDTH, DATA_WIDTH, DEPTH) (...)` (`library/verilog/pyc_byte_mem.v`)
- C++: `pyc::cpp::pyc_byte_mem<AddrWidth, DataWidth, DepthBytes>` (`library/cpp/pyc_byte_mem.hpp`)

Semantics (prototype):

- Read is combinational: `rdata` reflects `mem[raddr]`.
- Write is synchronous on posedge: when `wvalid`, update bytes selected by `wstrb`.
- Simulation initializes memory bytes to 0 by default (deterministic).
- Reset does not clear memory; testbenches can initialize contents via `memh` (SV) or poke helpers (C++).

### 4.2 `pyc_sync_mem` (1R1W, synchronous read, registered output)

- Verilog: `module pyc_sync_mem #(ADDR_WIDTH, DATA_WIDTH, DEPTH) (...)` (`library/verilog/pyc_sync_mem.v`)
- C++: `pyc::cpp::pyc_sync_mem<AddrWidth, DataWidth, DepthEntries>` (`library/cpp/pyc_sync_mem.hpp`)

Ports:

- `clk`, `rst`
- read: `ren`, `raddr`, `rdata` (registered)
- write: `wvalid`, `waddr`, `wdata`, `wstrb`

Semantics (prototype):

- Read data updates on posedge when `ren` is asserted (registered read).
- Write occurs on posedge when `wvalid` is asserted (byte enables via `wstrb`).
- Read-during-write to the same address returns **old-data** by default (read observes the pre-write value).
- Simulation initializes memory entries to 0 by default (deterministic). Reset does not clear memory contents.

### 4.3 `pyc_sync_mem_dp` (2R1W, synchronous read, registered outputs)

- Verilog: `module pyc_sync_mem_dp #(ADDR_WIDTH, DATA_WIDTH, DEPTH) (...)` (`library/verilog/pyc_sync_mem_dp.v`)
- C++: `pyc::cpp::pyc_sync_mem_dp<AddrWidth, DataWidth, DepthEntries>` (`library/cpp/pyc_sync_mem.hpp`)

Ports:

- `clk`, `rst`
- read0: `ren0`, `raddr0`, `rdata0` (registered)
- read1: `ren1`, `raddr1`, `rdata1` (registered)
- write: `wvalid`, `waddr`, `wdata`, `wstrb`

Semantics (prototype):

- Read-during-write to the same address returns **old-data** by default on both read ports.
- Simulation initializes memory entries to 0 by default (deterministic). Reset does not clear memory contents.

## 5) CDC

### 5.1 `pyc_cdc_sync` (dst-clocked synchronizer)

- Verilog: `module pyc_cdc_sync #(WIDTH, STAGES) (...)` (`library/verilog/pyc_cdc_sync.v`)
- C++: `pyc::cpp::pyc_cdc_sync<Width, Stages>` (`library/cpp/pyc_cdc_sync.hpp`)

Ports:

- `clk`, `rst`
- `in`, `out`

## 6) Debugging / Testbench (C++)

Prototype-only utilities to help with bring-up and debugging:

- Printing: `library/cpp/pyc_print.hpp` defines `operator<<` for `Wire`, `Vec`, and primitives.
- Testbench: `library/cpp/pyc_tb.hpp` provides `pyc::cpp::Testbench<Dut>` (multi-clock ready).
- Tracing: `library/cpp/pyc_vcd.hpp` provides a tiny VCD dumper (usable via `Testbench::enableVcd()`).
- Convenience include: `library/cpp/pyc_debug.hpp`.

Example testbenches are authored with `@testbench` in Python and lowered by `pycc`
from the testbench payload embedded in `.pyc`.

## 7) Generated C++ module-eval caching

When C++ is emitted from MLIR (`pycc --emit=cpp`), hierarchical
`pyc.instance` calls include a default-on input-change cache:

- if all instance inputs are unchanged in a parent `eval()` pass, generated
  code skips that submodule's `eval()`;
- instance outputs are still assigned to parent wires every pass.
- across `tick_commit()`, cache validity is preserved for combinational-only
  submodule hierarchies and invalidated only for stateful submodules.

This is a codegen/runtime performance optimization for large software workloads
on cycle models. To disable it for A/B comparisons, compile generated C++ with:

- `-DPYC_DISABLE_INSTANCE_EVAL_CACHE`

Additional scheduler/cache bisect flags:

- `-DPYC_DISABLE_PRIMITIVE_EVAL_CACHE`
- `-DPYC_DISABLE_SCC_WORKLIST_EVAL`
- `-DPYC_DISABLE_VERSIONED_INPUT_CACHE`

Runtime perf stats controls:

- `PYC_SIM_STATS=1`
- `PYC_SIM_STATS_PATH=<path>`
