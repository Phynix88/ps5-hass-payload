/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include "http.h"
#include "limits.h"

static int parse(const char *s, http_req *r) {
  return http_parse(s, strlen(s), r);
}

static void test_parse_get_complete(void) {
  const char *s = "GET /api/v1/info HTTP/1.1\r\nHost: 192.0.2.10:8181\r\n"
                  "Authorization: Bearer abc\r\nIf-None-Match: \"e1\"\r\n\r\n";
  http_req r;
  T_EQ_INT(parse(s, &r), (int)strlen(s));
  T_EQ_INT(r.method, HM_GET);
  T_EQ_STR(r.path, "/api/v1/info");
  T_EQ_STR(r.host, "192.0.2.10:8181");
  T_EQ_STR(r.authorization, "Bearer abc");
  T_EQ_STR(r.if_none_match, "\"e1\"");
  T_ASSERT(!r.has_origin && !r.has_marker && !r.has_length);
  T_EQ_INT(r.body_len, 0);
}

static void test_parse_incomplete_returns_0(void) {
  http_req r;
  T_EQ_INT(parse("GET /api/v1/info HTTP/1.1\r\nHost: x\r\n", &r), 0);
  T_EQ_INT(parse("", &r), 0);
}

static void test_parse_post_body_waits_for_length(void) {
  const char *head = "POST /api/v1/pair/finish HTTP/1.1\r\nHost: a.local\r\n"
                     "Content-Type: application/json\r\nX-PS5-Hass: 1\r\n"
                     "Content-Length: 10\r\n\r\n";
  char buf[512];
  http_req r;
  snprintf(buf, sizeof(buf), "%s{\"a\":", head);
  T_EQ_INT(parse(buf, &r), 0);
  snprintf(buf, sizeof(buf), "%s{\"a\":true}", head);
  T_EQ_INT(parse(buf, &r), (int)strlen(buf));
  T_EQ_INT(r.method, HM_POST);
  T_ASSERT(r.has_marker && r.has_length);
  T_EQ_STR(r.content_type, "application/json");
  T_EQ_INT(r.body_len, 10);
  T_ASSERT(!memcmp(r.body, "{\"a\":true}", 10));
}

static void test_header_case_insensitive(void) {
  http_req r;
  T_ASSERT(parse("GET / HTTP/1.1\r\norigin: http://evil\r\nHOST: x\r\n"
                 "x-ps5-hass: 1\r\n\r\n", &r) > 0);
  T_ASSERT(r.has_origin);
  T_ASSERT(r.has_marker);
  T_EQ_STR(r.host, "x");
}

static void test_marker_needs_value_1(void) {
  http_req r;
  T_ASSERT(parse("GET / HTTP/1.1\r\nX-PS5-Hass: yes\r\n\r\n", &r) > 0);
  T_ASSERT(!r.has_marker);
}

static void test_chunked_411(void) {
  http_req r;
  T_EQ_INT(parse("POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", &r),
           -411);
}

static void test_post_without_length_411(void) {
  http_req r;
  T_EQ_INT(parse("POST /x HTTP/1.1\r\nHost: a\r\n\r\n", &r), -411);
}

static void test_body_too_large_413(void) {
  http_req r;
  T_EQ_INT(parse("POST /x HTTP/1.1\r\nContent-Length: 4097\r\n\r\n", &r), -413);
  T_EQ_INT(parse("POST /x HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n"
                 "\r\n", &r), -413);
}

static void test_header_too_large_431(void) {
  static char buf[PS5H_MAX_HEADER + 100];
  http_req r;
  memset(buf, 'a', sizeof(buf));
  memcpy(buf, "GET / HTTP/1.1\r\nX: ", 19);
  T_EQ_INT(http_parse(buf, PS5H_MAX_HEADER + 1, &r), -431);
  /* complete but too long */
  memcpy(buf + PS5H_MAX_HEADER + 10, "\r\n\r\n", 4);
  T_EQ_INT(http_parse(buf, PS5H_MAX_HEADER + 14, &r), -431);
}

static void test_duplicate_length_400(void) {
  http_req r;
  T_EQ_INT(parse("POST /x HTTP/1.1\r\nContent-Length: 2\r\n"
                 "Content-Length: 2\r\n\r\n{}", &r), -400);
  T_EQ_INT(parse("POST /x HTTP/1.1\r\nContent-Length: 2x\r\n\r\n{}", &r),
           -400);
}

static void test_malformed_400(void) {
  http_req r;
  T_EQ_INT(parse("GARBAGE\r\n\r\n", &r), -400);
  T_EQ_INT(parse("GET noslash HTTP/1.1\r\n\r\n", &r), -400);
  T_EQ_INT(parse("GET / HTTP/1.1\r\nNoColon\r\n\r\n", &r), -400);
  T_EQ_INT(parse("GET / FTP/1.0\r\n\r\n", &r), -400);
}

static void test_path_too_long_400(void) {
  char buf[600];
  http_req r;
  char path[300];
  memset(path, 'a', sizeof(path));
  path[0] = '/';
  path[sizeof(path) - 1] = '\0';
  snprintf(buf, sizeof(buf), "GET %s HTTP/1.1\r\n\r\n", path);
  T_EQ_INT(parse(buf, &r), -400);
}

static void test_other_method(void) {
  http_req r;
  T_ASSERT(parse("DELETE /api/v1/info HTTP/1.1\r\n\r\n", &r) > 0);
  T_EQ_INT(r.method, HM_OTHER);
}

static void test_query_stripped(void) {
  http_req r;
  T_ASSERT(parse("GET /api/v1/state?x=1 HTTP/1.1\r\n\r\n", &r) > 0);
  T_EQ_STR(r.path, "/api/v1/state");
}

/* Network buffers are not NUL-terminated (found by fuzz_http). */
static void test_parse_not_nul_terminated(void) {
  const char *s = "POST /api/v1/x HTTP/1.1\r\nHost: a\r\nn/on\r\n"
                  "Content-Length: 2\r\n\r\n{}";
  size_t n = strlen(s);
  char *buf = malloc(n);
  http_req r;
  memcpy(buf, s, n);
  T_EQ_INT(http_parse(buf, n, &r), -400);
  s = "GET / HTTP/1.1\r\nA: b\r\n\r\n";
  memcpy(buf, s, strlen(s));
  T_EQ_INT(http_parse(buf, strlen(s), &r), (int)strlen(s));
  free(buf);
}

static void test_head_serialization(void) {
  char buf[256];
  int n = http_head(buf, sizeof(buf), 200, "application/json", 12, "\"abc\"");
  T_EQ_STR(buf, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: 12\r\nETag: \"abc\"\r\nConnection: close\r\n\r\n");
  T_EQ_INT(n, (int)strlen(buf));
  n = http_head(buf, sizeof(buf), 304, NULL, 0, "\"abc\"");
  T_EQ_STR(buf, "HTTP/1.1 304 Not Modified\r\nContent-Length: 0\r\n"
                "ETag: \"abc\"\r\nConnection: close\r\n\r\n");
  T_EQ_INT(http_head(buf, 10, 200, "x", 0, NULL), -1);
  T_EQ_STR(http_reason(431), "Request Header Fields Too Large");
}

int main(void) {
  T_RUN(test_parse_get_complete);
  T_RUN(test_parse_incomplete_returns_0);
  T_RUN(test_parse_post_body_waits_for_length);
  T_RUN(test_header_case_insensitive);
  T_RUN(test_marker_needs_value_1);
  T_RUN(test_chunked_411);
  T_RUN(test_post_without_length_411);
  T_RUN(test_body_too_large_413);
  T_RUN(test_header_too_large_431);
  T_RUN(test_duplicate_length_400);
  T_RUN(test_malformed_400);
  T_RUN(test_path_too_long_400);
  T_RUN(test_other_method);
  T_RUN(test_query_stripped);
  T_RUN(test_parse_not_nul_terminated);
  T_RUN(test_head_serialization);
  T_MAIN();
}
