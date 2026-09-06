# Agentic Circuit 团队 Specification 手册

| 字段 | 内容 |
| --- | --- |
| 目标版本 | Explicit Memory contract epoch `0.5` |
| 状态 | 已在 `main` 实现；本文是团队阅读入口 |
| 适用读者 | Python 前端、ACIR、gfsim、PYC/Verilog 和模型验证开发者 |
| 规范主文档 | [Agentic Circuit Specification Manual](agentic-circuit.md) |
| 机器可读清单 | `opcodes.json` |
| 可执行示例 | `examples/pipelines` |

## 文档定位

本文帮助团队成员快速理解和使用 Agentic Circuit。它解释编程模型、
常用积木、后端差异、验证方法和常见错误，并给出与仓内测试一致的示例。

本文不复制所有 ODS 签名和 verifier 条件。发生差异时，按以下顺序判断：

1. JSON Schema、opcode catalog 和 MLIR ODS；
2. verifier 与 conformance test；
3. [英文规范](agentic-circuit.md)；
4. 本文和设计提案。

规范中的 **MUST**、**MUST NOT**、**SHOULD**、**SHOULD NOT** 和 **MAY**
具有 RFC 风格的约束含义。本文使用“必须”“禁止”“应该”和“可以”表达同一
含义。

## 一句话理解

用户编写串行风格的 Python。编译器读取 AST，把 Python 变量解释成静态连接的
`ac.queue<T>`，把 lambda 内的值解释成零延迟 `ac.var<T>`，再从同一份冻结 ACIR
生成两种内部结构不同的后端：

```text
串行 Python
    |
    v
AST capture / ACPy
    |
    v
Queue/Var ACIR
    |
    +------------------------------+
    |                              |
    v                              v
typed gfsim C++              canonical PYC IR
SimQueue<T> 模型                  |
                                  v
                         pinned pycc
                           |       |
                           v       v
                        PYC C++  Verilog
```

Python 看起来按顺序书写，但运行时不是逐行解释 Python。系统体只在编译期完成
拓扑 elaboration；生成的 Queue、积木和作用域在运行期保持静态。

## 核心对象

### Queue 是带时序的状态

`ac.queue<T>` 是有限深度、带类型、带反压的 FIFO 通道。它具有：

- 编译期确定的 payload 类型 `T`；
- 正数 `depth` 和正数 `latency`；
- 稳定的逻辑身份和层级路径；
- `peek`、`pop`、`push` 和 commit-time 状态变化；
- gfsim 中的 `SimQueue<T>` 实例；
- PYC/RTL 中的 valid/data/ready 与固定存储。

Queue 的 latency 禁止为零。需要零延迟组合逻辑时使用 Var 表达式。

### Var 是不可变组合值

`ac.var<T>` 没有容量、占用率和运行时对象身份。以下对象都属于 Var：

- lambda 参数；
- 常量和算术结果；
- 结构体字段投影；
- 比较结果；
- `with_fields(...)` 产生的新 token。

Queue 可以改变占用状态，Queue 中的 token 不可原地修改。

```python
# 正确：创建一个新 token。
next_item = item.with_fields(remaining=item.remaining - 1)

# 错误：原地修改输入 token。
item.remaining -= 1
```

### Opcode 是公共硬件积木

公开积木统一使用 `ac.*` 命名空间。不存在 `ac.std.*`，也不允许用户定义私有
opcode、C++ provider、PYC provider 或直接插入 Verilog。

`decode`、`dispatch`、`rename`、`retire` 等名称属于具体架构的 scope 或积木组合，
不是通用 opcode。公共积木描述的是 transform、route、merge、memory、barrier、
credit、dependency 和 reorder 等可复用硬件行为。

查看当前闭集：

```bash
build/dev-llvm22/bin/acir-opcode-catalog
agentic-circuit schema opcode ac.transform
```

## 最小示例

下面的系统没有显式输入输出声明。`source` 和 `sink` 定义可执行边界，变量的定义和
使用关系定义 Queue 连接。

```python
import agentic_circuit as ac


@ac.system
def pipeline() -> None:
    incoming = ac.source(int, depth=4, latency=1)
    outgoing = incoming.apply(
        lambda item: item + 1,
        depth=2,
        latency=1,
    )
    ac.sink(outgoing)
```

它 elaboration 成：

```text
incoming Queue -> ac.transform(item + 1) -> outgoing Queue -> ac.sink
```

系统函数必须无参数。`source`、`apply` 和 `sink` 是 AST marker，不能通过普通
Python 调用系统体来模拟电路。

## 定义结构化 payload

使用 `@ac.struct` 冻结字段顺序、位宽和结构身份。

```python
import agentic_circuit as ac


@ac.struct
class WorkItem:
    sequence: ac.u64
    value: ac.u32
    route: ac.u2
    remaining: ac.u16
    valid: bool


@ac.system
def pipeline() -> None:
    incoming = ac.source(WorkItem)
    updated = incoming.apply(
        lambda item: item.with_fields(
            value=item.value + 1,
            remaining=item.remaining - 1,
        )
    )
    ac.sink(updated)
```

无符号电路位型完整定义为 `ac.u1` 到 `ac.u64`。每个名称代表一个精确位宽，
可直接作为 Queue payload 或 `@ac.struct` 字段。`+`、`-`、`*`、`&`、
`|`、`^`、`~`、`<<` 和 `>>` 都保持声明位宽；二元位操作数必须同宽，
右侧整数常量从左侧操作数获得类型。结果按 (2^N) 取模，移位量大于或等于
(N) 时结果为零。相等比较要求位宽一致，`ac.uN` 的关系比较采用无符号语义。

此外保留 `bool`、`int` 和 `ac.s8/s16/s32/s64`。当前 ACIR 契约已冻结整数
宽度，但尚未把有符号性作为完全独立的类型语义；这些保留的 `sN` 名称目前也
采用 signless 的无符号关系比较。真正的有符号比较需要后续类型系统 decision。

进入 MLIR printer 之前，前端使用不可变递归 descriptor 表示 logical bool、精确位宽
bits、nominal enum/struct、structural tuple 和固定 value array。每个 descriptor 都有
规范化身份、稳定 SHA-256 和递归 bit width。`BoolType()` 与 `BitsType(1)` 是两个
不同的编译器事实，虽然当前都渲染为 `i1`。持久 Python `list` 是 state container，
不是 `ArrayType` value。固定 payload array 渲染为 `!ac.value_array<N x T>`；
`!ac.array` 继续只表示静态 Queue/Var topology collection。tuple/value-array 的每个
元素都必须递归 immutable。当前可执行链已开放 acyclic nested struct、标准 Python
enum value、structural tuple 构造、固定 value-array 构造和常量 aggregate 索引。

tuple 与固定 array payload 使用普通 Python annotation 和 value：

```python
@ac.struct
class Packet:
    pair: tuple[ac.bits[3], ac.bits[5]]
    lanes: ac.array[4, ac.bits[4]]

updated = item.with_fields(
    pair=(item.pair[0] + 1, item.pair[1] + 1),
    lanes=(item.lanes[1], item.lanes[2], item.lanes[3], item.lanes[0]),
)
```

tuple/list literal 必须与静态 shape 完全等长；aggregate index 必须在 Frozen ACIR
之前证明为静态且不越界。编译器用类型化的 `ac.var.tuple`、`ac.var.array` 和
`ac.var.element` lowering，在 QueueGraph 保存 aggregate identity/width，并在 gfsim 与
PYC 中使用一个 packed value；Python 不增加硬件 container object。
enum 和 nominal struct 元素会在 aggregate 构造前递归 pack、在选取后递归恢复，
字段顺序与 PYC 的 MSB-first layout 相同。位宽加法/乘法采用溢出检查；超过 64 bit
的字段或跨越元素边界的非法 layout 会在 backend 生成前拒绝。

QueueProgram 在 Queue payload、persistent value、Table entry、memory、slot、rule
state effect 和 reusable module signature 中都保留 descriptor。expression lowering
返回 `(SSA name, ValueType)`，所有 identity、width、enum、aggregate 和 field 检查
直接使用 descriptor；只有 ACIR text renderer 才生成 MLIR spelling。C++ QueueGraph
在解析并验证该 ACIR 边界之后才建立自己的字符串表示。`BoolType()` 与
`BitsType(1)` 在前端保持不同身份；一组范围明确的 epoch-0.5 compatibility helper
仅用于保留已接受的 `i1` equality 与 integer-width 边界，真正的 bool/u1 hard
break 仍需独立 decision。

当前已开放 acyclic nested nominal struct。声明顺序可以前向引用，但递归 cycle 会在
ACIR 生成前拒绝。读取和更新仍使用普通 Python value：

```python
@ac.struct
class Packet:
    header: Header
    payload: ac.bits[17]

updated = item.with_fields(
    header=item.header.with_fields(mode=item.header.mode + 1)
)
```

编译器生成类型化的链式 `ac.var.get`/`ac.var.with`，按依赖顺序发射 C++ struct，
并在 PYC C++/Verilog 中递归使用同一 packing。Python struct 不会被摊平成大量字段，
相同 nested type 也不会按实例重复生成。

nominal value 使用标准 Python enum，不增加 `ac.enum` 前端构造器：

```python
from enum import Enum

class Mode(Enum):
    IDLE = 0
    RUN = 1
    WAIT = 2
```

member 必须按声明顺序从零连续编码。nested struct 字段可以直接标注 `Mode`，
`Mode.RUN` 降到 verifier 检查的 `ac.var.enum`。当前 enum 只支持 equality/inequality。
QueueGraph 保存 member list 与 encoding width；gfsim 生成一次紧凑 C++ enum，
PYC/Verilog 使用同一精确位宽 ordinal。

### 静态 bits 与命名 bitfield view

`ac.bits[N]` 与 `ac.uN` 表示同一种精确位宽无符号值；`N` 必须由确定性的静态
表达式求值为 `[1, 64]` 范围内的整数。普通 Python slice 保持半开区间语义：`word[4:21]` 从 bit 4
开始提取 17 bit。`ac.concat(a, b)` 按 MSB-first 放置参数，
`ac.insert(base, value, lsb=N)` 返回新值而不修改 `base`。

`ac.BitfieldSpec(width=N, fields={name: (msb, lsb)})` 使用闭区间定义命名
bit view。不同字段可以重叠作为替代读视图；一次 update 选择的字段必须互不重叠：

```python
INSTRUCTION = ac.BitfieldSpec(
    width=32,
    fields={
        "opcode": (31, 26),
        "rd": (25, 21),
        "imm17": (20, 4),
        "low25": (24, 0),  # overlapping read view
    },
)

opcode = INSTRUCTION(word).opcode
opcode_rd = INSTRUCTION(word)["opcode", "rd"]
updated = INSTRUCTION.update(word, rd=replacement)
```

命名单字段和多字段读取分别降到 `ac.var.extract` 与 MSB-first
`ac.var.concat`，更新降到不可变 `ac.var.insert`。ACIR 的 `ac.bitfield` 保存
规范化 width/range metadata 和稳定 SHA-256；verifier 在 topology freeze 前把每个
field-qualified operation 解析回声明并复核范围。Python 前端不需要表达任何
ready/full/Queue transaction 逻辑。

### 有界值约束

编译器使用一个小型确定性抽象域：`Constant`、`FiniteSet`、`ClosedInterval` 和
`Unknown`。Constraint 与 `ValueType` 分离，不参与 type identity 或 specialization
fingerprint；前端也不会把 range attribute 或 constraint marker 序列化进 ACIR。
位宽、固定 shape、aggregate index、slice/insert bound 和 topology loop count 在
ACIR 发射前仍必须收敛为具体静态整数。

typed bit transfer 严格遵循 `ac.var` 语义：算术按 (2^N) 取模，逻辑移位量大于等于
(N) 时结果为零。FiniteSet 与 Cartesian propagation 最多保留 64 个值，topology
展开最多 10,000 次；超过上限或无法安全表示时必须保守扩大，所有 proof site 均
fail closed。

编译器公开分析接口统一为 `ACDataFlowAnalyzer`。它基于 MLIR dataflow 从 ACIR SSA
重新推导 constraint，并在 rule lowering、topology freeze 和 QueueGraph planning
之前证明每个动态 persistent-list/Table index 都位于 `[0, entries - 1]`。MLIR 通用
`DataFlowSolver` 只存在于 analyzer 的私有实现中。例如 `u2` index 对 5-entry state
天然安全；未收窄的 `u3` index 会被拒绝。QueueGraph 还会独立重算同一 obligation，
防止伪造 Frozen ACIR 绕过 verifier。

当前切片刻意保持 path-insensitive。动态 aggregate extract/insert、由 guard 反向收窄、
runtime loop termination proof 和通用 enum branch exhaustiveness 是后续独立扩展。

### 掩码位匹配

`ac.matches(value, pattern)` 是精确位宽 bits value 的 decode-oriented masked
comparison。`pattern` 必须是 Python string literal，只能包含小写 `0`、`1`、`x`，
并且长度必须与 value 位宽完全一致；首字符对应最高位。`x` 只表示编译期 don't-care，
不会引入运行时 X/Z 语义。

```python
is_compute = ac.matches(opcode, "1xx0")
```

前端把 pattern 转换为唯一 canonical `mask`/`value` 并发射 `ac.var.matches`。ACIR
验证输入/结果类型、位宽边界以及 `value` 不得在 `mask` 之外置位。QueueGraph 以
`masked_match` 保留该语义，并把两个常量序列化为 exact-width lowercase hex string，
保证 bit 63 不丢失。gfsim 计算 `(input & mask) == value`；PYC 只降低为 vendor-neutral
`pyc.constant`、`pyc.and` 和 `pyc.eq`。

parser 实现在 semantic-core 中共享，但 Agentic 公共面刻意只开放上述 basic grammar；
pyCircuit 已有的扩展语法仍是独立 frontend policy。Agentic 不增加 pattern object、
alternation、capture、runtime pattern、Python `match/case` 或通用 ASL pattern matching。

### 变量属性

Python 前端只暴露普通值、module/class 字段和 lexical scope，不增加硬件命名的变量
类型。MLIR 推导两个正交属性：生命周期为 static、temporary 或 persistent；更新能力
为 immutable 或 assignable。`const` 是 static immutable；rule 参数、返回值和局部
表达式是 temporary immutable SSA snapshot；scope-owned state 是 persistent
assignable，但一次 rule 读到的 committed value 仍不可变，赋值只提出下一拍状态。

这些属性是编译器分析结果，不是 Python marker。后续 pass 再根据类型、访问模式、
def-use、调度和 NDF/target 限制选择实际存储与传输结构。`ac.var` 是表达式值和推导
状态共同使用的唯一 ACIR 变量值概念。

持久 module/class 字段仍降到同一个 `ac.var` 家族。内部 `ac.var.decl` 命名 lexical
state，`ac.var.read` 产生不可变 committed snapshot，`ac.var.assign` 在一个 rule 内
提出 next value。storage-selection pass 在 rule closure 前消除这些操作。第一条可执行
链支持零初始化 scalar integer，并选择单 entry committed 实现；Python 前端不暴露该
选择。

固定大小的持久 list 使用普通 Python 写法，例如
`entries: list[Entry] = [0] * 8`。前端生成带 shape 的 `ac.var.decl` 以及
`ac.var.read_element`/`ac.var.assign_element`，storage selection 再选择只更新 touched
entry 的 committed storage。只有当 `ACDataFlowAnalyzer` 证明动态 index 的值域位于
该 list shape 内时才接受；作者不需要在前端书写范围检查或 marker。

`ac.find(values, where=predicate, key=key)` 是 persistent Python list 上与存储无关的
集合查询。返回的 intrinsic value 具有 `.valid`、`.index` 和 `.value`；省略 `key` 时
选择第一个匹配 index，提供固定宽度整数 key 时选择最小 key，并以 index 稳定打破平局。
Raw ACIR 使用 `ac.var.match` 与 `ac.var.choose`，storage selection 再把它们改写为已有的
committed Table query，不改变 Python variable 模型。selection 的 index/value 只有在对应
`.valid` 条件下才能影响 state。

predicate 可以读取另一个 persistent list。该 owner 是 activation source；只有 rule 实际
写它时才是 transaction resource。生成 policy 通过 const reference 捕获只读 committed
storage，prepare/publish/commit 只覆盖 writable owner。因此 ISQ 的 readiness Table 更新只需
唤醒一次 oldest-ready query，不需要批量改写全部 resident entry。

当前 single-condition rule 子集还携带 verifier 推导的 typed summary attributes，覆盖 guard
kind、Queue availability/capacity check、effect、output-presence kind、state access、schedule
kind 和 lexical arbitration membership。closed enum 是当前 0/1-output 子集的分析词汇；旧
字符串只保留为派生的可读摘要。`predicate` summary 不是 path identity：general CFG 与
selected multi-output 仍必须让每个 output/state proposal 使用 SSA `!ac.var<i1>` presence，
该语义仍属于后续契约。

`ac.priority_encode(value, order="low")` 是语义级组合积木；`.index` 和
`.valid` 在 ACIR 中共享同一个 `ac.var.priority_encode`。`low` 选择最低置位，
`high` 选择最高置位，全零输入返回 `valid=0,index=0`。QueueGraph 使用 gfsim
参考模型，并把同一语义 lowering 为不含厂商名称的 `pyc.priority_encode`。

可执行示例：
`pyc_struct_pipeline.py`。

## 使用 scope 表达层级

`with ac.scope("name"):` 表达所有权和层级，不声明端口。编译器根据跨 scope 的
def-use 自动推导输入、输出和 interconnect 所属的最低公共祖先。

```python
@ac.system
def pipeline() -> None:
    incoming = ac.source(int)

    with ac.scope("frontend"):
        prepared = incoming.apply(lambda item: item + 1)

    with ac.scope("backend"):
        completed = prepared.apply(lambda item: item * 2)

    ac.sink(completed)
```

scope 名必须非空，同一路径不能重复声明。跨 scope 的 Queue 不会被两个子模块重复
拥有；生成系统拥有 interconnect，子模块只借用类型化 Queue 引用。

## 数据通路积木

### Transform

`apply` 生成一个 `ac.transform`。一次 firing 原子地 pop 输入并 push 输出。

```python
updated = incoming.apply(
    lambda item: item.with_fields(value=(item.value + 1) * 2),
    depth=4,
    latency=2,
)
```

lambda 必须是纯 Var 表达式，且返回类型与输出 Queue payload 一致。当前支持字段读取、
常量、`+`、`-`、`*`、比较和不可变 `with_fields(...)` 更新。

### Broadcast 与 Fork

同一 Queue 被多个消费点使用时，前端自动插入严格原子的 `ac.broadcast`：所有输出
必须在同一 firing 接收 token。

需要各输出在不同 cycle 接收时，显式使用 `fork`：

```python
left, right = incoming.fork(outputs=2, depth=2, latency=1)
```

`fork` 为当前 token 保存 delivered mask，每个输出恰好收到一次，全部完成后才 pop
输入。两者不能互换，因为反压和内部状态不同。

可执行示例：
`pyc_broadcast_pipeline.py` 和
`pyc_fork_pipeline.py`。

### Route 与 Merge

`route` 根据一个 Var selector 把 token 发送到一个静态输出，`merge` 把同类型 Queue
合并回来。

```python
scalar, vector = prepared.route(
    outputs=2,
    key=lambda item: item.route,
    depth=2,
    latency=1,
)

scalar_done = scalar.apply(lambda item: item.with_fields(value=item.value + 1))
vector_done = vector.apply(lambda item: item.with_fields(value=item.value + 2))

completed = scalar_done.merge(
    vector_done,
    policy="round_robin",
    depth=4,
    latency=1,
)
```

selector 越界产生确定性 `route_selector_out_of_range` 失败，不会回绕。merge policy
仅支持 `priority` 和 `round_robin`。

可执行示例：
`pyc_route_merge_pipeline.py`。

## 静态集合与运行时选择

Queue 和 Var 可以放入编译期确定形状的 array、map 和 set。

```python
lanes = ac.array(4, lambda index: ac.source(int, depth=index + 1))
named = ac.map({"scalar": lanes[0], "vector": lanes[1]})
active = ac.set({named["scalar"], named["vector"]})
```

静态索引和编译期遍历会被展开。运行时从 flat Queue collection 选择一个成员时，必须
提供显式 control Queue；编译器生成 `ac.select`，而不是动态 Queue 指针。

```python
@ac.struct
class Control:
    route: ac.u2


@ac.system
def selected_pipeline() -> None:
    control = ac.source(Control)
    lanes = ac.array(4, lambda index: ac.source(int))
    selected = lanes.select(
        control,
        key=lambda item: item.route,
        depth=2,
        latency=1,
    )
    ac.sink(selected)
```

control token 与被选中的 data token 原子传输。越界产生
`select_selector_out_of_range`。嵌套集合必须先静态展开，不能在运行时保存或返回
Queue handle。

可执行示例：
`pyc_select_pipeline.py`。

## 状态和调度积木

### Typed Memory

`memory` 是单读单写的类型化状态积木。一次请求总会读取；写入在 Xfer commit；同一
请求对同地址读写时返回旧值，新值对后续请求可见。

```python
@ac.struct
class Request:
    address: ac.u8
    write: bool
    data: ac.u16


@ac.system
def memory_pipeline() -> None:
    sram = ac.memory(ac.u16, entries=16, init=0, latency=3)
    requests = ac.source(Request)
    responses = sram.request(
        requests,
        address=lambda item: item.address,
        write=lambda item: item.write,
        data=lambda item: item.data,
        result_field="data",
        depth=4,
    )
    ac.sink(responses)
```

只允许 `init=0`，实例 `latency` 必须为正数。周期 `T` 接受的请求最早在
`T + latency` 提交响应；request 的 response Queue latency 固定为 1。一个实例可以
连接多个逻辑 endpoint，但只有一个物理端口和一个 outstanding request。endpoint 按
冻结 ordinal 固定优先级仲裁；访问延迟期间及 response Queue 阻塞时全部反压，直到
选中 response Queue 接纳响应。PYC 每个实例只生成一个 `pyc.sync_mem`。

Python 前端允许用同构 `ac.array` 静态声明 memory banks，并以
`banks.select(requests, key=...).request(...)` 明确选择 bank。该写法只在前端展开：
冻结 ACIR 包含一个 `ac.route`、每个 bank 各一个普通 memory instance/request，以及
一个 response `ac.merge`，不会引入新的 primitive。各 bank 的 outstanding 状态独立，
因此跨 bank response 可能乱序；需要保序时应在 payload 中保留 tag 并显式接
`reorder`。epoch 0.5 仅支持一维、data type、entries、init 和 latency 完全相同的 memory
array。

可执行示例：
`pyc_memory_pipeline.py`。

### Stateful Table 原型

epoch `0.5` 新增一维、全零初始化的状态 Table：

```python
Table16 = ac.table[16, Entry]
table = Table16(init=0)
entry = table.view(0)
snapshots = entry.read(when=entry.valid, depth=1, latency=1)

table.view(lambda update: update.index).patch(
    updates,
    enable=lambda update: update.enable,
    done=True,
    result=lambda update: update.result,
)

pending = table.match(lambda entry: not entry.valid)
table.view(pending).patch(
    enable=request_slot.valid,
    valid=True,
    age=lambda entry: entry.age + 1,
)

table.view(tail).allocate(
    enable=allocation.valid,
    value=allocation.value,
)
```

Entry 只能是 bool、定宽整数或仅包含这些字段的扁平 struct。`read()` 总是返回
`Queue<Entry>`；Queue-driven read 的 `when=false` 不消费输入，disabled write 消费
输入但不提出写 proposal。同 tick 读写返回 old committed Entry，写入在 tick commit
后可见，动态越界报告 `table_index_out_of_range`。

`Table.view(candidates)` 接受同一张 Table 的 `match` 所产生的 `CandidateSet`，用于
state-driven masked update。masked `write` 给所有命中 Entry 写入同一个完整值；
masked `patch` 的字段可以是统一表达式，也可以是从各命中 old Entry 求值的纯
`lambda entry`。`enable=false` 不求值 mask/value，空 mask 是 no-op，所有命中项在
同一个 tick edge 原子提交。scalar 与 masked endpoint 可以共存，但各 endpoint 静态声明的
顶层写字段集合必须两两不相交。第一版不分析 address、mask、enable 或 predicate 的动态
互斥性；只要两个 endpoint 声明同一字段就静态拒绝。

每张 Table 可以额外声明一个 state-driven scalar `allocate` endpoint，与上述普通字段
writer 共存。它在调用方提供的 index 安装完整 Entry，不搜索空位、不检查占用状态，也不
隐式修改 `valid`。Queue-driven 和 CandidateSet masked allocation 均拒绝。所有 policy
读取 old committed image；commit 先合并普通字段 proposal，再应用 allocation，因此同一
Entry 上 allocation 覆盖普通更新，不同 Entry 仍独立更新。

每次作者写出的 `match` 和 `choose` 都是共享值，不是 endpoint-local 语法糖。
Frozen ACIR 只生成一个支配所有使用点的 `ac.table.match` 或 `ac.table.choose`，read/write
policy region 捕获其 SSA result。QueueGraph 与 typed gfsim 保留这种共享关系：结果按完整
Epoch 惰性求值并缓存，同一 Epoch 的多个 consumer 只触发一次 Table scan；Epoch 前进或
模型 reset 后重新计算。choose 的 mask 必须来自同 Table 的 match。`policy="first"`
使用空 key region，min/max 仍要求一个有类型的 key region。

`EntryView` 只存在于 elaboration。`patch` 在 Frozen ACIR 前展开成
`ac.table.get -> ac.var.with -> ac.table.write` 或 `ac.table.masked_write`，不存在
`ac.table.patch`。两种 Frozen write op 都必须携带规范化、非空、无重复的
`write_fields` 与必需的 `mode`：普通 write 使用 `mode "field"`，scalar allocation
使用 `mode "replace"`，masked write 只允许 `field`。完整 struct write 展开为所有实际
字段，bool/int Entry 使用 `$entry`。
value region 仍返回完整 Entry，但 commit 只复制声明字段；所有 writer 从同一 old
committed image 求值，并在 tick edge 合并后一次发布。
当前 Table 只支持 typed gfsim C++；PYC/RTL 返回稳定的
`unsupported provisional Table` 诊断。旧 `ac.table(...)` 已删除，请求响应存储继续
使用 `ac.memory`。纵向示例见
唯一公开的 Python Table 示例是
`issue.py`。它组合两个字段不相交的操作数唤醒、下一
tick 的最小 age 选择、grant 驱动的移除，以及显式 match、choose old-state 空位后的
完整 Entry allocation。Table 满时 allocation 请求会保留到空位可选。其余聚焦源码仅
作为内部 E2E fixture，不再作为公开示例。

### Credit

`credit` 表达固定数量的并行 in-flight slot。每个 slot 独立倒计时，完成顺序可以和
输入顺序不同。

```python
completed = issued.credit(
    cost=lambda item: item.cycles,
    credits=4,
    depth=4,
    latency=1,
)
```

`credits` 必须为正，运行时 cost 必须为正。多个 slot 同时完成时，选择最低 canonical
slot index，并且每个 epoch 最多输出一个 token。

可执行示例：
`pyc_credit_pipeline.py`。

### Barrier

`barrier` 等待所有输入可 pop 且所有输出可 push，然后一次性提交所有效果。各输入
payload 类型可以不同，但输出必须按位置匹配。

```python
left_ready, right_ready = left.barrier(
    right,
    depth=2,
    latency=1,
)
```

输入 Queue 必须互不相同。barrier firing 前禁止发布部分结果。

可执行示例：
`pyc_barrier_pipeline.py`。

### Dependency 与 Reorder

`depend` 表达有界依赖窗口、资源占用和执行 cost；`reorder` 按连续 key 恢复顺序。
它们可以组合成 issue/execute/retire 风格架构，但这些应用阶段仍是 scope，而不是新
opcode。

```python
completed = issued.depend(
    key=lambda item: item.sequence,
    waits_for=lambda item: item.waits_for,
    resource=lambda item: item.route,
    cost=lambda item: item.cycles,
    capacity=16,
    resources=4,
    no_dependency=255,
    depth=8,
    latency=1,
)

retired = completed.reorder(
    key=lambda item: item.sequence,
    capacity=16,
    start=0,
    depth=4,
    latency=1,
)
```

完整参考：
`davincioo_queue_model.py`。该模型用公共
积木构造 DavinciOO-like 拓扑，并验证 15 条记录、out-of-order completion、in-order
retirement、Queue occupancy 和 453-cycle 投影。

## 串行控制流

### 编译期控制

`if True/False`、`range(constant)`、静态集合遍历和结构递减的有限递归在编译期展开。

```python
def add_stages(queue, count):
    if count == 0:
        return queue
    return add_stages(
        queue.apply(lambda item: item + 1),
        count - 1,
    )


@ac.system
def recursive_pipeline() -> None:
    incoming = ac.source(int)
    outgoing = add_stages(incoming, 3)
    ac.sink(outgoing)
```

递归深度必须是 `[0, 1024]` 范围内的编译期整数。后端中不会留下递归或动态模块创建。

可执行示例：
`pyc_recursive_pipeline.py`。

### 运行时 if

当前高层语法支持对同一 Queue 的对称二分支更新。它会 lowering 成 route、两个
transform 和互斥 priority merge。

```python
if incoming.route == 0:
    selected = incoming.apply(
        lambda item: item.with_fields(value=item.value + 10)
    )
else:
    selected = incoming.apply(
        lambda item: item.with_fields(value=item.value + 20)
    )
```

两个分支必须消费同一 Queue、各执行一次 apply，并赋值给同一新变量。更复杂的控制
使用显式 route/merge 组合。

可执行示例：
`pyc_conditional_pipeline.py`。

### 有界 while

Queue rebinding 的串行 `while` lowering 成带状态 feedback Queue 的 `ac.feedback`。

```python
while current.remaining > 0:
    if current.stop:
        break
    current = current.apply(
        lambda item: item.with_fields(remaining=item.remaining - 1)
    )
    if current.skip:
        continue
```

当前允许 update 前一个 break guard 和尾部一个 continue guard。最大迭代次数为
1024，超出时产生 `feedback_iteration_limit`。

可执行示例：
`pyc_feedback_pipeline.py` 和
`pyc_loop_control_pipeline.py`。

## Rule 前端

`@ac.rule` 是 Python 中唯一显式的调度边界。rule 接收不可变 payload 并返回输出
payload；用户不书写 Queue effect、检查、ready/valid、调度、commit 或 rollback。

```python
@ac.rule
def increment(item):
    return item.with_fields(value=item.value + 1)

@ac.system
def pipeline(incoming: Item) -> Item:
    outgoing = increment(incoming)
    return outgoing
```

非 `const` system 参数和带类型的返回值是推荐的外部边界写法。编译器在内部插入
`ac.source` 和 `ac.sink`；多个输出使用有序 `tuple[...]` 注解与 tuple 返回。显式
Python `source(...)`/`sink(...)` 仅作为过渡兼容路径保留。

epoch 0.5 的 pure rule 支持一个或多个 Queue 输入、一个输出和一条完整返回路径。
每个参数都是对应 Queue 的 committed head payload。前端只生成 variadic transient
`ac.rule` 与 typed output-handshake obligation；MLIR pass 推导全部输入消费和输出生产
effect，建立空检查契约，生成 `ready_valid_Nx1` 握手，解析调度并降到 marker-free
`ac.firing`。当前仍无可执行 dynamic checked IR，因此动态检查 obligation 会
fail-close 拒绝。证明完整契约等价后，pure firing 规范化为 variadic
`ac.transform`，QueueGraph/gfsim 用一个 atomic transform 执行；输出反压不会只消费
部分输入。输入 Queue 的 payload 类型可以不同；当前单个结果保持主输入的 payload
类型。

stateful rule 可以没有返回值。编译器将其推导为 `Nx0` consume-only transaction：
被选择的输入和 state proposal 一起提交，不创建 dummy Queue 或 sink。stateful rule
也可以没有 Queue 输入而返回一个值。包围 state assignment/return 的单个 Python
`if` 会降为 typed `ac.rule.condition`，再变成 `ac.firing.condition`；条件为 false 时
不形成 candidate transaction。输出容量仍属于推导出的同一 transaction，因此
state-driven retire 在结果 Queue 反压时不能清除 entry。

恰好一个 Queue payload、没有输出的 rule 可以用一个或多个普通串行 early `return`
明确表示“该 token 已处理，但不选择 state effect”：

```python
@ac.rule
def complete(entries, completion):
    old = entries[completion.index]
    if old.generation != completion.generation:
        return
    if old.epoch != completion.epoch:
        return
    entries[completion.index] = completion
```

它不同于上面的 blocking trailing `if`。编译器为 input consumption 生成 constant-true
candidate condition，把每个 early-return predicate 的反条件合成为一个 SSA conjunction，
再作为 `when` 附加到通用 `ac.var.assign`；storage selection 会把该 SSA presence 保留到
`ac.table.propose`。early return 必须连续并位于全部 state effect 之前，因此对 pure、total
predicate 的扁平求值不会把 write 移过 return。当前限制要求全部 conditional state effect
共用合成后的 predicate，且不能同时使用 blocking guard、多个 Queue payload 或 selected
output。

outputless、单输入 rule 还可以使用一个普通 `if/else`，两个 branch 分别赋值不同的
persistent owner。前端保留 branch test 与其 Boolean complement，形成两个 SSA presence。
Rule、Firing 和 QueueGraph verifier 会分别要求它们属于同一个 predicate 或一个结构上已证明
互补的 pair。生成的 gfsim 只计算一个 Work candidate，并只 prepare 被选择的 owner；input 与
该 state 仍在同一个 atomic group 中 publish。若两个 arm 都赋值同一个 scalar owner，编译器
生成一个 `ac.var.select` value join，再生成一个 unconditional state proposal；QueueGraph/gfsim
使用 ternary，PYC 使用 `pyc.mux`，因此该 owner 仍只有一个 write slot 和一次 commit。若两个
arm 都赋值同一个 persistent list，编译器会分别用 typed
`ac.var.select` join value 与 index，再生成一个 unconditional `ac.var.assign_element`。每个
源码 index 仍必须满足已有的精确位宽/full-domain 安全证明。一个 branch value 依赖另一个
branch 写入的 owner，仍需等待通用 state join。

一个 stateful output 可以是 optional。Python 末尾的
`if condition: return value` 再跟一个 `return`，表示 input 和之前的 state effect 总是被选择，
而 output 只在 condition 为真时被选择。前端生成 `ac.rule.output ... when`，MLIR 独立推导
predicate-qualified output capacity/effect summary。因此 output Queue 已满时，只有 presence=true
才阻塞完整 transaction；presence=false 路径仍会消费 input 并提交 state。Rule、Firing 和
QueueGraph verifier 要求这种不同于 candidate 的 output presence 只有一个 input，且 candidate
必须是 constant true。

`ACDataFlowAnalyzer` 会从 candidate、output presence 和 state-effect presence 反向遍历，
生成 compiler-owned state snapshot proof。顶层 `ac.table.get` 会成为带精确 static 或已证明
full-domain dynamic index 的 `ac.state.snapshot`；作为源的 `ac.table.match` 会 reservation
整个被扫描 owner；match predicate 内对另一张 Table 的读取则成为以 match mask 为 source 的
`ac.state.snapshot_set`，因此只 reservation 同一次 scan 中实际访问的 index。当前
`table.choose` key region 内对另一张 Table 的读取则使用 choose index result 作为规范的
evaluation provenance；只有 candidate-mask 命中、即将真正计算该 candidate key 时才更新
dependency mask。当前 snapshot-set 契约支持最多 64 个 entry。shared、非事务 choose 的
key region 若读取 Table 会 fail-close，不能在缺少 snapshot closure 时静默 lowering。

closure verifier 会在 freeze 前独立重推完整 proof set。QueueGraph 将 scalar/all/set
reservation 与 write、activation source、transaction resource 分别保存。生成的 gfsim 在原
match scan 内把 set reservation 折叠成 `uint64_t` mask，不进行第二次 state traversal，也不
需要堆分配。若 rule parameter 在调用点绑定 lexical persistent scalar，即使它只读，也会从
state-prefix binding 推导并降成 `ac.var.read`，作者不需要用 self-assignment 强制序列化。

snapshot proof 还携带有序 `read_fields`。若 Table result 通过直接 `ac.var.get` 读取字段，
reservation 会缩小到该字段；若消费完整 Entry，则记录全部声明字段；scalar entry 使用
`$entry`。QueueGraph 会依据 Entry 声明验证字段。生成的 gfsim 使用紧凑
`StateReservation` 保存完整 Entry 的 entry mask；partial read 则使用一个 64-bit relation，
每一位精确编码一个 `(entry, field)` pair。同 entry 的 field-merge write 只有对应 pair 存在时
才冲突，而 replace write 仍与该 entry 的任意读取冲突。heterogeneous clause 的 relation union
不会形成 cross-product，也不需要堆分配。partial relation 当前要求
`entries * declared_fields <= 64`；完整 Entry 与 scalar mask 仍使用已有的 64-entry 上限。

一次 rule activation 可以更新多个 lexical persistent value。scalar 与固定 list
assignment 在 storage selection 前仍是通用 `ac.var`；选出的异构 state owner 由一个
`gfsim::QueueStateTransition` 承载。Work 只计算包含全部 owner write 的 immutable
candidate；Arbitrate 要么一起 reserve/publish 全部 owner 与所选 Queue，要么一个也不
publish。

`examples/agentic-circuit/state/circular_rob.py` 使用这条链实现真实的四 entry circular
ROB。普通 scalar 保存 head、tail、occupancy 和 recovery epoch，普通
`list[RobEvent]` 保存 entry；四个 rule 分别实现 recovery、allocation、completion 与
state-driven retirement。生成式测试覆盖 full/empty、固定宽度 head/tail wrap、per-slot
generation stale rejection、recovery epoch、乱序完成、顺序退休，以及 allocation 和
retirement 输出反压。Python 源码不含 Queue/Table/source/sink/readiness/commit 操作。
generation 与 recovery epoch 是 16-bit 有限 tag；环境不得让 completion 跨越同一
slot 的 `2^16` 次复用或 `2^16` 个 recovery epoch。

Python `ac.atomic()` 与 `Queue.firing()` 已删除；编译器会给出迁移到 `@ac.rule` 的
明确诊断。`firing` 只保留为编译器内部 IR 概念。

可执行示例：`pyc_rule_pipeline.py` 和
`pyc_multi_input_rule_pipeline.py`。

stateful rule 的第一个参数是 owner state，之后可以接收零个或多个异构 Queue
payload。存在时，主输入和单个输出必须与 state Entry 类型一致，其余输入可作为 metadata、
completion 或 control token 参与普通顺序表达式：

```python
@ac.rule
def install(rob, entry, delta):
    old = rob[entry.index]
    rob[entry.index] = entry.with_fields(value=entry.value + delta.amount)
    return old
```

MLIR 推导 `ready_valid_Nx1_table`，并将全部 Queue 消费、单个 Table replace 和输出
生产闭合为一个 `ac.firing`。QueueGraph/gfsim 生成 variadic
`QueueTableTransition`；任一输入缺失、输出反压或 Table reservation 冲突时，全部输入
和 Table 都保持不变。可执行示例：`table_multi_input_rule.py`。

## Observation 与 Verification

`ac.observe(queue)` 非消费地观察 committed head，不参与反压，可以进入设计 lowering。

`ac.expect(...)` 也是非消费 leaf，但角色是 verification：

```python
ac.expect(
    completed,
    predicate=lambda item: item.value > 0,
    message="value must be positive",
)
```

gfsim 对每个新 committed head 检查 predicate，失败时报告 `expectation_failed`。
PYC design hierarchy 明确拒绝 `ac.expect`；对应检查必须放到 PYC testbench boundary。
这不是后端缺失，而是 design role 与 verification role 的边界。

可执行示例：
`gfsim_expect_pipeline.py`。

## gfsim 与 PYC/Verilog 的差异

两种后端共享冻结 ACIR 语义，但内部 IR 和状态结构不要求相同。

| 方面 | typed gfsim | PYC/Verilog |
| --- | --- | --- |
| Queue | `SimQueue<T>` 对象 | valid/data/ready + FIFO/register |
| Var | C++ 局部值或纯表达式 | wire、packed value、组合 op |
| 层级 | `gfsim::Module` 对象层级 | 静态 module hierarchy |
| 调度 | snapshot/proposal/Xfer/commit | 时钟边沿和 ready/valid handshake |
| Memory | 类型化 QueueMemory 状态 | `pyc.sync_mem` + 对齐寄存器 |
| Verification | `ac.expect` 可执行 | design 中拒绝，testbench 中实现 |
| 输出 | 类型化 C++ simulator | PYC C++ 与 Verilog |

跨后端 refinement 比较声明过的语义投影：输入/输出 transaction、接受与完成身份、
架构状态、memory-visible effect 和明确的 assertion。它不比较 gfsim delta、内部 Queue
布局、PYC 寄存器名称或每个未声明的内部 cycle。

同一 PYC IR 生成的 PYC C++ 和 Verilog 必须 cycle equivalent。gfsim 与 PYC 可以有
不同内部 latency，但必须满足选定的 observation/refinement contract。

## 编译和验证示例

先配置 LLVM 22.1.8 开发环境：

```bash
scripts/bootstrap-dev.sh
source .venv/bin/activate
cmake --preset dev-llvm22
cmake --build --preset dev-llvm22
```

生成 frozen ACIR、QueueGraph plan 和 typed gfsim C++：

```bash
PYTHONPATH=src .venv/bin/python tools/ac-queue-cxxgen.py \
  examples/pipelines/davincioo_queue_model.py \
  --system davincioo_queue_model \
  --acir-output build/davincioo_queue_model.ac.mlir \
  --plan-output build/davincioo_queue_model.queue-plan.json \
  --acir-opt build/dev-llvm22/bin/acir-opt \
  --queue-plan-tool build/dev-llvm22/bin/acir-queue-plan \
  --queue-cxxgen-tool build/dev-llvm22/bin/acir-queue-cxxgen \
  --output build/davincioo_queue_model.cpp
```

验证生成 C++：

```bash
c++ -std=c++20 -I include -fsyntax-only build/davincioo_queue_model.cpp
```

使用锁定的 pyCircuit toolchain 生成 PYC C++ 与 Verilog：

```bash
PYC_TOOLCHAIN_ROOT=/path/to/pycircuit/toolchain/install

.venv/bin/python tools/ac-queue-pyc-build.py \
  build/davincioo_queue_model.ac.mlir \
  --pycgen-tool build/dev-llvm22/bin/acir-queue-pycgen \
  --pycc "$PYC_TOOLCHAIN_ROOT/bin/pycc" \
  --toolchain-lock toolchains/pyc.lock.json \
  --toolchain-metadata \
    "$PYC_TOOLCHAIN_ROOT/share/pycircuit/toolchain-metadata.json" \
  --cxx "$(command -v c++)" \
  --verilator "$(command -v verilator)" \
  --pyc-output build/davincioo_queue_model.pyc \
  --cpp-output-dir build/davincioo_queue_model-pyc-cpp \
  --verilog-output-dir build/davincioo_queue_model-verilog \
  --manifest build/davincioo_queue_model-pyc-manifest.json
```

命令会检查 `pyc.lock.json`，执行 PYC
verification、C++ syntax check、Verilator lint，并记录确定性 artifact hash。目标输出
路径必须不存在，防止覆盖旧证据。

## 明确禁止的写法

### 显式系统端口

```python
# 禁止：系统边界由 source/sink 和 def-use 推导。
@ac.system
def pipeline(input_queue, output_queue):
    ...
```

### 零延迟 Queue

```python
# 禁止：Queue latency 必须为正。
incoming = ac.source(int, latency=0)
```

### 运行时拓扑和动态 Queue handle

```python
# 禁止：运行时不能构造、保存或返回 Queue 指针。
selected_queue = lanes[token.route]
```

应改用 `lanes.select(control, key=...)`。

### 私有 opcode 或后端代码

```python
# 禁止：用户不能从 Python 注册私有实现。
ac.register_opcode("my.dispatch", cpp_provider=..., verilog=...)
```

需要新能力时，先把它定义成通用硬件积木，并同步更新 Python、ACIR、verifier、
QueueGraph、gfsim、PYC、测试和 opcode catalog。

QueueGraph 可以保留结构化模块层次。此时 IR 使用唯一选中的 `ac.system`、可复用的
`ac.module` 定义、`ac.instance` 实例和模块局部 `ac.scope`；freeze pass 自动插入并
校验 definition fingerprint 与由 definition 加静态参数导出的 specialization
fingerprint。同一定义和同一组参数的多个实例必须共享 specialization 身份，后端按
specialization 生成一次实现类，只为每个实例绑定独立端口和状态，不能按层次路径把
模块摊开。旧的 flat QueueGraph 仍作为过渡输入保留，但不再是模块化设计的目标形式。
对于 stateful specialization，实现类共享 Table/transition 的成员布局，但每个实例必须
构造独立的运行时 Table 和 object ID；复用代码绝不能共享 committed state 或事务所有权。
同一 specialization 中的多条 firing rule 可以绑定模块 typed Queue interface 的不同子集并
共享一个局部 state owner。冲突规则按 frozen lexical priority 仲裁；失败规则保留全部输入，
下一拍基于新的 committed snapshot 重算，而其他模块实例只与自己的状态仲裁。
一条 firing 也可以同时提议多个模块局部 state owner。specialization 类只保存一次
`QueueStateTransition` policy 和成员布局，每个实例分别构造全部 Table；所有 Queue effect
和 owner write 仍属于同一个 prepare/publish/no-fail commit group，任一 owner reservation
失败都不能消费输入或部分更新状态。
同一模块中的不同 rule 可以触碰模块 owner 集合的不同有序子集。每个 transition 只绑定
自身实际使用的 Queue 和 owner，而 class 为每个实例构造所有声明 owner 的并集；因此
lexical arbitration 保持 rule-local，也不会把无关 Table 塞进事务。
specialization 可以实例化其他 specialization。planning 按 child-before-parent 顺序构造
无环依赖图；parent plan 只保存一份 direct child body 和实例绑定，codegen 先生成一次
child class，再生成一次 parent class，并递归划分 parent 的 dense runtime-ID 区间。
重复 parent 只重复对象构造和绑定，不重复任何 class body。
parent specialization 可以拥有连接 local block 与 child instance 的内部 Queue。内部 Queue
属于 parent 实例自己的 runtime-ID 区间，每个 parent object 独立构造；child 导出结果直接
绑定 parent interface。内部存储不能提升到 root，也不能在重复 parent 实例之间共享。

Python 用户用普通 typed `@ac.module` 函数声明可复用行为，并在 `@ac.system` 中以普通调用
使用它。前端不暴露 Queue port、instance object、specialization fingerprint、source、
sink、ready 或 backpressure。第一阶段支持纯 1x1 module：表达式 return 降为模块局部
transform，typed system 参数/结果生成内部边界，普通调用生成 `ac.instance`。
module 的表达式 return 可以直接调用另一个 typed module。前端生成包含 child
`ac.instance` 的 parent `ac.module`，既有 planning/codegen 保持 child-before-parent
复用；Python 函数仍只返回普通值，不命名任何 hierarchy object。

stateful module 链支持一个或多个零初始化 scalar lexical variable，每个变量按 Python
源码顺序赋值一次，并返回一个 typed expression：

```python
@ac.module
def accumulator(value: ac.u8) -> ac.u8:
    total: ac.u8 = 0
    total = total + value
    return total
```

activation 开始时一次性读取所有 committed variable；赋值按源码顺序更新局部不可变值
环境，再为每个 owner 生成一个 `ac.var.assign` proposal。MLIR 选择实际存储并推导一个
Queue 加全部 state 的原子事务；重复调用共享一份实现 class，但每个实例构造独立的
committed state。任意 arity、条件更新、shaped module state、static 参数和重复值 fanout
推导仍是后续工作。

rule-backed module 与 system 复用同一个 callable-body parser 和 QueueProgram event
renderer。此类 module 可以有多个 typed input/output 并调用多条普通 rule；每条 rule
本身仍返回零或一个 Queue。module 参数在内部绑定 borrowed Queue value，Python typed
return name 变成 `ac.return` operand。root system 只需用普通 tuple assignment 放置
module；`ac.instance`、interface binding、specialization identity 和每实例 state 都由
编译器生成。

`reusable_circular_rob.py` 用一份 3-input/2-output、五个 lexical state owner、四条 rule
的 ROB 定义放置两个独立实例；specialization body 和生成 class 都只出现一次。当前已支持
direct interface-to-rule graph，任意内部 Queue graph 与 module 内 repeated-input fanout
仍是后续工作。

host-integrated simulation 可使用编译选项 `--host-results`，把 typed system return 保留为
Top module Queue result，而不是插入自动 sink。生成的 `result_N()` 暴露 committed
occupancy，`try_take_result_N(system)` 把一次 dequeue 加入 external-Xfer frontier。standalone
与 host 模式使用同一份 Python，Python 不命名 boundary Queue 或 sink。

QueueGraph activation 从 typed Queue endpoint 与 Table footprint 推导。某个 input/output
Queue commit，或相关 Table 的 committed value 确实变化后，会在下一 epoch 唤醒所有订阅
block。写回相同最终值的 Table proposal 仍完整提交其原子事务，并保留 progress 与
observation 记录，但不传播 activation；独立的 Work-closure 关系把所有 input/output Queue 和 writable Table 加入该 block 的同拍
Arbitrate/Probe/Commit barrier，但不会调用它们的空 Work 方法。zero-input rule 初始执行一次，此后只由 owner
变化或 output dequeue 再次唤醒。activation edge 与 initial frontier 是 canonical compiler
evidence，后端不能从生成的 C++ 文本猜测。physical binding 会递归穿过当前支持的
specialization hierarchy，把 borrowed interface、parent-local Queue、block、Table 与 child
ObjectId 区间映射起来而不展开 class；`activation_complete()` 表示整个 reachable hierarchy
是否均已覆盖。

对 rule-backed block，`ac-infer-rule-activation` 在 `ACDataFlowAnalyzer` 生成 state footprint
之后冻结 enum-typed input、output 和 state resource record；firing verifier 在 QueueGraph
提取前重新推导并逐项校验。生成的 system input 提供 `offer_<name>(system, value)`，负责把
外部 Queue proposal 加入调度，而 Python authoring 不增加任何 ready 或 schedule 操作。

运行时先执行 active block Work，再先仲裁 owner、后仲裁 closure-only resource；完整 closure
全部 Probe 后才进入 Commit loop。外部 Queue offer 使用独立 Xfer frontier，不再把 Queue
伪装成 Work block。
host-result dequeue 使用同一 frontier；返回值只有在下一次 system step 提交 Queue pop 后才
算被 host 接受，该 commit 会唤醒此前因 result Queue full 而阻塞的 producer。

validated runtime profile 会在全局 Probe barrier 之后记录每个 committed ObjectId、epoch 与
semantic-change bit。该 commit timeline 用于等价验证和调试；fast profile 的 commit 热路径
不会分配或追加这些记录。

当前 single-condition、0/1-output rule 子集把 path selection 保存为真实 SSA evidence，
而不只是一项 summary category。`ac.rule.output` 与 `ac.firing.output` 将返回值及 ordinal
绑定到一个 `!ac.var<i1>` presence；每个 firing-local `ac.table.propose` 也在 storage
selection 后携带自己的 presence。Rule/Firing verifier 要求恰好一个 candidate condition、
完整 output ordinal、返回值 identity，并证明每个 effect presence 蕴含 candidate。只有当
candidate 为 constant true、恰好一个 input 且所有不同 effect 共用一个 predicate 时，
conditional-effect presence 才能不同于 candidate。QueueGraph 分别保存 candidate 与 effect
presence。gfsim 用 `nullopt` 表示 stall/保留输入，用 engaged plan 加 absent write 表示消费
输入但不提交 state；它会短暂 reserve analyzer-derived snapshot index 以对重叠 lexical
writer 验证 committed decision，然后取消未选择的 reservation 而不发布 Table proposal。
snapshot reader 彼此兼容；snapshot/write 在 index 重叠时冲突，不相交 index 可独立执行。
Python 不暴露这些 proof 或 reservation operation。candidate/output predicate 与
match/choose index set 的通用推导、CFG join 及 multiple selected output 仍不属于当前子集。

## 常见问题

| 现象 | 原因 | 处理方法 |
| --- | --- | --- |
| Queue 被两个消费者使用 | pop 是消费 effect | 需要原子复制时依赖自动 broadcast；需要解耦时显式 fork |
| `latency=0` 被拒绝 | Queue 是状态，不是 wire | 把逻辑写入 lambda，让它 lowering 成 Var |
| selector 越界 | route/select 拓扑是静态闭集 | 修正 selector 位宽或在上游保证合法范围 |
| loop 被拒绝 | 不是受支持的单 Queue 有界 feedback 形状 | 简化为一次 Queue update，或显式组合 route/merge/feedback |
| PYC 拒绝 `ac.expect` | verification leaf 不能进入 design | 把 assertion 放入 PYC testbench boundary |
| 后端结果内部 cycle 不同 | gfsim 与 RTL IR 不同 | 比较声明的 transaction/state/refinement projection |
| artifact epoch 不匹配 | serialized epoch 是 hard break | 重新生成 exact epoch `0.5` artifact，不使用兼容 shim |

## 修改公共契约的完成条件

新增或修改一个公共积木时，必须同步完成：

- Python 正向和拒绝语法；
- ACIR ODS 类型或 operation；
- verifier、错误码和诊断文本；
- QueueGraph canonical plan；
- gfsim runtime 语义与类型化 C++ 生成；
- PYC lowering，或明确的 backend-role 拒绝；
- 正向、负向、determinism、round-trip 和 cross-backend 测试；
- `opcodes.json`；
- [英文规范](agentic-circuit.md) 和本文对应入口。

不要把某个后端的实现细节提升成共享 ACIR 语义，也不要为已移除的公共表面增加兼容
别名。旧契约由 Git 历史、release tag 和
[`REF-HISTORY-001`](refs/history.md) 保存。

## 推荐阅读顺序

新同学可以按以下顺序阅读和动手：

1. 本文的“核心对象”和“最小示例”；
2. `examples/pipelines/README.md`；
3. `davincioo_queue_model.py`；
4. [英文规范](agentic-circuit.md)；
5. `opcodes.json` 和
   `test/ACIR`；
6. [NDF 仓库布局验证](../../development/acir/verification/repository-layout.md)。
