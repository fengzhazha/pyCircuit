import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.unit


def test_pyc_inventory_matches_ods_and_generated_ledger() -> None:
    before = subprocess.run(
        ["git", "status", "--porcelain=v1"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    checked = subprocess.run(
        [sys.executable, "tools/check-pyc-inventory.py"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    after = subprocess.run(
        ["git", "status", "--porcelain=v1"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    assert checked.returncode == 0, checked.stdout + checked.stderr
    assert before == after


def test_pyc_inventory_marks_only_vector_ops_for_issue_42() -> None:
    import yaml

    inventory = yaml.safe_load(
        (ROOT / "schemas/primitives/pyc_ir_inventory.yaml").read_text()
    )
    pending = {
        entry["name"]
        for entry in inventory["operations"]
        if entry["status"] == "pending-removal"
    }
    assert pending == {
        "pyc.v_get",
        "pyc.v_create",
        "pyc.v_broadcast",
        "pyc.v_broadcast_dim",
        "pyc.v_or_reduce",
        "pyc.v_and_reduce",
        "pyc.v_add_reduce",
    }


def test_pyc_inventory_records_exact_rtl_selection_boundary() -> None:
    import json

    coverage = json.loads(
        (ROOT / "schemas/primitives/pyc_ir_coverage.json").read_text()
    )
    operations = {entry["name"]: entry for entry in coverage["operations"]}
    expected = {
        "pyc.priority_encode": "pyc.priority_encode.v1",
        "pyc.popcount": "pyc.popcount.v1",
        "pyc.count_zeros": "pyc.count_zeros.v1",
    }
    for operation, semantic_id in expected.items():
        selection = operations[operation]["rtl_selection"]
        assert selection["pass"].endswith("SelectRtlPrimitivesPass.cpp")
        assert selection["catalog"] == "library/verilog/rtl_catalog.json"
        assert [entry["semantic_id"] for entry in selection["candidates"]] == [
            semantic_id
        ]
    assert len(operations["pyc.rtl.comb"]["rtl_selection"]["candidates"]) == 3
    assert operations["pyc.add"]["rtl_selection"] is None
