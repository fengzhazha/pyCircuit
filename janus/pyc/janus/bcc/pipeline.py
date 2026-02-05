from __future__ import annotations

from dataclasses import dataclass

from pycircuit import Reg


@dataclass(frozen=True)
class CoreState:
    stage: Reg
    pc: Reg
    br_kind: Reg
    br_base_pc: Reg
    br_off: Reg
    commit_cond: Reg
    commit_tgt: Reg
    cycles: Reg
    halted: Reg


@dataclass(frozen=True)
class IfIdRegs:
    window: Reg


@dataclass(frozen=True)
class IdExRegs:
    op: Reg
    len_bytes: Reg
    regdst: Reg
    srcl: Reg
    srcr: Reg
    srcp: Reg
    imm: Reg
    srcl_val: Reg
    srcr_val: Reg
    srcp_val: Reg


@dataclass(frozen=True)
class ExMemRegs:
    op: Reg
    len_bytes: Reg
    regdst: Reg
    alu: Reg
    is_load: Reg
    is_store: Reg
    size: Reg
    addr: Reg
    wdata: Reg


@dataclass(frozen=True)
class MemWbRegs:
    op: Reg
    len_bytes: Reg
    regdst: Reg
    value: Reg


@dataclass(frozen=True)
class RegFiles:
    gpr: list[Reg]
    t: list[Reg]
    u: list[Reg]

