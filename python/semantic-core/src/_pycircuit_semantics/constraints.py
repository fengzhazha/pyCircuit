"""Small deterministic abstract domain for bounded frontend proofs."""

from __future__ import annotations

import hashlib
import json
import operator
from dataclasses import dataclass
from typing import TYPE_CHECKING, TypeAlias

if TYPE_CHECKING:
    from .types import ValueType


ConstraintValue: TypeAlias = bool | int | str
DEFAULT_FINITE_SET_LIMIT = 64
MAX_STATIC_TRANSFER_SHIFT = 4096


class ConstraintError(ValueError):
    """A deterministic rejection of an invalid abstract value."""


def _atom_key(value: ConstraintValue) -> tuple[str, str]:
    if type(value) not in {bool, int, str}:
        raise ConstraintError("constraint values must be bool, int, or str")
    return type(value).__name__, repr(value)


def _unique_atoms(values: tuple[ConstraintValue, ...]) -> tuple[ConstraintValue, ...]:
    unique: dict[tuple[str, str], ConstraintValue] = {}
    for value in values:
        unique[_atom_key(value)] = value
    return tuple(unique[key] for key in sorted(unique))


class _ConstraintBase:
    __slots__ = ()

    def canonical(self) -> dict[str, object]:
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


@dataclass(frozen=True, slots=True, eq=False)
class Constant(_ConstraintBase):
    value: ConstraintValue

    def __post_init__(self) -> None:
        _atom_key(self.value)

    def __eq__(self, other: object) -> bool:
        return isinstance(other, Constant) and _atom_key(self.value) == _atom_key(
            other.value
        )

    def __hash__(self) -> int:
        return hash((Constant, _atom_key(self.value)))

    def canonical(self) -> dict[str, object]:
        return {
            "kind": "constant",
            "version": 1,
            "value_type": type(self.value).__name__,
            "value": self.value,
        }


@dataclass(frozen=True, slots=True, eq=False)
class FiniteSet(_ConstraintBase):
    values: tuple[ConstraintValue, ...]

    def __post_init__(self) -> None:
        values = _unique_atoms(tuple(self.values))
        if len(values) > DEFAULT_FINITE_SET_LIMIT:
            raise ConstraintError("finite set exceeds the cardinality limit")
        object.__setattr__(self, "values", values)

    def __eq__(self, other: object) -> bool:
        return isinstance(other, FiniteSet) and tuple(
            _atom_key(value) for value in self.values
        ) == tuple(_atom_key(value) for value in other.values)

    def __hash__(self) -> int:
        return hash((FiniteSet, tuple(_atom_key(value) for value in self.values)))

    def canonical(self) -> dict[str, object]:
        return {
            "kind": "finite_set",
            "version": 1,
            "values": [
                {"type": type(value).__name__, "value": value} for value in self.values
            ],
        }


@dataclass(frozen=True, slots=True)
class ClosedInterval(_ConstraintBase):
    lower: int
    upper: int

    def __post_init__(self) -> None:
        if type(self.lower) is not int or type(self.upper) is not int:
            raise ConstraintError("closed interval bounds must be integers")
        if self.lower > self.upper:
            raise ConstraintError("closed interval lower bound exceeds upper bound")

    def canonical(self) -> dict[str, object]:
        return {
            "kind": "closed_interval",
            "version": 1,
            "lower": self.lower,
            "upper": self.upper,
        }


@dataclass(frozen=True, slots=True)
class Unknown(_ConstraintBase):
    def canonical(self) -> dict[str, object]:
        return {"kind": "unknown", "version": 1}


Constraint: TypeAlias = Constant | FiniteSet | ClosedInterval | Unknown


@dataclass(frozen=True, slots=True)
class ValueConstraint(_ConstraintBase):
    type: ValueType
    domain: Constraint

    def __post_init__(self) -> None:
        from .types import (
            ArrayType,
            BitsType,
            BoolType,
            EnumType,
            StructType,
            TupleType,
            ValueType,
        )

        if not isinstance(self.type, ValueType):
            raise TypeError("value constraint type must be a ValueType")
        if not isinstance(self.domain, Constant | FiniteSet | ClosedInterval | Unknown):
            raise TypeError("value constraint domain is invalid")
        values = finite_values(self.domain)
        if isinstance(self.type, BitsType):
            maximum = (1 << self.type.width) - 1
            if values is not None and any(
                type(value) is not int or not 0 <= value <= maximum for value in values
            ):
                raise ConstraintError("bits constraint contains an invalid value")
            bounds = integer_bounds(self.domain)
            if bounds is not None and not (0 <= bounds[0] <= bounds[1] <= maximum):
                raise ConstraintError("bits constraint interval exceeds its type")
        elif isinstance(self.type, BoolType):
            if values is not None and any(type(value) is not bool for value in values):
                raise ConstraintError("bool constraint contains a non-bool value")
        elif isinstance(self.type, EnumType):
            if values is not None and any(
                type(value) is not str or value not in self.type.enumerants
                for value in values
            ):
                raise ConstraintError("enum constraint contains an unknown member")
        elif isinstance(
            self.type, StructType | TupleType | ArrayType
        ) and not isinstance(self.domain, Unknown):
            raise ConstraintError("aggregate values require an unknown scalar domain")

    def canonical(self) -> dict[str, object]:
        return {
            "kind": "value_constraint",
            "version": 1,
            "type": self.type.canonical(),
            "domain": self.domain.canonical(),
        }


def finite(
    values: tuple[ConstraintValue, ...] | list[ConstraintValue],
    *,
    limit: int = DEFAULT_FINITE_SET_LIMIT,
) -> Constraint:
    normalized = _unique_atoms(tuple(values))
    if len(normalized) > min(limit, DEFAULT_FINITE_SET_LIMIT):
        return Unknown()
    if len(normalized) == 1:
        return Constant(normalized[0])
    return FiniteSet(normalized)


def finite_values(
    constraint: Constraint, *, limit: int = DEFAULT_FINITE_SET_LIMIT
) -> tuple[ConstraintValue, ...] | None:
    if isinstance(constraint, Constant):
        return (constraint.value,)
    if isinstance(constraint, FiniteSet):
        return constraint.values if len(constraint.values) <= limit else None
    if isinstance(constraint, ClosedInterval):
        size = constraint.upper - constraint.lower + 1
        if size <= limit:
            return tuple(range(constraint.lower, constraint.upper + 1))
    return None


def integer_bounds(constraint: Constraint) -> tuple[int, int] | None:
    if isinstance(constraint, Constant) and type(constraint.value) is int:
        return constraint.value, constraint.value
    if (
        isinstance(constraint, FiniteSet)
        and constraint.values
        and all(type(value) is int for value in constraint.values)
    ):
        values = tuple(value for value in constraint.values if type(value) is int)
        return min(values), max(values)
    if isinstance(constraint, ClosedInterval):
        return constraint.lower, constraint.upper
    return None


def join(
    left: Constraint,
    right: Constraint,
    *,
    finite_limit: int = DEFAULT_FINITE_SET_LIMIT,
) -> Constraint:
    if left == right:
        return left
    if isinstance(left, Unknown) or isinstance(right, Unknown):
        return Unknown()
    left_bounds = integer_bounds(left)
    right_bounds = integer_bounds(right)
    if isinstance(left, ClosedInterval) or isinstance(right, ClosedInterval):
        if left_bounds is None or right_bounds is None:
            return Unknown()
        return ClosedInterval(
            min(left_bounds[0], right_bounds[0]),
            max(left_bounds[1], right_bounds[1]),
        )
    left_values = finite_values(left, limit=finite_limit)
    right_values = finite_values(right, limit=finite_limit)
    if left_values is not None and right_values is not None:
        combined = finite([*left_values, *right_values], limit=finite_limit)
        if not isinstance(combined, Unknown):
            return combined
    if left_bounds is not None and right_bounds is not None:
        return ClosedInterval(
            min(left_bounds[0], right_bounds[0]),
            max(left_bounds[1], right_bounds[1]),
        )
    return Unknown()


def meet(
    left: Constraint,
    right: Constraint,
    *,
    finite_limit: int = DEFAULT_FINITE_SET_LIMIT,
) -> Constraint:
    if left == right:
        return left
    if isinstance(left, Unknown):
        return right
    if isinstance(right, Unknown):
        return left
    left_values = finite_values(left, limit=finite_limit)
    right_values = finite_values(right, limit=finite_limit)
    if left_values is not None and right_values is not None:
        right_keys = {_atom_key(value) for value in right_values}
        return finite(
            [value for value in left_values if _atom_key(value) in right_keys]
        )
    left_bounds = integer_bounds(left)
    right_bounds = integer_bounds(right)
    if left_bounds is not None and right_bounds is not None:
        lower = max(left_bounds[0], right_bounds[0])
        upper = min(left_bounds[1], right_bounds[1])
        return FiniteSet(()) if lower > upper else ClosedInterval(lower, upper)
    if left_values is not None and right_bounds is not None:
        return finite(
            [
                value
                for value in left_values
                if type(value) is int and right_bounds[0] <= value <= right_bounds[1]
            ]
        )
    if right_values is not None and left_bounds is not None:
        return meet(right, left, finite_limit=finite_limit)
    return FiniteSet(())


def prove_within(constraint: Constraint, lower: int, upper: int) -> bool:
    if type(lower) is not int or type(upper) is not int or lower > upper:
        raise ConstraintError("proof bounds must form a closed integer interval")
    bounds = integer_bounds(constraint)
    return bounds is not None and lower <= bounds[0] and bounds[1] <= upper


def is_exhaustive(
    constraint: Constraint, covered: tuple[ConstraintValue, ...] | set[ConstraintValue]
) -> bool:
    values = finite_values(constraint)
    if values is None:
        return False
    covered_keys = {_atom_key(value) for value in covered}
    return all(_atom_key(value) in covered_keys for value in values)


_BINARY_OPERATORS = {
    "add": operator.add,
    "sub": operator.sub,
    "mul": operator.mul,
    "floordiv": operator.floordiv,
    "mod": operator.mod,
    "and": operator.and_,
    "or": operator.or_,
    "xor": operator.xor,
    "shl": operator.lshift,
    "shr": operator.rshift,
}


def transfer_static_binary(
    operation: str,
    left: Constraint,
    right: Constraint,
    *,
    finite_limit: int = DEFAULT_FINITE_SET_LIMIT,
) -> Constraint:
    function = _BINARY_OPERATORS.get(operation)
    if function is None:
        raise ConstraintError(f"unknown constraint operation {operation!r}")
    right_bounds = integer_bounds(right)
    if operation in {"shl", "shr"} and (
        right_bounds is None
        or right_bounds[0] < 0
        or right_bounds[1] > MAX_STATIC_TRANSFER_SHIFT
    ):
        return Unknown()
    left_values = finite_values(left, limit=finite_limit)
    right_values = finite_values(right, limit=finite_limit)
    if (
        left_values is not None
        and right_values is not None
        and len(left_values) * len(right_values) <= finite_limit
    ):
        results: list[ConstraintValue] = []
        try:
            for lhs in left_values:
                for rhs in right_values:
                    if type(lhs) is not int or type(rhs) is not int:
                        return Unknown()
                    results.append(function(lhs, rhs))
        except (ArithmeticError, MemoryError, ValueError):
            return Unknown()
        return finite(results, limit=finite_limit)

    left_bounds = integer_bounds(left)
    if left_bounds is None or right_bounds is None:
        return Unknown()
    ll, lu = left_bounds
    rl, ru = right_bounds
    if operation == "add":
        return ClosedInterval(ll + rl, lu + ru)
    if operation == "sub":
        return ClosedInterval(ll - ru, lu - rl)
    if operation == "mul":
        products = (ll * rl, ll * ru, lu * rl, lu * ru)
        return ClosedInterval(min(products), max(products))
    if operation == "mod" and rl == ru and rl > 0:
        return ClosedInterval(0, rl - 1)
    if operation == "shr" and ll >= 0 and rl >= 0:
        return ClosedInterval(ll >> ru, lu >> rl)
    if operation == "shl" and ll >= 0 and rl >= 0:
        return ClosedInterval(ll << rl, lu << ru)
    return Unknown()


def transfer_bits(
    operation: str,
    left: Constraint,
    right: Constraint,
    *,
    width: int,
    finite_limit: int = DEFAULT_FINITE_SET_LIMIT,
) -> Constraint:
    if type(width) is not int or not 1 <= width <= 64:
        raise ConstraintError("bitvector transfer width must be in [1, 64]")
    if operation not in _BINARY_OPERATORS:
        raise ConstraintError(f"unknown constraint operation {operation!r}")
    mask = (1 << width) - 1
    left_values = finite_values(left, limit=finite_limit)
    right_values = finite_values(right, limit=finite_limit)
    if (
        left_values is not None
        and right_values is not None
        and len(left_values) * len(right_values) <= finite_limit
    ):
        results: list[int] = []
        for lhs in left_values:
            for rhs in right_values:
                if type(lhs) is not int or type(rhs) is not int or rhs < 0:
                    return Unknown()
                lhs &= mask
                if operation == "shl":
                    result = 0 if rhs >= width else (lhs << rhs) & mask
                elif operation == "shr":
                    result = 0 if rhs >= width else lhs >> rhs
                elif operation in {"floordiv", "mod"} and rhs == 0:
                    return Unknown()
                else:
                    result = _BINARY_OPERATORS[operation](lhs, rhs) & mask
                results.append(result)
        return finite(results, limit=finite_limit)

    left_bounds = integer_bounds(left)
    right_bounds = integer_bounds(right)
    full = ClosedInterval(0, mask)
    if left_bounds is None or right_bounds is None:
        return full
    ll, lu = left_bounds
    rl, ru = right_bounds
    if ll < 0 or rl < 0:
        return full
    if operation in {"shl", "shr"} and rl >= width:
        return Constant(0)
    if operation == "add" and lu + ru <= mask:
        return ClosedInterval(ll + rl, lu + ru)
    if operation == "sub" and ll >= ru:
        return ClosedInterval(ll - ru, lu - rl)
    if operation == "mul" and lu * ru <= mask:
        products = (ll * rl, ll * ru, lu * rl, lu * ru)
        return ClosedInterval(min(products), max(products))
    if operation == "and":
        if isinstance(left, Constant) and type(left.value) is int:
            return ClosedInterval(0, min(mask, left.value & mask, ru))
        if isinstance(right, Constant) and type(right.value) is int:
            return ClosedInterval(0, min(mask, right.value & mask, lu))
    if operation == "shr" and ru < width:
        return ClosedInterval(0, min(mask, lu >> rl))
    return full


def transfer_compare(operation: str, left: Constraint, right: Constraint) -> Constraint:
    comparisons = {
        "eq": operator.eq,
        "ne": operator.ne,
        "lt": operator.lt,
        "le": operator.le,
        "gt": operator.gt,
        "ge": operator.ge,
    }
    function = comparisons.get(operation)
    if function is None:
        raise ConstraintError(f"unknown comparison operation {operation!r}")
    left_values = finite_values(left)
    right_values = finite_values(right)
    if left_values is not None and right_values is not None:
        try:
            results = {
                function(lhs, rhs) for lhs in left_values for rhs in right_values
            }
        except TypeError:
            return Unknown()
        return finite(list(results))
    left_bounds = integer_bounds(left)
    right_bounds = integer_bounds(right)
    if left_bounds is not None and right_bounds is not None:
        ll, lu = left_bounds
        rl, ru = right_bounds
        if operation == "eq" and (lu < rl or ru < ll):
            return Constant(False)
        if operation == "ne" and (lu < rl or ru < ll):
            return Constant(True)
        if operation == "lt" and lu < rl:
            return Constant(True)
        if operation == "lt" and ll >= ru:
            return Constant(False)
        if operation == "le" and lu <= rl:
            return Constant(True)
        if operation == "le" and ll > ru:
            return Constant(False)
        if operation == "gt" and ll > ru:
            return Constant(True)
        if operation == "gt" and lu <= rl:
            return Constant(False)
        if operation == "ge" and ll >= ru:
            return Constant(True)
        if operation == "ge" and lu < rl:
            return Constant(False)
    return FiniteSet((False, True))


def constraint_for_type(value_type: ValueType) -> Constraint:
    from .types import BitsType, BoolType, EnumType

    if isinstance(value_type, BoolType):
        return FiniteSet((False, True))
    if isinstance(value_type, BitsType):
        return ClosedInterval(0, (1 << value_type.width) - 1)
    if isinstance(value_type, EnumType):
        return finite(list(value_type.enumerants))
    return Unknown()
