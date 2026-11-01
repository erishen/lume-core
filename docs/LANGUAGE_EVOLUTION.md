# lume 语言演进提案 —— 七算法库实战产出

来源：`lib/{sci,linalg,optim,sig,graph,ml}.lume` 六库（90+ 导出函数）开发与
三后端一致性验证中实测踩到的语言限制。每条含**现象（报错原文）→ 踩坑场景 →
当前规避 → 提案 → 验收标准**。探针结论均可用 `tests/` 下一致性套件复现。

## 0. 事实澄清（2026-11-01 探针，修正此前口头结论）

| 能力 | 解释器 | 文本 IR | libLLVM | 备注 |
|---|---|---|---|---|
| 无捕获 lambda `let f = (x) => x*2;` | ✓ | ✓ | ✓ | SPEC §4.8，头等值 |
| `let g: fn(float) => float = ...` 注解 | ✓ | ✓ | ✓ | **胖箭头** `=>`，非 `->` |
| `fn` 类型作 func 参数并间接调用 | ✓ | ✗ | ✗ | 两编译后端同报 `call to unknown function 'f'` |
| `int(2.7)` 显式转换 | ✓ | ✓ | ✓ | 注解赋值 `let x: int = 1.5` 仍拒绝 |
| `floor/round/ceil` | ✓ | ✓ | ✓ | **`trunc` 不存在** |
| 命名 struct | = 命名的 map（值语义），无 C 式 `type X { }` 声明 | | | 见 `tests/eq2.lume` |

> 此前库内注释写「lume 不支持函数类型参数」不准确——解释器支持，是**编译后端
> 缺 fn 参数的间接调用**。sci/optim 的参数化变体（`bisect_poly`/`rk4_poly` 等）
> 正是绕开这一缺口的设计。

---

## P0-1 编译后端支持 `fn` 参数的间接调用（等价性缺口之首）✅ 已实现（2026-11-01）

- **现象**：`func apply(f: fn(float) => float, x: float): float { return f(x); }`
  解释器输出正确；文本/LLVM 后端报 `call to unknown function 'f'`。
- **踩坑场景**：所有「高阶算法」——通用求根、通用 ODE、通用极小化、回调式
  优化。六库被迫每个通用算法做 2–3 个参数化变体（`bisect_poly`/`newton_poly`/
  `rk4_{linear,affine,poly}`/`golden_poly_min`），库体积与 API 面翻倍。
- **提案**：fn 值降为 `{fnptr, env}` 二元组。无捕获 lambda 的 env 为空、退化为
  纯函数指针（编译后端已有 lambda 头等值基础）；捕获环境用现有 i8* 堆分配。
  先支持「无捕获 lambda + 顶层函数引用」作 fn 实参，捕获闭包可列 phase-2。
- **验收**：`tests/_probe_fn8.lume` 中的 `apply`/`integrate` 用例三后端一致；
  sci/optim 各删掉参数化变体、提供通用版本，六套一致性测试保持全绿。

## P0-2 整数除法与求模内置 ✅ 已实现（2026-11-01）

- **现象**：lume 的 `/` 恒产生 float（无 int/int→int），`w/2` 当索引用直接报
  `a list index must be an int`；求模写 `x - 2*(x/2)` 对奇数**静默出错**
  （`2.0*(1.0/2.0)==1.0` 精确还原，余数恒 0）——sig.lume 的 bitrev 因此全错
  过一轮才被发现。
- **踩坑场景**：sig（bitrev/FFT 分桶）、graph（BFS/拓扑的队列指针）、ml
  （kNN 投票计数）。
- **提案**：新增内置 `div(i: int, j: int): int`（截断除）与 `mod(i: int,
  j: int): int`（符号随被除数，与 C 一致）。不动 `/` 语义（避免破坏既有程序）。
- **验收**：`mod(7,2)==1`、`div(7,2)==3` 三后端一致；bitrev 改用 `mod` 后
  FFT 一致性测试保持等价。

## P0-3 HTTP 方法关键字不应全局保留 ✅ 已实现（2026-11-01）

- **现象**：`head/get/post/put/patch/delete/options` 是 `server`/`route` 块的
  路由方法关键字（`src/lexer.c` KEYWORDS 表），却**全局**保留——graph.lume 的
  BFS 队列指针取名 `head` 直接报 `expected variable name after 'let'`，毫无
  「这是保留字」的提示。
- **提案**：方法关键字仅在 route 语法上下文作为关键字，其余位置降级为普通
  标识符（lexer 记上一 token 上下文或 parser 回退接受）；短期至少把报错改为
  `'head' is a reserved keyword`。
- **验收**：`let head = 0;` 在非 route 上下文三后端可用；既有 server 示例
  回归不变。

## P0-4 诊断信息增强（低成本高收益）✅ 首批已实现（2026-11-01）

- `cannot infer a type for 'x'` 不指出**哪个调用的返回值**推不出来——
  ml/optim 里每个此类错误都要人工回溯（本地函数返回值、native 调用绑定均需
  显式标注，导入函数又能推断，规则不一致极难记）。建议：消息附「绑定目标
  函数名 + 该函数是否本模块/导入」。
- `a list index must be an int` 常由 `/` 间接导致（`x[i/2]`）。建议附提示
  「index 表达式含 `/`，其结果为 float；是否需要 floor()/div()？」。
- 编译后端 lambda 参数缺标注的报错已是良好示范
  （`parameter 't' of the lambda has no known type — annotate it or call it
  with a value of a known type`）——以此为准绳统一诊断风格。

## P1-1 本地函数返回值类型推断（编译后端）

- **现象**：`let x = 本模块函数(...)` 报 `cannot infer a type for 'x'`；
  `let cs = cos(ang)`（native 调用）同样；但**导入函数**（`la.mat_mul(...)`）
  可推断。同一份 ml-consistency 在解释器通过、文本后端编译失败，靠逐个补
  `: float[]` 修绿。
- **提案**：checker 第二遍把已解析的函数签名（本地/导入/native）传播到
  let 绑定。机械工作，收益是消除库开发中最高频的编译报错。

## P1-2 数组字面量的期望类型下推

- **现象**：含变量的嵌套字面量（`[[1.0, INF], ...]`）被推断为 `?[]`，传给
  `float[][]` 参数报签名不匹配；须先 `let W: float[][] = [...]` 再传入。
- **提案**：调用点的参数期望类型下推进字面量子表达式（checker 已有 hint
  机制，字面量路径未走全）。

## P1-3 `let` 块作用域（或精确报错）

- **现象**：`let` 为函数作用域，同一函数内所有循环变量名必须唯一（哪怕位于
  互不嵌套的块），报 `duplicate declaration of 'j'`。optim.linprog 内 15 个
  循环变量被迫命名 `ri/ci/si/oj/...`。
- **提案**：块作用域为长期方案；短期把报错改为指出「与第 N 行的 'j' 冲突」。

## P1-4 确定性 RNG 内置

- **现象**：无随机数。ml.mlp2_init 被迫用 sin-hash
  `frac(sin(k·12.9898+seed·78.233)·43758.5453)` 自造，质量与周期均有限。
- **提案**：内置 `rand_next(state: int): int`（如 PCG/ splitmix64，纯整数
  算术 + 位运算）或带种子的 `rand(seed)` 序列，三后端逐字节一致。
  与 P1-5 位运算配套。

## P1-5 整数位运算内置

- **现象**：bitrev/哈希/打包整数场景无 `shl/shr/and/or/xor`，只能算术模拟
  （且因 `/` 为 float 而坑上加坑）。
- **提案**：`shl(i,n)/shr(i,n)/bitand/bit_or/bit_xor` 六个 int 内置，走
  BUILTINS 表（text/LLVM 两份同步，参照 trig 内置先例，成本已知且低）。

## P2-1 受控可变数组（性能）

- **现象**：值语义下逐元素更新经 `set_flt` 全量拷贝，`set_m` 更是每元素
  O(行拷贝+矩阵拷贝)。ml.mlp2_train 全批量反传实际是 O(iters·n·(h·d)²)
  的隐式平方项；kmeans 梯度累加同病。
- **提案**：`mut` 局部数组（仅限函数内、禁止逃逸返回），或内置批量构造
  `fill_flt(n, f: fn(int) => float)` / `fill_mat(...)` 一次成型。前者快、
  后者保守（不动语义）。

## P2-2 多返回值 / 元组

- **现象**：MLP 权重四件套（W1/b1/W2/b2）打包为 `float[][]` 约定格式
  （h+3 行），靠文档维系；LU 分解返回压包矩阵再配 `lu_l/lu_u` 拆包。
- **提案**：`(A, B)` 元组返回 + 解构 `let (l, u) = ...`；或把命名 struct
  （命名的 map）扩展出类型声明语法并保证编译后端字段 GEP 效率
  （SPEC 已有 struct `==` 语义基础）。

---

## 已在本仓库修复的相关缺陷（2026-10-31）

| 提交 | 缺陷 |
|---|---|
| `1c21e6d` | 文本后端拒绝 `1e+30` 形态浮点字面量（`%.17g` 剥尾随零致 IR 无小数点） |
| `74300b2` | checker 收集遍诊断吞没（`ck_collect_note()` 从未实现 + 注解提前写回），曾致 `let x: num = 1.5` 三后端三种行为、libLLVM 运行段错误 |
| `f13c030` | `%.o` 规则缺 Makefile 依赖，CFLAGS/版本号变化后旧对象继续链接（幻绿） |

## 实现落点（2026-11-01，tests/p0-consistency.lume 三后端一致全绿）

| 项 | 实现 | 验证 |
|---|---|---|
| P0-1 | 文本/LLVM 后端新增 `cg_annotated_fn_call`：无 lambda 回链的 TY_FUNC 值按注解签名（`types[]`/`ret`）做 closure 间接调用（`{fn,cap}` ABI 与 lambda 一致） | fn 形参调用/高阶积分/fn 返回值/二元 fold 四类用例三后端逐字节一致 |
| P0-2 | `div/mod` int 内置：interp native + rt.c 镜像 + 两张 BUILTINS 表（i64 flavour）；除零约定返回 0；仅接受 int 实参（float 实参三后端一致拒绝，符合 SPEC §3.2） | 负数/除零/作索引用例一致 |
| P0-3 | `let` 绑定名接受 HTTP 方法关键字（仅 route 简写位置保留关键字语义），parser_stmt.c 两处 | `let head = 0` 三后端可用，route 示例回归不变 |
| P0-4 | `cannot infer` 附被调函数名与修复建议；list index 非 int 且含 `/` 时附 floor()/div() 提示 | 两条消息实测输出 |

另：探针修正 P1-4——lume 已有完整 Monte Carlo RNG（`seed/rand/rand_int/...`，
2026-10-24 落地），该项从"新增"改为"文档化 + 一致性验证"。

## 优先级总览

| 级别 | 项 | 一句话理由 |
|---|---|---|
| P0 | fn 参数间接调用 | 等价性缺口 + 库 API 瘦身一倍 |
| P0 | div/mod 内置 | 静默错值风险最高的一条 |
| P0 | 保留字上下文化 | 纯误伤，修复成本最低 |
| P0 | 诊断增强 | 全部后续开发受益 |
| P1 | 返回类型推断 / 字面量下推 / 块作用域 / RNG / 位运算 | 高频摩擦逐个消除 |
| P2 | 可变数组 / 元组 | 性能与人体工学，等库规模再验证 |
