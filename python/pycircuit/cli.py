from __future__ import annotations

import argparse
import ast
import importlib.util
import inspect
import sys
from pathlib import Path

from .dsl import Module
from .jit import JitError, compile as jit_compile


def _default_top_name(src: Path) -> str:
    parts = [p for p in src.stem.replace("-", "_").split("_") if p]
    if not parts:
        return "Top"
    return "".join(p[:1].upper() + p[1:] for p in parts)


def _load_py_file(path: Path) -> object:
    spec = importlib.util.spec_from_file_location(path.stem, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Failed to import {path}")
    m = importlib.util.module_from_spec(spec)
    # Ensure the module is visible during execution (dataclasses relies on this).
    sys.modules[spec.name] = m
    spec.loader.exec_module(m)
    return m


def _cmd_emit(args: argparse.Namespace) -> int:
    src = Path(args.python_file)
    out = Path(args.output)
    mod = _load_py_file(src)
    if not hasattr(mod, "build"):
        raise SystemExit(f"{src} must define build() -> pycircuit.Module")
    build = getattr(mod, "build")

    # JIT-by-default behavior:
    # - If `build` is a function that accepts a builder arg (e.g. `build(m: Circuit, ...)`),
    #   compile it via the AST/JIT frontend using default parameter values.
    # - Otherwise, call it normally and expect a `Module` result (legacy path).
    if callable(build):
        sig = inspect.signature(build)
        params = list(sig.parameters.values())
        if params:
            # Collect JIT-time parameters from defaults.
            jit_params: dict[str, object] = {}
            missing: list[str] = []
            for p in params[1:]:
                if p.default is inspect._empty:
                    missing.append(p.name)
                else:
                    jit_params[p.name] = p.default
            if missing:
                raise SystemExit(
                    f"build() is treated as a JIT design function but missing default values for: {', '.join(missing)}"
                )

            # Apply CLI overrides.
            for spec in args.param:
                if "=" not in spec:
                    raise SystemExit(f"--param expects name=value, got: {spec!r}")
                name, raw = spec.split("=", 1)
                name = name.strip()
                raw = raw.strip()
                if not name:
                    raise SystemExit(f"--param expects name=value, got: {spec!r}")
                if name not in jit_params:
                    raise SystemExit(f"unknown JIT parameter: {name!r} (available: {', '.join(jit_params.keys())})")
                try:
                    val = ast.literal_eval(raw)
                except Exception:
                    val = raw
                jit_params[name] = val

            name = _default_top_name(src)
            override = getattr(build, "__pycircuit_name__", None)
            if isinstance(override, str) and override.strip():
                name = override.strip()
            try:
                m = jit_compile(build, name=name, **jit_params)
            except JitError as e:
                raise SystemExit(f"JIT compile failed: {e}") from e
        else:
            m = build()
    else:
        m = build

    if not isinstance(m, Module):
        raise SystemExit("build must be a pycircuit.Module, a build() -> Module function, or a JIT build(m, ...) function")
    out.write_text(m.emit_mlir(), encoding="utf-8")
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="pycircuit")
    sub = p.add_subparsers(dest="cmd", required=True)

    emit = sub.add_parser("emit", help="Emit PYC MLIR (*.pyc) from a Python design file.")
    emit.add_argument("python_file", help="Python source defining build() -> pycircuit.Module")
    emit.add_argument("-o", "--output", required=True, help="Output .pyc path")
    emit.add_argument(
        "--param",
        action="append",
        default=[],
        help="Override a JIT parameter (repeatable): name=value (parsed as a Python literal when possible)",
    )
    emit.set_defaults(fn=_cmd_emit)

    ns = p.parse_args(argv)
    return int(ns.fn(ns))


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
