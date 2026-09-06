"""Immutable recursive value-type descriptors shared by pyCircuit frontends."""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from typing import TypeAlias


class ValueTypeError(ValueError):
    """A deterministic rejection of an invalid value-type descriptor."""


def _name(value: object, kind: str) -> str:
    if type(value) is not str or not value.strip():
        raise ValueTypeError(f"{kind} name must be a non-empty string")
    return value.strip()


class ValueType:
    """Base contract for immutable recursive types."""

    __slots__ = ()

    def canonical(self) -> dict[str, object]:
        raise NotImplementedError

    def mlir(self, *, scope: str = "types") -> str:
        raise NotImplementedError

    def bit_width(self) -> int:
        raise NotImplementedError

    @property
    def fingerprint(self) -> str:
        encoded = json.dumps(
            self.canonical(),
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
        return "sha256:" + hashlib.sha256(encoded).hexdigest()


@dataclass(frozen=True, slots=True)
class BoolType(ValueType):
    """A logical Boolean, currently serialized as MLIR ``i1``."""

    def canonical(self) -> dict[str, object]:
        return {"kind": "bool", "version": 1}

    def mlir(self, *, scope: str = "types") -> str:
        _ = scope
        return "i1"

    def bit_width(self) -> int:
        return 1


@dataclass(frozen=True, slots=True)
class BitsType(ValueType):
    """An exact-width unsigned bit-vector."""

    width: int

    def __post_init__(self) -> None:
        if type(self.width) is not int or not 1 <= self.width <= 64:
            raise ValueTypeError("bits width must be in [1, 64]")

    def canonical(self) -> dict[str, object]:
        return {"kind": "bits", "version": 1, "width": self.width}

    def mlir(self, *, scope: str = "types") -> str:
        _ = scope
        return f"i{self.width}"

    def bit_width(self) -> int:
        return self.width


@dataclass(frozen=True, slots=True)
class EnumType(ValueType):
    """A nominal enum with stable declaration-order encoding."""

    name: str
    enumerants: tuple[str, ...]

    def __post_init__(self) -> None:
        object.__setattr__(self, "name", _name(self.name, "enum"))
        values = tuple(_name(value, "enumerant") for value in self.enumerants)
        if not values:
            raise ValueTypeError("enum requires at least one enumerant")
        if len(set(values)) != len(values):
            raise ValueTypeError("enum enumerants must be unique")
        object.__setattr__(self, "enumerants", values)

    @property
    def encoding_width(self) -> int:
        return max(1, (len(self.enumerants) - 1).bit_length())

    def canonical(self) -> dict[str, object]:
        return {
            "kind": "enum",
            "version": 1,
            "name": self.name,
            "enumerants": list(self.enumerants),
            "encoding_width": self.encoding_width,
        }

    def mlir(self, *, scope: str = "types") -> str:
        return f"!ac.enum<@{_name(scope, 'type scope')}::@{self.name}>"

    def bit_width(self) -> int:
        return self.encoding_width


@dataclass(frozen=True, slots=True)
class ValueField:
    name: str
    type: ValueType

    def __post_init__(self) -> None:
        object.__setattr__(self, "name", _name(self.name, "field"))
        if not isinstance(self.type, ValueType):
            raise TypeError("field type must be a ValueType")

    def canonical(self) -> dict[str, object]:
        return {"name": self.name, "type": self.type.canonical()}


@dataclass(frozen=True, slots=True)
class StructType(ValueType):
    """A nominal, ordered recursive struct value."""

    name: str
    fields: tuple[ValueField, ...]

    def __post_init__(self) -> None:
        object.__setattr__(self, "name", _name(self.name, "struct"))
        fields = tuple(self.fields)
        if not fields:
            raise ValueTypeError("struct requires at least one field")
        if not all(isinstance(field, ValueField) for field in fields):
            raise TypeError("struct fields must be ValueField values")
        if len({field.name for field in fields}) != len(fields):
            raise ValueTypeError("struct field names must be unique")
        object.__setattr__(self, "fields", fields)

    def field(self, name: str) -> ValueField:
        for field in self.fields:
            if field.name == name:
                return field
        raise KeyError(f"unknown field {name!r} in struct {self.name!r}")

    def canonical(self) -> dict[str, object]:
        return {
            "kind": "struct",
            "version": 1,
            "name": self.name,
            "fields": [field.canonical() for field in self.fields],
        }

    def mlir(self, *, scope: str = "types") -> str:
        return f"!ac.struct<@{_name(scope, 'type scope')}::@{self.name}>"

    def bit_width(self) -> int:
        return sum(field.type.bit_width() for field in self.fields)


@dataclass(frozen=True, slots=True)
class TupleType(ValueType):
    """A structural immutable tuple value."""

    elements: tuple[ValueType, ...]

    def __post_init__(self) -> None:
        elements = tuple(self.elements)
        if not elements:
            raise ValueTypeError("tuple requires at least one element")
        if not all(isinstance(element, ValueType) for element in elements):
            raise TypeError("tuple elements must be ValueType values")
        object.__setattr__(self, "elements", elements)

    def canonical(self) -> dict[str, object]:
        return {
            "kind": "tuple",
            "version": 1,
            "elements": [element.canonical() for element in self.elements],
        }

    def mlir(self, *, scope: str = "types") -> str:
        return (
            "tuple<"
            + ", ".join(element.mlir(scope=scope) for element in self.elements)
            + ">"
        )

    def bit_width(self) -> int:
        return sum(element.bit_width() for element in self.elements)


@dataclass(frozen=True, slots=True)
class ArrayType(ValueType):
    """A structural fixed-length value array, distinct from persistent lists."""

    length: int
    element: ValueType

    def __post_init__(self) -> None:
        if type(self.length) is not int or self.length <= 0:
            raise ValueTypeError("array length must be a positive integer")
        if not isinstance(self.element, ValueType):
            raise TypeError("array element must be a ValueType")

    def canonical(self) -> dict[str, object]:
        return {
            "kind": "array",
            "version": 1,
            "length": self.length,
            "element": self.element.canonical(),
        }

    def mlir(self, *, scope: str = "types") -> str:
        return f"!ac.value_array<{self.length} x {self.element.mlir(scope=scope)}>"

    def bit_width(self) -> int:
        return self.length * self.element.bit_width()


ACType: TypeAlias = BoolType | BitsType | EnumType | StructType | TupleType | ArrayType
