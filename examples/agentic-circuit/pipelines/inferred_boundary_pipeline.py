"""A serial rule pipeline whose system signature defines its boundaries."""

import agentic_circuit as ac


@ac.rule
def increment(value):
    return value + 1


@ac.system
def inferred_boundary_pipeline(value: ac.u8) -> ac.u8:
    result = increment(value)
    return result
