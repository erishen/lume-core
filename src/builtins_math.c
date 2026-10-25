/* Native math builtins for Lume — hub (2026-10-25).
 *
 * 原 src/builtins_math.c 在 v0.10.1..HEAD 的量化开发里从 328 行膨胀到 2824 行。
 * 这里按领域连续切成 .inc 碎片,本文件只做薄聚合层:仍是单一编译单元,
 * 行为/符号/注册表与拆分前完全一致,无需改 Makefile 的 SRCS。
 *   - builtins_math_basic.inc      基础数值 abs/sqrt/exp/log/ln/pow/.../min/max
 *   - builtins_math_stochastic.inc 随机/分布/采样/special fns/金融路径/BS/假设检验/copula
 *   - builtins_math_quant.inc      量化统计/组合/风险/技术指标/时间序列/交易统计
 * rt.c 里的原生运行时镜像 (lume_bi_ 与 native_ 系列) 与本片逐函数对应,勿漂移。
 */

#include "builtins_internal.h"
#include <math.h>

/* 取一个数值参数；非数字 → error。(被所有切片共用) */
static bool arg_num(VM *vm, Value v, double *out) {
    if (!IS_NUM(v)) {
        vm_set_error(vm, "expected number argument");
        return false;
    }
    *out = AS_NUM(v);
    return true;
}

#include "builtins_math_basic.inc"
#include "builtins_math_stochastic.inc"
#include "builtins_math_quant.inc"
