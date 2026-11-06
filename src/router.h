/* Paths, browser guard, allow list, token check (design spec sections 5, 6).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_ROUTER_H
#define PS5H_ROUTER_H

#include <stdbool.h>
#include <stdint.h>

#include "apps.h"
#include "config.h"
#include "http.h"
#include "json.h"

typedef struct {
  int status;
  const char *ctype;  /* NULL for 304 */
  jw_t body;          /* JSON answers */
  const char *fixed;  /* constant body when building `body` failed */
  int file_fd;        /* icon: >= 0, body empty; owned by the response */
  int64_t file_size;
  char etag[48];      /* with quotes, "" = none */
} route_resp;

typedef struct {
  config *cfg;
  app_list *apps;
  const char *device_id;
  bool mdns_running;
  int64_t start_mono_ms;
} route_ctx;

/* peer_ipv4 in host byte order. */
void route(route_ctx *ctx, const http_req *req, uint32_t peer_ipv4,
           route_resp *out);
/* Answer for a request http_parse rejected (-status). */
void route_parse_error(int status, route_resp *out);
/* Frees the body and closes file_fd unless it was taken (set to -1). */
void route_resp_free(route_resp *r);

#endif
