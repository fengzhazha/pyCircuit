from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import TypeVar

DT = TypeVar("DT", bound="Data", covariant=True)


@dataclass(frozen=True, eq=False)
class Data(ABC):
    """Structured signal type carried by ``Signal.ty``.

    ``str(Data)`` yields the canonical MLIR type literal so that f-string
    interpolation (``f"{sig.ty}"``) emits the same text as before.

    All concrete subclasses expose ``.width`` as the integer bit-width.
    """

    def __eq__(self, other: object) -> bool:
        if isinstance(other, str):
            return str(self) == other
        if isinstance(other, Data):
            return type(self) is type(other) and self.__dict__ == other.__dict__
        return NotImplemented

    def __hash__(self) -> int:
        return hash(str(self))

    @property
    @abstractmethod
    def width(self) -> int:
        """Integer bit-width (clock/reset: 1)."""

    @classmethod
    def from_str(cls, s: str) -> Data:
        raw = str(s).strip()
        if raw.startswith("i"):
            return Bits.from_str(raw)
        if raw == "!pyc.clock":
            return Clock()
        if raw == "!pyc.reset":
            return Reset()
        raise ValueError(f"unsupported type literal: {s!r}")

    @abstractmethod
    def __str__(self) -> str:  # pragma: no cover - overridden by subclasses
        raise NotImplementedError


@dataclass(frozen=True, eq=False)
class Bits(Data):
    bitwidth: int

    def __post_init__(self) -> None:
        if not isinstance(self.bitwidth, int) or self.bitwidth <= 0:
            raise ValueError(
                f"Bits.bitwidth must be a positive int, got {self.bitwidth!r}"
            )

    @property
    def width(self) -> int:
        return self.bitwidth

    def __str__(self) -> str:
        return f"i{self.bitwidth}"

    @classmethod
    def from_str(cls, s: str) -> Bits:
        raw = str(s).strip()
        if not raw.startswith("i"):
            raise ValueError(f"invalid bits type: {s!r}")
        tail = raw[1:]
        if not (tail and tail.isdigit()):
            raise ValueError(f"invalid bits type: {s!r}")
        w = int(tail)
        if w <= 0:
            raise ValueError(f"invalid bits type: {s!r}")
        return cls(w)


@dataclass(frozen=True, eq=False)
class Clock(Data):
    def __str__(self) -> str:
        return "!pyc.clock"

    @property
    def width(self) -> int:
        return 1


@dataclass(frozen=True, eq=False)
class Reset(Data):
    def __str__(self) -> str:
        return "!pyc.reset"

    @property
    def width(self) -> int:
        return 1
