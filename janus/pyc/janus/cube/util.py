from __future__ import annotations

from dataclasses import dataclass

from pycircuit import Circuit, Wire


@dataclass(frozen=True)
class Consts:
    one1: Wire
    zero1: Wire
    zero3: Wire
    zero8: Wire
    one8: Wire
    zero16: Wire
    zero32: Wire
    zero64: Wire


def make_consts(m: Circuit) -> Consts:
    c = m.const
    return Consts(
        one1=c(1, width=1),
        zero1=c(0, width=1),
        zero3=c(0, width=3),
        zero8=c(0, width=8),
        one8=c(1, width=8),
        zero16=c(0, width=16),
        zero32=c(0, width=32),
        zero64=c(0, width=64),
    )
