static void native_abs(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(fabs(x));
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

