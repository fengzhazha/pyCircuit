"""Immutable metadata for Python architecture definitions."""

from __future__ import annotations

import inspect
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Literal, TypeAlias, TypeVar, overload


DefinitionKind: TypeAlias = Literal[
    "system",
    "module",
    "extern_module",
    "struct",
    "packet",
    "transaction",
    "protocol",
    "interface",
    "process",
    "rule",
]
F = TypeVar("F", bound=Callable[..., object])


@dataclass(frozen=True, slots=True)
class Definition:
    """A captured definition registered without executing its body."""

    kind: DefinitionKind
    function: Callable[..., object]
    qualified_name: str
    explicit_options: tuple[tuple[str, object], ...]
    module_name: str
    source_file: str | None
    source_line: int | None

    def __repr__(self) -> str:
        return f"Definition(kind={self.kind!r}, qualified_name={self.qualified_name!r})"

    @property
    def __signature__(self) -> inspect.Signature:
        return inspect.signature(self.function)

    @property
    def __name__(self) -> str:
        return self.function.__name__

    def __call__(self, *args: object, **kwargs: object) -> "PendingDefinitionCall":
        try:
            bound = self.__signature__.bind(*args, **kwargs)
        except TypeError as error:
            raise TypeError(f"ACPY-CALL-003: {error}") from error
        bound.apply_defaults()
        return PendingDefinitionCall(self, tuple(bound.arguments.items()))


@dataclass(frozen=True, slots=True)
class PendingDefinitionCall:
    definition: Definition
    arguments: tuple[tuple[str, object], ...]


@overload
def _decorate(definition_kind: DefinitionKind, function: F) -> Definition: ...


@overload
def _decorate(
    definition_kind: DefinitionKind, function: None = None, **options: object
) -> Callable[[F], Definition]: ...


def _decorate(
    definition_kind: DefinitionKind, function: F | None = None, **options: object
) -> Definition | Callable[[F], Definition]:
    def apply(target: F) -> Definition:
        module_name = getattr(target, "__module__", "")
        source_file = inspect.getsourcefile(target)
        source_line: int | None
        code = getattr(target, "__code__", None)
        if code is not None:
            source_line = code.co_firstlineno
        else:
            try:
                _, source_line = inspect.getsourcelines(target)
            except (OSError, TypeError):
                source_line = None
        return Definition(
            kind=definition_kind,
            function=target,
            qualified_name=target.__qualname__,
            explicit_options=tuple(sorted(options.items())),
            module_name=module_name,
            source_file=(
                str(Path(source_file).resolve()) if source_file is not None else None
            ),
            source_line=source_line,
        )

    return apply(function) if function is not None else apply


def system(function: F | None = None, **options: object):
    return _decorate("system", function, **options)


def module(function: F | None = None, **options: object):
    return _decorate("module", function, **options)


def extern_module(function: F | None = None, **options: object):
    return _decorate("extern_module", function, **options)


def struct(function: F | None = None, **options: object):
    return _decorate("struct", function, **options)


def packet(function: F | None = None, **options: object):
    return _decorate("packet", function, **options)


def transaction(function: F | None = None, **options: object):
    return _decorate("transaction", function, **options)


def protocol(function: F | None = None, **options: object):
    return _decorate("protocol", function, **options)


def interface(function: F | None = None, **options: object):
    return _decorate("interface", function, **options)


def process(function: F | None = None, **options: object):
    return _decorate("process", function, **options)


def rule(function: F | None = None, **options: object):
    """Declare one schedulable rule without executing its Python body."""

    return _decorate("rule", function, **options)
