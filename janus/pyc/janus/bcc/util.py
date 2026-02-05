from __future__ import annotations

from dataclasses import dataclass

from pycircuit import Circuit, Wire


@dataclass(frozen=True)
class Consts:
    one1: Wire
    zero1: Wire
    zero3: Wire
    zero6: Wire
    zero8: Wire
    zero32: Wire
    zero64: Wire
    one64: Wire


def make_consts(m: Circuit) -> Consts:
    c = m.const_wire
    return Consts(
        one1=c(1, width=1),
        zero1=c(0, width=1),
        zero3=c(0, width=3),
        zero6=c(0, width=6),
        zero8=c(0, width=8),
        zero32=c(0, width=32),
        zero64=c(0, width=64),
        one64=c(1, width=64),
    )


def masked_eq(m: Circuit, x: Wire, *, width: int, mask: int, match: int) -> Wire:
    c = m.const_wire
    return (x & c(mask, width=width)).eq(c(match, width=width))

