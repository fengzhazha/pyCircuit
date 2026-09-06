"""Closed interpreter for deterministic elaboration-time expressions."""

from __future__ import annotations

import ast
import math
import operator
from collections.abc import Callable, Iterator, Mapping
from dataclasses import dataclass, field
from types import MappingProxyType
from typing import TypeAlias


StaticScalar: TypeAlias = None | bool | int | float | str

_MAX_SAFE_INTEGER = (1 << 53) - 1
MAX_STATIC_EXPANSION = 10_000


class StaticEvalError(ValueError):
    """Raised when an expression has no portable static meaning."""


@dataclass(frozen=True, slots=True)
class FrozenMap(Mapping[str, "StaticValue"]):
    entries: tuple[tuple[str, StaticValue], ...]

    def __getitem__(self, key: str) -> StaticValue:
        for candidate, value in self.entries:
            if candidate == key:
                return value
        raise KeyError(key)

    def __iter__(self) -> Iterator[str]:
        return (key for key, _ in self.entries)

    def __len__(self) -> int:
        return len(self.entries)


StaticValue: TypeAlias = StaticScalar | tuple["StaticValue", ...] | FrozenMap
StaticHelper: TypeAlias = Callable[..., StaticValue]


@dataclass(frozen=True, slots=True)
class StaticEnvironment:
    values: Mapping[str, StaticValue]
    helpers: Mapping[str, StaticHelper] = field(default_factory=dict)

    def __post_init__(self) -> None:
        object.__setattr__(self, "values", MappingProxyType(dict(self.values)))
        object.__setattr__(self, "helpers", MappingProxyType(dict(self.helpers)))


def _check_scalar(value: StaticScalar) -> StaticScalar:
    if isinstance(value, int) and not isinstance(value, bool):
        if not -_MAX_SAFE_INTEGER <= value <= _MAX_SAFE_INTEGER:
            raise StaticEvalError("integer is outside the portable I-JSON range")
    if isinstance(value, float) and not math.isfinite(value):
        raise StaticEvalError("static floating-point values must be finite")
    return value


def validate_ijson_value(value: StaticValue) -> None:
    if value is None or isinstance(value, (bool, int, float, str)):
        _check_scalar(value)
        return
    if isinstance(value, tuple):
        for item in value:
            validate_ijson_value(item)
        return
    if isinstance(value, FrozenMap):
        for _, item in value.entries:
            validate_ijson_value(item)
        return
    raise StaticEvalError(f"unsupported static value {type(value).__name__}")


class _StaticEvaluator(ast.NodeVisitor):
    _binary = {
        ast.Add: operator.add,
        ast.Sub: operator.sub,
        ast.Mult: operator.mul,
        ast.FloorDiv: operator.floordiv,
        ast.Mod: operator.mod,
    }
    _unary = {ast.UAdd: operator.pos, ast.USub: operator.neg, ast.Not: operator.not_}
    _comparison = {
        ast.Eq: operator.eq,
        ast.NotEq: operator.ne,
        ast.Lt: operator.lt,
        ast.LtE: operator.le,
        ast.Gt: operator.gt,
        ast.GtE: operator.ge,
    }

    def __init__(self, environment: StaticEnvironment) -> None:
        self._values = dict(environment.values)
        self._helpers = environment.helpers
        self._expansions = 0

    def generic_visit(self, node: ast.AST):
        raise StaticEvalError(f"unsupported static syntax: {type(node).__name__}")

    def visit_Constant(self, node: ast.Constant) -> StaticValue:
        value = node.value
        if value is None or isinstance(value, (bool, int, float, str)):
            return _check_scalar(value)
        raise StaticEvalError(f"unsupported literal {type(value).__name__}")

    def visit_Name(self, node: ast.Name) -> StaticValue:
        try:
            return self._values[node.id]
        except KeyError as error:
            raise StaticEvalError(f"unknown static name {node.id!r}") from error

    def visit_Tuple(self, node: ast.Tuple) -> StaticValue:
        return tuple(self.visit(item) for item in node.elts)

    def visit_List(self, node: ast.List) -> StaticValue:
        return tuple(self.visit(item) for item in node.elts)

    def visit_Dict(self, node: ast.Dict) -> StaticValue:
        entries: dict[str, StaticValue] = {}
        for key_node, value_node in zip(node.keys, node.values, strict=True):
            if key_node is None:
                raise StaticEvalError("dictionary unpacking is not static")
            key = self.visit(key_node)
            if not isinstance(key, str):
                raise StaticEvalError("static map keys must be strings")
            if key in entries:
                raise StaticEvalError(f"duplicate static map key {key!r}")
            entries[key] = self.visit(value_node)
        return FrozenMap(tuple(sorted(entries.items())))

    def visit_Attribute(self, node: ast.Attribute) -> StaticValue:
        value = self.visit(node.value)
        if isinstance(value, FrozenMap):
            try:
                return value[node.attr]
            except KeyError as error:
                raise StaticEvalError(f"unknown static field {node.attr!r}") from error
        raise StaticEvalError("static attribute access requires a frozen record")

    def visit_BinOp(self, node: ast.BinOp) -> StaticValue:
        function = self._binary.get(type(node.op))
        if function is None:
            raise StaticEvalError(
                f"unsupported static operator {type(node.op).__name__}"
            )
        left = self.visit(node.left)
        right = self.visit(node.right)
        if type(left) not in (int, float, str, tuple) or type(right) not in (
            int,
            float,
            str,
            tuple,
        ):
            raise StaticEvalError("static arithmetic operands are incompatible")
        try:
            result = function(left, right)
        except (ArithmeticError, TypeError) as error:
            raise StaticEvalError("static arithmetic failed") from error
        validate_ijson_value(result)
        return result

    def visit_UnaryOp(self, node: ast.UnaryOp) -> StaticValue:
        function = self._unary.get(type(node.op))
        if function is None:
            raise StaticEvalError(
                f"unsupported static unary operator {type(node.op).__name__}"
            )
        value = self.visit(node.operand)
        try:
            result = function(value)
        except TypeError as error:
            raise StaticEvalError("static unary operation failed") from error
        validate_ijson_value(result)
        return result

    def visit_BoolOp(self, node: ast.BoolOp) -> StaticValue:
        values = [self.visit(value) for value in node.values]
        if any(type(value) is not bool for value in values):
            raise StaticEvalError("static boolean operators require booleans")
        return all(values) if isinstance(node.op, ast.And) else any(values)

    def visit_Compare(self, node: ast.Compare) -> StaticValue:
        left = self.visit(node.left)
        for operation, comparator in zip(node.ops, node.comparators, strict=True):
            right = self.visit(comparator)
            function = self._comparison.get(type(operation))
            if function is None:
                raise StaticEvalError(
                    f"unsupported static comparison {type(operation).__name__}"
                )
            try:
                matches = function(left, right)
            except TypeError as error:
                raise StaticEvalError(
                    "static comparison operands are incompatible"
                ) from error
            if not matches:
                return False
            left = right
        return True

    def visit_IfExp(self, node: ast.IfExp) -> StaticValue:
        condition = self.visit(node.test)
        if type(condition) is not bool:
            raise StaticEvalError("static condition must be boolean")
        return self.visit(node.body if condition else node.orelse)

    def visit_Subscript(self, node: ast.Subscript) -> StaticValue:
        value = self.visit(node.value)
        index = self.visit(node.slice)
        if isinstance(value, FrozenMap) and isinstance(index, str):
            return value[index]
        if isinstance(value, (tuple, str)) and type(index) is int:
            try:
                return value[index]
            except IndexError as error:
                raise StaticEvalError("static index is out of range") from error
        raise StaticEvalError("unsupported static subscript")

    def _range(self, node: ast.Call) -> tuple[StaticValue, ...]:
        if node.keywords:
            raise StaticEvalError("range does not accept keywords here")
        arguments = [self.visit(argument) for argument in node.args]
        if not 1 <= len(arguments) <= 3 or any(
            type(item) is not int for item in arguments
        ):
            raise StaticEvalError("range requires one to three integer arguments")
        if len(arguments) == 1:
            start, stop, step = 0, arguments[0], 1
        elif len(arguments) == 2:
            start, stop, step = arguments[0], arguments[1], 1
        else:
            start, stop, step = arguments
        if step <= 0:
            raise StaticEvalError("range step must be positive")
        bounded_range = range(start, stop, step)
        try:
            length = len(bounded_range)
        except OverflowError as error:
            raise StaticEvalError("static expansion exceeds the maximum") from error
        self._consume_expansion(length)
        result = tuple(bounded_range)
        return result

    def _consume_expansion(self, amount: int) -> None:
        self._expansions += amount
        if self._expansions > MAX_STATIC_EXPANSION:
            raise StaticEvalError("static expansion exceeds the maximum")

    def visit_Call(self, node: ast.Call) -> StaticValue:
        if not isinstance(node.func, ast.Name):
            raise StaticEvalError("unapproved call in static expression")
        name = node.func.id
        if name == "range":
            return self._range(node)
        if name in ("tuple", "list"):
            if len(node.args) != 1 or node.keywords:
                raise StaticEvalError(f"{name} requires one positional argument")
            value = self.visit(node.args[0])
            if not isinstance(value, tuple):
                raise StaticEvalError(f"{name} requires a static iterable")
            return value
        helper = self._helpers.get(name)
        if helper is None:
            raise StaticEvalError(f"unapproved call {name!r} in static expression")
        arguments = [self.visit(argument) for argument in node.args]
        keywords = {keyword.arg: self.visit(keyword.value) for keyword in node.keywords}
        if None in keywords:
            raise StaticEvalError("static helper keyword unpacking is forbidden")
        result = helper(*arguments, **keywords)
        validate_ijson_value(result)
        return result

    def _bind_target(self, target: ast.expr, value: StaticValue) -> None:
        if isinstance(target, ast.Name):
            self._values[target.id] = value
            return
        if isinstance(target, (ast.Tuple, ast.List)) and isinstance(value, tuple):
            if len(target.elts) != len(value):
                raise StaticEvalError("comprehension target shape does not match")
            for child, item in zip(target.elts, value, strict=True):
                self._bind_target(child, item)
            return
        raise StaticEvalError("unsupported comprehension target")

    def _target_names(self, target: ast.expr) -> set[str]:
        if isinstance(target, ast.Name):
            return {target.id}
        if isinstance(target, (ast.Tuple, ast.List)):
            names: set[str] = set()
            for child in target.elts:
                names.update(self._target_names(child))
            return names
        raise StaticEvalError("unsupported comprehension target")

    def _comprehend(
        self, element: ast.expr, generators: list[ast.comprehension], index: int = 0
    ) -> list[StaticValue]:
        if index == len(generators):
            self._consume_expansion(1)
            return [self.visit(element)]
        generator = generators[index]
        if generator.is_async:
            raise StaticEvalError("async comprehensions are forbidden")
        iterable = self.visit(generator.iter)
        if not isinstance(iterable, tuple):
            raise StaticEvalError("comprehension iterable must be statically bounded")
        output: list[StaticValue] = []
        for item in iterable:
            self._bind_target(generator.target, item)
            include = True
            for condition_node in generator.ifs:
                condition = self.visit(condition_node)
                if type(condition) is not bool:
                    raise StaticEvalError("comprehension condition must be boolean")
                if not condition:
                    include = False
                    break
            if include:
                output.extend(self._comprehend(element, generators, index + 1))
        return output

    def _evaluate_comprehension(
        self, element: ast.expr, generators: list[ast.comprehension]
    ) -> tuple[StaticValue, ...]:
        names: set[str] = set()
        for generator in generators:
            names.update(self._target_names(generator.target))
        missing = object()
        previous = {name: self._values.get(name, missing) for name in names}
        try:
            return tuple(self._comprehend(element, generators))
        finally:
            for name, value in previous.items():
                if value is missing:
                    self._values.pop(name, None)
                else:
                    self._values[name] = value

    def visit_GeneratorExp(self, node: ast.GeneratorExp) -> StaticValue:
        return self._evaluate_comprehension(node.elt, node.generators)

    def visit_ListComp(self, node: ast.ListComp) -> StaticValue:
        return self._evaluate_comprehension(node.elt, node.generators)


def evaluate_static(node: ast.AST, environment: StaticEnvironment) -> StaticValue:
    value = _StaticEvaluator(environment).visit(node)
    validate_ijson_value(value)
    return value
