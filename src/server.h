/* Sockets, poll loop, deadlines, file streaming, waking up
 * (design spec sections 4 and 8).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_SERVER_H
#define PS5H_SERVER_H

#include <stdint.h>

#include "router.h"

typedef struct server server;

/* Does not bind yet; the first server_step does. port 0 = any (tests). */
server *server_new(route_ctx *ctx, uint16_t port);
/* One poll round incl. periodic tasks. */
int server_step(server *s, int timeout_ms);
void server_free(server *s);
uint16_t server_bound_port(const server *s);
/* Tests: open connections, successful binds so far. */
void server_debug(const server *s, int *conns, int *binds);
/* Tests: the next `count` accept() calls fail with errno `err`. */
void server_debug_fail_accept(server *s, int err, int count);

#endif
