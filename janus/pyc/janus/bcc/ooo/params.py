from __future__ import annotations


class OooParams:
    """Sizing parameters for the OoO bring-up core.

    Keep these values small for fast compile/sim; scale up once the pipeline
    structure stabilizes.
    """

    # Physical registers (PRF entries).
    pregs: int = 64

    # Architectural regs (LinxISA reg5 namespace):
    # - gpr: 0..23 (r0 hardwired to 0)
    # - t:   24..27 (t#1..t#4)
    # - u:   28..31 (u#1..u#4)
    aregs: int = 32

    # Reorder buffer and issue queue depths.
    rob_depth: int = 16
    iq_depth: int = 16

    # Pipeline widths (bring-up: keep these values small).
    fetch_w: int = 4
    dispatch_w: int = 4
    issue_w: int = 4
    commit_w: int = 4

    # Bring-up FU split (issue width must equal the sum).
    alu_w: int = 2
    bru_w: int = 1
    lsu_w: int = 1

    def __init__(
        self,
        *,
        pregs: int | None = None,
        rob_depth: int | None = None,
        iq_depth: int | None = None,
        fetch_w: int | None = None,
        dispatch_w: int | None = None,
        issue_w: int | None = None,
        commit_w: int | None = None,
        alu_w: int | None = None,
        bru_w: int | None = None,
        lsu_w: int | None = None,
    ) -> None:
        if pregs is not None:
            self.pregs = int(pregs)
        if rob_depth is not None:
            self.rob_depth = int(rob_depth)
        if iq_depth is not None:
            self.iq_depth = int(iq_depth)
        if fetch_w is not None:
            self.fetch_w = int(fetch_w)
        if dispatch_w is not None:
            self.dispatch_w = int(dispatch_w)
        if issue_w is not None:
            self.issue_w = int(issue_w)
        if commit_w is not None:
            self.commit_w = int(commit_w)
        if alu_w is not None:
            self.alu_w = int(alu_w)
        if bru_w is not None:
            self.bru_w = int(bru_w)
        if lsu_w is not None:
            self.lsu_w = int(lsu_w)

        if self.pregs <= 0:
            raise ValueError("pregs must be > 0")
        if self.aregs <= 0:
            raise ValueError("aregs must be > 0")
        if self.rob_depth <= 0:
            raise ValueError("rob_depth must be > 0")
        if self.iq_depth <= 0:
            raise ValueError("iq_depth must be > 0")
        if self.fetch_w <= 0:
            raise ValueError("fetch_w must be > 0")
        if self.dispatch_w <= 0:
            raise ValueError("dispatch_w must be > 0")
        if self.issue_w <= 0:
            raise ValueError("issue_w must be > 0")
        if self.commit_w <= 0:
            raise ValueError("commit_w must be > 0")
        if self.alu_w <= 0:
            raise ValueError("alu_w must be > 0")
        if self.bru_w <= 0:
            raise ValueError("bru_w must be > 0")
        if self.lsu_w <= 0:
            raise ValueError("lsu_w must be > 0")

        if self.rob_depth & (self.rob_depth - 1):
            raise ValueError("rob_depth must be a power of two (wrap-friendly)")
        if self.iq_depth & (self.iq_depth - 1):
            raise ValueError("iq_depth must be a power of two (wrap-friendly)")

        # Bring-up backend constraint (C++ sim supports <=64-bit wires).
        if self.pregs > 64:
            raise ValueError("pregs must be <= 64 for the C++ backend (Bits<Width> constraint)")

        if self.fetch_w > 4:
            raise ValueError("fetch_w > 4 not supported (64-bit fetch window)")
        if self.dispatch_w > 4:
            raise ValueError("dispatch_w > 4 not supported (64-bit fetch window)")
        if self.fetch_w != self.dispatch_w:
            raise ValueError("fetch_w must equal dispatch_w (single-entry F4 bundle queue)")
        if self.issue_w > 4:
            raise ValueError("issue_w > 4 not supported (single-cycle exec bring-up)")
        if self.commit_w > 4:
            raise ValueError("commit_w > 4 not supported (bring-up)")
        if self.issue_w != (self.alu_w + self.bru_w + self.lsu_w):
            raise ValueError("issue_w must equal alu_w + bru_w + lsu_w")
        if self.lsu_w > 1:
            raise ValueError("lsu_w > 1 not supported (single mem pipeline bring-up)")

    @property
    def ptag_w(self) -> int:
        return (self.pregs - 1).bit_length()

    @property
    def rob_w(self) -> int:
        return (self.rob_depth - 1).bit_length()

    @property
    def iq_w(self) -> int:
        return (self.iq_depth - 1).bit_length()
