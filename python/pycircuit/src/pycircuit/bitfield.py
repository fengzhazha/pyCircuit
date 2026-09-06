"""Single-signal bitfield views (ASL ``bits(N) { [31] N, [3:0] M }`` alignment).

This is pure front-end sugar (ASL alignment TODO T1): it expands to the existing
``slice``/``cat`` primitives and emits byte-identical MLIR to hand-written
``signal[lsb:msb+1]`` reads and ``cat(...)`` rebuilds. No MLIR dialect semantics
change.

``BitfieldSpec`` declares named bit ranges over a fixed-width vector. Unlike a
``RecordSpec`` (port-level) or ``Bundle`` (positional), fields address bits of a
*single live signal* and **may overlap** (multiple views of the same register,
matching ASL's overlapping bitfields).

Example::

    INSTR = BitfieldSpec(width=32, fields={
        "opcode": (31, 26),
        "rd":     (25, 21),
        "imm16":  (15, 0),
        "imm26":  (25, 0),   # overlaps rd/imm16 -- allowed (different view)
    })

    f  = INSTR.view(instr)          # f["opcode"] == instr[26:32]
    hi = f["opcode", "rd"]          # concatenated read (MSB-first), == cat(...)
    wr = INSTR.update(instr, rd=x)  # read-modify-write, == cat(instr[26:32], x, instr[0:21])

Both ``Wire`` and ``CycleAwareSignal`` are accepted; the result keeps the input's
type (and, for cycle-aware signals, its cycle tag).
"""

from __future__ import annotations

from collections.abc import Iterator, Mapping
from dataclasses import dataclass, field
from typing import Any

from _pycircuit_semantics import BitfieldLayout

from .hw import Module, Reg, Wire, cat


def _is_cas(value: object) -> bool:
    """Duck-typed check for cycle-aware signals (lazy import avoids cycles)."""
    from .v6 import CycleAwareSignal, ForwardSignal, StateSignal

    return isinstance(value, CycleAwareSignal | ForwardSignal | StateSignal)


def _unwrap_base(signal: object) -> object:
    """Unwrap Forward/State wrappers to the underlying cycle-aware signal."""
    from .v6 import ForwardSignal, StateSignal

    if isinstance(signal, ForwardSignal):
        return signal._state._cas
    if isinstance(signal, StateSignal):
        return signal._cas
    return signal


def _underlying_wire(signal: object) -> Wire:
    """Return the raw ``Wire`` behind any signal/wrapper (duck-typed)."""
    if isinstance(signal, Wire):
        return signal
    if isinstance(signal, Reg):
        return signal.q
    from .v6 import CycleAwareSignal, ForwardSignal, StateSignal

    if isinstance(signal, ForwardSignal):
        return signal._state._cas._w
    if isinstance(signal, StateSignal):
        return signal._cas._w
    if isinstance(signal, CycleAwareSignal):
        return signal._w
    raise TypeError(
        f"bitfield signal must be a Wire/Reg/CycleAwareSignal/ForwardSignal, got {type(signal).__name__}"
    )


def _signal_width(signal: object) -> int:
    return _underlying_wire(signal).width


def _module_of(signal: object) -> Module:
    return _underlying_wire(signal).m


def _wrap_like(base: object, wire: Wire) -> object:
    """Wrap ``wire`` into the same kind (and cycle) as ``base``."""
    if _is_cas(base):
        from .v6 import CycleAwareSignal

        return CycleAwareSignal(base._domain, wire, base._cycle)  # type: ignore[union-attr]
    return wire


def _to_wire(piece: object) -> Wire:
    if isinstance(piece, Wire):
        return piece
    if _is_cas(piece):
        return piece._w  # type: ignore[union-attr]
    raise TypeError(f"expected Wire/CycleAwareSignal piece, got {type(piece).__name__}")


def _cat_like(pieces: list[object], base: object) -> object:
    """Concatenate ``pieces`` (MSB-first) preserving ``base``'s type/cycle."""
    if not pieces:
        raise ValueError("cannot concatenate zero pieces")
    if _is_cas(base):
        wire = cat(*[_to_wire(p) for p in pieces])
        return _wrap_like(base, wire)
    return cat(*[_to_wire(p) for p in pieces])


class BitfieldView:
    """Read-only named-field view over a live signal.

    ``view["fld"]`` / ``view.fld`` read a single field; ``view["a", "b"]`` reads a
    concatenation of fields (MSB-first, in the given order).
    """

    __slots__ = ("_spec", "_signal")

    def __init__(self, spec: BitfieldSpec, signal: object) -> None:
        object.__setattr__(self, "_spec", spec)
        object.__setattr__(self, "_signal", signal)

    def _read_one(self, name: str) -> object:
        msb, lsb = self._spec._field(name)
        return self._signal[lsb : msb + 1]

    def __getitem__(self, key: str | tuple[str, ...]) -> object:
        if isinstance(key, tuple):
            if not key:
                raise KeyError("empty field selection")
            pieces = [self._read_one(str(k)) for k in key]
            return _cat_like(pieces, self._signal)
        return self._read_one(str(key))

    def __getattr__(self, name: str) -> object:
        if name.startswith("_"):
            raise AttributeError(name)
        spec = object.__getattribute__(self, "_spec")
        if name not in spec.fields:
            raise AttributeError(name)
        return self._read_one(name)

    def __setattr__(self, name: str, value: object) -> None:
        raise AttributeError(
            "BitfieldView is read-only; use BitfieldSpec.update(signal, **fields) to write"
        )

    def __contains__(self, name: object) -> bool:
        return str(name) in self._spec.fields

    def __iter__(self) -> Iterator[str]:
        return iter(self._spec.fields)

    def keys(self) -> tuple[str, ...]:
        return tuple(self._spec.fields)

    def items(self) -> Iterator[tuple[str, object]]:
        for name in self._spec.fields:
            yield name, self._read_one(name)

    def __repr__(self) -> str:
        return (
            f"BitfieldView(width={self._spec.width}, fields={list(self._spec.fields)})"
        )


@dataclass(frozen=True)
class BitfieldSpec:
    """Named bit ranges over a fixed-width vector; fields may overlap.

    Ranges are ``(msb, lsb)`` closed intervals (ASL/Verilog convention). Overlap
    between distinct fields is allowed (they are alternate views), but a single
    :meth:`update` call must not write overlapping fields.
    """

    width: int
    fields: Mapping[str, tuple[int, int]]
    _layout: BitfieldLayout = field(init=False, repr=False, compare=False)

    def __post_init__(self) -> None:
        layout = BitfieldLayout(self.width, self.fields)
        object.__setattr__(self, "width", layout.width)
        object.__setattr__(self, "fields", layout.fields)
        object.__setattr__(self, "_layout", layout)

    @property
    def fingerprint(self) -> str:
        return self._layout.fingerprint

    def _field(self, name: str) -> tuple[int, int]:
        return self._layout.field(name)

    def field_slices(self) -> dict[str, tuple[int, int]]:
        """Return ``name -> (lsb, width)`` (parity with ``spec.StructSpec``)."""
        return self._layout.field_slices()

    def field_width(self, name: str) -> int:
        return self._layout.field_width(name)

    def _check_signal(self, signal: object) -> object:
        signal = _unwrap_base(signal)
        got = _signal_width(signal)
        if got != self.width:
            raise ValueError(
                f"signal width {got} does not match BitfieldSpec width {self.width}"
            )
        return signal

    def view(self, signal: object) -> BitfieldView:
        """Return a read-only named-field view over ``signal``."""
        return BitfieldView(self, self._check_signal(signal))

    def __call__(self, signal: object) -> BitfieldView:
        """Shorthand for :meth:`view`, giving ASL-like ``SPEC(x).fld`` access."""
        return self.view(signal)

    def bind(self, signal: object) -> BitfieldSignal:
        """Attach this layout to ``signal`` so fields can be accessed directly.

        Unlike :meth:`view` (a read-only projection), the returned
        :class:`BitfieldSignal` *is* a drop-in for the signal: it forwards
        arithmetic / comparison / ``<<=`` / slicing to the underlying signal, and
        adds field access ``x["opcode"]`` / ``x.opcode`` / ``x["a", "b"]`` plus
        ``x.update(field=...)`` — no need to call the spec each time.
        """
        got = _underlying_wire(signal).width
        if got != self.width:
            raise ValueError(
                f"signal width {got} does not match BitfieldSpec width {self.width}"
            )
        return BitfieldSignal(self, signal)

    def _coerce_field(self, value: object, fw: int, base: object) -> object:
        """Coerce a write value to a field-width piece of ``base``'s kind."""
        if isinstance(value, Reg):
            value = value.q
        if isinstance(value, bool):
            value = int(value)
        if isinstance(value, int):
            lo = -(1 << (fw - 1))
            hi = (1 << fw) - 1
            if not (lo <= value <= hi):
                raise ValueError(f"constant {value} does not fit in {fw}-bit field")
            module = _module_of(base)
            masked = value & ((1 << fw) - 1)
            wire = Wire(module, Module.const(module, masked, width=fw))
            return _wrap_like(base, wire)
        if _is_cas(value):
            if not _is_cas(base):
                raise TypeError(
                    "cannot write a CycleAwareSignal field into a plain Wire signal"
                )
            if value._domain is not base._domain:  # type: ignore[union-attr]
                raise ValueError("field value must share the base signal's domain")
            if value._cycle != base._cycle:  # type: ignore[union-attr]
                raise ValueError(
                    f"field value cycle {value._cycle} != base cycle {base._cycle}; "  # type: ignore[union-attr]
                    "align before update"
                )
            vw = value._w.width  # type: ignore[union-attr]
            if vw != fw:
                raise ValueError(f"field value width {vw} != field width {fw}")
            return value
        if isinstance(value, Wire):
            if value.m is not _module_of(base):
                raise ValueError("field value must belong to the same module")
            if value.width != fw:
                raise ValueError(f"field value width {value.width} != field width {fw}")
            return _wrap_like(base, value) if _is_cas(base) else value
        raise TypeError(f"unsupported field value type: {type(value).__name__}")

    def update(self, signal: object, **fields: object) -> object:
        """Return ``signal`` with the named fields replaced (read-modify-write).

        Expands to ``cat`` of untouched slices and the new field values, so the
        result keeps ``signal``'s type (and cycle, for cycle-aware signals).
        """
        base = self._check_signal(signal)
        if not fields:
            return base
        writes = [
            (lsb, msb, name, fields[name])
            for lsb, msb, name in self._layout.checked_writes(fields)
        ]

        pieces: list[object] = []
        pos = self.width - 1
        for lsb, msb, _name, value in reversed(writes):
            if pos > msb:
                pieces.append(base[msb + 1 : pos + 1])
            pieces.append(self._coerce_field(value, msb - lsb + 1, base))
            pos = lsb - 1
        if pos >= 0:
            pieces.append(base[0 : pos + 1])
        return _cat_like(pieces, base)

    def __pyc_template_value__(self) -> dict[str, Any]:
        return {
            "kind": "bitfield",
            "width": self.width,
            "fingerprint": self.fingerprint,
            "fields": {n: [msb, lsb] for n, (msb, lsb) in self.fields.items()},
        }


def coerce_bitfield_spec(fields: object, *, width: int | None = None) -> BitfieldSpec:
    """Normalize a ``fields=`` argument to a :class:`BitfieldSpec`.

    Accepts either an existing ``BitfieldSpec`` (returned as-is) or a plain
    ``{name: (msb, lsb)}`` mapping, in which case ``width`` must be given so the
    spec can be constructed inline (``domain.signal(width=32, fields={...})``).
    """
    if isinstance(fields, BitfieldSpec):
        return fields
    if isinstance(fields, Mapping):
        if width is None:
            raise TypeError(
                "fields={...} (a plain mapping) requires width= to build a BitfieldSpec"
            )
        return BitfieldSpec(width=int(width), fields=fields)
    raise TypeError(
        f"fields= must be a BitfieldSpec or a {{name: (msb, lsb)}} mapping, got {type(fields).__name__}"
    )


class BitfieldSignal:
    """A signal with an attached :class:`BitfieldSpec` layout.

    Created by :meth:`BitfieldSpec.bind` (or ``m.input(..., fields=SPEC)`` /
    ``domain.signal(..., fields=SPEC)``). It forwards all arithmetic / comparison
    / ``<<=`` / bit-slicing to the wrapped signal, so it is a drop-in for it, and
    adds ASL-like field access::

        instr = m.input("instr", fields=INSTR)   # bound at declaration
        op    = instr["opcode"]                    # or instr.opcode
        pair  = instr["opcode", "rd"]              # concatenated read
        instr2 = instr.update(rd=new_rd)           # read-modify-write (stays bound)
        m.output("op", wire_of(op))

    String subscripts (``x["opcode"]``) and attribute access (``x.opcode``) read
    fields; integer / ``slice`` subscripts (``x[3]`` / ``x[0:8]``) still do raw
    bit-slicing. Field names that collide with wrapper members (e.g. ``update``,
    ``raw``, ``width``) are reachable via the string subscript form.
    """

    __slots__ = ("_spec", "_signal")

    def __init__(self, spec: BitfieldSpec, signal: object) -> None:
        object.__setattr__(self, "_spec", spec)
        object.__setattr__(self, "_signal", signal)

    # ── bitfield access ───────────────────────────────────────────────

    @property
    def raw(self) -> object:
        """The underlying (unbound) signal."""
        return self._signal

    @property
    def spec(self) -> BitfieldSpec:
        return self._spec

    @property
    def width(self) -> int:
        return self._spec.width

    def __pyc_unwrap__(self) -> object:
        """Hook used by ``wire_of`` / ``m.output`` to reach the raw signal."""
        return self._signal

    def _read_field(self, name: str) -> object:
        msb, lsb = self._spec._field(name)
        return self._signal[lsb : msb + 1]

    def update(self, **fields: object) -> BitfieldSignal:
        """Read-modify-write named fields; result stays bound to this layout."""
        return BitfieldSignal(self._spec, self._spec.update(self._signal, **fields))

    def view(self) -> BitfieldView:
        """Return a read-only :class:`BitfieldView` over the underlying signal."""
        return self._spec.view(self._signal)

    def __getitem__(self, key: object) -> object:
        if isinstance(key, str):
            return self._read_field(key)
        if isinstance(key, tuple) and all(isinstance(k, str) for k in key):
            if not key:
                raise KeyError("empty field selection")
            pieces = [self._read_field(str(k)) for k in key]
            return _cat_like(pieces, _unwrap_base(self._signal))
        return self._signal[key]

    def __getattr__(self, name: str) -> object:
        if name.startswith("_"):
            raise AttributeError(name)
        spec = object.__getattribute__(self, "_spec")
        if name in spec.fields:
            return self._read_field(name)
        return getattr(object.__getattribute__(self, "_signal"), name)

    def __setattr__(self, name: str, value: object) -> None:
        raise AttributeError(
            "BitfieldSignal is immutable; use x.update(field=...) to build a new value"
        )

    def __ilshift__(self, other: object) -> BitfieldSignal:
        # ``x <<= expr`` closes a ForwardSignal feedback loop; stay bound.
        self._signal.__ilshift__(other)  # type: ignore[attr-defined]
        return self

    def assign(self, next_val: object, *, when: object = None) -> None:
        self._signal.assign(next_val, when=when)  # type: ignore[attr-defined]

    # ── operator delegation (mirror ForwardSignal) ────────────────────

    def __add__(self, other: object) -> object:
        return self._signal + other

    def __radd__(self, other: object) -> object:
        return other + self._signal

    def __sub__(self, other: object) -> object:
        return self._signal - other

    def __mul__(self, other: object) -> object:
        return self._signal * other

    def __and__(self, other: object) -> object:
        return self._signal & other

    def __or__(self, other: object) -> object:
        if isinstance(other, str):
            return self._signal
        return self._signal | other

    def __xor__(self, other: object) -> object:
        return self._signal ^ other

    def __invert__(self) -> object:
        return ~self._signal

    def __eq__(self, other: object) -> object:  # type: ignore[override]
        return self._signal == other

    def __ne__(self, other: object) -> object:  # type: ignore[override]
        return self._signal != other

    def __lt__(self, other: object) -> object:
        return self._signal < other

    def __gt__(self, other: object) -> object:
        return self._signal > other

    def __le__(self, other: object) -> object:
        return self._signal <= other

    def __ge__(self, other: object) -> object:
        return self._signal >= other

    def __hash__(self) -> int:
        return id(self)

    def __repr__(self) -> str:
        return f"BitfieldSignal({self._signal!r}, fields={list(self._spec.fields)})"
