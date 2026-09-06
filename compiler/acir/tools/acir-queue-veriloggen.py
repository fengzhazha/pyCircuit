#!/usr/bin/env python3
"""Generate self-contained Verilog from a frozen Queue ACIR module.

This is the first in-tree PYC compatibility backend.  It deliberately consumes
the canonical textual PYC emitted by ``acir-queue-pycgen`` instead of adding a
second ACIR lowering.  The small emitter covers the pyCircuit 6 operations used
by the golden queue slice and embeds only the required sequential runtime
modules in the resulting Verilog file.
"""

from __future__ import annotations

import argparse
import ast
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path


class PYCVerilogError(ValueError):
    pass


_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


@dataclass(frozen=True)
class Value:
    name: str
    type: str

    @property
    def net(self) -> str:
        return self.name.lstrip("%").replace(".", "_")

    @property
    def width(self) -> int:
        match = re.fullmatch(r"i(\d+)", self.type)
        if not match:
            raise PYCVerilogError(f"expected integer PYC type, got {self.type}")
        width = int(match.group(1))
        if width <= 0:
            raise PYCVerilogError("PYC integer widths must be positive")
        return width


@dataclass
class Module:
    name: str
    args: list[Value]
    results: list[Value]
    body: list[str]


def _split_csv(text: str) -> list[str]:
    """Split a comma list while ignoring commas inside brackets/quotes."""
    result: list[str] = []
    start = 0
    depth = 0
    quote = False
    escaped = False
    for index, char in enumerate(text):
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quote = False
            continue
        if char == '"':
            quote = True
        elif char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif char == "," and depth == 0:
            result.append(text[start:index].strip())
            start = index + 1
    tail = text[start:].strip()
    if tail:
        result.append(tail)
    return result


def _parse_types(text: str) -> list[str]:
    text = text.strip()
    if text.startswith("(") and text.endswith(")"):
        text = text[1:-1]
    return [part.strip() for part in _split_csv(text) if part.strip()]


def _parse_names_attr(header: str, key: str) -> list[str]:
    match = re.search(rf"{re.escape(key)}\s*=\s*(\[[^]]*\])", header)
    if not match:
        raise PYCVerilogError(f"function header is missing {key}")
    value = ast.literal_eval(match.group(1))
    if (
        not isinstance(value, list)
        or not all(
            isinstance(item, str) and _IDENTIFIER.fullmatch(item) for item in value
        )
        or len(value) != len(set(value))
    ):
        raise PYCVerilogError(f"{key} must be a unique Verilog identifier list")
    return value


def parse_pyc_module(text: str) -> Module:
    func_match = re.search(
        r"^\s*func\.func\s+@(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\((?P<args>.*?)\)\s*"
        r"->\s*\((?P<results>.*?)\)\s*attributes\s*(?P<attrs>\{.*\})\s*\{\s*$",
        text,
        re.MULTILINE | re.DOTALL,
    )
    if not func_match:
        raise PYCVerilogError(
            "canonical PYC module does not contain a supported func.func"
        )

    arg_parts = _split_csv(func_match.group("args"))
    args: list[Value] = []
    for part in arg_parts:
        match = re.fullmatch(r"(%[A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.+)", part.strip())
        if not match:
            raise PYCVerilogError(f"cannot parse function argument: {part}")
        args.append(Value(match.group(1), match.group(2).strip()))
    if len({value.name for value in args}) != len(args):
        raise PYCVerilogError("function arguments must have unique SSA names")

    result_types = _parse_types(func_match.group("results"))
    result_names = _parse_names_attr(func_match.group("attrs"), "result_names")
    if len(result_types) != len(result_names):
        raise PYCVerilogError("result_names and result types have different lengths")
    results = [Value(name, type_) for name, type_ in zip(result_names, result_types)]
    argument_ports = {value.net for value in args}
    if argument_ports.intersection(result_names):
        raise PYCVerilogError("function input and result port names must be unique")

    lines = text.splitlines()
    func_line = next(
        index
        for index, line in enumerate(lines)
        if line.strip().startswith("func.func @")
    )
    body: list[str] = []
    for line in lines[func_line + 1 :]:
        if line.strip() == "}":
            break
        if line.startswith("    "):
            body.append(line.strip())
    if not body:
        raise PYCVerilogError("PYC function body is empty")
    return Module(func_match.group("name"), args, results, body)


def _ssa_names(text: str) -> list[str]:
    return [part.strip() for part in _split_csv(text) if part.strip()]


def _value(values: dict[str, Value], name: str) -> Value:
    try:
        return values[name]
    except KeyError as exc:
        raise PYCVerilogError(f"unknown PYC SSA value {name}") from exc


def _port_decl(direction: str, value: Value) -> str:
    if value.type in ("!pyc.clock", "!pyc.reset"):
        return f"  {direction} wire {value.net}"
    width = value.width
    suffix = "" if width == 1 else f" [{width - 1}:0]"
    return f"  {direction} wire{suffix} {value.net}"


def _literal(value: str, width: int) -> str:
    try:
        number = int(value, 0)
    except ValueError as exc:
        raise PYCVerilogError(f"unsupported PYC constant {value}") from exc
    return f"{width}'d{number}"


def _parse_attr_int(attrs: str, key: str) -> int:
    match = re.search(rf"\b{re.escape(key)}\s*=\s*(-?\d+)", attrs)
    if not match:
        raise PYCVerilogError(f"PYC op is missing integer attribute {key}")
    return int(match.group(1))


def _runtime_sources(runtime_dir: Path) -> Iterable[str]:
    for name in ("pyc_reg.v", "pyc_fifo.v"):
        path = runtime_dir / name
        if not path.is_file():
            raise PYCVerilogError(f"missing in-tree PYC runtime module: {path}")
        yield f"// --- PYC runtime: {path.name}\n{path.read_text(encoding='utf-8')}"


def emit_verilog(module: Module, runtime_dir: Path) -> str:
    values: dict[str, Value] = {value.name: value for value in module.args}
    declarations: list[str] = []
    assigns: list[str] = []
    instances: list[str] = []
    assertions: list[str] = []
    returns: list[str] | None = None
    seen_outputs: set[str] = set()

    def add_value(name: str, type_: str) -> Value:
        value = Value(name, type_)
        if name in values:
            raise PYCVerilogError(f"SSA value {name} is defined more than once")
        value.width
        values[name] = value
        if name not in {arg.name for arg in module.args}:
            if type_ not in ("!pyc.clock", "!pyc.reset"):
                width = value.width
                suffix = "" if width == 1 else f" [{width - 1}:0]"
                declarations.append(f"  wire{suffix} {value.net};")
        return value

    for line in module.body:
        if line.startswith("func.return "):
            match = re.fullmatch(r"func\.return\s+(.*?)\s*:\s*(.*)", line)
            if not match:
                raise PYCVerilogError(f"cannot parse return: {line}")
            returns = _ssa_names(match.group(1))
            continue
        if line.startswith("pyc.assert "):
            match = re.fullmatch(
                r"pyc\.assert\s+(%[A-Za-z_][A-Za-z0-9_]*)\s*"
                r"\{\s*msg\s*=\s*(\"(?:[^\"\\]|\\.)*\")\s*\}",
                line,
            )
            if not match:
                raise PYCVerilogError(f"cannot parse assertion: {line}")
            condition = _value(values, match.group(1))
            if condition.type != "i1":
                raise PYCVerilogError("assertion condition must be i1")
            try:
                message = json.loads(match.group(2))
            except json.JSONDecodeError as exc:
                raise PYCVerilogError("assertion message is invalid") from exc
            assertions.extend(
                (
                    "  // synthesis translate_off",
                    "  always @* begin",
                    f"    if (!{condition.net}) begin",
                    f"      $display({json.dumps(message)});",
                    "      $stop;",
                    "    end",
                    "  end",
                    "  // synthesis translate_on",
                )
            )
            continue

        lhs: list[str] = []
        rhs = line
        if " = " in line:
            lhs_text, rhs = line.split(" = ", 1)
            lhs = _ssa_names(lhs_text)
        rhs = rhs.strip()

        if rhs.startswith("pyc.wire : "):
            if len(lhs) != 1:
                raise PYCVerilogError(f"wire expects one result: {line}")
            add_value(lhs[0], rhs[len("pyc.wire : ") :].strip())
            continue

        if rhs.startswith("pyc.constant "):
            match = re.fullmatch(r"pyc\.constant\s+(\S+)\s*:\s*(\S+)", rhs)
            if not match or len(lhs) != 1:
                raise PYCVerilogError(f"cannot parse constant: {line}")
            value = add_value(lhs[0], match.group(2))
            assigns.append(
                f"  assign {value.net} = {_literal(match.group(1), value.width)};"
            )
            continue

        if rhs.startswith("pyc.fifo "):
            match = re.fullmatch(r"pyc\.fifo\s+(.*?)\s*\{(.*?)\}\s*:\s*(\S+)", rhs)
            if not match or len(lhs) != 3:
                raise PYCVerilogError(f"cannot parse FIFO: {line}")
            inputs = _ssa_names(match.group(1))
            if len(inputs) != 5:
                raise PYCVerilogError(f"FIFO expects five operands: {line}")
            depth = _parse_attr_int(match.group(2), "depth")
            if depth <= 0:
                raise PYCVerilogError("FIFO depth must be positive")
            payload = match.group(3)
            out_values = [
                add_value(lhs[0], "i1"),
                add_value(lhs[1], "i1"),
                add_value(lhs[2], payload),
            ]
            in_valid, in_data, out_ready = (
                _value(values, inputs[2]),
                _value(values, inputs[3]),
                _value(values, inputs[4]),
            )
            clk, rst = _value(values, inputs[0]), _value(values, inputs[1])
            instances.append(
                f"  pyc_fifo #(.WIDTH({out_values[2].width}), .DEPTH({depth})) fifo_{out_values[0].net} (\n"
                f"    .clk({clk.net}), .rst({rst.net}),\n"
                f"    .in_valid({in_valid.net}), .in_ready({out_values[0].net}), .in_data({in_data.net}),\n"
                f"    .out_valid({out_values[1].net}), .out_ready({out_ready.net}), .out_data({out_values[2].net})\n"
                f"  );"
            )
            continue

        if rhs.startswith("pyc.reg "):
            match = re.fullmatch(r"pyc\.reg\s+(.*?)\s*:\s*(\S+)", rhs)
            if not match or len(lhs) != 1:
                raise PYCVerilogError(f"cannot parse register: {line}")
            inputs = _ssa_names(match.group(1))
            if len(inputs) != 5:
                raise PYCVerilogError(f"register expects five operands: {line}")
            q = add_value(lhs[0], match.group(2))
            clk, rst, en, data, init = (_value(values, item) for item in inputs)
            instances.append(
                f"  pyc_reg #(.WIDTH({q.width})) reg_{q.net} (\n"
                f"    .clk({clk.net}), .rst({rst.net}), .en({en.net}),\n"
                f"    .d({data.net}), .init({init.net}), .q({q.net})\n"
                f"  );"
            )
            continue

        if rhs.startswith("pyc.rr_arbiter "):
            match = re.fullmatch(
                r"pyc\.rr_arbiter\s+(.*?)\s*\{(.*?)\}\s*:\s*(.*?)\s*->\s*(\S+)", rhs
            )
            if not match or len(lhs) != 1:
                raise PYCVerilogError(f"cannot parse round-robin arbiter: {line}")
            inputs = _ssa_names(match.group(1))
            if len(inputs) != 2:
                raise PYCVerilogError(
                    f"round-robin arbiter expects request and cursor: {line}"
                )
            req, cursor = (_value(values, item) for item in inputs)
            out = add_value(lhs[0], match.group(4))
            num_inputs = _parse_attr_int(match.group(2), "num_inputs")
            if num_inputs != req.width or num_inputs != out.width:
                raise PYCVerilogError(
                    "rr_arbiter num_inputs must match request and grant widths"
                )
            if (1 << cursor.width) < num_inputs:
                raise PYCVerilogError(
                    "rr_arbiter cursor width cannot address every input"
                )
            instances.append(
                f"  pyc_rr_arbiter #(.NUM_INPUTS({num_inputs}), .POINTER_WIDTH({cursor.width})) rr_arbiter_{out.net} (\n"
                f"    .req({req.net}), .cursor({cursor.net}), .grant({out.net})\n"
                f"  );"
            )
            continue

        if rhs.startswith("pyc.concat("):
            match = re.fullmatch(r"pyc\.concat\((.*?)\)\s*:\s*(.*?)\s*->\s*(\S+)", rhs)
            if not match or len(lhs) != 1:
                raise PYCVerilogError(f"cannot parse concat: {line}")
            inputs = _ssa_names(match.group(1))
            out = add_value(lhs[0], match.group(3))
            annotated_types = _parse_types(match.group(2))
            input_values = [_value(values, item) for item in inputs]
            if (
                len(annotated_types) != len(input_values)
                or any(
                    value.type != annotated
                    for value, annotated in zip(input_values, annotated_types)
                )
                or sum(value.width for value in input_values) != out.width
            ):
                raise PYCVerilogError("concat operand/result types are inconsistent")
            assigns.append(
                f"  assign {out.net} = {{{', '.join(_value(values, item).net for item in inputs)}}};"
            )
            continue

        if rhs.startswith("pyc.extract "):
            match = re.fullmatch(
                r"pyc\.extract\s+(.*?)\s*\{(.*?)\}\s*:\s*(\S+)\s*->\s*(\S+)", rhs
            )
            if not match or len(lhs) != 1:
                raise PYCVerilogError(f"cannot parse extract: {line}")
            inputs = _ssa_names(match.group(1))
            if len(inputs) != 1:
                raise PYCVerilogError(f"extract expects one operand: {line}")
            src = _value(values, inputs[0])
            out = add_value(lhs[0], match.group(4))
            if src.type != match.group(3):
                raise PYCVerilogError(
                    "extract source annotation does not match operand"
                )
            lsb = _parse_attr_int(match.group(2), "lsb")
            if lsb < 0 or lsb + out.width > src.width:
                raise PYCVerilogError("extract slice is outside source width")
            if out.width == 1:
                assigns.append(f"  assign {out.net} = {src.net}[{lsb}];")
            else:
                assigns.append(
                    f"  assign {out.net} = {src.net}[{lsb + out.width - 1}:{lsb}];"
                )
            continue

        if rhs.startswith("pyc.select "):
            match = re.fullmatch(r"pyc\.select\s+(.*?)\s*:\s*(.*?)\s*->\s*(\S+)", rhs)
            if not match or len(lhs) != 1:
                raise PYCVerilogError(f"cannot parse select: {line}")
            inputs = _ssa_names(match.group(1))
            if len(inputs) != 3:
                raise PYCVerilogError(f"select expects condition, true, false: {line}")
            select, true_value, false_value = (_value(values, item) for item in inputs)
            annotated_types = _parse_types(match.group(2))
            out = add_value(lhs[0], match.group(3))
            if (
                annotated_types != [select.type, true_value.type, false_value.type]
                or select.type != "i1"
                or true_value.type != out.type
                or false_value.type != out.type
            ):
                raise PYCVerilogError("select condition/data types are inconsistent")
            assigns.append(
                f"  assign {out.net} = {select.net} ? {true_value.net} : {false_value.net};"
            )
            continue

        cast = re.fullmatch(
            r"pyc\.(zext|sext|trunc|alias)\s+(%[A-Za-z_][A-Za-z0-9_]*)"
            r"(?:\s*\{.*?\})?\s*:\s*(?:(\S+)\s*->\s*)?(\S+)",
            rhs,
        )
        if cast and len(lhs) == 1:
            source = _value(values, cast.group(2))
            annotated_source = cast.group(3)
            out = add_value(lhs[0], cast.group(4))
            if annotated_source is not None and source.type != annotated_source:
                raise PYCVerilogError("cast source annotation does not match operand")
            if cast.group(1) == "alias" and source.type != out.type:
                raise PYCVerilogError("alias source/result types must match")
            if cast.group(1) == "trunc" and source.width < out.width:
                raise PYCVerilogError("trunc cannot widen its operand")
            if cast.group(1) in {"zext", "sext"} and source.width > out.width:
                raise PYCVerilogError("extension cannot narrow its operand")
            if cast.group(1) == "sext" and out.width > source.width:
                fill = out.width - source.width
                assigns.append(
                    f"  assign {out.net} = "
                    f"{{{{{fill}{{{source.net}[{source.width - 1}]}}}}, {source.net}}};"
                )
            elif cast.group(1) == "trunc" and out.width < source.width:
                assigns.append(f"  assign {out.net} = {source.net}[{out.width - 1}:0];")
            else:
                assigns.append(f"  assign {out.net} = {source.net};")
            continue

        if rhs.startswith("pyc.assign "):
            match = re.fullmatch(
                r"pyc\.assign\s+(.*?),\s*(%[A-Za-z_][A-Za-z0-9_]*)\s*:\s*(\S+)", rhs
            )
            if not match:
                raise PYCVerilogError(f"cannot parse assign: {line}")
            target = _value(values, match.group(1).strip())
            source = _value(values, match.group(2))
            if target.type != match.group(3) or source.type != target.type:
                raise PYCVerilogError("assign source/target types are inconsistent")
            assigns.append(f"  assign {target.net} = {source.net};")
            continue

        binary = re.fullmatch(
            r"pyc\.(and|or|xor|add|sub|mul)\s+"
            r"(.*?)\s*:\s*(.*?)\s*->\s*(\S+)",
            rhs,
        )
        if binary and len(lhs) == 1:
            inputs = _ssa_names(binary.group(2))
            if len(inputs) != 2:
                raise PYCVerilogError(f"binary PYC op expects two operands: {line}")
            left, right = (_value(values, item) for item in inputs)
            operand_types = _parse_types(binary.group(3))
            if operand_types != [left.type, right.type]:
                raise PYCVerilogError("binary operand annotations are inconsistent")
            out = add_value(lhs[0], binary.group(4))
            if out.type != left.type:
                raise PYCVerilogError("binary result type must match operands")
            operator = {
                "and": "&",
                "or": "|",
                "xor": "^",
                "add": "+",
                "sub": "-",
                "mul": "*",
            }[binary.group(1)]
            expression = f"{left.net} {operator} {right.net}"
            assigns.append(f"  assign {out.net} = {expression};")
            continue

        compare = re.fullmatch(
            r'pyc\.cmp\s+(.*?)\s*\{\s*predicate\s*=\s*"(eq|ult|slt)"\s*\}'
            r"\s*:\s*(.*?)\s*->\s*(\S+)",
            rhs,
        )
        if compare and len(lhs) == 1:
            inputs = _ssa_names(compare.group(1))
            if len(inputs) != 2:
                raise PYCVerilogError(f"cmp expects two operands: {line}")
            left, right = (_value(values, item) for item in inputs)
            if _parse_types(compare.group(3)) != [left.type, right.type]:
                raise PYCVerilogError("cmp operand annotations are inconsistent")
            out = add_value(lhs[0], compare.group(4))
            if out.type != "i1":
                raise PYCVerilogError("cmp result must be i1")
            predicate = compare.group(2)
            if predicate == "eq":
                expression = f"{left.net} == {right.net}"
            elif predicate == "ult":
                expression = f"{left.net} < {right.net}"
            else:
                expression = f"$signed({left.net}) < $signed({right.net})"
            assigns.append(f"  assign {out.net} = {expression};")
            continue

        unary = re.fullmatch(r"pyc\.not\s+(.*?)\s*:\s*(\S+)", rhs)
        if unary and len(lhs) == 1:
            inputs = _ssa_names(unary.group(1))
            if len(inputs) != 1:
                raise PYCVerilogError(f"not expects one operand: {line}")
            src = _value(values, inputs[0])
            out = add_value(lhs[0], unary.group(2))
            if src.type != out.type:
                raise PYCVerilogError("not source/result types must match")
            assigns.append(f"  assign {out.net} = ~{src.net};")
            continue

        raise PYCVerilogError(f"unsupported canonical PYC operation: {line}")

    if returns is None:
        raise PYCVerilogError("PYC function has no return")
    if len(returns) != len(module.results):
        raise PYCVerilogError("PYC return count does not match function result_names")
    for result, returned in zip(module.results, returns):
        source = _value(values, returned)
        if result.type not in ("!pyc.clock", "!pyc.reset"):
            seen_outputs.add(result.net)
            assigns.append(f"  assign {result.net} = {source.net};")

    ports = [_port_decl("input", value) for value in module.args]
    ports.extend(_port_decl("output", value) for value in module.results)
    # The output intentionally embeds several primitive modules after the
    # top-level module.  Tell Verilator that the file name only identifies the
    # requested top module; secondary runtime modules are expected here.
    text = [
        "// Generated by agentic-circuit PYC compatibility backend",
        f"// Source function: {module.name}",
        "/* verilator lint_off DECLFILENAME */",
        "",
        f"module {module.name} (",
        ",\n".join(ports),
        ");",
    ]
    text.extend(declarations)
    text.extend(assigns)
    text.extend(assertions)
    text.append("")
    text.extend(instances)
    text.extend(["", "endmodule", ""])
    text.extend(_runtime_sources(runtime_dir))
    text.extend(["", "/* verilator lint_on DECLFILENAME */", ""])
    return "\n".join(text)


def _default_pycgen() -> str:
    script = Path(__file__).resolve()
    candidates = (
        script.parents[3]
        / ".pycircuit_out"
        / "acir"
        / "dev-llvm22"
        / "bin"
        / "acir-queue-pycgen",
        script.parent / "acir-queue-pycgen",
    )
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    return shutil.which("acir-queue-pycgen") or "acir-queue-pycgen"


def _default_runtime_dir() -> Path:
    script = Path(__file__).resolve()
    candidates = (
        # Source-tree invocation.
        script.parents[3] / "library" / "verilog",
        # Installed invocation: <prefix>/bin/script and <prefix>/include/verilog.
        script.parents[1] / "include" / "verilog",
    )
    for candidate in candidates:
        if candidate.is_dir():
            return candidate
    # Keep a useful diagnostic from emit_verilog if packaging is incomplete.
    return candidates[0]


def _write_atomic(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        dir=path.parent, prefix=f".{path.name}.", suffix=".tmp", text=True
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="acir-queue-veriloggen.py", description=__doc__
    )
    parser.add_argument(
        "input",
        type=Path,
        help="frozen Queue ACIR MLIR, or canonical PYC with --pyc-input",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("-"),
        help="output Verilog path (default: stdout)",
    )
    parser.add_argument(
        "--pycgen", default=_default_pycgen(), help="acir-queue-pycgen executable"
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=120.0,
        help="maximum seconds allowed for ACIR-to-PYC lowering (default: 120)",
    )
    parser.add_argument(
        "--pyc-input",
        action="store_true",
        help="treat input as canonical PYC instead of ACIR",
    )
    parser.add_argument(
        "--emit-pyc", type=Path, help="also save the canonical PYC artifact"
    )
    parser.add_argument("--runtime-dir", type=Path, default=_default_runtime_dir())
    args = parser.parse_args(argv)

    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be a finite positive number")
    try:
        input_identity = args.input.resolve(strict=True)
    except OSError as exc:
        parser.error(f"cannot resolve input: {exc}")
    destinations = [
        path for path in (args.emit_pyc, args.output) if path and str(path) != "-"
    ]
    destination_identities = [path.resolve(strict=False) for path in destinations]
    if input_identity in destination_identities:
        parser.error("input and output paths must be different")
    if len(destination_identities) != len(set(destination_identities)):
        parser.error("--emit-pyc and --output must be different paths")

    if args.pyc_input:
        try:
            pyc_text = args.input.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            parser.error(f"cannot read PYC input: {exc}")
    else:
        try:
            completed = subprocess.run(
                [args.pycgen, str(args.input)],
                check=True,
                capture_output=True,
                text=True,
                timeout=args.timeout,
            )
        except FileNotFoundError:
            parser.error(f"cannot find ACIR-to-PYC generator: {args.pycgen}")
        except subprocess.TimeoutExpired:
            parser.error(
                f"ACIR-to-PYC generator exceeded --timeout={args.timeout:g}s; "
                "reduce the input or run the single queue target with -j1"
            )
        except subprocess.CalledProcessError as exc:
            sys.stderr.write(exc.stderr or "")
            return 1
        pyc_text = completed.stdout
    if args.emit_pyc:
        try:
            _write_atomic(args.emit_pyc, pyc_text)
        except OSError as exc:
            parser.error(f"cannot publish PYC output: {exc}")

    try:
        output = emit_verilog(parse_pyc_module(pyc_text), args.runtime_dir)
    except (OSError, PYCVerilogError) as exc:
        parser.error(str(exc))
    if str(args.output) == "-":
        sys.stdout.write(output)
    else:
        try:
            _write_atomic(args.output, output)
        except OSError as exc:
            parser.error(f"cannot publish Verilog output: {exc}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
