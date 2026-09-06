"""Typed module calls lower to reusable QueueGraph specializations."""

from __future__ import annotations

import agentic_circuit as ac


@ac.module
def increment(value: ac.u8) -> ac.u8:
    return value + 1


@ac.system
def inferred_module_pipeline(left: ac.u8, right: ac.u8) -> tuple[ac.u8, ac.u8]:
    left_result = increment(left)
    right_result = increment(right)
    return left_result, right_result
