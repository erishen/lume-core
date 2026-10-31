/* Ed25519 (RFC 8032) —— 纯 C 实现，无外部依赖。
 *
 * 结构(照 RFC 8032 §5.1):
 *   1. 域运算  GF(2^255-19): 5 × 51-bit limb
 *   2. 群运算  Edwards 曲线, 扩展坐标 (X:Y:Z:T), x=X/Z, y=Y/Z, T=XY/Z
 *   3. 编解码  point compression / decompression
 *   4. 标量    模 L = 2^252 + 27742317777372353535851937790883648493
 *   5. 签名 / 验签
 *
 * 写法取舍: 定长数组、无动态分配、无可变全局状态, 审计时可逐行对照 RFC
 * 公式。比 donna/sandy2x 慢, 但 peer 每请求一次验签是微秒级, 不值得为此
 * 引入手写汇编。
 *
 * ⚠️ 未做侧信道加固。见 ed25519.h 的安全说明。
 */

#include "ed25519.h"
#include "../sha512/sha512.h"

#include <string.h>

typedef int64_t fe[5];     /* 域元素: 5 × 51-bit limbs */

#define MASK51 0x7ffffffffffffLL

/* ───────────────────────── 字节序辅助 ───────────────────────── */

static uint64_t load_8(const uint8_t *in) {
    uint64_t r = 0;
    for (int i = 7; i >= 0; i--) r = (r << 8) | in[i];
    return r;
}

static void store_8(uint8_t *out, uint64_t v) {
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(v >> (8 * i));
}

/* ───────────────────────── 1. 域运算 ───────────────────────── */

static void fe_0(fe h) { h[0]=0; h[1]=0; h[2]=0; h[3]=0; h[4]=0; }
static void fe_1(fe h) { h[0]=1; h[1]=0; h[2]=0; h[3]=0; h[4]=0; }
static void fe_copy(fe h, const fe f) {
    h[0]=f[0]; h[1]=f[1]; h[2]=f[2]; h[3]=f[3]; h[4]=f[4];
}

/* 32 字节小端 → limb；bit 255 按 RFC 8032 忽略。 */
static void fe_frombytes(fe h, const uint8_t *s) {
    h[0] = (int64_t)(load_8(s) & MASK51);
    h[1] = (int64_t)((load_8(s + 6) >> 3) & MASK51);
    h[2] = (int64_t)((load_8(s + 12) >> 6) & MASK51);
    h[3] = (int64_t)((load_8(s + 19) >> 1) & MASK51);
    h[4] = (int64_t)((load_8(s + 24) >> 12) & MASK51);
}

/* 规约到 [0,p) 后输出 32 字节小端。 */
static void fe_tobytes(uint8_t *s, const fe h) {
    /* 输入可能是未完全规约的 int64 limb（加减乘之后的形态）。
     *
     * 做法: 先把各 limb 折进 [0,2^51)，得到 [0, 2^255) 内的等价值;
     * 再最多减两次 p —— 这一步是 O(1) 的，不需要 Barrett。
     * ref10 用了一个巧妙的 q 判断, 但它依赖"输入已规约到 <2p"的隐式约定,
     * 一旦某处给的是 <4p 就会静默算错(本实现踩过)。这里宁可多一次比较。 */
    int64_t h0=h[0], h1=h[1], h2=h[2], h3=h[3], h4=h[4];

    /* 1) 进位链: 2^255 ≡ 19 */
    h1 += h0 >> 51; h0 &= MASK51;
    h2 += h1 >> 51; h1 &= MASK51;
    h3 += h2 >> 51; h2 &= MASK51;
    h4 += h3 >> 51; h3 &= MASK51;
    h0 += 19 * (h4 >> 51); h4 &= MASK51;
    h1 += h0 >> 51; h0 &= MASK51;

    /* 2) 反复减 p 直到 < p。p limbs = (2^51-19, 2^51-1, ...)。
     *    最多两次: 输入 < 2^255 < 2p + 2^51。 */
    for (int guard = 0; guard < 4; guard++) {
        int ge = (h4 == MASK51 && h3 == MASK51 && h2 == MASK51
                  && h1 == MASK51 && h0 >= MASK51 - 18);
        if (!ge) break;
        h0 -= (MASK51 - 18);   /* -= 2^51 - 19 */
        h1 -= MASK51; h2 -= MASK51; h3 -= MASK51; h4 -= MASK51;
        /* 借位回填 */
        h1 += h0 >> 51; h0 &= MASK51;
        h2 += h1 >> 51; h1 &= MASK51;
        h3 += h2 >> 51; h2 &= MASK51;
        h4 += h3 >> 51; h3 &= MASK51;
        h0 += 19 * (h4 >> 51); h4 &= MASK51;
        h1 += h0 >> 51; h0 &= MASK51;
    }

    store_8(s,      (uint64_t)h0 | ((uint64_t)h1 << 51));
    store_8(s + 8,  ((uint64_t)h1 >> 13) | ((uint64_t)h2 << 38));
    store_8(s + 16, ((uint64_t)h2 >> 26) | ((uint64_t)h3 << 25));
    store_8(s + 24, ((uint64_t)h3 >> 39) | ((uint64_t)h4 << 12));
}

static void fe_add(fe h, const fe f, const fe g) {
    for (int i = 0; i < 5; i++) h[i] = f[i] + g[i];
}

/* 借位补偿: 加 2p 的 limb 展开, 保证中间值非负。 */
static void fe_sub(fe h, const fe f, const fe g) {
    /* 加 2p 保证非负。2p 的 51-bit limb 分解为 (2^51-38, 2^51-1, ...)。 */
    h[0] = f[0] + 0xfffffffffffdaLL - g[0];
    h[1] = f[1] + 0xffffffffffffeLL - g[1];
    h[2] = f[2] + 0xffffffffffffeLL - g[2];
    h[3] = f[3] + 0xffffffffffffeLL - g[3];
    h[4] = f[4] + 0xffffffffffffeLL - g[4];
}

static void fe_neg(fe h, const fe f) {
    h[0] = 0xfffffffffffdaLL - f[0];
    h[1] = 0xffffffffffffeLL - f[1];
    h[2] = 0xffffffffffffeLL - f[2];
    h[3] = 0xffffffffffffeLL - f[3];
    h[4] = 0xffffffffffffeLL - f[4];
}

static void fe_mul(fe h, const fe f, const fe g) {
    const int64_t f0=f[0], f1=f[1], f2=f[2], f3=f[3], f4=f[4];
    const int64_t g0=g[0], g1=g[1], g2=g[2], g3=g[3], g4=g[4];

    /* 2^255 ≡ 19 (mod p): 高位项折叠时乘 19 */
    const int64_t g1_19 = 19 * g1, g2_19 = 19 * g2;
    const int64_t g3_19 = 19 * g3, g4_19 = 19 * g4;

    unsigned __int128 r0 = (unsigned __int128)f0*g0 + (unsigned __int128)f1*g4_19
                         + (unsigned __int128)f2*g3_19 + (unsigned __int128)f3*g2_19
                         + (unsigned __int128)f4*g1_19;
    unsigned __int128 r1 = (unsigned __int128)f0*g1 + (unsigned __int128)f1*g0
                         + (unsigned __int128)f2*g4_19 + (unsigned __int128)f3*g3_19
                         + (unsigned __int128)f4*g2_19;
    unsigned __int128 r2 = (unsigned __int128)f0*g2 + (unsigned __int128)f1*g1
                         + (unsigned __int128)f2*g0    + (unsigned __int128)f3*g4_19
                         + (unsigned __int128)f4*g3_19;
    unsigned __int128 r3 = (unsigned __int128)f0*g3 + (unsigned __int128)f1*g2
                         + (unsigned __int128)f2*g1    + (unsigned __int128)f3*g0
                         + (unsigned __int128)f4*g4_19;
    unsigned __int128 r4 = (unsigned __int128)f0*g4 + (unsigned __int128)f1*g3
                         + (unsigned __int128)f2*g2    + (unsigned __int128)f3*g1
                         + (unsigned __int128)f4*g0;

    uint64_t c;
    c = (uint64_t)(r0 >> 51); r0 &= (unsigned __int128)MASK51; r1 += c;
    c = (uint64_t)(r1 >> 51); r1 &= (unsigned __int128)MASK51; r2 += c;
    c = (uint64_t)(r2 >> 51); r2 &= (unsigned __int128)MASK51; r3 += c;
    c = (uint64_t)(r3 >> 51); r3 &= (unsigned __int128)MASK51; r4 += c;
    c = (uint64_t)(r4 >> 51); r4 &= (unsigned __int128)MASK51; r0 += 19 * c;
    c = (uint64_t)(r0 >> 51); r0 &= (unsigned __int128)MASK51; r1 += c;

    h[0] = (int64_t)r0; h[1] = (int64_t)r1; h[2] = (int64_t)r2;
    h[3] = (int64_t)r3; h[4] = (int64_t)r4;
}

static void fe_sq(fe h, const fe f) { fe_mul(h, f, f); }

static void fe_dbl(fe h, const fe f) { fe_add(h, f, f); }


/* h = f^e，e 为 32 字节小端二进制指数（平方-乘法）。
 *
 * 为什么不用 ref10 那种手写平方链: 链式拆分的位数一旦记错, 得到的是
 * "看起来像点、其实不是"的静默错误(实测踩过)。这里用通用幂,
 * 指数本身直接写成常量, 对照 RFC 一眼可查, 代价是慢几十倍 —— 每次验签
 * 只有一次解点, 微秒级, 换来的是不会写错。 */
static void fe_pow(fe out, const fe f, const uint8_t e[32]) {
    fe acc, base;
    fe_1(acc);
    fe_copy(base, f);
    for (int i = 0; i < 256; i++) {
        if ((e[i >> 3] >> (i & 7)) & 1) fe_mul(acc, acc, base);
        fe_sq(base, base);
    }
    fe_copy(out, acc);
}

/* h = 1/f (费马小定理: f^(p-2))。f=0 时结果为 0。 */
static void fe_invert(fe out, const fe z) {
    /* p - 2 = 2^255 - 21，小端字节即下面这个常量 */
    static const uint8_t EXP_PM2[32] = {
        0xeb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f
    };
    fe_pow(out, z, EXP_PM2);
}


static int fe_isnegative(const fe f) {
    uint8_t s[32];
    fe_tobytes(s, f);
    return s[0] & 1;
}

static int fe_iszero(const fe f) {
    uint8_t s[32];
    fe_tobytes(s, f);
    uint8_t d = 0;
    for (int i = 0; i < 32; i++) d |= s[i];
    return d == 0;
}

/* ───────────────────────── 2. 群运算 ───────────────────────── */

typedef struct { fe X, Y, Z, T; } ge_p3;     /* 完成点 (扩展坐标) */
typedef struct { fe YpX, YmX, Z, T2d; } ge_pc; /* 预计算: (Y+X, Y-X, Z, 2dT) */
typedef struct { fe X, Y, Z, T; } ge_p1p1;

/* d = -121665/121666 mod p */
static const fe ge_d = {
    929955233495203LL, 466365720129213LL, 1662059464998953LL,
    2033849074728123LL, 1442794654840575LL
};
/* 2d */
static const fe ge_d2 = {
    1859910466990425LL, 932731440258426LL, 1072319116312658LL,
    1815898335770999LL, 633789495995903LL
};
/* sqrt(-1) mod p */
static const fe ge_sqrtm1 = {
    1718705420411056LL, 234908883556509LL, 2233514472574048LL,
    2117202627021982LL, 765476049583133LL
};
/* 基点 B */
static const fe ge_Bx = {
    1738742601995546LL, 1146398526822698LL, 2070867633025821LL,
    562264141797630LL, 587772402128613LL
};
static const fe ge_By = {
    1801439850948184LL, 1351079888211148LL, 450359962737049LL,
    900719925474099LL, 1801439850948198LL
};

static void ge_p3_identity(ge_p3 *h) {
    fe_0(h->X); fe_1(h->Y); fe_1(h->Z); fe_0(h->T);
}

static void ge_p3_set_base(ge_p3 *h) {
    fe_copy(h->X, ge_Bx);
    fe_copy(h->Y, ge_By);
    fe_1(h->Z);
    fe_mul(h->T, ge_Bx, ge_By);
}

static void ge_pc_from_p3(ge_pc *r, const ge_p3 *p) {
    fe_add(r->YpX, p->Y, p->X);
    fe_sub(r->YmX, p->Y, p->X);
    fe_copy(r->Z, p->Z);
    fe_mul(r->T2d, ge_d2, p->T);
}

/* p1p1 = p + q  (add-2008-hwcd-3) */
static void ge_add(ge_p1p1 *r, const ge_p3 *p, const ge_pc *q) {
    fe a, b, c, d, e, f, g, h;

    /* A = (Y1-X1)*(Y2-X2) */
    fe t1, t2;
    fe_sub(t1, p->Y, p->X);
    fe_mul(a, t1, q->YmX);
    /* B = (Y1+X1)*(Y2+X2) */
    fe_add(t2, p->Y, p->X);
    fe_mul(b, t2, q->YpX);
    /* C = T1 * (2d*T2) */
    fe_mul(c, p->T, q->T2d);
    /* D = 2 * Z1 * Z2 */
    fe_mul(d, p->Z, q->Z);
    fe_dbl(d, d);

    fe_sub(e, b, a);   /* E = B-A */
    fe_sub(f, d, c);   /* F = D-C */
    fe_add(g, d, c);   /* G = D+C */
    fe_add(h, b, a);   /* H = B+A */

    fe_mul(r->X, e, f);
    fe_mul(r->Y, g, h);
    fe_mul(r->T, e, h);
    fe_mul(r->Z, f, g);
}

/* p1p1 = 2p  (dbl-2008-hwcd) */
static void ge_dbl(ge_p1p1 *r, const ge_p3 *p) {
    fe a, b, c, d, e, f, g, h;

    fe_sq(a, p->X);            /* A = X1^2 */
    fe_sq(b, p->Y);            /* B = Y1^2 */
    fe_sq(c, p->Z); fe_dbl(c, c);  /* C = 2*Z1^2 */
    fe_neg(d, a);              /* D = -A */

    fe_add(e, p->X, p->Y);
    fe_sq(e, e);
    fe_sub(e, e, a);
    fe_sub(e, e, b);           /* E = (X1+Y1)^2 - A - B */

    fe_add(g, d, b);           /* G = D+B */
    fe_sub(f, g, c);           /* F = G-C */
    fe_sub(h, d, b);           /* H = D-B */

    fe_mul(r->X, e, f);
    fe_mul(r->Y, g, h);
    fe_mul(r->T, e, h);
    fe_mul(r->Z, f, g);
}

/* p1p1 → p3: 用 T = X*Y*Zinv 的恒等式, 只需一次求逆。 */
static void ge_p1p1_to_p3(ge_p3 *r, const ge_p1p1 *p) {
    fe X = {p->X[0], p->X[1], p->X[2], p->X[3], p->X[4]};
    fe Y = {p->Y[0], p->Y[1], p->Y[2], p->Y[3], p->Y[4]};
    fe Z = {p->Z[0], p->Z[1], p->Z[2], p->Z[3], p->Z[4]};
    fe T = {p->T[0], p->T[1], p->T[2], p->T[3], p->T[4]};
    fe_copy(r->X, X);
    fe_copy(r->Y, Y);
    fe_copy(r->Z, Z);
    fe_copy(r->T, T);
}

/* 完成坐标 → 压缩形式(32 字节: y 的 255 位 + x 的最低位作符号位)。 */
static void ge_p3_tobytes(uint8_t *s, const ge_p3 *h) {
    fe recip, x, y;
    fe_invert(recip, h->Z);
    fe_mul(x, h->X, recip);
    fe_mul(y, h->Y, recip);
    fe_tobytes(s, y);
    s[31] ^= (uint8_t)(fe_isnegative(x) << 7);
}

/* 压缩形式 → 完成点。返回 0 成功，-1 表示不是曲线上的点。 */
static int ge_frombytes_neg(ge_p3 *h, const uint8_t *s) {
    fe u, v, vinv, x2, x, check;
    int sign = s[31] >> 7;

    fe_frombytes(h->Y, s);
    fe_1(h->Z);

    fe_sq(u, h->Y);              /* u = y^2     */
    fe_mul(v, u, ge_d);          /* v = d*y^2   */
    fe_sub(u, u, h->Z);          /* u = y^2 - 1 */
    fe_add(v, v, h->Z);          /* v = d*y^2+1 */

    /* x^2 = u/v */
    fe_invert(vinv, v);
    fe_mul(x2, u, vinv);

    /* x = x2^((p+3)/8)；(p+3)/8 = 2^252 - 2 */
    {
        static const uint8_t EXP_P3_8[32] = {
            0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0f
        };
        fe_pow(x, x2, EXP_P3_8);
    }

    /* 若 x^2 != u/v，则 x *= sqrt(-1) */
    fe_sq(check, x);
    fe_sub(check, check, x2);
    if (!fe_iszero(check)) {
        fe_mul(x, x, ge_sqrtm1);
        fe_sq(check, x);
        fe_sub(check, check, x2);
        if (!fe_iszero(check)) return -1;   /* 不在曲线上 */
    }

    /* 选与 sign 位一致的根 */
    if (fe_isnegative(x) != sign) fe_neg(x, x);

    fe_copy(h->X, x);
    fe_mul(h->T, h->X, h->Y);

    /* 返回 -A（验签用 [S]B + [k](-A)） */
    fe_neg(h->X, h->X);
    fe_neg(h->T, h->T);
    return 0;
}

/* ───────────────────────── 3. 标量运算 (mod L) ───────────────────────── */

/* L = 2^252 + 27742317777372353535851937790883648493，小端 */
static const uint8_t SC_L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
};

static int sc_ge_L(const uint8_t r[32]) {
    for (int j = 31; j >= 0; j--) {
        if (r[j] > SC_L[j]) return 1;
        if (r[j] < SC_L[j]) return 0;
    }
    return 1;  /* 相等也算 >= */
}

static void sc_sub_L(uint8_t r[32]) {
    int borrow = 0;
    for (int j = 0; j < 32; j++) {
        int v = (int)r[j] - (int)SC_L[j] - borrow;
        borrow = v < 0;
        r[j] = (uint8_t)(v & 0xff);
    }
}

/* b (64 字节小端) → b mod L，结果写入 b 前 32 字节。
 *
 * 逐位长除法("移一位, 条件减一次")：可读性远高于 ref10 的 Barrett 归约,
 * 代价是 256 次循环。每次签名/验签只用一两次, 而 peer 的瓶颈在 agent
 * 推理(秒级), 这点算术开销可忽略 —— 可审计性优先。 */
static void sc_reduce(uint8_t b[64]) {
    uint8_t r[32];
    memset(r, 0, sizeof r);

    for (int i = 511; i >= 0; i--) {
        uint8_t carry = (uint8_t)((b[i >> 3] >> (i & 7)) & 1);
        for (int j = 0; j < 32; j++) {
            uint8_t nc = (uint8_t)(r[j] >> 7);
            r[j] = (uint8_t)((r[j] << 1) | carry);
            carry = nc;
        }
        if (carry) {
            sc_sub_L(r);
            continue;
        }
        if (sc_ge_L(r)) sc_sub_L(r);
    }
    memcpy(b, r, 32);
    memset(b + 32, 0, 32);
}


/* 64 字节小端（如 SHA-512 输出）→ 32 字节 mod L。
 *
 * 必须吃满 64 字节: SHA-512 输出 512 bit, 只取前 32 字节会丢掉高位,
 * 得到的不是 h mod L —— 这是签名"公钥对、签名错"最典型的成因。 */
static void sc_reduce64(uint8_t out[32], const uint8_t in[64]) {
    uint8_t wide[64];
    memcpy(wide, in, 64);
    sc_reduce(wide);
    memcpy(out, wide, 32);
}

/* s = (a*b + c) mod L，输入输出均 32 字节小端。 */
static void sc_muladd(uint8_t *s, const uint8_t *a, const uint8_t *b, const uint8_t *c) {
    uint64_t x[8];
    memset(x, 0, sizeof x);

    for (int i = 0; i < 4; i++) {
        uint64_t carry = 0;
        uint64_t ai = load_8(a + i * 8);
        for (int j = 0; j < 4; j++) {
            unsigned __int128 t = (unsigned __int128)ai * load_8(b + j * 8)
                                + x[i + j] + carry;
            x[i + j] = (uint64_t)t;
            carry = (uint64_t)(t >> 64);
        }
        x[i + 4] += carry;
    }

    uint64_t carry = 0;
    for (int i = 0; i < 8; i++) {
        unsigned __int128 t = (unsigned __int128)x[i] + carry;
        if (i < 4) t += load_8(c + i * 8);
        x[i] = (uint64_t)t;
        carry = (uint64_t)(t >> 64);
    }

    uint8_t wide[64];
    for (int i = 0; i < 8; i++) store_8(wide + i * 8, x[i]);
    sc_reduce(wide);
    memcpy(s, wide, 32);
}

/* ───────────────────── 4. 标量乘 ───────────────────── */

/* r = [e]P。双倍-相加；e 为 256 bit 小端。
 *
 * 非恒定时间（分支依赖 e 的位）。peer 身份认证是固定身份、低频调用，
 * 时序攻击者需要反复触发同一私钥签名并做统计，本场景风险可接受。
 * 若将来用于高价值密钥，换 libsodium 之类加固实现。 */
static void ge_scalarmult(ge_p3 *r, const uint8_t e[32], const ge_p3 *p) {
    ge_p3 acc;
    ge_p3_identity(&acc);

    ge_p3 q = *p;
    ge_pc qc;
    ge_pc_from_p3(&qc, &q);
    (void)qc;

    ge_pc acc_c;
    ge_p1p1 t;

    for (int i = 255; i >= 0; i--) {
        int is_identity = (acc.X[0] == 0) && (acc.Y[0] == 1) && (acc.Z[0] == 1);
        if (!is_identity) {
            ge_dbl(&t, &acc);
            ge_p1p1_to_p3(&acc, &t);
        }
        if ((e[i >> 3] >> (i & 7)) & 1) {
            /* add 公式要求两点都非退化，单位元单独处理 */
            if (is_identity) {
                acc = q;
            } else {
                ge_pc_from_p3(&acc_c, &acc);
                ge_add(&t, &q, &acc_c);
                ge_p1p1_to_p3(&acc, &t);
            }
        }
    }
    *r = acc;
}

/* ───────────────────── 5. 密钥 / 签名 / 验签 ───────────────────── */

/* h = SHA512(seed); 前 32 字节 clamp 成标量 a，后 32 字节是 nonce 前缀。 */
static void expand_secret(const uint8_t sk[32], uint8_t a[32], uint8_t prefix[32]) {
    uint8_t h[64];
    sha512(sk, 32, h);
    memcpy(a, h, 32);
    a[0] &= 248;
    a[31] &= 127;
    a[31] |= 64;
    memcpy(prefix, h + 32, 32);
}

int ed25519_public_key(uint8_t pk[32], const uint8_t sk[32]) {
    if (!pk || !sk) return -1;
    uint8_t a[32], prefix[32];
    expand_secret(sk, a, prefix);

    ge_p3 A, B;
    ge_p3_set_base(&B);
    ge_scalarmult(&A, a, &B);
    ge_p3_tobytes(pk, &A);
    return 0;
}

int ed25519_sign(uint8_t sig[64], const uint8_t *msg, size_t len,
                 const uint8_t pk[32], const uint8_t sk[32]) {
    if (!sig || !pk || !sk || (!msg && len)) return -1;

    uint8_t a[32], prefix[32];
    expand_secret(sk, a, prefix);

    /* r = SHA512(prefix || M) mod L，先暂存在 sig+32 */
    {
        sha512_ctx c;
        sha512_init(&c);
        sha512_update(&c, prefix, 32);
        sha512_update(&c, msg, len);
        uint8_t h[64];
        sha512_final(&c, h);
        sc_reduce64(sig + 32, h);
    }

    /* R = [r]B → sig[0..31] */
    ge_p3 R, B;
    ge_p3_set_base(&B);
    ge_scalarmult(&R, sig + 32, &B);
    ge_p3_tobytes(sig, &R);

    /* k = SHA512(R || A || M) mod L */
    uint8_t k[32];
    {
        sha512_ctx c;
        sha512_init(&c);
        sha512_update(&c, sig, 32);
        sha512_update(&c, pk, 32);
        sha512_update(&c, msg, len);
        uint8_t h[64];
        sha512_final(&c, h);
        sc_reduce64(k, h);
    }

    /* S = (r + k*a) mod L */
    uint8_t r_copy[32];
    memcpy(r_copy, sig + 32, 32);
    sc_muladd(sig + 32, k, a, r_copy);
    return 0;
}

int ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t len,
                   const uint8_t pk[32]) {
    if (!sig || !pk || (!msg && len)) return -1;

    /* S 必须 < L（RFC 8032 严格检查，防可锻性） */
    if (sc_ge_L(sig + 32)) return 0;

    ge_p3 negA;
    if (ge_frombytes_neg(&negA, pk) != 0) return 0;

    /* k = SHA512(R || A || M) mod L */
    uint8_t k[32];
    {
        sha512_ctx c;
        sha512_init(&c);
        sha512_update(&c, sig, 32);
        sha512_update(&c, pk, 32);
        sha512_update(&c, msg, len);
        uint8_t h[64];
        sha512_final(&c, h);
        sc_reduce64(k, h);
    }

    /* R' = [S]B + [k](-A) */
    ge_p3 B, sB, kA;
    ge_p3_set_base(&B);
    ge_scalarmult(&sB, sig + 32, &B);
    ge_scalarmult(&kA, k, &negA);

    /* sB、kA 都不是单位元时才能直接加；退化情况(极小概率)单独算 */
    int sB_identity = (sB.X[0] == 0) && (sB.Y[0] == 1) && (sB.Z[0] == 1);
    int kA_identity = (kA.X[0] == 0) && (kA.Y[0] == 1) && (kA.Z[0] == 1);

    ge_p3 Rprime;
    if (kA_identity) {
        Rprime = sB;
    } else if (sB_identity) {
        Rprime = kA;
    } else {
        ge_pc kAc;
        ge_pc_from_p3(&kAc, &kA);
        ge_p1p1 sum;
        ge_add(&sum, &sB, &kAc);
        ge_p1p1_to_p3(&Rprime, &sum);
    }

    uint8_t check[32];
    ge_p3_tobytes(check, &Rprime);

    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(check[i] ^ sig[i]);
    return diff == 0 ? 1 : 0;
}

/* ───────────────────── 6. hex 辅助（给内建层用） ───────────────────── */

/* 32/64 字节 → 64/128 字符小写 hex；调用方保证 out 够大。 */
void ed25519_hex(const uint8_t *bin, size_t n, char *out) {
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = hx[bin[i] >> 4];
        out[i * 2 + 1] = hx[bin[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

/* hex → bin。返回 0 成功，-1 长度不符或含非 hex 字符。 */
int ed25519_unhex(const char *hex, uint8_t *bin, size_t n) {
    if (!hex) return -1;
    if (strlen(hex) != n * 2) return -1;
    for (size_t i = 0; i < n; i++) {
        int hi = -1, lo = -1;
        char a = hex[i * 2], b = hex[i * 2 + 1];
        if (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
        if (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
        if (hi < 0 || lo < 0) return -1;
        bin[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}
