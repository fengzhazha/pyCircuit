from __future__ import annotations

from dataclasses import dataclass

from pycircuit import Reg


@dataclass(frozen=True)
class CubeState:
    state: Reg  # FSM state (3-bit)
    cycle_count: Reg  # Cycle counter for timing (8-bit)
    done: Reg  # Computation done flag (1-bit)
    busy: Reg  # Busy flag (1-bit)


@dataclass(frozen=True)
class PERegs:
    weight: Reg  # Weight value (16-bit)
    acc: Reg  # Accumulator (32-bit for 16-bit inputs)
