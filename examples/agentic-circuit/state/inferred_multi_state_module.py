"""Multiple lexical variables commit atomically in one reusable module."""

from __future__ import annotations

import agentic_circuit as ac


@ac.module
def tally(value: ac.u8) -> ac.u8:
    count: ac.u8 = 0
    total: ac.u8 = 0
    count = count + 1
    total = total + value
    return total + count


@ac.system
def inferred_multi_state_module(left: ac.u8, right: ac.u8) -> tuple[ac.u8, ac.u8]:
    left_result = tally(left)
    right_result = tally(right)
    return left_result, right_result
