/* bench_c_ref.c —— examples/bench_loop.lume 的 C 参照（墙 2 profiling·clang -O2）。
 * 三个 kernel 与 .lume 版逐行对应·同工作量·严格 FP（无 -ffast-math 重结合）。
 * 不是 make test 的一部分——仅作性能基线复现用：
 *   cc -O2 -o bench_c_ref tests/bench_c_ref.c
 *   ./bench_c_ref            # 全部 kernel
 *   ./bench_c_ref A|B|C      # 单 kernel
 * 实测结论见 examples/bench_loop.lume 头注（A 1.02×·B ~11×·C ~16×）。 */
#include <stdio.h>
#include <stdlib.h>

/* A: 标量循环（n = 2e8·串行依赖链） */
static double k_scalar(long n) {
    double s = 0.0, x = 1.000001;
    for (long i = 0; i < n; i++) {
        s = s + x * 1.0000001 - 0.5;
        x = x * 0.9999999 + 0.0000001;
    }
    return s + x;
}

/* B: 数组循环（n = 1e7·写 n 个再读 n 个） */
static double k_list(long n) {
    double *xs = malloc((size_t)n * sizeof(double));
    if (!xs) exit(1);
    for (long i = 0; i < n; i++) xs[i] = (double)i * 0.000001;
    double s = 0.0;
    for (long j = 0; j < n; j++) s += xs[j] * xs[j];
    free(xs);
    return s;
}

/* C: 128×128 matvec × iters 轮（严格 FP 归约·不重结合） */
static double k_matvec(long iters) {
    enum { N = 128 };
    static double M[N][N], v[N];
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            M[r][c] = (r * 31 + c * 17) * 0.0001 - 1.0;
    for (int k = 0; k < N; k++) v[k] = (k * 13) * 0.001 - 0.5;
    double acc = 0.0;
    for (long it = 0; it < iters; it++) {
        double s = 0.0;
        for (int r2 = 0; r2 < N; r2++) {
            double dot = 0.0;
            for (int c2 = 0; c2 < N; c2++) dot += M[r2][c2] * v[c2];
            s += dot;
        }
        acc += s;
    }
    return acc;
}

int main(int argc, char **argv) {
    char k = argc > 1 ? argv[1][0] : 'A';
    if (k == 'A') { printf("A=%.6g\n", k_scalar(200000000L)); }
    if (k == 'B') { printf("B=%.6g\n", k_list(10000000L)); }
    if (k == 'C') { printf("C=%.6g\n", k_matvec(20000L)); }
    return 0;
}
