import pytest
from pycircuit import Circuit
from pycircuit.jit import JitError, compile_module


def test_frontend_emits_only_canonical_select_cmp_and_ssa_shifts() -> None:
    m = Circuit("canonical")
    cond = m.input("cond", width=1)
    lhs = m.input("lhs", width=8)
    rhs = m.input("rhs", width=8)
    m.output("selected", cond.select(lhs, rhs))
    m.output("equal", lhs == rhs)
    m.output("less", lhs.ult(rhs))
    m.output("shifted", lhs << 3)
    mlir = m.emit_mlir()

    assert "pyc.select" in mlir
    assert "pyc.cmp" in mlir
    assert 'predicate = "eq"' in mlir
    assert 'predicate = "ult"' in mlir
    assert "pyc.constant 3 : i2" in mlir
    assert "pyc.shl" in mlir
    for removed in (
        "pyc.mux",
        "pyc.eq",
        "pyc.ult",
        "pyc.slt",
        "pyc.shli",
        "pyc.lshri",
        "pyc.ashri",
    ):
        assert removed not in mlir


def _dynamic_shift(m: Circuit) -> None:
    value = m.input("value", width=8, shape=[2])
    amount = m.input("amount", width=3)
    m.output("left", value << amount)
    m.output("right", value >> amount)


def _vector_shift_amount(m: Circuit) -> None:
    value = m.input("value", width=8, shape=[2])
    amount = m.input("amount", width=3, shape=[2])
    m.output("bad", value << amount)


def _invalid_shift_amount_type(m: Circuit) -> None:
    value = m.input("value", width=8)
    m.output("bad", value << "two")


def test_jit_accepts_typed_scalar_ssa_shift_amount() -> None:
    mlir = compile_module(_dynamic_shift).emit_mlir()
    assert "pyc.shl" in mlir
    assert "pyc.lshr" in mlir


def test_jit_rejects_vector_shift_amount_shape() -> None:
    with pytest.raises(JitError, match="shift amount must be a scalar integer"):
        compile_module(_vector_shift_amount)


def test_jit_rejects_noninteger_shift_amount_type() -> None:
    with pytest.raises(
        JitError, match="shift amount must be an integer or typed scalar signal"
    ):
        compile_module(_invalid_shift_amount_type)
