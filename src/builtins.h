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
Value b_upper(VM *vm, int argc, Value *args);
Value b_lower(VM *vm, int argc, Value *args);
Value b_capitalize(VM *vm, int argc, Value *args);
Value b_trim(VM *vm, int argc, Value *args);
Value b_contains(VM *vm, int argc, Value *args);
Value b_split(VM *vm, int argc, Value *args);
Value b_join(VM *vm, int argc, Value *args);
Value b_substr(VM *vm, int argc, Value *args);
Value b_crypt_sha512(VM *vm, int argc, Value *args);
/* Hash builtins (builtins_crypt.c; SHA-256 vendored in compat/sha256/). */
Value b_sha256(VM *vm, int argc, Value *args);
Value b_hmac_sha256(VM *vm, int argc, Value *args);
/* Base64 (builtins_crypt.c; RFC 4648, 2026-10-16, PayPal OAuth2 Basic). */
Value b_base64_encode(VM *vm, int argc, Value *args);
Value b_base64_decode(VM *vm, int argc, Value *args);
Value b_len(VM *vm, int argc, Value *args);
Value b_keys(VM *vm, int argc, Value *args);
Value b_get(VM *vm, int argc, Value *args);
Value b_range(VM *vm, int argc, Value *args);
Value b_map(VM *vm, int argc, Value *args);
Value b_filter(VM *vm, int argc, Value *args);
Value b_reduce(VM *vm, int argc, Value *args);
/* SQLite builtins (builtins_sql.c, compiled when libsqlite3 was probed;
 * builtins.c carries runtime stubs otherwise). */
Value b_sql_query(VM *vm, int argc, Value *args);
Value b_sql_write(VM *vm, int argc, Value *args);
Value b_sql_tx_begin(VM *vm, int argc, Value *args);
Value b_sql_tx_exec(VM *vm, int argc, Value *args);
Value b_sql_tx_commit(VM *vm, int argc, Value *args);
Value b_sql_tx_rollback(VM *vm, int argc, Value *args);
Value b_json(VM *vm, int argc, Value *args);
Value b_stringify(VM *vm, int argc, Value *args);
Value b_now(VM *vm, int argc, Value *args);
Value b_env(VM *vm, int argc, Value *args);
Value b_argv(VM *vm, int argc, Value *args);
Value b_files(VM *vm, int argc, Value *args);
Value b_read_file(VM *vm, int argc, Value *args);
Value b_read_stdin(VM *vm, int argc, Value *args);
Value b_write_file(VM *vm, int argc, Value *args);
Value b_mkdir(VM *vm, int argc, Value *args);
Value b_lock_file(VM *vm, int argc, Value *args);
Value b_unlock_file(VM *vm, int argc, Value *args);
Value b_strftime(VM *vm, int argc, Value *args);
Value b_put(VM *vm, int argc, Value *args);
Value b_push(VM *vm, int argc, Value *args);
Value b_try(VM *vm, int argc, Value *args);
Value b_sort(VM *vm, int argc, Value *args);
Value b_str_match(VM *vm, int argc, Value *args);
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
/* Statistical distribution builtins (P4, 2026-10-20): the interpreter
 * wrappers validate arguments and surface vm_set_error; the same numbers
 * are computed by rt.c's lume_bi_* for the native backends. */
Value b_normcdf(VM *vm, int argc, Value *args);
Value b_norminv(VM *vm, int argc, Value *args);
Value b_seed(VM *vm, int argc, Value *args);
Value b_rand(VM *vm, int argc, Value *args);
Value b_rand_range(VM *vm, int argc, Value *args);
Value b_rand_int(VM *vm, int argc, Value *args);
Value b_rand_normal(VM *vm, int argc, Value *args);
Value b_rand_bool(VM *vm, int argc, Value *args);
Value b_rand_exponential(VM *vm, int argc, Value *args);
Value b_rand_poisson(VM *vm, int argc, Value *args);
Value b_shuffle(VM *vm, int argc, Value *args);
Value b_rand_cauchy(VM *vm, int argc, Value *args);
Value b_rand_gamma(VM *vm, int argc, Value *args);
Value b_rand_beta(VM *vm, int argc, Value *args);
Value b_rand_chisq(VM *vm, int argc, Value *args);
Value b_rand_student_t(VM *vm, int argc, Value *args);
Value b_rand_geometric(VM *vm, int argc, Value *args);
Value b_rand_weibull(VM *vm, int argc, Value *args);
Value b_rand_logistic(VM *vm, int argc, Value *args);
Value b_rand_pareto(VM *vm, int argc, Value *args);
Value b_rand_laplace(VM *vm, int argc, Value *args);
Value b_rand_binomial(VM *vm, int argc, Value *args);
Value b_rand_lognormal(VM *vm, int argc, Value *args);
Value b_rand_gumbel(VM *vm, int argc, Value *args);
Value b_rand_rayleigh(VM *vm, int argc, Value *args);
Value b_rand_triangular(VM *vm, int argc, Value *args);
Value b_rand_zipf(VM *vm, int argc, Value *args);
Value b_rand_negative_binomial(VM *vm, int argc, Value *args);
Value b_rand_erlang(VM *vm, int argc, Value *args);
Value b_rand_von_mises(VM *vm, int argc, Value *args);
Value b_rand_multinomial(VM *vm, int argc, Value *args);
Value b_sim_gbm(VM *vm, int argc, Value *args);
Value b_sim_bridge(VM *vm, int argc, Value *args);
Value b_sim_ou(VM *vm, int argc, Value *args);
Value b_sim_merton(VM *vm, int argc, Value *args);
Value b_bs_call(VM *vm, int argc, Value *args);
Value b_bs_put(VM *vm, int argc, Value *args);
Value b_bs_delta_call(VM *vm, int argc, Value *args);
Value b_bs_delta_put(VM *vm, int argc, Value *args);
Value b_bs_gamma(VM *vm, int argc, Value *args);
Value b_bs_vega(VM *vm, int argc, Value *args);
Value b_z_test(VM *vm, int argc, Value *args);
Value b_z_pvalue(VM *vm, int argc, Value *args);
Value b_tstat(VM *vm, int argc, Value *args);
Value b_ttest_p(VM *vm, int argc, Value *args);
Value b_chisq_stat(VM *vm, int argc, Value *args);
Value b_chisq_p(VM *vm, int argc, Value *args);
Value b_erf(VM *vm, int argc, Value *args);
Value b_gamma(VM *vm, int argc, Value *args);
Value b_lgamma(VM *vm, int argc, Value *args);
Value b_igamma(VM *vm, int argc, Value *args);
Value b_ibeta(VM *vm, int argc, Value *args);
Value b_tdist_cdf(VM *vm, int argc, Value *args);
Value b_chisq_cdf(VM *vm, int argc, Value *args);
/* Quant builtins (2026-10-24): descriptive statistics, covariance /
 * correlation / linear regression, pct_change and max_drawdown. The
 * interpreter wrappers validate and surface vm_set_error; rt.c's
 * lume_bi_* compute the same numbers for the native backends. */
Value b_mean(VM *vm, int argc, Value *args);
Value b_variance(VM *vm, int argc, Value *args);
Value b_stddev(VM *vm, int argc, Value *args);
Value b_median(VM *vm, int argc, Value *args);
Value b_percentile(VM *vm, int argc, Value *args);
Value b_covariance(VM *vm, int argc, Value *args);
Value b_correlation(VM *vm, int argc, Value *args);
Value b_linreg(VM *vm, int argc, Value *args);
Value b_pct_change(VM *vm, int argc, Value *args);
Value b_max_drawdown(VM *vm, int argc, Value *args);
Value b_skew(VM *vm, int argc, Value *args);
Value b_kurtosis(VM *vm, int argc, Value *args);
Value b_rolling_mean(VM *vm, int argc, Value *args);
Value b_rolling_stddev(VM *vm, int argc, Value *args);
Value b_ewma(VM *vm, int argc, Value *args);
Value b_el(VM *vm, int argc, Value *args);
Value b_render(VM *vm, int argc, Value *args);
Value b_html(VM *vm, int argc, Value *args);

#endif /* LUME_BUILTINS_H */
