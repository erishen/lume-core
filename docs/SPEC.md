# Lume 语言规格（SPEC）

这份文档是**规范性**的：它写明 Lume 「是什么」而不是「怎么实现」。实现改动后若与本文档冲突，
就是**实现错了**，需要改实现或走下面的流程改本文档，而不是让文档去追认实现。

- 它**不替代**实现，也不描述代码组织（那是 [ARCHITECTURE.md](ARCHITECTURE.md)）。
- 它**不承诺**宿主专属能力。宿主树 `lume` 的额外内建（`sql_*` 等）不在此列，
  见 [LUME.md §不承诺什么](LUME.md#不承诺什么)。
- 破坏性变更进 [CHANGELOG.md](../CHANGELOG.md)，并在下文标 **（规范变更）**。

本文档每一条都对着一个可执行的事实写成：能跑的例子在 `examples/`，能自动验证的例子在
`tests/native-consistency.lume`（三后端差分）与 `tests/smoke.c`（解释器自带计数，运行时打印
`N tests, M failed`）。**这两个数字都不要在本文档里抄写**——抄过一次就漂了：本文档曾长期写
「163 个断言」，而实际已是 180。抄数字的文档必然过时，要看真值就跑一遍。
改动本文档时，**先改测试、再改实现**，否则等于把没验证的承诺写进规格。

**这一版覆盖什么**：数值类型、求值顺序、类型系统与成员访问、`Result` / `?`、
**控制流与闭包**、内建（清单以 `src/typecheck.c` 的 `LUME_BUILTIN_NAMES` 为准——本节 §6 的表格
**没有列全**，例如 `run` / `replace` 未在此出现，5 个 `http_*` 也不在表里）、模块、三后端一致性。

**没覆盖**：`import` 之外的模块边界细节（`export` 的运行时可见性只在 §7.3 提了一句）、
`env` / `files` 等内建的错误路径全集、以及任何**性能或资源上限**的承诺
（迭代器长度的快照行为见 §8.1 第 5 条，那条已于本提交修复，原生后端不再因 `push` 而 OOM）。
这些以实现为准，改动时欢迎顺手补进本文档。

已登记的资源上限（刻意边界，不是缺陷）：解析器嵌套深度 `PARSER_MAX_DEPTH = 200`
——表达式链与块每进一层计一次，超限报 `expression nests too deeply`。这是防恶意
输入的护栏：递归下降每层括号吃约 12 个 C 帧，无上限时约 500 层括号即可打爆解析
线程栈（libFuzzer crash-eb4ae2，2026-10-18）；200 层约当 2.4k 帧，对 8 MB 主栈
是零头，而真实程序的嵌套从不过十。

---

## 1. 数值类型

只有两个数值类型：`int` 与 `float`。

### 1.1 int 是 i64（64 位二补码）

```
int 的取值范围：-9223372036854775808 .. 9223372036854775807
```

字面量按十进制解析，**不经过浮点**，所以超过 2^53 的整数**必须精确**：

```lume
print(9007199254740993);            // 9007199254740993，不是 ...992
print(9223372036854775807);         // 最大值
print(-9223372036854775808);        // 最小值
```

> 2^53 = 9007199254740992 是二进制能精确表示整数的上界。旧实现把 int 存成 C `double`，
> 于是 `9007199254740993` 被静默改成 `...992`，且类型检查器不报错。现在 int 是独立的
> i64 表示，这条不再可能发生。见 [CHANGELOG](CHANGELOG.md) 的「int 是 i64」条目。

**越界字面量是错误，不静默取模**：超过 i64 范围的整数字面量在词法阶段报错。
唯一的例外是 `2^63` 本身——它作为一元负号的操作数是 `-9223372036854775808`（最小值），
此时合法；单独写 `9223372036854775808` 报错（没有任何值能表示它）。

### 1.2 溢出按二补码回绕

int 运算**回绕**，不报错、不饱和。这与两个原生后端发出的无修饰 `add i64` / `mul i64` 一致：

```lume
let m = 9223372036854775807;
print(m + 1);                        // -9223372036854775808
print(m * 2);                        // -2
print(0 - m - 1);                    // -9223372036854775808
```

> 饱和（停在 `m`）曾经是解释器的行为，与后端分叉。现在三后端一致。

### 1.3 float 是 f64，显示用 %g

`float` 是 IEEE-754 双精度。`str()` / `print()` / `json()` 一律用 C 的 `%g`，所以
**尾零不保留**、位数多时进科学计数法：

```lume
print(str(1.0));                     // 1
print(str(0.5));                     // 0.5
print(str(1.0 / 3.0));               // 0.333333
```

`int` 则**始终**用十进制整数字面量输出（见 1.1 的 15 位例子），不受 `%g` 影响。

### 1.4 混合运算：任一方是 float 则走 float

`int` 与 `float` 混合运算时，`int` 提升为 `float`（类型检查器允许这种加宽）：

```lume
print(str(1 + 2.5));                 // 3.5
print(str(2.0 * 3));                 // 6
```

⚠️ 提升会丢精度：`int` 超过 2^53 后参与混合运算，结果只保留 53 位有效数字。这是
`float` 的固有性质，不是 bug；需要精确整数运算就不要引入 `float`。

### 1.5 比较跨 int/float

`int` 与 `float` 可以直接比较（`1 == 1.0` 为 `true`）。两个 `int` 比较走 i64，不经过
浮点，因此大整数比较是精确的：

```lume
print(str(1 == 1.0));                // true
print(str(1 < 1.5));                 // true
```

**字符串的 `==` / `!=` 是内容比较**（逐字节，同 `strcmp` 语义，解释器与两个原生后端
一致——原生侧走 `lume_str_eq`，不是指针比较；2026-10-12 修复，此前两个原生后端发
的是 i8* 指针 icmp）。`<` / `<=` / `>` / `>=` 对字符串**未定义**（不保证三后端一致），
不要用。

### 1.6 `/` 是浮点除法，`%` 是整数取模

- **`/` 永远是浮点除法**，即使两边都是 `int`：`7 / 2 == 3.5`。想要整数除法需自行约定。
- 除以 `0` 报 `division by zero`；`% 0` 报 `modulo by zero`。
- **`%` 两侧都是 `int` 时结果仍是 `int`**，且沿用 C 的符号约定（结果取**被除数**的符号）：

```lume
print(str(-7 / 2));                  // -3.5
print(str(-7 % 3));                  // -1
print(str(7 % -3));                  // 1
```

- **`%` 作用于 `float` 是错误**：`'%' does not apply to floats`。三后端消息已统一
  （原先 `--compile-llvm` 在更早的类型推断处报 `print() cannot print this value`，已修，见
  §8.1 第 1 条）。
- ⚠️ 整数 `%` 若**任一侧**是 `int` 而溢出（如 `LLONG_MIN % -1`），行为未定义。

### 1.7 一元负号保持数值类型

`-x` 的类型与 `x` 相同：`-7` 是 `int`，`-7.5` 是 `float`。

```lume
let a = -7;
print(str(a % 3));                    // -1，不是 float 取模错误
```

⚠️ `LLONG_MIN` 取负回绕到自身（`-(-9223372036854775808)` 仍是 `-9223372036854775808`），
与 1.2 的回绕规则一致，不是错误。

### 1.8 `==` / `!=` 是结构性比较（2026-10-12）

`==` / `!=` 对**容器**是**结构比较**，不是指针比较、更不是恒假。此前三后端各自为政
（解释器对 map/list 恒 `false`，两个原生后端发 i8* 指针 icmp），现已统一：

| 等价类 | 成员 | `==` 语义 |
| --- | --- | --- |
| numeric | `int` / `float` | 数值比较，int/float 之间加宽（§1.5） |
| string | `string` | 逐字节内容比较（`lume_str_eq`） |
| bool | `bool` | i1 比较 |
| container | `list` / `struct` / `enum` / `Result` | **结构比较**（见下） |
| func | `func` | 指针身份（同一个值才相等） |
| null | `null` | 只与 i8* 类（string / 容器 / func / any / null）比较：空指针身份 |
| any | `any` | 通配：与任何一类按该类的规则比较（运行时探 kind） |

- **跨等价类静态拒绝**（类型检查期，`'==' does not apply to X and Y`）：
  `1 == "x"`、`true == 1`、`[1] == 1`、`{"a":1} == "x"`、`1 == null`、`true == null`。
  `null` 不与 `int` / `float` / `bool` 比较——它们永不为 null（原生后端本就拒绝构造
  null 标量）。`any` 是唯一的通配类。
- **结构比较语义**（解释器与原生后端逐字节一致，rt.c 的 `lume_value_eq`）：
  - `list`：同长 + 逐元素递归比较（顺序敏感）；
  - `map`：同键数 + 每个键在对方存在且值相等，**键顺序无关**；
  - `enum`：就是带 `"$"` 键的 map，tag 与载荷一起比（`"$"` 只是又一个普通键）；
  - `struct`：**只比结构、不比类型名**——同字段集即按字段比（字段也对齐）；
  - 循环结构不支持（同 print）。
- **原生后端的发射**：string×string → `lume_str_eq`；容器×容器 →
  `lume_value_eq`（解释器的 values_equal 的原生镜像）；any×i8* 类 →
  `lume_any_eq`（运行时探 kind：双容器结构化、容器 vs 非容器恒不等、其余按字节）；
  **命名 struct 是唯一的例外形状**——它降为类型化聚合体 `%P = type {...}` 而不是
  i8* 堆 map，因此两个命名 struct 按字段 GEP + load 递归比较（AND over fields），
  字段集不匹配是常量答案 `false`（`!=` 为 `true`），不是错误。
- **已知缺口**（都不是相等性本身的语义问题）：
  - 命名 struct 与 map（匿名结构体）之间的 `==` 原生端拒绝（两种表示之间没有运行时
    桥），解释器按结构比较；
  - 字符串的 `<` / `<=` / `>` / `>=` 仍未定义（§1.5 末段），只有 `==` / `!=` 有保证；
  - 异构列表字面量（`[1,"a"]`）与全 null 列表的原生 for-in 不可用（元素类型降级为
    `list<any>`，与异构同类拒绝）；`null` 容器值本身**已可构造**（2026-10-13：null
    骑 obj 槽空载荷，print/str/eq/get 三后端一致渲染 `null`），但裸 null 绑定到
    容器类型标注（`let x: list = null`）原生端仍拒绝（变量类型无法推断）。

---

## 2. 求值顺序

所有求值点**从左到右**，无副作用的纯表达式除外（编译器不承诺重排）。

- 函数实参：从左到右。
- `map` 字面量的值：从左到右。
- 列表字面量的元素：从左到右。
- 二元运算符的两个操作数：**先左后右**。

```lume
func t(s: string) { print(s); return s; }
t("A") + t("B");                     // 打印 A 再打印 B
```

⚠️ 这条约束的是**可观测顺序**。依赖它会写出难维护的脚本，且一旦实现引入优化即可能失效。

---

## 3. 类型系统

### 3.1 标注可选，缺省即「松散」

类型标注是**可选**的。不写标注的参数、返回值与变量，其类型是内部的 `any`（不检查）。
`any` **不是类型关键字**——写 `func f(r: any)` 会报 `unknown type 'any'`。想表达「任意值」
就别写标注。

**函数类型标注 `fn (参数类型表) => 返回类型`**（2026-10-16 起，§4.8）：为闭包变量写出
lambda 的参数/返回类型，`let f: fn(int) => int = (x) => x * 2;`——标注为未注参数的
lambda 提供类型，类型不匹配时三后端一致拒绝（如 `fn(int) => int` 配 `(x: string) => x`）。

### 3.2 int 向 float 加宽合法，反向不合法

`type_compat` 允许 `int → float`，不允许 `float → int`（这需要显式 `int(...)` 转换）。
`int` 字面量可赋给 `float` 槽位；反过来不行。

### 3.3 所有类型可空

任何类型都可为 `null`；`null` 兼容任何槽位。

### 3.4 `strict_bools`：`and` / `or` / `not` 是严格布尔

`and` / `or` / `not` 要求 `bool`，不把真值性隐式转换当布尔用。所以 `if 1 { }` 这类写法
是类型错误而不是「真值判断」。

### 3.5 成员访问 `.length` / `.len` 与 struct 字段

点号用于**两种**东西，取决于左侧是什么：

- **struct 值** → 读**声明的字段**。字段名可以任意取，包括 `len`。
- **字符串 / 列表 / 匿名 map** → 只提供 `.length`（元素数 / 键数 / 字节长度）。

```lume
print(str("abc".length));// 3
print(str([1, 2].length));             // 2
print(str({ a: 1 }.length));           // 1
print(str((5).length));                // 报错：cannot read field 'length' on this type
```

🔴 **`.len` 与 `.length` 在 struct 上行为不同，这是最容易踩的一条**：

```lume
type Sized = { len: int, w: int };
let s: Sized = { len: 7, w: 2 };
print(str(s.len));                     // 7  ← 读声明的字段
print(str(s.w));                       // 2

let m = { a: 1, b: 2 };
print(str(m.len));                     // 2  ← 匿名 map：键数
print(str(m.length));// 2
```

即：**匿名 map 的 `.len`/`.length` 都给键数；命名 struct 的 `.len` 给字段值。**
两个 emitter 都按「struct 走字段表、map 走键数」解析，所以这不是解释器的实现差异。

字段访问**支持任意深度**：`o.i.v`。`type` 声明的字段是**结构性的**，但运行时
**不做类型检查**——字段名写错会得到 `cannot read field 'x' on this type`。

### 3.6 struct 是「命名的 map」，不是记录类型

`type` 声明的 struct 在运行时**就是一个 map**，加上一张字段表：

```lume
type P = { x: int, y: int };
let p: P = { x: 1, y: 2 };
print(str(p.x));                     // 1
print(str(len(p)));                  // 1  ← len 仍是键数
print(str(keys(p)));                 // ["x","y"]
```

- **字面量必须与声明严格一致**：缺字段报 `type P is missing field 'y'`，多字段报
  `type P has no field 'z'`，类型不符报 `struct field: cannot use a value of type string
  where int is expected`。这三类都在**类型检查阶段**报错，不在运行期。
- ⚠️ **struct 赋值是引用语义，不是值语义**：

  ```lume
  type P = { x: int };
  let a: P = { x: 1 };
  let b: P = a;                     // 复制的是引用
  b.x = 9;
  print(str(a.x));// 9← a 被改了
  ```

  要真正的副本得自己重建：`let b: P = { x: get(a, "x") };`。
- **没有方法语法**。`type P = { get(): int }` 是语法错误（`expected :, got (`）。想在
  struct 上「挂函数」就把函数字段放进 map，用 `get(o, "f")()` 调（见 §6.1）。
- 字段名可以和内建名同名（`{ len: int }` 合法），此时 `.len` 读字段而不是键数（见 §3.5）。

### 3.7 enum 声明与 match 表达式（借鉴 Lean inductive 的日常面）

```lume
enum Color { Red | Green | Blue }
enum Shape { Circle(r: float) | Rect(w: float, h: float) }
```

- **值的表示**：一个 enum 值就是一个 map，键 `"$"` 存变体名（`"$"` 不是合法标识符，
  与任何用户字段零碰撞），载荷字段按声明名各存一份。`print(Red)` 与
  `print({ "$": "Red" })` 同形——枚举值可进列表、可进 map、可当参数传。
- **构造名是全文件的构造器**：`Red`、`Circle(1.0)` 在任何表达式位置可用；解析器在
  解析后把 `V(args)` / 裸 `V` 改写成构造节点。构造名**不可被遮蔽**：let / 参数 /
  函数与变体同名一律拒绝（`'Red' shadows an enum constructor (variant names are
  reserved across the file)`）。
- **载荷只限四种标量**：`int` / `float` / `string` / `bool`。列表、map、struct 载荷
  在类型检查期拒绝。
- **`match` 是表达式**，不是语句：`let w = match (c) { Red => "stop", ... };` 可以
  出现在 return / let 初值 / 参数位 / 三元任意一侧。
- **穷尽性是编译期检查**：臂必须覆盖全部变体，或以 `_` 收尾（通配臂**吸收**其余变体，
  其绑定数为 0）。缺臂报 `match on 'Color' does not cover variant 'Blue' (add an
  arm or '_')`。同一变体出现两个臂报 `duplicate match arm`。
- **臂内绑定**：`Circle(r) => ...` 的 `r` 只在自己的臂里可见（解释器用臂级环境，
  原生后端发射期注册并在臂尾截断）。绑定数与变体载荷数不符报
  `variant 'Rect' carries 2 payload field(s), arm binds 1`。
- **or 模式**：`Red | Green | Blue => ...` 多变体共臂。覆盖性按各 alternative 的
  变体集**合并**计算，`_` 也可参与 or。带载荷时各 alternative 必须绑**同名且同型**
  的名字（比臂体统一更严：int/float **不加宽**，要加宽就写两条臂），分别报
  `or-pattern alternatives must bind the same names ('x' vs 'y')` 与
  `or-pattern binding 'x' has conflicting types ('int' vs 'string')`。同一 or
  列表内重复变体报 `duplicate match pattern 'Red'`。
- **匹配守卫**：`Circle(r) if r > 1.0 => ...`。守卫在绑定之后、臂体之前求值，
  条件为假**落下一臂**（不是错误）；条件必须是 bool（`expected bool, got float`）。
  **带守卫的臂不计入穷尽性**（Lean 同规）——所有臂都有守卫时必须另有 `_` 或
  无条件臂兜底（`match on 'Color' does not cover variant 'Red'`）；同理，
  **重复臂判定只看无条件臂**（`A if g => .., A => ..` 合法）。受守卫的末臂在
  伪造 map（没有 `"$"` 键的值）上落空：解释器报 `no match arm`，原生后端发
  `unreachable` trap——三后端都不允许静默穿过。
- **`if let`**：`if let P [| P] = e { ... } else { ... }`，语义 = 单臂 match 且
  **不要求穷尽**，但是**语句**（不是表达式）。绑定只在 then 块内可见（块作用域，
  同 match 臂）；支持 or 模式，**不支持守卫**；scrutinee 须是 enum 值（含 any），
  非 enum 报 `if-let needs an enum value (got int)`；未命中走 else（可省略），
  else-if 链可用。块内给外层局部赋值是**更新外层变量**（三后端一致）。
- **臂体统一**：所有臂按三元同一规则收拢——同型即该型，int/float 混合加宽为 float，
  含 `any` 则整个 match 为 `any`（原生后端拒绝发射 `any` 结果槽，要求至少一个可
  静态定型的臂）。
- **原生后端的读侧**：`xs[0]`（enum 元素列表）、for-in、作为 `?` 展开结果——枚举值
  一律按不透明 `i8*`（即 map 指针）读写，与解释器的 map 表示逐字节一致
  （tests/native-consistency.lume 的 enum 探针段钉死）。
- **跨模块**：enum 声明只在**本文件**可见（`export enum` 不存在）；导入方拿不到
  变体构造器，也拿不到类型名。这是 MVP 限制（§8.1）。
- **枚举值参与 `==`**：enum 值就是 map，`==` 按 §1.8 的结构规则比较——`"$"` 只是
  又一个普通键，tag 与载荷一起比（`Red != Green`、`Circle(2.0) == { "$": "Circle",
  "r": 2.0 }` 均为 `true` 侧成立）。此前「枚举值不参与 `==`（未定义）」的立场随
  2026-10-12 的结构相等落地移除。

### 3.8 字符串插值 `s!"..."`（2026-10-13，借鉴日常脚本语言）

- 形如 `s!"文本 {expr} 文本"`：双引号前带 `s!` 前缀即进入插值态。普通 `{` `}` 是字面
  花括号（用 `{{` `}}` 转义出字面花括号）。单个 `{` 进入**表达式态**：括号深度追踪、
  字符串字面量内的 `}` 不算深度；表达式段交给完整 parser 递归解析——故嵌套插值、
  索引、调用乃至 `with` 更新都能出现在段里。表达式经 `str()` 转字符串后与字面量段
  拼接；无插值时退化为普通字符串字面量。
- 转义序列（`\n` `\t` `\\` `\"` 等）沿用既有规则；BOM/非法 UTF-8 仍按既有 lexer 行为。
- 段内表达式的结果经 `str()` 统一转为字符串后拼接，因此 `s!"v={1+2}"` 得到 `"v=3"`、
  `s!"t={1==1}"` 得到 `"t=true"`、`s!"n={s!"{7}"}"` 得到 `"n=7"`（嵌套插值）。
- **三后端一致**：解释器、文本 IR（`--compile-text`）、libLLVM（`--compile-llvm`）
  对插值的输出逐字节相同（tests/native-consistency.lume 的插值探针段钉死）。`str()`
  对容器（list/map/struct）的格式化在两原生后端尚未实现，故探针段回避
  `str(container)`——这属原生 `str()` 既有缺口，非插值特性本身。

### 3.9 结构体更新 `{ base with ... }`（2026-10-13，借鉴 Lean 的 structure update）

- 形如 `{ base with f: e, ... }`：以现有 map/struct 为基底，浅拷贝其全部槽后按序覆盖
  指定字段，产出**新**值（基底原值不变，与解释器语义一致）。`base` 可以是变量、字面量、
  调用、成员访问或嵌套的 `{...with...}`。
- **map 基底**：结果为匿名 map（运行时堆 map），覆盖/新增键；读回用 `get(m, "k")`。
- **命名 struct 基底**（基底必须有类型标注，使更新保持为类型化聚合而非匿名 map）：
  结果为同类型 struct，按字段名覆盖；读回用 `.f` 字段访问。覆盖值的类型须与字段声明
  兼容，否则类型检查拒绝。
- **拒绝的形状**：标量/列表/函数/enum/Result 基底报 `'with' update requires a map or
  struct base`；未知字段报 `unknown field 'f' in struct update (T)`；命名 struct 上重复
  字段报 `duplicate field 'f' in with update`；空更新（无字段）报 `with update needs at
  least one field`；字段类型不匹配报 `struct field: cannot use a value of type ...`。
- **三后端一致**：map 基底与命名 struct 基底（基底带类型标注）的更新，在解释器与两原生
  后端输出逐字节相同（tests/native-consistency.lume 的更新探针段钉死）；解释器独有的
  断言（含未标注基底被当作 map 走 `.f` 动态读）见 tests/interp2.lume（仅解释器）。

---

## 4. 控制流与闭包

### 4.1 `for` 只遍历列表与 map

```lume
for (let x in [1, 2, 3]) { print(str(x)); }   // 逐元素
for (let k in { a: 1 })  { print(k); }        // 逐**键**
for (let i in range(0, 3)) { print(str(i)); } // range 结果是列表
```

range 的元素类型静态可推：全 int 实参 → `int[]`，含 float 实参 → `float[]`（解释器与两原生后端
同一条 `infer` 规则）；实参没有静态数值类型时原生编译期给「cannot infer」族诊断。

⚠️ **字符串不能迭代**：`for (let c in "ab")` 报 `for-in expects a list or map`。
⚠️ **map 的遍历顺序是插入顺序**，不是排序序：`for (let k in { b: 1, a: 2 })` 依次打出
`b`、`a`。要确定顺序请先 `keys()` 再排序。

### 4.2 🔴 循环变量**泄漏到外层作用域**

`for (let x in ...)` 的 `x` 在循环结束后**依然存在**，且值是**最后一次迭代的值**：

```lume
for (let x in [1, 2]) { }
print(str(x));                     // 2    ← 不是 undefined
```

这意味着循环变量不是块作用域。**副作用**：嵌套同名循环会**互相踩**：

```lume
for (let i in [1, 2]) {
  print("outer-before " + str(i)); // 1
  for (let i in [8, 9]) { print("inner " + str(i)); }
  print("outer-after " + str(i));  // 9← 被内层覆盖了，不是 1
}
```

⚠️ 内层 `for (let i ...)` 与外层**共用同一个变量**。需要独立计数就用别的名字。

（嵌套同名 `for` 的两个循环变量现在各占独立 slot，解释器、`--compile-text`、`--compile-llvm`
三后端行为一致，见 §8.1 第 4 条。）

### 4.3 `break` / `continue` 只作用于最内层循环

两者都存在，且只跳一层：

```lume
for (let x in [1, 2, 3]) { if (x == 2) { break; }    print(str(x)); }   // 1
for (let x in [1, 2, 3]) { if (x == 2) { continue; } print(str(x)); }   // 1 3
for (let a in [1, 2]) { for (let b in [1, 2]) { break; } print("outer"); } // outer outer
```

没有带标签的 `break`（`break outer` 之类），也不支持 `do...while`、`repeat...until`。

### 4.4 迭代中修改被迭代的列表

`for` 在循环入口快照**绑定时的长度**，`push` 进去的元素不会被遍历到（与解释器一致）：

```lume
let l = [1];
for (let x in l) { l = push(l, 9); }
print(str(len(l)));                // 2
```

两个原生后端在修复前会因「每轮重读长度」而无限循环、被 OOM 杀掉（退出码 137，见 §8.1
第 5 条）；现在长度在循环入口快照，与解释器走同一份边界。

⚠️ 一次迭代中**替换整个列表**（`l = [...]`）时，遍历继续用旧迭代器还是新值，本spec 不作承诺。

### 4.5 `while` 要求严格布尔条件

```lume
let i = 0;
while (i < 3) { print(str(i)); i = i + 1; }
```

条件必须是 `bool`：写 `while (1)` 报 `expected bool, got int`（与 §3.4 的`and`/`or` 同一口径，
不把真值性当布尔）。`while (true) { ... break; }` 是唯一的死循环写法。

### 4.6 🔴 闭包：**读**是引用，**写**是影子变量

`let f = () => ...` 创建闭包。捕获语义**不对称**，这是本语言最反直觉的一处：

```lume
let x = 1;
let f = () => x;
x = 99;
print(str(f()));                   // 99← 读**跟随**外层最新值（按引用）
```

但**写不会外泄**——闭包内赋值创建的是本次调用的**影子变量**，函数返回即丢弃：

```lume
let c = () => { x = x + 1; return x; };
print(str(c()));                   // 100
print(str(c()));                   // 100← 每次都从捕获值重新开始
print(str(x));                     // 99   ← 外层从未被改
```

⚠️ 这意味着**无法用闭包实现可变状态**（计数器、累加器都做不到）。要跨调用保存状态，
只能把状态放进 map/list 这类**对象**里（对象是引用语义，见 §3.6），再通过 `put` / `push` 改。

⚠️ 同样的规则适用于**具名 `func`**：`func g() { x = 5; }` 调用后外层 `x` 不变。

### 4.7 🔴 循环内创建的闭包**共享同一个变量**

```lume
let fns = [];
for (let i in [1, 2, 3]) { fns = push(fns, () => i); }
print(str(get(fns, 0)()));         // 3
print(str(get(fns, 2)()));         // 3    ← 全部拿到最后一个值
```

因为 §4.2 的循环变量是**同一个**变量（不是每轮新建）。这与 JavaScript `let` 的
「每轮一份」相反。想要每轮一份，就把值先拷进一个对象里闭包捕获它。

### 4.8 闭包与原生后端

**无捕获 lambda 已是三后端一致的头等值**（2026-10-08 起，本节原「只在解释器可用」的口径作废）：

```lume
let f = (x) => x * 2;
print(f(3));                        // 6 —— 三后端一致
print(map((x) => x * x, [1, 2, 3])); // [1,4,9] —— 三后端一致
```

**自由变量捕获（Phase-2，2026-10-15 起）**：函数体 / lambda 体内的 lambda 捕获外层局部变量，
文本后端在闭包创建处建捕获数组（各捕获变量槽的地址），闭包 define 把捕获注册为 `%cap<i>` 槽，
体内读写透过数组两点寻址直读/直写外层槽——**与外层共享存储**（解释器 env 链同语义）：

```lume
func outer(): int {
    let v = 5;
    let f = () => v;
    v = 10;              // 写穿：闭包看到 10
    return f();          // 10 —— 三后端一致
}
print(outer());
```

支持的捕获源：外层函数的参数、外层 lambda 的参数、局部 `let`（含 string / 列表等容器）；
嵌套 HOF（`map((xs) => reduce((a, b) => a + b, xs, 0), [[1,2],[3,4]])`）的内层 lambda
可捕获外层 lambda 参数并正常定型（元素为容器的列表类型递归为内层列表类型）。
一期限制：捕获"已被外层闭包捕获的变量"编译期报错；`return` 带捕获闭包编译期报错
（捕获数组随帧销毁，解释器因 env 链常驻而语义不同）；经列表 / 字段 / 调用结果带出
闭包未检测（文档缺口）。详见 §一阶段边界与 §8.1 #10。

lambda 在字面量处装箱成不透明闭包记录（函数指针 + 捕获上下文），可以存进变量、被调用、
以及作为实参传给 `map` / `filter` / `reduce` 三个内建高阶函数（两个原生后端把循环内联展开，
迭代长度只快照一次——循环体里的 `push` 不会延长迭代）。这些行为都进了`tests/native-consistency.lume` 的差分覆盖。

**一阶段边界（原生后端拒绝、解释器也可能拒绝或行为不同的部分）**：

- **自由变量捕获已支持（三后端一致）**。函数体 / lambda 体内的 lambda 可捕获外层局部变量
  （含参数、外层 lambda 的参数、局部 `let`、列表等任意可读写的槽），文本后端为闭包建捕获
  数组（槽地址），读写**直穿外层槽**，因此与外层共享存储（`v=10` 后闭包读到 10，§4.7、
  §8.1 #10）。限制：①捕获"已被外层闭包捕获的变量"（二级闭包捕获）编译期报错；
  ②返回捕获了本函数局部的闭包（`return f` / `return () => v`）编译期报错——捕获数组随
  帧销毁，解释器因 env 链常驻而语义不同；经列表/字段/调用结果带出闭包未检测（文档缺口）。
- **`try` 文本后端已支持**：成功 `{ ok: 结果, err: null }`，失败 `{ ok: null, err: 消息 }`
  （消息措辞与解释器略异，结构一致）；只接受 lambda 字面量或闭包变量，具名函数不是值。
  llvm 后端端点未验证。
- `filter` 的 lambda 返回值必须是 bool / int / float（真值测试只对这三类定义）；
  返回 string / 容器则拒绝。
- `reduce` 的累加器必须是标量或列表（struct 不可作累加器）。
- 元素为容器的列表（如 `[[1,2],[3]]`）已可 map/filter/reduce；嵌套列表（`[[1,2],[3,4]]`）
  的元素类型递归为内层列表类型，`map((xs) => reduce(..., xs, 0), ...)` 的嵌套 HOF 可正常
  定型（§8.1 #10 尾巴已补）；元素为 any 或空列表时 lambda 参数以 int 定型（循环不执行）。
- **闭包变量类型标注 `fn (参数) => 返回`（2026-10-16 起）**：`let f: fn(int) => int =
  (x) => x * 2;` 三后端一致——标注为 lambda 的未注参数提供类型（多参数 / 捕获外层局部 /
  作为实参传 `map` 等 HOF 均可），返回类型参与赋值检查。类型不匹配（如 lambda 参数标注
  `string` 而 fn 类型写 `int`）三后端一致拒绝。具名 `func` 两边都支持，要编译成原生二进制的
  脚本二者皆可用。

⚠️ §8 三后端一致性的上一版结构性例外（「闭包脚本不参与差分测试」）随之作废：
无捕获闭包 + HOF 的用例已直接躺在 `make native-consistency` 的靶子里。

### 4.9 三元条件表达式 `cond ? then : else`

三个操作数，**只求值选中的分支**（`flag ? 1 : 1/0` 在 flag 为真时不执行除法），
解释器与两个原生后端一致。

- **优先级**：低于 `or`、高于赋值；右结合（`a ? b : c ? d : e` 折叠为
  `a ? b : (c ? d : e)`）。
- **条件**：与 `if`/`while` 同一口径的布尔要求（§3.4 `strict_bools`）。
- **分支类型统一**：同型→该型；int/float 混合→float（与 §1.4 算术提升一致）；
  其余组合类型检查拒绝。
- **与 `?` 传播后缀的消歧（§5）**：`?` **紧贴调用**（无空白）一律是传播后缀；
  空格分隔的 `?` 才可能是三元——此时向后扫描，本语句内（`;`/EOF/括号深度回落
  之前，depth 0 的 `,` 截断）存在配对 `:` 即三元，否则兜底按传播。由此
  `divide(8, 2)? > 3 ? "big" : "small"` 合法（贴邻传播 + 空格三元连用）。

```lume
let w: float = 5 > 3 ? 1 : 2.5;      // int/float 混合分支 → float，w == 1.0
let label = n > 100 ? "big" : n > 0 ? "small" : "neg";   // 右结合嵌套
```

### 4.10 if 表达式 `if (cond) expr else expr`

表达式位的 `if`（Lean 的 if-then-else）。parser 把它**折叠成 `N_TERNARY`**，因此
§4.9 的全部规则原样适用：只求值选中分支、分支类型同一规则统一、右结合。三后端
一致（`cg_ternary` 一条路径）。

- **`else` 必需**。没有 else 就没有 false 分支的值——同 `c ? a` 不是三元。缺 else
  编译期报 `an if-expression needs an else branch`。
- **不带花括号**。`{` 在表达式位已是 map 字面量 / 结构更新，加了会歧义；语句位的
  `if (cond) { ... }`（控制流语句）保持花括号，且仍可省 else。
- 与三元可自由混用，嵌套、`let` 绑定、直接实参、算术操作数均可。

```lume
let w: float = if (n > 0) 1 else 2.5;              // 1.0
let label  = if (n > 100) "big" else if (n > 0) "small" else "neg";  // 右结合
print(if (ok) compute() else fallback());          // 只求值选中侧
```

负例（缺 else / 分支类型不兼容）由 `tests/ternary-bad/` 断言；三后端值语义由
`tests/native-consistency.lume` 覆盖。

---

### 4.11 尾随闭包 `f(args) { |x| body }`（2026-10-14）

调用实参表的右花括号之后再跟一个闭包，是「把它当作最后一个实参传入」的语法糖
（Rust / Swift / Kotlin 的 trailing closure）。parser 把它折叠成普通 `N_FUNC_LIT`
实参——typecheck 与两个发射器零改动：一种语法，三个消费者。

- **形如 `{ |a, b| body }`**：参数是裸名字，可带 `: type` 注解（注解能力与箭头
  形式完全一致；原生后端在推不出参数类型时需要它）。
- **零参写作 `{ || body }`**：lexer 把相邻两个 `|` 融合成单个 `||`（`TOK_OR`）
  token，解析器据此识别空参数表。
- **body 是语句序列**：最后一个**不带 `;`** 的表达式即闭包的值（隐式 return，
  Rust 式块体）；以 `;` 结尾则弃值（闭包返回 null）。`let` / `if` / `for` /
  `while` 等语句均可入体。
- **只挂在带实参表的调用之后**（`f(...) { ... }`）。识别靠双 token 前瞻：`{` 后
  必须紧跟 `|`（或 `||`）——map 字面量的首 token 是键、结构更新以基名开头、语句
  与 match 臂都不以 `|` 开头。因此 `if (f()) { ... }`、`while (f()) { ... }`、
  `match (f()) { ... }` 的条件位 / scrutinee 花括号绝不会被误吞。
- **void 闭包的结果用作数值时是 null**（解释器口径；两个原生后端一致——文本
  后端曾把类型名 `void` 当值 token 发射出非法 IR，本轮已修）。
- 内建 `map` / `filter` / `reduce` 的参数序是 `(fn, list)`，尾随闭包追加在**尾
  部**，所以这三个内建仍用箭头形式书写（`map((x) => x * 2, l)`）；尾随形式服务
  于函数参数在末位的宿主（`each(items) { |x| ... }`）。

```lume
func each(list, f) { for (x in list) { f(x); } }
func apply(list, f): int[] {
  let out = [];
  for (x in list) { push(out, f(x)); }
  return out;
}
each([1, 2]) { |x| print(x); };                    // 语句体
print(apply([1, 2, 3]) { |x| x * 2 });             // [2,4,6]  表达式体即值
print(apply([1, 2, 3]) { |x: int| let d = x * 2; d + 1 });  // [3,5,7]
```

负例（`{` 后无 `|`、参数非裸名、花括号不闭、坏注解、闭包计入 arity）由
`tests/trailing-closure-bad/` 断言；三后端值语义由 `tests/native-consistency.lume`
的尾随闭包探针段覆盖。

### 4.12 记录解构 `let { a, b } = e;`（2026-10-14）

一次性求值 `e`，再把 `e` 的每个同名 key 绑定到当前作用域的一个裸名上——等价于
`let a = e.a; let b = e.b;` 但 `e` 只求一次，且名字与字段同名；改名形式
`let { 字段: 新名, ... } = e;` 把「读哪个字段」与「绑成什么名」分开写
（2026-10-14 起，`fields[i]` 与 `names[i]` 在 AST 上各自独立）。规则与 `m.x`
点访问一一对应：

- **init 必须已知形状**：匿名 struct 字面量（`{ "a": 1, "b": 2 }`）或命名 struct。
  其字段集在编译期确定，每个模式名都必须存在，否则 `type has no field 'x'`。
- **动态 any map 故意不绑**：`let m: map = ...; let { a } = m;` 在编译期被接受
  （`types[i] = NULL`），但两个原生后端在发射期拒绝（"the destructured map has
  no known shape"）——与 `m.x` 在动态 map 上的同一既有边界；要解构未知形状的
  map 用 `get(m, k, d)`。
- **enum 不可解构**：`let { a } = SomeEnum` 报 "destructuring needs a map value
  (got E) — use if-let to destructure an enum"；enum 用 `if let` / `match`。
- **`len` / `length` 拒绝**：它们是容器的属性名而非 map key，同名遮蔽会踩与
  `.len` 一样的坑，解析期即拒（"choose another name"）。
- **命名 struct 字段绑定是地址还是值**：命名 struct 在 Val 槽里存的是地址
  （`agg = 0`），解构时须先 load 成聚合值再存储——这一路正是「struct 字段绑定
  未 load 成值」既有 bug 的现场（普通 `let v = o.inner` 同样踩中，本轮一并修掉）。
  匿名 struct 字面量 / 调用 / 字面量产生的字段已是聚合值（`agg = 1`），直接存。

```lume
type Point = { x: int, y: int };
let p = { x: 3, y: 4 };
let { x, y } = p;                 // x=3, y=4; p 原值仍可用
let { u, v } = { "u": 10, "v": 20 };   // 匿名 struct 字面量
let { s, f, t } = { "s": "txt", "f": 1.5, "t": true };  // 标量类型全支持
let { ix, iy } = p;               // 链式: 对已解构出的 struct 再解构一层
let { a } = { "a": 1, "b": 2 };   // 部分解构(只绑子集)
```

正向 12 条断言由 `tests/letpat.lume` 覆盖（解释器 + 两个原生后端各编各跑，输出
同为 12/12）；7 个编译期负例（`空模式` / `非名` / `容器属性名` / `重名` / `非 map`
/ `缺字段` / `enum init`）由 `tests/letpat-bad/` 断言；三后端值语义由
`tests/native-consistency.lume` 的解构探针段覆盖。

### 4.13 元组 `(a, b, ...)`（2026-10-14，借鉴 Lean 的匿名乘积类型）

元组是**带位置字段 `_0`、`_1`、… 的匿名 struct**——零新运行时表示，类型系统、
运行时、打印、字段访问、解构全部复用 struct 的既有机制。三种语法糖：

```lume
let p = (1, 2);                   // 字面量: 键自动编号 _0/_1（等价 { _0: 1, _1: 2 }）
print(p.0); print(p.1);           // 数字访问: `.N` 是 `._N` 的糖
print(p._0);                      // 下划线形式永远可用
let (x, y) = p;                   // 解构: 等价 let { _0: x, _1: y } = p
let one = (42,);                  // 单元素元组必须有尾逗号; (a) 无逗号仍是分组表达式
let h = (1, "hi", 2.5, true);     // 异构合法——匿名 struct 本就不限字段同型
let q = (1, (2, 3));
print(q._1._0);                   // 嵌套; 注意链式 `.1.0` 会被词法合成浮点 1.0
```

规则：

- **匿名是默认，命名只来自上下文**（2026-10-14 规范化，三后端与类型检查器同一
  规则）：map/struct 字面量是命名 struct **当且仅当** 所在上下文显式写了类型
  ——`let` 标注、函数返回类型、参数类型、struct 字段类型。不存在「按字段名形状
  扫描自动提升」：`let r = { u: 8 }` 哪怕恰好有 `type Rec = { u: int }` 也保持
  匿名。这条对元组是刚需——`type T2 = { _0: int, _1: int }` 一旦声明，`(2, 3)`
  的形状必然与它撞车，按形状提升会把嵌套元组重建成命名聚合而三后端互不一致。
- **显式上下文照常命名**：元组字面量放在期望命名 struct 的位置就提升为该类型
  ——`func mk(): T2 { return (3, 4); }`、`swap((7, 9))`（`swap(p: T2)`）。
- **`.N` 数字访问仅顶层可用**：`p.1.0` 的 `1.0` 会被词法器合成一个浮点 token
  （词法边界，不是语义规则）；嵌套访问用 `._N` 下划线形式。
- **`==` 一侧已知命名 struct 时，另一侧字面量按该类型求值**：`p == { x: 1, y: 2 }`
  （`p: P`）合法且是内容比较——两个原生后端把两侧都重建为 `%P` 聚合再比，
  与解释器逐键比较一致。

正向 15 条断言由 `tests/tuple.lume` 覆盖（解释器 + 两个原生后端各编各跑，输出同为
15/15）；4 个编译期负例（`重名绑定` / `非 map 解构` / `缺位置字段` / `元组类型标注`）
由 `tests/tuple-bad/` 断言；三后端值语义由 `tests/native-consistency.lume` 的元组
探针段覆盖。

---

### 4.14 局部函数 `func`（2026-10-14，B5）

函数体里也能写 `func` 声明——**局部函数**。它编译成普通的模块级函数（不捕获
外部局部变量），但**只在其包含函数内可见**：

```lume
func main() {
  func sq(x: int): int { return x * x; }
  func fact(n: int): int {
    if (n <= 1) { return 1; }
    return n * fact(n - 1);          // 递归
  }
  print(fact(6));                    // 720
  print(later(41));                  // 42 —— 前向引用: 提升语义, 声明前可调
  func later(x: int): int { return x + 1; }
  let f = (x) => sq(x) + 1;          // lambda 体里也调得到
  print(f(4));                       // 17
}
main();
print(sq(2));                        // 编译错误: undefined variable 'sq'
```

规则（三后端 + 解释器同一语义）：

- **不捕获**：局部函数没有闭包语义，身体里只能用自己的参数（要状态就传参数，
  要捕获就用 lambda）。它就是一份提升到模块级的具名函数。
- **作用域 = 包含函数**：局部名在包含函数内全程可见（声明位置之前也算——提升
  语义，与 Rust 嵌套 `fn` 一致）；在包含函数之外，这个名字**不存在**——顶层或
  其他函数里的同名调用一律 `undefined variable`。局部名与顶层函数名可以共存：
  包含函数内局部赢（遮蔽），顶层调用不受影响。
- **同名禁止**：同一包含函数里两个局部同名是编译错误（重命名按名字做，同名会
  把两组调用都送到同一个合成名上，只能拒绝而不是猜）。不同函数里的同名局部互
  不干扰。
- **实现注记**（不影响源码层语义）：每个局部在编译前被赋一个合成名，其包含函数
  内按原名发起的调用被整体改写到合成名上；checker、两个原生后端与解释器都在改
  写后的形态上工作。这也是作用域规则能三后端严格一致的原因——「可见」与「不可
  见」都由同一份改写决定，而不是各自的符号表各查各的。

正向 12 条断言由 `tests/localfn.lume` 覆盖（解释器 + 两个原生后端各编各跑，输出
同为 12/12）；2 个编译期负例（`同函数内同名局部` / `包含函数外调用`）由
`tests/localfn-bad/` 断言；三后端值语义由 `tests/native-consistency.lume` 的局部
函数探针段覆盖。

---

### 4.15 let 表达式 `let x = e1 in e2`（2026-10-14）

`let` 也有表达式形式（Lean 的 let-in）：`let x = e1 in e2` 先求值 `e1`，把 `x`
绑定到它的值上，再求值 `e2`——**`e2` 的值就是整个表达式的值**。

```lume
print(let a = 5 in a * 2);              // 10
let y = (let b = 3 in b + 1) * 2;       // 8 —— 语句位 RHS 也可以
let w: int = let k = n in if (k > 5) k else 0 - k;   // 注解 + if 表达式作 body
print(add(let m = 20 in m, 2));         // 22 —— 实参位
```

规则（三后端 + 解释器同一语义）：

- **作用域 = body**：`x` 只在 `e2` 内可见，`e2` 求值完绑定即消失——`e2` 之外的
  代码看不到它，`undefined variable`；body 内可以遮蔽外层同名绑定，body 结束后
  外层绑定原样可见（同行也能安全遮蔽，见下）。
- **可嵌套**：`let p = 2 in let q = p * 10 in q + p`——内层绑定对外层 body 可见。
- **body 向右 maximal munch**：body 是完整表达式，`let x = 1 in x + f(x)` 的两个
  操作数都在 body 里；要提前结束就加括号。
- **`in` 必需**。语句位以 `;` 结尾的普通 `let` 绑定不受影响；表达式位缺 `in` 是
  解析错误（`a let-expression needs 'in'`）。语句位写 `let x = e1 in e2;` 也合法
  ——整 体折叠成一条表达式语句，值被丢弃。
- **注解**：`let x: T = e1 in e2` 的 `T` 既约束 `e1`（不匹配编译期拒绝）也定型
  绑定；未注解时绑定的类型来自 `e1`，整个表达式的类型来自 `e2`。
- **赋值也是表达式**：body 里可以对绑定赋值（`let x = 10 in x = 2` 产出 `2`），
  外层同名绑定不受影响；赋值表达式的类型按绑定的类型算。
- **实现注记**（不影响源码层语义）：两个原生后端在**发射时**注册绑定槽并在 body
  发射完后截断 locals 表（match 臂绑定的同一套机制），所以「可见/不可见」由同一
  份发射序决定，同行遮蔽天然正确；解释器用子环境，同一语义。checker 把 body 的
  定型结果盖戳到节点上，scan 期推断读戳而不下钻 body——否则绑定名会落到外层同名
  绑定上（resolve-vs-infer 分裂的老坑）。

正向 12 条断言由 `tests/letexpr.lume` 覆盖（解释器 + 两个原生后端各编各跑，输出
同为 12/12）；3 个负例（`body 外使用` / `注解不匹配` / `缺 in`）由
`tests/letexpr-bad/` 断言；三后端值语义由 `tests/native-consistency.lume` 的
let 表达式探针段覆盖。

---

## 5. Result 与 `?` 传播

`Result` 是内建的 `{ ok: ... }` / `{ err: ... }` 形状。函数返回 `Result` 后，调用点用
`?` 解包：

```lume
func divide(a: int, b: int): Result {
  if (b == 0) { return { err: "division by zero" }; }
  return { ok: a / b };
}

func report(): Result {
  let q = divide(21, 7)?;            // 解 ok；err 则向外传播
  print("21 / 7 = " + str(q));
  return { ok: 1 };
}
```

规则：

- `?` **只允许出现在返回 `Result` 的函数体内**。函数返回 `Result` 但 `?` 的操作数不是
  `Result` 时，同样拒绝。
- 这条检查在**类型检查器**里（`typecheck_expr.c`），不合法时报清晰的定位消息
  `'?' used on a call that does not return Result` /
  `'?' needs the enclosing function to return Result`。
  ⚠️ 本节原先记的「检查落在 parser 层、不合法时报 `expected ;, got ? ('?')` 且位置跳行」
  **已过时**（见 §8.1 #3）：那条描述对应的是旧实现，现已不成立。
- 遇 `err` 时，当前函数**立刻返回该 `err`**，不再执行后续语句。
- `?` 的结果类型是 `Result` 里 `ok` 值的类型。
- **载荷类型由被调函数自己的 `return { ok: X }` 决定**：类型检查器跑两遍——第一遍只收集每个
  `Result` 标注的 `ok` 载荷类型（记在该函数自己的 Result 类型对象上），第二遍才真正检查并让
  调用点读它。**两遍是必需的**：`later()?` 而 `later` 定义在后面时，单遍看到的载荷是空的。
- 同一函数里多个 `return { ok: X }` 的 `X` **类型不一致时报错**（`two different types`），
  而不是取第一个或静默退化——该函数没有单一载荷类型，调用点读到哪个都会错配。
- **三个后端都支持 `?` 了**：`Result` 值本身与 `?` 传播均已一致（见 §8.1 #6）。
  `?` 在原生后端编译成：调用后先查 `err` 键，命中就**从当前函数返回该 Result**，
  未命中才按载荷类型读 `ok`。这与解释器一致（`interp.c` 的 `propagate` 分支）。
- ⚠️ **一个仍受限的情形**：若被调函数**只返回 `err`、没有任何 `return { ok: X }`**，
  则载荷类型未知，两个原生后端会拒绝编译并说明原因（解释器仍可运行）。

---

## 6. 内建函数

本节是**规范性**的：写明每个内建的签名、返回类型与边界行为。[LUME.md §内建函数](LUME.md)
只给名字清单，语义以本节为准。

### 6.0 通用约定

- **不能声明、不能遮蔽**：内建名已在类型检查器的名字表里（`typecheck.c` 的
  `LUME_BUILTIN_NAMES`），自己声明同名的 `func` 是**重复定义**错误。
- **参数多半是「松散」的**（内部 `any`），所以调用点通常不写类型标注。**返回类型不松散**：
  `keys` / `get` / `el` 等返回结构化类型，类型检查器知道它们的返回类型。
- ⚠️ **报错一律是运行时错误，不是陷阱**：内建不会 panic，被调脚本以非零码退出。
- ⚠️ 下面的「静默」指**不报错且给出一个值**。这是本语言最需要警惕的一类行为，已在每处标出。
- ⚠️ **本节的表格不是完整清单**。`typecheck.c` 的 `LUME_BUILTIN_NAMES` 才是（它同时是
  「不能声明、不能遮蔽」的判定依据）。下列内建本节没有各自的表格小节，语义只在别处带过：
  `run`（§7 服务端）、`replace` / `ceil`（与 `floor` / `round` 同族的数学组，§6.7）、
  `write` / `read`（§6.5 文件）、以及 5 个 `http_*`（§6.8，**本树在 Windows 上
  `LUME_HAS_HTTP=0`，它们存在但调用即报错**）。

### 6.1 输出与转换

| 内建 | 签名 | 返回 | 边界 |
| --- | --- | --- | --- |
| `print(x)` | 1 参| `null` | 字符串原样输出；`int` 按 i64（§1.1）；`float` 按 `%g`（§1.3）；其他走 JSON |
| `str(x)` | 1 参 | `string` | `int`→精确十进制；`float`→`%g`；`null`→`"null"`；map/list→JSON（三后端一致，2026-10-13 补齐原生）；⚠️ 命名 struct 仅解释器支持——原生是裸 GEP 结构无运行时头，`str`/`print` 均拒绝 |
| `string(x)` | 1 参 | `string` | 与 `str` 同 |
| `int(x)` | 1 参 | `int` | **`int` 原样；`float` 截断；非数字字符串静默返回 `0`** |
| `float(x)` | 1 参 | `float` | `int`→float；⚠️ **非数字字符串静默返回 `0.0`** |
| `bool(x)` | 1 参 | `bool` | 真值性：空串/`0`/`null`/空集合为 `false` |
| `json(x)` | 1 参 | `value` | **反序列化**：JSON 文本 → map/list/string/number/bool/null；**整数数字（无 `.`/`e`/`E`）解码为 `int`，含小数或指数的解码为 `float`**；解析失败置 VM error（可用 `try()` 接住）。**字符串参数的解析三后端一致（2026-10-16 补齐原生）**：原生端返回 tagged-any 槽（`LumeSlot*`），用 `get(x, k, dflt)`/`str`/`len`/`for-in` 按 dflt 类型读取（`[]`→list、`{}`→map、`""`→string）；`json()` 用于**序列化**（参数非字符串）时三后端一致 |
| `stringify(x)` | 1 参 | `string` | 序列化：任意值 → JSON 文本（与 `str()` 的 map/list 路径一致；字符串加引号转义）。三后端一致，命名 struct 同样支持：原生后端按声明字段构造 map 再序列化，与解释器 JSON 一致 |
| `now()` | 0 参 | `int` | Unix 秒 |
| `strftime(fmt, ts)` | 2 参 | `string` | ⚠️ **参数序是 `(格式, 时间戳)`**，不是反过来 |
| `str_match(pattern, text)` | 2 参 | `{text,index,groups}[]` | **正则抽取**：POSIX ERE 全扫描，每条 `{ text: 匹配子串, index: 0-based 偏移, groups: 捕获组[] }`；无匹配返回 `[]`；非法正则置 VM error（可 `try()` 接住）。⚠️ **解释器专属**：匹配列表元素是无名 map，原生后端没有该形状的发射路径，`--compile-text`/`--compile-llvm` 编译期拒绝（`cannot infer a type for ...`）——与 `json(str)` 原同属「动态结果形状」边界（`json(str)` 已通过 tagged-any 槽补齐原生，见 865 行；str_match 的匿名 map 列表形状仍是该边界的残余，见 TODO intentional boundaries） |
| `regex(pattern, text)` | 2 参 | `{text,index,groups}[]` | `str_match` 的别名（同为解释器专属） |

⚠️ **`int("abc") == 0` 且不报错**，`float("x") == 0.0` 同理。这是转换类最容易踩的坑：
拼错的字段名会静默变成 0，而不是失败。

```lume
print(str(int("abc")));                // 0    ← 静默
print(str(int("3.9")));                // 3    ← 截断，不是四舍五入
print(str(strftime("%Y", 0)));         // 1970
```

### 6.2 集合

| 内建 | 签名 | 返回 | 边界 |
| --- | --- | --- | --- |
| `len(x)` | 1 参 | `int` | 字符串/列表/map 的长度；⚠️ **其他类型静默返回 `0`** |
| `keys(m)` | 1 参 | `string[]` | ⚠️ **只接受 map**，传 list 报 `keys() expects a map` |
| `get(m, k)` | 2 参 | 任何 | 解释器缺键返回 `null`；原生缺键返回该值类型的零值（要 null 语义用三参数 `null` 默认） |
| `get(m, k, d)` | 3 参 | 任何 | 缺键返回 `d`（更安全，推荐）；`d` 为 `null` 时三后端一致（原生走 obj 读，null 槽与缺键都读回空指针，2026-10-13 补齐） |

**静态形状派发**（2026-10-14）：`k` 是**字符串字面量**时，原生后端按 `m` 的静态形状里该键的
value type 选择运行时 getter（`lume_map_get_s/_f/_i/_obj`）——map 字面量的键集、或声明式
struct 的字段表就是形状。于是 `print(get(m, "name"))` 三端都打 `lume`（曾永远走 int getter、
string 值静默读成 `0`），`let x = get(m, "name")` 无标注也能定型。三参数的 `d` 与键的形状类型
冲突时**编译期拒绝**：键存在时值必为形状类型，异型默认只可能在缺键时生效，一个静态调用点
无法同时满足两者。键不是字面量、或形状未声明该键时，派发退回 `d` 的类型（三参数）或 int
（两参数）。
| `put(m, k, v)` | 3 参 | **map** | **原地**写 map，并返回写完的 map；`v` 为 `null` 三后端一致（obj 槽空载荷） |
| `push(l, v)` | 2 参 | **list** | **原地**追加，并返回写完的 list |
| `range(a, b)` | 1–3 参 | `int[]` | `[a, b)`，**不含 b**。`range(stop)`＝`range(0, stop)`；第三参 `step` 可负（`range(5, 0, -2)`＝`[5,3,1]`），`step` 为 0 运行期报错。**三后端统一**：原生 `lume_range`（rt 助手）跑与解释器相同的 double 运算，整值落 int 槽，`print`/`for-in`/`len`/下标/`str` 逐一致 |
| `sort(l)` | 1 参 | **list** | **原地**排序 `l` 并返回它；仅支持**同构**列表——全数字（升序，全 int 按 i64 精确比较、含 float 走 double）或全字符串（`strcmp` 字节序）；异构或含容器/map 报错。⚠️ **解释器专属**：两个原生后端尚无 `sort` 发射路径（`--compile-text`/`--compile-llvm` 报 `call to unknown function 'sort'`）——与 `json(str)` 同属「动态结果形状」边界；2026-10-16 由 `examples/stats.lume` 的三后端验证发现，TODO P4 之后补 |

**下标语法**（规范变更）：`m["k"]` 与 `l[0]` 现在**存在**了。

- **链式**：`m["a"][0]`、`rows[i]["name"]` 都可以，因为下标在 postfix 循环里。
- **下标是完整表达式**，不是字面量——`m[k]`（k 是变量）才是值得有的那个形态。
- ⚠️ **严格**，这是它与 `get()` / `el()` 的**唯一**区别，也是要有这套语法的理由：
  **键不存在 / 索引越界一律报错**，而 `get()` 静默给 `null`、`el()` 给默认值。
- **负索引从末尾数**：`l[-1]` 是最后一项。
- **两个原生后端现已支持**（文本后端与 libLLVM 后端都通过 `src/rt.c` 的
  `lume_index_int` / `lume_index_float` / `lume_index_ptr` 统一入口下发；运行期按
  容器 `kind` 字段分派 list / map，见该组函数上方注释）。`tests/native-consistency.lume`
  已含下标用例，三个后端（解释器 + 两原生）输出逐字节一致。
- **列表下标完全可用**：元素类型在编译期已知，标量 / 字符串 / 嵌套容器结果都能直接 `print`，
  三后端一致。例如 `let l = [10, 20, 30]; print(l[0]); print(l[-1]);`。
- ⚠️ **map 读取的返回类型是「不透明 `i8*`」**：map 的值类型在编译期不跟踪，所以 `m["k"]`
  的静态类型是匿名 struct（即 map 形态），运行期实际可能是字符串或容器。因此：
  - 直接 `print(m["k"])` 在原生后端**不会崩溃**（已加 `kind` 护栏，打印 `(opaque value)`），
    但与解释器（`world`）**不一致**，所以**不要**把它写进一致性测试；
  - ⚠️ **未标注的「变量 map」标量读（`int`/`float`/`bool`）在原生后端是错误而非值**：
    `m["k"]` 一律 lowering 成 `i8*` 指针存取器，它只能承载 string / 容器值；遇到标量值
    读取失败，报错**明确指出值是标量**并引导 `get(m, k, d)`——**不会**再说误导性的
    `no such key`（key 实际存在）。例如 `let im={count:7}; print(im["count"])`
    在文本 / libLLVM 后端报：`map value at this key is a scalar; use get(m, k, d) to
    read it -- typed binding via m["k"] only works for string / container values`。
    解释器带运行时类型，同一行直接打印 `7`。
  - ⚠️ 标量的**类型标注绑定同样不可用**：`let v: int = m["k"]` 在原生后端仍走 `i8*`
    存取器，`i8* → i64` 无 coerce 边，编译期直接拒绝。所以标量 map 值的唯一合规读法
    就是 `get()`。
  - 可移植的写法是**绑定到带类型的变量**：`let c: string = m["k"]; print(c);` 或
    `let li: int[] = m["items"]; print(li[0]);`——解释器与两原生后端都接受且输出一致。
  - 链式 `m["p"][1]` 之所以可行，正是因为运行期在 `lume_index_ptr` 里按 `kind` 重新分派：
    只要每一步最终落到字符串 / 容器（而非「经不透明链再取标量」），三后端行为一致。

⚠️ `len` 对非集合返回 0，所以 `len(env("X"))` 这种写法会静默给 0。

⚠️ **`put` / `push` 返回的是集合本身，不是 `null`**，所以可以链起来：

```lume
let m = put({ a: 1 }, "b", 2);        // {"a":1,"b":2}
let l = push([1], 2);                 // [1,2]
```

### 6.3 高阶函数

⚠️ **参数序是 `(函数, 集合)`，不是 `(集合, 函数)`** —— 这与几乎所有语言相反，写错时报的是
`map() expects (fn, list)`，不会静默算错。

| 内建 | 签名 | 返回 |
| --- | --- | --- |
| `map(fn, list)` | 2 参 | 同类型 list，元素为 `fn(元素)` |
| `filter(fn, list)` | 2 参 | 满足 `fn` 的子集 |
| `reduce(fn, list, init)` | 3 参 | 累积值；`init` 必给（**无二元折叠重载**） |
| `try(fn)` | 1 参 | `{ ok, err }` |

```lume
print(str(map((x) => x * x, [1, 2, 3])));        // [1,4,9]
print(str(reduce((a, b) => a + b, [1, 2, 3], 0))); // 6
print(str(try(() => 1 / 0)));                      // {"ok":null,"err":"division by zero"}
print(str(try(() => 42)));                         // {"ok":42,"err":null}
```

`try` 是**唯一**的运行期错误捕获机制（没有 `try` 表达式、没有 `catch` 子句）。
⚠️ 它捕获的是**内建抛出的运行时错误**；类型检查失败在更早的阶段，不受它影响。

### 6.4 文件系统

| 内建 | 签名 | 返回 | 边界 |
| --- | --- | --- | --- |
| `read_file(p)` | 1 参 | `string` | ⚠️ **文件不存在静默返回 `null`**，不报错 |
| `write_file(p, data)` | 2 参 | `bool` | **原子写**（临时文件 + rename）；数据文件默认 `0600` |
| `mkdir(p)` | 1 参 | `bool` | 递归创建（等价 `mkdir -p`）；已存在返回 `true` |
| `files(p[, rec])` | 1–2 参 | `string[]` | 目录条目；目录带尾部 `/`；第二参真值时**递归**（条目变相对路径 `sub/file.txt`，深度上限 64） |
| `lock_file(p, ms)` | 2 参 | `bool` | 跨进程建议锁；超时返回 `false` |
| `unlock_file()` | 0 参 | `bool` | 释放本进程持有的锁 |

⚠️ `read_file` 对不存在的文件**不报错**，脚本里「读配置」失败会表现为后续对 `null` 的处理，
而不是一条清晰的错误。检查存在性请用 `files()`。

### 6.5 环境与加密

| 内建 | 签名 | 返回 | 边界 |
| --- | --- | --- | --- |
| `env(name)` | 1 参 | `string` | ⚠️ **未设置静默返回 `null`** |
| `argv()` | 0 参 | `string[]` | 脚本名之后的 CLI 参数（`lume-core s.lume a b` → `["a","b"]`）；无参数为空列表；⚠️ **解释器专属**（native 后端暂不支持） |
| `crypt_sha512(pw)` | 1 参 | `string` | ⚠️ **平台相关**：无 `crypt(3)` 的平台（含 macOS）报 `SHA-512 crypt unavailable on this platform` |

### 6.6 数学

| 内建 | 签名 | 返回 | 边界 |
| --- | --- | --- | --- |
| `abs(x)` | 1 参 | **`float`** | ⚠️ **恒返回 float**（内部是 `fabs`），`abs(7)` 是 `7.0` 不是 `7` |
| `sqrt(x)` | 1 参 | `float` | 负数报 `sqrt(): negative argument` |
| `exp(x)` / `log(x)` / `ln(x)` | 1 参 | `float` | `log`/`ln` 在 `0` 报 `log(): argument must be positive` |
| `pow(b, e)` | 2 参 | `float` | 结果非有限（如负底分数次）报 `pow(): result not finite (domain error)` |
| `floor` / `ceil` / `round` | 1 参 | `float` | ⚠️ **`round` 是银行家舍入**（半偶） |
| `min(a, b, ...)` / `max(a, b, ...)` | **变参，≥1** | **`float`** | ⚠️ **变参**（不是两参），同样恒返回 float |
| `pi` / `e` | 常量 | `float` | 精度同 C `M_PI` / `M_E` |

⚠️ **整个数学组都恒返回 `float`**（内部一律 `double` + `fabs`/`sqrt`/…），`min`/`max` 也是变参。
只有 `abs` 这类看起来「本该保型」的最容易误判，所以这一组统一按 float 理解：

```lume
print(str(min(3, 5, 1)));              // 1     ← 三参，不是两参
print(str(round(2.5)));                // 3
print(str(round(3.5)));                // 4     ← 半偶，不是"5"
print(str(ln(0)));                     // 报错
print(str(abs(0 - 7)));                // 7     ← 值是 float，按 %g 显示成 7
print(str(abs(0 - 9223372036854775807 - 1)));  // 9.22337e+18
```

⚠️ **`round` 是半偶舍入**（round-half-to-even），`round(2.5)=3`、`round(3.5)=4`。
按「四舍五入」预期写代码会得到相反结果。

⚠️ i64 最小值取绝对值在 double 下正过来得到 `9.22337e+18`，**不是**可精确表示的 int。
需要精确的大整数绝对值时不要用 `abs`。

### 6.7 页面小工具（虚拟 DOM）

| 内建 | 签名 | 返回 |
| --- | --- | --- |
| `el(tag, props, ...children)` | **≥3 参** | vnode |
| `render(vnode)` | 1 参 | `string` |
| `html(template, ...slots)` | **≥1 参** | `string` |

```lume
print(render(el("p", {}, "hi")));      // <p>hi</p>
```

⚠️ `el` 的 `props` **不可省**：`el("p")` 报 `el() needs (tag, props, ...children)`，
没有「省略 props」的简写。`props` 非map 时会被当成空 map（不报错）。

`html` 是**按位置索引**填充的模板串，槽位语法是 **`{N}`**（`N` 从 **0** 起，指向第 N 个
`...slots` 实参）：

```lume
print(html("a{0}b{1}c", "X", "Y"));    // aXbYc
print(html("a{1}b", "X", "Y"));        // aYb
print(html("a{5}b", "X"));             // a{5}b    ← 越界则原样保留
```

另外两条转义规则：

- **`{{` 与 `}}` 是 mustache 转义**，各自折成**一个字面花括号**：`html("{{x}}")` → `{x}`。
  所以想输出字面花括号要写双份。
- **单花括号不做插值**，除非后面紧跟数字构成合法 `{N}` 槽位。`html("a{b}c")` → `a{b}c`。

#### 转义与 `trusted_html`（XSS 边界）

`el` / `render` 与 `html` 的**转义口径不同**，跨这两者拼装时必须知道：

```lume
print(render(el("p", {}, "<b>")));              // <p>&lt;b&gt;</p>    ← 转义了
print(html("<i>{0}</i>", "<b>&</b>"));          // <i>&lt;b&gt;…</i>   ← 也转义
print(html("<i>{0}</i>", html("<b>&</b>")));    // <i><b>&</b></i>     ← 不转义！
```

规则：**`html()` 的返回值被标记为「已渲染可信 HTML」**，把它再塞进另一个 `html()` 的槽位时
会**原样注入、不再转义**。`el`/`render` 没有这个标记，字符串子节点一律转义。

⚠️ 后果：`html()` 嵌套 `html()` 是**注入通道**。若外层模板的槽位里放的是不可信数据，
而内层恰好是 `html()` 的产物，就会绕过转义。拼装用户输入时不要走 `html(html(...))` 这条路。

⚠️ `el` / `render` 对**属性值**转义（`&`→`&amp;`、`<`→`&lt;`、`"`→`&quot;`、单引号→`&#39;`），
但对 **tag 名与属性名不做任何校验**：

```lume
print(render(el("a", { href: "x?a=1&b=2" }, "t")));   // <a href="x?a=1&amp;b=2">t</a>
print(render(el("script", {}, "alert(1)")));         // <script>alert(1)</script>
```

所以它**不适合**直接生成需要校验的场景（URL scheme、`<script>`、事件处理器属性
`onclick` 之类）—— 值被转义了，但**标签与属性名是你给的、不会被拦**。

### 6.8 工具台（空壳）

本树无 Agent 运行时，这四个恒返回空值但**保留可用**（不在不同平台间分叉语言面）：

| 内建 | 返回 |
| --- | --- |
| `tools()` / `skills()` | `[]` |
| `mcps()` | `[]` |
| `catalog()` | `{ skills: [], tools: [], mcps: [...] }` |
| `discovery_endpoints()` | `{ llm: null, router: null, model: null }` |

### 6.9 出站 HTTP

本树没有服务器，但能主动发起 HTTP/HTTPS 请求。五个谓词共用一条实现：

```lume
let r = http_get(url,    { headers?, timeout?, max_bytes?, allow_private? });
let r = http_delete(url, { ...同上 });
let r = http_post(url,   { headers?, body?, timeout?, max_bytes?, allow_private? });
let r = http_put(url,    ... 同 post,带 body);
let r = http_patch(url,  ... 同 post,带 body);
```

返回统一是 `{ ok, status, body, err }`。要点：

- **SSRF 闸门默认开着**：回环、`localhost`、链路本地（`169.254.0.0/16` 云元数据）、
  私网、CGNAT（`100.64/10`）在**开 socket 之前**拒绝；userinfo 里藏目标（`http://ok@127.0.0.1/`）
  一样拒。域名**解析后逐个 IP 判**。
- `body` 只被 `post`/`put`/`patch` 读；单个 body 上限 1 MiB，超了报错不截断。
- 带 body 的方法**自动补 `Content-Length`**；自己写过就不再补（两个同名头是请求走私诱饵）。
- 重定向最多 5 跳，每跳重过闸门。`301`/`302`/`303` 把带 body 的方法降级成 `GET` 并丢掉
  `Content-Length`；`307`/`308` 原样保留（RFC 7231 §6.4.4 / §6.4.7）。
- **总闸**：`--no-net` 或 `LUME_NO_NET=1` 时任何 HTTP 内建直接报错，一个包都不发。
- **代理 / NO_PROXY 绕过**：出站请求跟随 `LUME_HTTP_PROXY`（强制）> `HTTPS_PROXY`/`HTTP_PROXY`（大小写都认）。
  设了 `NO_PROXY`/`no_proxy`（逗号/空格分隔，支持 `.domain` 后缀、`*` 通配、可选 `:port`）的主机**直连、不走代理**；
  `LUME_NO_PROXY` 优先级最高，且无论是否设了 `LUME_HTTP_PROXY` 都生效——内网地址必须能绕过代理直连。
- ⚠️ **本树无 Winsock**：`LUME_HAS_HTTP=0`，这些内建在 Windows 构建里**存在但调用即报错**
  （见 §6.9 与 §8 的 host/core 差异）。

### 6.10 名字在表里但不可调用

`typecheck.c` 的 `LUME_BUILTIN_NAMES` 里有四个名字**没有对应实现**，写出来会得到
`undefined variable`：类型词 `type`、`Result`（用于标注而非调用）、常量 `pi`/`e` 是
`builtins_math.c` 的常量而非常量名。动词组 `read`/`write` 是保留的组名（见 `seed_verb_groups`）。

⚠️ 这意味着 `type(1)` 不是「取类型」——本语言**没有**取运行时类型的内建。

---

## 7. 模块

### 7.1 导入按「引用文件所在目录」解析

`import "x.lume" as ns;` 的路径相对于**引用它的那个文件**的目录，不是进程 CWD：

```lume
// examples/modules/app.lume
import "tax.lume" as tax;             // 指 examples/modules/tax.lume
```

导入路径先拼接、再规范化（消掉 `a/../b`），规范化结果**必须是绝对路径**。

### 7.2 循环导入是错误

直接或间接导入自己会被拒绝并报
`circular import: '<path>' imports itself (directly or indirectly)`，不会栈溢出。

### 7.3 只有 `export` 的顶层声明可见

`export let` / `export func` / `export type` 才被外部模块看到；未导出的顶层声明是该模块私有的。

---

## 8. 三后端一致性（可执行的不变式）

Lume 有三条执行路径：**解释器**、**原生后端 A（手写 IR 文本）**、**原生后端 B（libLLVM）**。

**不变式：对同一份 `.lume`，三者的标准输出与退出码必须逐字节相同。**

这不是期望而是**要求**，`make native-consistency` 逐字节比对并同时做两两互比
（`ok  interp and text agree with each other` 等）。历史上正是缺少这条不变性，
让「int 存成 double」的分叉长期无人发现。

因此：**任何改动数值、字符串、错误行为的补丁，都必须在 `tests/native-consistency.lume`
里加一条覆盖用例**，否则后端可以静默分叉。

### 8.1 已知缺口

这条不变式目前覆盖**成功的输出与退出码**，对**错误消息**只做到「都拒绝」，未做到逐字节一致。
已知八处，**现已全部修完**（第 4、5 条在前次提交；第 1、2 条在 #1/#2 那次；第 3 条经核查本就
一致，见其标注；第 7 条 `null` 字面量、第 8 条 `null` 的标量标注、第 6 条的 `Result` 本体与
`?` 传播分别在其标注的提交里）。原先标着「行为差异而非消息差异」的第 6 条是其中最严重的一条：
它不是诊断措辞不同，而是同一份合法程序解释器接受、原生后端拒绝，且 `?` 在两个后端**从未实现**
（`propagate` 零处理），一度降级成普通调用而**静默给出错误答案**。

第 1–5 条均属「拒绝执行形式的诊断差异」。第 6、7、8 条是「接受/拒绝不同」，现已全部修到「要么
都能跑、要么都拒绝」：第 7 条让 `null` 能跑，第 6 条让 `Result` 能跑，第 8 条把静默错值
（`null` 变 `0`）改成两后端一致拒绝——它修复前最严重，因为 libLLVM 后端给的是**自信的错误答案**
而不是拒绝。

闭包落地（§4.8）后的排查又新增第 9–12 条：第 9、11 条是「解释器接受、两个原生后端以
**同一条清晰消息**拒绝」的开放缺口——不是静默错值，属可接受的 phase-1 边界，但都记录在案；
第 10 条（顶层绑定不可见）与第 12 条（类型查找位置盲，排查 #10 夹具用例时撞出来的
**预先存在**缺陷、可能静默给错类型）此后均已修复（2026-10-08，见各自条目）。

1. **[已修复] `float %` 的报错位置**（§1.6）：`--compile-text` 与解释器报
   `'%' does not apply to floats`，`--compile-llvm` 原在更早的类型推断处报
   `print() cannot print this value`（下游 `cg_print` 把 `cg_binary` 先发出的正确错误覆盖了）。

   **本提交已修复**：`llvm_codegen.c` 的错误宏改为「首错优先」（`g->err` 已有内容时不覆盖），
   根因错误不再被 `cg_print` 的通用消息吞掉；同时 `infer_node_type` 新增 `N_BINARY` 分支，
   在类型推断阶段就把 `float %` 直接判为非法，与 text/解释器一致。
2. **[已修复] `let x = -7`（变量由一元负号初始化）在 libLLVM 下不可打印**：
   `let neg = -7; print(neg)` 原报 `print() cannot print this value`，而
   `let n = 7; print(n)` 正常。`cg_unary` 本身会给表达式打上 int 类型，缺口在
   `let` 绑定的类型传递——预扫描 `infer_node_type` 当时没有 `N_UNARY` / `N_BINARY` 分支，
   导致 `let neg = -7` / `let d = 0 - 7` 推不出类型。

   **本提交已修复**：`infer_node_type` 新增 `N_UNARY`（`not`→bool，负号→操作数类型）与
   `N_BINARY`（逻辑/比较→bool，算术按浮点/整型推算，`string+string`→string）分支，`let`
   绑定在 libLLVM 下也能正确携带类型，三后端一致。回归用例见 `tests/native-consistency.lume`
   的 `neg` / `diff`（`-7` / `-7`）。
3. **[已一致] `?` 的合法性检查**：原 SPEC 称其「落在 parser 层」并报 `expected ;, got ?`、
   位置跳行；但当前代码（`src/typecheck_expr.c:297/299/303`）已在**类型检查器**里检查，
   三条后端统一报 `'?' used on a call that does not return Result` /
   `'?' needs the enclosing function to return Result` 等清晰消息，位置正确。无需改动；
   `?` 真正未实现的部分见第 6 条。
4. **[已修复] 同一函数里出现两个同名绑定时，IR 文本后端编不出来**：局部变量的 IR 名是按
   **名字**生成的（`%lv_<name>`，`codegen.c` 的 `asg_push`），不是按**绑定**生成的，所以
   两个同名绑定撞在同一个 IR 名上。触发条件比「遮蔽」更宽：**同一函数内任何两个同名绑定**
   都撞，包括嵌套 `for` 复用同名（§4.2）、**顺序**两个 `for` 用同名、`for (let a ...)`
   之后又 `let a = ...`。同作用域的重复 `let` 由类型检查器先拦掉（`duplicate declaration
   of 'a' in the same scope`），所以这条只对**跨作用域的同名**生效——而那恰好是循环变量
   泄漏（§4.2）制造出来的情形。

   **本提交已修复**：`asg_push` 现在在名字后加一个进程级绑定序号（`%lv_a_0`、`%lv_a_1`），
   且 `codegen_stmt.c` / `codegen_expr.c` 里写死的 `%lv_%s` 改为走 `asg_find` 拿到的
   `a->slot`。嵌套/顺序同名 `for` 现在三个后端都能编译且行为一致（见 `tests/
   native-consistency.lume` 的 `seq_same` / `nest_same`）。

   ⚠️ 注意：跨作用域同名 `let`（如 `if (true) { let a = 2; }` 之后 `print(a)`）在解释器里
   会因遮蔽未恢复而打印内层值（`2 2`），这是**另一个未修的语义问题**，不在本次范围内；
   本次只消除 IR 冲突，不改动遮蔽语义。
5. 🔴 **[已修复] 在循环里 `push` 当前列表会让原生后端被 OOM 杀掉**（退出码 137）：

   ```lume
   let l = [1];
   for (let q in l) { l = push(l, 9); }
   print(str(len(l)));              // 解释器 2；修复前两个原生后端：无限循环 + 持续分配 → SIGKILL
   ```

   解释器按**绑定时的长度**走一遍，正确。两个原生后端的 for 没有把迭代器长度与列表长度
   分开，列表一变长就永不收尾。**这是本文件里唯一一条「不是拒绝、而是把进程跑挂」的缺口**，
   现已修复。

   **本提交已修复**：两个 emitter 的 `cg_for_in` 在进入循环时把长度存进一个独立的 alloca
   （与列表变量分开），循环条件 load 该快照，而不是每轮重读 `len(list)`。现在 `for` 遍历
   绑定时的长度快照，与解释器一致（见 §4.4 与 `tests/native-consistency.lume` 的 `pt`）。

修法分别已落地：第 1、2 条（libLLVM 的 `float %` 报错位置、`let` 绑定类型推断）于本提交
修复（见各自标注）；第 4、5 条（同名绑定 IR 冲突、for-in 长度快照）于前次提交修复。第 3 条
经核查本就已在类型检查器里、三条后端消息一致，无需改动。第 6 条（原生后端 `call?` 代码生成）
属大改，另立计划。第 7 条（`null` 字面量）于本提交修复（见其标注）。

6. **[已修复] 原生后端编不出 `Result` 与 `call?`**：解释器能跑
   `let q = div(21,7)?;`（见 §5），两个原生后端曾报 `variable 'q' has no codegen type`。

   ⚠️ **本条原先的诊断是错的，已更正**。它记的是「`?` 是 `propagate` 后缀、两个后端零处理，
   根因是 `?` 的 `ok` 载荷在类型检查器里是 `any_type()`（无泛型），属运行时值装箱的大改」。
   实测后发现**范围小得多**：不是 `?` 缺处理，而是 **`Result` 这个类型在两个后端根本不存在**——
   `llvm_type_of()` / `ty_of()` 都没有 `TY_RESULT` 分支，一律返回 NULL。于是连**不带 `?`** 的
   `let r = div(21, 7);` 都编不出来。所谓「装箱大改」是当时把编译失败误读成了表示能力不足。

   **本提交已修复（`Result` 本体）**：`Result` 就是 `{ ok: ... }` / `{ err: ... }`，解释器也正是
   当普通 map 处理的（`interp.c` 的 `propagate` 分支用 `map_get` 查 `err` / `ok` 两个键），
   所以原生后端不需要新表示——`llvm_type_of()` 与 `ty_of()` 各补一条 `TY_RESULT → i8*`
   （与匿名 map 同形），两个后端的 `cg_print` 各补一个走 `lume_map_print` 的 case。
   `Result` 的往返（`{ok:int}`、`{err:string}`）现在三后端一致，回归用例见
   `tests/native-consistency.lume` 的 `divide` / `show`。

   顺带修掉一个**既存的 IR 生成缺陷**：`codegen_stmt.c` 的兜底 `ret %s 0` 对指针返回类型会发出
   `ret i8* 0`——那不是合法的指针常量，clang 报 `integer constant must have integer type`。
   任何指针类型函数只要走到那条兜底就会踩到，`Result` 是第一个触发它的（沿 `if` 走完函数体就
   落空）。已按既有的 `double` 特例同样处理：指针补 `ret %s null`。

   **`?` 本身也已修复（后续提交）**：`?` 的结果类型此前确实是 `any_type()`，但**不需要泛型**。
   类型检查器改为跑**两遍**（`type_check_module` 里的 Pass D）：第一遍只收集每个 `Result` 标注
   从 `return { ok: X }` 得到的载荷类型，记在**该函数自己的 Result 类型对象**的 `elem` 上
   （`record_ok_payload`）；第二遍才真正检查，调用点读 `callee_t->ret->elem`。
   **两遍是必需的**：`later()?` 而 `later` 定义在后面时，单遍看到的载荷还是空的。
   收集遍期间 `ck_fail` 只记账不中止，且收集遍独占一个顶层 scope——否则第二遍会把第一遍的
   顶层 `let` 当成重复声明而误报。
   同一函数多个 `return { ok: X }` 的 `X` 不一致时**报错**（没有单一载荷类型，取第一个会错配）。
   两个原生后端据此生成：调用后 `lume_map_has(res, "err")`，命中则 `ret` 该 Result（错误原样
   向外传，不重写），未命中则按载荷类型调 `lume_map_get_i/f/s/obj` 读 `ok`。
   ⚠️ 若被调函数**只有 `err`、没有任何 `return { ok: X }`**，载荷类型仍未知，两个后端拒绝编译。
   `tests/native_backends.sh` 的 `propagate` 断言已从「必须拒绝」翻转为「三后端输出必须与解释器
   相同」，且 ok / err 两条路径都查。

7. **[已修复] 原生后端编不出 `null` 字面量**：`null` 在解释器里是合法值（打印 `null`、可赋给
   任意可空类型），但两个原生后端原本都编不出——`infer_node_type` 缺 `LIT_NULL` 分支，
   `codegen_expr.c` / `llvm_codegen.c` 的 `cg_literal` 直接报 `'null' literals are not
   supported by the native backend yet'`，属「解释器接受、原生后端拒绝」的不一致（与第 6 条同类，
   但更窄、已修）。

   **本提交已修复**：两个后端的 `infer_node_type` 增 `LIT_NULL → TY_NULL` 分支；`TY_NULL` 在
   原生后端统一降为不透明 `i8*` 空指针（`codegen_types.c` 的 `llvm_type_of` 与 `llvm_codegen.c`
   的 `ty_of` 均返回 `i8*`）；`cg_literal` 对 `LIT_NULL` 发 `null` / `LLVMConstNull(i8*)`；
   `codegen.c` 补 `declare i64 @lume_print_null()`、`rt.c` 新增 `lume_print_null`（打印 `null`、
   无参数）。三后端对 `null` 与 `print(null)` 现在一致。回归用例见 `tests/
   native-consistency.lume` 的 `n` 与 `print(null)`。

8. **[已修复] 标注了标量类型的 `null` 在 libLLVM 后端变成一个静默的错误值**：

   ```lume
   let x: int = null;
   print(x);          // 解释器 null；修复前 libLLVM 后端 0，文本后端编译失败
   ```

   类型检查器放行是**有意的**（`type_compat` 写着「all types nullable」，本语言没有可空类型
   的区分）。问题在两个 emitter 的 `coerce`：`TY_NULL` 原生表示是不透明 `i8*`，而文本后端
   没有 `i8* → i64` 这条边（直接拒绝），libLLVM 后端却有一条 `ptrtoint`，于是把 `null` 静默
   变成了 `0`（`bool` 则变成 `false`）。**一个后端拒绝、另一个后端给出错误答案**——这是三后端
   分歧里最坏的形态，因为它是静默的。

   更糟的一种：标成 `string` 时两边都编译通过，但发出去的是
   `call i64 @lume_print_str(ptr null)`，把空指针交给 `snprintf` 的 `%s`——**未定义行为**
   （glibc 碰巧印 `(null)`，但没有任何东西保证它）。

   **本提交已修复**：两个后端的 `coerce` 都显式拒绝 `TY_NULL` 流向标量类型，报同一条消息
   `cannot use 'null' as a int value in the native backend`（类型名用新的共享助手
   `src_type_name()`，因为 `ty_str()` 在 `typecheck_internal.h` 后面、emitter 不该依赖类型
   检查器）；指针形态的目标（`string`/`list`/`struct`）保持合法，`rt.c` 的 `lume_print_str`
   加了空指针防护、转印 `null`，与解释器一致。

   顺带修掉一个**同类的既有脆弱点**：`coerce` 失败时返回空 `Val`，但 6 个调用点里有 4 个不检查
   就继续用它——libLLVM 后端会 `LLVMBuildStore(NULL)` 段错误（ASan 定位在
   `CreateAlignedStore`）。现在每个调用点都以 `g->err[0]` 为准早退。

   回归断言见 `tests/native_backends.sh` 的 `null-as-scalar`（要求每个 emitter 都拒绝且措辞
   相同）与 `null-as-string`（要求每个 leg 都印 `null`）。这两条**不能**放进
   `native-consistency.lume`——那个 fixture 只 diff 三个 leg 都接受的程序，而这里要断言的正是
   「不运行」。

9. **[开放] 闭包变量重赋值，两个原生后端拒绝**（§4.8 落地时顺带定界）：

   ```lume
   let f = (x) => x + 1;
   f = (x) => x * 10;
   print(map(f, [1, 2]));    // 解释器 [10,20]；两个原生后端拒绝
   ```

   闭包变量的类型回指针记的是**第一个** lambda 的签名，而运行期盒子里装的是**最后一次**
   赋进去的闭包——两个 lambda 签名一旦不同，调用就会静默按错误签名 lowering。解释器动态
   无此问题。原生路径在签名 pass 里显式拒绝：`reassigning a closure variable is not
   supported on the native backends — the variable keeps the first lambda's signature`。
   修复前这条路径报的是 `internal: lambda was never named...`（第二个 lambda 从未被签名
   pass 命名，`internal:` 消息泄漏给用户）。

10. **[已修] 顶层/外层绑定在函数体内不可见**（预先存在，非闭包引入；2026-10-08 修复）：

    ```lume
    let base = 10;
    func add(x: int): int { return x + base; }
    print(add(5));            // 解释器 15；原生后端曾 `unknown variable 'base'`
    ```

    修法：每个顶层 `let`（及顶层 for-in 变量）提升为一个模块级 `@lv_<name>` 槽，顶层语句与
    函数体读写**同一份存储**——语义逐点对齐解释器的 env 查型：

    - **调用时读取**：函数读到的是调用前顶层最后一次写入的值（不是定义时的快照）。
    - **函数体内对顶层名赋值 = 新建函数级局部**（解释器同款）：不穿透写全局；赋值行之前的
      读仍解析到全局，之后的读看到局部——沿 #12 的行号规则。
    - **参数遮蔽顶层名**：参数胜。
    - 顶层 for-in 变量同样提升（循环后重绑的值/类型对后调用的函数可见）。

    **函数体内自由变量捕获（2026-10-15 起已修）**：lambda 捕获外层函数 / 外层 lambda 的
    局部（参数、`let`、列表等）已三后端一致——捕获数组存外层槽地址，读写直穿外层槽，
    共享存储（§4.8 Phase-2 段、§一阶段边界）。一级捕获 + 嵌套 HOF（内层 lambda 捕获外层
    lambda 参数）均可正常定型。

    **同一函数体内兄弟 lambda（2026-10-16 实测定性）**：**单向互调已三后端支持**——
    `let a = (x) => x * 2; let b = () => a(5) + 1;` 中 `b` 调用先定义的 `a`（含带参推断，
    输出 11）在解释器与两原生后端逐字一致（闭包变量调用路径，§4.8 Phase-2 顺带覆盖）。
    **后向引用**（`is_even` / `is_odd` 互调、lambda 递归自引用）由类型检查器按**词法声明
    顺序**三后端一致拒绝（`undefined variable '...'`，typecheck 先行拦截，解释器运行时
    同语义）——**不是后端差异**。若日后要支持互调/递归 lambda，需引入局部提升语义或把
    lambda 注册为可调用符号（另行排期）。**二级闭包捕获**（捕获已被外层闭包捕获
    的变量）与 **`return` 带捕获闭包**（逃逸守卫）编译期报错，见 §一阶段边界。

11. **[已修] 空列表字面量直接进 `map`/`filter`/`reduce`**：

    ```lume
    print(map((x) => x, []));   // 解释器 []；文本后端同
    ```

    空/any 列表的 lambda 参数以 `int` 定型（循环不执行，元素类型无法推出的情形安全
    落地）；原生后端与解释器输出一致。

    ```lume
    let e: int[] = [];
    print(map((x) => x * 2, e));    // [] 三后端一致
    print(reduce((a, x) => a + x, e, 100));  // 100 三后端一致
    ```

12. **[已修] 同名后绑定会让先前的 for-in 循环变量拿到错误类型**（§8.1 第 4 条的尾巴）：

    ```lume
    let xi = [1, 2, 3, 4];
    for (e in xi) { print(e); }   // 这里 e 应是 int（元素类型）
    ...
    let e: string = "x";          // 文件后部出现同名 let
    ```

    两个原生后端把全程序绑定**平铺**进一张 locals 表（`asg_push` 追加、`asg_find`
    从后往前找——「后者胜」），而发射是按位置的：for-in 循环变量的元素类型在发射时经
    `asg_find(名字)` 取回，命中的却是**文件更后处**同名 let 的类型。第 4 条当年只给 IR
    槽名加了绑定序号（`%lv_e_0` / `%lv_e_1`），类型查找仍是位置盲的。若后绑定的类型让
    `list_at_fn` 恰好可读（如都是 int），程序静默通过；若是 string/list 等不同类，
    for-in 要么报 `a list of this element type cannot be iterated`（本条被发现的形态，
    `native-consistency.lume` 的空列表用例首写时误用了 `e` 这个名字而撞上），要么按错误
    类型读元素——**静默错值**。

    **修复（2026-10-08）**：`Asg` 记录声明的行号，`asg_find` 改为位置感知——使用点只能
    看到**不晚于自己行号**的声明，取其中最新一条。这正是解释器的 set-or-define 语义：
    同名 `let` / for-in 变量按声明顺序依次重绑，使用点看到的是**最近一次**不晚于它的
    绑定（for-in 变量是函数级绑定，循环后仍可见、可被后继同名 `let` 再重绑；同块重复
    `let` 由 typechecker 先行拒绝，到不了 codegen）。两后端镜像同步落地；回归用例见
    `native-consistency.lume` 的 `sz()`（sibling 块同名异型，旧实现首块会读到未初始化
    的 i8* 槽）与 `w2`（for-in 重绑穿透）。

---

## 9. MCP stdio 服务器（`--mcp`）

`lume-core --mcp <script.lume>` 先运行脚本（顶层执行，`tool "name", "desc", {schema}, handler;`
语句把工具注册进 VM 的工具表），随后进入 MCP stdio 循环：**stdin/stdout 上按行读写的
JSON-RPC 2.0**（Windows 下 stdin/stdout 切换为二进制模式，行分隔严格）。

已实现的方法：

- `initialize` → 返回协议版本、能力（tools）与服务名（lume-core）。
- `tools/list` → 每个注册工具的 `name` / `description` / `inputSchema`（即
  `tool` 构造的 params 字面量，原样 JSON 输出）。
- `tools/call` → 按 `params.name` 找工具，把 `params.arguments`（JSON 解码后的值）
  作为**单个参数**传给 handler（`(params) => ...`），返回
  `{content:[{type:"text",text:...}],isError:false}`；字符串结果直接作为 text，
  其他值 JSON 序列化；handler 报错时 `isError:true` 并带错误文本。

JSON-RPC 错误按规范回 `-32700`（解析）/ `-32600`（请求）/ `-32601`（方法）/
`-32602`（参数）。通知（无 `id`）不回包。stdin EOF 即退出。

用途：任何脚本可把任意 lume 函数暴露成 MCP 工具，供 Claude Desktop / 豆包等
MCP 客户端调用——例如 hello-gen 的 `gen_crud`（实体模型 → 多栈 CRUD 生成）。

## 10. serve 会话（cookie session）

`serve()` 的每个请求都会获得一个会话：

- `req.session`：当前会话的数据 map，**跨请求共享**（同一 `lume_sid` cookie）；
- `req.session_id`：会话 id 字符串。

没有携带有效 `lume_sid` cookie 的请求会**新建**会话，响应带
`Set-Cookie: lume_sid=<id>; Path=/; HttpOnly; SameSite=Lax`，浏览器下次请求
即可恢复同一会话 map（此时不再下发 Set-Cookie）。会话表由 VM 持有并纳入
GC 根，进程生命周期内持续有效（重启即清空，不做持久化）。会话每请求创建
（简单、可推理）；未来可加 `server { sessions = off }` 关闭。

## 11. 破坏性变更流程

1. 先在 `tests/native-consistency.lume` / `tests/smoke.c` 写下**新**行为的期望；
2. 改实现，直到三后端与测试同时变绿；
3. 改本文档对应小节；
4. 在 [CHANGELOG.md](../CHANGELOG.md) 记一条，标注影响面（解释器 / 原生后端 / 两棵树）。

## 12. LSP 服务器（`--lsp`）

`lume-core --lsp` 启动语言服务器：stdio + LSP **Content-Length 帧**协议
（与 `--mcp` 的换行 JSON 不同，帧头 `Content-Length: <n>\r\n\r\n`）。

已实现：

- 生命周期：`initialize`（capabilities：全文同步、hover、补全）/ `initialized` /
  `shutdown` / `exit`；
- `textDocument/didOpen`、`textDocument/didChange`（全文同步）→ 对最新文本
  做 `parse_program` + `type_check_program` → 推送
  `textDocument/publishDiagnostics`；错误行号取自 `line N: ...` 前缀
  （1-based），映射为 LSP 0-based 行，暂无列信息；无错时推送空数组清掉旧诊断；
- `textDocument/hover`：光标处的标识符若在内建表（§6 的
  `LUME_BUILTIN_NAMES`）→ 返回 markdown 说明，否则 `null`；
- `textDocument/completion`：返回全部内建名（kind=Function）。

编辑器接入示例（VS Code 客户端配置指向本二进制）：

```json
{
  "lume.languageServer": {
    "command": ["E:/Workspace/lume-core/bin/lume-core", "--lsp"]
  }
}
```