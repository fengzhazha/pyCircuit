from __future__ import annotations

# Array size
ARRAY_SIZE = 16  # 16x16 array

# Control states
ST_IDLE = 0
ST_LOAD_WEIGHTS = 1
ST_COMPUTE = 2
ST_DRAIN = 3
ST_DONE = 4

# Memory-mapped register addresses (relative to base)
ADDR_CONTROL = 0x00  # Control register (start, reset)
ADDR_STATUS = 0x08  # Status register (done, busy)
ADDR_MATRIX_A = 0x10  # Matrix A base (512 bytes: 16×16 × 16-bit)
ADDR_MATRIX_W = 0x210  # Matrix W base (512 bytes: 16×16 × 16-bit)
ADDR_MATRIX_C = 0x410  # Matrix C base (1024 bytes: 16×16 × 32-bit)

# Control bits
CTRL_START = 0x01
CTRL_RESET = 0x02

# Status bits
STATUS_DONE = 0x01
STATUS_BUSY = 0x02
