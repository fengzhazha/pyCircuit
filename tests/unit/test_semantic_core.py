from __future__ import annotations

import pytest
from _pycircuit_semantics import (
    ArrayType,
    BitfieldLayout,
    BitfieldLayoutError,
    BitsType,
    BoolType,
    ClosedInterval,
    Constant,
    ConstraintError,
    EnumType,
    FiniteSet,
    StructType,
    TupleType,
    Unknown,
    ValueConstraint,
    ValueField,
    ValueTypeError,
    constraint_for_type,
    finite,
    finite_values,
    is_exhaustive,
    join,
    meet,
    parse_bitmask,
    parse_bitmask_checked,
    prove_within,
    transfer_bits,
    transfer_compare,
    transfer_static_binary,
)

pytestmark = pytest.mark.unit


@pytest.mark.parametrize(
    "pattern, expected",
    [
        ("1xx0", (0b1001, 0b1000, 4)),
        ("1(01)0", (0b1001, 0b1000, 4)),
        ("1111_0000", (0xFF, 0xF0, 8)),
        ("----", (0, 0, 4)),
    ],
)
def test_shared_bitmask_parser_is_msb_first(
    pattern: str, expected: tuple[int, int, int]
) -> None:
    assert parse_bitmask(pattern) == expected


def test_shared_bitmask_parser_rejects_invalid_or_wrong_width_patterns() -> None:
    with pytest.raises(ValueError, match="invalid character"):
        parse_bitmask("10q1")
    with pytest.raises(ValueError, match="width 4, expected 5"):
        parse_bitmask_checked("10x1", width=5)
    with pytest.raises(ValueError, match="basic bit-mask pattern"):
        parse_bitmask_checked("1Xx0", width=4, extended=False)
    with pytest.raises(ValueError, match="basic bit-mask pattern"):
        parse_bitmask_checked("1_0x", width=3, extended=False)


def test_bitfield_layout_is_order_independent_and_immutable() -> None:
    first = BitfieldLayout(
        32,
        {"opcode": (31, 26), "rd": (25, 21), "imm26": (25, 0)},
    )
    second = BitfieldLayout(
        32,
        {"imm26": (25, 0), "rd": (25, 21), "opcode": (31, 26)},
    )

    assert first.fingerprint == second.fingerprint
    assert hash(first) == hash(second)
    assert first.field_slices() == {
        "imm26": (0, 26),
        "opcode": (26, 6),
        "rd": (21, 5),
    }
    with pytest.raises(TypeError):
        first.fields["new"] = (1, 0)  # type: ignore[index]


def test_bitfield_layout_allows_read_overlap_but_rejects_write_overlap() -> None:
    layout = BitfieldLayout(32, {"rd": (25, 21), "imm26": (25, 0)})

    assert layout.checked_writes(["rd"]) == ((21, 25, "rd"),)
    with pytest.raises(BitfieldLayoutError, match="overlap"):
        layout.checked_writes(["rd", "imm26"])


@pytest.mark.parametrize("width", [True, "8", 0])
def test_bitfield_layout_requires_an_exact_positive_integer_width(
    width: object,
) -> None:
    with pytest.raises(BitfieldLayoutError, match="width"):
        BitfieldLayout(width, {"field": (0, 0)})


def test_pycircuit_and_agentic_wrappers_share_exact_schema_identity() -> None:
    from agentic_circuit import BitfieldSpec as AgenticBitfieldSpec
    from pycircuit import BitfieldSpec as PyCircuitBitfieldSpec

    fields = {"opcode": (31, 26), "rd": (25, 21), "imm17": (20, 4)}
    agentic = AgenticBitfieldSpec(width=32, fields=fields)
    pycircuit = PyCircuitBitfieldSpec(width=32, fields=fields)

    assert agentic.fingerprint == pycircuit.fingerprint
    assert agentic.field_slices() == pycircuit.field_slices()


def test_recursive_value_types_have_stable_nominal_and_structural_identity() -> None:
    mode = EnumType("Mode", ("scalar", "vector", "matrix"))
    lane = StructType(
        "Lane",
        (
            ValueField("valid", BoolType()),
            ValueField("payload", BitsType(17)),
            ValueField("mode", mode),
        ),
    )
    pair = TupleType((BitsType(3), BitsType(5)))
    lanes = ArrayType(4, lane)
    packet = StructType(
        "Packet",
        (
            ValueField("lanes", lanes),
            ValueField("selector", pair),
        ),
    )

    assert mode.encoding_width == 2
    assert lane.bit_width() == 20
    assert packet.bit_width() == 88
    assert pair.mlir() == "tuple<i3, i5>"
    assert lanes.mlir() == "!ac.value_array<4 x !ac.struct<@types::@Lane>>"
    assert packet.mlir() == "!ac.struct<@types::@Packet>"
    assert packet == StructType("Packet", packet.fields)
    assert packet.fingerprint == StructType("Packet", packet.fields).fingerprint
    assert packet.fingerprint != StructType("OtherPacket", packet.fields).fingerprint


def test_bool_and_u1_are_distinct_descriptors_with_current_i1_lowering() -> None:
    logical = BoolType()
    bit = BitsType(1)

    assert logical != bit
    assert logical.fingerprint != bit.fingerprint
    assert logical.mlir() == bit.mlir() == "i1"


def test_value_type_identity_is_independent_of_mlir_symbol_scope() -> None:
    mode = EnumType("Mode", ("IDLE", "RUN"))
    header = StructType(
        "Header",
        (
            ValueField("mode", mode),
            ValueField("opcode", BitsType(6)),
        ),
    )
    descriptors = (
        mode,
        header,
        TupleType((mode, header, BitsType(3))),
        ArrayType(2, header),
    )

    for descriptor in descriptors:
        assert descriptor.mlir(scope="left") != descriptor.mlir(scope="right")
        duplicate = type(descriptor)(
            **{
                field.name: getattr(descriptor, field.name)
                for field in descriptor.__dataclass_fields__.values()
            }
        )
        assert descriptor == duplicate
        assert hash(descriptor) == hash(duplicate)
        assert descriptor.canonical() == duplicate.canonical()
        assert descriptor.fingerprint == duplicate.fingerprint
        assert "left" not in repr(descriptor.canonical())
        assert "right" not in repr(descriptor.canonical())


@pytest.mark.parametrize(
    "factory",
    [
        lambda: BitsType(0),
        lambda: EnumType("Empty", ()),
        lambda: StructType("Empty", ()),
        lambda: TupleType(()),
        lambda: ArrayType(0, BitsType(1)),
    ],
)
def test_recursive_value_types_reject_empty_or_zero_shapes(factory: object) -> None:
    with pytest.raises(ValueTypeError):
        factory()  # type: ignore[operator]


def test_bounded_constraints_have_canonical_identity_and_typed_atoms() -> None:
    values = FiniteSet((3, 1, 3, True, "RUN"))

    assert values.values == (True, 1, 3, "RUN")
    assert values == FiniteSet(("RUN", True, 3, 1))
    assert values.fingerprint == FiniteSet((1, 3, "RUN", True)).fingerprint
    assert Constant(True) != Constant(1)
    assert FiniteSet((True,)) != FiniteSet((1,))
    assert Constant(True).fingerprint != Constant(1).fingerprint
    assert Unknown().canonical() == {"kind": "unknown", "version": 1}
    with pytest.raises(ConstraintError, match="cardinality"):
        FiniteSet(tuple(range(65)))
    assert isinstance(finite(list(range(65))), Unknown)
    assert join(FiniteSet(tuple(range(64))), Constant(64)) == ClosedInterval(0, 64)


def test_bounded_constraint_join_meet_and_range_proofs_are_conservative() -> None:
    assert join(Constant(1), Constant(3)) == FiniteSet((1, 3))
    assert join(ClosedInterval(2, 4), Constant(8)) == ClosedInterval(2, 8)
    assert join(ClosedInterval(2, 4), Constant(3)) == ClosedInterval(2, 4)
    assert isinstance(join(Unknown(), Constant(1)), Unknown)
    assert meet(ClosedInterval(0, 7), ClosedInterval(4, 12)) == FiniteSet((4, 5, 6, 7))
    assert meet(FiniteSet((1, 4, 9)), ClosedInterval(0, 4)) == FiniteSet((1, 4))
    assert meet(Constant("RUN"), Constant("WAIT")) == FiniteSet(())
    assert prove_within(ClosedInterval(0, 3), 0, 3)
    assert not prove_within(ClosedInterval(0, 4), 0, 3)
    with pytest.raises(ConstraintError, match="closed integer interval"):
        prove_within(Constant(0), 2, 1)


def test_bounded_constraint_lattice_is_idempotent_and_commutative() -> None:
    constraints = (
        Constant(1),
        FiniteSet((1, 3)),
        ClosedInterval(0, 7),
        Unknown(),
    )
    for left in constraints:
        assert join(left, left) == left
        assert meet(left, left) == left
        for right in constraints:
            assert join(left, right) == join(right, left)
            assert meet(left, right) == meet(right, left)


def test_bounded_constraint_transfer_covers_arithmetic_shift_and_compare() -> None:
    assert transfer_static_binary(
        "add", ClosedInterval(1, 3), Constant(2)
    ) == FiniteSet((3, 4, 5))
    assert transfer_static_binary(
        "mul", ClosedInterval(-2, 3), ClosedInterval(4, 5), finite_limit=2
    ) == ClosedInterval(-10, 15)
    assert transfer_static_binary(
        "mod", ClosedInterval(0, 100), Constant(8)
    ) == ClosedInterval(0, 7)
    assert transfer_static_binary(
        "shl", ClosedInterval(1, 3), Constant(2)
    ) == FiniteSet((4, 8, 12))
    assert transfer_compare("lt", ClosedInterval(0, 3), ClosedInterval(4, 7)) == (
        Constant(True)
    )
    assert transfer_compare("eq", ClosedInterval(0, 3), ClosedInterval(8, 9)) == (
        Constant(False)
    )
    assert transfer_compare("eq", ClosedInterval(0, 3), ClosedInterval(2, 5)) == (
        FiniteSet((False, True))
    )
    assert isinstance(
        transfer_static_binary("shl", Constant(1), Constant(1 << 20)), Unknown
    )


def test_typed_bitvector_transfer_preserves_wrap_and_overshift_semantics() -> None:
    assert transfer_bits("add", Constant(7), Constant(1), width=3) == Constant(0)
    assert transfer_bits("sub", Constant(0), Constant(1), width=3) == Constant(7)
    assert transfer_bits("shl", Constant(1), Constant(3), width=3) == Constant(0)
    assert transfer_bits("shr", Constant(7), Constant(3), width=3) == Constant(0)
    assert transfer_bits(
        "and", ClosedInterval(0, 7), Constant(3), width=3, finite_limit=2
    ) == ClosedInterval(0, 3)
    assert transfer_bits(
        "add", ClosedInterval(0, 7), Constant(1), width=3, finite_limit=2
    ) == ClosedInterval(0, 7)
    with pytest.raises(ConstraintError, match=r"\[1, 64\]"):
        transfer_bits("add", Constant(0), Constant(0), width=0)


def test_value_type_constraints_cover_bits_bool_and_enum_exhaustiveness() -> None:
    mode = EnumType("Mode", ("IDLE", "RUN", "WAIT"))

    assert constraint_for_type(BitsType(3)) == ClosedInterval(0, 7)
    assert constraint_for_type(BoolType()) == FiniteSet((False, True))
    assert constraint_for_type(mode) == FiniteSet(("IDLE", "RUN", "WAIT"))
    assert isinstance(constraint_for_type(TupleType((BitsType(1),))), Unknown)
    assert is_exhaustive(constraint_for_type(mode), {"IDLE", "RUN", "WAIT"})
    assert not is_exhaustive(constraint_for_type(mode), {"IDLE", "RUN"})
    assert finite_values(ClosedInterval(2, 4)) == (2, 3, 4)
    assert finite_values(ClosedInterval(0, 100)) is None
    fact = ValueConstraint(BitsType(3), ClosedInterval(0, 7))
    assert fact.canonical()["type"] == BitsType(3).canonical()
    assert (
        fact.fingerprint
        == ValueConstraint(BitsType(3), ClosedInterval(0, 7)).fingerprint
    )
    with pytest.raises(ConstraintError, match="bits constraint"):
        ValueConstraint(BitsType(3), Constant(8))
    with pytest.raises(ConstraintError, match="unknown member"):
        ValueConstraint(mode, Constant("INVALID"))
