from __future__ import annotations

import importlib.util
import subprocess
from pathlib import Path

import pycircuit
import pycircuit.v6 as pyc6
import pytest

pytestmark = pytest.mark.unit


def test_cycle_aware_frontend_is_the_pyc6_surface() -> None:
    assert pycircuit.CycleAwareSignal is pyc6.CycleAwareSignal
    assert pycircuit.CycleAwareDomain is pyc6.CycleAwareDomain
    assert pycircuit.compile_cycle_aware is pyc6.compile_cycle_aware
    assert not hasattr(pycircuit, "StateSignal")
    assert not hasattr(pycircuit, "priority_mux")


def test_pyc6_data_model_is_scalar_only() -> None:
    from pycircuit.data import Data

    with pytest.raises(ValueError, match="unsupported type literal"):
        Data.from_str("vector<2xi8>")


def test_priority_encode_is_vendor_neutral_on_the_public_pyc6_surface() -> None:
    circuit = pycircuit.CycleAwareCircuit("priority")
    domain = circuit.create_domain("clk")
    mask = domain.create_signal("mask", width=13)

    result = pycircuit.priority_encode(pycircuit.cas(domain, mask), order="high")

    assert result.index.width == 4
    assert result.valid.width == 1
    assert result.index.cycle == result.valid.cycle == domain.cycle_index
    mlir = circuit.emit_mlir()
    assert "pyc.priority_encode" in mlir
    assert 'order = "high"' in mlir
    assert "basejump" not in mlir.lower()


def test_priority_encode_rejects_noncanonical_order() -> None:
    circuit = pycircuit.CycleAwareCircuit("bad_priority")
    domain = circuit.create_domain("clk")
    mask = pycircuit.cas(domain, domain.create_signal("mask", width=4))

    with pytest.raises(ValueError, match="'low' or 'high'"):
        pycircuit.priority_encode(mask, order="middle")


def test_popcount_is_exact_width_and_vendor_neutral_on_pyc6() -> None:
    circuit = pycircuit.CycleAwareCircuit("popcount")
    domain = circuit.create_domain("clk")
    value = pycircuit.cas(domain, domain.create_signal("value", width=13))

    count = pycircuit.popcount(value)

    assert count.width == 4
    assert count.cycle == domain.cycle_index
    mlir = circuit.emit_mlir()
    assert "pyc.popcount" in mlir
    assert "bsg_" not in mlir.lower()


def test_structural_circuit_popcount_emits_the_same_semantic_op() -> None:
    circuit = pycircuit.Circuit("structural_popcount")
    value = circuit.input("value", width=13)

    count = circuit.popcount(value)
    circuit.output("count", count)

    assert count.width == 4
    mlir = circuit.emit_mlir()
    assert "pyc.popcount" in mlir
    assert "bsg_" not in mlir.lower()


def test_count_leading_zeros_is_exact_width_and_cycle_aware() -> None:
    circuit = pycircuit.CycleAwareCircuit("count_leading_zeros")
    domain = circuit.create_domain("clk")
    value = pycircuit.cas(domain, domain.create_signal("value", width=13))

    count = pycircuit.count_leading_zeros(value)

    assert count.width == 4
    assert count.cycle == domain.cycle_index
    mlir = circuit.emit_mlir()
    assert "pyc.count_zeros" in mlir
    assert 'direction = "leading"' in mlir
    assert "lzc" not in mlir.lower()


def test_structural_count_leading_zeros_emits_the_same_semantic_op() -> None:
    circuit = pycircuit.Circuit("structural_count_leading_zeros")
    value = circuit.input("value", width=13)

    count = circuit.count_leading_zeros(value)
    circuit.output("count", count)

    assert count.width == 4
    mlir = circuit.emit_mlir()
    assert "pyc.count_zeros" in mlir
    assert 'direction = "leading"' in mlir


def test_count_trailing_zeros_uses_the_same_parameterized_semantic_family() -> None:
    circuit = pycircuit.CycleAwareCircuit("count_trailing_zeros")
    domain = circuit.create_domain("clk")
    value = pycircuit.cas(domain, domain.create_signal("value", width=13))

    count = pycircuit.count_trailing_zeros(value)

    assert count.width == 4
    assert count.cycle == domain.cycle_index
    mlir = circuit.emit_mlir()
    assert "pyc.count_zeros" in mlir
    assert 'direction = "trailing"' in mlir


def test_pyc5_module_is_not_shipped_as_a_compatibility_surface() -> None:
    assert importlib.util.find_spec("pycircuit.v5") is None


def test_runtime_and_trace_identifiers_are_pyc6_only() -> None:
    root = Path(__file__).resolve().parents[2]
    contract_files = (
        root / "CMakeLists.txt",
        root / "library/cpp/CMakeLists.txt",
        root / "library/cpp/pyc_runtime.cpp",
        root / "library/cpp/pyc_trace_bin.hpp",
        root / "python/pycircuit/src/pycircuit/cli.py",
        root / "compiler/mlir/tools/pycc.cpp",
        root / "flows/tools/gen_cmake_from_manifest.py",
        root / "flows/tools/dump_pyctrace.py",
    )
    text = "\n".join(path.read_text(encoding="utf-8") for path in contract_files)

    assert "pyc6_runtime" in text
    assert "PYC6TRC3" in text
    assert "pyc4_runtime" not in text
    assert "PYC4TRC2" not in text
    assert "PYC4TRC3" not in text


def test_repository_flow_pythonpath_includes_the_shared_semantic_core() -> None:
    root = Path(__file__).resolve().parents[2]
    completed = subprocess.run(
        ["bash", "-c", "source flows/scripts/lib.sh; pyc_pythonpath"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    )
    entries = completed.stdout.strip().split(":")
    assert str(root / "python/semantic-core/src") in entries
    assert str(root / "python/pycircuit/src") in entries
