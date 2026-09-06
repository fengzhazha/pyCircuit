#!/usr/bin/env python3
"""Check the exact canonical PYC IR inventory and generated coverage ledger."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
INVENTORY = ROOT / "schemas/primitives/pyc_ir_inventory.yaml"
LEDGER = ROOT / "docs/development/pyc-ir-inventory.md"
COVERAGE_JSON = ROOT / "schemas/primitives/pyc_ir_coverage.json"
OPS_TD = ROOT / "compiler/mlir/include/pyc/Dialect/PYC/PYCOps.td"
TYPES_TD = ROOT / "compiler/mlir/include/pyc/Dialect/PYC/PYCTypes.td"
DIALECT_CPP = ROOT / "compiler/mlir/lib/Dialect/PYC/PYCDialect.cpp"
RTL_SELECTION_PASS = ROOT / "compiler/mlir/lib/Transforms/SelectRtlPrimitivesPass.cpp"
RTL_CATALOG = ROOT / "library/verilog/rtl_catalog.json"

OP_RE = re.compile(r'def\s+(PYC_\w+Op)\s*:\s*PYC_Op<"([^"]+)"')
TYPE_RE = re.compile(
    r'def\s+(PYC_\w+Type)\s*:\s*TypeDef<PYCDialect,\s*"[^"]+">\s*\{.*?'
    r'let\s+mnemonic\s*=\s*"([^"]+)"',
    re.S,
)
REMOVED_VECTOR = {
    "pyc.v_get",
    "pyc.v_create",
    "pyc.v_broadcast",
    "pyc.v_broadcast_dim",
    "pyc.v_or_reduce",
    "pyc.v_and_reduce",
    "pyc.v_add_reduce",
}
REMOVED = REMOVED_VECTOR | {
    "pyc.mux",
    "pyc.eq",
    "pyc.ult",
    "pyc.slt",
    "pyc.shli",
    "pyc.lshri",
    "pyc.ashri",
}


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _sources(pattern: str, roots: tuple[Path, ...]) -> list[str]:
    regex = re.compile(pattern)
    matches: list[str] = []
    for root in roots:
        if not root.exists():
            continue
        paths = [root] if root.is_file() else root.rglob("*")
        for path in paths:
            if not path.is_file() or path.suffix not in {
                ".py",
                ".cpp",
                ".h",
                ".td",
                ".mlir",
            }:
                continue
            try:
                text = _read(path)
            except UnicodeDecodeError:
                continue
            if regex.search(text):
                matches.append(path.relative_to(ROOT).as_posix())
    return sorted(set(matches))


def load_inventory(
    errors: list[str],
) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    try:
        document = yaml.safe_load(_read(INVENTORY))
    except (OSError, yaml.YAMLError) as exc:
        errors.append(f"cannot load {INVENTORY.relative_to(ROOT)}: {exc}")
        return [], []
    if (
        not isinstance(document, dict)
        or document.get("schema") != "pyc-ir-inventory-v1"
    ):
        errors.append("PYC inventory must declare schema pyc-ir-inventory-v1")
        return [], []
    if document.get("dialect") != "pyc":
        errors.append("PYC inventory must declare dialect pyc")
    operations = document.get("operations")
    types = document.get("types")
    if not isinstance(operations, list) or not isinstance(types, list):
        errors.append("PYC inventory operations/types must be lists")
        return [], []
    for kind, entries in (("operation", operations), ("type", types)):
        if not all(
            isinstance(entry, dict) and isinstance(entry.get("name"), str)
            for entry in entries
        ):
            errors.append(f"every PYC {kind} entry must have a string name")
    return operations, types


def load_rtl_selections(errors: list[str]) -> dict[str, list[dict[str, str]]]:
    try:
        document = json.loads(_read(RTL_CATALOG))
    except (OSError, json.JSONDecodeError) as exc:
        errors.append(f"cannot load {RTL_CATALOG.relative_to(ROOT)}: {exc}")
        return {}
    if document.get("schema") != "pyc-rtl-catalog-v1":
        errors.append("RTL catalog must declare schema pyc-rtl-catalog-v1")
        return {}
    selections: dict[str, list[dict[str, str]]] = {}
    for entry in document.get("implementations", []):
        semantic_id = entry.get("semantic_id")
        implementation_id = entry.get("implementation_id")
        if not isinstance(semantic_id, str) or not isinstance(implementation_id, str):
            errors.append(
                "RTL catalog entries require semantic_id and implementation_id"
            )
            continue
        operation = semantic_id.rsplit(".v", 1)[0]
        selections.setdefault(operation, []).append(
            {
                "semantic_id": semantic_id,
                "implementation_id": implementation_id,
            }
        )
    return {
        operation: sorted(entries, key=lambda entry: entry["implementation_id"])
        for operation, entries in selections.items()
    }


def collect(
    errors: list[str],
) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    operations, types = load_inventory(errors)
    rtl_selections = load_rtl_selections(errors)
    ods_ops = {
        f"pyc.{mnemonic}": symbol for symbol, mnemonic in OP_RE.findall(_read(OPS_TD))
    }
    ods_types = {
        f"!pyc.{mnemonic}": symbol
        for symbol, mnemonic in TYPE_RE.findall(_read(TYPES_TD))
    }
    inventory_ops = {entry["name"]: entry for entry in operations}
    inventory_types = {entry["name"]: entry for entry in types}
    if set(inventory_ops) != set(ods_ops):
        errors.append(
            "PYC operation inventory differs from ODS: "
            f"missing={sorted(set(ods_ops) - set(inventory_ops))}, "
            f"extra={sorted(set(inventory_ops) - set(ods_ops))}"
        )
    if set(inventory_types) != set(ods_types):
        errors.append(
            "PYC type inventory differs from ODS: "
            f"missing={sorted(set(ods_types) - set(inventory_types))}, "
            f"extra={sorted(set(inventory_types) - set(ods_types))}"
        )
    pending = {
        name
        for name, entry in inventory_ops.items()
        if entry.get("status") != "normative"
    }
    if pending:
        errors.append(
            f"canonical PYC inventory has non-normative entries: {sorted(pending)}"
        )
    for name, entry in inventory_ops.items():
        if entry.get("status") != "normative":
            errors.append(f"{name} must be normative in the PYC inventory")

    registration = _read(DIALECT_CPP)
    for include in ("PYCOps.cpp.inc", "PYCTypes.cpp.inc"):
        if include not in registration:
            errors.append(f"PYC dialect registration omits {include}")

    ods_text = _read(OPS_TD)
    if not all(
        name in ods_ops
        for name in ("pyc.select", "pyc.cmp", "pyc.shl", "pyc.lshr", "pyc.ashr")
    ):
        errors.append("canonical select/cmp/SSA shift family is incomplete")
    for removed in REMOVED:
        if removed in ods_ops:
            errors.append(
                f"removed compatibility operation remains registered: {removed}"
            )
    for token in ("AnyIntegerOrV", "AnyVectorOfAnyRank", "vector<", "PYC_V"):
        if token in ods_text:
            errors.append(
                f"scalar-only PYC ODS retains forbidden vector token: {token}"
            )
    for path in (
        ROOT / "compiler/mlir/lib/Dialect/PYC/PYCOps.cpp",
        ROOT / "compiler/mlir/lib/Emit/CppEmitter.cpp",
        ROOT / "compiler/mlir/lib/Emit/VerilogEmitter.cpp",
    ):
        source = _read(path)
        for token in ("VectorType", "VGetOp", "VCreateOp", "VBroadcastOp"):
            if token in source:
                errors.append(
                    f"scalar-only compiler file {path.relative_to(ROOT)} retains {token}"
                )
    for path in (
        ROOT / "compiler/mlir/lib/Transforms/VectorUnrollPass.cpp",
        ROOT / "compiler/mlir/lib/Transforms/SLPPackWiresPass.cpp",
        ROOT / "library/cpp/pyc_vec.hpp",
        ROOT / "docs/vec-operators.md",
    ):
        if path.exists():
            errors.append(
                f"removed vector product surface still exists: {path.relative_to(ROOT)}"
            )
    frontend_forbidden = {
        ROOT / "python/pycircuit/src/pycircuit/data.py": ("class Vector",),
        ROOT / "python/pycircuit/src/pycircuit/dsl.py": ("pyc.v_", "def v_"),
        ROOT
        / "python/pycircuit/src/pycircuit/hw.py": (
            "Wire[Vector",
            "def vec(",
            "def priority_mux(",
        ),
        ROOT
        / "python/pycircuit/src/pycircuit/v6.py": (
            "def broadcast(",
            "def reduce_or(",
            "def priority_mux(",
        ),
        ROOT / "python/pycircuit/src/pycircuit/jit.py": ('kind == "vec"',),
        ROOT / "python/pycircuit/src/pycircuit/cli.py": ('startswith("vector<")',),
    }
    for path, tokens in frontend_forbidden.items():
        source = _read(path)
        for token in tokens:
            if token in source:
                errors.append(
                    f"scalar-only frontend file {path.relative_to(ROOT)} retains {token}"
                )
    dialect_impl = _read(ROOT / "compiler/mlir/lib/Dialect/PYC/PYCOps.cpp")
    if (
        'predicate != "eq" && predicate != "ult" && predicate != "slt"'
        not in dialect_impl
    ):
        errors.append(
            "pyc.cmp verifier does not enforce the closed eq/ult/slt predicate set"
        )

    rows: list[dict[str, object]] = []
    op_blocks = {
        name: re.search(
            rf"def\s+{re.escape(symbol)}\s*:.*?\n\}}", ods_text, re.S
        ).group(0)
        for name, symbol in ods_ops.items()
    }
    python_roots = (ROOT / "python/pycircuit/src/pycircuit",)
    acir_roots = (ROOT / "compiler/acir/lib/CodeGen/QueueGraphPyc.cpp",)
    cpp_root = ROOT / "compiler/mlir/lib/Emit/CppEmitter.cpp"
    verilog_root = ROOT / "compiler/mlir/lib/Emit/VerilogEmitter.cpp"
    tests_root = ROOT / "tests"
    e2e_root = ROOT / "tests/integration"
    examples_root = ROOT / "examples"
    canonicalization_roots = (ROOT / "compiler/mlir/lib/Transforms",)
    for name in sorted(ods_ops):
        mnemonic = name.removeprefix("pyc.")
        symbol = ods_ops[name]
        token = rf'pyc\.{re.escape(mnemonic)}(?![\w.])|["\']{re.escape(mnemonic)}["\']'
        class_token = rf"pyc::{re.escape(symbol.removeprefix('PYC_'))}\b"
        block = op_blocks[name]
        all_mlir = _sources(token, (ROOT / "tests/mlir",))
        negative_mlir = [path for path in all_mlir if "invalid" in Path(path).name]
        positive_mlir = [path for path in all_mlir if path not in negative_mlir]
        status = inventory_ops.get(name, {}).get("status", "missing")
        selected_rtl = rtl_selections.get(name, [])
        if name == "pyc.rtl.comb":
            selected_rtl = [
                entry for entries in rtl_selections.values() for entry in entries
            ]
        rtl_selection = None
        if selected_rtl:
            rtl_selection = {
                "pass": RTL_SELECTION_PASS.relative_to(ROOT).as_posix(),
                "catalog": RTL_CATALOG.relative_to(ROOT).as_posix(),
                "candidates": selected_rtl,
            }
        rows.append(
            {
                "name": name,
                "kind": "operation",
                "symbol": symbol,
                "stage": "backend-pyc" if name == "pyc.rtl.comb" else "canonical-pyc",
                "status": status,
                "replacement": (
                    "scalarize-before-canonical-pyc"
                    if status == "pending-removal"
                    else None
                ),
                "python_producer": _sources(token, python_roots),
                "verifier": (
                    "compiler/mlir/lib/Dialect/PYC/PYCOps.cpp"
                    if "hasVerifier = 1" in block
                    else None
                ),
                "folder": (
                    "compiler/mlir/lib/Dialect/PYC/PYCOps.cpp"
                    if "hasFolder = 1" in block
                    else None
                ),
                "canonicalization": _sources(class_token, canonicalization_roots),
                "acir_producer": _sources(token, acir_roots),
                "cpp_emitter": (
                    "compiler/mlir/lib/Emit/CppEmitter.cpp"
                    if re.search(class_token, _read(cpp_root))
                    else None
                ),
                "verilog_emitter": (
                    "compiler/mlir/lib/Emit/VerilogEmitter.cpp"
                    if re.search(class_token, _read(verilog_root))
                    else None
                ),
                "rtl_selection": rtl_selection,
                "positive_mlir": positive_mlir,
                "negative_mlir": negative_mlir,
                "examples": _sources(token, (examples_root,)),
                "e2e": _sources(token, (e2e_root,)),
                "tests": _sources(token, (tests_root,)),
            }
        )
    type_rows = [
        {
            "name": name,
            "kind": "type",
            "symbol": symbol,
            "stage": "canonical-pyc",
            "status": inventory_types.get(name, {}).get("status", "missing"),
            "replacement": None,
            "python_producer": _sources(re.escape(name), python_roots),
            "verifier": None,
            "folder": None,
            "canonicalization": [],
            "acir_producer": _sources(re.escape(name), acir_roots),
            "cpp_emitter": (
                "compiler/mlir/lib/Emit/CppEmitter.cpp"
                if name in _read(cpp_root)
                else None
            ),
            "verilog_emitter": (
                "compiler/mlir/lib/Emit/VerilogEmitter.cpp"
                if name in _read(verilog_root)
                else None
            ),
            "rtl_selection": None,
            "positive_mlir": [
                path
                for path in _sources(re.escape(name), (ROOT / "tests/mlir",))
                if "invalid" not in Path(path).name
            ],
            "negative_mlir": [
                path
                for path in _sources(re.escape(name), (ROOT / "tests/mlir",))
                if "invalid" in Path(path).name
            ],
            "examples": _sources(re.escape(name), (examples_root,)),
            "e2e": _sources(re.escape(name), (e2e_root,)),
            "tests": _sources(re.escape(name), (tests_root,)),
        }
        for name, symbol in sorted(ods_types.items())
    ]
    required = {
        "name",
        "kind",
        "symbol",
        "stage",
        "status",
        "replacement",
        "python_producer",
        "verifier",
        "folder",
        "canonicalization",
        "acir_producer",
        "cpp_emitter",
        "verilog_emitter",
        "rtl_selection",
        "positive_mlir",
        "negative_mlir",
        "examples",
        "e2e",
        "tests",
    }
    for entry in [*rows, *type_rows]:
        if set(entry) != required:
            errors.append(f"coverage entry {entry.get('name')} has incomplete fields")
    return rows, type_rows


def render(rows: list[dict[str, object]]) -> str:
    def cell(value: object) -> str:
        if isinstance(value, list):
            return "<br>".join(value) if value else "—"
        return "yes" if value is True else "no" if value is False else str(value)

    lines = [
        "# Canonical PYC IR inventory ledger",
        "",
        "<!-- Generated by tools/check-pyc-inventory.py --write-ledger. Do not edit. -->",
        "",
        "| operation | ODS symbol | status | verifier | folder | Python producer | ACIR mapping | C++ | Verilog | RTL selection | positive MLIR | negative MLIR |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for row in rows:
        values = [
            row["name"],
            row["symbol"],
            row["status"],
            row["verifier"],
            row["folder"],
            row["python_producer"],
            row["acir_producer"],
            row["cpp_emitter"],
            row["verilog_emitter"],
            row["rtl_selection"],
            row["positive_mlir"],
            row["negative_mlir"],
        ]
        lines.append("| " + " | ".join(cell(value) for value in values) + " |")
    lines.extend(
        [
            "",
            "## Types",
            "",
            "- `!pyc.clock` (`PYC_ClockType`)",
            "- `!pyc.reset` (`PYC_ResetType`)",
            "",
        ]
    )
    return "\n".join(lines)


def render_json(
    rows: list[dict[str, object]], type_rows: list[dict[str, object]]
) -> str:
    return (
        json.dumps(
            {
                "schema": "pyc-ir-coverage-v1",
                "dialect": "pyc",
                "operations": rows,
                "types": type_rows,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n"
    )


def run_checks(root: Path = ROOT) -> list[str]:
    del root
    errors: list[str] = []
    rows, type_rows = collect(errors)
    if not LEDGER.is_file() or _read(LEDGER) != render(rows):
        errors.append(
            "PYC inventory ledger is stale; run tools/check-pyc-inventory.py --write-ledger"
        )
    if not COVERAGE_JSON.is_file() or _read(COVERAGE_JSON) != render_json(
        rows, type_rows
    ):
        errors.append(
            "PYC machine-readable coverage is stale; run tools/check-pyc-inventory.py --write-ledger"
        )
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write-ledger", action="store_true")
    args = parser.parse_args(argv)
    errors: list[str] = []
    rows, type_rows = collect(errors)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    if args.write_ledger:
        LEDGER.parent.mkdir(parents=True, exist_ok=True)
        LEDGER.write_text(render(rows), encoding="utf-8")
        COVERAGE_JSON.write_text(render_json(rows, type_rows), encoding="utf-8")
        print(f"wrote {LEDGER.relative_to(ROOT)} and {COVERAGE_JSON.relative_to(ROOT)}")
        return 0
    if not LEDGER.is_file() or _read(LEDGER) != render(rows):
        print(
            "error: PYC inventory ledger is stale; run tools/check-pyc-inventory.py --write-ledger",
            file=sys.stderr,
        )
        return 1
    if not COVERAGE_JSON.is_file() or _read(COVERAGE_JSON) != render_json(
        rows, type_rows
    ):
        print(
            "error: PYC machine-readable coverage is stale; run tools/check-pyc-inventory.py --write-ledger",
            file=sys.stderr,
        )
        return 1
    print(
        f"PYC inventory: OK ({len(rows)} operations, 2 types, exact ODS/ledger match)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
