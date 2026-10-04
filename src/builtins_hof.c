/* 高阶集合函数:range / map / filter / reduce——从 builtins.c 拆出
 * (2026-09-27)。map/filter/reduce 通过 call_function 驱动 DSL 函数。 */

#include "builtins_internal.h"

/* ---- collection tools: range / map / filter / reduce ----
 * map/filter/reduce take a DSL function (named func or a lambda) and drive
 * it through the shared call machinery (call_function), so results, GC
 * rooting and `return` unwinding behave exactly like a regular call. */

static bool fn_arg(Value v) {
    return IS_OBJ(v) && (AS_OBJ(v)->type == OBJ_FUNC || AS_OBJ(v)->type == OBJ_NATIVE);
}

/* Call a DSL function with `argc` args (array), returning its result and
 * leaving the VM stack balanced (call_function leaves an error-null on
 * failure; drop it). */
static Value call_dsl_fn(VM *vm, Value fn, Value *args, int argc) {
    vm_push(vm, fn);
    for (int i = 0; i < argc; i++) vm_push(vm, args[i]);
    call_function(vm, fn, argc);
    if (vm->error) return vm_pop(vm); /* the error-result null */
    return vm_pop(vm);
}

/* range(stop) / range(start, stop) / range(start, stop, step) -> list.
 * Whole numbers stay ints so `range(3)` prints [0, 1, 2], not [0.0, ...]. */
void native_range(VM *vm, int argc, Value *args, Value *out) {
    double a = 0, b, step = 1;
    if (argc < 1) { vm_set_error(vm, "range() needs a stop or (start, stop)"); return; }
    if (argc == 1) {
        if (!IS_NUM(args[0])) { vm_set_error(vm, "range() argument must be a number"); return; }
        b = AS_NUM(args[0]);
    } else {
        if (!IS_NUM(args[0]) || !IS_NUM(args[1])) {
            vm_set_error(vm, "range() arguments must be numbers");
            return;
        }
        a = AS_NUM(args[0]); b = AS_NUM(args[1]);
    }
    if (argc >= 3) {
        if (!IS_NUM(args[2])) { vm_set_error(vm, "range() step must be a number"); return; }
        step = AS_NUM(args[2]);
        if (step == 0) { vm_set_error(vm, "range() step must not be zero"); return; }
    }
    Obj *list = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)list)); /* root while filling */
    if (step > 0) {
        for (double v = a; v < b; v += step)
            list_push(vm, list, v == (long long)v ? val_int((long long)v)
                                                  : val_num(v));
    } else {
        for (double v = a; v > b; v += step)
            list_push(vm, list, v == (long long)v ? val_int((long long)v)
                                                  : val_num(v));
    }
    *out = vm_pop(vm);
}

/* map(fn, list) -> new list of fn(item) for each item. */
void native_map(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 2 || !fn_arg(args[0]) ||
        !IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_LIST) {
        vm_set_error(vm, "map() expects (fn, list)");
        return;
    }
    Value fn = args[0];
    Obj *src = AS_OBJ(args[1]);
    Obj *dst = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)dst)); /* root while filling */
    for (int i = 0; i < src->as.list.count && !vm->error; i++) {
        Value one = src->as.list.items[i];
        Value r = call_dsl_fn(vm, fn, &one, 1);
        if (vm->error) break;
        list_push(vm, dst, r);
    }
    *out = vm_pop(vm);
}

/* filter(fn, list) -> new list of items where fn(item) is truthy. */
void native_filter(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 2 || !fn_arg(args[0]) ||
        !IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_LIST) {
        vm_set_error(vm, "filter() expects (fn, list)");
        return;
    }
    Value fn = args[0];
    Obj *src = AS_OBJ(args[1]);
    Obj *dst = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)dst)); /* root while filling */
    for (int i = 0; i < src->as.list.count && !vm->error; i++) {
        Value one = src->as.list.items[i];
        Value keep = call_dsl_fn(vm, fn, &one, 1);
        if (vm->error) break;
        if (value_truthy(keep)) list_push(vm, dst, one);
    }
    *out = vm_pop(vm);
}

/* reduce(fn, list, init) -> fn(acc, item) folded left over the list. */
void native_reduce(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 3 || !fn_arg(args[0]) ||
        !IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_LIST) {
        vm_set_error(vm, "reduce() expects (fn, list, init)");
        return;
    }
    Value fn = args[0];
    Obj *src = AS_OBJ(args[1]);
    Value acc = args[2];
    for (int i = 0; i < src->as.list.count && !vm->error; i++) {
        Value pair[2] = { acc, src->as.list.items[i] };
        acc = call_dsl_fn(vm, fn, pair, 2);
    }
    *out = acc;
}
