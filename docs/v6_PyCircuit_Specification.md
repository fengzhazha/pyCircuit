# PyCircuit V6 语言规范（Specification）

版本：6.0

**适用代码基线**：`pyCircuit` 主仓库 `main` 分支（含标量 `Data` 信号体系、Sidecar 测试调度、层次化 MLIR 发射）。

> **包名**：Python 包为 **`pycircuit`**（全小写）。不要使用 `pyCircuit` 作为 import 目标。
>
> **当前契约**：V6 以周期感知（Cycle-Aware）信号模型与层次化组合为正式语言设计，并统一类型化数据体系、Sidecar 测试调度、双编译路径以及存储 / FIFO / CDC 原语。Decision 0148 取代了早期移除全局周期感知模型的方向。
>
> **Agentic Circuit 边界**：Agentic Circuit 已切换到 epoch `0.5`，Python 以简单的 `@ac.rule` 作为唯一显式调度边界，MLIR pass 负责类型/effect 推导、检查、握手、调度与 marker 消除。Decision 0163 已贯通首个“一张 Table、一个 Queue 输入/输出、一次完整 Entry replace”的 stateful rule 到 QueueGraph/gfsim；Table 仍是 provisional state，PYC/RTL 必须在明确边界拒绝。该迁移不改变 CycleAwareSignal 或 PYC 的语义。

---

## 概述与设计哲学

PyCircuit 是嵌入在 Python 中的硬件描述语言（HDL）。设计者用普通 Python 函数描述电路，前端将其编译为 MLIR（`pyc` 方言），后端 `pycc` 再生成可综合 Verilog 与周期精确的 C++ 仿真模型。

V6 的核心概念：

- **周期感知信号（`CycleAwareSignal`，简称 CAS）**：设计中流动的唯一信号类型。每个信号携带「逻辑周期」标签，编译器根据周期差**自动插入 DFF** 完成流水线对齐。
- **前向声明寄存器（`domain.signal()` + `<<=`）**：先声明后赋值，读写周期差决定寄存器级数——寄存器是**推导**出来的，不是手写的。
- **层次化组合（`domain.call()`）**：Python 函数调用链即硬件模块层次；每个模块既能独立编译，又能被父模块组合。
- **标量类型体系（`Data` / `Wire[DT]`）**：`Bits` / `Clock` / `Reset` 由统一的 `Wire[DT]` 句柄承载。普通 Python list/tuple 用于静态重复结构；canonical PYC 不包含向量值或隐式逐元素运算。
- **周期精确仿真**：同一 MLIR 同时生成 Verilog 和 C++ 仿真器，测试台（`CycleAwareTb`）用与设计对称的 `next()` 模型编写。

### 标准导入

```python
from pycircuit import (
    # ── V6 核心（周期感知模型）──
    CycleAwareCircuit,    # 顶层电路
    CycleAwareDomain,     # 周期感知时钟域
    CycleAwareSignal,     # 周期感知信号（唯一信号类型）
    ForwardSignal,        # domain.signal() 的返回类型
    cas,                  # Wire → CycleAwareSignal
    compile_cycle_aware,  # V6 编译入口
    mux,                  # 多路选择器
    submodule_input,      # 双模输入辅助
    wire_of,              # 边界提取 Wire（仅用于 m.output()）
    # ── 测试 ──
    Tb, CycleAwareTb, testbench,
    # ── 位操作辅助 ──
    cat, zext, sext, trunc,
)
```

---

## 信号类型纪律

PyCircuit V6 强制单一信号类型。以下规则**不可违反**：

| 规则 | 说明 |
|------|------|
| **所有标量信号都是 `CycleAwareSignal`** | 设计中流动的每个标量值必须是 `CycleAwareSignal`（或 `ForwardSignal`，其读侧委托给 CAS）。 |
| **`domain.state()` 不存在** | 创建寄存器的唯一方式是 `domain.signal()` + `<<=` / `.assign()`。 |
| **`.wire` / `.w` 不可访问** | 所有算术、比较、mux、切片直接在 CAS 上进行。设计代码中读取 `.wire` 是错误。 |
| **`wire_of()` 是唯一的 Wire 提取方式** | 仅在 `m.output()` 边界调用。 |
| **`cas()` 包装裸 Wire** | `m.input()` 和 `m.const` 助手返回裸 `Wire`，必须用 `cas(domain, w, cycle=0)` 或 `submodule_input()` 包装后才能参与 CAS 表达式。`u(w,v)` / `s(w,v)` 字面量是 `LiteralValue`，CAS 运算符 / `mux()` / `cas()` 可直接消费，无需手动包装。 |
| **输出 dict 存 CAS** | 子模块返回的 dict 值必须是 `CycleAwareSignal`（保留 cycle 来源信息）。 |
| **重复结构用 Python 容器** | 多 lane 或多 entry 结构使用普通 Python list/tuple 和静态循环生成标量信号；typed aggregate 由 Agentic/ACIR 在 PYC 前打包。 |

```python
# ✅ 正确
pc = domain.signal(width=64, reset_value=0, name="pc")
enable = cas(domain, m.input("en", width=1), cycle=0)
result = pc + enable            # CAS + CAS → CAS（自动周期对齐）
m.output("pc", wire_of(pc))     # wire_of() 仅在边界

# ❌ 禁止
st = domain.state(...)          # 不存在
raw = pc.wire                   # 不可访问
outs["result"] = wire_of(r)     # dict 里必须存 CAS
x = a.wire + b.wire             # 直接在 CAS 上运算
```

---

## 核心类型与数据类型体系

PyCircuit 的 canonical PYC 数据路径是 scalar-only。`Data` 描述标量位宽、
时钟或复位语义，`Wire[DT]` 承载一个标量 SSA、端口或状态值。

### Data 类型层级

```text
Data (ABC)
├── Bits(bitwidth: int)  → iN
├── Clock                → !pyc.clock
└── Reset                → !pyc.reset
```

递归 struct/enum/tuple/fixed array 属于 semantic-core 与 ACIR 的高层
aggregate contract。它们按 descriptor/source 顺序、MSB-first 打包成一个
精确宽度 scalar integer 后才进入 canonical PYC。拓扑集合 `!ac.array` 与
persistent Python list 也不构成 PYC 向量值。

### Wire[DT] —— 标量信号句柄

`Wire` 只承载一个标量 `Bits`、`Clock` 或 `Reset`。算术、逻辑、比较、
选择、位片段与移位都生成 scalar PYC operation。重复结构直接使用普通
Python list/tuple 与静态循环：

```python
lanes = [m.input(f"lane_{i}", width=8) for i in range(4)]
biased = [lane + 1 for lane in lanes]
any_set = biased[0] != 0
for lane in biased[1:]:
    any_set = any_set | (lane != 0)
```

这段代码生成四组明确的 scalar SSA，而不是一个隐式逐 lane instruction。

### CycleAwareCircuit

顶层电路对象，`Circuit` 的子类。

```python
m = CycleAwareCircuit("my_circuit")
```

| 方法 | 说明 |
|------|------|
| `create_domain(name, *, frequency_desc="", reset_active_high=False)` | 创建 `CycleAwareDomain` |
| `input(name, *, width, signed=False)` | 标量输入端口，返回 `Wire[Bits]`（需 `cas()` 包装后参与周期感知运算） |
| `output(name, value)` | 注册标量输出端口（周期感知信号通过 `wire_of(sig)` 提取） |
| `const(value, *, width)` | 常量 `Wire`（用 `cas()` 包装后参与 CAS 表达式） |
| `cat(parts)` | 将若干标量按 MSB-first 拼成一个 packed `Wire[Bits]` |
| `emit_mlir()` | 生成 MLIR 文本。层次化编译时输出含所有子模块的多模块 `Design` |

### CycleAwareDomain

管理一个时钟域的逻辑周期状态。

```python
domain = m.create_domain("clk")
```

| 方法 | 说明 |
|------|------|
| `signal(*, width, reset_value=0, name="")` | **前向声明标量寄存器**——创建状态的唯一方式；返回 `ForwardSignal` |
| `cycle(sig, reset_value=None, name="")` | 对信号插入单级 DFF，返回延后一拍的 CAS |
| `next()` / `prev()` | 推进 / 回退当前逻辑周期 |
| `push()` / `pop()` | 周期计数器压栈 / 出栈（必须配对） |
| `call(fn, *, inputs=None, **kwargs)` | 调用子模块并自动 push/pop 隔离周期。扁平模式内联；层次化模式发射 `pyc.instance` |
| `delay_to(w, *, from_cycle, to_cycle, width)` | 显式打拍对齐（自动平衡的底层机制） |
| `create_signal(name, *, width, signed=False)` | 创建标量输入端口（裸 `Wire`） |
| `create_const(value, *, width, name="", signed=False)` | 常量 `Wire` |
| `create_reset()` | 复位信号（有效高视图，i1 `Wire`） |
| `cycle_index` | 属性：当前逻辑周期索引 |

多时钟域：

```python
cpu_clk = m.create_domain("CPU_CLK", frequency_desc="100MHz")
rtc_clk = m.create_domain("RTC_CLK", frequency_desc="1Hz")
```

跨时钟域信号**必须**经显式 CDC 原语（`cdc_sync` / `async_fifo`）传递；后端 `pyc-check-clock-domains` 检查违例并报错。

### CycleAwareSignal

唯一的标量信号类型。包含底层线网（内部管理）、`cycle`、`domain`。

**属性**：`cycle`、`domain`、`name`、`signed`。

**运算符**（所有输入输出均为 CAS，自动周期对齐）：

```python
r = a + b;  r = a - b;  r = a * b          # 算术
r = a & b;  r = a | b;  r = a ^ b;  r = ~a  # 位运算
r = a == b; r = a != b                      # 比较
r = a < b;  r = a > b;  r = a <= b; r = a >= b
r = a << k; r = a >> k                      # 移位
low = data[0:8];  bit5 = data[5]            # 切片 / 索引
```

**方法**：

| 方法 | 说明 |
|------|------|
| `select(t, f)` | 条件选择（等价 `mux(self, t, f)`） |
| `trunc(w)` / `zext(w)` / `sext(w)` | 宽度变换 |
| `slice(high, low)` | 位片段 |
| `named(name)` | 调试名称 |
| `as_signed()` / `as_unsigned()` | 符号标记（影响比较与右移语义） |

> CAS **禁止**作为 Python `bool` 使用（`if sig:` 报错）——硬件条件必须用 `mux` / `select` 表达。

### ForwardSignal

`domain.signal()` 的返回类型。读侧行为与 CAS 完全一致；额外提供写侧接口：

```python
sig <<= expr                     # 无条件赋值（连接 D 端）
sig.assign(expr, when=cond)      # 条件赋值（寄存器使能）
```

---

## Forward Signal 与寄存器推导

**核心思想：先声明后赋值，编译器根据读写周期差推导寄存器。**

```python
# 1. 声明（cycle 0）：Q 端立即可读
counter = domain.signal(width=8, reset_value=0, name="counter")

# 2. 组合逻辑（cycle 0）
count_next = mux(enable, counter + 1, counter)
m.output("count", wire_of(counter))

# 3. 推进周期
domain.next()                    # → cycle 1

# 4. 赋值：写周期 1 > 读周期 0 → 推导出一级反馈寄存器
counter <<= count_next
```

### 周期推导规则

| 读周期 | 写周期 | 推导结果 |
|--------|--------|----------|
| 0 | 1 | 一级反馈寄存器（DFF）——最常见 |
| 0 | 0 | 组合赋值（无寄存器） |
| 0 | N≥2 | N 级流水反馈（罕见，需明确意图） |
| N | < N | **编译错误**：不能向过去赋值 |

---

## 全局函数

### cas()

```python
x = cas(domain, m.input("x", width=8), cycle=0)
```

将裸 `Wire`（来自 `m.input()` 或 `u(w,v)` 字面量）包装为 CAS。是端口层与 CAS 类型系统之间的唯一桥梁。

### mux()

```python
result = mux(condition, true_value, false_value)
```

三个参数为 CAS（或 int 字面量），返回 CAS，自动周期对齐。裸 `Wire`
操作数也必须是标量，生成一个 `pyc.select`。

### wire_of()

```python
m.output("result", wire_of(outs["result"]))
```

从 CAS / `ForwardSignal` / 裸 `Wire` / `Reg` 中提取裸 `Wire`。**仅**允许出现在 `m.output()` 调用中。

### submodule_input()

双模输入解析：

```python
pc = submodule_input(inputs, "pc", m, domain, prefix="fe", width=32)
```

| `inputs` 状态 | 行为 |
|---------------|------|
| `None`（独立模式） | 创建 `m.input(f"{prefix}_{key}", width=W)` 并 `cas()` 包装 |
| dict 含 `key` | 直接返回 `inputs[key]`（父模块 CAS，cycle 保留） |
| dict 不含 `key` | 回退创建端口（通常是 key 拼写错误的征兆——务必传全） |

参数：`io, key, m, domain, *, prefix, width, cycle=0`。

---

## 周期管理与自动周期平衡

### next() / prev()

```python
domain.next()   # 逻辑周期 +1，标记时序分界点
domain.prev()   # 回退，用于补充同周期逻辑
```

### push() / pop() / call()

`domain.call()` 自动包裹 push/pop，保证**周期隔离**：

```python
print(domain.cycle_index)          # 0
domain.next()                      # → 1
child_out = domain.call(child_fn, inputs={...})   # 子函数内部任意 next()
print(domain.cycle_index)          # 1 ← 恢复，不受子函数影响
```

隔离由 push/pop 栈 + `try/finally` 保证（子函数抛异常也会恢复）。**注意**：隔离的是父函数的周期计数器，**不改变**返回信号自身携带的 cycle 值（cycle provenance）。

### 自动周期平衡

组合不同周期的信号时：**输出周期 = max(输入周期)**，较早的信号自动插入 DFF 延迟链。

```python
# sig_a 在 cycle 0，sig_b 在 cycle 2
result = sig_a + sig_b
# → result 在 cycle 2；sig_a 自动延迟 2 拍
```

生成的 MLIR：

```mlir
%a_d1 = pyc.reg %clk, %rst, %en, %a,    %init : i8
%a_d2 = pyc.reg %clk, %rst, %en, %a_d1, %init : i8
%r    = pyc.add %a_d2, %b : i8
```

---

## 模块签名与层次化组合

### 标准模块签名

每个 V6 模块是一个普通 Python 函数：

```python
def my_module(
    m: CycleAwareCircuit,          # ① 共享电路对象
    domain: CycleAwareDomain,      # ② 共享时钟域
    *,
    inputs: dict | None = None,    # ③ None=独立模式；dict=被组合
    width: int = 64,               # ④ 配置参数（keyword-only）
    prefix: str = "mod",           # ⑤ 端口/寄存器名前缀
) -> dict:                          # ⑥ 输出信号字典（值为 CAS）
    ...

my_module.__pycircuit_name__ = "my_module"   # 注册 RTL 模块名
```

### 双模运行

| 模式 | `inputs` | 输入 | 输出 | 用途 |
|------|----------|------|------|------|
| 独立 | `None` | `m.input()` 创建端口 | `m.output()` 发射端口 | 单元测试 / 独立综合 |
| 组合 | `{...}` | 从父模块 dict 读取 CAS | 仅返回 dict | 集成到父模块 |

### 子模块调用六步法

1. **声明自身输入**：`_in = submodule_input; a = _in(inputs, "a", m, domain, prefix=prefix, width=W)`
2. **构造子模块 inputs dict**：key 必须与子模块 `submodule_input()` 的 key **完全一致**，值必须是 CAS
3. **调用**：`child_out = domain.call(child_fn, inputs={...}, **config, prefix=f"{prefix}_ch")`
4. **读取输出**：`child_out["result"]`——CAS，cycle 保留子模块内部赋值时的值
5. **级联**：前一个子模块的输出直接作为下一个子模块的输入
6. **顶层收集**：`if inputs is None: m.output(f"{prefix}_{k}", wire_of(v))`

### 命名约定

前缀层次级联，避免冲突：

| 元素 | 模式 | 示例 |
|------|------|------|
| 输入 / 输出端口 | `{prefix}_{name}` | `fe_bpu_pc` |
| 状态寄存器 | `{prefix}_{name}` | `fe_fetch_pc` |
| 子模块前缀 | `{parent_prefix}_{child}` | `soc_cpu_fe` |

### 常见错误

| 错误 | 后果 | 纠正 |
|------|------|------|
| dict 值传 `Wire` 而非 CAS | 丢失 cycle 信息 / 类型错误 | 先 `cas()` 包装 |
| 输出 dict 存 `wire_of(x)` | 父模块无法周期对齐 | 存 CAS 本体 |
| key 拼写不匹配 | 静默创建多余端口 | key 与子模块 `_in` 完全一致 |
| 子模块共用 prefix | 端口 / 寄存器名冲突 | 每个 call 独立 prefix |
| 独立模式忘记 `m.output()` | 逻辑被 DCE 删光 | `if inputs is None:` 分支发射输出 |

### 层次化 MLIR 发射

```python
# 扁平（默认）：单一 func.func
circ = compile_cycle_aware(top, eager=True, name="top")

# 层次化：每个 domain.call() 边界保留为独立模块
circ = compile_cycle_aware(top, eager=True, name="top", hierarchical=True)
```

层次化模式下每个子模块编译为独立 `func.func`，父模块发射 `pyc.instance` 引用；输出的 MLIR 为多模块 `Design`（`module attributes {pyc.top = @top}`）。子模块内部的 `domain.call()` 递归处理。

---

## 存储 / FIFO / CDC 原语

`Circuit`（`CycleAwareCircuit` 继承）提供以下原语，直接映射到 `pyc` 方言 op，并由后端提供匹配的 Verilog 原语模块与 C++ 模型：

| 前端方法 | MLIR op | 语义 |
|----------|---------|------|
| `m.fifo(...)` | `pyc.fifo` | 单时钟 ready/valid FIFO（attr `depth`） |
| `m.byte_mem(...)` | `pyc.byte_mem` | 异步读、同步写、字节使能存储 |
| `m.sync_mem(...)` | `pyc.sync_mem` | 同步 1R1W（读数据打拍） |
| `m.sync_mem_dp(...)` | `pyc.sync_mem_dp` | 同步 2R1W |
| `m.async_fifo(...)` | `pyc.async_fifo` | 双时钟异步 FIFO |
| `m.cdc_sync(...)` | `pyc.cdc_sync` | 多级同步器（attr `stages`，默认 2） |
| `m.rv_queue(...)` | 组合原语 | ready/valid 队列；`pop()` 返回 `Pop(valid, data, fire)` |

跨时钟域数据**必须**经 `cdc_sync` / `async_fifo`；`pyc-check-clock-domains` 强制检查。

---

## 仿真与测试

### Tb（周期编号模型）

```python
from pycircuit import Tb, testbench

@testbench
def tb(t: Tb) -> None:
    t.clock("clk")
    t.reset("rst", cycles_asserted=2, cycles_deasserted=1)
    t.timeout(64)
    t.drive("enable", 1, at=1)
    t.expect("count", 1, at=2, phase="post")
    t.finish(at=10)
```

- `phase="pre"`：时钟沿计算后、提交前观测（TICK-OBS）
- `phase="post"`（默认）：提交后观测（XFER-OBS）
- 其余 API：`print` / `print_every` / `sva_assert` / `random`

### CycleAwareTb（隐式周期推进）

将 `at=cycle` 替换为与设计对称的 `tb.next()`：

```python
from pycircuit import CycleAwareTb, Tb, testbench

@testbench
def tb(t: Tb) -> None:
    tb = CycleAwareTb(t)
    tb.clock("clk")
    tb.reset("rst", cycles_asserted=2, cycles_deasserted=1)
    tb.timeout(64)

    tb.drive("enable", 0)
    tb.expect("count", 0)

    tb.next()                  # → cycle 1
    tb.drive("enable", 1)
    tb.expect("count", 0)      # 时钟沿后才更新

    tb.next()                  # → cycle 2
    tb.expect("count", 1)
    tb.finish()
```

| 方法 | 说明 |
|------|------|
| `CycleAwareTb(t)` | 包装 `Tb` |
| `next()` / `cycle` | 推进 / 读当前周期 |
| `drive(port, value)` | 当前周期驱动 |
| `expect(port, value, *, phase="post", msg=None)` | 当前周期检查 |
| `finish(*, at=None)` / `print(...)` / `timeout(n)` | 结束 / 打印 / 超时 |
| `clock` / `reset` / `sva_assert` / `random` | 透传 `Tb` |

### 测试调度：inline 与 sidecar

测试事件（drive/expect）有两种下发方式（`pycircuit build --tb-schedule-mode {inline,sidecar}`）：

| 模式 | 机制 | 适用 |
|------|------|------|
| `inline`（默认） | 事件编入生成的 C++ 源码 | 短测试 |
| `sidecar` | 事件序列化为二进制 **SIDECAR 容器**，运行时由稳定 runner 加载 | 长测试（避免 C++ 编译膨胀；改激励不必重编） |

Sidecar 容器（魔数 `SIDECAR\n`）含五个 section：`string_table`、`port_table`、`event_table`、`frame_table`、`pattern_table`。周期性反压（如 ready 口按固定模式起伏）自动压缩为 `periodic_drive` pattern。

检查工具：

```bash
pycircuit sidecar inspect out/tb.sidecar        # 打印容器内容
pycircuit sidecar verify  out/tb.sidecar        # 校验结构
```

---

## 编译入口

### compile_cycle_aware()（V6 主路径）

```python
def compile_cycle_aware(
    fn,
    *,
    name: str | None = None,       # 模块名
    domain_name: str = "clk",      # 时钟域名
    eager: bool = False,           # True=直接执行 fn；False=JIT 追踪
    hierarchical: bool = False,    # True=保留 domain.call() 边界（需 eager=True）
    **jit_params,                  # 转发给 fn 的配置参数
)
```

```python
circ = compile_cycle_aware(my_module, name="my_module", eager=True, width=16)
mlir_text = circ.emit_mlir()
```

- `eager=True`：直接执行 Python 函数体，即时构图。**推荐路径**。支持任意 Python 控制流（作为元编程展开）。
- `eager=False`（JIT）：AST 解析 fn，不执行；支持把 Python `if`（i1 条件）编译为 `scf.if` → mux，`for`（静态可迭代）展开。有原型级限制。
- `hierarchical=True`：见“层次化 MLIR 发射”。

### @module JIT 路径（结构化库接口）

```python
from pycircuit import module, compile, Circuit

@module
def build(m: Circuit): ...

design = compile(build)
```

`@module` 边界产生 `pyc.instance`；`@function` 内联；`@const` 为编译期纯元编程（禁发 IR）。`pycircuit emit` / `pycircuit build` CLI 走此入口。

### CLI

```bash
# 生成 MLIR
pycircuit emit design.py -o design.pyc [--param k=v ...]

# 一键构建 + 仿真（多 .pyc → pycc → CMake/Verilator）
pycircuit build tb_design.py --out-dir build/ \
    [--target {cpp,verilator,both}] [--jobs N] [--profile {dev,release}] \
    [--logic-depth N] [--tb-schedule-mode {inline,sidecar}] \
    [--run-verilator] [--param k=v ...]

# Sidecar 工具
pycircuit sidecar inspect FILE [--strict]
pycircuit sidecar verify FILE
```

---

## MLIR 映射参考

### 类型

| Python | MLIR |
| --- | --- |
| `Bits(W)` | `iW` |
| Clock | `!pyc.clock` |
| Reset | `!pyc.reset` |
| ACIR aggregate payload | 进入 PYC 前按 descriptor 打包为精确宽度 `iW` |

### 运算

| Python | MLIR |
| --- | --- |
| arithmetic | `pyc.add/sub/mul/udiv/urem/sdiv/srem` |
| bitwise | `pyc.and/or/xor/not` |
| compare | `pyc.cmp` with predicate `eq`, `ult`, or `slt` |
| `mux(c, a, b)` | `pyc.select` |
| cast/extract/shift/concat | 对应 scalar PYC primitive |

### Aggregate lowering boundary

ACIR `!ac.struct`、`!ac.enum`、builtin tuple 与 `!ac.value_array` 在
QueueGraph-to-PYC 中按稳定 MSB-first layout 变成 scalar integer。
canonical/backend PYC 中出现 builtin vector type 或 `pyc.v_*` 是硬错误。

## Tier 分层标注（3D 堆叠扩展，Proposed）

> **状态:Proposed**(尚未实现;完整提案与实现草图见 `docs/rfcs/tier_annotation.md`)。本节先行纳入规范,冻结语法形态与语义边界。

面向 3D 堆叠(细粒度逻辑折叠:设计折叠到 2–3 层垂直堆叠的裸片)的源码级层指派。信号携带第二种元数据 **`.tier`**(裸片层号),与 `.cycle` 并列。

**术语纪律:分层维度一律用 tier(裸片层),不用 layer**(后者指金属布线层)。

### 与 `.cycle` 的语义对照

| | `.cycle` | `.tier` |
|---|---|---|
| 语义地位 | **行为语义**,切错位置行为就变 | **物理提示**,不产生/不修改任何硬件 |
| 传播规则 | max 规则 + 自动周期平衡(插 DFF) | 继承规则,零电路效应 |
| 验证影响 | 必须过功能等价验证 | 逻辑恒等,功能验证无感 |

两套传播机制共存于同一次 elaboration,互不干扰。

### 语法

```python
a  = cas(domain, m.input("a", width=8), tier=0)          # 定义处显式
pc = domain.signal(width=64, name="pc", tier=0)           # 前向声明处显式
s1 = a + 1                                                # 隐式:继承输入的 tier
s2 = jump_tier(s1 * 3, to=1)                              # 强制跳层(声明一个键合点)
hot = cas(domain, m.input("b", width=8), tier=1,
          tier_lock=True)                                 # 锁定:下游 EDA 不得改写
tmp = cas(domain, m.input("c", width=8))                  # 未指派:tier=None,EDA 全权
outs = domain.call(alu, inputs={...}, tier=1)             # 模块级缺省 tier
```

### 语义规则

1. **传播:** 运算结果的 tier 由输入继承(全部/多数同层继承之;混层取主导方向),强度记为推断;
2. **反馈信号:** `domain.signal()` 的 tier 在声明处确定,`<<=` 赋值不改变它;跨层反馈须用 `jump_tier` 显式表达;
3. **自动周期平衡插入的对齐 DFF 继承驱动信号的 tier**(平衡不引入额外跨层);
4. **`jump_tier(expr, to=k)`:** 返回新 CAS,`.tier == k`、`.cycle` 不变、零硬件效应;`to` 须为编译期常量;
5. **强度三态与 EDA 契约:** free(未指派,分割器全权)/ hint(显式与推断,可改写但必须输出结构化 diff)/ locked(必须服从,不可满足报错)。分割器结果写入以稳定 ID 为键的 sidecar tier 表,不回写源码。

### IR 与发射

- MLIR 可选属性:`pyc.tier`(int)、`pyc.tier_strength`(`"hint"|"locked"`)、模块级 `pyc.tier_default`;无标注即无属性,**完全向后兼容**;
- Verilog 三条冗余通道:`(* pyc_tier = 1 *)` 属性、层次化命名编码、sidecar tier 指派表(主通道);三者不一致构成流程告警;
- C++ 仿真后端忽略 tier(功能无关),可选地在 DFX 元数据中携带以便按层聚合统计。

## API 参考表

### CycleAwareCircuit

| 方法 | 说明 |
|------|------|
| `CycleAwareCircuit(name)` | 创建顶层电路 |
| `create_domain(name, ...)` | 时钟域 |
| `input(name, *, width, signed=False)` | 标量输入端口（`Wire[Bits]`） |
| `output(name, value)` | 输出端口 |
| `const(value, *, width)` | 常量 |
| `cat(parts)` | scalar bit 拼接 |
| `fifo / byte_mem / sync_mem / sync_mem_dp / async_fifo / cdc_sync / rv_queue` | 原语 |
| `emit_mlir()` | 导出 MLIR |

### CycleAwareDomain

| 方法 | 说明 |
|------|------|
| `signal(*, width, reset_value=0, name="")` | 前向声明寄存器（唯一方式） |
| `cycle(sig, ...)` | 单级 DFF |
| `next()` / `prev()` | 推进 / 回退周期 |
| `push()` / `pop()` | 周期栈 |
| `call(fn, *, inputs=None, **kwargs)` | 子模块调用（自动隔离） |
| `delay_to(w, *, from_cycle, to_cycle, width)` | 显式打拍 |
| `create_signal / create_const / create_reset` | 端口 / 常量 / 复位 |
| `cycle_index` | 当前逻辑周期 |

### 全局函数

| 函数 | 说明 |
|------|------|
| `cas(domain, wire, cycle=N)` | Wire → CAS |
| `mux(cond, t, f)` | 多路选择（自动对齐） |
| `submodule_input(io, key, m, domain, *, prefix, width, cycle=0)` | 双模输入 |
| `wire_of(sig)` | 提取 Wire（仅 `m.output()`） |
| `cat / zext / sext / trunc` | 位操作辅助 |

### ForwardSignal

| 接口 | 说明 |
|------|------|
| `sig <<= expr` | 无条件赋值 |
| `sig.assign(expr, when=cond)` | 条件赋值（使能） |
| 其余读侧接口 | 与 CAS 相同 |

### compile_cycle_aware

| 参数 | 说明 |
|------|------|
| `fn` | `def fn(m, domain, *, inputs=None, ...) -> dict` |
| `name` / `domain_name` | 模块名 / 时钟域名 |
| `eager` | `True` 直接执行（推荐） |
| `hierarchical` | 保留 `domain.call()` 边界（需 eager） |
| `**jit_params` | 转发给 `fn` |

### CycleAwareTb

| 方法 | 说明 |
|------|------|
| `CycleAwareTb(t)` / `next()` / `cycle` | 包装 / 推进 / 读周期 |
| `drive(port, value)` / `expect(port, value, *, phase, msg)` | 激励 / 检查 |
| `clock / reset / timeout / finish / print / sva_assert / random` | 配置与控制 |

---

**Copyright © 2024-2026 Liao Heng / PyCircuit Contributors. All rights reserved.**
