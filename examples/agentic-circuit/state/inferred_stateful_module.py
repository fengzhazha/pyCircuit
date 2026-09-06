"""Lexical Python state lowers to one reusable stateful module."""

from __future__ import annotations

import agentic_circuit as ac


@ac.module
def accumulator(value: ac.u8) -> ac.u8:
    total: ac.u8 = 0
    total = total + value
    return total


@ac.system
def inferred_stateful_module(left: ac.u8, right: ac.u8) -> tuple[ac.u8, ac.u8]:
    left_total = accumulator(left)
    right_total = accumulator(right)
    return left_total, right_total
