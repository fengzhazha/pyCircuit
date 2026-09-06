# PyCircuit V6 编程教程（Tutorial）

版本：6.0

本教程通过一系列由浅入深的完整示例，教你用 PyCircuit V6 设计数字电路：从一个计数器开始，逐步覆盖流水线、层次化组合、标量重复结构、测试台编写与完整的构建/仿真流程。

**配套文档**：

- 语言定义 → `docs/v6_PyCircuit_Specification.md`
- 工具链架构 → `docs/v6_PyCircuit_Software_Architecture.md`

---

## 环境准备

### 安装

```bash
git clone https://github.com/PTO-ISA/pyCircuit.git
cd pyCircuit

# 安装 Python 前端（editable）
python3 -m pip install -e .

# 构建 pycc 后端工具链（需要已安装 LLVM/MLIR，见 docs/getting-started/installation.md）
bash flows/scripts/pyc build
# 产物在 .pycircuit_out/toolchain/install/bin/pycc
```

### 环境验证

```bash
export PYTHONPATH=$PWD/python/pycircuit/src:$PYTHONPATH
export PYC_TOOLCHAIN_ROOT=$PWD/.pycircuit_out/toolchain/install

python3 -c "import pycircuit; print('ok')"
python3 -m pycircuit.cli build examples/pycircuit/counter/tb_counter.py \
    --out-dir /tmp/pyc_counter --target cpp --jobs 8
```

如果最后一条命令能编译并跑通仿真，环境就绪。

---

## 第一个设计：计数器

新建 `counter.py`：

```python
from pycircuit import (
    CycleAwareCircuit, CycleAwareDomain,
    cas, compile_cycle_aware, wire_of,
)

def build(m: CycleAwareCircuit, domain: CycleAwareDomain, width: int = 8) -> None:
    # ① 输入端口：m.input() 返回裸 Wire，用 cas() 打上 cycle=0 标签
    enable = cas(domain, m.input("enable", width=1), cycle=0)

    # ② 前向声明一个寄存器：Q 端立即可读（cycle 0）
    count = domain.signal(width=width, reset_value=0, name="count")

    # ③ 输出：wire_of() 只在 m.output() 边界使用
    m.output("count", wire_of(count))

    # ④ 推进逻辑周期：下面的赋值发生在 cycle 1
    domain.next()

    # ⑤ 条件赋值：enable 为真时 count+1，否则保持
    count.assign(count + 1, when=enable)

build.__pycircuit_name__ = "counter"

if __name__ == "__main__":
    print(compile_cycle_aware(build, name="counter", eager=True, width=8).emit_mlir())
```

运行：

```bash
python3 counter.py
```

会打印出 MLIR，核心是一个 `pyc.reg`（寄存器）加上加法器和 mux。

**逐行解读关键点：**

1. **没有手写寄存器**。`domain.signal()` 声明了一个「未来会被赋值」的信号；在 `domain.next()`（周期 1）之后赋值，编译器发现「读在周期 0、写在周期 1」，自动推导出一级 DFF。
2. **`cas()` 是入口桥**：`m.input()` 返回的是裸 `Wire`，必须包上周期标签才能参与运算。
3. **`wire_of()` 是出口桥**：只有 `m.output()` 需要裸 `Wire`，其他任何地方都不要提取。

这三条就是 PyCircuit 的「类型纪律」，全书所有例子都遵守。

---

## 心智模型：周期感知信号

写 PyCircuit 时，你在脑中维护一条**时间线**：

```text
cycle 0          cycle 1          cycle 2
  │                │                │
  输入到达      寄存器更新       流水下一级
  组合逻辑      （<<= 赋值）
```

- 每个信号（`CycleAwareSignal`）都记着自己属于哪个周期。
- `domain.next()` 把「当前书写位置」推进一格——就像在时序图上向右移一列。
- **不同周期的信号相遇时，编译器自动补拍**：

```python
# a 在 cycle 0，b 在 cycle 2
r = a + b       # r 在 cycle 2；a 被自动延迟 2 拍（插 2 级 DFF）
```

这叫**自动周期平衡**。你只描述数据流的逻辑关系，寄存器对齐由编译器完成。

**读写周期差决定寄存器**：

| 声明后读（cycle N） | 赋值时（cycle M） | 结果 |
|---|---|---|
| N=0 | M=1 | 一级反馈寄存器（最常见） |
| N=0 | M=0 | 纯组合赋值 |
| N=0 | M=2 | 两级流水反馈 |
| — | M < N | 编译错误（不能向过去赋值） |

**一个必须内化的规则**：CAS 不能当 Python 布尔用。`if sig:` 是错的——硬件里没有「运行时 if」，条件逻辑用 `mux(cond, a, b)` 表达。Python 的 `if`/`for` 只用来做**元编程**（生成电路结构），在 `eager=True` 模式下它们在编译期展开。

---

## 组合逻辑与多路选择

做一个简单 ALU（纯组合，无状态）：

```python
from pycircuit import (
    CycleAwareCircuit, CycleAwareDomain,
    cas, compile_cycle_aware, mux, wire_of,
)

def mini_alu(m: CycleAwareCircuit, domain: CycleAwareDomain, width: int = 32) -> None:
    a  = cas(domain, m.input("a",  width=width), cycle=0)
    b  = cas(domain, m.input("b",  width=width), cycle=0)
    op = cas(domain, m.input("op", width=2),     cycle=0)

    add_r = a + b
    sub_r = a - b
    and_r = a & b
    or_r  = a | b

    # 级联 mux 实现 4 路选择（op: 00=add 01=sub 10=and 11=or）
    r01 = mux(op[0], sub_r, add_r)
    r23 = mux(op[0], or_r,  and_r)
    result = mux(op[1], r23, r01)

    # 标志位：切片、比较都直接在 CAS 上做
    zero = result == 0
    msb  = result[width - 1]

    m.output("result", wire_of(result))
    m.output("zero",   wire_of(zero))
    m.output("msb",    wire_of(msb))

mini_alu.__pycircuit_name__ = "mini_alu"
```

要点：

- 所有运算符（`+ - * & | ^ ~`、比较、切片 `x[i]` / `x[lo:hi]`、移位）都直接作用在 CAS 上，结果仍是 CAS。
- 宽度变换用 `trunc(x, width=w)` / `zext(x, width=w)` / `sext(x, width=w)`（函数式，从 `pycircuit` 导入）；有符号比较先 `x.as_signed()`。
- 整段代码没有 `domain.next()`——全部逻辑都在 cycle 0，输出即纯组合。

---

## 流水线：让编译器插入寄存器

设计一个两级流水乘加器：`out = (a * b) + c`，乘法一拍、加法一拍。

```python
def mac2(m: CycleAwareCircuit, domain: CycleAwareDomain, width: int = 16) -> None:
    a = cas(domain, m.input("a", width=width), cycle=0)
    b = cas(domain, m.input("b", width=width), cycle=0)
    c = cas(domain, m.input("c", width=width), cycle=0)

    # ── Stage 1：乘法结果打一拍 ──
    prod = domain.signal(width=width, name="prod")
    domain.next()                     # → cycle 1
    prod <<= a * b                    # 写 cycle 1、读 cycle 0 → 1 级寄存器

    # ── Stage 2：加法结果再打一拍 ──
    acc = domain.signal(width=width, name="acc")
    domain.next()                     # → cycle 2
    acc <<= prod + c                  # c 在 cycle 0，prod 在 cycle 1 →
                                      #   c 自动延迟 1 拍对齐（自动平衡！）

    m.output("out", wire_of(acc))
```

注意 `prod + c` 这一行：`c` 是 cycle 0 的输入，`prod` 是 cycle 1 的寄存器输出。编译器自动为 `c` 插入一级 DFF，两者在 cycle 1 相加，结果在 cycle 2 写入 `acc`。**你从头到尾没有写过任何「对齐寄存器」**——这正是周期感知模型的价值：改流水级数时，只动 `domain.next()` 的位置，所有旁路信号自动重新对齐。

补一个实用技巧：`domain.prev()` 可以回到上一列补写逻辑；`domain.cycle(sig)` 显式给某个信号打一拍。

---

## 编写测试台

给前文的计数器写测试。新建 `tb_counter.py`：

```python
from pycircuit import (
    CycleAwareCircuit, CycleAwareDomain, CycleAwareTb, Tb,
    cas, compile_cycle_aware, testbench, wire_of,
)
from counter import build   # 前文的计数器设计

@testbench
def tb(t: Tb) -> None:
    tb = CycleAwareTb(t)
    tb.clock("clk")
    tb.reset("rst", cycles_asserted=2, cycles_deasserted=1)
    tb.timeout(64)

    # cycle 0：不使能，计数保持 0
    tb.drive("enable", 0)
    tb.expect("count", 0)

    tb.next()                    # → cycle 1：使能
    tb.drive("enable", 1)
    tb.expect("count", 0)        # 寄存器要等下一个时钟沿才更新

    tb.next()                    # → cycle 2
    tb.expect("count", 1)

    tb.next()                    # → cycle 3
    tb.expect("count", 2)

    tb.next()
    tb.drive("enable", 0)        # 撤销使能
    tb.next()
    tb.expect("count", 3)        # 保持

    tb.finish()
```

测试台的心智模型与设计对称：设计里 `domain.next()` 推进设计时间线，测试里 `tb.next()` 推进激励时间线。

两个 `expect` 观测相位：

- `phase="post"`（默认）：时钟沿提交**之后**观测——看到的是更新后的寄存器值。
- `phase="pre"`：沿计算后、提交前观测——用于检查「即将写入」的值。

运行仿真（详见“从 Python 到 Verilog”）：

```bash
# 生成并编译 C++ 仿真器
python3 -m pycircuit.cli build tb_counter.py --out-dir /tmp/tb_counter --target cpp

# 运行：expect 全部通过则正常退出，失败则报错并返回非零码
/tmp/tb_counter/cpp_build/build/pyc_tb
```

**长测试用 sidecar**：当激励长达数万周期时，加 `--tb-schedule-mode=sidecar`，事件序列会外置为二进制文件，C++ 编译时间不随测试长度增长，且改激励不需要重编仿真器。

---

## 标准模块模板与双模运行

真实项目中的模块要既能**独立编译测试**，又能**被父模块组合**。标准模板：

```python
from pycircuit import (
    CycleAwareCircuit, CycleAwareDomain,
    cas, compile_cycle_aware, mux, submodule_input, wire_of,
)

def accumulator(
    m: CycleAwareCircuit,
    domain: CycleAwareDomain,
    *,
    inputs: dict | None = None,     # ← 双模开关
    width: int = 32,
    prefix: str = "acc",
) -> dict:
    _in = submodule_input

    # ── Step 1: 输入（独立模式创建端口；组合模式取父模块信号）──
    data_in = _in(inputs, "data_in", m, domain, prefix=prefix, width=width)
    valid   = _in(inputs, "valid",   m, domain, prefix=prefix, width=1)

    # ── Step 2: 状态 + 组合 ──
    acc = domain.signal(width=width, reset_value=0, name=f"{prefix}_acc")
    acc_next = mux(valid, acc + data_in, acc)

    # ── Step 3: 时序更新 ──
    domain.next()
    acc <<= acc_next

    # ── Step 4: 输出 dict（值必须是 CAS！）──
    outs = {"sum": acc, "sum_next": acc_next}

    # ── Step 5: 仅独立模式发射端口 ──
    if inputs is None:
        for k, v in outs.items():
            m.output(f"{prefix}_{k}", wire_of(v))

    return outs

accumulator.__pycircuit_name__ = "accumulator"

# ── Step 6: 独立编译入口 ──
if __name__ == "__main__":
    circ = compile_cycle_aware(accumulator, name="accumulator", eager=True, width=16)
    print(circ.emit_mlir())
```

| 模式 | 触发 | 输入来源 | 输出去向 |
|------|------|----------|----------|
| 独立 | `inputs=None` | `m.input(f"{prefix}_{key}")` | `m.output()` |
| 组合 | `inputs={...}` | `inputs[key]`（父模块 CAS） | 仅返回 dict |

三个高频错误提前打预防针：

1. **dict 值必须是 CAS**，不要 `outs["x"] = wire_of(x)`；
2. **key 必须与子模块完全一致**——拼错不会报错，而是静默多出一个端口；
3. **每个子模块实例给独立 prefix**，否则寄存器名冲突。

---

## 层次化组合：搭一个小 CPU

用 `domain.call()` 把模块组合成层次。三层结构：`soc_top` → `cpu_core` → `frontend` + `backend`。

```python
def frontend(m, domain, *, inputs=None, pc_width=32, prefix="fe") -> dict:
    _in = submodule_input
    redirect_valid  = _in(inputs, "redirect_valid",  m, domain, prefix=prefix, width=1)
    redirect_target = _in(inputs, "redirect_target", m, domain, prefix=prefix, width=pc_width)

    pc = domain.signal(width=pc_width, reset_value=0, name=f"{prefix}_pc")
    FOUR = cas(domain, u(pc_width, 4), cycle=0)
    next_pc = mux(redirect_valid, redirect_target, pc + FOUR)

    domain.next()
    pc <<= next_pc

    outs = {"pc": pc, "next_pc": next_pc}
    if inputs is None:
        m.output(f"{prefix}_pc", wire_of(pc))
    return outs

frontend.__pycircuit_name__ = "frontend"


def backend(m, domain, *, inputs=None, data_width=32, prefix="be") -> dict:
    _in = submodule_input
    op_a   = _in(inputs, "op_a",   m, domain, prefix=prefix, width=data_width)
    op_b   = _in(inputs, "op_b",   m, domain, prefix=prefix, width=data_width)
    alu_op = _in(inputs, "alu_op", m, domain, prefix=prefix, width=4)

    result = mux(alu_op[0], op_a - op_b, op_a + op_b)
    wb = domain.signal(width=data_width, name=f"{prefix}_wb")
    domain.next()
    wb <<= result

    outs = {"wb_data": wb, "result": result}
    if inputs is None:
        m.output(f"{prefix}_wb_data", wire_of(wb))
    return outs

backend.__pycircuit_name__ = "backend"


def cpu_core(m, domain, *, inputs=None, data_width=32, pc_width=32, prefix="cpu") -> dict:
    _in = submodule_input
    redirect = _in(inputs, "redirect", m, domain, prefix=prefix, width=1)
    target   = _in(inputs, "target",   m, domain, prefix=prefix, width=pc_width)

    # 子模块调用：inputs 的 key 与子模块 _in 的 key 一一对应
    fe = domain.call(frontend, inputs={
        "redirect_valid":  redirect,
        "redirect_target": target,
    }, pc_width=pc_width, prefix=f"{prefix}_fe")

    # 级联：frontend 输出直接喂给 backend
    be = domain.call(backend, inputs={
        "op_a":   fe["pc"],
        "op_b":   cas(domain, u(data_width, 0), cycle=0),
        "alu_op": cas(domain, u(4, 0), cycle=0),
    }, data_width=data_width, prefix=f"{prefix}_be")

    outs = {"pc": fe["pc"], "wb_data": be["wb_data"]}
    if inputs is None:
        m.output(f"{prefix}_pc",      wire_of(outs["pc"]))
        m.output(f"{prefix}_wb_data", wire_of(outs["wb_data"]))
    return outs

cpu_core.__pycircuit_name__ = "cpu_core"
```

**`domain.call()` 做了三件事：**

1. `push()` 保存父模块的周期计数器；
2. 执行子函数（子函数里随便 `domain.next()`）；
3. `pop()` 恢复——调用返回后 `domain.cycle_index` 与调用前完全相同。

所以多个子模块之间、子模块与父模块之间的周期计数**互不干扰**；但子模块**返回信号的 cycle 标签保留**（例如 `fe["pc"]` 带着它在 frontend 内部被赋值时的周期），父模块拿它继续运算时自动平衡照常工作。

**层次化编译**——保留模块边界到 MLIR 和 Verilog：

```python
circ = compile_cycle_aware(cpu_core, name="cpu_core", eager=True,
                           hierarchical=True)
mlir = circ.emit_mlir()
# → 多个 func.func（frontend / backend / cpu_core），
#   cpu_core 内部用 pyc.instance 引用子模块
```

不加 `hierarchical=True` 则全部内联为单一模块（扁平模式）。两种模式的取舍见架构文档；经验法则：**大设计用层次化**（增量编译、综合分区友好），**小模块单测用扁平**。

---

## 标量重复结构与高层聚合

canonical PYC 是 scalar-only：每个 `Wire` 承载一个 `Bits`、`Clock` 或
`Reset` 值。需要多 lane 或多 entry 时，用普通 Python list/tuple 与静态
`for` 展开标量结构：

```python
lanes = [m.input(f"lane_{i}", width=32) for i in range(8)]
biased = [lane + u(32, i) for i, lane in enumerate(lanes)]

any_nonzero = biased[0] != 0
for lane in biased[1:]:
    any_nonzero = any_nonzero | (lane != 0)
```

每个 list 元素保持普通标量 `Wire`，因此生成的 IR 是明确的
`pyc.add`/`pyc.cmp`/`pyc.or` SSA 图。没有 implicit broadcast、lane
instruction 或 backend vector runtime。

数组/tuple/struct/enum 作为**值类型**时由 Agentic Circuit 的递归
descriptor 与 ACIR 表达。进入 PYC 前按稳定 descriptor/source 顺序、
MSB-first 打包成一个精确宽度 scalar integer；字段和元素操作变成
`pyc.extract`、`pyc.concat` 与其他 scalar primitives。这样 Python 仍可用
class、tuple、list 和静态循环表达数据结构，而 C++/Verilog 后端只维护一套
标量语义。

需要优先选择或归约时，也直接对 Python list 生成确定性标量树。推荐按
升序元素顺序两两合并，并把奇数个节点的最后一个带到下一层，这与
Decision 0220 的 deterministic balanced-tree contract 一致。

## 存储、FIFO 与 CDC

寄存器堆 / RAM / 队列不要用 `domain.signal()` 数组硬堆（会展开成海量 mux），用内建原语：

```python
# 同步 1R1W 存储（读数据打一拍）→ pyc.sync_mem → Verilog pyc_sync_mem 原语
rdata = m.sync_mem(clk, rst,
                   ren=ren, raddr=raddr,
                   wvalid=wen, waddr=waddr, wdata=wdata, wstrb=strb,
                   depth=64, name="dcache_data")

# ready/valid FIFO → pyc.fifo
in_ready, out_valid, out_data = m.fifo(clk, rst,
                                       in_valid=iv, in_data=idata,
                                       out_ready=oready, depth=8)

# 跨时钟域：唯一合法通道是 CDC 原语
sync_bit = m.cdc_sync(dst_clk, dst_rst, src_bit, stages=2)
# 或整包数据走 m.async_fifo(...)
```

> **小容量、全并行读**的结构（如 8 项的重命名映射表）仍适合 `domain.signal()` 数组 + mux 树；**大容量、单端口访问**的结构必须用 mem 原语，综合工具才能映射成 SRAM/BRAM。

跨时钟域纪律：后端 `pyc-check-clock-domains` 会拒绝任何未经 `cdc_sync` / `async_fifo` 的跨域信号，这不是风格建议而是编译错误。

---

## 从 Python 到 Verilog

以下命令均在仓库根目录执行，假设已按“环境准备”设置好环境：

```bash
cd pyCircuit
export PYTHONPATH=$PWD/python/pycircuit/src:$PYTHONPATH
export PYC_TOOLCHAIN_ROOT=$PWD/.pycircuit_out/toolchain/install   # pycc 所在工具链
```

### 一键路径

`pycircuit build` 把「前端 emit → pycc → CMake 编译 C++ 仿真器 → Verilator」串成一条流水线。以仓库自带的计数器为例：

```bash
# 生成 RTL + C++ 仿真器 + Verilator 仿真器，并直接运行 Verilator 仿真
python3 -m pycircuit.cli build examples/pycircuit/counter/tb_counter.py \
    --out-dir /tmp/pyc_counter \
    --target both --jobs 8 \
    --logic-depth 64 \
    --run-verilator
```

| 标志 | 说明 |
|------|------|
| `--target cpp` | 只生成并编译 C++ 仿真器 |
| `--target verilator` | 只生成 Verilog + Verilator 仿真器 |
| `--target both`（默认） | 两者都做（可交叉比对） |
| `--run-verilator` | 构建后立即运行 Verilator 仿真（`--run-arg` 可传运行参数） |
| `--tb-schedule-mode sidecar` | 长测试改用 sidecar 外置激励 |
| `--param width=16` | 覆盖设计的 JIT 参数（可重复） |

**产物目录布局**（`--out-dir /tmp/pyc_counter`）：

```text
/tmp/pyc_counter/
├── device/verilog/           ← ★ RTL 输出（综合用）
│   ├── counter.v             #    每个模块一个 .v
│   ├── pyc_primitives.v      #    pyc_reg 等原语库
│   ├── manifest.json         #    文件清单
│   ├── compile_stats.json    #    寄存器/深度统计
│   └── yosys_synth.ys        #    现成的 Yosys 综合脚本
├── device/cpp/               ← C++ 仿真模型（pyc::gen::*）
├── tb/
│   ├── tb_counter.cpp        #    C++ 测试主程序
│   └── tb_counter.sv         #    SystemVerilog 测试台
├── cpp_build/build/pyc_tb    ← ★ C++ 仿真器可执行文件
└── verilator_build/Vtb_counter  ← ★ Verilator 仿真器可执行文件
```

**运行仿真**：

```bash
# C++ 周期精确仿真（构建完成后手动运行）
/tmp/pyc_counter/cpp_build/build/pyc_tb

# Verilator 仿真（--run-verilator 已自动跑过；也可手动重跑）
cd /tmp/pyc_counter && ./verilator_build/Vtb_counter
```

测试中的 `expect` 失败会报错并以非零码退出；全部通过则正常结束。VCD 波形按 `--trace-config` / 测试台配置生成在运行目录。

### 分步路径

#### Python → MLIR

在设计脚本里放标准 `__main__`：

```python
if __name__ == "__main__":
    import sys
    hier = "--hierarchical" in sys.argv
    circ = compile_cycle_aware(my_top, eager=True, name="my_top", hierarchical=hier)
    with open("my_top.mlir", "w") as f:
        f.write(circ.emit_mlir())
```

```bash
python3 my_top.py                  # 扁平 MLIR（子模块全部内联）
python3 my_top.py --hierarchical   # 层次化 MLIR（domain.call 边界保留为 func.func）
```

以前文的计数器为例，生成的 MLIR 长这样（节选）：

```mlir
func.func @counter(%clk: !pyc.clock, %rst: !pyc.reset, %enable: i1) -> i8 {
  %v1 = pyc.wire {pyc.name = "count__next"} : i8
  %v4 = pyc.reg %clk, %rst, %v2, %v1, %v3 : i8      // 推导出的反馈寄存器
  %v13 = pyc.add %v11, %v12 : i8, i8 -> i8
  %v14 = pyc.select %enable, %v13, %v5 : i1, i8, i8 -> i8
  pyc.assign %v1, %v14 : i8
  func.return %v5 : i8
}
```

#### MLIR → Verilog

```bash
PYCC=$PYC_TOOLCHAIN_ROOT/bin/pycc

# ── 层次化输出：每个子模块独立 .v + 原语库 + manifest + yosys 脚本 ──
$PYCC my_top.mlir --emit=verilog --hierarchical \
      --logic-depth=256 --out-dir=build_out/verilog_hier
# 产物: build_out/verilog_hier/{my_top.v, fetch.v, ..., pyc_primitives.v,
#        manifest.json, compile_stats.json, yosys_synth.ys}

# ── 扁平单文件输出：全部内联为一个 module ──
$PYCC my_top.mlir --emit=verilog --flatten \
      --logic-depth=256 -o build_out/my_top_flat.v

# ── FPGA 目标（在文件头加 `define PYC_TARGET_FPGA）──
$PYCC my_top.mlir --emit=verilog --target=fpga -o build_out/my_top_fpga.v
```

编译成功时 stderr 会打印资源统计并写出 `.stats.json`：

```text
stats: regs=42 (336 bits), mems=0 (0 bits), max_depth=7/256, WNS=249, TNS=0, fuse_comb=on
```

拿到 RTL 后可直接用发射器生成的脚本走 Yosys 综合冒烟：

```bash
cd build_out/verilog_hier && yosys -s yosys_synth.ys
```

#### MLIR → C++ 仿真模型

```bash
$PYCC my_top.mlir --emit=cpp --out-dir=build_out/cpp
# 产物: 每模块 .hpp/.cpp 分片 + cpp_compile_manifest.json（源列表/包含路径/运行时库）
```

生成的模型是 `pyc::gen::my_top` 结构体，配合 `library/cpp/pyc_tb.hpp` 的
`Testbench<Dut>` 即可手写 C++ 测试；日常更推荐让 `pycircuit build` 自动生成
测试主程序并完成 CMake 编译。

#### Verilog → Verilator 仿真

`pycircuit build --target verilator` 自动完成；等价的手动命令：

```bash
verilator --binary -Wall -Wno-fatal --timing --trace \
    --top-module tb_counter \
    --Mdir build_out/verilator_build \
    /tmp/pyc_counter/tb/tb_counter.sv \
    /tmp/pyc_counter/device/verilog/pyc_primitives.v \
    /tmp/pyc_counter/device/verilog/counter.v

./build_out/verilator_build/Vtb_counter     # 运行 RTL 仿真
```

同一份测试台同时驱动 C++ 模型（`tb/*.cpp`）与 RTL（`tb/*.sv`），
`--target both` 下两边结果可直接比对——这是工具链自带的等价性验证手段。

### 编译期门禁

`pycc` 流水线内置多道检查，失败即编译错误：

| 检查 | 抓什么 |
|------|--------|
| `--logic-depth=N`（默认 32） | 单个组合路径 op 数超限（近似时序预算；报 WNS/TNS） |
| 组合环检测 | 无寄存器切断的 wire 反馈环 |
| 时钟域检查 | 未经 CDC 原语的跨域信号 |
| 层次纪律 | 过大的内联函数、裸循环复制层次等 |

编译完成还会输出资源统计：

```text
stats: regs=1234 (45678 bits), mems=4 (131072 bits), max_depth=87/256, WNS=169, TNS=...
```

把 `max_depth` 和 regs/mems 纳入日常观察，是在综合之前控制 QoR 的第一道手段。

### 波形

C++ 仿真器支持 VCD：测试运行目录下生成 `.vcd`，用 GTKWave / Surfer 打开；大型设计可用 `setVcdWindow` 限制 dump 区间。

---

## 大型项目组织与调试

### 目录布局

```text
designs/my_soc/
├── common/parameters.py       # 全局参数（位宽、深度、端口数）
├── frontend/
│   ├── fetch/fetch.py         # 一个模块函数 = 一个文件
│   └── decode/decode.py
├── backend/
│   ├── scalar_exu/alu.py
│   └── scalar_rs/scalar_rs.py
├── soc_top.py                 # 顶层组合
└── tests/
    ├── unit/test_alu.py       # 每个模块独立编译 + CycleAwareTb
    └── integration/test_soc.py
```

原则：

- 每个模块函数满足“标准模块模板与双模运行”的约定 → 每个模块可独立编译、独立测试；
- 集成自底向上：先单测 `alu`，再单测 `scalar_rs`，最后 `soc_top` 集成测试；
- 参数集中管理，模块通过 keyword-only 配置参数接收。

（大规模范例：Davinci 乱序处理器核，27 个模块层次化组合，见 [DavinciOO 仓库](https://github.com/hengliao1972/DavinciOO)。）

### 调试清单

| 症状 | 常见原因 | 手段 |
|------|----------|------|
| 输出恒 0 / 逻辑消失 | 独立模式忘记 `m.output()`，被死代码消除 | 检查 `if inputs is None` 分支 |
| 多出意外端口 | `inputs` dict key 拼写与子模块不一致 | 对照子模块 `_in(...)` 的 key |
| 结果晚到 N 拍 | 自动平衡插了对齐 DFF | 打印 `sig.cycle` 检查各信号周期 |
| 编译报「向过去赋值」 | `<<=` 时写周期 < 声明周期 | 检查 `domain.next()` 位置 |
| logic-depth 超限 | 链式归约 / 长 mux 链 | 归约改 `mode="tree"`；中间插 `domain.cycle()` 打拍 |
| 波形对不上预期 | pre/post 观测相位混淆 | `expect(..., phase=)` 与波形沿对齐 |

实用技巧：

- **给关键信号命名**：`(a + b).named("sum_ab")` → MLIR/Verilog/波形中出现该名字；
- **打印周期**：调试自动平衡时 `print(sig.cycle)` 立即可见时间线；
- **每级流水加注释横幅**（`# === Stage 2: Decode ===` + `domain.next()`），代码即时序图。

---

## 下一步

- 完整语言定义（`Data` 类型体系 / `Wire[DT]` / MLIR 映射的权威语义）：`docs/v6_PyCircuit_Specification.md`
- 3D 堆叠分层标注（`tier=` / `jump_tier`，Proposed）：`docs/v6_PyCircuit_Specification.md` 的“Tier 分层标注”与 `docs/rfcs/tier_annotation.md`
- 工具链内部（pyc 方言、pass 流水线、双发射器、sidecar 运行时）：`docs/v6_PyCircuit_Software_Architecture.md`
- 仓库内可运行示例：`examples/pycircuit/`（counter、calculator、fifo_loopback…）、`designs/blocks/BypassUnit`（标量 lane 列表）、`designs/blocks/IssueQueue`（标量重复结构 + 复杂状态）

---

**Copyright © 2024-2026 Liao Heng / PyCircuit Contributors. All rights reserved.**
