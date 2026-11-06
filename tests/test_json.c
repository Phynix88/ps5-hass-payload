/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include "json.h"

static jsmntok_t tok[4096];

/* Writes w into a NUL-terminated static string. */
static const char *out(jw_t *w) {
  static char buf[65536];
  size_t n = w->len < sizeof(buf) - 1 ? w->len : sizeof(buf) - 1;
  memcpy(buf, w->buf ? w->buf : "", n);
  buf[n] = '\0';
  return buf;
}

static void test_jw_escapes(void) {
  jw_t w;
  jw_init(&w);
  jw_cstr(&w, "a\"b\\c\n\x01\t");
  T_ASSERT(!w.failed);
  T_EQ_STR(out(&w), "\"a\\\"b\\\\c\\n\\u0001\\t\"");
  jw_free(&w);
}

static void test_jw_invalid_utf8(void) {
  jw_t w;
  jw_init(&w);
  jw_arr(&w);
  jw_cstr(&w, "x\xffy");     /* invalid lead byte */
  jw_cstr(&w, "\xc0\xaf");   /* overlong: two maximal subparts */
  jw_cstr(&w, "\xe2\x82");   /* truncated 3-byte sequence: one subpart */
  jw_cstr(&w, "\xed\xa0\x80"); /* encoded surrogate: three subparts */
  jw_cstr(&w, "Caf\xc3\xa9 \xf0\x9f\x98\x80"); /* valid stays */
  jw_end_arr(&w);
  T_EQ_STR(out(&w), "[\"x\xef\xbf\xbdy\",\"\xef\xbf\xbd\xef\xbf\xbd\","
                    "\"\xef\xbf\xbd\",\"\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\","
                    "\"Caf\xc3\xa9 \xf0\x9f\x98\x80\"]");
  jw_free(&w);
}

static void test_jw_nesting_commas(void) {
  jw_t w;
  jw_init(&w);
  jw_obj(&w);
  jw_key(&w, "a");
  jw_arr(&w);
  jw_int(&w, 1);
  jw_int(&w, 2);
  jw_end_arr(&w);
  jw_key(&w, "b");
  jw_null(&w);
  jw_key(&w, "c");
  jw_obj(&w);
  jw_end_obj(&w);
  jw_key(&w, "d");
  jw_bool(&w, true);
  jw_key(&w, "e");
  jw_opt_int(&w, false, 7);
  jw_key(&w, "f");
  jw_opt_int(&w, true, -7);
  jw_end_obj(&w);
  T_ASSERT(!w.failed);
  T_EQ_STR(out(&w),
           "{\"a\":[1,2],\"b\":null,\"c\":{},\"d\":true,\"e\":null,\"f\":-7}");
  jw_free(&w);
}

static void test_jw_alloc_failure(void) {
  jw_t w;
  jw_init(&w);
  jw_test_fail_after(1);
  jw_obj(&w);
  for (int i = 0; i < 1000; i++) {
    jw_key(&w, "some key that makes the buffer grow");
    jw_int(&w, i);
  }
  jw_end_obj(&w);
  jw_test_fail_after(-1);
  T_ASSERT(w.failed);
  jw_free(&w);
}

static void test_jr_get_nested_only_direct_children(void) {
  const char *s = "{\"x\":{\"titleName\":\"in\"},\"titleName\":\"out\"}";
  jr_t r;
  char buf[16];
  int v;
  T_EQ_INT(jr_parse(&r, s, strlen(s), tok, 4096), 0);
  v = jr_get(&r, 0, "titleName");
  T_ASSERT(v > 0);
  T_ASSERT(jr_string(&r, v, buf, sizeof(buf)));
  T_EQ_STR(buf, "out");
  T_EQ_INT(jr_get(&r, 0, "missing"), -1);
  v = jr_get(&r, 0, "x");
  T_ASSERT(jr_string(&r, jr_get(&r, v, "titleName"), buf, sizeof(buf)));
  T_EQ_STR(buf, "in");
}

static void test_jr_unicode_escape(void) {
  const char *s = "{\"a\":\"\\u00e4\\ud83d\\ude00\\n\\/\",\"b\":\"\\ud800x\"}";
  jr_t r;
  char buf[32];
  T_EQ_INT(jr_parse(&r, s, strlen(s), tok, 4096), 0);
  T_ASSERT(jr_string(&r, jr_get(&r, 0, "a"), buf, sizeof(buf)));
  T_EQ_STR(buf, "\xc3\xa4\xf0\x9f\x98\x80\n/");
  /* lone surrogate becomes U+FFFD */
  T_ASSERT(jr_string(&r, jr_get(&r, 0, "b"), buf, sizeof(buf)));
  T_EQ_STR(buf, "\xef\xbf\xbdx");
  /* too small output buffer */
  T_ASSERT(!jr_string(&r, jr_get(&r, 0, "a"), buf, 4));
}

static void test_jr_numbers_and_bools(void) {
  const char *s = "{\"i\":65536,\"n\":-3,\"f\":1.5,\"t\":true,\"s\":\"7\"}";
  jr_t r;
  int64_t i;
  bool b;
  T_EQ_INT(jr_parse(&r, s, strlen(s), tok, 4096), 0);
  T_ASSERT(jr_int(&r, jr_get(&r, 0, "i"), &i));
  T_EQ_INT(i, 65536);
  T_ASSERT(jr_int(&r, jr_get(&r, 0, "n"), &i));
  T_EQ_INT(i, -3);
  T_ASSERT(!jr_int(&r, jr_get(&r, 0, "f"), &i));
  T_ASSERT(!jr_int(&r, jr_get(&r, 0, "s"), &i));
  T_ASSERT(jr_bool(&r, jr_get(&r, 0, "t"), &b));
  T_ASSERT(b);
  T_ASSERT(!jr_bool(&r, jr_get(&r, 0, "i"), &b));
  T_ASSERT(!jr_int(&r, -1, &i));
}

static void test_jr_rejects_truncated(void) {
  jr_t r;
  T_ASSERT(jr_parse(&r, "{\"a\":", 5, tok, 4096) < 0);
  T_ASSERT(jr_parse(&r, "", 0, tok, 4096) < 0);
}

static void test_jr_token_limit(void) {
  static char s[20000];
  size_t n = 0;
  jr_t r;
  s[n++] = '[';
  for (int i = 0; i < 5000; i++) {
    s[n++] = '1';
    s[n++] = ',';
  }
  s[n - 1] = ']';
  T_ASSERT(jr_parse(&r, s, n, tok, 4096) < 0);
}

int main(void) {
  T_RUN(test_jw_escapes);
  T_RUN(test_jw_invalid_utf8);
  T_RUN(test_jw_nesting_commas);
  T_RUN(test_jw_alloc_failure);
  T_RUN(test_jr_get_nested_only_direct_children);
  T_RUN(test_jr_unicode_escape);
  T_RUN(test_jr_numbers_and_bools);
  T_RUN(test_jr_rejects_truncated);
  T_RUN(test_jr_token_limit);
  T_MAIN();
}
