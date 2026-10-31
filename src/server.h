#ifndef LUME_SERVER_H
#define LUME_SERVER_H

#include <stddef.h>
#include <stdint.h>

typedef struct VM VM;

/* lume_http_serve() is the in-tree http server entrypoint the lume-core
 * bridge uses when it is built without agent-httpd (i.e. the standalone
 * lume-core build). The interpreter calls it through bridge_run(); the
 * native backend calls lume_srv_run() from bridge_native.c instead.
 *
 * 第 4 个参数 VM * 是后加的（handler 需要 VM 才能求值），签名必须与
 * bridge_serve.c 里的定义一致，否则调用点会按旧原型传 3 个参数。 */

void lume_http_serve(const char *host, long port, int backlog, VM *vm);

#endif /* LUME_SERVER_H */
