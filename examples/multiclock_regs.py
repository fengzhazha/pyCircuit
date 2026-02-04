from __future__ import annotations

from pycircuit import Circuit


def build(m: Circuit) -> None:
    clk_a = m.clock("clk_a")
    rst_a = m.reset("rst_a")
    clk_b = m.clock("clk_b")
    rst_b = m.reset("rst_b")

    en = m.const_wire(1, width=1)

    a = m.out("a", clk=clk_a, rst=rst_a, width=8, init=0, en=en)
    with m.scope("A"):
        a.set(a.out() + 1)

    b = m.out("b", clk=clk_b, rst=rst_b, width=8, init=0, en=en)
    with m.scope("B"):
        b.set(b.out() + 1)

    m.output("a_count", a.out())
    m.output("b_count", b.out())
