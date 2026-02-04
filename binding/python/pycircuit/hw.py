from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass
from typing import Any, Iterable, Iterator, Union, overload

from .dsl import Module, Signal


def _int_width(ty: str) -> int:
    if not ty.startswith("i"):
        raise TypeError(f"expected integer type iN, got {ty!r}")
    w = int(ty[1:])
    if w <= 0:
        raise ValueError(f"invalid integer width: {ty!r}")
    return w


@dataclass(frozen=True)
class Wire:
    m: Module
    sig: Signal

    def __post_init__(self) -> None:
        _int_width(self.sig.ty)

    @property
    def ref(self) -> str:
        return self.sig.ref

    @property
    def ty(self) -> str:
        return self.sig.ty

    @property
    def width(self) -> int:
        return _int_width(self.sig.ty)

    def __str__(self) -> str:
        return self.sig.ref

    def out(self) -> "Wire":
        """Stage-friendly sugar: a Wire's value is itself."""
        return self

    def _coerce(self, other: Union["Wire", "Reg", Signal, int]) -> Signal:
        if isinstance(other, Reg):
            if other.q.m is not self.m:
                raise ValueError("cannot combine wires from different modules")
            return other.q.sig
        if isinstance(other, Wire):
            if other.m is not self.m:
                raise ValueError("cannot combine wires from different modules")
            return other.sig
        if isinstance(other, Signal):
            return other
        if isinstance(other, int):
            return self.m.const(other, width=self.width)
        raise TypeError(f"unsupported operand type: {type(other).__name__}")

    def __add__(self, other: Union["Wire", "Reg", Signal, int]) -> "Wire":
        b = self._coerce(other)
        return Wire(self.m, self.m.add(self.sig, b))

    def __and__(self, other: Union["Wire", "Reg", Signal, int]) -> "Wire":
        b = self._coerce(other)
        return Wire(self.m, self.m.and_(self.sig, b))

    def __or__(self, other: Union["Wire", "Reg", Signal, int]) -> "Wire":
        b = self._coerce(other)
        return Wire(self.m, self.m.or_(self.sig, b))

    def __xor__(self, other: Union["Wire", "Reg", Signal, int]) -> "Wire":
        b = self._coerce(other)
        return Wire(self.m, self.m.xor(self.sig, b))

    def __invert__(self) -> "Wire":
        return Wire(self.m, self.m.not_(self.sig))

    def eq(self, other: Union["Wire", "Reg", Signal, int]) -> "Wire":
        b = self._coerce(other)
        if b.ty != self.ty:
            raise TypeError(f"eq requires same types, got {self.ty} and {b.ty}")
        return Wire(self.m, self.m.eq(self.sig, b))

    def select(self, a: Union["Wire", "Reg", Signal, int], b: Union["Wire", "Reg", Signal, int]) -> "Wire":
        if self.ty != "i1":
            raise TypeError("select() requires a 1-bit selector wire (i1)")

        def as_wire(v: Union[Wire, Reg, Signal, int], *, int_width: int | None) -> Wire:
            if isinstance(v, Reg):
                v = v.q
            if isinstance(v, Wire):
                if v.m is not self.m:
                    raise ValueError("cannot combine wires from different modules")
                return v
            if isinstance(v, Signal):
                return Wire(self.m, v)
            if isinstance(v, int):
                if int_width is None:
                    raise TypeError("select() cannot infer width for int literal (provide a Wire/Reg operand)")
                return self.m.const_wire(int(v), width=int_width)
            raise TypeError(f"unsupported operand type: {type(v).__name__}")

        if isinstance(a, int) and isinstance(b, int):
            raise TypeError("select() requires at least one Wire/Reg/Signal operand (cannot infer width from two ints)")

        if isinstance(a, int) and not isinstance(b, int):
            bw = as_wire(b, int_width=None)
            aw = as_wire(a, int_width=bw.width)
        elif isinstance(b, int) and not isinstance(a, int):
            aw = as_wire(a, int_width=None)
            bw = as_wire(b, int_width=aw.width)
        else:
            aw = as_wire(a, int_width=None)
            bw = as_wire(b, int_width=None)

        return Wire(self.m, self.m.mux(self.sig, aw.sig, bw.sig))

    def trunc(self, *, width: int) -> "Wire":
        return Wire(self.m, self.m.trunc(self.sig, width=width))

    def zext(self, *, width: int) -> "Wire":
        return Wire(self.m, self.m.zext(self.sig, width=width))

    def sext(self, *, width: int) -> "Wire":
        return Wire(self.m, self.m.sext(self.sig, width=width))

    def slice(self, *, lsb: int, width: int) -> "Wire":
        return Wire(self.m, self.m.extract(self.sig, lsb=lsb, width=width))

    def shl(self, *, amount: int) -> "Wire":
        return Wire(self.m, self.m.shli(self.sig, amount=amount))

    def __getitem__(self, idx: int | slice) -> "Wire":
        if isinstance(idx, slice):
            if idx.step is not None:
                raise TypeError("wire slicing does not support step")
            lsb = 0 if idx.start is None else int(idx.start)
            stop = self.width if idx.stop is None else int(idx.stop)
            if lsb < 0 or stop < 0:
                raise ValueError("wire slice indices must be >= 0")
            if stop < lsb:
                raise ValueError("wire slice stop must be >= start")
            width = stop - lsb
            if width <= 0:
                raise ValueError("wire slice width must be > 0")
            if lsb + width > self.width:
                raise ValueError("wire slice out of range")
            return self.slice(lsb=lsb, width=width)

        bit = int(idx)
        if bit < 0:
            raise ValueError("wire bit index must be >= 0")
        if bit >= self.width:
            raise ValueError("wire bit index out of range")
        return self.slice(lsb=bit, width=1)

    def named(self, name: str) -> "Wire":
        """Attach a debug name via `pyc.alias` (pure)."""
        scoped = str(name)
        scoped_name = getattr(self.m, "scoped_name", None)
        if callable(scoped_name):
            scoped = scoped_name(scoped)
        return Wire(self.m, self.m.alias(self.sig, name=scoped))


@dataclass(frozen=True)
class ClockDomain:
    clk: Signal
    rst: Signal


@dataclass(frozen=True)
class Reg:
    q: Wire
    clk: Signal
    rst: Signal
    en: Wire
    next: Wire
    init: Wire

    @property
    def ref(self) -> str:
        return self.q.ref

    @property
    def ty(self) -> str:
        return self.q.ty

    @property
    def width(self) -> int:
        return self.q.width

    def __str__(self) -> str:
        return self.q.ref

    def out(self) -> Wire:
        """Read the current value of the register (q) as a Wire."""
        return self.q

    def __add__(self, other: Union[Wire, Signal, int]) -> Wire:
        return self.q + other

    def __and__(self, other: Union[Wire, Signal, int]) -> Wire:
        return self.q & other

    def __or__(self, other: Union[Wire, Signal, int]) -> Wire:
        return self.q | other

    def __xor__(self, other: Union[Wire, Signal, int]) -> Wire:
        return self.q ^ other

    def __invert__(self) -> Wire:
        return ~self.q

    def eq(self, other: Union[Wire, Signal, int]) -> Wire:
        return self.q.eq(other)

    def select(self, a: Union[Wire, "Reg", Signal, int], b: Union[Wire, "Reg", Signal, int]) -> Wire:
        return self.q.select(a, b)

    def trunc(self, *, width: int) -> Wire:
        return self.q.trunc(width=width)

    def zext(self, *, width: int) -> Wire:
        return self.q.zext(width=width)

    def sext(self, *, width: int) -> Wire:
        return self.q.sext(width=width)

    def slice(self, *, lsb: int, width: int) -> Wire:
        return self.q.slice(lsb=lsb, width=width)

    def shl(self, *, amount: int) -> Wire:
        return self.q.shl(amount=amount)

    def __getitem__(self, idx: int | slice) -> Wire:
        return self.q[idx]

    def set(self, value: Union[Wire, "Reg", Signal, int], *, when: Union[Wire, Signal, int] = 1) -> None:
        """Drive `self.next` (backedge) for a stateful variable.

        - `r.set(v)` is equivalent to `m.assign(r.next, v)`
        - `r.set(v, when=cond)` drives `cond ? v : r` (hold otherwise)
        """
        m = self.q.m
        if not isinstance(m, Circuit):
            raise TypeError("Reg.set requires the Reg to belong to a Circuit")

        def as_wire(v: Union[Wire, Reg, Signal, int], *, width: int) -> Wire:
            if isinstance(v, Reg):
                return v.q
            if isinstance(v, Wire):
                if v.m is not m:
                    raise ValueError("cannot combine wires from different modules")
                return v
            if isinstance(v, Signal):
                return Wire(m, v)
            if isinstance(v, int):
                return m.const_wire(int(v), width=width)
            raise TypeError(f"unsupported value type: {type(v).__name__}")

        next_w = as_wire(value, width=self.width)

        if isinstance(when, int) and int(when) == 1:
            m.assign(self.next, next_w)
            return

        cond = as_wire(when, width=1)
        if cond.ty != "i1":
            raise TypeError("when must be i1")
        m.assign(self.next, cond.select(next_w, self))

    def __ilshift__(self, other: Union[Wire, "Reg", Signal, int]) -> "Reg":
        self.set(other)
        return self


class Circuit(Module):
    """High-level wrapper over `Module` that returns `Wire`/`Reg` objects."""

    def __init__(self, name: str) -> None:
        super().__init__(name)
        self._scope_stack: list[str] = []

    def scoped_name(self, name: str) -> str:
        if not self._scope_stack:
            return name
        return "__".join([*self._scope_stack, name])

    @contextmanager
    def scope(self, name: str) -> Iterator[None]:
        self._scope_stack.append(str(name))
        try:
            yield
        finally:
            self._scope_stack.pop()

    def apply(self, fn: Any, /, *args: Any, name: str | None = None) -> Any:
        """Call `fn(*args)` under a naming scope (debug-friendly).

        This does not create a true hierarchical module boundary in the current IR,
        but it provides stable scoped names for any `named()` aliases created inside.
        """
        scope = name or getattr(fn, "__name__", "apply")
        with self.scope(str(scope)):
            return fn(*args)

    def domain(self, name: str) -> ClockDomain:
        return ClockDomain(clk=self.clock(f"{name}_clk"), rst=self.reset(f"{name}_rst"))

    def in_wire(self, name: str, *, width: int) -> Wire:
        return Wire(self, self.input(name, width=width))

    def const_wire(self, value: int, *, width: int) -> Wire:
        return Wire(self, self.const(value, width=width))

    def new_wire(self, *, width: int) -> Wire:
        return Wire(self, super().new_wire(width=width))

    def named_wire(self, name: str, *, width: int) -> Wire:
        return Wire(self, super().new_wire(width=width, name=self.scoped_name(name)))

    def wire(self, sig: Signal) -> Wire:
        return Wire(self, sig)

    def named(self, v: Union[Wire, Reg, Signal], name: str) -> Wire:
        """Attach a scoped debug name via `pyc.alias` (pure)."""
        if isinstance(v, Reg):
            v = v.q
        if isinstance(v, Wire):
            return Wire(self, self.alias(v.sig, name=self.scoped_name(name)))
        return Wire(self, self.alias(v, name=self.scoped_name(name)))

    def assign(self, dst: Union[Wire, Reg, Signal], src: Union[Wire, Reg, Signal, int]) -> None:  # type: ignore[override]
        def as_sig(v: Union[Wire, Reg, Signal]) -> Signal:
            if isinstance(v, Reg):
                return v.q.sig
            if isinstance(v, Wire):
                return v.sig
            return v

        dst_sig = as_sig(dst)
        if isinstance(src, int):
            src_sig = self.const(int(src), width=_int_width(dst_sig.ty))
        else:
            src_sig = as_sig(src)
        super().assign(dst_sig, src_sig)

    def out(
        self,
        name: str,
        *,
        clk: Signal | None = None,
        rst: Signal | None = None,
        domain: ClockDomain | None = None,
        width: int,
        init: Union[Wire, Reg, Signal, int] = 0,
        en: Union[Wire, Signal, int] = 1,
        stage: str | None = None,
        signed: bool | None = None,  # reserved for future type inference / lowering
    ) -> Reg:
        """Declare a named stateful variable (backedge register).

        This is a higher-level replacement for `backedge_reg(...)` that:
        - takes a stable logical name (for debug/name mangling),
        - optionally tags the name with a pipeline stage prefix,
        - declares a named backedge wire for `next`.
        """
        _ = signed  # unused for now (kept for API stability)

        if domain is not None:
            clk = domain.clk
            rst = domain.rst
        if clk is None or rst is None:
            raise TypeError("out() requires either domain=... or both clk=... and rst=...")

        full = str(name)
        if stage:
            full = f"{stage}__{full}"
        full = self.scoped_name(full)

        next_w = Wire(self, super().new_wire(width=width, name=f"{full}__next"))
        if isinstance(en, int):
            en_w: Union[Wire, Signal] = self.const_wire(int(en), width=1)
        else:
            en_w = en

        if isinstance(init, int):
            init_w: Union[Wire, Signal] = self.const_wire(int(init), width=width)
        else:
            init_w = init

        r = self.reg_wire(clk, rst, en_w, next_w, init_w)
        # Name the observable value of the state variable.
        q_named = Wire(self, self.alias(r.q.sig, name=full))
        return Reg(q=q_named, clk=r.clk, rst=r.rst, en=r.en, next=r.next, init=r.init)

    def reg_wire(
        self, clk: Signal, rst: Signal, en: Union[Wire, Signal], next_: Union[Wire, Signal], init: Union[Wire, Signal, int]
    ) -> Reg:
        en_w = en if isinstance(en, Wire) else Wire(self, en)
        next_w = next_ if isinstance(next_, Wire) else Wire(self, next_)
        if isinstance(init, int):
            init_w = self.const_wire(init, width=next_w.width)
        else:
            init_w = init if isinstance(init, Wire) else Wire(self, init)

        q_sig = self.reg(clk, rst, en_w.sig, next_w.sig, init_w.sig)
        q_w = Wire(self, q_sig)
        return Reg(q=q_w, clk=clk, rst=rst, en=en_w, next=next_w, init=init_w)

    def reg_domain(self, domain: ClockDomain, en: Union[Wire, Signal], next_: Union[Wire, Signal], init: Union[Wire, Signal, int]) -> Reg:
        return self.reg_wire(domain.clk, domain.rst, en, next_, init)

    def backedge_reg(
        self,
        clk: Signal,
        rst: Signal,
        *,
        width: int,
        init: Union[Wire, Signal, int],
        en: Union[Wire, Signal, int] = 1,
    ) -> Reg:
        """Create a register whose `next` is a placeholder `pyc.wire` meant to be driven via `pyc.assign`.

        This pattern enables feedback loops (state machines) in a netlist-like style:

        - `r = m.backedge_reg(...)` creates `r.next` as a `pyc.wire`
        - Later: `m.assign(r.next, some_next_value)`
        """
        next_w = self.new_wire(width=width)
        if isinstance(en, int):
            en_w: Union[Wire, Signal] = self.const_wire(en, width=1)
        else:
            en_w = en
        return self.reg_wire(clk, rst, en_w, next_w, init)

    def vec(self, *elems: Union["Wire", "Reg"]) -> "Vec":
        return Vec(elems)

    def cat(self, *elems: Union["Wire", "Reg"]) -> Wire:
        """Concatenate wires/regs into a packed bus (MSB-first)."""
        return self.vec(*elems).pack()

    def bundle(self, **fields: Union["Wire", "Reg"]) -> "Bundle":
        return Bundle(fields)

    def byte_mem(
        self,
        clk: Signal,
        rst: Signal,
        *,
        raddr: Union[Wire, Reg, Signal],
        wvalid: Union[Wire, Reg, Signal],
        waddr: Union[Wire, Reg, Signal],
        wdata: Union[Wire, Reg, Signal],
        wstrb: Union[Wire, Reg, Signal],
        depth: int,
        name: str | None = None,
    ) -> Wire:
        def as_sig(v: Union[Wire, Reg, Signal]) -> Signal:
            if isinstance(v, Reg):
                return v.q.sig
            if isinstance(v, Wire):
                return v.sig
            return v

        rdata = super().byte_mem(
            clk,
            rst,
            as_sig(raddr),
            as_sig(wvalid),
            as_sig(waddr),
            as_sig(wdata),
            as_sig(wstrb),
            depth=depth,
            name=name,
        )
        return Wire(self, rdata)

    def fifo(
        self,
        clk: Signal,
        rst: Signal,
        *,
        in_valid: Union[Wire, Reg, Signal],
        in_data: Union[Wire, Reg, Signal],
        out_ready: Union[Wire, Reg, Signal],
        depth: int,
    ) -> tuple[Wire, Wire, Wire]:
        """Strict ready/valid FIFO (single-clock, prototype)."""

        def as_sig(v: Union[Wire, Reg, Signal]) -> Signal:
            if isinstance(v, Reg):
                return v.q.sig
            if isinstance(v, Wire):
                return v.sig
            return v

        in_ready, out_valid, out_data = super().fifo(
            clk,
            rst,
            as_sig(in_valid),
            as_sig(in_data),
            as_sig(out_ready),
            depth=depth,
        )
        return Wire(self, in_ready), Wire(self, out_valid), Wire(self, out_data)

    def fifo_domain(
        self,
        domain: ClockDomain,
        *,
        in_valid: Union[Wire, Reg, Signal],
        in_data: Union[Wire, Reg, Signal],
        out_ready: Union[Wire, Reg, Signal],
        depth: int,
    ) -> tuple[Wire, Wire, Wire]:
        return self.fifo(domain.clk, domain.rst, in_valid=in_valid, in_data=in_data, out_ready=out_ready, depth=depth)

    def queue(
        self,
        name: str,
        *,
        clk: Signal | None = None,
        rst: Signal | None = None,
        domain: ClockDomain | None = None,
        width: int,
        depth: int,
    ) -> "Queue":
        if domain is not None:
            clk = domain.clk
            rst = domain.rst
        if clk is None or rst is None:
            raise TypeError("queue() requires either domain=... or both clk=... and rst=...")
        return Queue(self, name, clk=clk, rst=rst, width=width, depth=depth)


@dataclass(frozen=True)
class Vec:
    """A small fixed-length container of wires/regs for building pipelines."""

    elems: tuple[Union[Wire, Reg], ...]

    def __post_init__(self) -> None:
        if not self.elems:
            raise ValueError("Vec cannot be empty")

        m0 = self._module_of(self.elems[0])
        for e in self.elems[1:]:
            if self._module_of(e) is not m0:
                raise ValueError("Vec elements must belong to the same Circuit/Module")

    @staticmethod
    def _module_of(e: Union[Wire, Reg]) -> Module:
        if isinstance(e, Wire):
            return e.m
        return e.q.m

    @property
    def m(self) -> Module:
        return self._module_of(self.elems[0])

    def __len__(self) -> int:
        return len(self.elems)

    def __iter__(self) -> Iterator[Union[Wire, Reg]]:
        return iter(self.elems)

    @overload
    def __getitem__(self, idx: int) -> Union[Wire, Reg]: ...

    @overload
    def __getitem__(self, idx: slice) -> "Vec": ...

    def __getitem__(self, idx: int | slice) -> Union[Wire, Reg, "Vec"]:
        if isinstance(idx, slice):
            return Vec(self.elems[idx])
        return self.elems[int(idx)]

    def wires(self) -> tuple[Wire, ...]:
        out: list[Wire] = []
        for e in self.elems:
            out.append(e if isinstance(e, Wire) else e.q)
        return tuple(out)

    @property
    def total_width(self) -> int:
        return sum(w.width for w in self.wires())

    def pack(self) -> Wire:
        """Concatenate elements into a single bus wire (MSB-first).

        `Vec([a, b, c]).pack()` yields `{a, b, c}` in Verilog terms.
        """
        ws = self.wires()
        out_w = self.total_width
        if out_w <= 0:
            raise ValueError("cannot pack a zero-width Vec")

        m = ws[0].m
        acc = m.const_wire(0, width=out_w)
        lsb = 0
        for w in reversed(ws):
            part = w.zext(width=out_w)
            if lsb:
                part = part.shl(amount=lsb)
            acc = acc | part
            lsb += w.width
        return acc

    def unpack(self, packed: Wire) -> "Vec":
        """Extract elements from a packed bus (inverse of pack())."""
        ws = self.wires()
        if packed.width != self.total_width:
            raise ValueError(f"unpack width mismatch: got i{packed.width}, expected i{self.total_width}")

        parts_rev: list[Wire] = []
        lsb = 0
        for w in reversed(ws):
            parts_rev.append(packed.slice(lsb=lsb, width=w.width))
            lsb += w.width
        return Vec(tuple(reversed(parts_rev)))

    def regs_domain(self, domain: ClockDomain, en: Union[Wire, Signal, int], init: Union[Wire, Signal, int] = 0) -> "Vec":
        """Create a register per element and return a Vec of Regs."""
        ws = self.wires()
        m = ws[0].m
        if not isinstance(m, Circuit):
            raise TypeError("regs_domain requires elements to belong to a Circuit")
        regs: list[Reg] = []
        for w in ws:
            regs.append(m.reg_domain(domain, en, w, init))
        return Vec(tuple(regs))


@dataclass(frozen=True)
class Bundle:
    """A small named container (like a Verilog struct/bundle).

    Intended syntax:
      b = m.bundle(a=a, b=b)
      x = b["a"]
      packed = b.pack()
    """

    fields: dict[str, Union[Wire, Reg]]

    def __post_init__(self) -> None:
        if not self.fields:
            raise ValueError("Bundle cannot be empty")
        # Ensure all elements come from the same Module.
        vals = list(self.fields.values())
        m0 = Vec._module_of(vals[0])
        for v in vals[1:]:
            mv = Vec._module_of(v)
            if mv is not m0:
                raise ValueError("Bundle fields must belong to the same Circuit/Module")

    def __getitem__(self, key: str) -> Union[Wire, Reg]:
        return self.fields[str(key)]

    def items(self) -> Iterable[tuple[str, Union[Wire, Reg]]]:
        return self.fields.items()

    def pack(self) -> Wire:
        elems = tuple(self.fields.values())
        return Vec(elems).pack()

    def unpack(self, packed: Wire) -> "Bundle":
        """Extract fields from a packed bus (inverse of pack())."""
        elems = tuple(self.fields.values())
        vec = Vec(elems)
        parts = vec.unpack(packed)
        out: dict[str, Union[Wire, Reg]] = {}
        for (k, _), v in zip(self.fields.items(), parts.elems):
            out[k] = v
        return Bundle(out)


@dataclass(frozen=True)
class Pop:
    valid: Wire
    data: Wire
    fire: Wire


class Queue:
    """Queue-like wrapper over `pyc.fifo` (single-clock, strict ready/valid).

    Intended usage (event-ish):
      q = m.queue("q", domain=dom, width=8, depth=2)
      accepted = q.push(x, when=in_valid)
      p = q.pop(when=out_ready)
      # p.valid / p.data / p.fire
    """

    def __init__(self, m: Circuit, name: str, *, clk: Signal, rst: Signal, width: int, depth: int) -> None:
        self.m = m
        self.name = str(name)
        self.width = int(width)
        self.depth = int(depth)

        if self.width <= 0:
            raise ValueError("Queue width must be > 0")
        if self.depth <= 0:
            raise ValueError("Queue depth must be > 0")

        # Input placeholders driven by the high-level API (finalized before emit_mlir()).
        self._in_valid = m.named_wire(f"{self.name}__in_valid", width=1)
        self._in_data = m.named_wire(f"{self.name}__in_data", width=self.width)
        self._out_ready = m.named_wire(f"{self.name}__out_ready", width=1)

        # Underlying FIFO instance.
        in_ready, out_valid, out_data = m.fifo(clk, rst, in_valid=self._in_valid, in_data=self._in_data, out_ready=self._out_ready, depth=self.depth)
        self.in_ready = in_ready
        self.out_valid = out_valid
        self.out_data = out_data

        self._push_bound = False
        self._pop_bound = False
        self._push_valid_expr: Union[Wire, Reg, Signal, int] = 0
        self._push_data_expr: Union[Wire, Reg, Signal, int] = 0
        self._pop_ready_expr: Union[Wire, Reg, Signal, int] = 0

        # Defer assigns so we can keep single-driver semantics while supporting a push/pop API.
        m.add_finalizer(self._finalize)

    def push(self, data: Union[Wire, Reg, Signal, int], *, when: Union[Wire, Signal, int] = 1) -> Wire:
        if self._push_bound:
            raise ValueError("Queue.push() may only be called once per Queue instance (prototype limitation)")
        self._push_bound = True
        self._push_valid_expr = when
        self._push_data_expr = data
        # Fire when valid && ready.
        w_when = self._coerce_i1(when, ctx="queue push when")
        return w_when & self.in_ready

    def pop(self, *, when: Union[Wire, Signal, int] = 1) -> Pop:
        if self._pop_bound:
            raise ValueError("Queue.pop() may only be called once per Queue instance (prototype limitation)")
        self._pop_bound = True
        self._pop_ready_expr = when
        w_when = self._coerce_i1(when, ctx="queue pop when")
        fire = self.out_valid & w_when
        return Pop(valid=self.out_valid, data=self.out_data, fire=fire)

    def _finalize(self) -> None:
        # Defaults: drive inactive.
        m = self.m
        m.assign(self._in_valid, self._push_valid_expr)
        m.assign(self._in_data, self._push_data_expr)
        m.assign(self._out_ready, self._pop_ready_expr)

    def _coerce_i1(self, v: Union[Wire, Signal, int], *, ctx: str) -> Wire:
        if isinstance(v, Wire):
            if v.m is not self.m:
                raise ValueError("cannot combine wires from different modules")
            if v.ty != "i1":
                raise TypeError(f"{ctx}: expected i1, got {v.ty}")
            return v
        if isinstance(v, Signal):
            if v.ty != "i1":
                raise TypeError(f"{ctx}: expected i1, got {v.ty}")
            return Wire(self.m, v)
        if isinstance(v, int):
            return self.m.const_wire(int(v), width=1)
        raise TypeError(f"{ctx}: expected Wire/Signal/int, got {type(v).__name__}")
