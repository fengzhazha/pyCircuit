from __future__ import annotations

import json
import re
from collections.abc import Callable
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, Generic, TypeGuard

from .connectors import Connector
from .data import DT, Bits, Clock, Data, Reset

if TYPE_CHECKING:
    from .hw import Module, Reg, Wire

_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


@dataclass(frozen=True)
class Signal(Generic[DT]):
    ref: str
    ty: DT

    def __post_init__(self) -> None:
        if not isinstance(self.ty, (Bits, Clock, Reset)):
            raise TypeError(
                f"Signal.ty must be Bits/Clock/Reset, got {type(self.ty).__name__}"
            )

    def __str__(self) -> str:
        return self.ref

    @property
    def width(self) -> int:
        return self.ty.width

    @classmethod
    def as_sig(cls, v: Connector | Wire | Reg | Signal) -> Signal:
        from .hw import Reg, Wire

        if isinstance(v, Connector):
            v = v.read().sig
        if isinstance(v, Reg):
            v = v.q.sig
        if isinstance(v, Wire):
            return v.sig
        if isinstance(v, Signal):
            return v
        raise TypeError(f"cannot convert {type(v).__name__} to Signal")


@dataclass(frozen=True)
class PriorityEncodeResult:
    index: Any
    valid: Any


def is_bits_signal(signal: Signal[Data]) -> TypeGuard[Signal[Bits]]:
    """Return whether ``signal`` carries a scalar ``Bits`` type."""
    return isinstance(signal.ty, Bits)


class Module:
    def __init__(self, name: str) -> None:
        self.name = name
        self._args: list[tuple[str, Signal]] = []
        self._results: list[tuple[str, Signal]] = []
        self._lines: list[str] = []
        self._temp_var_index = 0
        self._indent_level = 1
        # finalize callbacks to run after emit_mlir() but before returning the final MLIR string.
        self._finalizers: list[Callable[[], None]] = []
        self._finalized = False
        # Extra `func.func` attributes emitted by `emit_func_mlir()`.
        # Values are stored as MLIR attribute literals (e.g. `"foo"`).
        self._func_attrs: dict[str, str] = {}

    def _set_func_attr_impl(self, key: str, value_literal: str) -> None:
        if self._finalized:
            raise RuntimeError("cannot set func attributes after emit_mlir()")
        k = str(key).strip()
        if not k:
            raise ValueError("func attribute key must be non-empty")
        v = str(value_literal).strip()
        if not v:
            raise ValueError("func attribute literal must be non-empty")
        self._func_attrs[k] = v

    def set_func_attr(self, key: str, value: str) -> None:
        """Set a `func.func` string attribute.

        This is intended for attaching debug/metadata attributes such as:
        - `pyc.base = "Core"`
        - `pyc.params = "{\"WIDTH\":32}"`
        """
        # MLIR string attributes use double quotes; reuse JSON escaping.
        self._set_func_attr_impl(key, json.dumps(str(value), ensure_ascii=False))

    def set_func_attr_literal(self, key: str, value_literal: str) -> None:
        """Set a `func.func` attribute using a raw MLIR attribute literal."""
        self._set_func_attr_impl(key, value_literal)

    def set_func_attr_json(self, key: str, value: object) -> None:
        """Set a `func.func` attribute using JSON-compatible MLIR literal syntax."""
        self._set_func_attr_impl(key, json.dumps(value, ensure_ascii=False))

    # --- types ---
    def clock(self, name: str) -> Signal:
        return self._arg(name, Clock())

    def reset(self, name: str) -> Signal:
        return self._arg(name, Reset())

    def reset_active(self, rst: Signal) -> Signal:
        """Return i1 where **1** means reset is asserted (same convention as ``Tb.reset`` / SV TB)."""
        if not isinstance(rst.ty, Reset):
            raise TypeError(
                "reset_active expects a !pyc.reset signal (use m.reset(...))"
            )
        tmp = self._get_next_temp_var()
        self._emit(f"{tmp} = pyc.reset_active {rst.ref} : i1")
        return Signal(ref=tmp, ty=Bits(1))

    def input(self, name: str, *, width: int) -> Signal[Bits]:
        if width <= 0:
            raise ValueError("width must be > 0")
        return self._arg(name, Bits(width))

    def output(self, name: str, value: Signal) -> None:
        self._results.append((name, value))

    # --- builders ---
    def const(self, value: int, *, width: int) -> Signal[Bits]:
        if not isinstance(value, int):
            raise TypeError(f"const() value must be int, got {type(value).__name__}")
        if width <= 0:
            raise ValueError("width must be > 0")
        ty = Bits(width)
        imm = int(value) & ((1 << int(width)) - 1)
        tmp = self._get_next_temp_var()
        self._emit(f"{tmp} = pyc.constant {imm} : {ty}")
        return Signal(ref=tmp, ty=ty)

    def _emit_elementwise_binary(
        self, op: str, a: Signal, b: Signal, *, compare: bool = False
    ) -> Signal:
        if not isinstance(a.ty, Bits) or not isinstance(b.ty, Bits):
            raise TypeError(f"{op} operands must be scalar integer signals")
        if a.ty != b.ty:
            raise TypeError(f"{op} operand types must match: {a.ty} vs {b.ty}")
        result_ty = Bits(1) if compare else a.ty
        tmp = self._get_next_temp_var()
        self._emit(f"{tmp} = pyc.{op} {a.ref}, {b.ref} : {a.ty}, {b.ty} -> {result_ty}")
        return Signal(ref=tmp, ty=result_ty)

    def add(self, a: Signal, b: Signal) -> Signal:
        return self._emit_elementwise_binary("add", a, b)

    def sub(self, a: Signal, b: Signal) -> Signal:
        return self._emit_elementwise_binary("sub", a, b)

    def mul(self, a: Signal, b: Signal) -> Signal:
        return self._emit_elementwise_binary("mul", a, b)

    def udiv(self, a: Signal, b: Signal) -> Signal:
        return self._emit_elementwise_binary("udiv", a, b)

    def urem(self, a: Signal, b: Signal) -> Signal:
        return self._emit_elementwise_binary("urem", a, b)

    def sdiv(self, a: Signal, b: Signal) -> Signal:
        return self._emit_elementwise_binary("sdiv", a, b)

    def srem(self, a: Signal, b: Signal) -> Signal:
        return self._emit_elementwise_binary("srem", a, b)

    def mux(self, sel: Signal, a: Signal, b: Signal) -> Signal:
        if sel.ty != Bits(1):
            raise TypeError(f"mux sel must be i1, got {sel.ty}")
        if not isinstance(a.ty, Bits) or a.ty != b.ty:
            raise TypeError(
                f"mux values must have the same scalar integer type, got {a.ty} vs {b.ty}"
            )
        tmp = self._get_next_temp_var()
        self._emit(
            f"{tmp} = pyc.select {sel.ref}, {a.ref}, {b.ref} : "
            f"{sel.ty}, {a.ty}, {b.ty} -> {a.ty}"
        )
        return Signal(ref=tmp, ty=a.ty)

    def and_(self, a: Signal, b: Signal) -> Signal:
        return self._emit_elementwise_binary("and", a, b)

    def or_(self, a: Signal[DT], b: Signal[DT]) -> Signal[DT]:
        return self._emit_elementwise_binary("or", a, b)

    def xor(self, a: Signal, b: Signal) -> Signal:
        return self._emit_elementwise_binary("xor", a, b)

    def not_(self, a: Signal) -> Signal:
        tmp = self._get_next_temp_var()
        self._emit(f"{tmp} = pyc.not {a.ref} : {a.ty}")
        return Signal(ref=tmp, ty=a.ty)

    def eq(self, a: Signal, b: Signal) -> Signal:
        return self._emit_compare("eq", a, b)

    def ult(self, a: Signal, b: Signal) -> Signal:
        return self._emit_compare("ult", a, b)

    def slt(self, a: Signal, b: Signal) -> Signal:
        return self._emit_compare("slt", a, b)

    def _emit_compare(self, predicate: str, a: Signal, b: Signal) -> Signal:
        if predicate not in {"eq", "ult", "slt"}:
            raise ValueError(f"unsupported comparison predicate: {predicate}")
        if not isinstance(a.ty, Bits) or a.ty != b.ty:
            raise TypeError(
                f"cmp operands must have the same scalar integer type: {a.ty} vs {b.ty}"
            )
        tmp = self._get_next_temp_var()
        self._emit(
            f'{tmp} = pyc.cmp {a.ref}, {b.ref} {{predicate = "{predicate}"}} '
            f": {a.ty}, {b.ty} -> i1"
        )
        return Signal(ref=tmp, ty=Bits(1))

    def trunc(self, a: Signal, *, width: int) -> Signal:
        if not isinstance(a.ty, Bits):
            raise TypeError("trunc requires a scalar integer input")
        if width >= a.width:
            raise ValueError(
                f"trunc width must be < input width, got {width} >= {a.width}"
            )
        out_ty = Bits(width)
        tmp = self._get_next_temp_var()
        self._emit(f"{tmp} = pyc.trunc {a.ref} : {a.ty} -> {out_ty}")
        return Signal(ref=tmp, ty=out_ty)

    def zext(self, a: Signal, *, width: int) -> Signal:
        if not isinstance(a.ty, Bits):
            raise TypeError("zext requires a scalar integer input")
        out_ty = Bits(width)
        tmp = self._get_next_temp_var()
        self._emit(f"{tmp} = pyc.zext {a.ref} : {a.ty} -> {out_ty}")
        return Signal(ref=tmp, ty=out_ty)

    def sext(self, a: Signal, *, width: int) -> Signal:
        if not isinstance(a.ty, Bits):
            raise TypeError("sext requires a scalar integer input")
        out_ty = Bits(width)
        tmp = self._get_next_temp_var()
        self._emit(f"{tmp} = pyc.sext {a.ref} : {a.ty} -> {out_ty}")
        return Signal(ref=tmp, ty=out_ty)

    def extract(self, a: Signal, *, lsb: int, width: int) -> Signal:
        if not isinstance(a.ty, Bits):
            raise TypeError("extract requires a scalar integer input")
        if lsb < 0:
            raise ValueError("extract lsb must be >= 0")
        if width <= 0:
            raise ValueError("extract width must be > 0")
        if lsb + width > a.width:
            raise ValueError("extract slice out of range for input width")
        out_ty = Bits(width)
        tmp = self._get_next_temp_var()
        msb = int(lsb) + int(width) - 1
        self._emit(
            f"{tmp} = pyc.extract {a.ref} {{lsb = {int(lsb)}, msb = {msb}}} "
            f": {a.ty} -> {out_ty}"
        )
        return Signal(ref=tmp, ty=out_ty)

    def shl(self, a: Signal, amount: Signal) -> Signal:
        if not isinstance(a.ty, Bits) or not isinstance(amount.ty, Bits):
            raise TypeError("shl requires scalar integer input and amount")
        tmp = self._get_next_temp_var()
        self._emit(f"{tmp} = pyc.shl {a.ref}, {amount.ref} : {a.ty}, {amount.ty}")
        return Signal(ref=tmp, ty=a.ty)

    def lshr(self, a: Signal, amount: Signal) -> Signal:
        if not isinstance(a.ty, Bits) or not isinstance(amount.ty, Bits):
            raise TypeError("lshr requires scalar integer input and amount")
        tmp = self._get_next_temp_var()
        self._emit(f"{tmp} = pyc.lshr {a.ref}, {amount.ref} : {a.ty}, {amount.ty}")
        return Signal(ref=tmp, ty=a.ty)

    def ashr(self, a: Signal, amount: Signal) -> Signal:
        if not isinstance(a.ty, Bits) or not isinstance(amount.ty, Bits):
            raise TypeError("ashr requires scalar integer input and amount")
        tmp = self._get_next_temp_var()
        self._emit(f"{tmp} = pyc.ashr {a.ref}, {amount.ref} : {a.ty}, {amount.ty}")
        return Signal(ref=tmp, ty=a.ty)

    def concat(self, *inputs: Signal) -> Signal:
        """Concatenate integer signals into a packed bus (MSB-first)."""
        if not inputs:
            raise ValueError("concat requires at least one input")

        def w(ty: Data) -> int:
            if not isinstance(ty, Bits):
                raise TypeError("concat only supports integer types")
            return ty.width

        out_w = sum(w(s.ty) for s in inputs)
        out_ty = Bits(out_w)
        tmp = self._get_next_temp_var()
        op_list = ", ".join(s.ref for s in inputs)
        ty_list = ", ".join(str(s.ty) for s in inputs)
        self._emit(f"{tmp} = pyc.concat ({op_list}) : ({ty_list}) -> {out_ty}")
        return Signal(ref=tmp, ty=out_ty)

    def priority_encode(
        self, value: Signal[Bits], *, order: str = "low"
    ) -> PriorityEncodeResult:
        if not isinstance(value.ty, Bits):
            raise TypeError(f"priority_encode expects scalar Bits, got {value.ty}")
        normalized = str(order).strip().lower()
        if normalized not in {"low", "high"}:
            raise ValueError("priority_encode order must be 'low' or 'high'")
        index_type = Bits(max(1, (value.width - 1).bit_length()))
        index_ref = self._get_next_temp_var()
        valid_ref = self._get_next_temp_var()
        self._emit(
            f"{index_ref}, {valid_ref} = pyc.priority_encode {value.ref} "
            f"{{order = {json.dumps(normalized)}}} : {value.ty} -> "
            f"{index_type}, i1"
        )
        return PriorityEncodeResult(
            index=Signal(ref=index_ref, ty=index_type),
            valid=Signal(ref=valid_ref, ty=Bits(1)),
        )

    def popcount(self, value: Signal[Bits]) -> Signal[Bits]:
        if not isinstance(value.ty, Bits):
            raise TypeError(f"popcount expects scalar Bits, got {value.ty}")
        count_type = Bits(max(1, value.width.bit_length()))
        count_ref = self._get_next_temp_var()
        self._emit(
            f"{count_ref} = pyc.popcount {value.ref} : {value.ty} -> {count_type}"
        )
        return Signal(ref=count_ref, ty=count_type)

    def _count_zeros(self, value: Signal[Bits], *, direction: str) -> Signal[Bits]:
        if not isinstance(value.ty, Bits):
            raise TypeError(f"count_zeros expects scalar Bits, got {value.ty}")
        if direction not in {"leading", "trailing"}:
            raise ValueError("count_zeros direction must be 'leading' or 'trailing'")
        count_type = Bits(max(1, value.width.bit_length()))
        count_ref = self._get_next_temp_var()
        self._emit(
            f'{count_ref} = pyc.count_zeros {value.ref} {{direction = "{direction}"}} '
            f": {value.ty} -> {count_type}"
        )
        return Signal(ref=count_ref, ty=count_type)

    def count_leading_zeros(self, value: Signal[Bits]) -> Signal[Bits]:
        return self._count_zeros(value, direction="leading")

    def count_trailing_zeros(self, value: Signal[Bits]) -> Signal[Bits]:
        return self._count_zeros(value, direction="trailing")

    def instance_op(
        self,
        callee: str,
        *inputs: Signal,
        result_types: list[Data | str],
        name: str | None = None,
        short_name: str | None = None,
        keep: bool = False,
    ) -> list[Signal]:
        """Instantiate a sub-module by symbol (pyc.instance).

        `callee` is the referenced `func.func` symbol name.
        """
        callee = str(callee).strip()
        if not callee:
            raise ValueError("instance_op callee must be non-empty")

        out: list[Signal] = []
        for ty in result_types:
            tmp = self._get_next_temp_var()
            dt = ty if isinstance(ty, Data) else Data.from_str(ty)
            out.append(Signal(ref=tmp, ty=dt))

        lhs = ""
        if out:
            if len(out) == 1:
                lhs = f"{out[0].ref} = "
            else:
                lhs = f"{', '.join(s.ref for s in out)} = "

        ops = ", ".join(s.ref for s in inputs)
        attrs = f"{{callee = @{callee}"
        if name is not None:
            attrs += f", name = {json.dumps(str(name), ensure_ascii=False)}"
        if short_name is not None:
            attrs += f", short_name = {json.dumps(str(short_name), ensure_ascii=False)}"
        if keep:
            attrs += ", pyc.debug_keep = true"
        attrs += "}"

        in_ty_sig = ", ".join(str(s.ty) for s in inputs)
        in_sig = f"({in_ty_sig})"
        if len(out) == 0:
            out_sig = "()"
        elif len(out) == 1:
            out_sig = str(out[0].ty)
        else:
            out_ty_sig = ", ".join(str(s.ty) for s in out)
            out_sig = f"({out_ty_sig})"

        if ops:
            self._emit(f"{lhs}pyc.instance {ops} {attrs} : {in_sig} -> {out_sig}")
        else:
            self._emit(f"{lhs}pyc.instance {attrs} : {in_sig} -> {out_sig}")
        return out

    def alias(self, a: Signal, *, name: str | None = None) -> Signal:
        """Alias a value (pure) to attach a debug name in codegen."""
        tmp = self._get_next_temp_var()
        if name is None:
            self._emit(f"{tmp} = pyc.alias {a.ref} : {a.ty}")
        else:
            self._emit(f'{tmp} = pyc.alias {a.ref} {{pyc.name = "{name}"}} : {a.ty}')
        return Signal(ref=tmp, ty=a.ty)

    def new_wire(self, *, width: int, name: str | None = None) -> Signal:
        return self.new_signal(width=width, name=name)

    def new_signal(self, *, width: int, name: str | None = None) -> Signal:
        if width <= 0:
            raise ValueError("width must be > 0")
        ty = Bits(width)
        tmp = self._get_next_temp_var()
        if name is None:
            self._emit(f"{tmp} = pyc.wire : {ty}")
        else:
            self._emit(f'{tmp} = pyc.wire {{pyc.name = "{name}"}} : {ty}')
        return Signal(ref=tmp, ty=ty)

    def assign(self, dst: Signal, src: Signal) -> None:
        self._require_same_ty(dst.ty, src.ty, "assign")
        self._emit(f"pyc.assign {dst.ref}, {src.ref} : {dst.ty}")

    def assert_(self, cond: Signal, *, msg: str | None = None) -> None:
        """Simulation-only assertion (prototype)."""
        if cond.ty != Bits(1):
            raise TypeError("assert_ cond must be i1")
        if msg is None:
            self._emit(f"pyc.assert {cond.ref}")
            return
        s = str(msg)
        if not s:
            self._emit(f"pyc.assert {cond.ref}")
            return
        self._emit(
            f"pyc.assert {cond.ref} {{msg = {json.dumps(s, ensure_ascii=False)}}}"
        )

    def reg(
        self, clk: Signal, rst: Signal, en: Signal, next_: Signal, init: Signal
    ) -> Signal:
        if not isinstance(clk.ty, Clock):
            raise TypeError("reg clk must be !pyc.clock")
        if not isinstance(rst.ty, Reset):
            raise TypeError("reg rst must be !pyc.reset")
        if en.ty != Bits(1):
            raise TypeError("reg en must be i1")
        self._require_same_ty(next_.ty, init.ty, "reg")
        tmp = self._get_next_temp_var()
        self._emit(
            f"{tmp} = pyc.reg {clk.ref}, {rst.ref}, {en.ref}, {next_.ref}, {init.ref} : {next_.ty}"
        )
        return Signal(ref=tmp, ty=next_.ty)

    def fifo(
        self,
        clk: Signal,
        rst: Signal,
        in_valid: Signal,
        in_data: Signal,
        out_ready: Signal,
        *,
        depth: int,
    ) -> tuple[Signal, Signal, Signal]:
        if not isinstance(clk.ty, Clock):
            raise TypeError("fifo clk must be !pyc.clock")
        if not isinstance(rst.ty, Reset):
            raise TypeError("fifo rst must be !pyc.reset")
        if in_valid.ty != Bits(1):
            raise TypeError("fifo in_valid must be i1")
        if out_ready.ty != Bits(1):
            raise TypeError("fifo out_ready must be i1")
        if depth <= 0:
            raise ValueError("fifo depth must be > 0")
        in_ready = self._get_next_temp_var()
        out_valid = self._get_next_temp_var()
        out_data = self._get_next_temp_var()
        self._emit(
            f"{in_ready}, {out_valid}, {out_data} = pyc.fifo {clk.ref}, {rst.ref}, {in_valid.ref}, {in_data.ref}, {out_ready.ref} "
            + f"{{depth = {int(depth)}}} : {in_data.ty}"
        )
        return (
            Signal(in_ready, Bits(1)),
            Signal(out_valid, Bits(1)),
            Signal(out_data, in_data.ty),
        )

    def byte_mem(
        self,
        clk: Signal,
        rst: Signal,
        raddr: Signal,
        wvalid: Signal,
        waddr: Signal,
        wdata: Signal,
        wstrb: Signal,
        *,
        depth: int,
        name: str,
    ) -> Signal:
        """Byte-addressed memory (async read + sync write, prototype)."""
        if not isinstance(clk.ty, Clock):
            raise TypeError("byte_mem clk must be !pyc.clock")
        if not isinstance(rst.ty, Reset):
            raise TypeError("byte_mem rst must be !pyc.reset")
        if wvalid.ty != Bits(1):
            raise TypeError("byte_mem wvalid must be i1")
        if raddr.ty != waddr.ty:
            raise TypeError("byte_mem raddr/waddr must have the same type")
        if wdata.ty != Bits(64):
            raise TypeError("byte_mem wdata must be Bits(64)")
        if wstrb.ty != Bits(8):
            raise TypeError("byte_mem wstrb must be Bits(8)")
        if depth <= 0:
            raise ValueError("byte_mem depth must be > 0")
        if not isinstance(name, str) or not name.strip() or not _IDENT_RE.match(name):
            raise ValueError(
                "byte_mem name must match [A-Za-z_][A-Za-z0-9_]* (Decision 0025)"
            )

        tmp = self._get_next_temp_var()
        attrs = f'{{depth = {int(depth)}, name = "{name}"}}'
        self._emit(
            f"{tmp} = pyc.byte_mem {clk.ref}, {rst.ref}, {raddr.ref}, {wvalid.ref}, {waddr.ref}, {wdata.ref}, {wstrb.ref} "
            + f"{attrs} : {raddr.ty}, {wdata.ty}, {wstrb.ty}"
        )
        return Signal(ref=tmp, ty=wdata.ty)

    def sync_mem(
        self,
        clk: Signal,
        rst: Signal,
        ren: Signal,
        raddr: Signal,
        wvalid: Signal,
        waddr: Signal,
        wdata: Signal,
        wstrb: Signal,
        *,
        depth: int,
        name: str,
    ) -> Signal:
        """Synchronous 1R1W memory (registered read data, prototype)."""
        if not isinstance(clk.ty, Clock):
            raise TypeError("sync_mem clk must be !pyc.clock")
        if not isinstance(rst.ty, Reset):
            raise TypeError("sync_mem rst must be !pyc.reset")
        if ren.ty != Bits(1):
            raise TypeError("sync_mem ren must be i1")
        if wvalid.ty != Bits(1):
            raise TypeError("sync_mem wvalid must be i1")
        if raddr.ty != waddr.ty:
            raise TypeError("sync_mem raddr/waddr must have the same type")
        if depth <= 0:
            raise ValueError("sync_mem depth must be > 0")
        if not isinstance(name, str) or not name.strip() or not _IDENT_RE.match(name):
            raise ValueError(
                "sync_mem name must match [A-Za-z_][A-Za-z0-9_]* (Decision 0025)"
            )

        tmp = self._get_next_temp_var()
        attrs = f'{{depth = {int(depth)}, name = "{name}"}}'
        self._emit(
            f"{tmp} = pyc.sync_mem {clk.ref}, {rst.ref}, {ren.ref}, {raddr.ref}, {wvalid.ref}, {waddr.ref}, {wdata.ref}, {wstrb.ref} "
            + f"{attrs} : {raddr.ty}, {wdata.ty}, {wstrb.ty}"
        )
        return Signal(ref=tmp, ty=wdata.ty)

    def sync_mem_dp(
        self,
        clk: Signal,
        rst: Signal,
        ren0: Signal,
        raddr0: Signal,
        ren1: Signal,
        raddr1: Signal,
        wvalid: Signal,
        waddr: Signal,
        wdata: Signal,
        wstrb: Signal,
        *,
        depth: int,
        name: str,
    ) -> tuple[Signal, Signal]:
        """Synchronous 2R1W memory (registered outputs, prototype)."""
        if not isinstance(clk.ty, Clock):
            raise TypeError("sync_mem_dp clk must be !pyc.clock")
        if not isinstance(rst.ty, Reset):
            raise TypeError("sync_mem_dp rst must be !pyc.reset")
        if ren0.ty != Bits(1) or ren1.ty != Bits(1):
            raise TypeError("sync_mem_dp ren0/ren1 must be i1")
        if wvalid.ty != Bits(1):
            raise TypeError("sync_mem_dp wvalid must be i1")
        if raddr0.ty != raddr1.ty or raddr0.ty != waddr.ty:
            raise TypeError("sync_mem_dp raddr0/raddr1/waddr must have the same type")
        if depth <= 0:
            raise ValueError("sync_mem_dp depth must be > 0")
        if not isinstance(name, str) or not name.strip() or not _IDENT_RE.match(name):
            raise ValueError(
                "sync_mem_dp name must match [A-Za-z_][A-Za-z0-9_]* (Decision 0025)"
            )

        out0 = self._get_next_temp_var()
        out1 = self._get_next_temp_var()
        attrs = f'{{depth = {int(depth)}, name = "{name}"}}'
        self._emit(
            f"{out0}, {out1} = pyc.sync_mem_dp {clk.ref}, {rst.ref}, {ren0.ref}, {raddr0.ref}, {ren1.ref}, {raddr1.ref}, "
            + f"{wvalid.ref}, {waddr.ref}, {wdata.ref}, {wstrb.ref} {attrs} : {raddr0.ty}, {wdata.ty}, {wstrb.ty}"
        )
        return Signal(ref=out0, ty=wdata.ty), Signal(ref=out1, ty=wdata.ty)

    def async_fifo(
        self,
        in_clk: Signal,
        in_rst: Signal,
        out_clk: Signal,
        out_rst: Signal,
        in_valid: Signal,
        in_data: Signal,
        out_ready: Signal,
        *,
        depth: int,
    ) -> tuple[Signal, Signal, Signal]:
        if not isinstance(in_clk.ty, Clock) or not isinstance(out_clk.ty, Clock):
            raise TypeError("async_fifo clk must be !pyc.clock")
        if not isinstance(in_rst.ty, Reset) or not isinstance(out_rst.ty, Reset):
            raise TypeError("async_fifo rst must be !pyc.reset")
        if in_valid.ty != Bits(1):
            raise TypeError("async_fifo in_valid must be i1")
        if out_ready.ty != Bits(1):
            raise TypeError("async_fifo out_ready must be i1")
        if depth <= 0:
            raise ValueError("async_fifo depth must be > 0")
        in_ready = self._get_next_temp_var()
        out_valid = self._get_next_temp_var()
        out_data = self._get_next_temp_var()
        self._emit(
            f"{in_ready}, {out_valid}, {out_data} = pyc.async_fifo {in_clk.ref}, {in_rst.ref}, {out_clk.ref}, {out_rst.ref}, "
            + f"{in_valid.ref}, {in_data.ref}, {out_ready.ref} {{depth = {int(depth)}}} : {in_data.ty}"
        )
        return (
            Signal(in_ready, Bits(1)),
            Signal(out_valid, Bits(1)),
            Signal(out_data, in_data.ty),
        )

    def cdc_sync(
        self, clk: Signal, rst: Signal, a: Signal, *, stages: int | None = None
    ) -> Signal:
        if not isinstance(clk.ty, Clock):
            raise TypeError("cdc_sync clk must be !pyc.clock")
        if not isinstance(rst.ty, Reset):
            raise TypeError("cdc_sync rst must be !pyc.reset")
        tmp = self._get_next_temp_var()
        if stages is None:
            self._emit(f"{tmp} = pyc.cdc_sync {clk.ref}, {rst.ref}, {a.ref} : {a.ty}")
        else:
            self._emit(
                f"{tmp} = pyc.cdc_sync {clk.ref}, {rst.ref}, {a.ref} {{stages = {int(stages)}}} : {a.ty}"
            )
        return Signal(ref=tmp, ty=a.ty)

    # --- structured emission helpers (for AST/JIT frontends) ---
    def emit_line(self, line: str) -> None:
        """Emit a raw line at the current indentation level (inside func body)."""
        self._emit(line)

    def push_indent(self) -> None:
        self._indent_level += 1

    def pop_indent(self) -> None:
        if self._indent_level <= 1:
            raise RuntimeError("indent underflow")
        self._indent_level -= 1

    # --- emission ---
    def emit_func_mlir(self) -> str:
        if not self._finalized:
            self._finalized = True
            for fn in list(self._finalizers):
                fn()

        arg_sig = ", ".join(f"{sig.ref}: {sig.ty}" for _, sig in self._args)
        res_types = [v.ty for _, v in self._results]
        if len(res_types) == 0:
            res_sig = "-> ()"
            ret_ty = ""
        elif len(res_types) == 1:
            res_sig = f"-> {res_types[0]}"
            ret_ty = res_types[0]
        else:
            res_sig = f"-> ({', '.join(str(t) for t in res_types)})"
            ret_ty = ", ".join(str(t) for t in res_types)
        in_names = ", ".join(f'"{n}"' for n, _ in self._args)
        out_names = ", ".join(f'"{n}"' for n, _ in self._results)
        extra = ""
        if self._func_attrs:
            extra = ", " + ", ".join(f"{k} = {v}" for k, v in self._func_attrs.items())
        header = (
            f"func.func @{self.name}({arg_sig}) {res_sig} "
            f"attributes {{arg_names = [{in_names}], result_names = [{out_names}]{extra}}} {{\n"
        )
        body = "\n".join(self._lines)
        outs = ", ".join(v.ref for _, v in self._results)
        if outs:
            tail = f"\n  func.return {outs} : {ret_ty}\n}}\n"
        else:
            tail = "\n  func.return\n}\n"
        return header + body + tail

    def emit_mlir(self) -> str:
        return "module {\n" + self.emit_func_mlir() + "}\n"

    # --- finalizers ---
    def add_finalizer(self, fn: Callable[[], None]) -> None:
        if self._finalized:
            raise RuntimeError("cannot add finalizers after emit_mlir()")
        self._finalizers.append(fn)

    # --- internals ---
    def _arg(self, name: str, ty: Data) -> Signal:
        ref = f"%{name}"
        s = Signal(ref=ref, ty=ty)
        self._args.append((name, s))
        return s

    def _get_next_temp_var(self) -> str:
        self._temp_var_index += 1
        return f"%v{self._temp_var_index}"

    def _emit(self, line: str) -> None:
        self._lines.append(("  " * self._indent_level) + line)

    @staticmethod
    def _require_same_ty(a: Data, b: Data, op: str) -> None:
        if a != b:
            raise TypeError(f"{op} requires same types, got {a} and {b}")
