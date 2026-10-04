# Lume 开发文档（本树：脱离宿主的独立语言树）

Lume 是一门内嵌的可强类型脚本 DSL，宿主语言为 C11，只依赖 libc（外加构建期
探测、`llvm-config` 存在时**可选**链 libLLVM）。本树**没有 HTTP 服务器、没有
agent 运行时、没有前端、没有 Docker、没有 git submodule**。

> 同一份语言还有个宿主树 `work/research/lume`：它把 Lume 嵌进 agent-httpd
> （服务器 / agent 工具 / React 前端 / 容器镜像），动词一律**加服务器**，例如
> `route` 注册的是远端路由、`run()` 真的伺服、`make invest` 起 UI。
> 本树是同一份源码的**不带历史的切分**：语言本体从那边搬过来，agent-httpd
> 的依赖整块换成 fork-local 替身。改语言**先改本树**。

```
lexer -> parser(AST) -> type checker(compile-time)
                          |
                          +-> tree-walk interpreter (解释执行 .lume)
                          +-> codegen* -> IR -> (clang | libLLVM) -> 原生二进制
```

---

## 目录结构

```
lume-core/
├── src/
│   ├── lume.h               # 全部公共头：tokens、Type、Node、Value/Obj/GC、VM、桥接原型
│   ├── sbuf.h               # fork-local 可增长字符串缓冲（替宿主 minijson 的 sbuf 半边）
│   ├── token.c              # 枚举 -> 名字表（错误信息用）
│   ├── lexer.c              # 源码 -> Token 数组
│   ├── parser.c             # Token -> AST（递归下降），含类型标注/`type` 声明/`?`
│   ├── parser_stmt.c / parser_expr.c
│   ├── typecheck.c + typecheck_{expr,stmt}.c   # 静态类型检查 + Type 对象/构造器
│   ├── value.c              # Value/Obj、GC（mark-sweep）、map/env/string、json 编解码
│   ├── interp.c             # 树遍历解释器（value stack + jmp_buf 返回展开）+ 内建函数
│   ├── bridge_stub.c        # 桥接替身：route/tool 登记进 VM 表；run() 打印并 exit(2)
│   ├── builtins*.c          # 内建函数族（见下）
│   ├── loader.c             # 多文件模块加载链（import/export）
│   ├── vdom.c               # vdom 值：el()/render() 的构造与序列化
│   ├── codegen.c + codegen_internal.h  # 原生发射入口（CG context、容器助手、codegen_emit_ir）
│   ├── codegen_{types,expr,scan,sig,stmt}.c   # 按原注释区段切分（纯搬移）
│   ├── irbuf.c              # IR 文本缓冲（文本后端专用）
│   ├── backend.c / backend_llvm.c   # 两条原生后端的编排与链接收尾
│   ├── llvm_codegen.c       # libLLVM 路：用 LLVM C API 建 IR（仅 HAVE_LLVM 时编）
│   └── main.c               # CLI：--check / --dump / --watch / --compile / --no-pass
├── examples/                # 语言本体示例（无 UI、无服务器）
│   ├── hello.lume               # 最小入门
│   ├── lang-basics.lume         # 纯语言脚本（纯函数 + 打印）
│   ├── modules/app.lume         # 多文件模块示例
│   ├── native-fact.lume         # 原生编译冒烟（递归）
│   └── native-bench.lume        # 文本路 vs libLLVM 路跑分
├── tests/
│   ├── smoke.c              # 解释器 + 类型检查单测（106 项：输出逐字节比对 / 类型拒绝 / 运行时错误 / 多文件模块）
│   ├── test-crypt.lume      # crypt 内建样例（ crypt-test 用）
│   ├── native_backends.sh   # interp / text / llvm 三腿互比
│   ├── native-consistency.lume + .expected
│   ├── native-bench.expected
│   └── lsan-suppressions.txt
├── scripts/check-backend-parity.sh   # 两侧后端的 AST 标签覆盖一致性
├── Makefile                # make / check / dump / test / asan / native*
└── bin/lume-core            # 编译产物
```

> `src/rt.c` 是早期原型的残留（未进 `SRCS`，不参与构建），已知孤儿，别照着它
> 写新代码。

---

## 编辑器支持

VS Code 扩展（`editor/lume-vscode/`：TextMate 高亮、括号配对、跳转定义、大纲、
内建函数文档）**只存在于宿主树**；本树没有它。语言面两边一致，所以宿主树那份
扩展可以直接用在本树上。

C/C++ 扩展（IntelliSense）要从 `.vscode/c_cpp_properties.json` 取 include 路径，
而它不会读 Makefile —— 所以 libLLVM 那条路的头（`llvm-c/*.h`，被
`src/backend_llvm.h`、`src/llvm_codegen.c` 引用）会报

```
cannot find source file "llvm-c/Core.h"  → 检测到 #include 错误。
请更新你的 includePath … C/C++(1696)
```

`make vscode-cpp` 用 `llvm-config --includedir` 生成这个文件（需要 `python3`），
顺带把 libLLVM C API 必需的三个 `__STDC_*_MACROS` 和 `HAVE_LIBLLVM=1` 写进 `defines`。
路径带 LLVM 版本号（`Cellar/23.1.2/` 这种），所以**不入库、由目标生成**；
brew 升级 llvm 后重跑一次即可，别手改 JSON。生成后若红色报错还在，跑一次
「C/C++: 重置 IntelliSense」或重载窗口。

---

## 常规操作

```bash
make                  # 构建 bin/lume-core（探测 llvm-config，有就多编 libLLVM 路）
make check            # 类型检查全部语言示例（make 依赖）
make dump             # 打印 hello 示例的 AST
make vscode-cpp       # 生成 .vscode/c_cpp_properties.json（libLLVM 的 includePath）
make asan             # sanitized 构建 + smoke（ASan/UBSan）
make test             # 全部测试：backend-parity / smoke-bin / crypt-test /
                      # native-consistency / native / native-text
make backend-parity   # 只跑两个原生后端的 AST 覆盖一致性
make native           # 跑原生编译产物（默认 LLVM 路）
make native-text      # 强制文本 IR 路跑一遍
make native-bench     # 文本路 vs libLLVM 路跑分（含 --no-pass 对照）
make native-consistency  # 三腿输出一致性
make clean            # 清理 build/ 和 bin/
```

`--compile-llvm` 在没链 libLLVM 的构建里会被明确拒绝；`--compile-text` 任何
构建都可用（只依赖 clang）。

> 新增 AST 节点类型时跑一次 `make backend-parity` 即可确认两个原生后端
> （`src/codegen*.c` 文本 IR / `src/llvm_codegen.c` libLLVM）都接上了；单边
> 实现会直接炸套件。它已经挂在 `make test` 的依赖里，通常不必单独调。
> 注意 parity 脚本把 `src/codegen*.c` 当**同一侧**读——否则住在
> `codegen_stmt.c` 里的一个 `case N_...` 会被判成两路发散。

`main.c` 的 CLI 规则：先 `parse_program`，再 `type_check_program`（永远执行），
然后解释执行；`--check` 在类型检查后即退出（不产生副作用）。

---

## 语言速览

```lume
// 结构体声明（编译期类型，运行时就是 map）
type User = { name: string, age: int };

server {
  port = 8081;
  workers = 4;
}   // 本树仍解析并把值存进 vm->server_config，但没有伺服方——纯 inert

// func 参数/返回可选标注；未标注 = “宽松”(any)，route/tool 回调即如此
func greeting(u: User): string {
  return "hi, " + u.name + " (age " + str(u.age) + ")";
}

// 动态路径段：路径含 `:name` 段（`/api/stage/:id`）时逐段匹配并捕获
// URL 解码后的值，桥接层同步定位 DSL 路由，req.params 暴露出来。匹配优先级：
// 字面量精确 > 动态段 > 尾部 `*`（三遍扫描）。
route "GET", "/hello", func(req) {
  return { status: 200, type: "text/plain", body: "hi" };
};

// 方法缩写 + 箭头函数:`get "path", fn` ≡ `route "GET", "path", fn`
// (get/head/post/put/patch/delete/options 七个关键字; `(a,b)=>{}` ≡
//  `func(a,b){}`，但箭头不支持返回类型标注、函数体必须是块)
get "/ping", (req) => { return "pong"; };

// `write`/`read` 是内置方法组（预置在 interp 的种子里），无需声明：
// `write "/p", fn` 一次注册 POST/PUT/PATCH/DELETE，req.label 为约定标签。
write "/items", (req) => { return { method: req.method, action: req.label }; };

// handler 可省略：`write "/p";` 用 vm->default_handler（返回 { action, method, got }）
write "/ack";

// 自定义方法组用 `verbs` 声明（见 lume.h TOK_VERBS/N_VERBS）
verbs crud = { POST: "created", DELETE: "removed" };
crud "/things", (req) => { return { method: req.method, action: req.label }; };

tool "greet", "say hi", { name: "string" }, func(arg) {
  return { msg: "hi " + arg.name };
};

// Result 错误处理（无泛型：{ok:..}/{err:..} 单键 map 即 Result）
func maybe(a: int): Result {
  if (a > 0) { return { ok: a }; }
  return { err: "negative" };
}
func use(): Result {
  let v = maybe(7)?;   // `?` 解包 ok；遇到 err 直接向上传播
  print(str(v));
  return { ok: v };
}

run();   // 本树：bridge_stub 打印「需要 agent-httpd 宿主构建」并 exit(2)
```

### 类型系统

- 基本类型：`int` / `float` / `string` / `bool` / `null` / `Result`；全部
  **可空**（null 可赋任意类型），无需 `?` 后缀。
- 列表：`int[]`（`T[]`）；map 字面量/匿名结构体推断。
- 结构体：`type Name = { f: T, ... }`；具名结构体的字面量必须**字段齐全且不多余**。
- 推断：`let x = 5` 推导为 `int`；`5.5` 为 `float`；标注可省略。
- 规则：
  - `int -> float` 隐式加宽，其余无隐式转换（字面量 `5` 可给 `float` 参数）。
  - `if/while/and/or/not` 要求 `bool`（无 JS 式 truthiness）。
  - 类型关键字（`type/int/float/string/bool/Result`）在表达式里仍可作为普通
    标识符（所以 `int("42")` 内建可用）；也可作 map 键 / 字段名。
  - 未标注的参数/返回/handler = `any`，永不约束；内建函数全部 `any`。
  - 类型检查在**第一条错误即停**，停止后不执行。

### 内建函数

`run()` `print(...)` `str(...)` `int(...)` `float(...)` `bool(...)` `string(...)`
`len(x)` `keys(m)` `get(m,k[,def])`
`json(s)` `stringify(v)` `now()`（注册在 `interp.c: bridge_seed_builtins`）。
字符串变换：`replace(s, from, to)`（`src/builtins_str.c`，字面量全局替换，
空 from / 无匹配返回原串）。

发现/IO 类内建：`env(k)`、`files(dir)`、`read_file(path)`、`write_file(path, s)`、
`tools()`、`skills()`、`mcps()`：

- `tools()` / `skills()` 枚举**本树的注册表**（接口与宿主同名同义：
  `tools_count/tools_get/skills_count/skills_get`，另有 `tool_register/
  skill_register` 供以后扩展）。宿主从 agent-httpd 的注册表读表，本树那张表
  是空的——所以这些内建返回空列表。**函数是、语义是，只是没有东西可枚举**，
  语言面不留洞（也因此 `tests/smoke.c` 里 host 专用的「tools 注册表能枚举到
  scratch 工具」那条用例被删了）。
- `mcps()` 读 `<cwd>/.data/mcp-servers-{router,}.json`；本树不会同步这些文件，
  读不到即「无 MCP 服务器」= 空列表，不是报错。
- `write_file` 原子写（`.tmp.<pid>` + `rename`）+ `fchmod 0600`；
  `lock_file`/`unlock_file` 是 flock 排他锁。

`str`/`int`/`float`/`bool`/`string` 被 seed 成 callable 原生（`int("42")` 一直可以）。
带多参数时 = 取值 + 转换：`int(m,"a")` ≡ `int(get(m,"a"))`，缺键 → 类型零值
（int→0、str→字面 `"null"`），可传第三个参数当缺省。

`tool` 的参数 map 支持**裸类型关键字**：`{ a: int }` 求值时 `int` 是全局里的
native 值，`N_TOOL` 求值前会把 map 里 native/函数值 rewrite 成其名字字符串，
于是序列化成 `{"a":"int"}`，再由桥接层升级成 `{"a":{"type":"int"}}`。
千万别把裸 `"string"` 直接塞进 `properties`——宽松的网关会静默放行，严格的网关
会以 **HTTP 400 `invalid 'parameters' schema`** 拒收。

**tool handler 首参定型（零值缺省）**：`typecheck.c: N_TOOL` 从 params AST 直接
推导一个匿名 struct（`tool_param_struct`），一次性喂给 handler 第一个参数
（仅 arity==1）；`interp.c: N_MEMBER` 遇到缺键且 `member.type` 非空 →
`vm_zero_value` 返回类型零值（int/float→0、string→""、bool→false）。

---

## 解释器核心约定（改代码前必读）

1. **值栈即 GC 根**：每个 `eval_expr` 结束正好留下一个值在 `vm->stack` 上。
   GC 在分配前触发（`vm->gc_threshold`，默认 2MB），所有临时值因此都是活跃根。
   **不要让值“悬空”（先 pop 再分配同类对象）**。
2. **map_set/env_set 内部自根**：它们会先把新值 push 上栈再 strdup 键，
   避免中间分配回收入参。
3. **函数调用契约**：调用方先压 `callee`，再压 `argc` 个实参（顺序）→
   `call_function(vm, callee, argc)` → 该区间被替换成**单个结果**。
4. **返回展开**：用户函数调用前 push 一个 `jmp_buf`
   （`vm->jump_bufs[vm->jump_depth]`），`return` 填 `vm->call_result` 后
   `longjmp` 到 `jump_depth-1`；正常/长跳两条路径都恢复 `jump_depth` 并停用 frame。
5. **`?` 传播**复用同一机制：`interp.c N_CALL` 中若结果 map 有 `err` 键 →
   `vm->call_result = 该map; longjmp(...)`；有 `ok` 键 → 栈顶换成 `ok` 值。
6. **VM 每进程一份**，没有 worker/fork 模型（本树不进服务器）。因为不再有
   「请求之间复用 VM」这回事，**`vm_after_request` 那套清场逻辑在宿主树才有**；
   本树的 VM 生命周期就是整个进程。
7. **桥接替身的守则**：`bridge_*` 原型在 `lume.h`，替身与宿主实现必须**同签名**；
   `bridge_run` **必须失败退出**（打印 + `exit(2)`），不能静默返回——
   一个「看起来已经服务过」的静默返回比明确的失败更糟。
8. **AST/Type 生命周期 = 进程**：一次性解析，永不 free；可直接借用 AST 里的
   `Type** param_types` 数组（typecheck.c 就是这么做的）。

---

## 怎么加功能

### 加一个内建函数

1. `interp.c` 写 `static void native_xxx(...)`，再包一层 `b_xxx`（NativeFn）。
2. 在 `bridge_seed_builtins` 的 `built[]` 里登记。
3. `typecheck.c` 的 `BUILTINS[]` 已默认按 `any` 放行，无需改。

### 加一种新语句/关键字

1. `lume.h`：加 `TokenType` 枚举项；`token.c` 补 `TOKEN_NAMES` 的对应名字。
2. `lexer.c`：`KEYWORDS[]` 或标点分支。
3. `parser.c`：`parse_statement` 分支生成新 `NodeType`；`lume.h` 扩 AST union；
   `node_print` 加调试输出。
4. `typecheck.c`：`ck_stmt`/`ck_expr` 处理新节点（编译期强类型就在这保证）。
5. `interp.c`：`exec_statement`/`eval_expr` 给运行时语义；
   **两条原生后端也要接**（`codegen*.c` 与 `llvm_codegen.c` 各一个 `case N_X`）。
6. `tests/smoke.c` 补一条 `check(...)`（正常路径）和必要时 `reject(...)`
   （类型错误路径）。

### 多文件模块（import / export）约定

- **语法层**：`import "路径" as ns` 与 `export` 前缀在 `parser.c
  parse_statement` 顶部；`Node.is_export` + `N_IMPORT`（`as.imp.path` 保留
  **带引号原始串**，loader 负责解转义）。
- **加载链在 `loader.c`**（`make` 的 SRCS 里有它）：`loader_run` 是 `main.c`
  唯一入口——解析、类型检查、执行都从这里走；`--check` 与执行共用同一条链
  （递归检查依赖但**不执行**）。
- **作用域模型**：每个模块一个 `Module`（`lume.h`）：独立顶层 `Env` +
  `exports` 导出表；`N_IMPORT` 在运行时把 `ns` 绑定成导出表对象
  （`OBJ_ENV`），成员读取走 `interp.c` 的 `OBJ_ENV` 分支；类型检查侧
  `scope_put_ns/scope_get_ns` + `export_add` 写回 `export_types`。
- **GC 约定**：模块 `env/exports` 要挂进 `vm->active_envs` 链（loader.c
  `exec_module`），否则下一个模块执行触发 GC 时会被回收。
- **入口模块**：`env = vm->globals`（内建与 `route/tool` 注册都靠它）；
  依赖模块 `env` 的 parent 指向入口 globals，保证内建可见。
- **循环检测**：`load_stack` + `Module.loading`，报 `circular import`；
  `Module.executed` 保证菱形依赖里被共享的库顶层只执行一次。
- **测试**：`tests/smoke.c` 的 `check_modules()`（写临时目录后走 `loader_run`）；
  `examples/modules/` 是可运行示例，已进 `make check`。

### 加一个新内置类型（如 date）

- `lume.h TypeKind` / `typecheck.c` 的 `ck_expr` literal 与 `type_compat`、
  `ty_str/tp_inner` 都要同步加分支。

---

## 渲染内建（Lume 侧的 SSR 与 vdom）

本树保留 `el()` / `render()` / `html()`（实现在 `src/vdom.c` 与 `interp.c` 的
`render_value`），它们是**语言内建**，不依赖任何前端框架；宿主树额外有的
React 客户端（`frontend/` + `www/` docroot + pnpm）**本树没有**。

- **两套写法，可互相组合**：
  - `el(tag, props, ...children)` 构建 vnode 树 `{type, props, children}`；
    `render(tree)` 一次性序列化整棵树为 HTML。
  - `html("...{0}...{1}...", a, b)` 是**模板字符串**，`{N}` 按位置替换第 N 个
    实参（`interp.c` 的 `html_slot`）：
    - **独立标量槽**：字符串/数字/bool/null/map → 转义后按文本输出（默认安全）。
    - **列表槽**：视为子节点序列；vnode 结构性渲染、字符串原样输出。
    - vnode 槽：结构性渲染、不转义。
    - `{{` / `}}` 输出字面 `{` / `}`。
  - 组件函数**刻意不加返回类型**（loose/any——UI 是动态结构）。
- **序列化规则**：
  - 文本与属性值一律 **HTML 转义**（`& < > "`）。
  - `on*` 事件属性在 SSR **丢弃**（没有客户端可挂）。
  - `data_page` → `data-page`（下划线转连字符）。
  - void 标签（`img/link/meta/br...`）不输出闭合标签；`class:false` 不输出；
    bool true 输出 `k="true"`；裸值（非元素 map）回退成转义 JSON。
- **`server{ views = ... }` / docroot 静态伺服**：属于宿主的框架层，本树没有
  静态文件分发；`server{}` 里的字段被解析成值，但不驱动任何行为。

---

## 测试约定

- `make test` 依次跑（本树无服务器，所以**没有** live HTTP 那条腿）：
  1. `make`（构建）
  2. `backend-parity`：`scripts/check-backend-parity.sh` 逐个比对两个后端各 28 个
     `N_*` AST 标签的覆盖；
  3. `tests/smoke-bin`：106 项单测（解析 + 类型检查 + 执行，stdout 逐字节比对）；
  4. `crypt-test`：`tests/test-crypt.lume`；
  5. `native-consistency`：`tests/native-consistency.lume` 三腿（interp / text /
     llvm）输出与 `tests/native-consistency.expected` 一致；
  6. `native` / `native-text`：两条原生后端的真实编译 + 运行。
- smoke 的 `capture_begin/End` 会用 `dup` 暂存原 stdout 再恢复；新增测试直接
  复用 `check(name, src, expect)`（正常路径，逐字节比对 stdout）/
  `reject(name, src, 含的错误子串)`（类型检查期拒绝）/
  `check_err(name, src, 含的错误子串)`（能过类型检查、但运行时必须报错的路径，
  比如 `sqrt(-1)`、`abs("3")`）。需要「带某个开关跑一遍」时用同文件里的
  `check_err_nofs(name, src, 错误子串)`：它置 `g_no_fs` 再调 `check_err`、
  用完必清。`g_no_fs` 必须声明在 `check_err` **之前**。
- `make asan` = sanitized 构建 + `tests/smoke-bin-asan`（本树没有静态库要插桩，
  所以 e2e 腿不存在）。ASan/UBSan 干净的好处是：解释路径上的 UB 会被逮住，而
  这不是"能过就行"——`type` 树共享引用、`jmp_buf` 长跳、GC 悬空值都靠它。
- `tests/native_backends.sh` 三腿互比是**防漂移的 Main net**：任一侧后端少写
  一个 `case N_X`，输出就会和另外两条腿分叉。

---

## 已知约定 / 坑

- 本树不监听端口，没有端口约定（宿主树的 demo/invest/hub 端口与
  agent-httpd 的 18080/18081/18101 一律不适用于本树）。
- `route`/`tool` 语句在加载期**立即求值**，被引用的函数必须**先定义再注册**
  （反过来会运行时报 `undefined variable`，路由不注册——纯 `--check` 发现不了，
  那是运行期错误）。
- `server {}` 里 `type` 等关键字可做字段名，但作为顶层变量名会被当作类型
  关键字（保留字语义）。
- 具名结构体返回类型要求字段齐全：`type P={x,y}` 时 `return {x:1}` 是类型
  错误（缺失 `y`）。
- 类型检查会拒绝 `not 0` / `not ""`（不是 bool），如需 truthiness 语义请显式转 bool。
- `--watch` 热重载的每次编辑都会重解析 + 重类型检查，新 AST / Type 树不
  释放——**这是有意的 dev 工具泄漏**（parse/typecheck 无 free 路径，且 Type
  对象存在共享引用：多个引用点经 `resolve()` 指向同一个 struct 定义，递归
  free 会 double-free）。**不要**给 Type 树加递归 free 而不先处理共享引用语义。
- 原生跑分靶子必须**双层循环**：外层局部令返回值随 n 三次方增长，内层包
  `K=1000` 才有界又够迭代；裸 loop 到 1e8 会溢出 i64。
