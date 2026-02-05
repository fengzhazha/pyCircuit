from __future__ import annotations

from pycircuit import Circuit, Wire, jit_inline

from janus.cube.cube_types import PERegs


@jit_inline
def build_pe(
    m: Circuit,
    *,
    load_weight: Wire,  # Enable signal for loading weight
    compute: Wire,  # Enable signal for computation
    weight_in: Wire,  # Weight input (16-bit)
    activation: Wire,  # Activation input (16-bit)
    partial_sum_in: Wire,  # Partial sum from left PE (32-bit)
    pe_regs: PERegs,
) -> tuple[Wire, Wire]:
    """
    Build a single processing element.

    Returns:
        (partial_sum_out, result): Partial sum to right PE, final result
    """
    # Load weight when enabled
    pe_regs.weight.set(weight_in, when=load_weight)

    # Compute: multiply and accumulate
    # NOTE: pyCircuit currently doesn't support multiplication operator
    # Using addition as a placeholder for now (weight + activation instead of weight * activation)
    # TODO: Replace with proper multiplication when supported
    weight = pe_regs.weight.out()
    product = weight.zext(width=32) + activation.zext(width=32)  # Placeholder: should be *
    acc_next = pe_regs.acc.out() + product + partial_sum_in

    pe_regs.acc.set(acc_next, when=compute)

    # Outputs
    partial_sum_out = acc_next
    result = pe_regs.acc.out()

    return partial_sum_out, result
