# Lume 用户指南

写 `.lume` 文件,就能跑起一个 HTTP 服务、一套页面和一个可接真实大模型
的 Agent。本指南只讲 Lume 这一层:**业务开发只需要写 `.lume`(以及可选的
前端脚本和 CSS),不需要接触其他任何语言。** 维护者视角的 C 内部说明见
[DEVELOPMENT.md](DEVELOPMENT.md)。**写业务前先读 [PITFALLS.md](PITFALLS.md)**
——同名覆盖、map 键、json 失败、SPA 路由这些实战坑都在那,能少踩半天坑。

---

## 快速开始

```bash
make dev          # 完整示例 demo → http://localhost:8081
make dev-minimal  # 最小入门示例 examples/hello.lume → http://localhost:8082
make invest       # 投资助手例子 examples/invest.lume → http://localhost:8082
make hub          # 网关能力台例子 examples/hub.lume → http://localhost:8083
```

打开 `make dev` 的浏览器看 `/`(首页)、`/counter`(服务器状态)、`/chat`(Agent)、
`/hello`(纯 API)。改完 `examples/demo.lume` 后 Ctrl-C 重跑即可。
想只看最简版先读 `examples/hello.lume`(它还带一个演示 POST/PUT/PATCH/DELETE
的 React 小页面:开 `http://localhost:8082/items`;壳是静态 `www/hello/items.html`,
组件在 `frontend/src/hello/items.tsx`(TypeScript + Tailwind),`make dev-minimal`
会自动用 esbuild/Tailwind 打成 `www/hello/items.js` + `www/hello/items.css`);
只想算点东西(不入服务器)用 `examples/lang-basics.lume`。
`make invest` / `make hub` 是两个完整产品示例,各自带前端页面。

常见命令:

```bash
bin/lume examples/demo.lume        # 直接运行
bin/lume --check examples/demo.lume  # 只做类型检查,不启动(等效 make check)
bin/lume --dump examples/demo.lume   # 打印 AST(等效 make dump)
make test                          # 全部测试(见文末)
```

端口一律在 **10000 以下**(示例:demo 用 :8081、invest/hello 用 :8082、hub 用
:8083;`server{}` 里省略时是框架默认 :18080,见下)。

---

## 最小服务器

```lume
server {
  port = 8081;
  workers = 4;
  docroot = "./www";    // 静态资源目录(css/js/图片),可选
}

// 路由:方法 + 路径 + 处理函数(函数要先定义,再被 route 引用)
func hello(req) {
  return {
    status: 200,
    type: "text/plain",
    body: "hello, you asked for " + req.path + " (" + req.method + ")",
  };
}
route "GET", "/hello", hello;

run();    // 启动服务器,一直阻塞
```

`server {}` 里的字段可以留给 **`.env`** 兜底(在 CWD 读取,和 LLM 配置同一个文件):

```dotenv
PORT=8081
WORKERS=4
DOCROOT=./www
```

优先级:**`.lume` 里写的字面量 > `.env`/环境变量 > 框架默认**(端口 18080、
fork-per-connection、docroot `./www`)。所以本地写死端口、部署时用 `PORT` 覆盖
不了字面量——要由环境变量接管,就**别在 `server{}` 里写该字段**。

**静态资源**:`docroot` 里的文件按路径直接伺服(如 `/invest/app.js`、`/app.css`)。
和多数 Web 服务器一样,`/` 默认映射到 `index.html`,且**无扩展名的路径会回退
到同名 `.html`**(`GET /items` → `www/hello/items.html`;带扩展名或没有同名文件则
404)。所以静态页面可以不写扩展名。

**`views`——docroot 下的页面子目录(可选)**:一个示例站的页面壳和它自己的
bundle 可以先从这个子目录伺服,URL 却**保持根路径不变**。静态分发的完整顺序是
**DSL 路由 → views 子目录 → docroot 根**:

```lume
server {
  port = 8083;
  views = "hub";      // URL 先落 <docroot>/hub 再回落 docroot 根
  docroot = "./www";
}
```

文件落在 `www/hub/` 时,`/`、`/chat`、`/hub-home.js` 这些 URL 都由 views 命中,
而共享的 chunk、`invest/app.js`、`app.css` 不在 views 里,自然回落 docroot 根。
所以网站目录可以按示例组织(见下),而各示例共享的资产仍只放一份在 docroot 下。

**示例目录组织**(`examples/*.lume` 与前端一一对应):

```
examples/invest.lume  → frontend/src/invest/  → www/invest/  (views = "invest")
examples/hub.lume     → frontend/src/hub/     → www/hub/     (views = "hub")
examples/hello.lume   → frontend/src/hello/   → www/hello/   (views = "hello")
```

每个 example 一个 `frontend/src/<名字>/` 和一个 `www/<名字>/`;页面壳引用自家
bundle 时写根 URL(如 `/dashboard.js`),views 会把它映射进子目录。前端由
esbuild 一次构建**分包**(bundle-splitting):React 抽成一个 `www/chunk-*.js`
共享 chunk,每页只出 3~10kb 的入口包,`<script type="module" src="...">` 挂载。
首访任意页面后 React chunk 被浏览器缓存,其它页面与 invest/hub 共用的聊天
bundle(`/invest/app.js`)都只复用这一个 chunk;壳样式 `/app.css` 同理全站一份。

(只有 demo.lume 和 lang-basics.lume 没有前端:`demo.lume` 是 SSR 试验台
——页面纯由 `render()/html()` 生成,被 `make test` 驱动;`lang-basics.lume`
是纯语言脚本,不启动服务器。)

**响应规则**——默认值给全了,能省就省:

| 处理函数返回 | 状态码 | Content-Type | 响应体 |
|---|---|---|---|
| 字符串 | 200 | `text/html` | 该字符串 |
| 普通 map(不含 `body` 键) | 200 | `application/json` | 整张 map 自动 JSON 化 |
| `{ status?, type?, body }` | 默认 200 | 默认:body 是字符串→`text/html`,否则→`application/json` | body(非字符串自动 JSON 化) |
| 列表 / 数字 / bool | 200 | `application/json` | 自动 JSON 化 |

所以返回 JSON **不用写 `stringify`,也不用写 `status`/`type`**:

```lume
get "/api/stuff", (req) => {
  return { a: 1, b: "x", when: now() };        // 自动 200 + application/json
};

post "/api/items", (req) => {
  return { status: 201, body: { created: true } };  // 要改状态码才写 status
};
```

只有需要非默认 Content-Type(比如 `text/plain`)时才显式给 `type`:

```lume
return { type: "text/plain", body: "hello" };
```

注意:map 里**一旦出现 `body` 键就按"信封"解释**,`status`/`type` 才具有
特殊含义;若你想返回一个字段名恰好叫 `body` 的 JSON 对象,包一层即可:
`return { body: { body: "x" } };`。

处理函数收到一个 **请求 map**,字段有:`method`、`path`(含 query 串)、
`remote_addr`、`host`、`content_type`、`content_length`、`user_agent`、
`body`、`query`(原始查询串,不含 `?`;无查询 → `null`)、`query_params`
(解析后的参数 map,key/value 均已 URL 解码,`+` → 空格,段无 `=` 时值为
空串,重复 key 后者覆盖;无查询 → 空 map)、`params`(动态路径段捕获,
`:name` 段 → 解码后的值;路由无动态段 → 空 map)。

### 方法与路由缩写

除了 `route "GET", "path", handler`,可以直接把 HTTP 方法当语句头:

```lume
get     "/",           (req) => { return "home"; };
post    "/echo",       (req) => { return "posted"; };
put     "/api/things", (req) => { return "updated"; };
patch   "/api/things", (req) => { return "patched"; };
delete  "/api/things", (req) => { return "deleted"; };
head    "/health",     (req) => { return ""; };
options "/api",        (req) => { return ""; };
```

`get "path", fn;` 完全等价于 `route "GET", "path", fn;`,也支持路径通配
(`get "/files/*", fn`)。

**动态路径段**:路径可含 `:name` 段(`get "/api/stage/:id", fn`),匹配
**恰好一个**路径段(不跨 `/`),捕获值经 URL 解码后放进请求 map 的
`params` 字段:`get(req, "params", {}).id` 取参。字面量与动态段可混用
(`get "/api/stage/:id/quiz", fn`)。匹配优先级:字面量精确 > 动态段 >
尾部 `*` 通配,所以 `get "/api/me", fn` 永远赢过 `get "/api/:a", fn`。已注册的路由在框架内置的"方法门"之前分发,所以
**不需要 `/cgi-bin` 路径**,PUT/PATCH/DELETE 会直接命中你的 handler;方法名
也不限于上面七个——`route "PROPFIND", "/x", fn` 这类任意字符串同样有效。

注意 `get` 同时是内建函数(取 map 字段):只有 `get "字符串", ...` 这种形式
才被当作路由,`get(m, "key")` 仍是普通函数调用,`{ get: 1 }` 这种键也不受影响。

#### 内置方法组 `write` / `read`

最常见的"多个写方法共用一个 handler"**无需声明**,直接用内置组。连 handler 也能省:

```lume
write "/items";                 // 默认应答:{ action, method, got }

write "/items2", (req) => {     // 自定义 handler:req.label 是方法的标签
  return { action: req.label, method: req.method, got: req.body };
};
```

- `write "path";` 一次注册 **POST/PUT/PATCH/DELETE**;省略 handler 时用默认应答
  `{ action: <标签>, method: <方法>, got: <原样请求体> }`(无标签的组 `action` 为 null)。
- `write "path", handler;` 则共享你给的 handler,组内每个方法带一个约定标签
  (`created` / `replaced` / `patched` / `deleted`),请求时注入 `req.label`。
- `read "path";` / `read "path", handler;` 注册 **GET/HEAD**(纯方法列表,无标签)。
- 两者就是普通全局变量(map / list),可 `get(write, "POST")`、`len(read)`;
  自定义 `verbs write = ...;` 会覆盖(遮蔽)内置定义。
- handler 省略对所有路由写法都成立:`route "GET", "/x";`、`get "/x";` 同样可用默认应答。

#### `verbs` —— 自定义方法组

若要自己的方法集合或标签,用 `verbs` 先声明一个方法组,再用组名注册:

```lume
// 列表形式:只有方法,没有标签
verbs write = ["POST", "PUT", "PATCH", "DELETE"];

// 映射形式:方法 -> 标签,标签会在请求时注入到 req.label
verbs crud = { POST: "created", PUT: "replaced",
               PATCH: "patched", DELETE: "deleted" };

func items_reply(req) {
  // 列表组里 req.label 不存在;映射组里它是对应方法的标签
  return { action: req.label, method: req.method, got: req.body };
}

crud "/items", items_reply;   // 等价于四行 post/put/patch/delete "/items", ...
```

- `verbs name = <表达式>;` 里的表达式会**立即求值**,必须是字符串列表(仅方法)
  或"方法 -> 标签"映射(标签须为字符串/null),否则报错。
- 组名本质就是一个普通变量:列表组是 list,映射组是 map。可以 `len(write)`、
  `get(write, 0)`、`get(crud, "POST")`(= `"created"`)。
- `name "path", handler;` 会为组内**每个**方法各注册一条路由,共享同一 handler。
  路由在请求时按方法字符串精确匹配,`req.method` 是命中的方法;映射组还会把该
  方法的标签放进 `req.label`(列表组/普通 `route` 没有这个字段)。
- 组名必须**先声明后使用**,且只识别"标识符紧跟字符串字面量"这种写法——普通
  `foo "bar"` 不是合法表达式,所以不存在歧义。

---

## 语言速览

Lume 是 JavaScript 风格、**强类型**的脚本语言。类型在运行前全部检查完毕。

```lume
// 结构体(类型别名):编译期检查,运行时就是一个 map
type User = { name: string, age: int };

func greeting(u: User): string {
  return "hi, " + u.name + " (age " + str(u.age) + ")";
}

let u: User = { name: "Tom", age: 42 };   // 字段必须齐全、不能多也不能少
print(greeting(u));
```

### 类型

| 类型 | 说明 |
|---|---|
| `int` / `float` | `5` 是 int,`5.5` 是 float;int 自动加宽成 float |
| `string` / `bool` | `"x"` / `true` `false` |
| `null` | 所有类型都**可空**(null 可赋给任何类型,无需 `?` 后缀) |
| `T[]` | 列表,如 `int[]` |
| `type Name = {...}` | 具名字段结构体 |

类型规则:

- **严格 bool**:`if` / `while` / `and` / `or` / `not` 里的值必须是 `bool`;
  符号写法 `&&` / `||` / `!` 与关键字 `and` / `or` / `not` 完全等价(同为
  短路求值,`!` 为一元取反;`&&` / `||` 短路,不计算右侧多余表达式)
  (`if (1)` 是错误,没有 JS 式的 truthiness)。类型检查会在**第一条错误就
  停止**,不会执行。
- **无隐式转换**(除 `int -> float`);字面量 `5` 可以传给 `float` 参数。
- 函数参数/返回值**可以不标类型** = 不约束(任何类型都行),UI 组件、路由
  处理函数通常就不标。
- 类型关键字(`type/int/float/string/bool/Result`)在表达式里仍是普通
  标识符,所以 `int("42")` 可用,map 的 `type` 字段名也合法。

### 变量与函数

```lume
let a = 5;            // 类型自动推断为 int
let b: int = 5;       // 显式标注
// 普通变量没有 const/let 之分,赋值即修改

func add(x: int, y: int): int { return x + y; }   // 传统写法
let add2 = (x: int, y: int) => { return x + y; }; // 箭头写法,等价
let loose = (x) => { return x; };                 // 未标类型 = 任意
let f = add;          // 函数也是值,可以赋给变量
let g = (a, b) => { return a * b; };              // 箭头也可做路由 handler
```

`func(...) {...}` 与 `(a, b) => {...}` 完全等价,按喜好混用。区别只有一点:
箭头不支持返回类型标注(需要时用 `func`)。箭头体有两种写法(2026-09-27 起):

```lume
let f1 = (x) => { return x * 2; };   // 块体(传统写法)
let f2 = (x) => x * 2;               // 表达式体: 隐式 return,等价于 f1
let r  = try(() => json(body));      // 表达式体最常见的用法: 捕获解析错误
```

`=>` 后是 `{` 一律按块体解析(与 map 字面量体的歧义按 block 优先);要返回
map 字面量,用分组 `=> ({ a: 1 })`。`(x) * 3` 仍是普通的括号分组,不会被当成函数。

### 控制流与运算符

```lume
if (n > 0) { ... } else { ... }
while (n < 10) { n = n + 1; }

// for 的两种形态:
for (let i = 0; i < 5; i = i + 1) { ... }   // C 风格: init; cond; incr 皆可省略 (for (;;))
for (x in xs) { ... }                        // 迭代: 列表的元素 / map 的键(排序)
for (let x in xs) { ... }                    // 显式 let 的 for-in 形式

// break / continue 在 for 和 while 里都可用
for (x in xs) {
  if (x == null) { break; }     // 跳出循环
  if (x == "")   { continue; }  // 跳过本轮,直接进下一次迭代
}

// 比较: == != < <= > >=    算术: + - * / %
// 逻辑: and or not(严格 bool)
```

字符串用 `+` 拼接;**相邻字符串字面量自动合并**(`"a" "b"` ≡ `"ab"`,JS 风格,合并发生在解析期,`\n` 等转义跨连接保持)。数字转字符串用 `str(x)`。

### Result 错误处理(无异常机制)

`Result` 不是真正的独立类型——任何一个**单键 map** 都可以:
`{ ok: v }` 表示成功,`{ err: "消息" }` 表示失败。

```lume
func divide(a: int, b: int): Result {
  if (b == 0) { return { err: "division by zero" }; }
  return { ok: a / b };
}

func use(): Result {
  let q = divide(10, 2)?;   // `?` 解出 ok 的值;若是 err 则整个函数直接返回这个 err
  print("10 / 2 = " + str(q));
  return { ok: 1 };
}
```

**`?` 只能出现在函数内部**,它会截断当前函数、把 `{ err: ... }` 一路向上抛。
之前成功例子里 `?` 用在**顶层**(不在函数里)是不合法的。

### 多文件模块(import / export)

一个 `.lume` 文件 = 一个模块:模块有**独立的顶层作用域**,只把显式标记的
绑定导出给入口。适合把策略、计算、数据结构拆到独立文件,多个入口复用。

```lume
// lib.lume —— 库侧:只用 export 前缀标记要导出的东西
export let TAX_RATE = 0.13;
let internal_note = "module-private";        // 未导出:入口不可见

export type Order = { amount: float, zone: string };

export func apply(o: Order): float {
  if (o.zone == "free-trade") { return o.amount * 0.06; }
  return o.amount * TAX_RATE;
}
```

```lume
// app.lume —— 入口侧:import "路径" as 命名空间,再通过 ns.xxx 使用
import "lib.lume" as lib;

let o = { amount: 1000.0, zone: "mainland" };
print(lib.apply(o));      // 130
print(lib.TAX_RATE);      // 0.13
print(lib.internal_note); // 类型错误:module 'lib' has no export 'internal_note'
```

规则:

- **`export` 只修饰顶层 `let` / `func` / `type`**(`export let / export func / export type`)。
  其余顶层绑定是模块私有的,入口读取会在类型检查时报错。
- **`import "相对路径" as ns` 只能出现在顶层**,路径相对**当前文件所在目录**
  解析(自动 realpath 规范化,`a/../b` 会折叠)。一次 `import` 引入一个命名空间,
  命名空间不能重名。
- **每个模块的顶层只执行一次**(依赖图缓存):多个模块 import 同一个库,库的
  顶层不会重复执行。执行顺序是依赖优先,入口模块最后。
- **循环 import 会报错**(`circular import: ... imports itself`),不会死循环。
- **`--check` 与执行走同一条加载链**:`lume --check app.lume` 会递归检查全部
  依赖模块,但不执行。
- 模块顶层就是普通程序顶层,照样可以注册 `route` / `tool` / `server`;
  入口模块的顶层就是整个程序。

可运行的完整示例:`examples/modules/`(tax.lume 库 + app.lume CLI 入口,
`make modules` 运行)、`examples/modules-server.lume`(同库的服务器版,
`make modules-ui` 构建 React 前端并在 :8090 提供税额计算器页面),都已纳入
`make check`。

现有示例也按此拆库:`examples/abac/policy.lume`(ABAC 策略引擎,入口
examples/abac.lume 只剩 HTTP 层)、`examples/invest/ledger.lume`(账本 +
portfolio_* 工具,`tool` 声明在模块顶层照常注册,入口 import 一行即全部就绪)
与 `examples/demo/ui.lume`(SSR 页面组件 cls/nav/card/page,纯函数、不碰
服务器状态,类比前端 React 组件库)。

---

## 内建函数

| 函数 | 作用 |
|---|---|
| `run()` | 启动服务器(阻塞) |
| `print(...)` | 打印到标准输出 |
| `str(x)` | 任意值 → 字符串 |
| `int(x)` | 字符串/数字 → int(`int(null)` = 0) |
| `float(x)` | 字符串/数字 → float |
| `bool(x)` | 任意值 → bool(真值) |
| `string(x)` | `|| str(x)` 的别名 |
| `len(x)` | 字符串长度或列表长度 |
| `keys(m)` | map 的键列表 |
| `get(m, k, 缺省?)` | 取 map 字段;缺键时返回第三个参数(否则 null)。**跨边界取值建议用它**,比如 agent 工具实参 |
| `range(止)` / `range(起, 止)` / `range(起, 止, 步)` | 等差数列列表(整数元素保持 int);步长不能为 0 |
| `map(fn, list)` | 对每个元素调用 fn → 新列表(fn 可传具名 `func`、`func (x) { ... }` lambda 或 `(x) => { ... }` 箭头函数,`filter`/`reduce` 同理) |
| `filter(fn, list)` | 保留 fn(item) 为真的元素 → 新列表 |
| `reduce(fn, list, init)` | 从左折叠:fn(累加值, 元素),返回最终值 |
| `abs(x)` / `sqrt(x)` / `pow(x, y)` / `exp(x)` / `log(x)` / `ln(x)` | 绝对值 / 平方根(负数报错)/ 幂 / 自然指数 / 自然对数(`log` 与 `ln` 是同一个,都注册了)/ 对数(参数须 > 0);入参只接受**数字字面量/数字值**(不接受 `"3"` 这种数字字符串,报 `expected number argument`),返回永远是 float |
| `floor(x)` / `ceil(x)` / `round(x)` | 下取整 / 上取整 / 四舍五入(返回 float,**不会变回 int**) |
| `min(a, …)` / `max(a, …)` | 至少 1 个数字参数,可变参取最小 / 大(返回 float;不是 Comparison 写法) |
| `push(list, item)` | 原地追加元素(不复制),返回**原列表本身**;参数必须正好 2 个且第一个是 list |
| `put(map, key, value)` | 运行时键名写入(第一个参数必须是 map,键必须 string),返回**原 map 本身**;这是 DSL 里唯一能用变量当键的地方 |
| `replace(s, from, to)` | 字符串**全域**替换(不是正则);无匹配 → 原串;`from` 为空串 → 原串 |
| `strftime(fmt, ts)` | 本地时区格式化,形如 `strftime("%Y-%m-%d", 1700000000)` → `2023-11-15`;`fmt` 必须是第一个参数,漏一个就报 `needs a format and a unix timestamp` |
| `try(fn)` | 捕获 `fn`(具名 func / 字面量 / 箭头函数)内部置起的错误,不抛出;返回固定双键结构 `{ ok: 结果, err: null }`(成功)/ `{ ok: null, err: 消息 }`(失败);用 `r.ok` / `r.err` 读(缺键会抛,所以用 `get(r,"ok")` 更稳) |
| `sql_query(sql[, params])` / `sql_query(path, sql[, params])` | 只读 SELECT → 行 map 列表(与 chat 工具同源护栏:单语句、仅 SELECT,库物理只读;缺省用 `SQLITE_DB` 环境变量);`params` 为 `?` 占位符绑定值列表(null 绑定 SQL NULL,值不进入 SQL 文本,杜绝注入) |
| `sql_write(sql[, params])` / `sql_write(path, sql[, params])` | 护栏写:INSERT / UPDATE / DELETE(须带 WHERE)/ CREATE TABLE → 影响行数(DDL 为 0);DROP/ALTER/PRAGMA 等与 portfolio 镜像表一律拒绝;`params` 同 `sql_query`( `?` 占位符绑定,值可含任意 SQL 片段但只当字面值处理) |
| `json(s)` | JSON 字符串 → Lume 值(map/list/标量) |
| `stringify(v)` | Lume 值 → JSON 字符串(JSON 响应、调试用) |
| `now()` | 当前时间戳 |
| `env(k)` | 环境变量值;未设置 → `null`。凭据命名的变量(`*API_KEY`/`*TOKEN`/`*SECRET`/`*PASSWORD`/`*CREDENTIAL` 等,大小写不敏感、词边界匹配)对脚本层脱敏 → `null`(运行时自身读取不受影响);`HTPASSWD_FILE` 这类含 PASSWD 但语义非密钥的配置名不受影响 |
| `files(dir)` | 目录条目列表(排序;目录带尾部 `/`;缺失 → `[]`) |
| `read_file(path)` | 整个文件内容;缺失/不可读 → `null`(上限 16 MiB);`--no-fs` 打开时改为**运行时报错**,见下方「安全边界」 |
| `write_file(path, s)` | 写文件(**原子写**:先写同目录 `.tmp.<pid>` 再 rename,崩溃不会留半截文件);成功 → `true` |
| `mkdir(path)` | 逐级创建目录(0700),已是目录也返回 `true` |
| `lock_file(path, 等待毫秒?)` | flock 排他锁(文件自动创建 0600;进程死自动释放;同进程再次调用会替换旧锁);拿到 → `true`,超时/失败 → `false` |
| `unlock_file()` | 释放当前进程持有的 flock 锁 |
| `tools()` | 已注册工具名列表(本地内建 + DSL `tool` + MCP `<server>/<tool>` + router 代理,排序) |
| `skills()` | 技能索引列表,每项 `{ name, desc }`(整理自 `SKILL.md` 目录,排序) |
| `mcps()` | 读取 `./.data/mcp-servers-router.json` 并解析为列表;文件缺失/无效 → **空列表** `[]`(不是 `null` —— router 同步产物离线时是「还没有」这个正常状态,不是读错误;`null` 会让 `/discovery` 的 `data.mcps.length` 炸掉,`tests/smoke.c` 的 `mcps: missing catalog -> empty list` 钉着这条) |

**取值+转换一步到位**:`str`/`int`/`float`/`bool` 可带 `(map, 键, 默认?)`,等价 `str(get(m,k[,默认]))`,
专为跨边界取数设计(缺键不再炸):

```lume
tool "add", "Add two integers", { a: int, b: int }, (arg) => {   // 参数可写裸类型关键字
  return { sum: arg.a + arg.b };   // 缺 a/b → 0(arg 已按 schema 定型)
};
// 无类型标注的 map 用取值+转换:
// str(arg, "name")     缺键 → 字面 "null"
// str(arg, "name", "")  缺键 → ""
```

**`tool` handler 的 `arg` 自动定型**:工具的 params map 既是给模型的 schema,也是 handler
首参的静态类型。因此声明过的字段可直接 `arg.a` 读取,且**缺键按类型给零值**
(int/float→`0`、string→`""`、bool→`false`),而不是报 `map has no field`。未声明的字段
或未定型的 map(如 `route` 回调的 `req`)仍按老规矩报错,防拼写错误——那里改用
`get()` / `int(m,"k")`。见 `typecheck.c: tool_param_struct` 与 `interp.c: vm_zero_value`。


UI 相关内建(`el` / `render` / `html`)见下面的 **页面开发** 一节。

---

> 编辑器提示：在 VS Code 中 Ctrl+Click 任意内建函数可直达其 C 实现
> （`native_<name>`），Peek 里同时给出签名与行为说明（完整 51 个见
> `editor/lume-vscode/builtins.lume`，由
> `editor/lume-vscode/scripts/gen-builtins.py` 从注册表自动生成；
> 加了新内建只补脚本里的 `SIG`/`IMPL`/`DOC`/`GROUPS` 再重跑，脚本自带
> 「清单与注册表不一致就退出」的对账，不会静默漏掉）。

## 安全边界:解释器不是沙箱

`env()` 会把凭据命名的变量对脚本层脱敏成 `null`,但这**不等于** lume 脚本跑在沙箱里。
文件类内建(`read_file` / `write_file` / `files` / `mkdir` / `lock_file`)对路径**不做任何校验**:
一个不可信脚本照样能 `read_file(".env")` 把刚被脱敏的 key 原样读走,或往任何本进程能写的路径 `write_file`。
现存的护栏(单次 16 MiB 上限、路径长度 < 4096、原子写、文件 0600 / 目录 0700)
都是**可用性**护栏,不是安全边界。

要跑**来源不可信**的脚本,显式打开运行时开关:

```bash
bin/lume --no-fs untrusted.lume            # 或 LUME_NO_FS=1 bin/lume untrusted.lume
bin/lume --no-fs --check untrusted.lume    # 只类型检查:开关对类型无影响,照样通过
```

打开后上述内建在**运行时直接报错**(刻意不返回 `null`:返回 `null` 会让脚本把「被拦下」和「文件不存在」混为一谈)。

| 开关 | 行为 |
| --- | --- |
| `--no-fs` | 命令行开关;`--watch` 重启的服务进程自动继承这个 flag |
| `LUME_NO_FS` | 环境变量等价开关。除 `0` / `off` / `false` / 未设置 / 空值以外**任何取值**都视为开启(宁可锁死也别放宽松) |
| 默认 | **关闭**。产品工具链靠 `write_file` 把设置写进 `frameworks/autogen-pse/.env`、把报告写进 `.data/`,这些路径本来就在 cwd 之外,一开就全断 |

这个开关刻意做成「运行时总闸」而不是「根目录白名单」:白名单要为每个写入点配一条路径,
而产品脚本写入的目录会随部署形态漂移,配错了比不配更危险。

## 页面开发:两套写法,可互相组合

服务端只有 Lume,没有模板文件。两种生成 HTML 的写法:

### 1. `el()` 构建虚拟节点树 + `render()` 序列化

```lume
func card(title, body) {
  return el("div", { class: "card" },
    el("h3", {}, title),
    el("p", {}, body));
}

// 在路由里:
route "GET", "/cards", func(req) {
  return render(
    el("section", {},
      card("A", "first"),
      card("B", "second")));
};
```

`el(tag, props, ...children)`:props 是 map,children 是字符串/vnode/列表。

### 2. `html()` 模板字符串

`html("...{0}...{1}...", a, b)` 把 `{N}` 替换成第 N 个参数。

```lume
func page(title, body_list) {
  return html("<!doctype html><html><body>"
    + "<h1>{0}</h1>"
    + "<main>{1}</main>"
    + "</body></html>", title, body_list);
}
```

槽位规则(默认安全,不必手动转义):

- **字符串/数字/bool**(标量)→ 自动 HTML 转义,用户数据放这里。
- **列表** → 当成一串子节点依次渲染(vnode 结构性渲染、字符串原样输出)。
- **vnode** → 结构性渲染,不转义。
- 想输出字面 `{` / `}` 用 `{{` / `}}`。

### 序列化细节(两端通用,`render()` 与 `html()` 一致)

- 文本与属性值一律 **HTML 转义**(`& < > "`)。
- 事件属性 `on*` 在服务端**不输出**(事件只活在客户端)。
- `data_page` 自动转成 `data-page`(下划线→连字符)。页面入口用
  `data-page='名称'` 告诉客户端挂哪个组件。
- void 标签(`img/link/meta/br` 等)不输出闭合标签;`class: false` 不输出。
- 组件函数惯例上**不写返回类型**(UI 是动态结构)。

### 客户端(可选):真 React

服务端负责 **SSR 壳 + JSON API**,交互交给一个标准 React 客户端
(`frontend/src/invest/app.tsx`,TypeScript 构建前经 `tsc` 检查,esbuild 打成
`www/invest/app.js` 且 React 分包进 `www/chunk-*.js`,Tailwind 生成 `app.css`):

```lume
func counter_page(req) {
  return page("Counter", "counter", [
    html("...<section class='counter' id='counter-root'>"
      + "<big id='count'>{0}</big>"
      + "<button id='inc'>+1</button></section>", str(app_state.count)),
  ]);
}
```

组件通过 `id` 挂进服务端留下的容器;`data-page` 决定挂哪个组件。
修改前端后重新构建一次(`cd frontend && pnpm install && pnpm run build`;
`make ui` 会在缺依赖时自动 `pnpm install`);`node_modules` 在 docroot 之外,
不会被打包伺服出去。

---

## 服务器状态:一条铁律

服务器的执行分两条路径:GET/HEAD 走"主进程快路径",其他(POST 等)走
**工作进程**,每个进程各有一份独立的状态。

所以**可变的共享状态,只能放在 GET 路由上读写**(如 demo 里的 `app_state`
计数器),才能保证读写的是同一份。POST 接口读到的可能是别的工作进程的副本。
需要跨进程、可持久的状态,就接真实的存储,语言这层不提供。

---

## Agent(聊天 + 工具)

浏览器端的聊天由框架原生提供(`POST /react/api/chat`),Lume 层要做的:

### 1. 注册工具(给 LLM 调用的函数)

```lume
tool "greet", "Say hello to a person by their name",
     { name: string, age: int },      // 裸类型关键字;写 "string" 字符串形式也行
     func(arg) {
       // arg 已按 schema 定型:直接取字段;缺键按类型给零值(name→"", age→0)
       return { greeting: "hi " + arg.name + " (age " + str(arg.age) + ")" };
     };
```

- 参数描述 `{ name: string }` 是语法糖,注册时会自动转成完整 schema——
  你只管这么写(等价 `{ name: "string" }`)。
- 工具函数里用 `get(arg, "session")` 能取到**会话 id**(agent 多轮流水的 id)。
- handler 首参 `arg` 的静态类型来自这份 schema,声明过的字段直接 `arg.name` 读即可,
  模型漏传的键按类型给零值(int/float→0、string→"")。要自定义缺省或字段没定型,
  再用 `get()` / `int(arg,"k")` 这类取值+转换。

### 2. 配置模型(`.env`,从运行目录读取)

```bash
cp .env.example .env   # 然后填写:
```

| 环境变量 | 含义 |
|---|---|
| `LLM_API_URL` | OpenAI 兼容的接口地址 |
| `LLM_MODEL` | 模型名(可填 `auto`) |
| `LLM_API_KEY` | **留空 = 离线演示引擎**;填了就走真实模型 |
| `LLM_TIMEOUT` | 超时秒数,默认 60 |

`LLM_API_KEY` 为空时,服务器用内置演示引擎本地生成回答走 SSE 返回
(适合开发、测试、离线演示):

```bash
curl -N -X POST -d '{"message":"hi"}' localhost:8081/react/api/chat
```

`LLM_API_URL` 指到 llm-router(`http://localhost:19070/v1/chat/completions`,
`ROUTER_API_URL` 默认复用)时,伺服器启动会把 router 的目录同步下来:
skills 落到 `skills/router/`、MCP 服务器挂本地、`GET /v1/tools` 目录则
注册成代理工具——调用时向 `POST /v1/tools/invoke` 转发 `{"name","args"}`。
代理工具里 `get(arg,"session")` 无效(记忆隔离在 router 的 key 上)。
与本地内建同名(calc/get_time/fetch_url/skill-run/remember/recall/read_file)
的工具本地实现优先,不走网关往返。

### 3. 聊天页的交互

SSE 事件信封(`data: {"t":...}`):

| 事件 | 含义 |
|---|---|
| `note` | 状态提示(如正在调用工具) |
| `delta` | 流式回答片段,逐段拼接 |
| `error` | 出错 |
| `done` | 结束 |

- 会话 id 只允许 `字母 / 数字 / - _ +`(浏览器端用 `sess-<hex>` 就行)。
- 带着同一个 `sessionId` 再请求,服务器会重放之前的对话作为上下文。
- 跨域 POST 被拒(403);无 Origin 的非浏览器调用(如 curl)放行,方便调试。

聊天页的 SSR 壳只要留一个挂载点(`#chat-root`),真正的聊天组件在
客户端(Agent/New session/Stop 的按钮都在上面)。

---

## 常见坑(写之前先看)

1. **`route`/`tool` 引用的函数必须先定义**:route 在读取到它那行时立即
   注册,引用了还没定义的函数会报 "undefined variable",而且这条路由不会
   被注册——`--check` 抓不到这种错,因为它是运行期错误。写的时候把处理
   函数放在 route/tool 语句**之前**。
2. **端口 < 10000**;别用框架体系里别的端口号。
3. **具名结构体字段必须齐全**:`type P = { x, y }` 时 `return { x: 1 }` 是
   类型错误(缺 `y`)。要省事就别给返回标 `: P`。
4. **严格 bool**:`not 0`、`not ""` 都是类型错误,需要时先转 `bool`。
5. **`?` 只能在函数内**用。
6. **缺字段不报错**:`int(null)` 是 `0`、`str(null)` 是字面 `"null"`(不是空
   串)。缺键/空值的落地行为取决于 `get()` 的第三参缺省值,解析外部数据
   时显式传缺省,不要依赖隐式行为。
7. **类型关键字可当字段名/函数名用**:响应的 `type` 字段、内建 `int()` 都合法。

---

## 测试

`make test`:

1. 类型检查 + 构建。
2. 语言层单测(解析/类型检查/执行/SSR 序列化),逐字节比对输出。
3. 工具注册与 JSON 往返。
4. 起一个真实服务器(:8999)打一遍所有接口:`/hello`、`/sum`、`/echo`、
   首页 UI、`/counter`、`/chat`、静态 css/js、`/api/count`+`/api/inc` 跨请求
   状态、`/react/api/chat` 的 SSE demo。
5. 300 请求的 GC 压测。

测试强制走**离线演示引擎**,不碰外部模型,离线可跑、不花钱。