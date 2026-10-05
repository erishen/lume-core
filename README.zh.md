# Lume

[English](README.md) | [简体中文](README.zh.md)

> 读音：**lu-mé**（/luˈmeɪ/，两音节，重音在后）。

**这是「脱离宿主」的那棵语言树。** 它是 Lume 的语言部分：前端（词法/语法/类型
检查）+ 两个原生发射器（手写 LLVM IR 文本、libLLVM C API）+ 一棵树遍历解释器。
除了 libc（以及探测到 `llvm-config` 时的 libLLVM）什么也不链——这里没有 HTTP
服务器、没有 agent 运行时、没有 Docker 镜像。

把 Lume **嵌进** agent 宿主（`libagenthttpd.a`、活体 HTTP、聊天、工具派发、原生
SQLite 工具、Docker 镜像）的那棵树是姊妹项目 `work/research/lume`：同一门语言，
不同的部署形态；语言侧改动先落在这棵，宿主树从这边带过去。

| | 本树（`work/research/lume-core`） | 宿主树（`work/research/lume`） |
|---|---|---|
| 定位 | 独立语言 + libLLVM 原生路线 | 语言嵌进 agent-httpd |
| 链接 | libc、libLLVM（可选） | 再加 `libagenthttpd.a` |
| `make test` | 解释器单测、crypt、两条原生后端、双发射器一致性 | 再加 HTTP/e2e 套件（`tests/run_all.sh`） |
| server/demo 目标 | 无 | `make dev` / `hub` / `invest` / `image` 等 |

单二进制 **C11 编译器**：业务逻辑写在 `.lume` 脚本里，`lume --compile a.lume`
经 libLLVM 直接产出原生可执行文件。有意保留两个发射器并互相校验（见
[docs/NATIVE.md](docs/NATIVE.md)）；`--no-pass` 用来跳过优化流水线，直看某条
发射器下探出来的 IR。

> ℹ️ 与 [lumeland/lume](https://github.com/lumeland/lume)（Deno 静态站点生成器）
> 及 [lume/lume](https://github.com/lume/lume)（CSS3D/WebGL UI 工具包）**无关联**
> ——只是恰好同名的不同项目。

## ⚠️ 安全

**本树不带服务、不监听端口、没有 agent 运行时** —— 它是编译器和命令行工具。
下面那些关于 HTTP 端口的注意事项描述的是**宿主树**（`work/research/lume`）
的运行面，只有你跑那棵树时才需要读。

本树的攻击面就是 `lume` 这个二进制本身，规则也就是本地编译器的常规规则：

- `--compile` / `--compile-llvm` 会外面调 `cc` 或 libLLVM；不可信的源码以你的
  权限执行，别编译（也别 `run()`）你不信任的脚本。
- `fs` 内建按脚本所在目录解析相对路径，请保证脚本放在你可控的目录里。
- DSL 里的 `run()` 语句在本树直接 `exit(2)`——脚本要求起宿主服务器，而这棵
  构建里没有。

## 快速开始

```bash
make             # 构建 bin/lume-core（只要 cc；libLLVM 仅在 llvm-config 存在时才编）
make check       # 对内置示例做类型检查，不产出
make test        # parity + 单测 + crypt + 两条原生后端 + 一致性
make treecheck   # 同一套检查加一道闸：仓库里跟踪的 blob 不含构建机绝对路径
make asan        # 用 ASan/UBSan 重编并跑单测
make dump        # 打印 examples/hello.lume 的 IR 文本后端产物
make native      # 默认（libLLVM）后端跑 native-bench
make native-text # 手写 IR 文本后端跑同一份靶子
make native-bench # 两条后端同脚本对比
make clean
```

解释执行，或者编出来直接跑：

```bash
./bin/lume-core examples/hello.lume                  # 解释器
./bin/lume-core --compile examples/native-fact.lume  # -> native-fact（libLLVM）
./bin/lume-core --compile-text examples/native-fact.lume   # 手写 IR 文本
./bin/lume-core --no-pass --compile examples/native-fact.lume  # 跳过 -O
```

依赖：一个 C11 编译器（`cc`）；走 libLLVM 那条路还需要 `PATH` 里有
`llvm-config`（此时按 `llvm-config --libs` 链接；探不到照样出 `bin/lume-core`，
只是 `--compile-llvm` 不可用）。没有别的——不需要 `node`、不需要 `pnpm`、
不需要服务运行时、不需要 SQLite。

## 安装

没有要装的东西——不需要 npm registry、不需要包管理器、这棵树也没有发布
tarball。拉下来编就好：

```bash
git clone <本仓库> lume-core && cd lume-core
make                                  # -> bin/lume-core（约 270 KB，其中大半是 LLVM 胶水）
make check                            # 自检：内置示例都能过类型检查
sudo cp bin/lume-core /usr/local/bin/lume-core  # 可选
```

上面那套 `LUME_VERSION` / `LUME_PREFIX` / `LUME_SHA256` 覆盖项，以及那个
`install.sh` 一行安装、React SSR 演示，全都是**宿主树**的发布流程；本树只产出
编译器，所以 `make && cp bin/lume-core` 就是安装动作本身。

构建时可覆盖的变量：

- `CC=` —— 换编译器
- `LLVM_PREFIX=` / `LLVM_CONFIG=` —— `llvm-config` 不在 `PATH` 时指定它
- `CFLAGS=` / `LDFLAGS=` —— 追加，例如 `CFLAGS=-O2` 或额外头文件路径

## 目录

| 路径 | 内容 |
|---|---|
| `src/` | 词法/语法/类型检查/树遍历解释器 + **本树自持的桥接替身**（`bridge_stub.c`）+ 原生后端（IR 文本走 `codegen.c` 及拆分出去的 `codegen_{types,expr,scan,sig,stmt}.c` / `irbuf.c` / `backend.c`，可选的 libLLVM 路走 `llvm_codegen.c` / `backend_llvm.c`），45 个文件、约 15k 行 C11；另有 fork-local 的 `sbuf.h` 替掉宿主的 minijson 字符串缓冲 |
| `examples/` | 纯语言脚本：`hello`、`lang-basics`、`modules/app`、`native-fact`、`native-bench`、`http-get` |
| `tests/` | C 单测（`smoke.c`，113 项）+ `test-crypt.lume` + `native_backends.sh`（解释/文本/libLLVM 三路比对）+ 一致性期望文件 |
| `scripts/` | `check-backend-parity.sh` —— 两条发射器的 AST 标签覆盖 parity 检查 |
| `docs/` | 完整文档，见下 |

> 宿主树另有 `frontend/`（React 客户端）、`www/`（docroot）、`docker/`
> （镜像 + compose）、`editor/lume-vscode/` 以及一堆 server/demo 的 `.lume`
> 脚本——本树按设计全都没有。

### 本树是上游，宿主是下游

同仓兄弟目录 `work/research/lume` 是**服务器产品**。它把本树以 **vendor+pin**
的方式持有在 `lang/`：`lang/PIN` 记着上游 `sha` + 宿主叠在其上的文件清单
（`host_owned`）；那边 `make sync-lang` 推进 pin、`make check-sync` 报告漂移。
因此：

- 属于**语言本体**的改动应该落在这里，宿主靠 bump pin 拿到，**不是手抄**；
- 宿主自持的文件（`interp.c`、`main.c`、`typecheck.*`、`lume.h`、`builtins*` …
  注意它有自己的 `builtins.c` 和 `builtins_http.c`）**故意分叉**，
  `sync` 永不覆盖；
- 宿主后来从本树 port 了 `builtins_http.c`，`LUME_HAS_HTTP` 默认 1（Makefile 里显式 `-D`），
  注册挂在本树 `interp.c` 的 `#if` 上（见 `docs/ARCHITECTURE.md` 5.1）。

> ⚠️ **英文 README 里有四节（`## Containers`、`## Security notes`、
> `## SQLite support (native)`、`## Text2SQL`）写的是宿主 `lume` 树的
> 东西，本树一个都不实现。** 本 README 不收录它们——本树要部署的就是一个
> 静态语言工具 `bin/lume-core`，安全边界只有 `--no-fs` / `--no-net` /
> SSRF 闸门这三样。

### 发布打包

```sh
make pack        # 用 pack CFLAGS 重编 + strip + 打 dist/.pack -> lume-core-<os>-<arch>.tar.gz
make packcheck   # 在成品 tarball 里扫构建机绝对路径；扫到就非 0 退出
```

tarball 曾在两处漏出 `/Users/…`：`-g` 留下的 DWARF，以及 `LUME_RT_SRC` 被
`backend.o` 当字符串字面量烤进去（`strip` 去不掉，只有用包内相对路径重编才行）。
`packcheck` 就是这道闸，CI 在 `pack` 之后跑它，回归会失败而不是被发出去。

## 文档

- [docs/LUME.md](docs/LUME.md) —— **用户指南**：语言速览（类型/控制流/Result）、
  内建函数、路由与工具注册、`el()`/`html()` 两套页面写法、模块序列化细节、
  多文件 `import` / `export`。
- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) —— **开发文档**：目录结构、
  解释器核心约定（改代码前必读）、怎么加内建函数/新语句/新类型、测试约定
  （本树 `make test` 没有 HTTP/e2e 那条腿）、渲染类内建、已知约定与坑。
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) —— **架构文档**：系统全景与
  设计决策、进程/内存模型、分层、编译/执行时序、部署形态与安全边界。
- [docs/STYLE.md](docs/STYLE.md) —— **写法指南**：用当前能力集怎么写 `.lume`
  （箭头表达式体、`try` 固定键、`"` 引号 map 字面量、`spa`、写路径纪律），附
  提交前自查清单。
- [docs/PITFALLS.md](docs/PITFALLS.md) —— **坑位台账**：真实踩过的坑与修复
  状态，写业务 `.lume` 前先读。
- [docs/NATIVE.md](docs/NATIVE.md) —— **原生后端（实验，`--compile` 默认走
  libLLVM，`--compile-text` 退到手写 IR 文本）**：两条路把 Lume 编成可执行文件
  （libLLVM C API vs 手写 IR 文本）、为什么 `print` 不调 `printf`、覆盖到哪些
  内建、哪些还没覆盖，以及两条路的横向实测对比。

## 原生后端（实验）

`--compile` 把 `.lume` 脚本编成一个真正的可执行文件。两条后端共用同一套前端
（词法 → 语法 → 类型检查）：

- **默认（`--compile`，等价于 `--compile-llvm`）**：类型检查后的 AST 交给
  `src/llvm_codegen.c`，用 **libLLVM C API** 把 IR 建出来 —— 即 IR 是 LLVM
  **value**，LLVM 当场（`LLVMVerifyModule`）校验形状，目标文件也由
  `LLVMTargetMachineEmitToFile` 自己出。IR 长错就错在刚生成的地方，不是拖到
  clang 那步才炸。
- **`--compile-text`**：交给 `src/codegen*.c`（按区段拆成几个文件：类型拼写 /
  表达式 / 扫描 / 签名推断 / 语句，见 `src/codegen.c` 头部说明）产出 **LLVM IR
  文本**，clang 收尾。

```bash
make native                     # 默认后端: 编译 + 跑 + 与 tests/native-fact.expected 比对
make native-text                # 同一份期望基线, 强制走文本路
make native-llvm                # 同一份期望基线, 强制走 libLLVM
bin/lume-core --compile examples/native-fact.lume && ./out/native-fact
bin/lume-core --compile script.lume -o mybin     # IR 落在 out/mybin.ll
```

目前覆盖 core subset：标量、结构体、函数、`if`/`while`/`for`、算术与比较，
以及一批内建（`print`/`abs`/`min`/`max`/`sqrt`/`pow`/`floor`/`ceil`/`round`/
`str`/`int`/`float`，以及字符串 `+`）。`server {}`/`route`/`tool` 的注册在本树
会进 VM 自带的表，但 `run()` 直接停在本树没有宿主的信号上；原生路径覆盖的是
语言核心，宿主相关的语句不在范围内。设计与取舍见
[NATIVE.md](docs/NATIVE.md)。

`llvm-config` 找得到就编译进 `bin/lume-core`（Makefile 自动探测，否则这个二进制
根本没有 libLLVM 那条路，`--compile` 自动落到文本后端并在 stderr 说明）。

已知代价：**优化 pass 是 2026-10-03 才接上的**。此前一直记着「`LLVMRunPasses`
从纯 C TU 里调会崩」，于是产物正确但没 mem2reg，循环密集的靶子上比
`--compile-text` 慢一到两个数量级。重测发现那个结论前提错了两次（入口头文件
`llvm-c/Transforms/PassBuilder.h` 本来就已经 include 进来，崩是因为
`LLVMRunPasses` 被隐式声明成返回 `int` 再塞进 `LLVMErrorRef` 这个指针；
真正会死的是 opt level `None` 而不是 `Default`）。现在 libLLVM 这条路在 emit
前跑 `default<O2>`，和文本路的 `clang -O2` 同一档：

```
backend   compile-ms   ir-KiB  obj-KiB  bin-KiB    run-ms  nopass-ms
text            446        3        1       35         9          -
llvm            184        2        1       35         9        215
```

正确性与各体积列两边打平，运行侧落在同一档（1e8 次迭代里的局部变量被 promote
成寄存器，不再是内存槽），**编译侧 libLLVM 更快**（它的 IR 不用再被 clang 解析
和 codegen 一遍）。末列 `nopass-ms` 是哨兵：`lume --no-pass`（等价的环境变量
`LUME_NO_PASS=1`）跳过优化 pipeline 再编一次同一份源码，llvm 腿会从个位数掉到
200ms 上下 —— 想看实测就跑
`make native-bench`（双层循环靶子 + 无 pass 对照）。细节见
[NATIVE.md](docs/NATIVE.md)。

## 容器

> ⚠️ **本节描述的是宿主 `lume` 树，不是本树。** 上游这两棵树是分叉的：本树没有
> `docker/` 目录、没有 `Dockerfile`，也没有 `make invest` / `make demo-sqlite`
> （见 `docs/ARCHITECTURE.md` §5.4「container deployment: not for this tree」）。
> 容器、compose 与 k8s 都属于宿主那棵 `lume/` 树；本树只交付一个静态工具
> `bin/lume-core`。

```bash
cd lume && docker compose -f docker/docker-compose.yml up -d --build
```

一个镜像 `lume:latest`；compose 里的 `invest`（宿主 `127.0.0.1:18082`）和
`hub`（`127.0.0.1:18083`）两个服务各自只换 `examples/<name>.lume` + 端口 +
白名单环境变量，共用一份 docroot。构建上下文是 `lume/` 仓库根（agent-httpd 是
仓内子模块），所以 `cd lume && docker build -f docker/Dockerfile` 可用 ——
Lume 静态链 agent-httpd 的 `libagenthttpd.a`，而宿主编出来的归档是 Mach-O，
必须在 Linux 容器里重编。

镜像里不带 `.env`（由 compose 的 `env_file` 注入）也不带 `.data/` 会话；
`skills/router/` 那份同步副本**会**进镜像 —— 它是 `llm-router` 启动时写的兜底
缓存，让容器在同步失败时仍留着最后一份技能定义；它不是可复现产物，一直被
git 忽略。

## SQLite 支持（仅宿主树）

> ⚠️ **本节描述的是宿主 `lume` 树，不是本树。** `sql_query` / `sql_write` /
> `sql_tables` / `sql_schema` 这一族由宿主的 agent-httpd 经 `SQLITE_DB` 注册；
> 本树的解释器**没有**这些内建，它唯一的数据能力是 `json()`。

SQLite 直接链进服务器：`agent-httpd` 直接链 libsqlite3
（`agent-httpd/src/agent/sqlite_tool.c` —— 这个文件属于宿主树，**不属于**本树），
只要 `SQLITE_DB` 指向一个库就注册三个原生工具：不落地 Python、不落 MCP stdio
子进程，静态容器镜像也一样能用。

- `sql_query` —— 单条只读 SELECT。库以 `SQLITE_OPEN_READONLY` 打开，即使语句
  绕过了文本检查，写操作与 DDL 在物理层也进不来。护栏沿用旧 MCP 服务器的那一套：
  单语句、去掉注释后只允许 SELECT、prepare 期做语法校验、200 行上限。
- 两个工具都接受可选的 `?` 绑定参数 —— `sql_query(sql[, params])` /
  `sql_write(path, sql[, params])` —— 其中 `params` 是经 `sqlite3_bind_*`
  绑定的标量列表。值永不进入 SQL 文本，所以护栏检查只看到语句骨架，通过参数
  值注入是不可能的（值里的引号、`;`、`--`、DDL 关键字都是惰性的）。凡是不是
  编译期常量的值，都优先走绑定而不是字符串拼。
- `sql_write` —— **可选、默认关**：单条写语句 —— `INSERT` / `UPDATE` /
  `DELETE`（UPDATE/DELETE 必须带 WHERE 条件）或新表的 `CREATE TABLE`。
  `DROP` / `ALTER` / `TRUNCATE` / `VACUUM` / `ATTACH` / `PRAGMA` / `GRANT` /
  `REVOKE`，以及任何提到 `portfolio` 镜像表的语句，一律拒。它编进了服务器但
  **不在默认白名单里**；把 `sql_write` 加进 `HARNESS_TOOLS_ALLOW`
  （Makefile 的 `INVEST_TOOLS`、compose/k8s）才让模型建分析表。账本本身在
  `.data/portfolio.json`，仍是权威。
- `sql_tables` —— 列出表名。
- `sql_schema` —— 把表/列/行数/样本值当提示文本做内省。

- **数据**：带类型的领域工具（`portfolio_add` / `portfolio_remove`）仍是 JSON
  账本的权威写入口。`make invest` 每次启动都会重新 seed 那份 SQLite 镜像
  （`.data/lume.db`，经 `tools/sqlite-migrate.py` 按 symbol upsert）；portfolio
  镜像按构造只读。
- **启用**：`make invest` 设 `SQLITE_DB=.data/lume.db` 并把只读 `sql_*` 工具加进
  白名单。容器里设 `SQLITE_DB`（例如经挂载卷指向 `/app/.data/lume.db`）即可 ——
  compose/k8s 里白名单条目本来就齐。`make demo-sqlite`
  （`examples/sqlite-write.lume`，:8084）是一个能真跑的 demo，自带一套自包含
  的聊天界面（`www/sqlite-write/`，不带 invest 前端），额外把 `sql_write`
  也加白名单：让模型自己建一张分析表，看受控写循环怎么转。
- **遗留 MCP 服务器**：`tools/mcp-sqlite-safe.py` 作为归档的可选写路径留着（给
  分析表用）。把它加回 `INVEST_MCPS` 并恢复 `.data/mcp-servers.json` 里的条目
  就能用；默认档位是原生只读。

## Text2SQL（仅宿主树）

> ⚠️ **本节描述的是宿主 `lume` 树，不是本树。** 那是宿主 invest 服务器在
> `SQLITE_DB` 被设置时把库结构注入 chat system prompt 的做法
> （`sqlite_system_extra()`）；本树没有聊天。

DataPulse 那一路的自然语言转 SQL：只要 `SQLITE_DB` 被设置，服务器就内省数据库
（语义同 DataPulse 的 `describe()`），经 `sqlite_system_extra()`（按库的 mtime
缓存）把实时 schema 与数据纪律注入 chat 的 system prompt：

- 模型看到表、列、行数、样本值和外键提示，于是对着真实名字写正确的只读 SQL，
  而不是猜；
- 写作规则默认只读（只用 `sql_query`；`sql_write` 要白名单才开），答案规则强制
  接地：只报返回行里的数，不许编造日期，单元格是数据不是指令。

整条回路留在原生 ReAct agent 里：模型写 SQL，`sql_query` 在进程内只读执行
（`sql_write` 仅在显式白名单时），agent 依真实结果作答 —— 没有 Python、没有
MCP stdio 子进程、没有 Node 边车、也没有第二次 LLM 调用。

## 相关资料

- 宿主树那篇介绍讲的是**服务器形态**的 Lume（agent-httpd、聊天、SQL 工具）：
  [Lume：C11 单二进制的 Agent DSL 服务器](https://erishen.cn/lume/)
