# Lume 语言规格（SPEC）

这份文档是**规范性**的：它写明 Lume 「是什么」而不是「怎么实现」。实现改动后若与本文档冲突，
就是**实现错了**，需要改实现或走下面的流程改本文档，而不是让文档去追认实现。

- 它**不替代**实现，也不描述代码组织（那是 [ARCHITECTURE.md](ARCHITECTURE.md)）。
- 它**不承诺**宿主专属能力。宿主树 `work/research/lume` 的额外内建（`sql_*` 等）不在此列，
  见 [LUME.md §不承诺什么](LUME.md#不承诺什么)。
- 破坏性变更进 [CHANGELOG.md](../CHANGELOG.md)，并在下文标 **（规范变更）**。

本文档每一条都对着一个可执行的事实写成：能跑的例子在 `examples/`，能自动验证的例子在
`tests/native-consistency.lume`（三后端差分）与 `tests/smoke.c`（解释器，**163 个断言**）。
改动本文档时，**先改测试、再改实现**，否则等于把没验证的承诺写进规格。

**这一版覆盖什么**：数值类型、求值顺序、类型系统、`Result` / `?`、**全部 55 个内建**、
模块、三后端一致性。**没覆盖**：字符串的内建方法（本语言没有字符串方法，一切走函数）、
`for` / `while` 的完整语义、struct 类型的字段与方法的细节、闭包捕获规则。
这些以实现为准，改动时欢迎顺手补进本文档。

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

### 1.6 `/` 是浮点除法，`%` 是整数取模

- **`/` 永远是浮点除法**，即使两边都是 `int`：`7 / 2 == 3.5`。想要整数除法需自行约定。
- 除以 `0` 报 `division by zero`；`% 0` 报 `modulo by zero`。
- **`%` 两侧都是 `int` 时结果仍是 `int`**，且沿用 C 的符号约定（结果取**被除数**的符号）：

```lume
print(str(-7 / 2));                  // -3.5
print(str(-7 % 3));                  // -1
print(str(7 % -3));                  // 1
```

- **`%` 作用于 `float` 是错误**：`'%' does not apply to floats`。
  ⚠️ **已知实现差异**：`--compile-text` 与解释器给出这条消息，而 `--compile-llvm`
  目前在更早的类型推断处报 `print() cannot print this value`（它把 `%` 的结果类型推成别的）。
  两者都是**拒绝**，不是接受后算出不同的值，所以不会产生静默的错误结果；但消息不一致，
  待修（见 §6）。
- ⚠️ 整数 `%` 若**任一侧**是 `int` 而溢出（如 `LLONG_MIN % -1`），行为未定义。

### 1.7 一元负号保持数值类型

`-x` 的类型与 `x` 相同：`-7` 是 `int`，`-7.5` 是 `float`。

```lume
let a = -7;
print(str(a % 3));                    // -1，不是 float 取模错误
```

⚠️ `LLONG_MIN` 取负回绕到自身（`-(-9223372036854775808)` 仍是 `-9223372036854775808`），
与 1.2 的回绕规则一致，不是错误。

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

---

## 4. Result 与 `?` 传播

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
- ⚠️ 当前这条检查**落在 parser 层**，不是类型检查器：不合法的 `?` 报
  `expected ;, got ? ('?')`，而不是一条讲清「`?` 需要 Result」的诊断。这是已知的
  诊断质量问题（消息指向下一行分号，位置会跳），待改为类型检查器报错。
- 遇 `err` 时，当前函数**立刻返回该 `err`**，不再执行后续语句。
- `?` 的结果类型是 `Result` 里 `ok` 值的类型。

---

## 5. 内建函数

本节是**规范性**的：写明每个内建的签名、返回类型与边界行为。[LUME.md §内建函数](LUME.md)
只给名字清单，语义以本节为准。

### 5.0 通用约定

- **不能声明、不能遮蔽**：内建名已在类型检查器的名字表里（`typecheck.c` 的
  `LUME_BUILTIN_NAMES`），自己声明同名的 `func` 是**重复定义**错误。
- **参数多半是「松散」的**（内部 `any`），所以调用点通常不写类型标注。**返回类型不松散**：
  `keys` / `get` / `el` 等返回结构化类型，类型检查器知道它们的返回类型。
- ⚠️ **报错一律是运行时错误，不是陷阱**：内建不会 panic，被调脚本以非零码退出。
- ⚠️ 下面的「静默」指**不报错且给出一个值**。这是本语言最需要警惕的一类行为，已在每处标出。

### 5.1 输出与转换

| 内建 | 签名 | 返回 | 边界 |
| --- | --- | --- | --- |
| `print(x)` | 1 参| `null` | 字符串原样输出；`int` 按 i64（§1.1）；`float` 按 `%g`（§1.3）；其他走 JSON |
| `str(x)` | 1 参 | `string` | `int`→精确十进制；`float`→`%g`；`null`→`"null"`；map/list→JSON |
| `string(x)` | 1 参 | `string` | 与 `str` 同 |
| `int(x)` | 1 参 | `int` | **`int` 原样；`float` 截断；非数字字符串静默返回 `0`** |
| `float(x)` | 1 参 | `float` | `int`→float；⚠️ **非数字字符串静默返回 `0.0`** |
| `bool(x)` | 1 参 | `bool` | 真值性：空串/`0`/`null`/空集合为 `false` |
| `json(x)` | 1 参 | `string` | **只覆盖序列化方向**；无反序列化 |
| `stringify(x)` | 1 参 | `string` | 与 `json` 同（别名） |
| `now()` | 0 参 | `int` | Unix 秒 |
| `strftime(fmt, ts)` | 2 参 | `string` | ⚠️ **参数序是 `(格式, 时间戳)`**，不是反过来 |

⚠️ **`int("abc") == 0` 且不报错**，`float("x") == 0.0` 同理。这是转换类最容易踩的坑：
拼错的字段名会静默变成 0，而不是失败。

```lume
print(str(int("abc")));                // 0    ← 静默
print(str(int("3.9")));                // 3    ← 截断，不是四舍五入
print(str(strftime("%Y", 0)));         // 1970
```

### 5.2 集合

| 内建 | 签名 | 返回 | 边界 |
| --- | --- | --- | --- |
| `len(x)` | 1 参 | `int` | 字符串/列表/map 的长度；⚠️ **其他类型静默返回 `0`** |
| `keys(m)` | 1 参 | `string[]` | ⚠️ **只接受 map**，传 list 报 `keys() expects a map` |
| `get(m, k)` | 2 参 | 任何 | 缺键返回 `null` |
| `get(m, k, d)` | 3 参 | 任何 | 缺键返回 `d`（更安全，推荐） |
| `put(m, k, v)` | 3 参 | **map** | **原地**写 map，并返回写完的 map |
| `push(l, v)` | 2 参 | **list** | **原地**追加，并返回写完的 list |
| `range(a, b)` | 2 参 | `int[]` | `[a, b)`，**不含 b** |

⚠️ **脚本里没有下标语法**：`m["k"]` 与 `l[0]` 都**不存在**，读 map 一律用 `get`。
`len` 对非集合返回 0，所以 `len(env("X"))` 这种写法会静默给 0。

⚠️ **`put` / `push` 返回的是集合本身，不是 `null`**，所以可以链起来：

```lume
let m = put({ a: 1 }, "b", 2);        // {"a":1,"b":2}
let l = push([1], 2);                 // [1,2]
```

### 5.3 高阶函数

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

### 5.4 文件系统

| 内建 | 签名 | 返回 | 边界 |
| --- | --- | --- | --- |
| `read_file(p)` | 1 参 | `string` | ⚠️ **文件不存在静默返回 `null`**，不报错 |
| `write_file(p, data)` | 2 参 | `bool` | **原子写**（临时文件 + rename）；数据文件默认 `0600` |
| `mkdir(p)` | 1 参 | `bool` | 递归创建（等价 `mkdir -p`）；已存在返回 `true` |
| `files(p)` | 1 参 | `string[]` | 目录条目；目录带尾部 `/` |
| `lock_file(p, ms)` | 2 参 | `bool` | 跨进程建议锁；超时返回 `false` |
| `unlock_file()` | 0 参 | `bool` | 释放本进程持有的锁 |

⚠️ `read_file` 对不存在的文件**不报错**，脚本里「读配置」失败会表现为后续对 `null` 的处理，
而不是一条清晰的错误。检查存在性请用 `files()`。

### 5.5 环境与加密

| 内建 | 签名 | 返回 | 边界 |
| --- | --- | --- | --- |
| `env(name)` | 1 参 | `string` | ⚠️ **未设置静默返回 `null`** |
| `crypt_sha512(pw)` | 1 参 | `string` | ⚠️ **平台相关**：无 `crypt(3)` 的平台（含 macOS）报 `SHA-512 crypt unavailable on this platform` |

### 5.6 数学

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

### 5.7 页面小工具（虚拟 DOM）

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

### 5.8 工具台（空壳）

本树无 Agent 运行时，这四个恒返回空值但**保留可用**（不在不同平台间分叉语言面）：

| 内建 | 返回 |
| --- | --- |
| `tools()` / `skills()` | `[]` |
| `mcps()` | `[]` |
| `catalog()` | `{ skills: [], tools: [], mcps: [...] }` |
| `discovery_endpoints()` | `{ llm: null, router: null, model: null }` |

### 5.9 出站 HTTP

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
- ⚠️ **本树无 Winsock**：`LUME_HAS_HTTP=0`，这些内建在 Windows 构建里**存在但调用即报错**
  （见 §5.9 与 §7 的 host/core 差异）。

### 5.10 名字在表里但不可调用

`typecheck.c` 的 `LUME_BUILTIN_NAMES` 里有四个名字**没有对应实现**，写出来会得到
`undefined variable`：类型词 `type`、`Result`（用于标注而非调用）、常量 `pi`/`e` 是
`builtins_math.c` 的常量而非常量名。动词组 `read`/`write` 是保留的组名（见 `seed_verb_groups`）。

⚠️ 这意味着 `type(1)` 不是「取类型」——本语言**没有**取运行时类型的内建。

---

## 6. 模块

### 6.1 导入按「引用文件所在目录」解析

`import "x.lume" as ns;` 的路径相对于**引用它的那个文件**的目录，不是进程 CWD：

```lume
// examples/modules/app.lume
import "tax.lume" as tax;             // 指 examples/modules/tax.lume
```

导入路径先拼接、再规范化（消掉 `a/../b`），规范化结果**必须是绝对路径**。

### 6.2 循环导入是错误

直接或间接导入自己会被拒绝并报
`circular import: '<path>' imports itself (directly or indirectly)`，不会栈溢出。

### 6.3 只有 `export` 的顶层声明可见

`export let` / `export func` / `export type` 才被外部模块看到；未导出的顶层声明是该模块私有的。

---

## 7. 三后端一致性（可执行的不变式）

Lume 有三条执行路径：**解释器**、**原生后端 A（手写 IR 文本）**、**原生后端 B（libLLVM）**。

**不变式：对同一份 `.lume`，三者的标准输出与退出码必须逐字节相同。**

这不是期望而是**要求**，`make native-consistency` 逐字节比对并同时做两两互比
（`ok  interp and text agree with each other` 等）。历史上正是缺少这条不变性，
让「int 存成 double」的分叉长期无人发现。

因此：**任何改动数值、字符串、错误行为的补丁，都必须在 `tests/native-consistency.lume`
里加一条覆盖用例**，否则后端可以静默分叉。

### 7.1 已知缺口

这条不变式目前覆盖**成功的输出与退出码**，对**错误消息**只做到「都拒绝」，未做到逐字节一致。
已知三处，都不是本轮的int 改造引入，且都不产生错误结果（都是拒绝执行），但会误导读者：

1. **`float %` 的报错位置**（§1.6）：`--compile-text` 与解释器报
   `'%' does not apply to floats`，`--compile-llvm` 在更早的类型推断处报
   `print() cannot print this value`。
2. **`let x = -7`（变量由一元负号初始化）在 libLLVM 下不可打印**：
   `let neg = -7; print(neg)` 报 `print() cannot print this value`，而
   `let n = 7; print(n)` 正常。`cg_unary` 本身会给表达式打上int 类型，缺口在
   `let` 绑定的类型传递。因此本文件 §1.7 的用例放在 smoke 套件而非三后端靶子。
3. **`?` 的诊断落在 parser 层**（§4），报 `expected ;, got ?`，而不是讲清「`?` 需要
   Result」的消息，且位置跳到下一行分号。

修法分别是：让 libLLVM 在类型推断里把 `float %` 与「负号初始化的 let」直接判为非法，
以及把 `?` 的合法性检查从 parser 移到类型检查器。

---

## 8. 破坏性变更流程

1. 先在 `tests/native-consistency.lume` / `tests/smoke.c` 写下**新**行为的期望；
2. 改实现，直到三后端与测试同时变绿；
3. 改本文档对应小节；
4. 在 [CHANGELOG.md](../CHANGELOG.md) 记一条，标注影响面（解释器 / 原生后端 / 两棵树）。
