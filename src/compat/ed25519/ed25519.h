#ifndef LUME_COMPAT_ED25519_H
#define LUME_COMPAT_ED25519_H

#include <stddef.h>
#include <stdint.h>

/* Ed25519 (RFC 8032) — detached signatures, pure C, no external deps.
 *
 * 为什么 vendored 而不是用 OpenSSL: lume-core 的 OpenSSL 是"有就编进去、
 * 没有只是缺"的可选依赖(见 Makefile 的 HAVE_OPENSSL 探测)。签名能力如果
 * 挂在它上面, 就会出现"这台机器能验签、那台不能"的构建差异 —— 而 peer 是
 * 跨机协议, 恰恰最不能容忍两端能力不一致。本实现与 compat/sha256 同规矩:
 * 纯 C、无外部依赖、所有平台行为一致。
 *
 * 密钥格式(全部定长, 不做 ASN.1 / PEM):
 *   secret_key  32 字节 seed (RFC 8032 的私钥输入, 公钥由它推导)
 *   public_key  32 字节 (compressed Edwards-y)
 *   signature   64 字节 (R || S)
 *
 * 调用方负责序列化(hex/base64), 本层只处理二进制。
 *
 * 安全说明: 本实现是"参考实现风格"——清晰、可审计、常量时间无关的
 * 分支很少, 但**没有做侧信道加固**(没有刻意消除查表/条件分支的时序差异)。
 * 用于节点间身份认证(伪造成本远高于收益)是合适的; 若要用于防御本地
 * 侧信道攻击者的场景, 应换成 libsodium 之类加固过的实现。 */

#define ED25519_SECRET_LEN 32
#define ED25519_PUBLIC_LEN 32
#define ED25519_SIGN_LEN   64

/* 从 32 字节 seed 推导公钥。sk/pk 均不可为 NULL。
 * 返回 0 成功，-1 参数非法。 */
int ed25519_public_key(uint8_t pk[ED25519_PUBLIC_LEN],
                       const uint8_t sk[ED25519_SECRET_LEN]);

/* 对 msg(长度 len, 可为 NULL 当 len==0) 签名。
 * 返回 0 成功，-1 参数非法。 */
int ed25519_sign(uint8_t sig[ED25519_SIGN_LEN],
                 const uint8_t *msg, size_t len,
                 const uint8_t pk[ED25519_PUBLIC_LEN],
                 const uint8_t sk[ED25519_SECRET_LEN]);

/* 验签。返回 1 通过，0 不通过(含格式非法)，-1 参数非法。
 * 不通过不区分"签名错"与"公钥不匹配"——调用方要区分的话自己比对公钥。 */
int ed25519_verify(const uint8_t sig[ED25519_SIGN_LEN],
                   const uint8_t *msg, size_t len,
                   const uint8_t pk[ED25519_PUBLIC_LEN]);

#endif /* LUME_COMPAT_ED25519_H */

/* hex 编解码辅助（内建层用；也便于测试与调试输出）。
 * ed25519_hex: n 为字节数，out 需 (n*2+1) 字节。
 * ed25519_unhex: hex 长度必须恰好 n*2，否则返回 -1。 */
void ed25519_hex(const uint8_t *bin, size_t n, char *out);
int ed25519_unhex(const char *hex, uint8_t *bin, size_t n);
