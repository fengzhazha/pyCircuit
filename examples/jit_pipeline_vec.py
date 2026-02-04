from __future__ import annotations

from pycircuit import Circuit


def build(m: Circuit, STAGES: int = 3) -> None:
    dom = m.domain("sys")
    en = m.const_wire(1, width=1)

    a = m.in_wire("a", width=16)
    b = m.in_wire("b", width=16)
    sel = m.in_wire("sel", width=1)

    # Some combinational logic feeding a multi-field pipeline bus.
    sum_ = a + b
    x = a ^ b
    data = sel.select(sum_, x)
    tag = a.eq(b)
    lo8 = data[0:8]

    pkt = m.bundle(tag=tag, data=data, lo8=lo8)
    bus = pkt.pack()

    # Pipeline the packed bus through STAGES flops (stage-like style).
    for _ in range(STAGES):
        with m.scope("PIPE"):
            bus_r = m.out("bus", domain=dom, width=bus.width, init=0, en=1)
            bus_r.set(bus, when=en)
            bus = bus_r.out()

    out = pkt.unpack(bus)
    m.output("tag", out["tag"])
    m.output("data", out["data"])
    m.output("lo8", out["lo8"])
