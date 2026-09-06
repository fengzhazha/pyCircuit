"""Agentic Circuit's portable Python construction surface."""

from __future__ import annotations

from pkgutil import extend_path
from typing import Never


__path__ = extend_path(__path__, __name__)

from . import _types as _types_module
from ._definitions import (
    extern_module,
    generated_module,
    interface,
    module,
    packet,
    process,
    protocol,
    rule,
    struct,
    system,
    transaction,
)
from ._jit import config, jit
from ._resources import address_map, address_space, queue
from ._types import (
    BitfieldSpec,
    Endpoint,
    Flow,
    ResourceRef,
    Static,
    array,
    bits,
    const,
    s8,
    s16,
    s32,
    s64,
)

_UNSIGNED_NAMES = tuple(f"u{width}" for width in _types_module.UNSIGNED_WIDTHS)
for _name in _UNSIGNED_NAMES:
    globals()[_name] = getattr(_types_module, _name)
del _name


__all__ = (
    "system",
    "module",
    "extern_module",
    "generated_module",
    "struct",
    "packet",
    "transaction",
    "protocol",
    "interface",
    "process",
    "rule",
    "scope",
    "array",
    "map",
    "set",
    "instances",
    "view",
    "find",
    "bits",
    "BitfieldSpec",
    "concat",
    "insert",
    "matches",
    "queue",
    "ResourceRef",
    "address_space",
    "address_map",
    "Static",
    "Flow",
    "Endpoint",
    "source",
    "count_leading_zeros",
    "count_trailing_zeros",
    "popcount",
    "priority_encode",
    "memory",
    "sink",
    "observe",
    "expect",
    "compute",
    "pipeline",
    "config",
    "const",
    "jit",
    "route",
    "merge",
    "schedule",
    "engine",
    "reorder",
    "round_robin",
    "priority",
    "fork",
    "barrier",
    "table",
    "slot",
    *_UNSIGNED_NAMES,
    "s8",
    "s16",
    "s32",
    "s64",
)


def _not_implemented(primitive: str) -> Never:
    raise NotImplementedError(
        f"{primitive} is part of the public surface but is not implemented yet"
    )


def scope(name: str) -> Never:
    return _not_implemented("scope")


def map(*values: object) -> Never:
    return _not_implemented("map")


def set(*values: object) -> Never:
    return _not_implemented("set")


def instances(*values: object) -> Never:
    return _not_implemented("instances")


def view(value: object, *selectors: object) -> Never:
    return _not_implemented("view")


def find(values: object, *, where: object, key: object | None = None) -> Never:
    _ = (values, where, key)
    return _not_implemented("find")


def concat(*values: object) -> Never:
    return _not_implemented("concat")


def insert(value: object, field: object, *, lsb: int) -> Never:
    _ = (value, field, lsb)
    return _not_implemented("insert")


def matches(value: object, pattern: str) -> Never:
    _ = (value, pattern)
    return _not_implemented("matches")


def source(
    payload: object, *, depth: int = 1, latency: int = 1, rate: int = 1
) -> Never:
    return _not_implemented("source")


def popcount(value: object) -> Never:
    return _not_implemented("popcount")


def count_leading_zeros(value: object) -> Never:
    return _not_implemented("count_leading_zeros")


def count_trailing_zeros(value: object) -> Never:
    return _not_implemented("count_trailing_zeros")


def priority_encode(value: object, *, order: str = "low") -> Never:
    _ = (value, order)
    return _not_implemented("priority_encode")


def memory(
    data_type: object, *, entries: int = 16, init: int = 0, latency: int = 1
) -> Never:
    return _not_implemented("memory")


def sink(value: object) -> Never:
    return _not_implemented("sink")


def observe(value: object) -> Never:
    return _not_implemented("observe")


def expect(value: object, *, predicate: object, message: str) -> Never:
    return _not_implemented("expect")


def compute(
    value: object,
    function: object,
    *,
    depth: int = 1,
    latency: int = 1,
    rate: int = 1,
) -> Never:
    return _not_implemented("compute")


def pipeline(
    value: object,
    *,
    stages: int = 1,
    depth: int = 1,
    rate: int = 1,
) -> Never:
    return _not_implemented("pipeline")


round_robin = "round_robin"
priority = "priority"


def route(
    value: object,
    *,
    by: object,
    outputs: int,
    depth: int = 1,
    latency: int = 1,
) -> Never:
    return _not_implemented("route")


def merge(
    *values: object,
    policy: object = priority,
    depth: int = 1,
    latency: int = 1,
) -> Never:
    return _not_implemented("merge")


def schedule(
    value: object,
    *,
    by: object,
    waits_for: object,
    resource: object,
    cost: object,
    entries: int = 16,
    resources: int = 1,
    no_dependency: int = 0,
    depth: int = 1,
    latency: int = 1,
) -> Never:
    return _not_implemented("schedule")


def engine(
    value: object,
    *,
    cost: object,
    lanes: int = 1,
    depth: int = 1,
    latency: int = 1,
) -> Never:
    return _not_implemented("engine")


def reorder(
    value: object,
    *,
    by: object,
    entries: int = 16,
    start: int = 0,
    depth: int = 1,
    latency: int = 1,
) -> Never:
    return _not_implemented("reorder")


def fork(
    value: object,
    *,
    outputs: int,
    depth: int = 1,
    latency: int = 1,
) -> Never:
    return _not_implemented("fork")


def barrier(
    *values: object,
    depth: int = 1,
    latency: int = 1,
) -> Never:
    return _not_implemented("barrier")


class _TableDeclaration:
    def __init__(self, entries: object, entry_type: object) -> None:
        self.entries = entries
        self.entry_type = entry_type

    def __call__(self, *, init: int = 0) -> Never:
        return _not_implemented("table")


class _TableFactory:
    def __getitem__(self, parameters: object) -> _TableDeclaration:
        if not isinstance(parameters, tuple) or len(parameters) != 2:
            raise TypeError("ac.table requires ac.table[entries, Entry]")
        return _TableDeclaration(parameters[0], parameters[1])

    def __call__(self, *args: object, **kwargs: object) -> Never:
        del args, kwargs
        raise TypeError(
            "legacy ac.table(value, ...) was removed; use ac.memory for "
            "request/response memory or ac.table[entries, Entry](init=0) "
            "for state Table"
        )


table = _TableFactory()


def slot(value: object) -> Never:
    return _not_implemented("slot")
