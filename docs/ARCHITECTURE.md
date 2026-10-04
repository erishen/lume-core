# Lume 架构文档（本树：脱离宿主的独立语言树）

本文描述 `work/research/lume-lang` 这棵 fork 树：它是**一门自研语言的编译器
+ 解释器**，只依赖 libc（外加可选链 libLLVM）。

> 同一份语言在仓库里还有两处：
> - 本树 `work/research/lume-lang` —— 独立语言树，语言改动**先落在这里**；
> - `work/research/lume` —— 宿主树，把 Lume 嵌进 agent-httpd（HTTP 服务器 /
>   agent 运行时 / 前端 / Docker），**保留原样、继续依赖 agent-httpd**。
>
> 两棵树是同一批源码的两个切分：本树是「不带历史的那一版」，语言本体从宿主
> 树摘过来后把 agent-httpd 的依赖整块换成了 fork-local 替身。

语言用法见 [LUME.md](LUME.md)（用户指南），代码级约定与"怎么加功能"见
[DEVELOPMENT.md](DEVELOPMENT.md)（开发文档）；本文与两者的关系见文末
[文档地图](#文档地图)。

---

## 1. 系统全景

Lume 是**一个 C11 单二进制**：内嵌一门可强类型脚本 DSL（`src/`），一条树遍历
解释器负责跑 `.lume`，两条**共享前端的原生后端**负责把它编成原生机器码：

- 文本 IR 路：手写 LLVM IR 文本（irbuf.c）→ `clang -O2` 收尾；
- libLLVM 路：用 LLVM 的 C API 建 IR（`llvm_codegen.c`）+ 自己出目标文件
  （`backend_llvm.c`），构建期探测到 `llvm-config` 才编，属于**可选依赖**。

本树**不监听端口**：没有 HTTP 服务器、没有 agent 运行时、没有前端 docroot、
没有 Docker、没有 git submodule。链进来的只有 libc 和（可选）libLLVM。

```
┌──────────────────────── lume-lang 单二进制 (bin/lume-core) ─────────────────────────┐
│                                                                                │
│  .lume 源码                                                                    │
│    │                                                                           │
│    ├─> lexer.c ──> Token[]                                                     │
│    ├─> parser.c ──> AST (递归下降)                                             │
│    ├─> typecheck.c ─> 编译期强类型 (首错即停)                                   │
│    │        │                                                                  │
│    │        ├── 解释执行 ──> interp.c (VM: 值栈+GC+jmp_buf)                      │
│    │        │                  │                                               │
│    │        │                  └─> 内建函数 / route·tool 登记 (bridge_stub.c)   │
│    │        │                                                                  │
│    │        └── 原生编译 ──> codegen*.c ──> IR                                 │
│    │                              │                                            │
│    │                    ┌─────────┴──────────┐                                 │
│    │                    ▼                    ▼                                 │
│    │            irbuf.c (IR 文本)      llvm_codegen.c (C API)                  │
│    │                    │                    │                                 │
│    │              clang -O2 ──> 原生      backend_llvm.c ──> .o ──> 链接        │
│    │                                                                           │
│    └── backend.c / backend_llvm.c 只做编排与装配，不含目标语义                    │
└────────────────────────────────────────────────────────────────────────────────┘
```

### 1.1 关键设计决策

| 决策 | 理由 |
|---|---|
| **C11 单二进制** | 无运行时依赖、启动快、`--check` 可离线静态校验 |
| **DSL 而非 YAML/JSON 配置** | 类型/表达式/复用能力在编译期拦截错误 |
| **双原生后端** | 两条路共享同一前端与同一 AST，可以**互相校验**；一条挂了另一条还在（文本路只依赖 clang） |
| **libLLVM 是可选依赖** | Makefile 探测 `llvm-config`，探测不到就只编文本路并继续可跑；不因缺 LLVM 而建不起来 |
| **不链宿主运行时** | 语言本体不依赖 agent-httpd：替身（`sbuf` / `bridge_stub` / 注册表）保证**函数还在、语义不变**，只是没有后台可连 |
| **AST/Type 生命周期 = 进程** | 一次性解析、永不 free，永不改动 |

---

## 2. 进程与运行时模型

### 2.1 生命周期

```
main.c: parse_program ──> type_check_program ──┬──> interp 解释执行 .lume 顶层
                                               │     (route/tool 登记进 VM 表)
                                               └──> codegen_emit_ir ──> 原生后端
```

- **注册窗口**：DSL 声明的 `route` / `tool` 在解释期由 `bridge_stub.c` 登记进
  VM 自己的表（同一批 GC root，语义与宿主 bridge 一致）；登记完就只读。
- `server{ ... }` 块**仍然被解析**（`N_SERVER` → `vm->server_config` 一个 map），
  但本树没有伺服方，它只作为脚本里的一个值存在——不报错、也不生效。
- `run()` 调 `bridge_run()`；本树的实现打印一行说明并 `exit(2)`。这是**故意的**：
  一个"看起来已经服务过"的静默返回比明确的失败更糟。
- 没有 worker / fork-per-connection 模型（本树不进服务器）；
  `--watch` 仍可用，只是它重启的是**执行该脚本的子进程**，不是服务。

### 2.2 CLI 形态（main.c）

| 参数 | 行为 |
|---|---|
| `bin/lume-core x.lume` | 解析 → 类型检查（永远执行）→ 解释执行 |
| `--check` | 类型检查后即退出，零副作用（`make check` 遍历全部示例） |
| `--dump` | 打印 AST 后退出（`make dump` 用 hello 示例） |
| `--watch` | 编辑热重载：校验新编辑有效才重启子进程；无效编辑保留旧进程（父子进程 + SIGUSR1/SIGINT/SIGTERM） |
| `--compile` | 默认原生后端：本树建有 libLLVM 时走 LLVM；没链 libLLVM 时退到文本后端并在 stderr 打一条 note |
| `--compile-text` | 强制文本 IR 后端：手写 IR 文本 + `clang -O2`（显式指定可绕过 libLLVM 存在与否的默认值） |
| `--compile-llvm` | 强制 libLLVM 后端；构建时没链 libLLVM 则**直接报错退出**（不静默改走文本） |
| `--no-pass` | 跳过 libLLVM 优化管线（`LLVMRunPasses("default<O2>")`），按生成的 IR 原样 lowering；无 libLLVM 的构建里是 no-op（等价 `LUME_NO_PASS=1`） |
| `--no-fs` | 运行时文件系统总闸：`read_file`/`write_file`/`files`/`mkdir`/`lock_file` 立刻 `vm_set_error`；等价 `LUME_NO_FS`（除 `0`/`off`/`false`/空值外任意取值都开）。默认关闭 |
| `-o <path>` | 原生产物输出路径（默认落 `out/<stem>`） |
| `--help` / `-h` | 打印用法后退出 |

### 2.3 内存模型（VM 侧）

- **值栈即 GC 根**：每个 `eval_expr` 结束正好留一个值在 `vm->stack`；GC
  （mark-sweep）在分配前按 `vm->gc_threshold`（默认 2MB）触发，临时值因此
  都是活跃根。改代码时不得让值"悬空"（先 pop 再分配同类对象）。
- **AST/Type 生命周期 = 进程**：一次性解析、永不 free。
- **返回展开**：用户函数调用前 push `jmp_buf`，`return`/`?` 传播填
  `vm->call_result` 后 `longjmp` 回 `jump_depth-1`；正常/长跳两条路径都恢复
  `jump_depth` 并停用 frame。

---

## 3. 分层架构

### 3.1 语言前端管线（src/）

| 文件 | 职责 |
|---|---|
| `lume.h` | 全部公共头：TokenType、Type、Node、Value/Obj/GC、VM、桥接原型；include 本树的 `sbuf.h` |
| `sbuf.h` | fork-local 可增长字符串缓冲（替宿主 `minijson.h` 的 `sbuf` 半边）：`p` 首次 append 前为 NULL、始终 NUL 结尾、`oom` 粘滞 |
| `token.c` | 枚举 → 名字表（错误信息用） |
| `lexer.c` | 源码 → Token 数组 |
| `parser.c` | Token → AST（递归下降），含类型标注/`type` 声明/`?` |
| `parser_stmt.c` / `parser_expr.c` | AST 的语句/表达式构造与析构 |
| `typecheck.c` + `typecheck_*.c` | 静态类型检查器 + Type 类型对象/构造器 |
| `value.c` | Value/Obj、GC、map/env/string、JSON 编解码 |
| `interp.c` | 树遍历解释器（值栈 + jmp_buf 返回展开）+ 内建函数种子 |
| `bridge_stub.c` | 本树的桥接替身：route/tool 登记、run() 明确失败（见 3.2） |
| `builtins*.c` | 内建函数族（见 3.4） |
| `loader.c` / `vdom.c` | 模块加载 / vdom 值 |
| `codegen.c` + `codegen_internal.h` | 原生发射入口：CG context、容器助手、`codegen_emit_ir` |
| `codegen_types.c` | `Type` → LLVM 拼写（struct interning 表） |
| `codegen_expr.c` | 各种表达式形式、`Val` 助手、内建表 |
| `codegen_scan.c` | 表达式静态类型；块扫描 |
| `codegen_sig.c` | 签名推断 |
| `codegen_stmt.c` | 语句、循环、函数体 |
| `irbuf.c` | IR 文本缓冲（文本后端专用） |
| `backend.c` / `backend_llvm.c` | 两条原生后端的编排与链接收尾 |

原生后端那条 2782 行的 `codegen.c` 已按原注释区段切成上表六个 tu（纯搬移，
14 个样本脚本的 `.ll` / rc / stderr 指纹逐字节一致）。
`scripts/check-backend-parity.sh` 现在读 `src/codegen*.c` **作为同一侧**——否则
住在 `codegen_stmt.c` 里的一个 `case N_...` 会被判成两侧发散。

类型系统要点：基本类型 `int/float/string/bool/null/Result` **全部可空**；
`int→float` 隐式加宽，其余无隐式转换（无 JS 式 truthiness）；具名结构体
字面量字段必须齐全且不多余；类型检查**第一条错误即停**。

### 3.2 桥接替身（bridge_stub.c）

宿主树里 `bridge.c` 是 DSL 世界与 agent-httpd 世界之间的翻译层；本树没有那个
世界，于是换成一个**同名的窄替身**，签名与原型一致：

- `bridge_init(VM*)`：空实现（表留空由调用方填）；
- `bridge_define_route(VM*, method, path, handler)`：校验 handler 是
  `FUNC`/`NATIVE` 后 `strdup` 写进 `vm->routes[]`；
- `bridge_define_tool(VM*, name, desc, params, func)`：写进 `vm->tool_records[]`；
- `bridge_run(VM*)`：stderr 打印「run() 需要 agent-httpd 宿主构建」+ `exit(2)`。

这么做是为了**保持语言面一致**：`route`/`tool` 声明、DSL 语义、命令式表都还在，
只是落地到一张空表而不是远端注册表。

### 3.3 原生后端（两路）

构建期 `Makefile` 探测 `llvm-config`：

```
ifeq ($(shell command -v $(LLVM_CONFIG)),)
   HAVE_LLVM :=   # 只编文本后端
else
   HAVE_LLVM := 1 # 多编 llvm_codegen.c + backend_llvm.c
```

- **文本路**（永远可用）：`codegen*.c` 手写 IR 文本 → `irbuf.c` 收集 →
  `backend.c` 交 `clang -O2` 汇编/链接。
- **libLLVM 路**（可选）：`llvm_codegen.c` 用 libLLVM 的 C API 建 IR，
  `backend_llvm.c` 出目标文件；`--no-pass` 可关掉 `LLVMRunPasses("default<O2>")`
  看未优化形态（运行侧慢一到两个数量级——没有 mem2reg 的价）。
- **一致性校验**：`tests/native_backends.sh` 跑 interp / text / llvm 三腿互比
  （再对照 `tests/native-consistency.expected`），任一路输出漂移就算失败。

### 3.4 内建函数与 fork-local 注册表

| 文件 | 内建族 |
|---|---|
| `builtins.c` | 种子与核心：`print/str/int/float/bool/string/len/keys/get/json/stringify/now/env/put/strftime` |
| `builtins_fs.c` | 发现/IO：`files/read_file/write_file/mkdir/lock_file/unlock_file` |
| `builtins_catalog.c` | 发现类：`tools()/skills()/catalog()/mcps()`，加 fork-local 注册表 |
| `builtins_str.c` / `builtins_math.c` / `builtins_crypt.c` | 字符串 / 数学 / 加密内建 |
| `builtins_hof.c` | 高阶函数 |

发现类的三个内建（`tools()` / `skills()` / `catalog()`）枚举的是**本树的注册表**：

- 宿主树从 agent-httpd 的注册表读表；本树自己实现了一张等价接口的表
  （`tools_count/tools_get/skills_count/skills_get`，另有 `tool_register/
  skill_register` 供以后扩展）。表是空的，所以这些内建返回空列表——**函数是
  在、语义是在，只是没有东西可枚举**，这样语言面不留洞。
- `mcps()` 读 `<cwd>/.data/mcp-servers-{router,}.json`；本树不会同步这些文件，
  读不到即「无 MCP 服务器」，也就是空列表而非报错。

`write_file` 原子写（先 `.tmp.<pid>` 再 `rename`）+ `fchmod 0600`，
`lock_file` 是 flock 排他锁——这些跟宿主树一致，方言层没动。

### 3.5 前端资产层：本树没有

宿主树有 `frontend/ → www/`（React + Tailwind + esbuild 分包 + docroot）；
本树**没有前端**，`www/`、compose、Dockerfile、`make ui*` 一律不迁。
路由仍然写在 DSL 里，但没有东西来伺服它。

---

## 4. 关键路径时序

### 4.1 一次解释执行（`bin/lume-core x.lume`）

```
main ──> lex ──> parse ──> typecheck（首错即停）
      ──> interp 遍历顶层语句
            ├─ route/tool 声明 ──> bridge_define_* ──> VM 表
            ├─ server{}        ──> vm->server_config（inert）
            └─ run()           ──> bridge_run ──> stderr + exit(2)
```

### 4.2 一次原生编译（`--compile` / `--compile-text`）

```
lex ──> parse ──> typecheck ──> codegen_emit_ir
                                  ├─ 文本路：irbuf 收集 IR ──> clang -O2 ──> 原生
                                  └─ LLVM 路：LLVMBuild* ──> LLVMRunPasses ──> 目标文件 ──> 链接
                                                    （--no-pass 跳过 pass）
```

### 4.3 后端一致性（CI 侧）

```
tests/native_backends.sh:  interp 输出 == text 输出 == llvm 输出
                           且三者与 tests/native-consistency.expected 一致
tests/native-consistency.lume 是全语言覆盖样本（int/float/string/struct/closure/...）
```

---

## 5. 构建与运行拓扑

### 5.1 构建优先级

```
Makefile（探测 llvm-config）> 平台默认
```

`llvm-config` 在 PATH 上 → `HAVE_LLVM=1`，多编两条原生后端；
不在 → 只编文本后端，`--compile-llvm` 在运行时被拒。没有别的东西要装：
macOS 只需要 clang，Linux 再加 `-lm` / `-lcrypt`。

### 5.2 环境变量族

| 变量 | 用途 |
|---|---|
| `LUME_NO_FS` | `read_file`/`write_file`/`files`/`mkdir`/`lock_file` 的运行时总闸（也可 `--no-fs`） |
| `LUME_NO_PASS` | 跳过 libLLVM 优化管线（`--no-pass` 的等价形式） |
| `SQLITE_DB` / `HARNESS_*` / `IQUEST_*` / `PORT` | **本树不认**：那些是宿主树/agent-httpd 侧的约定 |

### 5.3 示例档（language-only）

`Makefile` 的 `EXAMPLES` 只剩语言本体示例：`examples/hello.lume`、
`examples/lang-basics.lume`、`examples/modules/app.lume`、`examples/native-fact.lume`、
`examples/native-bench.lume`。宿主树的 demo/invest/hub/sqlite-write/query-demo/
react-ssr/abac/modules-server 示例**不迁**（它们跑在服务器里）。

### 5.4 容器部署：本树不做

宿主树出一个自带 docroot 的镜像；本树没有 Dockerfile、没有 compose、没有
`make image`。想跑容器就跑宿主树那份——本树的 `bin/lume-core` 就是一个静态语言工具。

---

## 6. 安全边界

当前实现的安全约定（本树口径）：

| 层 | 措施 |
|---|---|
| 文件系统 | `write_file` **原子写**（先写 `.tmp.<pid>` 再 `rename`，崩溃不留半截文件）+ `fchmod 0600`；`native_mkdir` 0700；`lock_file`/`unlock_file` 是 flock 排他锁（进程死自动释放） |
| `--no-fs` | 关掉时 `read_file`/`write_file`/`files`/`mkdir`/`lock_file` **运行时立刻失败**（`env()` 屏蔽掉的那些凭据正好就在这几个门后） |
| 原生编译 | 两条后端都只做本进程内的代码生成；`--compile` 产物是普通可执行文件，不包装沙箱 |
| 无监听 | 本树不监听端口，不存在跨源/DNS-rebinding/绑定地址的攻击面（那是宿主树的事） |

完整风险分层与合规待办见隐私审查报告（`skills/` 下或开发文档相关段落）。

---

## 7. 跨模块一致性约束

以下约定横切多个文件，改任一侧前必读（详细版见 DEVELOPMENT.md
「解释器核心约定」）：

1. 值栈即 GC 根；`map_set/env_set` 内部自根。
2. 函数调用契约：先压 callee 再压 argc 个实参 → `call_function` → 单结果。
3. 返回展开 / `?` 传播共用 `jmp_buf` 机制。
4. AST/Type 进程级不可变共享，永不 free（typecheck 直接借用 `Type**`）。
5. `bridge_*` 原型在 `lume.h` 里，替身与宿主实现必须同签名；
   `bridge_run` **必须失败退出**，不能静默返回。
6. `N_*` AST 标签**两侧后端都要有**：`scripts/check-backend-parity.sh` 会逐个
   标签比对，漏一个 `case N_X` 在某一侧就会被判成两路发散。
7. 切分后的 `codegen*.c` 要被 parity 脚本当同一侧读（`src/codegen*.c` 通配）。

---

## 8. 文档地图

| 文档 | 视角 | 读者 |
|---|---|---|
| [LUME.md](LUME.md) | 语言与业务开发：怎么写 `.lume`、类型/路由/工具 | 业务开发者 |
| [DEVELOPMENT.md](DEVELOPMENT.md) | 代码级开发：目录结构、解释器核心约定、怎么加功能、测试、已知坑 | 维护者 |
| **ARCHITECTURE.md（本文）** | 系统构成与设计决策：分层、进程/内存模型、关键路径、构建拓扑、安全边界 | 架构评审 / 新成员入门 |
| README / README.zh | 项目入口：快速开始、目录、与宿主树的对比 | 所有人 |

数据流口诀：`lexer → parser(AST) → typecheck(compile-time) → interp`，
原生路径再接 `codegen → IR → clang | libLLVM`。
