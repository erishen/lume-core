# Lume 原生后端(实验)

`--compile` 给 Lume 加了第三条执行路径:不再是「解释器边跑边解释」,而是把
程序编译成一个真正的机器码可执行文件。

```
.lume ──► lexer / parser / typecheck (本仓库自有)
       ──► codegen*.c: 手写 LLVM IR 文本 (.ll)
       ──► clang/cc: 汇编 + 链接
       ──► 可执行文件(链接 src/rt.c 编出来的 runtime helper)
```

```bash
make native                                  # 编译 + 跑 + 比对期望输出
make && bin/lume-core --compile examples/native-fact.lume && ./out/native-fact
bin/lume-core --compile script.lume -o mybin      # 指定输出名
bin/lume-core --compile script.lume               # 默认 out/<文件名>,IR 落在 out/<文件名>.ll
```

## 宿主机工具链要什么

分三档(默认后端是 libLLVM,所以「跑 `make native`」和「跑 `make
native-text`」的依赖不一样):

| 你跑什么 | 需要什么 |
|---|---|
| 解释器 / `make dev` / `--check` | 一个 C11 编译器 + libc(就是 `cc`),别的都不用 |
| `--compile`(默认 = libLLVM 后端) | LLVM **开发包**:`llvm-config` 在 PATH 上或在 `/opt/homebrew/opt/llvm/bin/`。Makefile 找到它才多编 `llvm_codegen.c` + `backend_llvm.c` 并链 `-lLLVM`;找不到就编译出一个**没有 libLLVM 的 `bin/lume-core`**,`--compile` 自动落到文本路(stderr 会 note 一行)。**注意** `llvm-config` 的路径非空不等于它能用(brew 升级会把 `opt/` 下的符号链接换掉),所以 Makefile 真的去问 `--version`;问不出来就当没有,而不是让 `llvm-c/Core.h` 那一记 fatal error 冒出来 |
| `--compile-text` | 上面那个 `cc` **再加运行时 PATH 里的 `clang`** —— IR 文本是吐给**子进程**的(`src/backend.c` 的 `pick_cc()` 依次取 `$CC` / `clang` / `cc`),所以构建图里不进任何 LLVM 头文件与库 |

所以「宿主机只要 C11 + libc」这句话的精确版是:**编 `bin/lume-core` 和跑解释器
不需要 LLVM 开发包**。默认 `--compile` 会被 libLLVM 接管,那确实要装开发包
(程序里要**调 LLVM 的 API**、链 dylib);退到 `--compile-text` 只需要一个现成
的 clang 命令行,而 clang 内部虽然是 LLVM,但那是子进程的事,不进 lume 的
依赖图。想让构建完全不碰 LLVM,`make clean && make all` 时把 llvm-config 藏
起来即可(或者 `make native-text`,产物照样跑)。

也没有 CMake 什么事。lume 只有一条 Makefile、约 30 个固定翻译单元、一个产物
(`bin/lume-core`),依赖**只有 libc / `-lm`(Linux 加 `-lcrypt`)**,外加 PATH 上的
`llvm-config`(找到才多编 libLLVM 那路)和文本路收尾用的 `clang`。没有
`-lsqlite3`、没有 `libagenthttpd.a`、没有 git submodule——本树是脱离宿主的
独立语言树,链进来的只有 libc 和(可选)libLLVM。CMake 是 **LLVM 自己的**构建
系统(几十个子项目、几百个开关);lume 只消费预编译好的 LLVM,从来不构建
LLVM 源码。

## 默认后端:`--compile-llvm`(链 libLLVM)

手写 IR 只是**把 LLVM 的 IR 语法自己敲出来**——IR 长什么样、块有没有终结、
类型对不对,全是 `codegen*.c` 自己保证(拼写、alloca、终结符都在这几个文件里),
错了要到 clang 那步才炸。所以加了
一条走 libLLVM **C API** 的路(现在是 `--compile` 的默认):IR 以 **value**
形态拼出来,由 LLVM 在构造时就校验(`LLVMVerifyModule`),最后由
`LLVMTargetMachineEmitToFile` 自己出目标文件。

```
.lume ──► lexer / parser / typecheck (与文本后端同一套)
       ──► llvm_codegen.c: 用 LLVMBuild* 造出 module(LLVMVerifyModule 自检)
       ──► LLVMTargetMachineEmitToFile → .o(libLLVM 做的 codegen)
       ──► clang/cc 只负责链接
```

```bash
make native-llvm                             # 同一份期望基线的第二条回归
bin/lume-core --compile-llvm examples/native-fact.lume
```

两个后端**同时编进一个 `bin/lume-core`**:Makefile 找得到 `llvm-config` 就多编
`src/llvm_codegen.c` + `src/backend_llvm.c` 并链 `-lLLVM-23`,找不到就当它
不存在(所以这个后端是**可选依赖**,不是硬要求 —— 那时 `--compile` 默认就走
文本路)。

一个构建没编进 libLLVM 时,**显式 `--compile-llvm` 会硬错退出**(exit 1,提示
改用 `--compile-text` 或带 `LLVM_CONFIG=` 重建),不会悄悄掉到文本后端:悄悄
改道比明着失败更难查。

## 两个后端的一致性:`make native-consistency`

两套 IR 发射器(`codegen.c` 手敲文本 / `llvm_codegen.c` 调 C API)是**语义
等价但代码不相干**的两份实现,分叉只会以「某天某个例子给出错答案」的形式冒出
来 —— `mul i64 %v1, undef` 和 struct 字段那次 GEP 都是这么漏的。所以:

```bash
make native-consistency     # 或者它已经挂在 make test 上
./tests/native_backends.sh
```

`tests/native-backends.sh` 拿 `tests/native-consistency.lume` 跑三条路(解释器 /
`--compile-text` / `--compile-llvm`),各自跟 `tests/native-consistency.expected`
比、再**两两互比** —— 解释器是「语言本来该干什么」的参照,所以任一路跟它不一致
都算失败(早期版本只有两个 emitters 互比,解释器那条腿其实没跑到,「三路」是虚
的)。并且**编译器只要往 stderr 打任何东西就算失败** —— 被 clang 或 LLVM 校验器
抱怨的 IR,不算「编译通过」。解释器那条路会在 stderr 留一行
`note: script completed without run()`(关于入口约定的提示,与夹具无关),这一行
在比对前丢掉,其余 stderr 内容照旧算失败。缺 clang 或缺 libLLVM 时那一路 skip,
不让整条套件变红。

这个夹具是**故意窄的**(整型 + 显式 `let` 类型 + 结构体字段:函数/if/while/
for/递归/比较/结构体传参/结构体返回/字段读/字段写/字面量取字段/顶层语句)。
目前两边的覆盖面都还窄,而且已经不是完全重合:

| 构造 | 文本后端 | libLLVM 后端 |
| --- | --- | --- |
| 整型算术、比较、`if`/`while`/`for`、递归 | ✅ | ✅ |
| 结构体字段读写(局部变量 / 参数 / 传参 / 返回 / 字面量 / 调用结果) | ✅ | ✅ |
| list / map 字面量、`.len`、len/push/get/put/keys | ✅ | ✅ |

> 表里每一格都是拿脚本在三条路上各跑一遍得到的,不是按代码里写了什么估的。
> 上面 float 算术 / 字符串 `+` / bool 那一行曾经标成 ❌❌,实测三路都通 ——
> 照着实现清单写表会把已经补上的东西记成「不支持」。`tests/native-consistency.lume`
> 的价值就在于这类格子的判据只有夹具能给出。
| `for (x in xs)`(list 按元素 / map 按键) | ✅ | ✅ |
| `and` / `or` 短路(右操作数根本不发射) | ✅ | ✅ |
| `/` 在整型上也按浮点除 | ✅ | ✅ |
| 字符串 `.len` / `.length` | ✅ | ✅(2026-10-03 补) |
| float 算术 / 字符串 `+` / bool 比较 | ✅ | ✅ |
| `let` 不带显式类型、函数参数 / 返回值不标注(由初始化式与调用点推断) | ✅(2026-10-03 补) | ✅(同上) |
| `int → float` 返回时的拓宽(`return 1;` / `return y;` 落进 float 槽) | ✅(2026-10-03 补) | ✅ |
| 同一个函数被两个调用点推出两种参数类型(如先 `f(1)` 再 `f("s")`) | ✅ 报源码行号 | ✅ 同一行、同一句 |

「三行 ❌ 是两后端都不行、✅ 那格只有解释器能跑」的日子到 2026-10-03 为止了:
那三行补齐之后,这张表现在**没有 ❌**,剩下的每行都是三路同答。诊断那两列留着
是为了让「同因不同报」露出来 —— 同一个「参数没有 codegen 类型」,文本后端指名
道姓(`parameter 'x'`),libLLVM 只说 print 收到个打印不了的值,那是**把定义
阶段的问题报在调用点上**;推断 pass 出来之后,两种报法都换成了指名 call site
的那一条(见最后一行)。

/ 那一行是 2026-10-03 补的。此前两个后端都发 `sdiv`,解释器答 3.5 —— 这是一条
**错答案**而不是报错,所以一致性格局之外没有任何东西会拦它。修法是把两个操作数
`sitofp` 成 double 再 `fdiv`,结果类型跟着变 float。

`and` / `or` 那一行也是这次补进去的:短程路径必须直接**存常量**,而不是「把分支
目标换一下」—— 否则 `true or 1/0` 会答成右操作数的值。而且必须是**三块**
(short / long / join):两块写法会让 join 块被发射两次,clang 直接拒
(`Terminator found in the middle of a basic block`)。

结构体这一行是 2026-10-02 修掉的,修之前 libLLVM 后端**整体判为坏掉了**。三个
坑,按发现顺序:

1. **GEP 从指针反推结构体类型,在 LLVM 23 上必然拿错。** 原来
   `gep_field()` 用 `LLVMGetElementType(LLVMTypeOf(ptr))` 去问指针指的是什么
   —— 但 LLVM 23 是不透明指针,`LLVMTypeOf(ptr)` 打印就是 `ptr`,
   `LLVMGetElementType(ptr)` 返回的是 `half`(不是 `%P`),于是 IR 变成
   `getelementptr inbounds nuw half, ptr %0, i32 0, i32 0`,校验器直接拒
   (`Invalid indices for GEP pointer type!`)。改法:结构体类型由调用方显式传
   进来(`LLVMBuildStructGEP2` 本来就要第二参数)。
2. **结构体值的「物化」缺一环。** 结构体在 lume 里**总是按引用传递**(变量就是
   它的槽位),但 `f()` 返回结构体、`{…}` 字面量这类值本身是 `insertvalue`
   链,**没有地址**;直接拿去做 GEP 就是退化成「指针里存着结构体」。修法是一个
   `struct_addr()`:需要地址时先 `alloca` + `store`。文本后端没有类型信息可查,
   `Val` 里因此加了 `agg` 标记(只有两处产出聚合值:结构体字面量、返回结构体的
   调用);libLLVM 后端用 `LLVMTypeOf(v)` 判断是不是 `ptr` 就够。
3. **(更根本,和结构体无关)** 脚本里只要**有一条顶层语句**(`run();`、
   `let x: int = f();`),libLLVM 后端就段错误。栈在
   `llvm_codegen_module` 给 C entry 发射 `call void @L_top()` 那一行:
   `LLVMBuildCall2` 的**末位名字参数传 `NULL` 会崩**。三种取值实测(最小 C 探
   针,`LLVM 23.1`):
   `"c"` → 校验器报 `Instruction has a name, but provides a void value!`;
   `NULL` → SIGSEGV(C API 把名字当 `Twine` 构造后解引用空指针);
   `""` → **校验通过**。所以 void 调用的名字必须传空串。同一个坑在 `cg_call`
   里还有第二处:调用返回 void 的用户函数,名字同样要传 `""` 而不是 `"c"`。

`src/llvm_codegen.c` 的 `gep_field()` 与 `struct_addr()` 上方留了结论注释,免得
下一个人再踩一遍。加新覆盖范围时注意:先让一个后端跑对,再加进夹具,否则套件
常红就等于没信号。

libLLVM 顺手解决掉了文本后端最坑的两件事:

- **triple 不用管了**。`LLVMGetDefaultTargetTriple()` 给值、libLLVM 自己
  选目标,没有 `-DTARGET_TRIPLE`、也没有 clang 那步的
  `-Wno-override-module` 两阶段拆分。
- **struct 取字段不会读错位**。C API 的 `LLVMBuildStructGEP` 单下标就行,
  文本那边的 `getelementptr inbounds %Rect, %Rect* %r, i32 0, i32 f`
  双下标漏掉 `0` 会静默读到 offset 16 的错字段。(LLVM 23 是不透明指针,结构体
  类型必须由调用方显式传进来 —— 想从指针反推
  `LLVMGetElementType(LLVMTypeOf(ptr))` 只会拿到 `half`。)

而文本后端靠「函数尾补一个 `ret`/`unreachable`」换个合法的做法,在这里换成
`LLVMGetBasicBlockTerminator` 判断是否已终结 —— 这才是 `if (c) { return 1; }`
能合法的原因。

已知代价:

- **优化 pass 是 2026-10-03 才打通的。**此前记的代价是「`LLVMRunPasses` 从纯 C 的 TU 里调会崩
  (opt level `None` 时 `verify` / `mem2reg` / `instcombine` / `default<O2>` 都是 SIGSEGV;
  opt level `Default` 时六种 code model 一律 `LLVM ERROR: tiny code model is only supported on
  ELF`)」,于是 alloca 重的 IR 直接被 codegen、没走 mem2reg,产物**正确但慢一两个数量级**。
  这个结论**前提错了两次**,当天重测就推翻了:
  1. 入口在 `llvm-c/Transforms/PassBuilder.h`,而这个文件 `backend_llvm.c` **本来就 include 了**。
     当初那个独立探针没拉它进来,`LLVMRunPasses` 于是被编译成**隐式声明返回 `int`**,再被塞进
     `LLVMErrorRef`(一个指针)⇒ 段错误。**和「C-only TU」没有任何关系。**
  2. opt level 用 `Default`(也就是 `make_target_machine` 里定的
     `LLVMCodeGenLevelDefault`)时,`verify` / `mem2reg` / `default<O2>` **都正常返回成功**。
     **会死的是 `None`**,不是 `Default`。那句 `tiny code model` 是 opt level `None` 那一档的
     顺带产物,被误记成了「Default 档六种 code model 都不行」。
  现在 libLLVM 这条路在 emit 前跑 `default<O2>`,和文本路交给 clang 的 `-O2` 同一档。
  🔴以后再看到「C API 跑不了 pass」这类结论,先确认头文件 include 了**再**下判断 ——
  隐式声明不报错(C 里只是个 warning),崩了也说不清是谁的错。
- `bin/lume-core` 从此依赖 libLLVM 的 dylib(二进制本身不大,链接时拉 23 的
  公共库)。

## 两条路的横向对比

靶子是 `examples/native-bench.lume`:一个双层循环,外层 4 个局部、内层 2 个
局部,总迭代 1e8。内层套一层是为了让**返回值有界** —— 外层局部让结果按
~n³ 涨,裸跑到 1e8 就溢出 i64 了,而内存往返的差异要到迭代量很大时才现形。

```bash
make native-bench
```

```
backend   compile-ms   ir-KiB  obj-KiB  bin-KiB    run-ms  nopass-ms
text            446        3        1       35         9          -
llvm            184        2        1       35         9        215
```

(这张表 2026-10-03 重取过多轮。改之前 llvm 那列是 220ms —— 见下一段;现在两条路
的运行侧落在同一档,差异在噪声里。)

`run-ms` 那一列留着上一段那段误判的疤痕:**mem2reg 没跑的时候,同一个程序慢
一到两个数量级**(多轮实测 5ms→220ms、7ms→220ms、9ms→423ms,取最短的一批)。
局部变量本来是 alloca 槽,每次内层迭代都是一次真实的 load/store;promote
成寄存器之后这 1e8 次往返全部消失 —— 上表 llvm 那列从 220ms 掉到 9ms,就是
2026-10-03 把 `default<O2>` 接上之后的结果。

**注意 `run-ms` 现在只能说明一件事:「两边的优化 pass 都跑上了」。**靶子是纯
算术循环,`default<O2>`/clang `-O2` 会把它整段折叠成向量算术(〈2 x i64〉、
`mul i64 %n, 1000`),1e8 次迭代在 IR 层就没了,剩下的只是测「编译器干活了
没有」,测不出两条 codegen 路谁更好。要验证「pass 真的生效」,看最后一列
`nopass-ms`:`--no-pass`(等价的环境变量 `LUME_NO_PASS=1`)跳过 pipeline 再编
一次同一份源码,llvm 腿从
9ms 变 215ms(实测 6ms→219ms、9ms→215ms)。**这一列才是哨兵** —— 哪天
pipeline 悄悄失效(LLVM 换版本、pass 串写错、被哪次改动删掉),`run-ms` 会
照旧报「很快」,而 `nopass-ms` 会先跳起来。

`compile-ms` 这一列是**当前唯一稳定偏向 libLLVM 的地方**:文本路的产出是 IR
文本,要再交给 clang 走一遍 parse + 优化 + codegen,libLLVM 路在自己进程里
就做完了 ISel 和 asm printer,clang 只在最后链接那一步出现。多轮实测
llvm 184–264ms / text 355–446ms,libLLVM 快一到两倍半。体积列两边一样小。

两个后端都能过同一份 `tests/native-bench.expected`,输出逐字节一致;体积
列也几乎一样(libLLVM 的 codegen 和 clang 出来的 .o 一样小)。所以这条对比
现在是**运行侧打平、编译侧 libLLVM 领先、正确性完全打平**。

于是「哪条默认」的取舍少了一半 —— 速度已经不是 libLLVM 的减分项,默认路
(`--compile` = libLLVM)剩下的收益只有「IR 形状由 LLVM 自己把关」这一条。
详见下面「这个默认值的决定」那一节,以及为什么这条决定在 2026-10-03 被重开
过一次又维持原样。

## 哪条是默认

`--compile` 走的是**默认后端**,而默认后端 = **有 libLLVM 就用 libLLVM**:

```bash
bin/lume-core --compile foo.lume          # libLLVM(本机的 bin/lume-core 编进 libLLVM 时)
bin/lume-core --compile-llvm foo.lume     # 同上;显式写出来也是它
bin/lume-core --compile-text foo.lume     # 强制文本路(clang -O2 带 mem2reg)
```

判据就在 `src/main.c`:`HAVE_LIBLLVM` 编进去时 `use_llvm = (native_choice !=
NAT_TEXT)`,否则一律文本后端并在 stderr 打一行 note。make 侧的对应目标:
`make native`(默认)、`make native-llvm`(显式 libLLVM)、`make native-text`
(显式文本),三个目标比对的是**同一份** `tests/native-fact.expected`。

所以「默认选 libLLVM」的实际取舍是:**代价**=这条腿要宿主从 LLVM 开发包,
没有就自动退回文本路(那条腿也测不到了),**收益**=①IR 是 LLVM 自己建出来的,
`LLVMVerifyModule` 在生成点就报错,不用等到 clang 那一步才发现块没终结、类型
不匹配;②编译更快(bench 的 `compile-ms`:libLLVM 184–264ms / 文本
355–446ms)。运行侧已经打平,不再是 libLLVM 的短板。

### 这个默认值的决定(2026-10-03)

评估过「默认改成 `--compile-text`」这一项,结论是**不动**,默认仍是 libLLVM。
当时的三个候选与落点:

- **默认切文本** —— 当时唯一的论据是「运行快一个数量级」(bench 6ms vs
  211ms,差距全在运行侧),而且 clang 本来就是两条路的共同依赖(文本路自己就
  `clang -O2` 收尾),「会多一个依赖」不成立。真正的代价是 libLLVM 退化成
  显式需求,并且 `make native` 要改成显式带 `--compile-llvm` 才能继续测到
  libLLVM 那条腿。**这条候选在 2026-10-03 接通 `default<O2>` 之后已经无论据了**
  ——运行侧打平、编译侧反而是 libLLVM 更快,所以它被放过的这一次其实比当时
  的 record 更稳(见上一节)。
- **保持 libLLVM + 那行 stderr 提示(采纳)** —— 默认路径顺带证明「可选依赖
  在工作」,`make native` 仍跑 llvm 腿;那句提示由 `src/main.c` 的
  `native_choice == NAT_DEFAULT` 打,说 libLLVM 路跑不了优化 pass。
  **2026-10-03 同一下午:pipeline 接通了,这句提示的前提没了,已删。**代价改成
  「按可选依赖算」,收益改成编译更快。
- **按脚本启发式自动选** —— 行为不透明、同一份脚本在不同机器可能落到不同
  后端,文档和一致性测试都要重写。不采纳。

以后要重开这个议题,先重跑 `make native-bench` 取数,别直接引用上面的表。

## 为什么 IR 是文本,不是 libLLVM

`codegen*.c` 拼的是 LLVM IR 的**文本**,再由 clang 收尾 —— 文本路(`--compile-text`)
是「不链 libLLVM 时的那条路」:代价是 IR 合法性得自己保证,收益是 `bin/lume-core`
在任何机器上都能零 LLVM 依赖编出来。默认路(`--compile`)走 libLLVM C API,
所以这条不是主路了,但它解释了不少只有手写 IR 才会踩的点。

几个因此必须自己拿捏的点:

- **必须写 `target triple`**。不写的话 LLVM 按通用目标降级、选错 ABI,
  表现不是报错而是 `printf` 打印乱码。triple 由 Makefile 在编译期从宿主
  工具链取(`cc -print-target-triple`)注入。
- **局部变量用 `alloca` + `store/load`,不做 SSA**。IR 按源码顺序往一个
  buffer 里追加,所以每个函数体发射前要先扫一遍变量名、把 `alloca` 全放进
  entry block;`clang -O2` 会跑 mem2reg 把它们还原成正经 SSA。
- 那遍「扫变量名」必须能进**嵌套块**。`scan_stmt` 的 if/while/for 分支
  往下递的是 **block 节点**,`N_BLOCK` 一开始没有 case,直接落到 `default`
  被吞掉 —— 结果 `while` 体内 `let` 声明的局部从来没进过 locals 表,读它是
  `bad operands`、写它是 `unknown variable`。两个后端同这个 bug,已修。
- **不支持的写法直接报错**,绝不吐残缺的 IR —— 否则错误会伪装成 clang
  报的莫名 opcode,指不到真实的语言构造上。
- **字符串字面量要剥引号**、`\XX` 是**一个字节**而不是三个字面字符,
  所以 `[N x i8]` 的尺寸按解码后的字节数算。

## 为什么 print 不调 printf

macOS/arm64 的 `va_start` 读的参数保存区**由调用方搭建**:C 编译器发射的
调用方序列里有 `str x8, [sp]`。手写 IR 即使把 printf 声明成非变参,也补不
出这段保存区,结果就是打印乱码(值留在 x1,callee 去栈上重读)。

所以 print 走 `src/rt.c` 里**由 C 编译出来**的非变参 helper:

```c
long lume_print_i64(long);
long lume_print_double(double);
long lume_print_bool(long);
long lume_print_str(const char *);
```

IR 只发普通非变参 call,参数走普通整数/浮点寄存器,ABI 天然正确。

## list / map 在原生后端是什么

原生后端既没有 GC 也没有解释器的 `Value` 布局,所以 list 和 map 不做成值,而做成
**堆对象 + 不透明指针**:`llvm_type_of(TY_LIST)` 和匿名结构体(就是 map)都返回
`i8*`,由 `lume_list_new()` / `lume_map_new()` 分配出来。元素是一个标记槽:

```c
typedef struct { long num; const char *str; int tag; } LumeSlot;  /* tag: INT/FLOAT/STR */
```

float 走 memcpy 进出槽,所以一套 `lume_list_push_i/f/s` 能装三种元素,调用方按
**静态类型**挑 helper —— 这是原生后端唯一能拿到的类型信息。

这里有个只会在一条腿上发作、因此格外难查的坑:list / map 的「槽」里装的是指针,
**不是对象本身**。所以取变量值时必须 `load` 一次 —— 具名结构体(变量就是它的槽位)
反而是直接把槽地址当指针用。`cg_var` 因此分成两支:

```c
if (a->ty->kind == TY_STRUCT && a->ty->name) return val_make(a->ty, a->slot);  /* 具名:槽即地址 */
/* list / map:槽里是指针,必须 load,否则 runtime 会把栈帧当对象读 */
```

少这一条 `load` 的后果是 `print(m)` 直接段错误:`lume_map_len` 把栈槽内存当成
`LumeMap*` 解析出一个天文数字的长度,再照它去 deref。

`.len` / `.length` 在两边都是**运行时长度调用**,不是字段 —— 它没有声明处可比,
所以判断要排在结构体字段遍历**之前**。这顺序在 libLLVM 侧还有第二层意思:
`struct_addr()`(把值物化成地址)对 map 会 alloca 一个槽、把堆指针存进去、再把
`o->v` 换成槽地址;把 `.len` 判断放在它后面,就等于把槽传给 `lume_map_len`,
又是同一类错。

## 内建函数

Lume 自己的内建是 `Value` 形态(`Native2(VM*, int, Value*, Value*)`),
编译期既没有 VM 可递,也没法在 IR 里重建 Value 的内存布局。所以原生后端
给自己的 runtime 写了一套**纯标量 helper**(`src/rt.c`,`lume_bi_*`),
内建调用降到这些 helper 上。

已覆盖:

| 类别 | 内建 |
|---|---|
| 输出 | `print` |
| 数值 | `abs` `min` `max` `sqrt` `pow` `floor` `ceil` `round`(int/float 两种重载各自选 helper) |
| 转换 | `str` `int` `float` |
| 运算 | 字符串 `+`(走 `lume_bi_cat`) |
| list | `len` `push`(按元素静态类型选 `lume_list_push_i/f/s`),`x.len` / `x.length` |
| map | `len` `get`(可带默认值,按默认值类型选 `_i/_f/_s`)`put` `keys`(`keys` 返回 list);`m.len` / `m.length` |

`min`/`max` 这类重载按实参类型选 int 或 float flavour;因为 helper 的**参数
类型不等于操作数的类型**(`int(x)` 收 double 吐 i64),表里要单独记参数类型。

## 顶层语句

Lume 的顶层就是语句序列(顶层 `let`、`print` 等)。这些语句不在任何函数里,
codegen 会把它们收成一个合成的 `L_top` 函数(复用 `cg_function` 那条路径),
C entry **只跑 `L_top` 这一个函数**,按源码顺序;和解释器一样,
`main()` 在这里**不是特殊入口**,它就是个普通函数 —— 想让它跑就在顶层
`main();` 调一下。

> 这里踩过一次:entry 曾经跑完 `L_top` 还会自动调 `main()`。后果是顶层写了
> `main();` 的脚本**跑两遍**(而解释器只跑一遍),没写的脚本解释器
> `note: script completed without run()`、原生却照跑 —— 两个后端和解释器
> 各说一套。现在统一成「entry 只跑顶层,`main()` 归顶层调」,三路一致。
> 紧接着又把退出码找回来了:顶层**确实**调 `main()` 时,`L_top` 的返回类型就
> 是 int,合成体把那次调用的返回值带出来,C entry 走
> `call i64 @L_top()` → `trunc i64 %r to i32` → `ret i32 %t`;顶层没调
> `main()` 的脚本不变(仍是 `call void @L_top()` + 退出码 0)。解释器走同一条
> 语义:只有**入口模块**顶层那一次 `main()` 调用的返回值算退出码,导入模块里
> 自己调的 `main()` 是普通调用,不当退出码。三路的判据在
> `tests/native_backends.sh` 的 exit status 一节(`main()` 返回 3 ⇒ 三条腿都
> 以 3 退出;不调 `main()` ⇒ 三条腿都是 0)。

## 目前的不完整之处

- `default<O2>` 这条 pipeline 是整体接上的,没有按 pass 拆开:想只跑 `mem2reg`
  而跳过别的(比如怀疑某个 pass 在 lume 的堆对象 IR 上动错了)就得改
  `backend_llvm.c` 里那一个字符串。跑通时 `LLVMPassBuilderOptionsSetVerifyEach`
  是打开的,所以 pipeline 每步都会验一遍 IR 形状,不对会当场报。
- 优化只做在**本机目标**上:pipeline 用的是 `make_target_machine()` 建的
  cross 目标机器,没有 LTO,也没有 cross-target 的概念。
- 支持的是 core subset:标量、`type` 结构体、list / map、函数、`if`/`while`/
  `for`(含 `for (x in xs)`)、`break`/`continue`/`return`、算术与比较
  (含 `and` / `or` 短路)、上面那些内建。
- **没有实现** `server {}` / `route` / `tool` / `run()` 这条 DSL 主线
  (那是解释器 + bridge 的活儿),碰到就显式报错。
- 没有 `Result` / `?` 传播、函数字面量、模块 `import`
  的跨模块链接、错误与异常通道、包管理。
- **推断是调用点驱动、每函数名只许一个签名**:`func dbl(v) { return v * 2; }`
  这种不标注的写法能编了(推断结果写回 AST,两个后端共用同一条 pass,
  `codegen_infer_signatures()` 由驱动在分派后端**之前**跑一次)。但它不是全
  推断:同一个函数被两个调用点推出两种类型(先 `f(1)` 再 `f("s")`)是**报错**
  而不是取第一个 —— 报 `line N: argument 1 of 'f' is a string, but the
  parameter was already resolved as a int`。一个函数名一个签名,和 C 一样。
- **没有下标语法**:`xs[0]` 在三条路上都是解析错误。列表取元素靠
  `for (x in xs)` 或 `get`,字符串只有 `.len`。
- `and` / `or` 短路了,但**两个操作数都得是 bool 且在原生路径上真的会被短路**;
  `not`、比较这类表达式按构造就是 bool。
- 进程退出码**只有顶层调 `main()` 时**才是 `main()` 的返回值(低八位);程序
  顶层没调 `main()`,退出码就是 0 —— 这种情况下只能自己把失败写到 stderr。
- 默认后端(`--compile`,libLLVM)在 emit 前跑 `default<O2>` —— 这条是 2026-10-03
  补上的(补之前 libLLVM 那路没有 mem2reg,慢一两个数量级)。同一个循环密集
  脚本两边运行侧在同一档,libLLVM 的**编译**更快(`make native-bench` 实测
  184–264ms / 355–446ms),因为文本路的 IR 还要再被 clang 解析和 codegen 一遍。
  设 `--no-pass`(等价的环境变量 `LUME_NO_PASS=1`,脚本里用更省事)可以跳过
  pipeline(诊断与 bench 的哨兵用,`native-bench` 最后一列就靠它)。
- 元素类型推不出来时(典型是 `let xs = [];` 空列表再 `for (x in xs)`),三条腿
  行为一致:解释器正常跑,两个后端都是 **exit 1 + 一条同类诊断**
  (`variable 'x' has no codegen type (its type could not be inferred, ...)`)。
  这里有一条不许退回去的底线:元素的类型拿不到,就**不能**把 NULL 类型喂给
  `LLVMBuildAlloca` —— 那会让编译器直接 SIGSEGV,而同一份源码的文本路只该
  好好报错(2026-10-03 踩过:llvm 腿 139、text 腿 1)。

## 相关文件

| 路径 | 作用 |
|---|---|
| `src/llvm_codegen.c` / `llvm_codegen.h` | AST → LLVM IR,通过 libLLVM C API 造 value(可选编译) |
| `src/backend_llvm.c` / `backend_llvm.h` | 工具链那一步:目标机 + `EmitToFile` + 链接 runtime(可选编译) |
| `src/codegen.c` / `codegen.h` | 文本路入口:CG 上下文、容器 helper、`codegen_emit_ir` |
| `src/codegen_internal.h` | 文本路几个 tu 的共享面(上下文、ERR 宏、`Val`) |
| `src/codegen_types.c` | `Type` → LLVM 拼写(具名 struct 的 intern 表) |
| `src/codegen_expr.c` | 所有表达式形态,以及 `Val` / 内建表与声明 |
| `src/codegen_scan.c` | 表达式的静态类型、块扫描(alloca 前登记变量) |
| `src/codegen_sig.c` | 签名推断(源码没写的 `ret` / `params[i]`) |
| `src/codegen_stmt.c` | 语句、循环、函数体 prologue |
| `src/irbuf.c` / `irbuf.h` | IR 文本缓冲 |
| `src/backend.c` / `backend.h` | 工具链那一步:写 `.ll`、编 runtime、clang 收尾 |
| `src/rt.c` | print helper、`lume_bi_*` 内建 helper,以及 list / map 对象(`lume_list_*` / `lume_map_*`) |
| `examples/native-fact.lume` | 端到端靶子(阶乘/斐波那契/平方) |
| `tests/native-fact.expected` | `make native` 的期望输出 |
| `examples/native-bench.lume` | 两后端对比靶子(双层循环,1e8 次内层迭代) |
| `tests/native-bench.expected` | `make native-bench` 的期望输出(两后端共用) |
