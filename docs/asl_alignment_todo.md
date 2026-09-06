# PyCircuit 对齐 ASL 数据类型 —— 差距分析与 TODO

<!-- markdownlint-disable MD031 MD036 -->

**状态：** 待开工（v0.1）
**分支：** `asl_align`
**来源：** 对照 `docs/arm_data_type.md`（ARM ASL1 数据类型分析）与当前 pyCircuit 6 代码库（`python/pycircuit/src/pycircuit/`）逐项核实。
**用途：** 逐条列出「ASL 有、PyCircuit 信号级 DSL 缺」的功能、当前现状、可复用基座、建议 API 与门禁，供开工。勾选框仅表示"是否已实现"，不代表优先级。

---

## 0. 结论速览

ASL 是**行为规范语言**（单一时间轴＝指令执行，数学域在运行时），PyCircuit 是**硬件构造语言**（双时间轴＝展开期＋硬件运行期，数学域在展开期）。两者共享"位向量为唯一具体类型 + 显式转换到无界数学域"的双域模型。

**机器域（位向量类型与运算）已高度一致**，无需改动：

- `bits(N)`/`bit` ↔ `Wire`（`iN` 定宽）+ `CycleAwareSignal`（额外携带周期标签，ASL 无此维度）。
- 位逻辑 `& | ^ ~`、算术 `+ - * // %`、拼接 `cat`、比较（含显式 `.ult()/.slt()`）、移位 `<< >>`、`mux/select`、`zext/sext/trunc`、`as_signed/as_unsigned` 全部就位。
- `boolean` vs `bit` 纪律等效（`Wire.__bool__` 抛错，禁止 `if sig:`）。
- `record`↔`RecordSpec`/`StructSpec`、`array`↔`Vec`、tuple↔Python tuple、无界 `integer/real`↔ 展开期 Python `int/float`。

**需要补齐的是 ASL 的类型级"显式性/防错"语法糖**，共 5 项（全部为**纯前端语法糖**，展开为现有 slice/cat/and-eq/assert，**不改 MLIR 方言语义**，符合 AGENTS.md gate-first 边界）：

| 编号 | 功能 | ASL 对照 | 优先级 | 可复用基座 |
|---|---|---|---|---|
| T1 | 单信号位域视图 ✅ **已完成** | `bits(N){[31] N,[3:0] M}` | ★★★ 高 | `spec.StructSpec.field_slices()` |
| T2 | 位掩码模式匹配 ✅ **已完成** | `x IN {'1xx0'}`、`opcode == '1(0)x0'` | ★★★ 高（解码器刚需） | `spec.DecodeRule(mask, match)` |
| T3 | 类型安全枚举 ✅ **已完成** | `enumeration {RED,...}`（不可转 int） | ★★ 中 | Python `enum` + 展开期检查 |
| T4 | 断言式类型转换 ✅ **已完成** | `expression as ty`（断言+转换） | ★★ 中 | 现有 `trunc()` + 仿真断言 |
| T5 | 缩放切片 ✅ **已完成** | `x[idx *: len]` | ★ 低 | `Wire.lane` → `Wire.slice` |
| T6 | 文档级约定 ✅ **已完成** | `UInt/SInt`、`constant`/`config` 意图分层 | ★ 低 | 编程规范 + 补齐 `sgt/sle/sge` |

**明确不吸纳**（`docs/arm_data_type.md` §9.5 已论证）：约束整数（width 已承担该职责）、运行时无界整数（违背硬件代价显式）、`ARBITRARY`（硬件需确定性）、短路布尔（组合电路无短路）。

---

## T1. 单信号位域视图（对应 ASL `bits(N) { [31] N, [3:0] M }`）—— ★★★

- **ASL 对照**（`arm_data_type.md` §2.1/§5.2/§9.5-①）：位向量可带命名位域，**字段可重叠**（同一寄存器多种视图），`x.fld` 读、`x.[f1,f2]` 拼接读、`PSTATE.[N,Z,C,V] = '0011'` 多字段写（读-改-写语义）。
- **现状（0.40）**：
  - `Wire`/`CycleAwareSignal` **无位域方法**；解码/系统寄存器代码充满 `instr[25:21]` 魔法数字。
  - `RecordSpec`（`record.py`）是**端口级**展开（`in_engine_kind_0`），`Bundle.unpack`（`hw.py`）是**位置级**，均非单信号命名切片视图。
  - **可复用基座**：`spec/types.py` 的 `StructSpec.field_slices()` 已能给出 `字段名 → (lsb, width)`（`spec/types.py:84-90,530`），但没有 `view(signal)` 作用到活信号，也不支持重叠字段。
- **缺口**：把 `field_slices()` 接到活信号（`Wire`/CAS），并支持重叠字段。
- **建议 API**：
  ```python
  INSTR = BitfieldSpec(width=32, fields={
      "opcode": (31, 26), "rd": (25, 21), "imm16": (15, 0),
      "imm26":  (25, 0),                 # 与 rd/imm16 重叠 —— 合法（视图不同）
  })
  f  = INSTR.view(instr)                 # f["opcode"] ≡ instr.slice(31, 26)（CAS 闭区间）
  wr = INSTR.update(instr, rd=new_rd)    # 读-改-写：cat 出未改字段 + new_rd
  ```
- [x] **T1.1（已完成）**：新增 `BitfieldSpec`（`fields: dict[str, (msb, lsb)]`，闭区间，**允许重叠**；`__post_init__` 校验 width>0、名字非空、`0<=lsb<=msb<width`）。落地 `python/pycircuit/src/pycircuit/bitfield.py`，另提供 `field_slices()`（名→`(lsb,width)`，与 `spec.StructSpec` 对齐）与 `field_width()`。已从 `pycircuit` 顶层导出 `BitfieldSpec`/`BitfieldView`。
- [x] **T1.2（已完成）**：`view(signal)` 返回只读 `BitfieldView`，`f["opcode"]`/`f.opcode` → `signal[lsb:msb+1]`；`f["a","b"]` 多字段拼接读（MSB-first，等价 `cat`）；支持 `keys/items/__iter__/__contains__`；写操作抛错（只读）。
- [x] **T1.3（已完成）**：`update(signal, **kwargs)` 展开为 `cat(未改高位片段, new_field, 未改低位片段)`（MSB-first 平铺）；单次调用写重叠字段报错（歧义）；字段值支持 int（带值域检查）/Wire/Reg/CAS，宽度不符报错。
- [x] **T1.4（已完成）**：门禁 `tests/test_bitfield_view.py`（26 项，全通过）：字段读/多字段拼接读/单字段与多字段 `update` 与**手写 slice/cat 字节级等价**（对比 `emit_mlir()`）；重叠视图；越界/歧义写、宽度不符、越界常量、未知字段、宽度不匹配、只读均报错；CAS 视图/更新保持类型与 cycle，跨 cycle 字段写报错。

**双信号类型支持**：`Wire` 与 `CycleAwareSignal`/`ForwardSignal` 均可（CAS 结果保留 cycle 标签）。纯前端语法糖，展开为现有 `slice`/`cat`，不触碰 MLIR 方言语义。

- [x] **T1.5（已完成，声明即绑定）**：新增绑定型 `BitfieldSignal`，让"声明时指示位域、之后直接 `x["fld"]`/`x.fld` 访问"成为可能，无需每次调用 spec。
  - `SPEC.bind(signal)` 把布局附着到信号，返回 `BitfieldSignal`；它把算术/比较/`<<=`/位切片全部委托给底层信号（drop-in 替换），并新增字段访问 `x["opcode"]` / `x.opcode` / `x["a","b"]` 与 `x.update(fld=...)`（结果仍为绑定态）；`x.raw` 取回底层信号、`x.spec` 取回布局。
  - 声明期直接绑定：`m.input(name, fields=...)` 与 `domain.signal(name=.., fields=...)`，其中 `fields=` **同时接受 `BitfieldSpec` 或原始字典 `{name:(msb,lsb)}`**（字典时须给 `width=` 现场构造，spec 时 width 可省略、从 spec 推断）。归一化集中在 `bitfield.coerce_bitfield_spec()`。
  - `wire_of()` / `m.output()` / `_to_wire()` 通过 `__pyc_unwrap__` 钩子自动解包绑定信号（wire 背衬可直接 `output`，CAS 背衬用 `wire_of`）。
  - 字符串下标/属性 → 字段读；整数/`slice` 下标（`x[3]`/`x[0:8]`）→ 原始位切片（无歧义）。与成员名冲突的字段（如 `update`/`raw`/`width`）用字符串下标访问。
  - 门禁共 43 项（`test_bitfield_view.py` 全通过）：绑定读/属性读/多字段读/位切片与手写等价、算术委托、`update` 保持绑定、`input(fields=)`/`domain.signal(fields=)` 声明期绑定（含内联字典）+ 反馈 `<<=`、宽度冲突/缺 width/只读报错、`output` 直接接受 wire 背衬绑定信号。
- [x] **T1.6（已完成，附加便利）**：
  - `SPEC(x)` 作为 `SPEC.view(x)` 的可调用简写，写法贴近 ASL：`SPEC(x).opcode`。
  - `pyc.extract` 发射时把 `msb` 一并写入属性 `{lsb = N, msb = M}`（`dsl.py`，`msb=lsb+width-1`），阅读发射的 MLIR 一眼可知提取范围（无需再从结果类型宽度反推）。
- [x] **T1.7（已完成，后端方言支持 + 自洽门禁）**：`msb` 已在 MLIR 方言侧落地为**可选属性**并加 verifier（gate-first）。
  - `.td`：`PYC_ExtractOp` 参数增加 `OptionalAttr<I64Attr>:$msb`（`compiler/mlir/include/pyc/Dialect/PYC/PYCOps.td`）；`assemblyFormat` 用 `attr-dict`，无需改解析格式，旧 IR（无 msb）仍兼容。
  - verifier（`compiler/mlir/lib/Dialect/PYC/PYCOps.cpp` `ExtractOp::verify`）：当 `msb` 存在时强制 `msb == lsb + result_width - 1`，否则报错 `msb must equal lsb + result_width - 1 (expected .., got ..)`。
  - 构造点适配：`PackI1RegsPass` 的 `create<ExtractOp>` 补传 `msb`（1 位提取 msb==lsb）。
  - 已 `ninja pycc` 全量编译链接通过；端到端实测：正确 msb 通过 verify，篡改 msb（31→30）被解析期拒绝。

### 使用示例

**声明位域**（`(msb, lsb)` 闭区间，允许字段重叠）：

```python
from pycircuit import BitfieldSpec

INSTR = BitfieldSpec(width=32, fields={
    "opcode": (31, 26), "rd": (25, 21), "rs": (20, 16), "imm16": (15, 0),
    "imm26":  (25, 0),                 # 与 rd/rs/imm16 重叠 —— 合法（视图不同）
})
```

**用法 (A) —— 临时视图**（对已有信号即用即取，不改变原信号）：

```python
f  = INSTR.view(instr)                 # 或简写 INSTR(instr)
op = f["opcode"]                       # ≡ instr[26:32]
op = f.opcode                          # 属性形式，等价
hi = f["opcode", "rd"]                 # 多字段拼接读 ≡ cat(instr[26:32], instr[21:26])
wr = INSTR.update(instr, rd=new_rd)    # 读-改-写 ≡ cat(instr[26:32], new_rd, instr[0:21])
```

**用法 (B) —— 声明即绑定**（声明处直接指示位域，之后 `x[...]`/`x....` 直接用）：

```python
# 传字典（须给 width）——最简洁
instr = m.input("instr", width=32, fields={
    "opcode": (31, 26), "rd": (25, 21), "imm16": (15, 0),
})
op   = instr["opcode"]                 # 下标形式
rd   = instr.rd                        # 属性形式
pair = instr["opcode", "rd"]           # 多字段拼接读

# 或传预先声明的 BitfieldSpec（width 从 spec 推断，可省略）
instr = m.input("instr", fields=INSTR)

# 寄存器同理，算术 / <<= 反馈照常工作
cnt = domain.signal(name="cnt", width=32, fields={
    "opcode": (31, 26), "rd": (25, 21),
})
cnt <<= cnt + 1                        # 委托给底层信号
m.output("opcode", wire_of(cnt["opcode"]))
m.output("rd",     wire_of(cnt.rd))

# 读-改-写（结果仍是绑定信号，可继续链式访问）
nxt = cnt.update(rd=new_rd)
m.output("nxt_rd", wire_of(nxt["rd"]))

# 取回底层信号 / 布局
raw_wire = instr.raw
layout   = instr.spec
```

**访问语义速查**：

| 写法 | 含义 |
|---|---|
| `x["opcode"]` / `x.opcode` | 字段读（字符串下标 / 属性） |
| `x["opcode", "rd"]` | 多字段拼接读（MSB-first，展开为 `cat`） |
| `x[3]` / `x[0:8]` | 原始位切片（整数 / `slice` 下标，Python 半开区间） |
| `x[i, 8]` / `x.lane(i, width=8)` | 缩放/按元素切片（ASL `x[i *: 8]`），≡ `x[i*8:(i+1)*8]` |
| `x.update(rd=v)` | 读-改-写，返回新的绑定信号 |
| `x + 1`、`x & y`、`x <<= v` | 委托给底层信号（`BitfieldSignal` 是 drop-in） |
| `wire_of(x)` / `m.output("p", x)` | 自动解包底层信号 |

> 冲突约定：字段名若与包装成员冲突（如 `update`/`raw`/`width`/`spec`），用字符串下标 `x["update"]` 访问；`x.update` 始终解析为读-改-写方法。

**发射的 MLIR** 现在把 `msb` 一并写进属性，便于核对提取范围：

```mlir
%v9  = pyc.extract %v5 {lsb = 26, msb = 31} : i32 -> i6
%v10 = pyc.extract %v5 {lsb = 21, msb = 25} : i32 -> i5
%v13 = pyc.concat (%v9, %v10) : (i6, i5) -> i11
```

---

## T2. 位掩码模式匹配（对应 ASL `x IN {'1xx0'}` / `opcode == '1(0)x0'`）—— ★★★（解码器刚需）

- **ASL 对照**（`arm_data_type.md` §5.7/§9.5-②）：`x` 为忽略位，括号内 0/1 也视为忽略位；`x == bit_mask ≡ x IN {bit_mask}`。ASL 用它把指令编码表写得与架构手册一字不差。
- **现状（0.40）**：
  - 信号级**无位掩码匹配**。`trace_dsl.py:123` 的 `matches` 是 trace 元数据匹配、`spec/builders.py:69` 的 `in_` 是端口方向 builder，**均与位掩码无关**。
  - **可复用基座**：`spec.DecodeRule(mask, match, ...)`（`spec/types.py:741-783` + `ruleset()` builder）是**编译期解码规则表**，语义接近但不作用于活信号。
- **缺口**：给 `Wire`/CAS 增加把掩码串编译为 `(sig & mask) == value` 的方法。
- **建议 API**：
  ```python
  hit = opcode.matches("1xx0")               # x 忽略；'1(0)x0'/'1(01)0' 括号内也忽略
  sel = opcode.in_({"000x", "0010", "11xx"}) # 命中集合 → 各 matches 的 or 归约
  ```
- [x] **T2.1（已完成）**：掩码串解析器 `parse_bitmask(pattern) -> (mask, value, width)` 落地纯模块 `python/pycircuit/src/pycircuit/bitmask.py`（无依赖，避免循环导入）：`0/1` 为 care，`x`/`X`/`-` 及**括号内任意位**为 don't-care（对应 ASL `'1(0)x0'`/`'1(01)0'`）；空格/`_` 为分隔符忽略；另有 `parse_bitmask_checked(pattern, width=)` 校验宽度。
- [x] **T2.2（已完成）**：`Wire.matches(pattern)`（`hw.py`）/ `CycleAwareSignal.matches(pattern)`（`v6.py`，保留 cycle）→ `(self & mask) == value`，返回 i1；宽度不一致报错。`ForwardSignal`/`StateSignal`/`BitfieldSignal` 经委托自动可用。
- [x] **T2.3（已完成）**：`.in_(patterns)` → 各 `matches` 的 `|` 归约；`.not_in_(patterns)` 取反（对应 ASL `IN !{...}`）；空集合报错。
- [x] **T2.4（已完成）**：门禁 `tests/test_bitmask_match.py`（25 项，全通过）：解析正确性（含括号/分隔符/`'1xx0'≡'1(0)x0'≡'1(01)0'`）、`matches`/`in_`/`not_in_` 与手写 `(sig&mask)==value` **字节级等价**、结果为 i1、宽度不符/非法字符/括号错配/空模式报错、CAS 保持 cycle。

### 使用示例

```python
# opcode : Wire / CycleAwareSignal（宽度须等于模式位数）
hit  = opcode.matches("1xx0")               # ≡ (opcode & 0b1001) == 0b1000，命中 1000/1010/1100/1110
hit2 = opcode.matches("1(0)x0")             # 括号内位也忽略，与 "1xx0" 完全等价
sel  = opcode.in_(["000x", "0010", "11xx"]) # 命中集合 → or 归约，返回 i1
miss = opcode.not_in_(["1xx0", "0011"])     # ASL IN !{...}

# 解码器风格：与架构手册一字对照
is_add = instr["opcode"].matches("100000")  # 配合 T1 位域视图
m.output("is_add", wire_of(is_add))
```

| 写法 | 含义 |
|---|---|
| `x.matches("1xx0")` | 单模式匹配 → `(x & mask) == value`（i1） |
| `x.in_([p1, p2, ...])` | 任一命中（or 归约） |
| `x.not_in_([...])` | 均不命中（ASL `IN !{...}`） |

模式语法：`0`/`1` 关心位；`x`/`X`/`-` 或**括号内任意位**为忽略位（ASL `'1(0)x0'`）；空格/`_` 为分隔符。模式位数（MSB-first）须等于信号宽度。

---

## T3. 类型安全枚举（对应 ASL `enumeration {...}`，不可与整数互转）—— ★★

- **ASL 对照**（`arm_data_type.md` §2.5/§9.5-④）：枚举必须声明为命名类型，**不可与整数互转**；防"拼错枚举/宽度不够/跨枚举比较"。
- **现状（0.40）**：**无 `PycEnum`**；枚举仍是裸整型常量 + 手算 width，防错价值缺失。
- **缺口**：展开期类型安全枚举，自动推 width、禁止与裸 int/跨枚举比较。
- **建议 API**：
  ```python
  class SRType(PycEnum):                  # 展开为最小宽度常量
      LSL = auto(); LSR = auto(); ASR = auto(); ROR = auto()

  alu_op = m.input("op", enum=SRType)     # width 自动 = ceil(log2(4)) = 2
  hit = alu_op.is_(SRType.LSL)            # 与裸 int / 其他枚举比较 → 展开期报错
  ```
- [x] **T3.1（已完成）**：`PycEnum` 基类落地 `python/pycircuit/src/pycircuit/enums.py`，基于 Python `enum.Enum`（**非 `IntEnum`**，成员不隐式转 int）+ 自定义元类 `_PycEnumMeta`。`auto()` 经 `_generate_next_value_` 产出 **0-based** 编码；`E.width`（类级，元类属性）与 `member.width`（成员级）= `max(1, max_code.bit_length())`（即 `ceil(log2(n))`）；`member.const(ctx)` 依 `ctx` 是 `Circuit`/`CycleAwareDomain` 返回定宽 `Wire`/`CAS` 常量；`E.bind(sig)` 把枚举类型贴到任意信号。编码越界（负数/非 int）在取 width/emit 时报错。
- [x] **T3.1b（已完成，贴近 ASL 语法）**：函数式构造器 `enumeration("Color", "RED GREEN BLUE")` —— 一行、只列名字，直接对应 ASL `type Color of enumeration {RED, GREEN, BLUE}`。名字可用**变长参数 / 列表 / 逗号或空格分隔的字符串**，等价于 `class Color(PycEnum): RED=auto();...`（复用同一元类与 0-based 编码，经 Python `enum` 函数式 API 落地）。空/非标识符/重名/空类型名均报错。需要显式编码或文档字符串时仍用 `class` 形式。
- [x] **T3.2（已完成）**：`m.input(name, enum=E)`（`hw.py`）与 `domain.signal(name=.., enum=E)`（`v6.py`）声明期即绑定枚举，width 自动取自 `E.width`（显式 `width=` 不一致则报错，且不可与 `fields=`/`shape=` 混用），返回 `EnumSignal`。寄存器可 `st <<= E.MEMBER`（跨枚举/裸值经 `_coerce_assign` 校验后加载编码常量）。
- [x] **T3.3（已完成）**：`EnumSignal.is_(E.MEMBER)` → `raw == const(code)`（返回 i1，与手写字节级等价）；`is_not` 取反；`==`/`!=` 为 `is_`/`is_not` 的别名。对**裸 int**（`op.is_(0)` / `op == 2`）或**跨枚举**成员（`op.is_(Color.RED)`）在展开期 `raise TypeError`（带枚举类型标签）。`wire_of()`/`m.output()`/`_to_wire()` 经 `__pyc_unwrap__` 自动解包。
- [x] **T3.4（已完成）**：门禁 `tests/test_pyc_enum.py`（25 项，全通过）：0-based 编码 / width 推导（含单成员、显式编码、越界/非 int 报错）；`member.const` 在 `Circuit`/`domain` 上的宽度与常量值；`is_`/`is_not`/`==` 与手写 `raw==code` **字节级等价**、结果 i1；裸 int / 跨枚举比较报错；`m.input(enum=)` 宽度推导 + 冲突报错 + 与 `fields=`/`shape=` 互斥；`domain.signal(enum=)` 寄存器 `<<= E.MEMBER` 与跨枚举赋值报错；`E.bind` 贴 `Wire`/`CAS`（保持 cycle）。

### 使用示例

```python
from pycircuit import PycEnum, auto, enumeration, wire_of

# 写法① 贴近 ASL：一行、只列名字（≡ ASL `type Color of enumeration {RED, GREEN, BLUE}`）
Color  = enumeration("Color", "RED GREEN BLUE")     # 也可 ("Color", "RED","GREEN","BLUE") 或列表
SRType = enumeration("SRType", "LSL LSR ASR ROR")

# 写法② class 形式（需要显式编码/文档字符串时用）
class SRType(PycEnum):                 # 0-based 编码，width 自动 = ceil(log2(4)) = 2
    LSL = auto(); LSR = auto(); ASR = auto(); ROR = auto()

SRType.width          # 2
SRType.ASR.value      # 2（成员编码；非 IntEnum，不隐式转 int）

# (A) 声明即绑定：输入端口按枚举宽度声明，返回 EnumSignal
op  = m.input("op", enum=SRType)       # width 自动 = 2
hit = op.is_(SRType.LSL)               # ≡ (raw == 0)，i1
m.output("is_lsl", hit)                # is_ 结果是 Wire，可直接 output
m.output("is_ror", op == SRType.ROR)   # == 是 is_ 的别名

# 类型安全：以下均在展开期 raise TypeError
# op.is_(0)              # 裸 int → 报错（枚举不与整数互转）
# op == 2               # 同上
# op.is_(Color.RED)     # 跨枚举 → 报错

# (B) 枚举寄存器：<<= 直接写成员，加载其编码常量
st = domain.signal(name="state", enum=SRType)
st <<= SRType.LSR                       # 载入编码 1（跨枚举赋值会报错）
m.output("in_lsr", wire_of(st.is_(SRType.LSR)))

# (C) 给已有信号贴枚举类型 / 取回底层信号 / 造常量
raw   = m.input("raw", width=2)
tagged = SRType.bind(raw)               # EnumSignal
c      = SRType.ROR.const(m)            # i2 常量 Wire（值=3）；domain 参数则返回 CAS
low    = tagged.raw                      # 取回底层 Wire
```

| 写法 | 含义 |
|---|---|
| `enumeration("E", "A B C")` | ASL 式一行声明（只列名字，0-based，`E.width` 自动） |
| `class E(PycEnum): A = auto()` | 等价 class 形式（需显式编码/文档时用） |
| `m.input("p", enum=E)` / `domain.signal(enum=E)` | 声明即绑定，返回 `EnumSignal` |
| `x.is_(E.MEMBER)` / `x == E.MEMBER` | 成员相等 → `raw == code`（i1） |
| `x.is_not(E.MEMBER)` / `x != E.MEMBER` | 成员不等（i1） |
| `x <<= E.MEMBER` | 枚举寄存器加载成员编码 |
| `E.MEMBER.const(m/domain)` | 定宽常量 `Wire`/`CAS` |
| `E.bind(sig)` / `x.raw` | 贴枚举类型 / 取回底层信号 |
| `x.is_(0)`、`x == 2`、`x.is_(其他枚举)` | 展开期 `TypeError`（防错） |

> 说明：`PycEnum` 基于 `enum.Enum`（非 `IntEnum`），成员**不**隐式转 int；`EnumSignal` 刻意只暴露 `is_`/`is_not`/`==`/`!=`（同枚举成员）与 `.raw`，位级运算请用 `.raw`。字段名保留：`width`/`raw`/`enum`/`is_`/`is_not`。

---

## T4. 断言式类型转换（对应 ASL `expression as ty`）—— ★★ ✅ **已完成**

- **ASL 对照**（`arm_data_type.md` §5.5/§9.5-⑤）：`as` = 断言表达式属于该类型（不满足即编码错误）+ 转换静态类型。
- **现状（0.40）**：只有静默 `trunc()`（`hw.py:416-423`），**无 `assert_fits`**；"我确信高位为零"的设计意图无法转成可仿真检查的契约。
- **缺口**：截断前挂一条仿真期断言。
- **拼写说明**：Python 的 `as` 是保留关键字，无法重载成 `x as ty`，故用带下划线的方法 `x.as_(width=..)`（与 T2 的 `in_`/`not_in_` 同一惯例），并提供 `assert_fits` 别名。
- **API**：
  ```python
  y = x.as_(width=4)             # 仿真期 pyc.assert(高位==0) 之后 trunc(4)；综合时退化为 trunc
  y = x.assert_fits(width=4)     # 等价别名（拼出断言意图）
  ```
- [x] **T4.1（已完成）**：`Wire.as_(...)`（`hw.py`）与 `CycleAwareSignal.as_`（`v6.py`，保留 cycle）支持**三种受检转换**（互斥，一次一种），对应 ASL `as bits(N)` 与 `as integer{...}`：
  - **取值集合（默认位置参数）** —— 值**直接作为位置参数**：`x.as_(2)` / `x.as_(2, 3)` / `x.as_([2, 3])`，断言 `x ∈ {…}`（各 `==` 的 `or` 归约），原样返回 `x`。这是最省字的写法（无需 `values=`/`[]`），单值即 `x == v`；等价关键字 `values=[...]` 与具名方法 `assert_in(values)` 均保留。
  - `range=(lo, hi)`（关键字）—— **值区间**：断言 `lo <= x <= hi`（无符号），原样返回 `x`；`assert_range(lo, hi)` 为具名方法。平凡边界（`lo==0` / `hi==2^w-1`）不发对应比较，全覆盖时不发断言。
  - `width=w`（关键字）—— **宽度收窄**：断言高位切片 `x[w:] == 0`（截掉的高位全零、装得下）经 `m.assert_` + `trunc(w)`；`assert_fits(width=)` 为别名。
  - **消歧**：位置参数**只归取值集合**，`width=`/`range=` 必须用关键字，故 `x.as_(2)` 只表示"断言 == 2"，不会被误解成"2 位宽"；三类互斥，一次一种。`BitfieldSignal`/`ForwardSignal` 经属性委托自动可用；值断言不改宽度/值，只挂契约。
- [x] **T4.2（已完成）**：宽度收窄的数据路径**就是** `trunc`——断言是可丢弃的副作用语句，综合路径下丢掉 `pyc.assert` 即退化为纯 `trunc`（零硬件代价契约）。等宽为 no-op（不发断言、原样返回）；加宽报错（提示改用 `zext`/`sext`）；值区间/取值集合断言零数据路径改动。
- [x] **T4.3（已完成）**：门禁 `tests/test_assert_fits.py`（24 项，全通过）：收窄发射 `extract 高位 + eq 0 + pyc.assert + trunc` 且与手写 `assert_(x[4:8]==0)+trunc` **字节级等价**；位置参数简写 `x.as_(2)`/`x.as_(2,3)`/`x.as_([2,3])` 与 `values=[...]` **字节级等价**、单值即 `x==v`；`range=`/`values=` 与手写 `assert_(x.uge&x.ule)` / `assert_((x==a)|(x==b)…)` **字节级等价**；平凡边界省略比较、全覆盖不发断言；`as_`≡`assert_fits`、`assert_range`/`assert_in` 具名方法；自定义 msg；等宽 no-op；加宽/零宽/空值/越界/多种约束并用/位置与关键字并用/零约束均报错；CAS 保持 cycle、可在 `compile_cycle_aware` 中使用。

### 使用示例

```python
# ① 取值集合，值直接作为位置参数（ASL: y = x as integer{2}）—— 最省字
y = x.as_(2)                             # 断言 x == 2，原样返回 x
y = x.as_(0, 5, 10)                      # 断言 x ∈ {0,5,10}
y = x.as_([0, 5, 10])                    # 传列表也行
y = x.as_(values=[0, 5, 10])            # 等价关键字写法
y = x.assert_in([0, 5, 10])             # 具名方法

# ② 值区间（ASL: y = x as integer{2..9}）
y = x.as_(range=(2, 9))                   # 断言 2 <= x <= 9（无符号），原样返回
y = x.assert_range(2, 9)                  # 具名方法

# ③ 宽度收窄（ASL: y = x as bits(4)）
y = x.as_(width=4)                        # 断言 x 高位为 0，再截断到 i4
y = x.as_(width=4, msg="opcode 必须 4 位")  # 自定义断言信息
y = x.assert_fits(width=4)                # 等价别名

# 宽度收窄发射的 MLIR（i8 -> i4）：高位切片校验 + 断言 + 截断
#   %hi = pyc.extract %x {lsb = 4, msb = 7} : i8 -> i4
#   %ok = pyc.cmp %hi, %zero {predicate = "eq"} -> i1
#   pyc.assert %ok {msg = "as_: value does not fit in 4 bits"}
#   %y  = pyc.trunc %x : i8 -> i4         # ← 综合仅保留这一步
```

| 写法 | 含义 |
|---|---|
| `x.as_(v)` / `x.as_(v1, v2, …)` / `x.as_([…])` | **取值断言（默认位置参数）**：断言 `x ∈ {…}`，原样返回（单值即 `x==v`） |
| `x.as_(values=[...])` / `x.assert_in([...])` | 同上的等价关键字 / 具名写法 |
| `x.as_(range=(lo, hi))` / `x.assert_range(lo, hi)` | 断言 `lo<=x<=hi`（无符号），原样返回 |
| `x.as_(width=w)` / `x.assert_fits(width=w)` | 宽度收窄的**受检**转换：断言高位为零 + `trunc(w)` |
| `..., msg="..")` | 自定义断言失败信息 |
| 等宽 `x.as_(width=x.width)` | no-op，原样返回（不发断言） |
| 加宽 `x.as_(width>x.width)` | 报错（改用 `zext`/`sext`） |

> **位置参数只归取值断言**；`range=`/`width=` 必须用关键字，故 `x.as_(2)` 明确表示"断言 == 2"（不会被当成 2 位宽）。三类**互斥**，一次一种。与静默 `trunc()` 的区别：`trunc` 悄悄砍高位；`as_` 留一条可仿真验证的契约，综合时可丢弃。取值断言（位置参数/`values`/`range`）**不改宽度/值**，只挂契约。当前宽度收窄只断言**无符号**意义的"装得下"（高位全零）；有符号收窄请显式配合 `as_signed`。

---

## T5. 缩放切片（对应 ASL `x[idx *: len]`）—— ★ ✅ 已完成

- **ASL 对照**（`arm_data_type.md` §5.1/§9.5-③）：`x[idx *: len] ≡ x[(idx*len) +: len]`，按元素粒度索引。
- **现状（0.40）**：无 `Wire.lane()`，需手写 `x[i*8:(i+1)*8]`；`Vec` 已覆盖多数"按元素取"场景。
- **缺口**：给扁平总线一个按元素取的糖（残余场景）。
- **API**（两种等价写法）：
  ```python
  lane = bus[i, 8]                # 下标糖（元组 (index, width)），最接近 ASL x[i *: 8]
  lane = bus.lane(i, width=8)     # 显式函数式；两者 ≡ bus[i*8:(i+1)*8] = bus.slice(lsb=i*8, width=8)
                                  # i 为展开期 Python int；lane i 占 bit [i*8, i*8+7]
  ```
  > 注：ASL 的 `x[i *: 8]` 里 `*:` 在 Python 中是 `SyntaxError`（宿主语言限制），故用元组下标 `x[i, 8]` 编码；它与位切片 `x[a:b]`、`BitfieldSignal` 的多字段读 `x["a","b"]` 无歧义（不同下标类型/不同类）。
- [x] **T5.1**：`Wire.lane(idx, *, width)` → `self.slice(lsb=idx*width, width=width)`（`hw.py`）；`idx`/`width` 强制 `int`；`width<=0`、`idx<0`、`idx*width+width > 信号宽度` 均在展开期抛 `ValueError`。`CycleAwareSignal.lane`（`v6.py`）转发到底层 Wire 并保留 cycle 标签。**纯前端语法糖**，最终仍落到 `pyc.extract`，后端零改动。
- [x] **T5.1b**：下标糖 `x[i, w]`——`Wire.__getitem__` / `CycleAwareSignal.__getitem__` 识别二元组 `(index, width)` 并委托 `lane`；非二元组元组抛 `TypeError`。
- [x] **T5.2**：门禁 `tests/test_lane_slice.py`（28 项，全通过）：多组 `lane(i,width=w)` 与手写半开区间 `bus[i*w:(i+1)*w]` / `slice(lsb=,width=)` 字节级 MLIR 等价、结果宽度、末元素恰好对齐 MSB 不报错、越界/部分溢出/零宽/负宽/负索引报错、CAS 版等价 + 保留 cycle + 越界报错、元组下标 `x[i,w]` 与 `.lane` 等价（Wire+CAS）、元数错误 `TypeError`、元组越界报错。

---

## T6. 文档级约定（零实现成本）—— ★ ✅ 已完成

- [x] **T6.0（前置补齐）**：Wire 显式比较此前只有 `ult/slt/ugt/ule/uge`（有符号只有 `slt`），现补齐 **`sgt/sle/sge`**（`hw.py`，镜像无符号集：`sgt`≡翻转 `slt`、`sle`≡`~sgt`、`sge`≡`~slt`，结果 i1）。`CycleAwareSignal` 同步补齐**全套**显式比较 `ult/ugt/ule/uge/slt/sgt/sle/sge`（`v6.py`，对齐 cycle 后委托底层 Wire、保留 cycle 标签），使"显式签名比较"约定在 cycle-aware 主建模 API 上也可用。**纯前端语法糖**，最终统一落到带 `slt`/`ult` predicate 的 `pyc.cmp`，后端零改动。门禁 `tests/test_signed_compare.py`（17 项，全通过）：`sgt/sle/sge` 与手写等价、保留正确 signed/unsigned predicate、结果 i1、接受 int 操作数、CAS 全套暴露 + 与 Wire 等价 + 保留 cycle。
- [x] **T6.1**：编程规范——签名不能从上下文一眼看出的比较,一律用显式 `.ult()/.ugt()/.ule()/.uge()` 或 `.slt()/.sgt()/.sle()/.sge()`,不要依赖 `<`/`>`（其签名取决于操作数 `signed` 标志,易误判）。对应 ASL 强制 `UInt/SInt` 包装。
- [x] **T6.2**：编程规范——`reduce_sum`/累加/移位左扩等易溢出处显式写 `width=`,或用 T4 的 `x.as_(width=w)` 加装得下断言。对应 ASL"截断必须显式"。
- [x] **T6.3**：编程规范——架构常量集中命名分层（`constant` 派生量 vs `config` 参数量,集中一处定义、按意图分组）。对应 ASL `constant`/`config` 意图分层。

> 约定速记：**比较显式签名、截断显式宽度、常量集中分层**。前两条已有对应 API（T6.0 补齐的显式比较、T4 的 `as_(width=)`）可直接落实。

---

## 建议开工顺序

1. ~~**T2 位掩码匹配** + **T1 位域视图**（解码器双刚需）~~ ✅ **均已完成**。
2. ~~**T3 类型安全枚举**（防错价值高，独立）~~ ✅ **已完成**。
3. ~~**T4 断言式转换**（依赖 assert/诊断机制）~~ ✅ **已完成**。
4. ~~**T5 缩放切片**~~ ✅ **已完成**。
5. ~~**T6 文档约定 + 补齐 sgt/sle/sge**（收尾）~~ ✅ **已完成**。

> **T1–T6 全部完成**。全量门禁 312 passed / 2 skipped。

每项遵循 AGENTS.md **gate-first**：先加/扩门禁（发射等价 + 语义），再落地实现；所有特性均为前端语法糖，**不触碰 MLIR 方言语义**。

---

**参考**

- `docs/arm_data_type.md` §9（ASL ↔ PyCircuit 对照与建议）
- `python/pycircuit/src/pycircuit/hw.py`（`Wire`/`Vec`/`cat`/`zext`）、`v6.py`（`CycleAwareSignal`/`mux`）、`record.py`、`spec/types.py`（`StructSpec.field_slices`/`DecodeRule`）
- `docs/rfcs/pyc6-decisions.md` Decision 0008（spec 分层类型系统）
