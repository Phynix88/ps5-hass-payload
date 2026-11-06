/* HTTP/1.1 request parsing and response heads, without sockets.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_HTTP_H
#define PS5H_HTTP_H

#include <stdbool.h>
#include <stddef.h>

typedef enum { HM_GET, HM_POST, HM_OTHER } http_method;

typedef struct {
  http_method method;
  char path[256];
  bool has_origin, has_marker, chunked, has_length;
  char host[256], content_type[64], authorization[128], if_none_match[80];
  size_t content_length;
  const char *body;
  size_t body_len;
} http_req;

/* > 0: complete, value = bytes used; 0: need more data;
 * < 0: -HTTP status (400, 411, 413, 431). */
int http_parse(const char *buf, size_t len, http_req *out);

/* Status line and headers incl. the empty line; Connection: close always.
 * ctype and etag may be NULL. Returns length or -1 if cap is too small. */
int http_head(char *out, size_t cap, int status, const char *ctype,
              size_t clen, const char *etag);

const char *http_reason(int status);

#endif
