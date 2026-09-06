"""Public annotation categories and frontend-only symbolic values."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from typing import TYPE_CHECKING, Generic, Never, TypeVar

if TYPE_CHECKING:
    from _pycircuit_semantics import BitfieldLayout

T = TypeVar("T")
P = TypeVar("P")
InterfaceT = TypeVar("InterfaceT")
R = TypeVar("R")


@dataclass(frozen=True, slots=True)
class ScalarType:
    width: int
    signed: bool = False

    def __post_init__(self) -> None:
        if type(self.width) is not int or not 1 <= self.width <= 64:
            raise ValueError("ACPY-TYPE-001: bit width must be in [1, 64]")
        if type(self.signed) is not bool:
            raise TypeError("ACPY-TYPE-001: signed must be bool")


class BitsFactory:
    """Static-width bit-vector annotation family used as ``ac.bits[N]``."""

    __slots__ = ()

    def __getitem__(self, width: int) -> ScalarType:
        return ScalarType(width)


bits = BitsFactory()


class ArrayFactory:
    """Fixed value-array annotation and AST-only static collection intrinsic."""

    __slots__ = ()

    def __getitem__(self, parameters: tuple[int, object]) -> object:
        from _pycircuit_semantics import ArrayType, BitsType, ValueType

        if not isinstance(parameters, tuple) or len(parameters) != 2:
            raise TypeError("ACPY-TYPE-006: array requires [length, element]")
        length, element = parameters
        descriptor = element if isinstance(element, ValueType) else None
        descriptor = getattr(element, "descriptor", descriptor)
        if descriptor is None and isinstance(element, ScalarType):
            descriptor = BitsType(element.width)
        if not isinstance(descriptor, ValueType):
            raise TypeError("ACPY-TYPE-006: array element must be an AC value type")
        return ArrayType(length, descriptor)

    def __call__(self, *values: object) -> Never:
        _ = values
        raise NotImplementedError(
            "array is an AST intrinsic inside Agentic definitions"
        )


array = ArrayFactory()


@dataclass(frozen=True, slots=True, init=False)
class BitfieldSpec:
    """A static named view over one ``bits[N]`` value.

    Fields use closed ``(msb, lsb)`` intervals. The object is immutable and
    data-only; rule bodies are inspected by the compiler and never execute its
    view/update methods at runtime.
    """

    _layout: BitfieldLayout

    def __init__(self, width: int, fields: Mapping[str, tuple[int, int]]) -> None:
        from _pycircuit_semantics import BitfieldLayout

        layout = BitfieldLayout(width, fields)
        if layout.width > 64:
            raise ValueError("ACPY-TYPE-001: bitfield width must be in [1, 64]")
        object.__setattr__(self, "_layout", layout)

    @property
    def width(self) -> int:
        return self._layout.width

    @property
    def fields(self) -> Mapping[str, tuple[int, int]]:
        return self._layout.fields

    @property
    def fingerprint(self) -> str:
        return self._layout.fingerprint

    def field(self, name: str) -> tuple[int, int]:
        return self._layout.field(name)

    def field_width(self, name: str) -> int:
        return self._layout.field_width(name)

    def field_slices(self) -> dict[str, tuple[int, int]]:
        return self._layout.field_slices()

    def __pyc_template_value__(self) -> dict[str, object]:
        return self._layout.metadata()

    def __call__(self, value: object) -> Never:
        _ = value
        raise NotImplementedError(
            "BitfieldSpec views are compiler intrinsics inside Agentic definitions"
        )

    def view(self, value: object) -> Never:
        return self(value)

    def update(self, value: object, **fields: object) -> Never:
        _ = (value, fields)
        raise NotImplementedError(
            "BitfieldSpec updates are compiler intrinsics inside Agentic definitions"
        )


UNSIGNED_WIDTHS = tuple(range(1, 65))
for _width in UNSIGNED_WIDTHS:
    globals()[f"u{_width}"] = ScalarType(_width)
del _width
s8 = ScalarType(8, True)
s16 = ScalarType(16, True)
s32 = ScalarType(32, True)
s64 = ScalarType(64, True)


class Static(Generic[T]):
    """Mark an elaboration-time specialization parameter."""


# The Queue frontend uses the lower-case spelling to make the
# specialization boundary read like a value category rather than a runtime
# container.  Keep ``Static`` available for the existing component frontend;
# both annotations have the same runtime origin.
const = Static


class Flow(Generic[T, P]):
    """Describe a typed logical dataflow edge using protocol ``P``."""


class Endpoint(Generic[InterfaceT, R]):
    """Describe interface ``I`` bound in role ``R``."""


@dataclass(frozen=True, slots=True, eq=False)
class SymbolicValue:
    """Frontend identity for an architecture value without a Python value."""

    stable_name: str
    annotation: object

    def __repr__(self) -> str:
        return f"SymbolicValue({self.stable_name!r})"

    def _reject(self, operation: str) -> Never:
        raise TypeError(
            f"ACPY-STATIC-002: {self.stable_name!r} cannot be used for {operation}"
        )

    def __bool__(self) -> Never:
        return self._reject("truth testing")

    def __int__(self) -> Never:
        return self._reject("integer conversion")

    def __hash__(self) -> Never:
        return self._reject("hashing")

    def __iter__(self) -> Never:
        return self._reject("iteration")

    def __eq__(self, other: object) -> Never:
        return self._reject("equality")

    def __ne__(self, other: object) -> Never:
        return self._reject("equality")


@dataclass(frozen=True, slots=True, eq=False, repr=False)
class ResourceRef(SymbolicValue, Generic[T, R]):
    """A typed resource capability bound in a declared role."""

    role: object

    @property
    def resource_type(self) -> object:
        return self.annotation

    def __repr__(self) -> str:
        return f"ResourceRef({self.stable_name!r})"


def _test_symbolic(stable_name: str, annotation: object) -> SymbolicValue:
    """Create a symbolic value for contract tests without elaboration state."""

    return SymbolicValue(stable_name=stable_name, annotation=annotation)
