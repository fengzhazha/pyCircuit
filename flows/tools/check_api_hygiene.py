#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


ROOT = _repo_root()
SEMANTIC_CORE = ROOT / "python" / "semantic-core" / "src"
FRONTEND = ROOT / "python" / "pycircuit" / "src"
for source_root in (FRONTEND, SEMANTIC_CORE):
    if str(source_root) not in sys.path:
        sys.path.insert(0, str(source_root))

from pycircuit.api_contract import TEXT_RULES, TextRule, scan_text  # noqa: E402
from pycircuit.diagnostics import render_diagnostic  # noqa: E402

DEFAULT_TARGETS: tuple[str, ...] = (
    "python/pycircuit/src/pycircuit",
    "examples/pycircuit",
    "docs",
    "flows/tools",
    "README.md",
)

TEXT_SUFFIXES = {
    ".md",
    ".py",
    ".rst",
    ".txt",
    ".toml",
    ".yaml",
    ".yml",
    ".json",
    ".sh",
}

SKIP_DIRS = {
    ".git",
    ".pycircuit_out",
    "contrib",
    "__pycache__",
    "build",
    "build-top",
}

# PYC416 removed from TEXT_RULES (mux allowed for eager/V6); keep other relaxes.
FRONTEND_RELAX_CODES = {"PYC415", "PYC417", "PYC418", "PYC423"}
ACIR_DOC_RELAX_CODES = {"PYC415"}


def iter_target_files(path: Path) -> list[Path]:
    if not path.exists():
        return []
    if path.is_file():
        return [path]
    out: list[Path] = []
    for f in path.rglob("*"):
        if not f.is_file():
            continue
        dir_parts = f.parts[:-1]
        if any(part in SKIP_DIRS for part in dir_parts):
            continue
        # Skip common build output directories (build/, build2/, build-foo/, ...).
        if any(part.startswith("build") for part in dir_parts):
            continue
        if f.name in {"check_api_hygiene.py", "api_contract.py"}:
            continue
        if f.suffix and f.suffix.lower() not in TEXT_SUFFIXES:
            continue
        out.append(f)
    return out


def scan_file(path: Path, *, rules: tuple[TextRule, ...] = TEXT_RULES) -> list[str]:
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return []
    diags = scan_text(path=path, text=text, stage="api-hygiene", rules=rules)
    return [render_diagnostic(d) for d in diags]


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Fail if stale/forbidden pyCircuit frontend API tokens are present."
    )
    ap.add_argument(
        "--scan-root",
        default=None,
        help="Optional root directory to scan. Defaults to this repo root.",
    )
    ap.add_argument(
        "targets",
        nargs="*",
        default=list(DEFAULT_TARGETS),
        help="Files/directories to scan",
    )
    args = ap.parse_args()

    scan_root = Path(args.scan_root).resolve() if args.scan_root else ROOT
    if not scan_root.exists():
        raise SystemExit(f"--scan-root does not exist: {scan_root}")

    violations = 0
    for target in args.targets:
        tp = (
            (scan_root / target).resolve()
            if not Path(target).is_absolute()
            else Path(target)
        )
        for f in iter_target_files(tp):
            rel = f.relative_to(scan_root) if f.is_relative_to(scan_root) else f
            rel_posix = rel.as_posix() if isinstance(rel, Path) else str(rel)
            rules = TEXT_RULES
            if rel_posix.startswith("python/pycircuit/src/pycircuit/"):
                rules = tuple(
                    r for r in TEXT_RULES if r.code not in FRONTEND_RELAX_CODES
                )
            elif rel_posix.startswith("docs/acir/"):
                # Agentic Circuit collections intentionally expose `.select`;
                # PYC415 applies only to the removed pyCircuit Wire API.
                rules = tuple(
                    r for r in TEXT_RULES if r.code not in ACIR_DOC_RELAX_CODES
                )
            msgs = scan_file(f, rules=rules)
            for m in msgs:
                print(m)
                violations += 1

    if violations:
        print(f"error: found {violations} forbidden API token(s)")
        return 1
    print("ok: API hygiene check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
