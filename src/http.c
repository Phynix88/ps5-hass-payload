/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "http.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "limits.h"

/* First "\r\n" in [s, e); e itself when none (buffers are not
 * NUL-terminated, so no str* functions here). */
static const char *find_crlf(const char *s, const char *e) {
  for (; s + 1 < e; s++) {
    if (s[0] == '\r' && s[1] == '\n') {
      return s;
    }
  }
  return e;
}

static const char *find_crlfcrlf(const char *buf, size_t len) {
  for (size_t i = 0; i + 3 < len; i++) {
    if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
        buf[i + 3] == '\n') {
      return buf + i;
    }
  }
  return NULL;
}

/* Copies [s, e) without surrounding blanks; false if it does not fit. */
static bool copy_value(const char *s, const char *e, char *out, size_t cap) {
  size_t n;
  while (s < e && (*s == ' ' || *s == '\t')) {
    s++;
  }
  while (e > s && (e[-1] == ' ' || e[-1] == '\t')) {
    e--;
  }
  n = (size_t)(e - s);
  if (n >= cap) {
    return false;
  }
  memcpy(out, s, n);
  out[n] = '\0';
  return true;
}

static bool name_is(const char *s, size_t n, const char *name) {
  return strlen(name) == n && !strncasecmp(s, name, n);
}

static int parse_request_line(const char *s, const char *e, http_req *out) {
  const char *sp1 = memchr(s, ' ', (size_t)(e - s));
  const char *sp2, *q;
  size_t n;
  if (!sp1) {
    return -400;
  }
  sp2 = memchr(sp1 + 1, ' ', (size_t)(e - sp1 - 1));
  if (!sp2 || sp1[1] != '/') {
    return -400;
  }
  if ((size_t)(e - sp2 - 1) != 8 || strncmp(sp2 + 1, "HTTP/1.", 7)) {
    return -400;
  }
  n = (size_t)(sp1 - s);
  /* method names are case-sensitive: "get" is not GET */
  out->method = (n == 3 && !memcmp(s, "GET", 3))    ? HM_GET
                : (n == 4 && !memcmp(s, "POST", 4)) ? HM_POST
                                                    : HM_OTHER;
  q = memchr(sp1 + 1, '?', (size_t)(sp2 - sp1 - 1));
  n = (size_t)((q ? q : sp2) - (sp1 + 1));
  if (n >= sizeof(out->path)) {
    return -400;
  }
  memcpy(out->path, sp1 + 1, n);
  out->path[n] = '\0';
  if (q) {
    size_t qn = (size_t)(sp2 - q - 1);
    if (qn >= sizeof(out->query)) {
      qn = sizeof(out->query) - 1;
      out->query_cut = true;
    }
    memcpy(out->query, q + 1, qn);
    out->query[qn] = '\0';
  }
  return 0;
}

static int parse_length(const char *s, const char *e, size_t *out) {
  uint64_t v = 0;
  bool any = false;
  while (s < e && (*s == ' ' || *s == '\t')) {
    s++;
  }
  while (e > s && (e[-1] == ' ' || e[-1] == '\t')) {
    e--;
  }
  for (; s < e; s++) {
    if (*s < '0' || *s > '9') {
      return -400;
    }
    any = true;
    if (v > PS5H_MAX_BODY) {
      continue; /* already too large; keep validating digits */
    }
    v = v * 10 + (uint64_t)(*s - '0');
  }
  if (!any) {
    return -400;
  }
  *out = v > PS5H_MAX_BODY ? PS5H_MAX_BODY + 1 : (size_t)v;
  return 0;
}

static bool contains_ci(const char *s, const char *e, const char *needle) {
  size_t n = strlen(needle);
  for (; s + n <= e; s++) {
    if (!strncasecmp(s, needle, n)) {
      return true;
    }
  }
  return false;
}

int http_parse(const char *buf, size_t len, http_req *out) {
  const char *end = find_crlfcrlf(buf, len < PS5H_MAX_HEADER + 4
                                            ? len
                                            : PS5H_MAX_HEADER + 4);
  const char *line, *eol;
  size_t head_len;
  int rc;

  memset(out, 0, sizeof(*out));
  if (!end) {
    return len > PS5H_MAX_HEADER ? -431 : 0;
  }
  head_len = (size_t)(end - buf) + 4;
  if (head_len > PS5H_MAX_HEADER) {
    return -431;
  }
  if (memchr(buf, '\0', head_len)) {
    return -400;
  }

  eol = find_crlf(buf, end + 2);
  if ((rc = parse_request_line(buf, eol, out))) {
    return rc;
  }

  for (line = eol + 2; line < end; line = eol + 2) {
    const char *colon;
    size_t nlen;
    eol = find_crlf(line, end + 2);
    colon = memchr(line, ':', (size_t)(eol - line));
    if (!colon || colon == line) {
      return -400;
    }
    nlen = (size_t)(colon - line);
    if (name_is(line, nlen, "Host")) {
      if (!copy_value(colon + 1, eol, out->host, sizeof(out->host))) {
        return -400;
      }
    } else if (name_is(line, nlen, "Origin")) {
      out->has_origin = true;
    } else if (name_is(line, nlen, "Content-Type")) {
      if (!copy_value(colon + 1, eol, out->content_type,
                      sizeof(out->content_type))) {
        return -400;
      }
    } else if (name_is(line, nlen, "Content-Length")) {
      if (out->has_length) {
        return -400;
      }
      if ((rc = parse_length(colon + 1, eol, &out->content_length))) {
        return rc;
      }
      out->has_length = true;
    } else if (name_is(line, nlen, "Authorization")) {
      if (!copy_value(colon + 1, eol, out->authorization,
                      sizeof(out->authorization))) {
        return -400;
      }
    } else if (name_is(line, nlen, "If-None-Match")) {
      if (!copy_value(colon + 1, eol, out->if_none_match,
                      sizeof(out->if_none_match))) {
        return -400;
      }
    } else if (name_is(line, nlen, "X-PS5-Hass")) {
      char v[8];
      out->has_marker = copy_value(colon + 1, eol, v, sizeof(v)) &&
                        !strcmp(v, "1");
    } else if (name_is(line, nlen, "Transfer-Encoding")) {
      if (contains_ci(colon + 1, eol, "chunked")) {
        out->chunked = true;
      }
    }
  }

  if (out->chunked) {
    return -411;
  }
  if (out->method == HM_POST && !out->has_length) {
    return -411;
  }
  if (out->content_length > PS5H_MAX_BODY) {
    return -413;
  }
  if (len - head_len < out->content_length) {
    return 0;
  }
  out->body = buf + head_len;
  out->body_len = out->content_length;
  return (int)(head_len + out->content_length);
}

const char *http_reason(int status) {
  switch (status) {
  case 200: return "OK";
  case 304: return "Not Modified";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 409: return "Conflict";
  case 411: return "Length Required";
  case 413: return "Payload Too Large";
  case 429: return "Too Many Requests";
  case 431: return "Request Header Fields Too Large";
  case 500: return "Internal Server Error";
  default: return "Unknown";
  }
}

int http_head(char *out, size_t cap, int status, const char *ctype,
              size_t clen, const char *etag) {
  int n = snprintf(out, cap, "HTTP/1.1 %d %s\r\n%s%s%sContent-Length: %zu\r\n"
                             "%s%s%sConnection: close\r\n\r\n",
                   status, http_reason(status), ctype ? "Content-Type: " : "",
                   ctype ? ctype : "", ctype ? "\r\n" : "", clen,
                   etag ? "ETag: " : "", etag ? etag : "", etag ? "\r\n" : "");
  if (n < 0 || (size_t)n >= cap) {
    return -1;
  }
  return n;
}
