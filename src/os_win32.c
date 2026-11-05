/* Windows (mingw-w64) implementations of the lume_mkdir / lume_flock shims
 * and the ed25519_keypair() CSPRNG (BCryptGenRandom).
 *
 * Kept in a separate translation unit on purpose: this file #includes
 * <windows.h>, which (via winnt.h) also defines a TokenType enumerator. The
 * rest of the tree (lume.h, builtins_fs.c, builtins_crypt.c, ...) uses
 * TokenType as a type name, and the two collide in the same TU ("redeclared
 * as different kind of symbol"). Putting the Windows-only code here — where
 * lume.h is never included — keeps the rest of the tree free of <windows.h>
 * while still providing the symbols they declare under _WIN32.
 *
 * On POSIX this whole file compiles to nothing (the body is inside the
 * #ifdef), so it is safe to keep it in SRCS unconditionally. builtins_fs.c
 * then defines lume_mkdir/lume_flock itself (static) for the POSIX build, and
 * builtins_crypt.c uses getrandom(2)/arc4random_buf instead. */

#ifdef _WIN32

#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <windows.h>
#include <bcrypt.h>
#include <winerror.h>

#ifndef LOCK_SH
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_UN 8
#define LOCK_NB 4
#endif

int lume_mkdir(const char *p) { return _mkdir(p); }

int lume_flock(int fd, int op) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    if (op & LOCK_UN)
        return UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov) ? 0 : -1;
    DWORD flags = (op & LOCK_EX) ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    if (op & LOCK_NB) flags |= LOCKFILE_FAIL_IMMEDIATELY;
    if (LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &ov)) return 0;
    if ((op & LOCK_NB) && GetLastError() == ERROR_LOCK_VIOLATION)
        errno = EAGAIN;
    return -1;
}

/* ed25519_keypair() 的 Windows 随机源。BCryptGenRandom 是系统 CSPRNG
 * (bcrypt.dll, Vista+ 自带); hAlgorithm=NULL + USE_SYSTEM_PREFERRED_RNG
 * 让系统自选 provider, 不依赖 /dev 挂载。失败返回 -1 让调用方明确报错,
 * 绝不退化成 rand()/时间种子 —— 可预测的私钥会让 peer 信任模型形同虚设。 */
int lume_win32_random(unsigned char *buf, size_t n) {
    if (n > (size_t)ULONG_MAX) return -1;
    NTSTATUS st = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n,
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return BCRYPT_SUCCESS(st) ? 0 : -1;
}

#endif /* _WIN32 */
