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

## P1-1 本地/导入/native 函数返回值类型推断（编译后端）✅ 已实现（2026-11-01）

- **现象**：`let x = 本模块函数(...)` 报 `cannot infer a type for 'x'`；
  `let cs = cos(ang)`（native 调用）同样；但**导入函数**（`la.mat_mul(...)`）
  可推断。同一份 ml-consistency 在解释器通过、文本后端编译失败，靠逐个补
  `: float[]` 修绿。
- **实现**：根因是 `infer_node_type` 的 `N_CALL` 通用路径只 `sig_find` 本地/
  导入函数签名，**native 内置不以 bare name 注册进 sigs**，且 `sin/cos/tan/
  sqrt/pow/floor/ceil/round/abs/min/max` 等数学/trig 内置漏在硬编码推断列表
  之外（只有 `exp/log/ln` 在列），于是落到 `sig_find` 返回 NULL。修法：
  - 新增共享 helper `builtin_infer_ret(name, arg0)`（`src/codegen_call.c`，
    基于 `Bi` 表）：float-only 内置恒答 float、int-only 恒答 int、多态
    `abs/min/max` 按首参静态类型答 int/float；首参类型未知则答 NULL（安全
    回退，绝不静默错型，与 `cg_builtin` 的 flavour 选择逐位对齐）。
  - 文本后端 `codegen_scan.c` 与 LLVM 后端 `llvm_cg_scan.inc` 的 `N_CALL`
    通用路径改为「先 `sig_find` 本地/导入函数，查不到再 `builtin_infer_ret`」
    —— 单一数据源，`Bi` 表即权威，新增内置自动获得推断，不再维护两份长列表。
- **验收**：`tests/p1-1-consistency.lume` 覆盖 native trig/math 内置、本地函数
  （标注/无标注/返回 list）、导入函数、嵌套 `abs(local_f(...))`、链式
  `local_f(cos(...))`、多态 `abs/min/max` 按首参类型；三后端逐字节一致。首参
  类型未知时仍正确报 `cannot infer`（带提示），无类型不匹配崩溃。
- **收益**：消除库开发中最高频的「native 调用须补 `: float` 标注」摩擦；
  `ml`/`sci`/`optim` 等库后续可甩掉大量冗余标注。

## P1-2 数组字面量的元素类型通用推断 ✅ 已实现（2026-11-01）

- **现象**：含变量的字面量（`[big, 1.0]` / `[[1.0, INF], ...]`）被推断为
  `?[]`，传给 `float[]` / `float[][]` 参数报「argument 1 of 'f' is a ?[],
  but the parameter was already resolved as a float[]」；须先
  `let a: float[] = [big]` 再传入。
- **根因**（比提案的 hint 下推更根本）：两后端各自的 `infer_list_elem`
  （`codegen_lit.c` / `llvm_cg_scan.inc` 镜像）只认字面量/ctor/元组/嵌套
  list 元素，**任何其他元素（变量 N_VAR、索引、调用、算术、取负变量）都
  返回 NULL → 整个字面量塌 list<any>**——连变量自己的已注册类型都没查。
- **实现**：两份 `infer_list_elem` 末尾加通用回退分支——问通用
  `infer_node_type`（slots→globals→scope→arm 解析链、sig_find→
  builtin_infer_ret），只接受可静态拼写的类型（prim/list/enum）；any 与
  struct（匿名形状归一由既有 anon 规则与 ck_list 管）维持旧塌缩行为。
  顺带修 `OP_NEG` 分支的吞元素问题：它匹配取负节点但只答字面量操作数，
  `-big` 曾被它消费后强制塌缩——现在仅当操作数是字面量时接管，其余落到
  通用回退。
- **验收**：`tests/p1-2-consistency.lume` 三后端逐字节一致：1-D 变量元素
  直传、取负变量、索引元素、2-D 嵌套含变量/混合内层/变量行、int 侧、
  native 调用元素（`[sqrt(4.0), cos(0.0)]`）、for-in 消费；异质塌缩
  （float+int 混合 → list<any>）行为不变。14 套 backend 套件 +
  case-parity + lsp_smoke 全绿。

## P1-3 `let` 块作用域（或精确报错）

- **现象**：`let` 为函数作用域，同一函数内所有循环变量名必须唯一（哪怕位于
  互不嵌套的块），报 `duplicate declaration of 'j'`。optim.linprog 内 15 个
  循环变量被迫命名 `ri/ci/si/oj/...`。
- **提案**：块作用域为长期方案；短期把报错改为指出「与第 N 行的 'j' 冲突」。

## P1-4 确定性 RNG 内置 ✅ 已实现（2026-11-01）

- **现象**：ml.mlp2_init 被迫用 sin-hash
  `frac(sin(k·12.9898+seed·78.233)·43758.5453)` 自造，质量与周期均有限。
  2026-10-24 已有全局 PCG32 流（`seed/rand/rand_range/…`，三后端一致），但
  **全局流有序、不可分叉**：按下标取值必须按序推进，无法并行/乱序。
- **实现**：纯函数式 splitmix64 两个内置（纯整数算术 + P1-5 位原语，无隐藏
  状态、序无关，`rand_next(seed + k)` 按下标独立取值）：
  - `rand_next(s: int): int`——s+黄金比例常数后过 finalizer（两次 xor-shift
    乘法混合）；
  - `rand_float(s: int): float`——输出高 53 位 × 2^-53 映射到 [0,1)；
  - 接线：`builtins_math_basic.inc`（interpreter natives + b_* 包装）、
    `builtins.h`、`interp.c` 注册、`rt.c`（`lume_bi_rand_next/rand_float`）、
    两张 BUILTINS 表（`codegen_call.c` + `llvm_codegen_internal.h`）、
    typecheck 白名单/元数（1 元）。
  - **Bi 表首个「int flavour 行、double 返回」**（rand_float）：顺带修三处
    潜伏错位——① `builtin_infer_ret` 从「有 fn_i 就答 int」启发式改为读真实
    `rty` 列（单一数据源再进一步）；② `cg_builtin`（text）与 llvm 等价发射
    点的结果 Val 类型从 `want`（coerce 后实参类型）改为跟随 `rty` 列（既有行
    pty==rty 时两者恒等，从未暴露；本次被 rand_float 双双炸出）。
  - **顺手修 P1-6 漏网缺陷**：hex 数位含 `e/E`（如 `0x3E5`、splitmix 常量
    `0x9E3779B97F4A7C15`）被浮点启发式当科学计数法定型成 float。修法：带
    `0x/0X/0b/0B` 前缀的字面量在 `emit_number` 跳过 `.`/`e`/`E` 浮点探测，
    直接走整数分支（带前缀即不可能是浮点，无 `0x1.8p3` 支持）。
  - **库收益**：`ml.lume` 的 `hash01` 换 `rand_float(k * 0xF4243 + seed)`
    （0xF4243=1000003 质数混匀下标，P1-6 字面量首次实战），sin-hash 退役；
    mlp2_init 数值序列改变（确定性不破坏，ml 套件在新序列下自洽）。
- **验收**：`tests/rand-consistency.lume` + `rand_backends.sh`：固定种子黄金
  值锚点、同态可复现/异态异值、链式喂回、逐点取值互异、1000 样本落域
  [0,1) 与均值 ~0.5 的确定性统计、**lume 侧用 P1-5 位原语手工展开 splitmix64
  与 C 实现逐算术步互证**（含 >2^63 常量写补码负数的 P1-6 边界实践）、
  P1-1 免标注推断 int/float 各一；13 套 backend 套件 + case-parity +
  lsp_smoke 全绿。

## P1-5 整数位运算内置 ✅ 已实现（2026-11-01）

- **现象**：bitrev/哈希/打包整数场景无 `shl/shr/and/or/xor`，只能算术模拟
  （且因 `/` 为 float 而坑上加坑）——`lib/sig.lume` 的 `bitrev` 被迫用
  `floor`/mod 浮点模拟，曾因「乘回去精确还原」把奇数误判为偶数。
- **实现**：六个纯 int 风格内置 `shl/shr/bitand/bit_or/bit_xor`，完全复用
  P0-2 `div/mod` 的 `BUILTINS[i64 风格]` 机制（text/LLVM 两份表同步 +
  `rt.c` 镜像 + `interp.c` 注册 + `typecheck.c` 白名单/元数），零新机制。
- **语义约定**：移位按无符号 64 位**逻辑**移位（右移非负号扩展），移位数先
  掩 `& 63`（C 对 `>=64` 移位未定义 → 掩码防 UB）；仅接受 int 实参（float
  实参解释器拒绝、编译后端 coerce，与 `div/mod` 同行为）。`and/or/xor` 不是
  lume 表达式运算符，故命名为 `bitand/bit_or/bit_xor`（提案原命名）。
- **验收**：`tests/bitops-consistency.lume` 覆盖基本位运算、负数逻辑右移、
  大移位数掩码、`bitrev8` 组合、与 `div/mod` 回归，三后端逐字节一致。
- **库收益**：`lib/sig.lume` 的 `bitrev` 改用 `shl/shr/bitand/bit_or` 重写，
  消除浮点模隐患，`sig` 套件三后端不变。

## P1-6 十六进制 / 二进制整数字面量支持 ✅ 已实现（2026-11-01）

- **现象**：写 `0xF0` / `0b1010` 会被 lexer 拆成 `0` + 标识符（`xF0` /
  `b1010`），直接报 `unexpected token` / `unknown identifier`；位模式只能
  十进制写。
- **实现**：scanner 在数字分支前探测 `0x`/`0X`/`0b`/`0B` 前缀，按对应进制
  收集数字（hex 用 is_hexdigit、binary 只收 0/1），`emit_number` 按前缀选
  `strtoll` 基数 16/2。int 仍是有符号 i64（二进制补码），溢出报错；与十进制
  不同，hex/binary **不提供** 2^63 负号可化特例（`-0x8000000000000000` 不可
  表示，直接拒绝——十进制 `-9223372036854775808` 的 clamp+negate 机制对
  hex 无意义）。前缀后无有效数字（`0x`、`0xG`、`0b2`）是词法错误。
- **验收**：`tests/literals-bitops-consistency.lume` 覆盖大小写前缀、负号、
  i64 最大值 `0x7FFFFFFFFFFFFFFF`、与十进制混算、经函数往返（P1-1 推断
  协同）；三后端逐字节一致。

## P1-7 位运算中缀运算符（`|` / `&` / `^`）✅ 已实现（2026-11-01）

- **现象**：`|` / `&` / `^` 不是表达式运算符，写 `a | b` 报 `expected ;`；
  只能调用函数式 `bit_or(a,b)` / `bitand(a,b)` / `bit_xor(a,b)`。
- **实现**：lexer 把单写 `&` 从 `TOK_ERROR` 改为 `TOK_AMP`、新增 `^` →
  `TOK_CARET`（`||` 逻辑或、`&&` 逻辑与不变；单写 `|` 复用既有 `TOK_PIPE`，
  其 enum 变体分隔与 trailing-closure 定界用法均在各自语法位置、与表达式
  位或不冲突）。parser 在 `parse_term`（`+ -`）与 `parse_comparison` 之间插
  入三层：`parse_bitor → parse_bitxor → parse_bitand → parse_term`——即
  `&` 最紧、`^` 次之、`|` 最松，整体**低于 `+ -`、高于比较/相等**
  （`a & b == c` 解析为 `(a & b) == c`）。三后端各加发射：
  interp int 分支直接 `&`/`|`/`^`；文本 IR `and/or/xor i64`；libLLVM
  `LLVMBuildAnd/Or/Xor`；两后端 `infer_node_type` 显式答 int（防浮点侧
  误宽化）。`bitand`/`bit_or`/`bit_xor` 函数保留为等价别名。
- **验收**：`tests/literals-bitops-consistency.lume` 覆盖基本运算、与函数式
  别名等价、优先级（`7 & 3 == 3` 为 true、`1 | 2 + 3` 为 5）、左结合链、
  与 shl/shr、div/mod、负数补码组合；三后端逐字节一致。enum `|` 变体分隔
  （tests/enum.lume）与 trailing closure（tests/hof-consistency.lume）回归
  不变。

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
| P1-5 | `shl/shr/bitand/bit_or/bit_xor` 六个 int 内置：复用 `div/mod` 的 `BUILTINS[i64 风格]` 机制（text/LLVM 表 + rt.c 镜像 + interp.c 注册 + typecheck 白名单/元数）；移位按无符号逻辑移位、移位数掩 `&63` 防 UB | `tests/bitops-consistency.lume` 三后端逐字节一致；`lib/sig.lume` 的 `bitrev` 改用原语重写消除浮点模隐患 |
| P1-1 | 共享 helper `builtin_infer_ret(name, arg0)`（`src/codegen_call.c`，基于 `Bi` 表）：float-only 恒答 float、int-only 恒答 int、多态 `abs/min/max` 按首参静态类型；文本/LLVM 两后端 `N_CALL` 通用路径改为「先 `sig_find` 本地/导入函数，查不到再 `builtin_infer_ret`」（单一数据源，新增内置自动获得推断） | `tests/p1-1-consistency.lume` 三后端逐字节一致；覆盖 native trig/math 内置、本地函数（标注/无标注/list）、导入函数、嵌套/链式、多态按首参类型；首参未知仍报 `cannot infer`（带提示），无错型崩溃 |
| P1-6 | scanner 探测 `0x/0X/0b/0B` 前缀按进制收数字，`emit_number` 按 base 16/2 `strtoll`；有符号 i64 溢出报错，无 2^63 负号可化特例 | `tests/literals-bitops-consistency.lume` 三后端逐字节一致；大小写前缀/负号/i64 最大值/混算/函数往返 |
| P1-7 | 单写 `&`→`TOK_AMP`（原 TOK_ERROR）、新增 `^`→`TOK_CARET`、单写 `|` 复用 `TOK_PIPE`；parser 在 term 与 comparison 间插 `parse_bitor→parse_bitxor→parse_bitand→parse_term` 三层（低于 `+ -`、高于比较）；interp int 分支 + 文本 IR `and/or/xor i64` + libLLVM `LLVMBuildAnd/Or/Xor` + 两 scan 显式答 int | 同上套件；优先级（`(a&b)==c`）、别名等价、enum/trailing-closure 回归不变 |

另：探针修正 P1-4——lume 已有完整 Monte Carlo RNG（`seed/rand/rand_int/...`，
2026-10-24 落地），该项从"新增"改为"文档化 + 一致性验证"。

## P0-1 后续缺口与算法库升级（2026-11-01，tests/hof-consistency.lume 三后端一致全绿）

P0-1 落地后把 sci/optim 的参数化变体升级为通用 fn 接口时，探针暴露两个
编译器缺口（均在"lambda 捕获"与"跨模块"的组合下）：

| 缺陷 | 现象 | 根因 | 修复 |
|---|---|---|---|
| LLVM 捕获 ptr-like 参数 | lambda 捕获 `float[]`/`float[][]`/fn 形参 → SIGSEGV | 捕获数组的约定是"存外部 slot 的**地址**"，读回侧解引用一次；但 ptr-like 参数的 slot IS the value（无 alloca），创建侧存了值本身 | `llvm_cg_lit.inc` 创建侧对 ptr-like 参数先 box 进临时 alloca（对齐文本后端"每个参数都有 alloca"的约定）；`ptr_like_param` 上移到 `llvm_codegen_internal.h`（lit.inc 在 stmt.inc 之前 include） |
| 跨模块 lambda 返回类型推断失败 | 被 import 模块内 `(t: float) => a.f(t)` 表达式体 → `define void` 包 `ret double` 的坏 IR（clang 拒） | 各模块先独立推断（无依赖模块签名），entry 推断时 shared 表只注入**命名函数**不含模块 lambda，lambda 的 ret 无从补推 | `codegen_infer_signatures_multi` 把每个模块的 lambda（cname 已盖戳）也注册进 shared，entry 固定点补推 ret/params 并写回 AST（写回只填空槽，幂等） |
| 文本/LLVM 闭包内 plain call 跨模块 unknown | 模块内 lambda 体调同模块函数 `poly_val` 报 `call to unknown function` | 多文件 pass 2 结束后 `cur_ns` 重置为 NULL，closure 统一生成阶段 plain call 按 bare name 查表，而多文件注册 key 是 `"<ns>.<name>"` | 两处 closure 生成循环（codegen.c / llvm_codegen.c）按所属模块恢复 `cur_ns` |

**已知边界**（探针确认，记录不修）：lambda 捕获不可逃逸（返回捕获闭包编译
后端明确拒绝）；lambda 嵌套时内层捕获外层 lambda 的参数尚不支持
（"captured variable has no codegen slot"）。库内算法均为单层不逃逸。

**库升级**（sci/optim v0.2.0，双轨接口——通用 fn 内核 + 数据驱动薄包装）：

| 库 | 新通用内核 | 薄包装（原变体，签名不变） |
|---|---|---|
| sci.lume | `rk4(f, y0, t0, t1, n)` / `bisect(f, a, b, tol)` / `newton(f, df, x0, tol)` / `integrate(f, a, b, n)` | rk4_linear / rk4_affine / rk4_poly / bisect_poly / newton_poly |
| optim.lume | `golden_min(f, a, b, tol)` / `grad_desc(grad, x0, iters, lr)` | golden_poly_min / quad_min |

薄包装全部走"内部 lambda 捕获不逃逸"路径，三后端逐字节一致；原有黄金值
断言（√2、e、Dottie number、∫x²=1/3 等）全部保持。

## 优先级总览

| 级别 | 项 | 一句话理由 |
|---|---|---|
| P0 | fn 参数间接调用 | 等价性缺口 + 库 API 瘦身一倍 |
| P0 | div/mod 内置 | 静默错值风险最高的一条 |
| P0 | 保留字上下文化 | 纯误伤，修复成本最低 |
| P0 | 诊断增强 | 全部后续开发受益 |
| P1 | 返回类型推断（P1-1 已落地）/ 字面量元素类型通用推断（P1-2 已落地）/ 块作用域（P1-3 待办） | 高频摩擦逐个消除（RNG/P1-4、位运算/P1-5、函数返回值推断/P1-1、字面量元素推断/P1-2 已落地） |
| P1 | 十六进制/二进制字面量 / 位运算中缀运算符（P1-6/P1-7 已落地） | 位运算落地后暴露的语法糖缺口，零语义风险 |
| P2 | 可变数组 / 元组 | 性能与人体工学，等库规模再验证 |
