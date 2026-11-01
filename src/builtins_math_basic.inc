static void native_abs(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(fabs(x));
}

/* 整数除法/求模（语言演进提案 P0-2, 2026-11-01）。lume 的 `/` 恒产生
 * float（无 int/int→int），索引与位操作场景只能算术模拟，且 `x - 2*(x/2)`
 * 对奇数静默出错（浮点乘回去精确还原，余数恒 0）——div/mod 补上原语。
 * 仅接受 int 实参：SPEC §3.2 规定 float→int 不隐式合法，编译后端 coerce
 * 会拒绝，这里同样拒绝才能三后端行为一致。除零返回 0（不崩，保持三
 * 后端一致——编译后端除零是 SIGFPE，解释器是 error，吞掉分歧最安全）。 */
static void native_div(VM *vm, int argc, Value *args, Value *out) {
    if (argc != 2 || !IS_INT(args[0]) || !IS_INT(args[1])) {
        vm_set_error(vm, "div(): needs two int arguments");
        return;
    }
    long long a = AS_INT(args[0]), b = AS_INT(args[1]);
    *out = val_int(b == 0 ? 0 : a / b);
}

static void native_mod(VM *vm, int argc, Value *args, Value *out) {
    if (argc != 2 || !IS_INT(args[0]) || !IS_INT(args[1])) {
        vm_set_error(vm, "mod(): needs two int arguments");
        return;
    }
    long long a = AS_INT(args[0]), b = AS_INT(args[1]);
    /* C 语义：截断除，余数符号随被除数（mod(-7,2)==-1）。 */
    *out = val_int(b == 0 ? 0 : a % b);
}

/* 整数位运算内置（语言演进提案 P1-5, 2026-11-01）：bitrev / 哈希 / 打包整数
 * 场景原本只能算术模拟（且因 '/' 恒 float 而坑上加坑）。六个 int 原语，走与
 * div/mod 相同的 BUILTINS[i64 风格] 机制（text/LLVM 两份表同步，参照 trig
 * 内置先例，成本已知且低）。仅接受 int 实参（与 div/mod 一致）；移位数先掩
 * 0..63（C 对 >=64 移位未定义）；移位按无符号 64 位逻辑移位，与常见位操作
 * 语义对齐（右移为逻辑而非算术）。 */
static void native_shl(VM *vm, int argc, Value *args, Value *out) {
    if (argc != 2 || !IS_INT(args[0]) || !IS_INT(args[1])) {
        vm_set_error(vm, "shl(): needs two int arguments"); return;
    }
    unsigned long long a = (unsigned long long)AS_INT(args[0]);
    int n = (int)(AS_INT(args[1]) & 63);
    *out = val_int((long long)(a << n));
}
static void native_shr(VM *vm, int argc, Value *args, Value *out) {
    if (argc != 2 || !IS_INT(args[0]) || !IS_INT(args[1])) {
        vm_set_error(vm, "shr(): needs two int arguments"); return;
    }
    unsigned long long a = (unsigned long long)AS_INT(args[0]);
    int n = (int)(AS_INT(args[1]) & 63);
    *out = val_int((long long)(a >> n));   /* 逻辑右移（无符号） */
}
static void native_bitand(VM *vm, int argc, Value *args, Value *out) {
    if (argc != 2 || !IS_INT(args[0]) || !IS_INT(args[1])) {
        vm_set_error(vm, "bitand(): needs two int arguments"); return;
    }
    *out = val_int(AS_INT(args[0]) & AS_INT(args[1]));
}
static void native_bit_or(VM *vm, int argc, Value *args, Value *out) {
    if (argc != 2 || !IS_INT(args[0]) || !IS_INT(args[1])) {
        vm_set_error(vm, "bit_or(): needs two int arguments"); return;
    }
    *out = val_int(AS_INT(args[0]) | AS_INT(args[1]));
}
static void native_bit_xor(VM *vm, int argc, Value *args, Value *out) {
    if (argc != 2 || !IS_INT(args[0]) || !IS_INT(args[1])) {
        vm_set_error(vm, "bit_xor(): needs two int arguments"); return;
    }
    *out = val_int(AS_INT(args[0]) ^ AS_INT(args[1]));
}

/* 确定性 RNG 内置（语言演进提案 P1-4, 2026-11-01）：纯函数式 splitmix64，
 * 与 rt.c 的 lume_bi_rand_* 逐算术步骤一致（三后端一致性的硬要求）；
 * 与全局 PCG32 流（builtins_math_stochastic.inc）互补——无隐藏状态、
 * 序无关，rand_next(seed + k) 可并行取值。 */
static void native_rand_next(VM *vm, int argc, Value *args, Value *out) {
    if (argc != 1 || !IS_INT(args[0])) {
        vm_set_error(vm, "rand_next(): needs one int argument"); return;
    }
    unsigned long long z = (unsigned long long)AS_INT(args[0]) + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    *out = val_int((long long)(z ^ (z >> 31)));
}
static void native_rand_float(VM *vm, int argc, Value *args, Value *out) {
    if (argc != 1 || !IS_INT(args[0])) {
        vm_set_error(vm, "rand_float(): needs one int argument"); return;
    }
    unsigned long long z = (unsigned long long)AS_INT(args[0]) + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    *out = val_num((double)((z ^ (z >> 31)) >> 11) * (1.0 / 9007199254740992.0));
}

static void native_sqrt(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    if (x < 0.0) {
        vm_set_error(vm, "sqrt(): negative argument");
        return;
    }
    *out = val_num(sqrt(x));
}

static void native_exp(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(exp(x));
}

static void native_log(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    if (x <= 0.0) {
        vm_set_error(vm, "log(): argument must be positive");
        return;
    }
    *out = val_num(log(x));
}

static void native_pow(VM *vm, int argc, Value *args, Value *out) {
    double x, y;
    if (argc < 2 || !arg_num(vm, args[0], &x) || !arg_num(vm, args[1], &y)) return;
    double r = pow(x, y);
    if (!isfinite(r)) {
        vm_set_error(vm, "pow(): result not finite (domain error)");
        return;
    }
    *out = val_num(r);
}

static void native_floor(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(floor(x));
}

static void native_ceil(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(ceil(x));
}

static void native_round(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(round(x));
}

/* min/max 支持可变参数（≥1 个数值），逐个比较。 */
static void native_min(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) {
        vm_set_error(vm, "min() expects at least one number");
        return;
    }
    double best = 0.0;
    for (int i = 0; i < argc; i++) {
        double x;
        if (!arg_num(vm, args[i], &x)) return;
        if (i == 0 || x < best) best = x;
    }
    *out = val_num(best);
}

static void native_max(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) {
        vm_set_error(vm, "max() expects at least one number");
        return;
    }
    double best = 0.0;
    for (int i = 0; i < argc; i++) {
        double x;
        if (!arg_num(vm, args[i], &x)) return;
        if (i == 0 || x > best) best = x;
    }
    *out = val_num(best);
}

Value b_abs(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_abs); }
Value b_div(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_div); }
Value b_mod(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_mod); }
Value b_shl(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_shl); }
Value b_shr(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_shr); }
Value b_bitand(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_bitand); }
Value b_bit_or(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_bit_or); }
Value b_bit_xor(VM *vm, int argc, Value *args){ return vm_native(vm, argc, args, native_bit_xor); }
Value b_rand_next(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_rand_next); }
Value b_rand_float(VM *vm, int argc, Value *args){ return vm_native(vm, argc, args, native_rand_float); }
Value b_sqrt(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_sqrt); }
Value b_exp(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_exp); }
Value b_log(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_log); }
Value b_ln(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_log); }
Value b_pow(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_pow); }
Value b_floor(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_floor); }
Value b_ceil(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_ceil); }
Value b_round(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_round); }
Value b_min(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_min); }
Value b_max(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_max); }

/* --- 三角函数 / 双曲 / 反三角 (信息计算科学扩展, 2026-10-31) --- */
static void native_sin(VM *vm, int argc, Value *args, Value *out) {
    double x; if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(sin(x));
}
static void native_cos(VM *vm, int argc, Value *args, Value *out) {
    double x; if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(cos(x));
}
static void native_tan(VM *vm, int argc, Value *args, Value *out) {
    double x; if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(tan(x));
}
static void native_asin(VM *vm, int argc, Value *args, Value *out) {
    double x; if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    if (x < -1.0 || x > 1.0) { vm_set_error(vm, "asin(): domain [-1,1]"); return; }
    *out = val_num(asin(x));
}
static void native_acos(VM *vm, int argc, Value *args, Value *out) {
    double x; if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    if (x < -1.0 || x > 1.0) { vm_set_error(vm, "acos(): domain [-1,1]"); return; }
    *out = val_num(acos(x));
}
static void native_atan(VM *vm, int argc, Value *args, Value *out) {
    double x; if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(atan(x));
}
static void native_atan2(VM *vm, int argc, Value *args, Value *out) {
    double y, x;
    if (argc < 2 || !arg_num(vm, args[0], &y) || !arg_num(vm, args[1], &x)) return;
    *out = val_num(atan2(y, x));
}
static void native_sinh(VM *vm, int argc, Value *args, Value *out) {
    double x; if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(sinh(x));
}
static void native_cosh(VM *vm, int argc, Value *args, Value *out) {
    double x; if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(cosh(x));
}
static void native_tanh(VM *vm, int argc, Value *args, Value *out) {
    double x; if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(tanh(x));
}
static void native_asinh(VM *vm, int argc, Value *args, Value *out) {
    double x; if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(asinh(x));
}
static void native_acosh(VM *vm, int argc, Value *args, Value *out) {
    double x; if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    if (x < 1.0) { vm_set_error(vm, "acosh(): domain [1,inf)"); return; }
    *out = val_num(acosh(x));
}
static void native_atanh(VM *vm, int argc, Value *args, Value *out) {
    double x; if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    if (x <= -1.0 || x >= 1.0) { vm_set_error(vm, "atanh(): domain (-1,1)"); return; }
    *out = val_num(atanh(x));
}
Value b_sin(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_sin); }
Value b_cos(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_cos); }
Value b_tan(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_tan); }
Value b_asin(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_asin); }
Value b_acos(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_acos); }
Value b_atan(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_atan); }
Value b_atan2(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_atan2); }
Value b_sinh(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_sinh); }
Value b_cosh(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_cosh); }
Value b_tanh(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_tanh); }
Value b_asinh(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_asinh); }
Value b_acosh(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_acosh); }
Value b_atanh(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_atanh); }

