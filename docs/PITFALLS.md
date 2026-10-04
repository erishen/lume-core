# Lume 实战坑位清单（PITFALLS）

> 在 Lume 上真刀真枪搭过完整业务（标讯信号仪表盘、CRM、个人投资台账）后沉淀。
> 目标是：**让下一个人写 `.lume` 时，用 C 实现的直觉就能绕开 80% 的运行时惊吓。**
> 每条都标注了「已修复 / 现状如此」，修复版本见 git log。
>
> 配套 **[STYLE.md](STYLE.md)**：这里说「别踩什么」，那里说「踩过的坑之后
> 正确的写法长什么样」（箭头表达式体 / `try` 固定键 / 字符串键 map / 写路径纪律）。

---

## 1. 同名变量与函数会静默覆盖 —— 已修复（编译期拦截）

**坑**：`let crm_lock = "/tmp/x.lock";` + `export func crm_lock() { ... }` 曾经编译全绿。
`let` 和 `func` 进同一个作用域表，后声明的覆盖先声明的，**运行时按声明顺序决定谁活着**——
最坏的一类 bug：编译过、主进程不触发、只在 fork 出来的 worker 里、只在特定写路径上报错。

**现状（2026-09-27 起）**：`--check` 报 `duplicate declaration of 'xxx' in the same scope`。
只拦**用户声明之间**的重名（let/let、let/func、func/func、参数之间、函数体内 let 与参数）。
以下仍然合法：

- 内层遮蔽外层：`let x = 1; { let x = 2; }` ✓
- 遮蔽内置函数：`let len = 3;` ✓（内置名允许用户覆盖）
- for-in 迭代变量复用：`let k; for (k in m) { ... }` ✓（迭代是赋值式绑定，不是声明）

**建议**：写业务时仍定期自查「锁路径变量 vs 同名函数」这类命名，编译器现在能兜底，
但别依赖它。

---

## 2. 列表不能在 DSL 层 push —— 已修复（`push` 内置）

**坑**：`list_push` 曾经只是 C 内部 helper，DSL 里无法原地追加元素。只能
`map(fn, keys(m))` 现造列表，人体工学很差。

**现状（2026-09-27 起）**：`push(list, item)` 是普通内置函数，返回原列表：

```lume
let acc = [];
push(acc, 1);
push(acc, 2);      // [1, 2]
```

> 提示：数据设计仍然建议「以 map 为骨干」（id → 记录），读的时候再现造列表——
> 这是 Lume 的数据模型强项（map 键查找 + 迭代键），不是缺陷。

---

## 3. map 字面量键只收标识符/数字，不收字符串 —— 已修复

**坑**：`{"a": 1}` 报 `expected map key (name or string)`——报错文案写着 "string"，
实际只收标识符类 token。而 `json()` 解码出来的键**又是字符串**，于是
「字面量写法」和「round-trip 的键类型」不一致，种子数据只能 `put()` 动态搭。

**现状（2026-09-27 起）**：字符串字面量可直接作 map 键和成员名：

```lume
let m = {"a": 1, "b": 2};
m."带 空 格 的 键" = 3;   // 成员访问也能用字符串键了
```

**注意（现状如此）**：字符串键剥掉首尾引号，**键内的转义序列原样保留**
（`{"a\nb": 1}` 的键是字面 `a\nb`，不是换行后的 `a`+换行+`b`）。含转义的键极罕见，
如果真用到，用 `put(m, "a\nb", 1)`（运行时字符串求值会正确反转义）绕过。

---

## 4. `json()` 解析失败直接 500，DSL 接不住 —— 已修复（`try` 内置）

**坑**：`json()` 解析失败会置起 VM 级 error，它是 sticky 的——DSL 层没有捕获机制，
整个 handler 直接 500。合法 body 没事，但「宽松解析 + 兜底」写不出来。

**现状（2026-09-27 起）**：`try(func)` 捕获被调函数内部的一切 VM error，返回
**固定双键结构** `{ ok: 结果, err: null }`（成功）或 `{ ok: null, err: 消息 }`
（失败）——两个键始终都在，`r.err == null` 即成功。参数可以是 `func` 字面量
或箭头函数（含表达式体 `=> expr`，2026-09-27 起支持）：

```lume
let r = try(() => json(req.body));          // 表达式体箭头
if (r.err != null) {
  return { status: 400, body: { err: "body 不是合法 JSON: " + r.err } };
}
return { status: 200, body: r.ok };
```

**注意一**：`try` 返回的是普通 map，不是 Result 类型——不需要 `?` 操作符
（`?` 是给 `{ok}/{err}` 字面量结果用的）。error 在 `try` 返回前已被清理。

**注意二（成员访问 vs get()，真实踩坑 2026-09-27）**：Lume 的成员访问
`m.xxx` 是**严格**的——键缺失时抛 sticky error（`map has no field 'xxx'`），
不是返回 null；只有 `get(m, key, default)` 是宽容的。曾在 `try` 还返回单键
结构时，成功路径（map 只有 `ok` 没有 `err`）用 `r.err` 直读判空，反而 500。
现在 `try` 返回双键结构，`r.ok` / `r.err` 直读安全；但对**未知来源的 map**
（`json()` 解码、外部数据、上游返回值）判断键是否存在，一律用
`get(m, key, null)`，不要用成员访问。

---

## 5. 静态伺服没有 SPA fallback —— 已修复（`server { spa = true }`）

**坑**：前端用 history 路由（React Router 等）时，刷新 `/customers/42` 会 404——
之前被迫 hash 路由 + 壳脚本归一化旧 URL（还修过它刷新时把 hash 覆盖掉的坑）。

**现状（2026-09-27 起）**：

```lume
server {
  port = 8091;
  workers = 2;
  docroot = "./www";
  spa = true;      // 静态 404 且客户端要 HTML 时回退到 docroot/index.html
};
```

回退条件是**三者同时满足**：`GET` + `Accept: text/html` + 静态路径解析失败。
API 404、非 HTML 请求（`fetch`、curl 无 Accept）、HEAD 全部保持原样 404，
不会把前端壳吐给 API 客户端。

---

## 6. 其他值得知道的现状（设计如此，别踩）

### for 尾分号
`for (init; cond; incr) { }` 的 `incr` 表达式**必须**以分号结尾，漏了会报语法错。
`for (x in xs) { }` 没有此问题。

### map 键类型：标识符 vs 字符串
- 字面量 `{ ok: 1 }` / `{ err: 1 }` 单键会被类型检查器识别为 **Result**。
- 如果你真的想构造一个**普通 map**、恰好键叫 `ok`/`err`，用两个键以上，或
  `put(m, "ok", 1)` 动态搭。

### 保留字做不了变量名
`let type`、`let put`、`let get` 都会在 parser 层报 `expected variable name`
——这些是类型/动词/路由关键字。但它们**都能做 map 键和成员名**（`m.type`、`{ put: 1 }` 合法）。
调用同名内置没问题：`put(m, "k", v)`、`get(m, "k")` 照常工作。

### 函数调用位置宽松，声明位置严格
调用 `put(m,k,v)`、`get(m,k)` 合法（表达式层接受方法关键字），
但 `let put = ...` 不合法（声明层只收纯标识符）。两边行为不一致，记住即可。

### import 命名空间 vs 内置函数（已修复 v0.4.2）
`import "m.lume" as tools` 曾经与内置 `tools()` 重名时报 `duplicate name`，
而 `let len = 3` 遮蔽内置是合法的——两条声明路径行为不一致。v0.4.2 起
import 命名空间与 `let`/`func` 同语义：**内置名允许遮蔽**。仍拦：两个
import 同名、import 与用户声明（`let`/`func`）同名。注意：遮蔽内置后该名字
的内置调用不可用（`as tools` 后 `tools()` 会尝试调用命名空间对象而报错），
确实还需要内置函数时，换个命名空间名（如 `as topics`）。

### hash 路由刷新覆盖（已随 SPA fallback 解决）
旧前端用 `#/items` 路由，刷新时浏览器会把 `#` 后的部分当作 URL 的一部分发给服务端。
现在用 `spa = true` 走 history 路由，不再需要 hash。

### 日志脱敏口径
数据库错误原文只进 **stderr**（`[db] 类别: 原文`），DSL 侧拿到的只有简短类别
（`postgres: query failed` 之类）——不要试图在 handler 里拼 SQL 错误细节给用户看，
拿不到（这是刻意的）。

---

### 编辑器跳转是静态启发式（别拿它当类型检查）

扩展的跳转/大纲用正则 + 花括号深度扫描，不解析表达式：

- 跨文件只认 `import "…" as ns` 映射，别名转发（如 `let db = data; db.x`）跳不动；
- 字符串/注释里的 `//`、`{}` 会干扰行级解析（多数自平衡，偶发误判只影响跳转）；
- 用户定义的同名符号会覆盖内置函数跳转（期望行为）。

不影响编译与运行——`make check` 才是权威校验。

## 7. 运维成熟度（本树现状）

- **无嵌入式宿主库**：本树不链 `libagenthttpd.a`、没有 git submodule、没有服务
  器要守护，构建与分发就是一个 `bin/lume`（外加文本路需要的 `clang`）。
  宿主树那边才有「子模块提交 → push → Lume 根 bump → 独立项目 pull」那四步。
- **数据规模**：JSON + flock 是目标形态，适合个人/小团队内部工具；
  多人多租户、大数据量请另选型。**本树没有 `sql_query`/`sql_write`**——
  那条 SQLite 通道随 `builtins_sql.c` 一起留在宿主树（它依赖宿主的
  `db_layer.h`），这里的数据面只有文件与 flock。

---

## 8. 出站 HTTP（`http_get`，2026-10-04）

这一组都是实现 `builtins_http.c` 时真踩过的，症状都长得像"网络不通"，实际
全是参数/口径问题：

1. **`getaddrinfo` 的 service 不能是 `NULL`，也不能图省事填 `"0"`。**
   node 为空时 service 非空才不会 `EAI_NONAME`（否则 `127.0.0.1` 这种 IP
   字面量形式直接解析失败）；但填 `"0"` 会让返回的 `sockaddr` 里
   `sin_port = 0`，`connect()` 到端口 0 得到 `EADDRNOTAVAIL`（49）。
    connect 前必须把**真实端口**当 service 传。
2. **等待函数收的是绝对 deadline，不是相对毫秒。** `now_ms()` 从开机起算，
   传 `15000` 表示"15 秒"立刻过期。给 TLS 握手单独设上限时要
   `now_ms() + min(剩余, 上限)`。
3. **`poll()` 返回 `EINTR` 要重来。** 原本 `if (r < 0) return -1;` 会把一个
   定时/子进程信号当成"连不上"，表现是代理握手随机超时。
4. **状态码不能硬编码下标。状态行 `"HTTP/1.1 200 OK"` 的状态第一位在下标
   9**（`"HTTP/1.1"` 占 0..7，空格在 8）；CONNECT 的回包同理。
5. **`sbuf` 释放要复位。** 只 `free(p)` 不置 NULL，下一跳或收尾再 free 一次
   就是双重释放（`exit 134` / SIGABRT）；但也不能在拼结果 map 之前就复位
   ——那时 `body.len` 已经清零，map 里的 body 会变成空串。
   统一走 `body_done()`（free + 复位）一个出口。
6. **默认发 `Accept-Encoding: identity`。** 本实现不解压；中间代理/服务端回
   gzip 时 body 是一串二进制，看着像"拿不到内容"。
7. **timeout 的单位是秒。** `{ timeout: 30 }` 内部转毫秒；当毫秒用会导致还没
   握手完就超时。
8. **`any` 不是类型。** `func f(r: any)` 报 `unknown type 'any'`；参数想接
   任意值就不写标注（松散）。

---

## 9. 避坑优先级（新手先看这三条）

1. 变量/函数命名：同名覆盖已编译期拦截，但**保持命名区分**（`crm_lock_path` vs `crm_lock()`）。
2. 数据统一走 map（id → 记录），列表只做展示层现造。
3. 任何解析/外部输入进 JSON 之前包一层 `try(func(){...})`，让 500 变 400。
