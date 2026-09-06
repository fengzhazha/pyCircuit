"""Gate for assertion-style narrowing cast (ASL alignment TODO T4, ``expr as ty``).

``x.as_(width=w)`` (alias ``x.assert_fits(width=w)``) is a *checked* narrowing:
it emits a simulation-time ``pyc.assert`` that the truncated-away high bits are
zero, then returns ``trunc(w)``. In synthesis the assertion is droppable, so the
data path degrades to a plain ``trunc`` (zero-cost contract). Widening is
rejected; equal width is a no-op. ``Wire`` and ``CycleAwareSignal`` are both
supported (the cycle tag is preserved).
"""

from __future__ import annotations

import pytest
from pycircuit import Circuit, CycleAwareCircuit, cas, compile_cycle_aware, wire_of

# --- narrowing: assert(high bits == 0) + trunc ------------------------------


def test_as_emits_assert_and_trunc() -> None:
    m = Circuit("t")
    x = m.input("x", width=8)
    m.output("y", x.as_(width=4))
    mlir = m.emit_mlir()
    assert "pyc.extract %x {lsb = 4, msb = 7} : i8 -> i4" in mlir  # high bits
    assert "pyc.assert" in mlir
    assert "pyc.trunc %x : i8 -> i4" in mlir


def test_as_assertion_is_high_bits_zero() -> None:
    def via_as() -> str:
        m = Circuit("t")
        x = m.input("x", width=8)
        m.output("y", x.as_(width=4))
        return m.emit_mlir()

    def manual() -> str:
        m = Circuit("t")
        x = m.input("x", width=8)
        m.assert_(x[4:8] == 0, msg="as_: value does not fit in 4 bits")
        m.output("y", x.trunc(width=4))
        return m.emit_mlir()

    assert via_as() == manual()


def test_as_data_path_is_plain_trunc() -> None:
    # T4.2: the *returned value* is exactly a trunc of the input (the assertion
    # is side-effecting scaffolding that synthesis can drop).
    m = Circuit("t")
    x = m.input("x", width=8)
    y = x.as_(width=4)
    z = x.trunc(width=4)
    assert y.width == z.width == 4


def test_as_result_width() -> None:
    m = Circuit("t")
    x = m.input("x", width=16)
    assert x.as_(width=5).width == 5


def test_assert_fits_is_alias_of_as_() -> None:
    def via(method: str) -> str:
        m = Circuit("t")
        x = m.input("x", width=8)
        y = getattr(x, method)(width=4)
        m.output("y", y)
        return m.emit_mlir()

    assert via("as_") == via("assert_fits")


def test_as_custom_msg() -> None:
    m = Circuit("t")
    x = m.input("x", width=8)
    m.output("y", x.as_(width=4, msg="opcode must be 4-bit"))
    assert 'msg = "opcode must be 4-bit"' in m.emit_mlir()


# --- edge cases -------------------------------------------------------------


def test_as_equal_width_is_noop() -> None:
    m = Circuit("t")
    x = m.input("x", width=8)
    y = x.as_(width=8)
    assert y is x
    m.output("y", y)
    assert "pyc.assert" not in m.emit_mlir()  # nothing to assert


def test_as_widen_raises() -> None:
    m = Circuit("t")
    x = m.input("x", width=4)
    with pytest.raises(ValueError, match="cannot widen"):
        x.as_(width=8)


def test_as_zero_width_raises() -> None:
    m = Circuit("t")
    x = m.input("x", width=8)
    with pytest.raises(ValueError, match="must be > 0"):
        x.as_(width=0)


# --- value-range / value-set assertions (ASL `as integer{...}`) -------------


def test_as_range_equivalent_to_manual() -> None:
    def via_as() -> str:
        m = Circuit("t")
        x = m.input("x", width=8)
        m.output("y", x.as_(range=(2, 9)))
        return m.emit_mlir()

    def manual() -> str:
        m = Circuit("t")
        x = m.input("x", width=8)
        m.assert_(x.uge(2) & x.ule(9), msg="assert_range: value not in [2, 9]")
        m.output("y", x)
        return m.emit_mlir()

    assert via_as() == manual()


def test_as_range_returns_self_unchanged_width() -> None:
    m = Circuit("t")
    x = m.input("x", width=8)
    y = x.as_(range=(0, 3))
    assert y is x  # value/width unchanged; only a contract
    assert "pyc.assert" in m.emit_mlir()


def test_as_range_lower_zero_skips_lo_check() -> None:
    m = Circuit("t")
    x = m.input("x", width=8)
    x.as_(range=(0, 5))
    mlir = m.emit_mlir()
    # lo == 0 is trivially satisfied -> only the upper bound is checked
    assert mlir.count("pyc.assert") == 1
    assert "pyc.cmp" in mlir and 'predicate = "ult"' in mlir


def test_as_range_full_cover_emits_no_assert() -> None:
    m = Circuit("t")
    x = m.input("x", width=4)
    x.as_(range=(0, 15))  # covers all 4-bit values
    assert "pyc.assert" not in m.emit_mlir()


def test_as_range_errors() -> None:
    m = Circuit("t")
    x = m.input("x", width=4)
    with pytest.raises(ValueError, match="empty range"):
        x.as_(range=(5, 2))
    with pytest.raises(ValueError, match="lower bound must be"):
        x.as_(range=(-1, 3))
    with pytest.raises(ValueError, match="exceeds"):
        x.as_(range=(0, 99))  # 99 > 15


def test_as_values_equivalent_to_manual() -> None:
    def via_as() -> str:
        m = Circuit("t")
        x = m.input("x", width=8)
        m.output("y", x.as_(values=[0, 5, 10]))
        return m.emit_mlir()

    def manual() -> str:
        m = Circuit("t")
        x = m.input("x", width=8)
        cond = (x == 0) | (x == 5) | (x == 10)
        m.assert_(cond, msg="assert_in: value not in [0, 5, 10]")
        m.output("y", x)
        return m.emit_mlir()

    assert via_as() == manual()


def test_assert_in_and_assert_range_named_methods() -> None:
    m = Circuit("t")
    x = m.input("x", width=8)
    assert x.assert_range(2, 9) is x
    assert x.assert_in([1, 2]) is x
    assert m.emit_mlir().count("pyc.assert") == 2


def test_as_values_errors() -> None:
    m = Circuit("t")
    x = m.input("x", width=4)
    with pytest.raises(ValueError, match="at least one value"):
        x.as_(values=[])
    with pytest.raises(ValueError, match="out of range"):
        x.as_(values=[3, 99])


def test_as_requires_exactly_one_kind() -> None:
    m = Circuit("t")
    x = m.input("x", width=8)
    with pytest.raises(TypeError, match="exactly one"):
        x.as_()
    with pytest.raises(TypeError, match="exactly one"):
        x.as_(width=4, range=(0, 3))
    with pytest.raises(TypeError, match="exactly one"):
        x.as_(2, width=4)  # positional value + width
    with pytest.raises(TypeError, match="not both"):
        x.as_(2, values=[3])  # positional + values=


def test_as_positional_values_shorthand() -> None:
    # x.as_(2) / x.as_(2, 3) / x.as_([2, 3]) all mean "assert x in {...}".
    def via(call) -> str:
        m = Circuit("t")
        x = m.input("x", width=8)
        call(x)
        m.output("x", x)
        return m.emit_mlir()

    assert via(lambda x: x.as_(2)) == via(lambda x: x.as_(values=[2]))
    assert via(lambda x: x.as_(2, 3)) == via(lambda x: x.as_(values=[2, 3]))
    assert via(lambda x: x.as_([2, 3])) == via(lambda x: x.as_(values=[2, 3]))


def test_as_single_value_asserts_equality() -> None:
    def via_as() -> str:
        m = Circuit("t")
        x = m.input("x", width=8)
        m.output("y", x.as_(2))
        return m.emit_mlir()

    def manual() -> str:
        m = Circuit("t")
        x = m.input("x", width=8)
        m.assert_(x == 2, msg="assert_in: value not in [2]")
        m.output("y", x)
        return m.emit_mlir()

    assert via_as() == manual()


# --- cycle-aware support ----------------------------------------------------


def test_cas_as_preserves_cycle_and_width() -> None:
    m = CycleAwareCircuit("t")
    d = m.create_domain("clk")
    x = cas(d, m.input("x", width=8))
    y = x.as_(width=4)
    assert y.cycle == x.cycle
    assert y._w.width == 4


def test_cas_as_usable_in_compile() -> None:
    def top(m, domain):
        x = cas(domain, m.input("x", width=8))
        m.output("y", wire_of(x.as_(width=4)))

    mlir = compile_cycle_aware(top, name="c", eager=True).emit_mlir()
    assert "pyc.assert" in mlir
    assert "pyc.trunc" in mlir


def test_cas_as_range_preserves_cycle() -> None:
    m = CycleAwareCircuit("t")
    d = m.create_domain("clk")
    x = cas(d, m.input("x", width=8))
    y = x.as_(range=(1, 7))
    assert y is x  # value unchanged, cycle kept
    assert y.cycle == x.cycle


def test_cas_as_values_in_compile() -> None:
    def top(m, domain):
        x = cas(domain, m.input("x", width=8))
        m.output("y", wire_of(x.as_(values=[3, 4, 5])))

    mlir = compile_cycle_aware(top, name="c", eager=True).emit_mlir()
    assert "pyc.assert" in mlir
    assert "pyc.cmp" in mlir and 'predicate = "eq"' in mlir
