"""Fresh-process trusted project capture and verified result transport."""

from __future__ import annotations

import argparse
import ast
import contextlib
import importlib.util
import io
import json
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

try:
    import _pycircuit_semantics
except ModuleNotFoundError:
    _pycircuit_semantics = None

from ._canonical_json import JsonValue, canonical_json_bytes
from ._capabilities import schema_root
from ._diagnostics import Diagnostic, FixIt, RelatedLocation, SourceSpan
from ._frontend import CaptureRequest, elaborate_frontend
from ._output import OutputSink
from ._schemas import SchemaRegistry
from ._staging import ArtifactStage
from ._static_eval import FrozenMap, StaticValue


@dataclass(frozen=True, slots=True)
class CaptureWorkerRequest:
    python: str
    workspace: Path
    entry: Path
    system: str
    static_arguments: tuple[tuple[str, JsonValue], ...]
    component_roots: tuple[Path, ...]
    private_output: Path
    timeout: float = 30.0


@dataclass(frozen=True, slots=True)
class CaptureWorkerResult:
    acpy: bytes | None
    acir: bytes | None
    diagnostics: tuple[Diagnostic, ...]
    project_report: bytes | None
    frontend_kind: str | None


_STAGE_FILES = (
    "model.ac.mlir",
    "model.acpy.json",
    "project-report.txt",
    "request.json",
    "result.json",
)


def _source(value: object) -> SourceSpan | None:
    if value is None:
        return None
    if type(value) is not dict or set(value) != {"file", "line", "column"}:
        raise ValueError("worker diagnostic source is invalid")
    return SourceSpan(
        str(value["file"]),
        int(value["line"]),
        int(value["column"]),
        int(value["line"]),
        int(value["column"]),
    )


def _diagnostic(value: object) -> Diagnostic:
    if type(value) is not dict:
        raise ValueError("worker diagnostic is invalid")
    related = tuple(
        RelatedLocation(
            message=item["message"],
            source=_source(item["source"]),
            object_path=item["object_path"],
        )
        for item in value["related"]
    )
    fixits = tuple(FixIt(item["message"]) for item in value["fixits"])
    return Diagnostic(
        stage=value["stage"],
        code=value["code"],
        severity=value["severity"],
        message=value["message"],
        source=_source(value["source"]),
        object_path=value["object_path"],
        expected=value["expected"],
        actual=value["actual"],
        related=related,
        fixits=fixits,
    )


def _failure(code: str, message: str) -> CaptureWorkerResult:
    return CaptureWorkerResult(
        acpy=None,
        acir=None,
        diagnostics=(
            Diagnostic(
                stage="frontend-capture",
                code=code,
                severity="error",
                message=message,
            ),
        ),
        project_report=None,
        frontend_kind=None,
    )


def _request_json(request: CaptureWorkerRequest, output: Path) -> dict[str, JsonValue]:
    return {
        "schema": "agentic-circuit-capture-request",
        "version": "0.1",
        "contract_epoch": "0.5",
        "workspace": request.workspace.resolve().as_posix(),
        "entry": request.entry.resolve().as_posix(),
        "system": request.system,
        "static_arguments": dict(request.static_arguments),
        "component_roots": [
            path.resolve().relative_to(request.workspace.resolve()).as_posix()
            for path in request.component_roots
        ],
        "output": output.resolve().as_posix(),
    }


def run_capture_worker(request: CaptureWorkerRequest) -> CaptureWorkerResult:
    # Namespace-package path order is the import authority chosen by the
    # parent process. Preserve it so the isolated worker cannot prefer a stale
    # build/install copy merely because its path sorts before the source tree.
    import_roots = [
        str(Path(location).resolve().parent)
        for location in sys.modules["agentic_circuit"].__path__
    ]
    if _pycircuit_semantics is not None:
        import_roots.append(
            str(Path(_pycircuit_semantics.__file__).resolve().parent.parent)
        )
    package_parents = os.pathsep.join(dict.fromkeys(import_roots))
    bootstrap = (
        "import os,runpy,sys;"
        "sys.path[:0]=sys.argv.pop(1).split(os.pathsep);"
        "runpy.run_module('agentic_circuit._capture_worker',run_name='__main__')"
    )
    with ArtifactStage(request.private_output, expected=_STAGE_FILES) as stage:
        assert stage.path is not None
        request_path = stage.path / "request.json"
        request_path.write_bytes(
            canonical_json_bytes(_request_json(request, stage.path))
        )
        environment = {
            "PATH": os.environ.get("PATH", ""),
            "PYTHONHASHSEED": "0",
        }
        try:
            completed = subprocess.run(
                (
                    request.python,
                    "-I",
                    "-c",
                    bootstrap,
                    package_parents,
                    "--request",
                    os.fspath(request_path),
                ),
                stdin=subprocess.DEVNULL,
                capture_output=True,
                timeout=request.timeout,
                check=False,
                env=environment,
            )
        except subprocess.TimeoutExpired:
            return _failure("ACPY-CAPTURE-002", "frontend capture timed out")
        if completed.returncode != 0:
            detail, _ = OutputSink.bounded_capture(
                (completed.stderr or completed.stdout).decode("utf-8", errors="replace")
            )
            return _failure(
                "ACPY-CAPTURE-001",
                "frontend capture worker failed" + (f": {detail}" if detail else ""),
            )
        try:
            stage.verify()
            response = json.loads((stage.path / "result.json").read_text())
            if set(response) != {
                "schema",
                "version",
                "contract_epoch",
                "has_acpy",
                "has_acir",
                "diagnostics",
                "frontend_kind",
            }:
                raise ValueError("worker result fields are invalid")
            diagnostics = tuple(_diagnostic(item) for item in response["diagnostics"])
            acpy = (
                (stage.path / "model.acpy.json").read_bytes()
                if response["has_acpy"]
                else None
            )
            acir = (
                (stage.path / "model.ac.mlir").read_bytes()
                if response["has_acir"]
                else None
            )
            report = (stage.path / "project-report.txt").read_bytes() or None
        except (OSError, UnicodeError, ValueError, KeyError, TypeError) as error:
            return _failure("ACPY-CAPTURE-001", f"capture result is invalid: {error}")
        frontend_kind = response["frontend_kind"]
        if frontend_kind not in {"structural", "queue_rule"}:
            raise ValueError("worker frontend kind is invalid")
        return CaptureWorkerResult(acpy, acir, diagnostics, report, frontend_kind)


def _load_project(entry: Path, workspace: Path) -> dict[str, object]:
    sys.path.insert(1, os.fspath(workspace))
    spec = importlib.util.spec_from_file_location("_agentic_architecture", entry)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load architecture entry {entry}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return vars(module)


def _worker_diagnostic(error: BaseException, entry: Path) -> Diagnostic:
    if isinstance(error, SyntaxError):
        line = error.lineno or 1
        column = error.offset or 1
        source = SourceSpan(entry.name, line, column, line, column)
        code = "ACPY-SYNTAX-001"
    else:
        source = None
        candidate = str(error).partition(":")[0]
        code = candidate if candidate.startswith("ACPY-") else "ACPY-CAPTURE-001"
    return Diagnostic(
        stage="frontend-capture",
        code=code,
        severity="error",
        message=f"trusted project execution failed: {error}",
        source=source,
    )


def _static_value(value: JsonValue) -> StaticValue:
    if value is None or type(value) in (bool, int, float, str):
        return value
    if type(value) is list:
        return tuple(_static_value(item) for item in value)
    if type(value) is dict:
        return FrozenMap(
            tuple(sorted((key, _static_value(item)) for key, item in value.items()))
        )
    raise TypeError("capture static argument is not an I-JSON value")


def _decorator_leaf(node: ast.expr) -> str:
    candidate = node.func if isinstance(node, ast.Call) else node
    if isinstance(candidate, ast.Name):
        return candidate.id
    if isinstance(candidate, ast.Attribute):
        return candidate.attr
    return ""


def _contains_rule(tree: ast.Module) -> bool:
    return any(
        isinstance(node, ast.FunctionDef)
        and any(
            _decorator_leaf(decorator) == "rule" for decorator in node.decorator_list
        )
        for node in tree.body
    )


def _worker_main(request_path: Path) -> int:
    request = json.loads(request_path.read_text())
    workspace = Path(request["workspace"]).resolve()
    entry = Path(request["entry"]).resolve()
    output = Path(request["output"]).resolve()
    captured_stdout = io.StringIO()
    captured_stderr = io.StringIO()
    document = None
    acir = None
    frontend_kind = "structural"
    diagnostics: tuple[Diagnostic, ...]
    try:
        with (
            contextlib.redirect_stdout(captured_stdout),
            contextlib.redirect_stderr(captured_stderr),
        ):
            namespace = _load_project(entry, workspace)
            component_roots = tuple(
                (workspace / value).resolve() for value in request["component_roots"]
            )
            if any(not path.is_relative_to(workspace) for path in component_roots):
                raise ValueError("component root escapes the workspace")
            text = entry.read_text(encoding="utf-8")
            tree = ast.parse(text, filename=entry.name, type_comments=True)
            has_rule = _contains_rule(tree)
            static_arguments = {
                key: _static_value(value)
                for key, value in request["static_arguments"].items()
            }
            if has_rule:
                from ._queue_frontend import (
                    build_queue_acpy,
                    lower_queue_program,
                    parse_queue_program,
                )

                frontend_kind = "queue_rule"
                program = parse_queue_program(
                    text,
                    request["system"],
                    static_arguments=static_arguments,
                )
                document = build_queue_acpy(
                    text,
                    request["system"],
                    entry.relative_to(workspace).as_posix(),
                )
                acir = lower_queue_program(program)
                diagnostics = ()
            else:
                schemas = SchemaRegistry.from_catalog(
                    schema_root() / "stdlib" / "catalog.json",
                    schema_root().parents[1],
                ).with_component_roots(component_roots)
                result = elaborate_frontend(
                    CaptureRequest(
                        entry=entry,
                        workspace=workspace,
                        system=request["system"],
                        static_arguments=tuple(sorted(static_arguments.items())),
                    ),
                    namespace,
                    schemas,
                )
                document = result.document
                acir = result.acir
                diagnostics = result.diagnostics
    except BaseException as error:
        diagnostics = (_worker_diagnostic(error, entry),)
    report, _ = OutputSink.bounded_capture(
        captured_stdout.getvalue() + captured_stderr.getvalue()
    )
    (output / "model.acpy.json").write_bytes(
        document.canonical_bytes() if document is not None else b""
    )
    (output / "model.ac.mlir").write_bytes(
        acir.encode("utf-8") if acir is not None else b""
    )
    (output / "project-report.txt").write_text(report, encoding="utf-8")
    response = {
        "schema": "agentic-circuit-capture-result",
        "version": "0.1",
        "contract_epoch": "0.5",
        "has_acpy": document is not None,
        "has_acir": acir is not None,
        "diagnostics": [item.to_json() for item in diagnostics],
        "frontend_kind": frontend_kind,
    }
    (output / "result.json").write_bytes(canonical_json_bytes(response))
    return 0


def _main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--request", type=Path, required=True)
    arguments = parser.parse_args()
    return _worker_main(arguments.request)


if __name__ == "__main__":
    raise SystemExit(_main())
