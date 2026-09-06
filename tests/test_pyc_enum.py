"""Gate for type-safe enumerations (ASL alignment TODO T3).

``PycEnum`` gives members 0-based codes and derives a minimal bit ``width``;
``EnumSignal`` (via ``m.input(enum=E)`` / ``domain.signal(enum=E)`` / ``E.bind``)
tags a signal with its enum type so ``x.is_(E.MEMBER)`` expands to
``raw == const(code)`` -- byte-identical to the hand-written form -- while
rejecting comparisons against bare ints or a *different* enum at elaboration
time (ASL enums are not interchangeable with integers).
"""

from __future__ import annotations

import pytest
from pycircuit import (
    Circuit,
    CycleAwareCircuit,
    EnumSignal,
    PycEnum,
    Wire,
    auto,
    cas,
    compile_cycle_aware,
    enumeration,
    wire_of,
)
from pycircuit.enums import coerce_enum_cls, enum_width


class SRType(PycEnum):
    LSL = auto()
    LSR = auto()
    ASR = auto()
    ROR = auto()


class Color(PycEnum):
    RED = auto()
    GREEN = auto()


# --- codes & width ----------------------------------------------------------


def test_auto_codes_are_zero_based() -> None:
    assert [m.value for m in SRType] == [0, 1, 2, 3]


def test_width_ceil_log2() -> None:
    assert SRType.width == 2  # ceil(log2(4))
    assert Color.width == 1  # ceil(log2(2))
    assert SRType.LSL.width == 2  # member-level access


def test_single_member_width_is_one() -> None:
    class One(PycEnum):
        ONLY = auto()

    assert One.width == 1


def test_explicit_codes_width() -> None:
    class Op(PycEnum):
        A = 0
        B = 5  # max code 5 -> 3 bits

    assert Op.width == 3


def test_members_are_not_ints() -> None:
    # ASL enums do not silently convert to integers.
    assert not isinstance(SRType.LSL, int)


def test_enum_width_rejects_empty_and_bad_codes() -> None:
    with pytest.raises(TypeError):
        enum_width(PycEnum)  # no members

    class Neg(PycEnum):
        A = -1

    with pytest.raises(ValueError, match="negative code"):
        Neg.width

    class Str(PycEnum):
        A = "x"

    with pytest.raises(TypeError, match="non-int code"):
        Str.width


def test_coerce_enum_cls_rejects_non_enum() -> None:
    with pytest.raises(TypeError, match="PycEnum subclass"):
        coerce_enum_cls(int)
    with pytest.raises(TypeError, match="PycEnum subclass"):
        coerce_enum_cls(PycEnum)  # empty base


# --- enumeration() functional constructor (ASL one-liner) -------------------


def test_enumeration_matches_asl_syntax() -> None:
    Color = enumeration("Color", "RED", "GREEN", "BLUE")
    assert issubclass(Color, PycEnum)
    assert [m.name for m in Color] == ["RED", "GREEN", "BLUE"]
    assert [m.value for m in Color] == [0, 1, 2]
    assert Color.width == 2
    assert Color.__name__ == "Color"


@pytest.mark.parametrize(
    "arg",
    [
        ("RED GREEN BLUE",),  # single space-separated string
        ("RED, GREEN, BLUE",),  # single comma-separated string
        (["RED", "GREEN", "BLUE"],),  # single list
        ("RED", "GREEN", "BLUE"),  # varargs
    ],
)
def test_enumeration_input_forms_equivalent(arg) -> None:
    Color = enumeration("Color", *arg)
    assert [(m.name, m.value) for m in Color] == [("RED", 0), ("GREEN", 1), ("BLUE", 2)]


def test_enumeration_is_class_form_equivalent() -> None:
    Functional = enumeration("SRType", "LSL LSR ASR ROR")

    def via(E) -> str:
        m = Circuit("t")
        op = m.input("op", enum=E)
        m.output("y", op.is_(E.ASR))
        return m.emit_mlir()

    assert via(Functional) == via(SRType)  # SRType is the class-form definition


def test_enumeration_usable_as_input_enum() -> None:
    Color = enumeration("Color", "RED GREEN BLUE")
    m = Circuit("t")
    c = m.input("c", enum=Color)
    assert isinstance(c, EnumSignal)
    assert c.width == 2
    m.output("is_blue", c.is_(Color.BLUE))
    assert "pyc.cmp" in m.emit_mlir() and 'predicate = "eq"' in m.emit_mlir()


def test_enumeration_single_member() -> None:
    E = enumeration("Solo", "ONLY")
    assert [m.value for m in E] == [0]
    assert E.width == 1


def test_enumeration_errors() -> None:
    with pytest.raises(ValueError, match="at least one member"):
        enumeration("Empty")
    with pytest.raises(ValueError, match="identifier"):
        enumeration("Bad", "1RED")
    with pytest.raises(ValueError, match="duplicate"):
        enumeration("Dup", "RED", "RED")
    with pytest.raises(TypeError, match="non-empty str"):
        enumeration("", "RED")


# --- member.const -----------------------------------------------------------


def test_member_const_on_circuit_returns_wire() -> None:
    m = Circuit("t")
    c = SRType.ASR.const(m)
    assert isinstance(c, Wire)
    assert c.width == 2
    m.output("y", c)
    assert "pyc.constant 2 : i2" in m.emit_mlir()


def test_member_const_on_domain_returns_cas() -> None:
    def top(m, domain):
        c = SRType.ROR.const(domain)  # CycleAwareSignal
        assert c.cycle == 0
        m.output("y", wire_of(c))

    mlir = compile_cycle_aware(top, name="c", eager=True).emit_mlir()
    assert "pyc.constant 3 : i2" in mlir


# --- is_ == raw comparison (byte-level MLIR) --------------------------------


def test_is_equivalent_to_raw_eq() -> None:
    def via_is() -> str:
        m = Circuit("t")
        op = m.input("op", enum=SRType)
        m.output("y", op.is_(SRType.ASR))
        return m.emit_mlir()

    def manual() -> str:
        m = Circuit("t")
        op = m.input("op", width=2)
        m.output("y", op == 2)  # ASR.value == 2
        return m.emit_mlir()

    assert via_is() == manual()


def test_is_not_equivalent_to_raw_ne() -> None:
    def via() -> str:
        m = Circuit("t")
        op = m.input("op", enum=SRType)
        m.output("y", op.is_not(SRType.LSL))
        return m.emit_mlir()

    def manual() -> str:
        m = Circuit("t")
        op = m.input("op", width=2)
        m.output("y", op != 0)
        return m.emit_mlir()

    assert via() == manual()


def test_eq_operator_aliases_is_() -> None:
    def via_op() -> str:
        m = Circuit("t")
        op = m.input("op", enum=SRType)
        m.output("y", op == SRType.ROR)
        return m.emit_mlir()

    def via_is() -> str:
        m = Circuit("t")
        op = m.input("op", enum=SRType)
        m.output("y", op.is_(SRType.ROR))
        return m.emit_mlir()

    assert via_op() == via_is()


def test_is_result_is_i1() -> None:
    m = Circuit("t")
    op = m.input("op", enum=SRType)
    assert op.is_(SRType.LSL).width == 1


# --- type safety ------------------------------------------------------------


def test_compare_with_bare_int_raises() -> None:
    m = Circuit("t")
    op = m.input("op", enum=SRType)
    with pytest.raises(TypeError, match="not comparable"):
        op.is_(0)
    with pytest.raises(TypeError, match="not comparable"):
        _ = op == 2


def test_compare_with_other_enum_raises() -> None:
    m = Circuit("t")
    op = m.input("op", enum=SRType)
    with pytest.raises(TypeError, match="different enum"):
        op.is_(Color.RED)


# --- m.input(enum=E) --------------------------------------------------------


def test_input_enum_sizes_width_and_returns_enum_signal() -> None:
    m = Circuit("t")
    op = m.input("op", enum=SRType)
    assert isinstance(op, EnumSignal)
    assert op.width == 2
    assert op.enum is SRType
    assert isinstance(op.raw, Wire)
    assert op.raw.width == 2


def test_input_enum_width_conflict_raises() -> None:
    m = Circuit("t")
    with pytest.raises(ValueError, match="does not match enum"):
        m.input("op", width=3, enum=SRType)


def test_input_enum_matching_width_ok() -> None:
    m = Circuit("t")
    op = m.input("op", width=2, enum=SRType)  # explicit but consistent
    assert op.width == 2


def test_input_enum_conflicts_with_fields_and_shape() -> None:
    m = Circuit("t")
    with pytest.raises(TypeError, match="cannot be combined"):
        m.input("op", enum=SRType, fields={"a": (1, 0)})
    with pytest.raises(TypeError, match="cannot be combined"):
        m.input("op", enum=SRType, shape=4)


def test_input_enum_port_emits() -> None:
    m = Circuit("t")
    op = m.input("op", enum=SRType)
    m.output("is_ror", op.is_(SRType.ROR))
    mlir = m.emit_mlir()
    assert "pyc.and" not in mlir  # plain eq, no masking
    assert "pyc.cmp" in mlir and 'predicate = "eq"' in mlir


# --- domain.signal(enum=E) register -----------------------------------------


def test_domain_signal_enum_register_assign_member() -> None:
    def top(m, domain):
        st = domain.signal(name="st", enum=SRType)
        st <<= SRType.LSR
        m.output("is_lsr", wire_of(st.is_(SRType.LSR)))

    mlir = compile_cycle_aware(top, name="c", eager=True).emit_mlir()
    assert "pyc.constant 1 : i2" in mlir  # LSR code loaded into reg
    assert "pyc.cmp" in mlir and 'predicate = "eq"' in mlir


def test_domain_signal_enum_cross_enum_assign_raises() -> None:
    def top(m, domain):
        st = domain.signal(name="st", enum=SRType)
        st <<= Color.RED

    with pytest.raises(TypeError, match="cannot assign"):
        compile_cycle_aware(top, name="c", eager=True).emit_mlir()


def test_domain_signal_enum_conflicts_with_fields() -> None:
    def top(m, domain):
        domain.signal(name="st", enum=SRType, fields={"a": (1, 0)})

    with pytest.raises(TypeError, match="cannot be combined"):
        compile_cycle_aware(top, name="c", eager=True).emit_mlir()


# --- E.bind on an arbitrary signal ------------------------------------------


def test_bind_tags_existing_signal() -> None:
    m = Circuit("t")
    raw = m.input("op", width=2)
    op = SRType.bind(raw)
    assert isinstance(op, EnumSignal)
    assert op.raw is raw
    m.output("y", op.is_(SRType.ASR))
    assert "pyc.cmp" in m.emit_mlir() and 'predicate = "eq"' in m.emit_mlir()


def test_bind_on_cas_preserves_cycle() -> None:
    m = CycleAwareCircuit("t")
    d = m.create_domain("clk")
    op = SRType.bind(cas(d, m.input("op", width=2)))
    hit = op.is_(SRType.LSL)
    assert hit.cycle == op.raw.cycle
    assert hit._w.width == 1
