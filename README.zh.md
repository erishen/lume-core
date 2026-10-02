# lume-llvm

把 [Lume](https://github.com/erishen) 语言编译到原生机器码的研究型编译器：
**`.lume` → AST → LLVM IR（文本）→ 原生可执行文件**。

```
examples/hello.lume
        │
        │   vendored frontend（词法/语法/类型检查）        upstream/
        ▼
      AST（Node* / Type*）
        │
        │   手写的后端                                     src/codegen.c
        ▼
      LLVM IR 文本（.ll）
        │
        │   clang / cc                                     src/main.c
        ▼
      ./out/hello（可直接运行）
```

前端不是重写的：`upstream/` 放的是 Lume 真实的词法、语法与类型检查器，只是把它
从 Lume 运行时里剥了出来（原本 `lume.h` 里唯一的耦合就是那句
`#include "agenthttpd.h"`，已经删掉）。所以语法错误和类型错误都是上游编译器
报的，不是我们再实现一套。

## 构建与运行

宿主机工具链只要 **C11 + libc** —— 不需要 LLVM 开发包，也不需要 CMake：

```sh
make                                    # -> bin/lume-llvm
make check                              # 对每个示例做类型检查
make test                               # 编译+运行每个示例，并比对输出
make examples                           # 编译+运行每个示例，打印输出
make dump f=examples/fact.lume          # 打印 AST
make clean
```

单个文件：

```sh
./bin/lume-llvm --compile examples/fact.lume && ./out/fact
./bin/lume-llvm --emit-ir  examples/fact.lume  # 只生成 out/fact.ll，不链接
./bin/lume-llvm --check    examples/fact.lume  # 只做解析 + 类型检查
```

`make test` 会把每个示例的 stdout 和 `tests/<stem>.expected` 做 diff，所以后端
一旦回归（GEP 下标写错、`alloca` 漏插、类型提升没做），看到的是一条 diff，而不是
一个悄悄算错的数。

## 为什么 IR 是文本而不是调 LLVM C API

* **零链接依赖。** 为了「其实就是在拼字符串」这件事去链一个巨大的 libLLVM 不值。
  `clang` 每台开发机都有，而且能直接吃 `.ll` 文件。
* **IR 保持可读**——研究型编译器最要紧的就是这个，每个阶段都能停下来看中间产物，
  `.ll` 可以直接读明白某个语法结构降成了什么。
* **可替换。** 哪天真要换成 MLIR 或 LLVM 官方 IRBuilder，只需要换掉
  `src/irbuf.c` 这一个文件。

代价是 IR 的正确性只有 clang 会校验，所以后端**绝不吐半成品模块**：凡是降不下去的
构造，直接报一个能定位的错误
（`line 26: for (this construct) is not supported by the native backend yet`），
而不是让 clang 对着一段残缺 IR 报一句莫名其妙的 opcode 错误。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `src/main.c` | 驱动：命令行、解析、类型检查、IR → 原生二进制 |
| `src/codegen.c` | AST → LLVM IR 文本（后端本体） |
| `src/irbuf.c` | 可增长文本缓冲 + IR 字符串字面量转义 |
| `src/rt.c` | 生成的 IR 调用的运行时 helper（`print`） |
| `upstream/` | vendored 的 Lume 前端（token/lexer/parser/typecheck） |
| `examples/` | `.lume` 示例程序 |
| `tests/` | 每个示例的期望输出，`make test` 用 |

## 当前支持的子集

类型：`int`、`float`、`bool`、`string`，以及 `type Name = { f: T, ... }`。

* 顶层 `func`、递归、多参数、有返回类型
* `let`（带标注或不带）、赋值、成员赋值
* `if` / `else`、`while`、C 风格 `for`、`break`、`continue`
* 数字（int/float）、布尔、字符串字面量
* `+ - * / %`（int）与 `+ - * /`（float）、`== != < <= > >=`、`! -`
* `&& ||` —— 直接降成 `and`/`or` 位运算，**是急切求值、不短路**（语法分析阶段
  两侧操作数就已经求完值了）。研究子集够用，要真短路得改成分支。
* 字段访问与结构体值（`{ w: 3, h: 4 }`、返回结构体的函数）
* 字符串不参与算术（字符串算术是报错，不是崩）

暂不支持、但会给出可定位错误的：闭包 / 函数字面量、列表字面量、`import`，以及
Lume 只为 HTTP server 保留的那几个（`server`、`route`、`tool`、`verbs`）。

## 后端笔记（有意思的部分）

局部变量用 `alloca` + `load`/`store`，而不是 SSA + `phi`，这样发射器写起来极简；
`clang -O2` 会跑 mem2reg，自动把它变成正经 SSA。IR 是按源码顺序往一个缓冲里追加
的，所以每个函数体在发射 entry block 之前会先整体扫一遍，把 `alloca` 全部提到
entry。

几个真正踩过坑、且很容易写错的点：

* **在 macOS/arm64 上，手写 IR 不能调 `printf`。** 变参的参数保存区必须由**调用方**
  搭好，而 clang 不会替手写调用搭一个。所以 `print` 走 `src/rt.c` 里的非变参
  helper（`lume_print_i64`、`lume_print_double`、`lume_print_bool`、
  `lume_print_str`），IR 里只发普通非变参 call。
* **`.ll` 必须声明 `target triple`。** 不写的话 LLVM 按通用目标降级，ABI 对不上，
  打印出来是一堆垃圾字符。triple 取自宿主机工具链（`$(CC) -print-target-triple`），
  同时也会传给 C 编译器，保证运行时目标文件和 IR 用的是同一个 triple。
* **结构体字段访问是「双下标 GEP」**：
  `getelementptr inbounds %Rect, %Rect* %obj, i32 0, i32 <字段>`。只写一个下标
  被当成**数组**下标，会静默地读到 offset 16 上的错误字段。
* **具名结构体类型有两套拼写**（值用 `%Rect`、指针用 `%Rect*`），而且必须 intern
  保存：把它们写进同一个临时缓冲区，只要同一次格式化里同时要值类型和指针类型就会
  互相覆盖。
* **读一个结构体变量拿到的是它的槽位指针**；结构体形参本身就是指针；结构体字面量
  是 `insertvalue` 链；返回结构体又不能写成 `ret %Rect 0`。

## 路线图

* 函数字面量 / 闭包、列表字面量
* 覆盖前端更多语法（`import`、高阶调用）
* 交给 clang 之前先在 `.ll` 上跑一层 `--opt`
* IR 形态稳定后，从文本发射切到 LLVM C API
