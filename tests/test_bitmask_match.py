"""Gate for ASL-style bit-mask pattern matching (ASL alignment TODO T2).

``signal.matches("1xx0")`` is pure front-end sugar: it compiles the pattern to a
``(mask, value)`` pair and expands to ``(signal & mask) == value``, emitting
byte-identical MLIR to the hand-written form. ``in_`` / ``not_in_`` are OR
reductions (and their negation). Both ``Wire`` and ``CycleAwareSignal`` are
supported; the cycle tag is preserved.
"""

from __future__ import annotations

import pytest
from pycircuit import Circuit, CycleAwareCircuit, cas, wire_of
from pycircuit.bitmask import parse_bitmask, parse_bitmask_checked

# --- pure parser ------------------------------------------------------------


@pytest.mark.parametrize(
    "pattern, mask, value, width",
    [
        ("1xx0", 0b1001, 0b1000, 4),
        ("1010", 0b1111, 0b1010, 4),
        ("xxxx", 0b0000, 0b0000, 4),
        ("----", 0b0000, 0b0000, 4),
        ("1(0)x0", 0b1001, 0b1000, 4),  # parenthesized bit is don't-care
        ("1(01)0", 0b1001, 0b1000, 4),  # any bits in parens are don't-care
        ("1000 1010"[:4], 0b1111, 0b1000, 4),
        ("1111_0000", 0b11111111, 0b11110000, 8),  # '_' separator ignored
        ("1 0 x 0", 0b1101, 0b1000, 4),  # spaces ignored
    ],
)
def test_parse_bitmask(pattern, mask, value, width) -> None:
    assert parse_bitmask(pattern) == (mask, value, width)


def test_paren_forms_equivalent() -> None:
    # ASL: '1xx0', '1(0)x0', '1(01)0' all describe the same care/value set.
    assert parse_bitmask("1xx0") == parse_bitmask("1(0)x0") == parse_bitmask("1(01)0")


@pytest.mark.parametrize(
    "pattern, match",
    [
        ("", "no bits"),
        ("12", "invalid character"),
        ("1(0", "unclosed"),
        ("1)0", "unmatched"),
        ("1((0))0", "nested"),
    ],
)
def test_parse_errors(pattern, match) -> None:
    with pytest.raises(ValueError, match=match):
        parse_bitmask(pattern)


def test_parse_checked_width_mismatch() -> None:
    with pytest.raises(ValueError, match="width 4, expected 8"):
        parse_bitmask_checked("1xx0", width=8)


# --- matches == hand-written (sig & mask) == value (byte-level MLIR) ---------


def test_matches_equivalent_to_and_eq() -> None:
    def via_matches() -> str:
        m = Circuit("t")
        s = m.input("s", width=4)
        m.output("y", s.matches("1xx0"))
        return m.emit_mlir()

    def manual() -> str:
        m = Circuit("t")
        s = m.input("s", width=4)
        m.output("y", (s & 0b1001) == 0b1000)
        return m.emit_mlir()

    assert via_matches() == manual()


def test_paren_pattern_same_emission_as_plain() -> None:
    def via(p: str) -> str:
        m = Circuit("t")
        s = m.input("s", width=4)
        m.output("y", s.matches(p))
        return m.emit_mlir()

    assert via("1(0)x0") == via("1xx0")


def test_in_is_or_reduction() -> None:
    def via_in() -> str:
        m = Circuit("t")
        s = m.input("s", width=4)
        m.output("y", s.in_(["1xx0", "0011", "11xx"]))
        return m.emit_mlir()

    def manual() -> str:
        m = Circuit("t")
        s = m.input("s", width=4)
        hit = (
            ((s & 0b1001) == 0b1000)
            | ((s & 0b1111) == 0b0011)
            | ((s & 0b1100) == 0b1100)
        )
        m.output("y", hit)
        return m.emit_mlir()

    assert via_in() == manual()


def test_not_in_is_inverted_in() -> None:
    def via_not_in() -> str:
        m = Circuit("t")
        s = m.input("s", width=4)
        m.output("y", s.not_in_(["1xx0", "0011"]))
        return m.emit_mlir()

    def manual() -> str:
        m = Circuit("t")
        s = m.input("s", width=4)
        hit = ((s & 0b1001) == 0b1000) | ((s & 0b1111) == 0b0011)
        m.output("y", ~hit)
        return m.emit_mlir()

    assert via_not_in() == manual()


def test_matches_result_is_i1() -> None:
    m = Circuit("t")
    s = m.input("s", width=8)
    assert s.matches("1xxxxxx0").width == 1
    assert s.in_(["1xxxxxx0", "0000xxxx"]).width == 1


# --- error handling ---------------------------------------------------------


def test_matches_width_mismatch_raises() -> None:
    m = Circuit("t")
    s = m.input("s", width=4)
    with pytest.raises(ValueError, match="width"):
        s.matches("1x0")  # 3 bits vs width 4


def test_in_empty_raises() -> None:
    m = Circuit("t")
    s = m.input("s", width=4)
    with pytest.raises(ValueError, match="at least one pattern"):
        s.in_([])


# --- cycle-aware support ----------------------------------------------------


def test_cas_matches_preserves_cycle_and_width() -> None:
    m = CycleAwareCircuit("t")
    d = m.create_domain("clk")
    opcode = cas(d, m.input("opcode", width=4))
    hit = opcode.matches("1xx0")
    assert hit.cycle == opcode.cycle
    assert hit._w.width == 1


def test_cas_in_and_not_in_usable_as_output() -> None:
    def top(m, domain):
        opcode = cas(domain, m.input("opcode", width=4))
        m.output("hit", wire_of(opcode.in_(["1xx0", "0011"])))
        m.output("miss", wire_of(opcode.not_in_(["1xx0"])))

    from pycircuit import compile_cycle_aware

    mlir = compile_cycle_aware(top, name="c", eager=True).emit_mlir()
    assert "pyc.and" in mlir  # (sig & mask)
    assert "pyc.cmp" in mlir and 'predicate = "eq"' in mlir
