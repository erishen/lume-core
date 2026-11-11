# Lume 用户指南

本树(`lume-core`)是 **语言本体**:一份 `.lume` 脚本 + 一个编译器
(`bin/lume-core`)。这里**没有前端构建、没有 Agent 运行时**——
宿主树 `lume` 才有那套东西;语言内建一个极简 HTTP 服务能力
(`server{}` + `run()`·路由与 SSE 流式),但不是 Web 框架。

- 想知道"这两个仓库分别是什么":见 [README.md](../README.md) 开头的对照表。
- 写业务/写脚本之前:先读 [PITFALLS.md](PITFALLS.md) 与 [STYLE.md](STYLE.md)。
- 想看 C 侧怎么实现的、IR 长什么样:见 [ARCHITECTURE.md](ARCHITECTURE.md)、
  [DEVELOPMENT.md](DEVELOPMENT.md)、[NATIVE.md](NATIVE.md)。

---

## 快速开始

```bash
make                  # 编出 bin/lume-core(只需要 cc;libLLVM 只在 llvm-config 存在时才用)
./bin/lume-core examples/hello.lume         # 跑一个脚本
./bin/lume-core --check examples/hello.lume # 只做解析 + 类型检查
make test             # 全套:parity + 单测 + 加密 + 两个原生后端 + 一致性
make asan             # 用 ASan/UBSan 重编并跑单测
```

自带的示例:

| 文件 | 看什么 |
| --- | --- |
| `examples/hello.lume` | 最小脚本:函数、`print`、返回值 |
| `examples/lang-basics.lume` | 语言全貌:类型、struct、map、模块、Result 等 |
| `examples/native-fact.lume` | 两个原生后端共用的基线小程序 |
| `examples/native-bench.lume` | 手写 IR 文本 vs libLLVM 的性能对照 |
| `examples/http-get.lume` | 出站 HTTP:SSRF 闸门、走代理、超时与上限 |
| `examples/modules/` | `import` / `export` 多文件模块 |

`make check` 会对上面除 `modules/` 之外的示例逐个跑 `--check`。

---

## 语言速览

这一节只给坐标,细节看 `examples/lang-basics.lume`(能跑)与 [PITFALLS.md](PITFALLS.md)
(踩过的坑)。

- **语句/表达式**:`let`、`func`、`if/else`、`for/while`、`return`、struct、
  map/列表字面量、`Result`(`ok`/`err`)。
- **类型标注是可选的**。参数、返回值、变量不写类型就是"松散"(内部是
  `TY_ANY`);反过来**,`any` 不是类型关键字**——写 `func f(r: any)` 会报
  `unknown type 'any'`。想表达"任意值",就别写标注。
- **模块**:`import "lib.lume" as lib;`,`export let/func` 才会被外部看到。
  见 `examples/modules/`。
- **运行与退出**:没有 `main()` 时脚本跑完就结束;有 `main()` 返回整数,
  那个整数就是进程退出码。纯脚本跑完会打一行
  `lume: note: script completed without run()`,不是报错。

---

## 内建函数

内建函数可以直接当函数调用,**不需要声明、也不能和别人重名声明**(会当成重复
定义)。参数全都是松散类型,所以调用点一般都不写类型标注。

> 本节只给名字与最小示例。**每个内建的参数序、返回类型、静默失败等边界以
> [SPEC.md §5](SPEC.md#6-内建函数) 为准** —— 那里逐条列了行为并配了可执行断言。

### 输出与转换

`print`(打印一行)、`str` / `int` / `float` / `bool` / `string`(转字符串)、
`len`、`keys`、`get`(map/列表取值;带默认值时更安全)、`replace`(字符串替换)、
`range`(生成区间)、`json`(序列化)、`stringify`、`now`(时间戳)、
`strftime`(时间格式化)。

### 集合

`map` / `filter` / `reduce`(高阶函数)、`put`(写 map 键)、`push`(列表追加)。

### 文件系统

`files`(列目录)、`read_file`、`write_file`、`mkdir`、`lock_file` /
`unlock_file`(跨进程文件锁)。

### 环境

`env`(取环境变量)、`crypt_sha512`(字符串摘要)。

### 数学

`abs` `sqrt` `exp` `log` `ln` `pow` `floor` `ceil` `round` `min` `max`
(另有常量 `pi` `e`)。

### 页面小工具

`el` / `render` / `html`(极简虚拟 DOM 到字符串的拼接)。

### 出站 HTTP —— `http_get`

脚本除能用内建 `server{}` 块起本地 HTTP 服务(路由 + SSE,见 tests/sse-stream.lume)外,
也常需要**主动发起** HTTP/HTTPS 请求:

```lume
let r = http_get("https://api.github.com/zen", {
  timeout: 30,                        // 秒;默认 10,上限 60
  max_bytes: 1048576,                 // body 上限;默认 1 MiB,超出截断并留在 err
  headers: { "Accept": "text/plain" }, // 可选,附加请求头
  allow_private: false,               // 默认 false:放行私有地址(自己搭网关时用)
});
// r = { ok: bool, status: int, body: string, err: string }
print(str(get(r, "status", 0)) + " " + get(r, "body", ""));
```

要点:

- **SSRF 闸门默认开着**。回环(`127.0.0.1`)、`localhost`、链路本地
  (`169.254.0.0/16` 云元数据)、私网(`10/8`、`172.16/12`、`192.168/16`)、
  CGNAT(`100.64/10`)等一律在**开 socket 之前**拒绝;故意用
  `http://ok@127.0.0.1/` 在 userinfo 里藏真实目标,一样拒。
  域名是解析后**逐个 IP 判**的,Round-Robin DNS 换不出第二个机会。
- **代理**:自动跟随 `LUME_HTTP_PROXY` → `https_proxy`/`http_proxy` →
  `HTTPS_PROXY`/`HTTP_PROXY`;`https://` 目标走 `CONNECT` 隧道后再握手
  (校验证书,不支持自定义 CA)。
- **重定向**最多跟 5 跳,每跳都重新过一遍闸门。
- 恒定发 `Accept-Encoding: identity`,不做解压;想要 gzip 自己接。
- 超时与 body 上限都会体现在 `err` 里,`ok` 保持 `false`。
- **总闸**:`--no-net` 或 `LUME_NO_NET=1`(非 `0/off/false`)时,任何
  HTTP 内建直接报错,一个包都不会发出去。

#### HTTP 谓词

```lume
let r = http_get(url,    { headers?, timeout?, max_bytes?, allow_private? });
let r = http_delete(url, { ...同上 });
let r = http_post(url,   { headers?, body?, timeout?, max_bytes?, allow_private? });
let r = http_put(url,    ... 同 post,带 body);
let r = http_patch(url,  ... 同 post,带 body);
```

五个谓词共用一条实现,除方法名之外行为一致,返回的都是
`{ok, status, body, err}`:

- `body` 只被 `post` / `put` / `patch` 读,`get` / `delete` 传了也当没传。
  单个 body 上限 1 MiB(`HTTP_MAX_BODY_BYTES`),超了直接报错,不截断。
- 带 body 的方法**自动补 `Content-Length`**;如果你在 `headers` 里自己给了
  `Content-Length`,就只发你那一个 —— 报文里出现两个同名头是请求走私的
  经典诱饵。
- **重定向的语义差异**:`301` / `302` / `303` 会把带 body 的方法降级成
  `GET` 并丢掉 `Content-Length`(RFC 7231 §6.4.4);`307` / `308` 原样保留
  方法与 body(§6.4.7)。降级那条路最容易漏的就是 `Content-Length`,漏了
  服务端会守着一个再也不来的 body 读到超时。

完整可跑样例见 `examples/http-get.lume`。

### 工具台(空壳)

`tools` / `skills` / `mcps` / `catalog` 在这棵树里返回空列表——它们原本要接
宿主的 Agent 工具表,本树没有那层。

---

## 纯 .lume 算法库

`lib/` 下的算法库**全部用 lume 自身写成**，没有为它们加一行 C 内建
（只复用已有的 `erf` / `igamma` / `norminv` / `sin` / `exp` …）。同一份
AST 由解释器、手写 IR 文本后端、libLLVM 后端执行，浮点运算顺序由 lume
显式控制，所以三后端天然一致；每个库都配一份
`tests/<名>-consistency.lume` 数值回归和 `tests/<名>_backends.sh`
逐字节比对脚本。

`import` 路径相对于**引用文件所在目录**；例如 `examples/` 下的脚本这样用：

```lume
import "../lib/sci.lume" as sci;
let y = sci.rk4((t: float, v: float) => -v, 1.0, 0.0, 1.0, 100);
print(y[100]);                            // 0.367879
print(sci.SCI_VERSION);                   // 0.7.0
```

| 库 | 版本 | 覆盖 | 回归脚本 |
| --- | --- | --- | --- |
| `sci.lume` | 0.7.0 | 信息论、数值积分/插值、多项式、求根；ODE 单步（RK4 / 自适应 / 向量系统 / 隐式 θ / 辛 Verlet）、线性多步（AB1–4 / BDF2）、事件检测、稠密输出 | `sci_backends.sh`、`ode_backends.sh` |
| `linalg.lume` | 0.1.0 | 矩阵运算、LU / QR / 特征值 / SVD 分解、线性方程组 | `linalg_backends.sh` |
| `optim.lume` | 0.2.0 | 梯度下降 / 共轭梯度 / 线性规划单纯形 / 黄金分割 / 非线性最小二乘 | `optim_backends.sh` |
| `pde.lume` | v0.8.0 | 抛物型（1D/2D 热方程、CN、ADI）、双曲型（1D/2D 波动、线性对流、Burgers）、对流扩散（2D 迎风 + 中心扩散）、椭圆型（Poisson 的 Jacobi → SOR → 几何多重网格、Robin 两点边值）；Dirichlet / Neumann / 周期 / Robin 四类边界 | `pde_backends.sh` |
| `dist.lume` | v0.1.0 | 8 种连续分布的 PDF / CDF / PPF | `dist_backends.sh` |
| `complex.lume` | v0.1.0 | 复数四则、模/辐角、指数/对数/幂/三角（`[Re, Im]` 表示） | `complex_backends.sh` |
| `sig.lume` | 0.1.0 | 卷积、FIR、滑动平均、DFT / IDFT、基 2 FFT（`dft`/`idft` 复用 `complex.cmul`） | `sig_backends.sh` |
| `graph.lume` | 0.1.0 | BFS / DFS、最短路、最小生成树 | `graph_backends.sh` |
| `ml.lume` | v0.4.0 | AI / 机器学习：向量化激活（sigmoid/tanh/gelu/swish/leaky_relu/softplus）、损失（mse/mae/bce/cce）、正规方程线性回归与岭回归、PCA（基于 `linalg.mat_svd`）、单头 / 多头注意力、表格 Q-learning、GMM-EM；**深度学习构件（v0.3.0）**：L1/L2 范数、层归一化、批归一化（训练/推理）、Dropout、Conv1d/Conv2d、RNN/GRU 单元及序列前向；**优化器与反向（v0.4.0）**：vanilla SGD / SGD+momentum / Adam、embedding 查表与稀疏梯度、softmax 交叉熵损失与梯度，并配 `examples/dl_train_demo.lume` 纯 .lume 训练循环；以及 kNN / k-means / 逻辑回归 / 感知机 / 朴素贝叶斯 / MLP 反向传播 | `ml_backends.sh` |
| `pinn.lume` | v0.3.0 | 物理信息神经网络（PINN）：前向模式自动微分（Taylor 二阶 dual numbers）**对网络输入求导**（∂u/∂x_j·∂²u/∂x_j²·Jacobian）、热方程 PDE 残差（解析导数）；**训练闭环（v0.2.0）**：PINN 损失（内部残差 + 初始 + 边界）、Adam 训练；**反向传播（v0.3.0）**：对权重解析梯度（伴随 dual 反扫·无磁带分配），梯度步实测 ~8× 提速（demo 全程 10s→1.3s 编译 / 4.5min→19s 解释器），解析 vs 数值梯度互证 ~1e-11、Python 独立复算逐位吻合；`pinn_grad_fd`（中心差分）保留作验证 oracle；配 `examples/pinn_demo.lume`（AD vs 中心差分自检·机器精度一致）与 `examples/pinn_train_demo.lume`（训练闭环：PINN vs 精确解 vs `pde.lume` 有限差分三方对照） | `pinn_backends.sh` |

> `approval.lume` / `domains.lume` / `identity.lume` / `peer.lume` 是宿主
> 业务侧库，不属于科学计算算法层，故不列。
>
> 本机工具链只带运行期 `libLLVM-23.dll`，缺 `llvm-config`、`llvm-c` 头文件
> 与导入库，故 `HAVE_LIBLLVM=0`，`*_backends.sh` 会打印
> `ok <name>: interp/text identical (llvm skipped)`——即只验证了解释器与
> 文本 IR 两腿，不表示 LLVM 腿已验证。装好完整 LLVM 开发包后以
> `make LLVM_CONFIG=<llvm-config 路径>` 重建即可启用第三腿。

实现时反复踩到的 lume 语义边界（写新库前建议先读）：

- `let` 是函数作用域：同一函数内各循环变量必须唯一；`for-init` 不可遮蔽
  外层局部绑定。内置名（如 `ic` / `pi`）**现在可以**作循环变量（P1-3 修复：
  内置符号独立成最外层 scope，经典 for 的 init 绑定三后端都是循环私有），
  但为避免与后续读内置值混淆，给循环变量加前缀仍是好习惯。
- 数组/矩阵**不能原地索引赋值**，值语义下要用 `push` 整体重建。
- 从 `[]` 循环 `push` 构造的数组、以及绑定本地函数返回值的 `let`，在文本
  后端常被推成 `?[]` 或推不出类型，需显式标注 `let out: float[] = [];`。
- `/` 恒为浮点除法（`17/2 = 8.5`），整数尺寸链要用 `* 1.0` 或循环计数。

---

## 命令行开关

```
bin/lume-core [选项] <脚本.lume>
  --check          只解析 + 类型检查,不执行
  --dump           打印 AST
  --compile        编译到原生二进制(默认;走 libLLVM)
  --compile-llvm   强制走 libLLVM 后端
  --compile-text   强制走手写 IR 文本后端(不依赖 libLLVM)
  --no-fs          禁用文件读写
  --no-net         禁用所有 http_* 内建(等价于 LUME_NO_NET=1)
  --no-pass        跳过优化 pass(有些 LLVM 版本在这步会崩)
```

没装 `llvm-config` 时 `HAVE_LLVM` 关着,`make` 只产出解释器那条路
(细节见 [NATIVE.md](NATIVE.md))。

---

## 测试

```bash
make test    # parity 检查 + 解释器单测 + 加密单测 + 两个原生后端 + 一致性
make asan    # ASan/UBSan 版单测(LSan 检测泄漏,目标是 SUMMARY: 0 byte)
make check   # 对自带示例逐个 --check
```

单测在 `tests/smoke.c` 与 `tests/test-crypt.lume`。http 用例(SSRF 闸门、
`--no-net` 总闸)是**离线可判定**的,CI 无网也能跑。

> 🔴 `make test` 依赖 `tests/smoke-bin`,而这个二进制在 agent-httpd 摘除那一步
> **漏了构建规则**:`test:` 里引用它、却从来没人编译它,于是单测一直在跑磁盘上
> 上一次留下的旧二进制。规则已补(`CORE_OBJS` + `build/tests`)。以后改
> `tests/smoke.c` 先 `make tests/smoke-bin` 再跑,别拿旧二进制的结果当数。

---

## 不承诺什么

这一节写的是**边界**, 不是缺陷清单。配合 [README.md](../README.md) 开头的
两树对照表一起读。

- **宿主专属内建一概没有。** 本树摘掉了 `bridge.c` / `iquest.*` /
  `builtins_sql.c`, 换成 78 行的 `bridge_stub.c` —— 它的 `bridge_run()` 直接
  `exit(2)`。宿主的 VS Code 语法扩展会把 `sql_query` / `sql_schema` /
  `sql_tables` / `sql_write` / `recall` / `remember` / `skill-run` /
  `discovery_endpoints` 那一组高亮成内建函数, 那些是 host-only。**本树的可用内
  建以本文档上面的清单为唯一准绳**, 清单之外的一律不存在 —— 不是「有但行为不
  同」, 是根本没有。写脚本前先对着清单确认名字。
  (顺带: 拿一个不存在的名字调用时, 报错并不是 "unknown function", 而是 parser
  层的 "expected ;, got identifier" —— 位置会跳到下一行, 别照着行号找。)
- **标准库是薄的。** 排序、正则、随机数、JSON 反序列化这一类都没有;`json` 只
  覆盖序列化方向。
- **语言规格刚起步,覆盖面还不全。** [SPEC.md](SPEC.md) 是规范性文档(写「是什么」而非
  「怎么实现」), 已覆盖数值类型、求值顺序、类型系统与成员访问、`Result`/`?`、**控制流与
  闭包**、**全部 55 个内建函数(含参数序/返回类型/静默失败等边界)**、模块、三后端一致性。
  **尚未覆盖**:模块边界的运行时细节、部分内建的错误路径全集、以及任何资源上限承诺。
  规范之外的行为仍以实现为准: 破坏性变更会进 CHANGELOG, 但不预告。
- **别在循环里 `push` 被迭代的那个列表。** 两个原生后端会**无限循环并被 OOM 杀掉**
  (退出码 137), 解释器是对的。见 [SPEC.md §8.1](SPEC.md#81-已知缺口) 第 5 条。
- **闭包只用「无捕获」子集。** `let f = (x) => x * 2` 这类 lambda 三后端都能编译执行，
  也能传给 `map` / `filter` / `reduce`；尾随闭包 `f(args) { |x| body }` 是同一棵树的
  语法糖（SPEC §4.11），边界完全相同。但**自由变量捕获只有解释器支持**（顶层 `let`
  会被提升成模块全局，不算捕获），`try` 同样是解释器独占。边界全集见
  [SPEC.md §4.8](SPEC.md#48-闭包与原生后端)。
- **本文件的内建清单只给名字与用法示例, 语义以 SPEC §5 为准。** 两者冲突时以 SPEC 为准。
- **发布面是新的, 没有版本承诺。** `make pack` 是 2026-10-04 才补上的目标(之前
  没有任何打包通路, 只能手工 `cp`)。没有 tag、没有语义版本 —— 打出来的
  `lume-core-<os>-<arch>.tar.gz` 就是当前 HEAD 的产物, 不代表某个「已发布版本」。

---

## 文档地图

| 文档 | 内容 |
| --- | --- |
| [README.md](../README.md) / `README.zh.md` | 两树对照、Quick start、Make 目标表 |
| 本文件 | 语言用法、内建清单、CLI 开关 |
| [SPEC.md](SPEC.md) | **规范性**语言规格:数值类型、求值顺序、类型系统与成员访问、`Result`/`?`、**控制流与闭包**、**全部 55 个内建**、模块、三后端一致性 |
| [PITFALLS.md](PITFALLS.md) | 实战踩坑(同名覆盖、map 键、`any` 不是类型、超时口径…) |
| [STYLE.md](STYLE.md) | C 代码与 `.lume` 的写法约定 |
| [ARCHITECTURE.md](ARCHITECTURE.md) | 分层、IR、bridge 那层怎么被替掉的 |
| [DEVELOPMENT.md](DEVELOPMENT.md) | 编译、可选依赖探测、加内建的步骤 |
| [NATIVE.md](NATIVE.md) | 双原生后端、parity、基准与各自的坑 |
