# Lume 风格约定（STYLE）

> 正向清单：**该怎么写**。`PITFALLS.md` 告诉你「别踩什么」，这里告诉你「踩过的坑之后
> 正确的写法长什么样」。约定全部经过 `--check` 与真实 HTTP 冒烟验证，示例取自
> `lume-crm`（Lume + React CRM，可整份对照）。
>
> 适用版本：含 `try` 固定键 / 箭头表达式体 / 字符串键 map / `push` / `spa` 的二进制
> （git log 自 `39ac355` 起；`make` 重新构建后生效）。

---

## 1. 回调一律箭头函数，单表达式用表达式体

`filter` / `map` / `reduce` / `try` 的回调，凡是**单表达式**就写成一行（`5b829f8` 起
箭头支持表达式体，`=> x * 2` 等价 `=> { return x * 2; }`）。块体只留给多语句：

```lume
// 好:一行,意图即筛选条件
let hit = filter((c) => c.name == q or c.company == q, all);
let recs = map((k) => customer_row(d, int(k)), keys(cs));

// 差:单条 return 也开块体,纯噪音
let hit = filter((c) => {
  return c.name == q or c.company == q;
}, all);

// 多语句才用块体
let my_deals = filter((k) => {
  let de = get(deals, k, {});
  return de.customer == cid and de.stage != "成交";
}, keys(deals));
```

`=>` 后是 `{` 一律按块体解析；表达式体要返回 map 字面量时用分组
`=> ({ a: 1 })`。

## 2. `try` 捕获解析/IO 错误：闭包捕获 + 固定键成员访问

`try(func)` 只有**一个参数、内部以 0 参调用**它——输入靠闭包捕获，
`try((s) => json(s), body)` 这种写法不成立：

```lume
// 路由里接住 json() 的 sticky error(不套 try 时 500,套了变 400)
let r = try(() => json(req.body));
if (r.err != null) {
  return { status: 400, body: { err: "body 不是合法 JSON: " + r.err } };
}
let body = r.ok;
```

固定键保证（`5b829f8`）：成功 `{ ok: 值, err: null }`、失败
`{ ok: null, err: 消息 }`，两键恒在，`r.ok` / `r.err` 直接成员访问安全。
对外部来源（JSON、文件）仍建议用宽容的 `get(m, "k", 默认)` 读嵌套值。

## 3. 键是固定字面量 → map 字面量；键是运行时值 → `put`

字符串字面量键已合法（`53729b7`），固定形态的记录和种子数据直接写字面量：

```lume
func cust_m(id, name, company, email, phone, t) {
  return {
    id: id, name: name, company: company,
    email: email, phone: phone, created: t,
  };
}

func seed_data() {
  let t = now();
  return {
    next_cust: 3,
    customers: {
      "1": cust_m(1, "林晓", "蓝湾科技", "linxiao@lanwan.dev", "13800000001", t),
      "2": cust_m(2, "陈默", "山海资本", "chenmo@shancai.vc", "13800000002", t),
    },
    deals: { ... },
  };
}
```

键是 `str(cid)`、`de.stage + ".amt"` 这类**运行时字符串**时，字面量盖不住，
老老实实 `put`——但被 put 的表要先取出来（见 §4）：

```lume
put(crm_table(d, "customers"), str(cid), cust_m(...));
```

## 4. 数据建模：map 做骨干，读侧现造列表

- 实体表存 `id(字符串) → 记录`，`json()` round-trip 回来的键天然是字符串，
  写键一律 `str(int)` 归一，读写两侧同型。
- 要「列表」时现造：`map((k) => customer_row(d, int(k)), keys(cs))`。
- 新增 `push(list, item)`（`39ac355`）可用于构建期列表；
  但实体存储仍推荐 map（O(1) 按键读写 + 键即 id），列表只做展示层现造。

## 5. 写路径纪律（invest / lume-crm 同款三条）

1. **锁**：读-改-写全程持 `lock_file(path, 3000)`，拿不到 → 409 / err 返回；
   锁文件的**父目录要自己 `mkdir`**（`lock_file` 只建锁文件本身）。
2. **原子写 + 失败检查**：`save_crm` 包一层 `write_file` 并返回其结果，
   调用方 `if (not save_crm(d)) → 500/err`——原子写只保证不烂尾，不保证成功。
3. **计数器自愈**：`next_*` 不直接取用，load 后抬到 `max(现值, 表内最大 id + 1)`，
   手工改过账本也不会发重号。

## 6. 命名：变量与函数区分开

同名覆盖已被 `--check` 拦（`dbbcd2b`），但**别依赖它兜底**：
`let crm_lock`（路径）+ `func crm_lock()`（锁函数）是真实踩过的事故形态
（编译全绿、只在 fork worker 的写路径炸）。约定：路径/常量变量带
`_path` / `_file` 后缀，函数名保留干净（`crm_lock_path` vs `crm_lock()`）。
for-in 的循环变量也不限作用域（同函数内两次 `for (k in ...)` 会撞
"duplicate declaration"），多循环各取各名（`kc/kd/ka`）。

## 7. SPA 前端：`spa = true` + 单壳 + history 路由

```lume
server {
  port = 8089;
  workers = 2;
  bind = "127.0.0.1";
  docroot = "./www";
  spa = true;     // GET + Accept:text/html + 静态 404 → 回退 docroot/index.html
};
```

- 不要 `views` 子目录壳：回退目标固定是 docroot 根的 `index.html`。
- 前端用 `pushState` + 全局 click 拦截做客户端导航（干净 URL、无白屏），
  浏览器前进/后退监听 `popstate`。回退条件三者同时满足，
  **API 的 404 与 fetch（无 Accept: text/html）不受影响**——不要给 API 打 fallback。
- 曾被迫用的 hash 路由（`#/customers/3` + 壳脚本归一化）在新特性下退役，
  旧项目迁移：删 hash、`location.hash` → `location.pathname`，壳合并成一个。

## 8. 路由 handler 的错误返回约定

```lume
return { status: 404, body: { err: "客户 " + str(id) + " 不存在" } };
```

状态码 + `{ err }` 字符串，前端 fetch 层统一读 `err` 抛错；
成功路径可带 `{ status: 201, body: { ok: true, id } }`。
注意 `return { ok: 1 }` 这种**单键 map 会被类型检查器识别为 Result**，
要普通 map 就带上第二个键或改用 `put`。

## 9. 快速自查清单（提交前过一遍）

- [ ] 单表达式回调都写成表达式体箭头？
- [ ] `json(req.body)` / 文件解析有 `try(() => ...)` 包着？
- [ ] 固定键记录是 map 字面量，动态键才 `put`？
- [ ] 被 `put` 的表经过保证存在的助手（缺键补空表）？
- [ ] `write_file` 的返回值有人检查？
- [ ] 变量/函数无同名，循环变量不重名？
- [ ] 前端是 history + `spa = true`（不再是 hash + 多壳）？
