#ifndef LUME_BUILTINS_H
#define LUME_BUILTINS_H

#include "lume.h"

/* Function-pointer shape of a native builtin implementation. */
typedef void (*Native2)(VM *, int, Value *, Value *);

/* Shared helper (builtins.c): run a native implementation and return its
 * output value. Used by the b_* wrappers and by the vdom trio (vdom.c). */
Value vm_native(VM *vm, int argc, Value *args, Native2 impl);

/* The b_* wrappers registered by bridge_seed_builtins() in interp.c.
 * Implementations live in builtins.c (general builtins) and vdom.c
 * (el / render / html). */
Value b_run(VM *vm, int argc, Value *args);
Value b_print(VM *vm, int argc, Value *args);
Value b_str(VM *vm, int argc, Value *args);
Value b_int(VM *vm, int argc, Value *args);
Value b_float(VM *vm, int argc, Value *args);
Value b_bool(VM *vm, int argc, Value *args);
Value b_string(VM *vm, int argc, Value *args);
Value b_replace(VM *vm, int argc, Value *args);
Value b_crypt_sha512(VM *vm, int argc, Value *args);
Value b_len(VM *vm, int argc, Value *args);
Value b_keys(VM *vm, int argc, Value *args);
Value b_get(VM *vm, int argc, Value *args);
Value b_range(VM *vm, int argc, Value *args);
Value b_map(VM *vm, int argc, Value *args);
Value b_filter(VM *vm, int argc, Value *args);
Value b_reduce(VM *vm, int argc, Value *args);
/* b_sql_query / b_sql_write are host-only: they drive agent-httpd's db layer. */
Value b_json(VM *vm, int argc, Value *args);
Value b_stringify(VM *vm, int argc, Value *args);
Value b_now(VM *vm, int argc, Value *args);
Value b_env(VM *vm, int argc, Value *args);
Value b_argv(VM *vm, int argc, Value *args);
Value b_files(VM *vm, int argc, Value *args);
Value b_read_file(VM *vm, int argc, Value *args);
Value b_write_file(VM *vm, int argc, Value *args);
Value b_mkdir(VM *vm, int argc, Value *args);
Value b_lock_file(VM *vm, int argc, Value *args);
Value b_unlock_file(VM *vm, int argc, Value *args);
Value b_strftime(VM *vm, int argc, Value *args);
Value b_put(VM *vm, int argc, Value *args);
Value b_push(VM *vm, int argc, Value *args);
Value b_try(VM *vm, int argc, Value *args);
Value b_tools(VM *vm, int argc, Value *args);
Value b_skills(VM *vm, int argc, Value *args);
Value b_mcps(VM *vm, int argc, Value *args);
Value b_discovery_endpoints(VM *vm, int argc, Value *args);
Value b_catalog(VM *vm, int argc, Value *args);
Value b_http_get(VM *vm, int argc, Value *args);
Value b_http_post(VM *vm, int argc, Value *args);
Value b_http_put(VM *vm, int argc, Value *args);
Value b_http_patch(VM *vm, int argc, Value *args);
Value b_http_delete(VM *vm, int argc, Value *args);
Value b_default_route(VM *vm, int argc, Value *args);
Value b_abs(VM *vm, int argc, Value *args);
Value b_sqrt(VM *vm, int argc, Value *args);
Value b_exp(VM *vm, int argc, Value *args);
Value b_log(VM *vm, int argc, Value *args);
Value b_ln(VM *vm, int argc, Value *args);
Value b_pow(VM *vm, int argc, Value *args);
Value b_floor(VM *vm, int argc, Value *args);
Value b_ceil(VM *vm, int argc, Value *args);
Value b_round(VM *vm, int argc, Value *args);
Value b_min(VM *vm, int argc, Value *args);
Value b_max(VM *vm, int argc, Value *args);
Value b_el(VM *vm, int argc, Value *args);
Value b_render(VM *vm, int argc, Value *args);
Value b_html(VM *vm, int argc, Value *args);

#endif /* LUME_BUILTINS_H */
