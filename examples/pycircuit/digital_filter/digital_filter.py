"""4-tap Feed-Forward (FIR) Filter — pyCircuit V6 cycle-aware.

Implements:
    y[n] = c0·x[n] + c1·x[n-1] + c2·x[n-2] + c3·x[n-3]
"""

from __future__ import annotations

from pycircuit import (
    CycleAwareCircuit,
    CycleAwareDomain,
    cas,
    compile_cycle_aware,
    sext,
    wire_of,
)


def build(
    m: CycleAwareCircuit,
    domain: CycleAwareDomain,
    *,
    TAPS: int = 4,
    DATA_W: int = 16,
    COEFF_W: int = 16,
    COEFFS: tuple = (1, 2, 3, 4),
) -> None:
    assert len(COEFFS) == TAPS, f"need {TAPS} coefficients, got {len(COEFFS)}"

    GUARD = (TAPS - 1).bit_length()
    ACC_W = DATA_W + COEFF_W + GUARD

    x_in = cas(domain, m.input("x_in", width=DATA_W), cycle=0)
    x_valid = cas(domain, m.input("x_valid", width=1), cycle=0)

    delay_states = [
        domain.signal(width=DATA_W, reset_value=0, name=f"delay_{i}")
        for i in range(1, TAPS)
    ]
    taps = [x_in, *delay_states]
    products = []
    for tap, coefficient in zip(taps, COEFFS, strict=True):
        tap_ext = cas(domain, sext(wire_of(tap), width=ACC_W), cycle=tap.cycle)
        coefficient_value = cas(
            domain,
            domain.create_const(
                int(coefficient), width=ACC_W, signed=int(coefficient) < 0
            ),
            cycle=0,
        )
        products.append(tap_ext * coefficient_value)
    y_comb = products[0]
    for i in range(1, TAPS):
        y_comb = y_comb + products[i]

    y_out_state = domain.signal(width=ACC_W, reset_value=0, name="y_out_reg")
    y_valid_state = domain.signal(width=1, reset_value=0, name="y_valid_reg")

    m.output("y_out", wire_of(y_out_state))
    m.output("y_valid", wire_of(y_valid_state))

    domain.next()

    delay_states[0].assign(x_in, when=x_valid)
    for i in range(1, len(delay_states)):
        delay_states[i].assign(delay_states[i - 1], when=x_valid)

    y_out_state.assign(y_comb, when=x_valid)
    y_valid_state <<= x_valid


build.__pycircuit_name__ = "digital_filter"

if __name__ == "__main__":
    print(
        compile_cycle_aware(
            build,
            name="digital_filter",
            eager=True,
            TAPS=4,
            DATA_W=16,
            COEFF_W=16,
            COEFFS=(1, 2, 3, 4),
        ).emit_mlir()
    )
