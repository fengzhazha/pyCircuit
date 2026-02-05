from __future__ import annotations

from pycircuit import Circuit, Signal, Wire


def build_buffers(
    m: Circuit,
    *,
    clk: Signal,
    rst: Signal,
    # Memory interface
    mem_wvalid: Wire,
    mem_waddr: Wire,
    mem_wdata: Wire,
    mem_raddr: Wire,
    # Control
    load_weight: Wire,
    compute: Wire,
    cycle_count: Wire,
) -> tuple[list[Wire], list[Wire], list]:
    """
    Build input/output buffers for 16x16 array with 16-bit elements.

    Returns:
        (weights, activations_buf, results): Buffer outputs
    """
    with m.scope("BUFFERS"):
        # Weight buffer (256 × 16-bit)
        weights = []
        for i in range(256):
            with m.scope(f"w{i}"):
                addr_match = mem_waddr == (0x210 + i * 2)
                we = mem_wvalid & addr_match
                w_reg = m.out(f"weight", clk=clk, rst=rst, width=16, init=0, en=we)
                w_reg.set(mem_wdata.trunc(width=16), when=we)
                weights.append(w_reg.out())

        # Activation buffer (256 × 16-bit)
        activations_buf = []
        for i in range(256):
            with m.scope(f"a{i}"):
                addr_match = mem_waddr == (0x10 + i * 2)
                we = mem_wvalid & addr_match
                a_reg = m.out(f"activation", clk=clk, rst=rst, width=16, init=0, en=we)
                a_reg.set(mem_wdata.trunc(width=16), when=we)
                activations_buf.append(a_reg.out())

        # Select 16 activations based on cycle count - moved to caller
        # (This logic needs to be in JIT-compiled code)

        # Result buffer (256 × 32-bit) - will be written by array
        results = []
        for i in range(256):
            with m.scope(f"c{i}"):
                c_reg = m.out(f"result", clk=clk, rst=rst, width=32, init=0, en=compute)
                results.append(c_reg)

        return weights, activations_buf, results
