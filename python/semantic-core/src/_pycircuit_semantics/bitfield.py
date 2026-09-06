"""Immutable, backend-neutral named bitfield layout semantics."""

from __future__ import annotations

import hashlib
import json
from collections.abc import Iterable, Mapping
from dataclasses import dataclass, field
from types import MappingProxyType


class BitfieldLayoutError(ValueError):
    """A deterministic rejection of an invalid bitfield schema or update."""


def _name_sort_key(value: str) -> bytes:
    return value.encode("utf-8", errors="surrogatepass")


@dataclass(frozen=True, slots=True, init=False)
class BitfieldLayout:
    """A fixed-width collection of named, potentially overlapping bit views.

    Field ranges use closed ``(msb, lsb)`` intervals. Declaration overlap is
    legal because fields are views; one update may not select overlapping
    fields. Schema identity is independent of mapping insertion order.
    """

    width: int
    fields: Mapping[str, tuple[int, int]]
    fingerprint: str
    _slices: Mapping[str, tuple[int, int]] = field(repr=False)

    def __init__(self, width: object, fields: Mapping[object, object]) -> None:
        if type(width) is not int:
            raise BitfieldLayoutError("BitfieldSpec width must be an integer")
        normalized_width = width
        if normalized_width <= 0:
            raise BitfieldLayoutError("BitfieldSpec width must be > 0")
        if not isinstance(fields, Mapping) or not fields:
            raise BitfieldLayoutError("BitfieldSpec requires at least one field")

        normalized: dict[str, tuple[int, int]] = {}
        for raw_name, raw_range in fields.items():
            if type(raw_name) is not str:
                raise BitfieldLayoutError("bitfield field name must be a string")
            name = raw_name.strip()
            if not name:
                raise BitfieldLayoutError("bitfield field name must be non-empty")
            if name in normalized:
                raise BitfieldLayoutError(f"duplicate bitfield field {name!r}")
            if not isinstance(raw_range, tuple | list) or len(raw_range) != 2:
                raise BitfieldLayoutError(
                    f"bitfield field {name!r} range must be a (msb, lsb) pair, "
                    f"got {raw_range!r}"
                )
            if type(raw_range[0]) is not int or type(raw_range[1]) is not int:
                raise BitfieldLayoutError(
                    f"bitfield field {name!r} range must be a (msb, lsb) pair, "
                    f"got {raw_range!r}"
                )
            msb = raw_range[0]
            lsb = raw_range[1]
            if lsb < 0:
                raise BitfieldLayoutError(f"bitfield field {name!r} lsb must be >= 0")
            if msb < lsb:
                raise BitfieldLayoutError(
                    f"bitfield field {name!r} requires msb >= lsb, got ({msb}, {lsb})"
                )
            if msb >= normalized_width:
                raise BitfieldLayoutError(
                    f"bitfield field {name!r} msb {msb} out of range for width "
                    f"{normalized_width}"
                )
            normalized[name] = (msb, lsb)

        ordered = dict(
            sorted(normalized.items(), key=lambda item: _name_sort_key(item[0]))
        )
        slices = {name: (lsb, msb - lsb + 1) for name, (msb, lsb) in ordered.items()}
        fingerprint_preimage = {
            "kind": "bitfield",
            "version": 1,
            "width": normalized_width,
            "fields": [[name, msb, lsb] for name, (msb, lsb) in ordered.items()],
        }
        encoded = json.dumps(
            fingerprint_preimage,
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8")

        object.__setattr__(self, "width", normalized_width)
        object.__setattr__(self, "fields", MappingProxyType(ordered))
        object.__setattr__(self, "_slices", MappingProxyType(slices))
        object.__setattr__(
            self, "fingerprint", "sha256:" + hashlib.sha256(encoded).hexdigest()
        )

    def field(self, name: str) -> tuple[int, int]:
        try:
            return self.fields[name]
        except KeyError:
            raise KeyError(
                f"unknown bitfield {name!r}; known fields: {sorted(self.fields)}"
            ) from None

    def __hash__(self) -> int:
        return hash(self.fingerprint)

    def field_width(self, name: str) -> int:
        self.field(name)
        return self._slices[name][1]

    def field_slices(self) -> dict[str, tuple[int, int]]:
        """Return a detached ``name -> (lsb, width)`` metadata mapping."""
        return dict(self._slices)

    def checked_writes(self, names: Iterable[str]) -> tuple[tuple[int, int, str], ...]:
        """Return writes ordered by lsb after rejecting overlap and duplicates."""
        writes: list[tuple[int, int, str]] = []
        seen: set[str] = set()
        for name in names:
            if name in seen:
                raise BitfieldLayoutError(f"bitfield update repeats field {name!r}")
            seen.add(name)
            msb, lsb = self.field(name)
            writes.append((lsb, msb, name))
        writes.sort(key=lambda write: write[0])
        for previous, current in zip(writes, writes[1:], strict=False):
            if current[0] <= previous[1]:
                raise BitfieldLayoutError(
                    f"update writes overlap: {previous[2]!r} and {current[2]!r}"
                )
        return tuple(writes)

    def metadata(self) -> dict[str, object]:
        """Return stable JSON-compatible field slice and identity metadata."""
        return {
            "kind": "bitfield",
            "version": 1,
            "width": self.width,
            "fingerprint": self.fingerprint,
            "fields": {
                name: {"msb": msb, "lsb": lsb, "width": msb - lsb + 1}
                for name, (msb, lsb) in self.fields.items()
            },
        }
