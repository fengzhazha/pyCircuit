"""Gate for explicit signed comparison methods (ASL alignment TODO T6).

ASL forces an explicit ``UInt``/``SInt`` wrapper at every comparison so the
signedness intent is never implicit. PyCircuit already exposes the unsigned set
``ult/ugt/ule/uge`` and signed ``slt``; T6 fills the gap with the remaining
signed methods ``sgt/sle/sge`` so the "always spell out signedness" convention
is fully expressible. These are pure sugar: ``sgt`` must emit the same MLIR as
the equivalent flipped ``slt``, ``sle``/``sge`` the negation of ``sgt``/``slt``.
Both plain ``Wire`` and ``CycleAwareSignal`` (main modeling API) expose them.
"""

from __future__ import annotations

import pytest
from pycircuit import Circuit, CycleAwareCircuit, cas, wire_of

# --- Wire: sgt/sle/sge == hand-written equivalents (MLIR equivalence) -------


def test_sgt_matches_flipped_slt() -> None:
    def via_sgt() -> str:
        m = Circuit("t")
        a = m.input("a", width=8, signed=True)
        b = m.input("b", width=8, signed=True)
        m.output("y", a.sgt(b))
        return m.emit_mlir()

    def manual() -> str:
        m = Circuit("t")
        a = m.input("a", width=8, signed=True)
        b = m.input("b", width=8, signed=True)
        m.output("y", b.slt(a))
        return m.emit_mlir()

    assert via_sgt() == manual()


def test_sle_matches_not_sgt() -> None:
    def via_sle() -> str:
        m = Circuit("t")
        a = m.input("a", width=8, signed=True)
        b = m.input("b", width=8, signed=True)
        m.output("y", a.sle(b))
        return m.emit_mlir()

    def manual() -> str:
        m = Circuit("t")
        a = m.input("a", width=8, signed=True)
        b = m.input("b", width=8, signed=True)
        m.output("y", ~a.sgt(b))
        return m.emit_mlir()

    assert via_sle() == manual()


def test_sge_matches_not_slt() -> None:
    def via_sge() -> str:
        m = Circuit("t")
        a = m.input("a", width=8, signed=True)
        b = m.input("b", width=8, signed=True)
        m.output("y", a.sge(b))
        return m.emit_mlir()

    def manual() -> str:
        m = Circuit("t")
        a = m.input("a", width=8, signed=True)
        b = m.input("b", width=8, signed=True)
        m.output("y", ~a.slt(b))
        return m.emit_mlir()

    assert via_sge() == manual()


def test_signed_compares_emit_slt_op() -> None:
    m = Circuit("t")
    a = m.input("a", width=8, signed=True)
    b = m.input("b", width=8, signed=True)
    m.output("gt", a.sgt(b))
    m.output("le", a.sle(b))
    m.output("ge", a.sge(b))
    mlir = m.emit_mlir()
    # every signed compare lowers to the closed signed predicate.
    assert "pyc.cmp" in mlir and 'predicate = "slt"' in mlir
    assert 'predicate = "ult"' not in mlir


def test_signed_result_is_i1() -> None:
    m = Circuit("t")
    a = m.input("a", width=8, signed=True)
    b = m.input("b", width=8, signed=True)
    assert a.sgt(b).ty == "i1"
    assert a.sle(b).ty == "i1"
    assert a.sge(b).ty == "i1"


def test_sgt_accepts_int_operand() -> None:
    m = Circuit("t")
    a = m.input("a", width=8, signed=True)
    m.output("y", a.sgt(3))
    assert "pyc.cmp" in m.emit_mlir() and 'predicate = "slt"' in m.emit_mlir()


# --- signed vs unsigned actually differ ------------------------------------


def test_signed_and_unsigned_gt_differ_in_mlir() -> None:
    def signed() -> str:
        m = Circuit("t")
        a = m.input("a", width=8)
        b = m.input("b", width=8)
        m.output("y", a.sgt(b))
        return m.emit_mlir()

    def unsigned() -> str:
        m = Circuit("t")
        a = m.input("a", width=8)
        b = m.input("b", width=8)
        m.output("y", a.ugt(b))
        return m.emit_mlir()

    s, u = signed(), unsigned()
    assert 'predicate = "slt"' in s and 'predicate = "ult"' in u
    assert s != u


# --- CycleAwareSignal parity ------------------------------------------------


@pytest.mark.parametrize("op", ["ult", "ugt", "ule", "uge", "slt", "sgt", "sle", "sge"])
def test_cas_exposes_explicit_compares(op) -> None:
    m = CycleAwareCircuit("t")
    d = m.create_domain("clk")
    a = cas(d, m.input("a", width=8, signed=True))
    b = cas(d, m.input("b", width=8, signed=True))
    res = getattr(a, op)(b)
    m.output("y", wire_of(res))
    assert "func.return" in m.emit_mlir()


def test_cas_sgt_matches_flipped_slt() -> None:
    def via_sgt() -> str:
        m = CycleAwareCircuit("t")
        d = m.create_domain("clk")
        a = cas(d, m.input("a", width=8, signed=True))
        b = cas(d, m.input("b", width=8, signed=True))
        m.output("y", wire_of(a.sgt(b)))
        return m.emit_mlir()

    def via_flipped() -> str:
        m = CycleAwareCircuit("t")
        d = m.create_domain("clk")
        a = cas(d, m.input("a", width=8, signed=True))
        b = cas(d, m.input("b", width=8, signed=True))
        m.output("y", wire_of(b.slt(a)))
        return m.emit_mlir()

    assert via_sgt() == via_flipped()


def test_cas_keeps_cycle_on_compare() -> None:
    m = CycleAwareCircuit("t")
    d = m.create_domain("clk")
    a = cas(d, m.input("a", width=8, signed=True))
    d.next()
    b = cas(d, m.input("b", width=8, signed=True))
    res = a.sgt(b)
    # result lives in the later of the two operand cycles
    assert res.cycle == b.cycle
