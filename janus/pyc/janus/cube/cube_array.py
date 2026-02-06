from __future__ import annotations

from pycircuit import Circuit, Wire

from janus.cube.cube_pe import build_pe
from janus.cube.cube_types import PERegs


def build_array(
    m: Circuit,
    *,
    load_weight: Wire,
    compute: Wire,
    weights: list[Wire],  # 256 weights (row-major order, 16-bit each)
    activations: list[Wire],  # 16 activations per cycle (16-bit each)
    pe_array: list[list[PERegs]],  # 16×16 PE registers
) -> list[Wire]:
    """
    Build 16×16 systolic array.

    Returns:
        results: 256 results (row-major order, 32-bit each)
    """
    with m.scope("ARRAY"):
        results = []

        for row in range(16):
            partial_sum = m.const(0, width=32)

            for col in range(16):
                pe_idx = row * 16 + col

                with m.scope(f"r{row}_c{col}"):
                    partial_sum, result = build_pe(
                        m,
                        load_weight=load_weight,
                        compute=compute,
                        weight_in=weights[pe_idx],
                        activation=activations[col],
                        partial_sum_in=partial_sum,
                        pe_regs=pe_array[row][col],
                    )
                    results.append(result)

        return results
