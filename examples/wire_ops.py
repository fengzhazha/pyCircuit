from __future__ import annotations

from pycircuit import Circuit


def build(m: Circuit) -> None:
    dom = m.domain("sys")

    a = m.in_wire("a", width=8)
    b = m.in_wire("b", width=8)
    sel = m.in_wire("sel", width=1)

    with m.scope("COMB"):
        y = sel.select(a & b, a ^ b)

    en = m.const_wire(1, width=1)
    r = m.out("y_reg", domain=dom, width=8, init=0, en=1)
    with m.scope("REG"):
        r.set(y, when=en)

    m.output("y", r.out())
