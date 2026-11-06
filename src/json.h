/* JSON writer (growing buffer, UTF-8 sanitizing) and reader (jsmn).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_JSON_H
#define PS5H_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JSMN_HEADER
#define JSMN_PARENT_LINKS
#define JSMN_STRICT
#include "jsmn.h"

/* --- writer ----------------------------------------------------------- */

#define JW_MAX_DEPTH 32

typedef struct {
  char *buf;
  size_t len, cap;
  bool failed; /* set on allocation failure; all later calls do nothing */
  int depth;
  bool need_comma[JW_MAX_DEPTH];
  bool after_key;
} jw_t;

void jw_init(jw_t *w);
void jw_free(jw_t *w);
void jw_obj(jw_t *w);
void jw_end_obj(jw_t *w);
void jw_arr(jw_t *w);
void jw_end_arr(jw_t *w);
void jw_key(jw_t *w, const char *key);
/* Escapes; invalid UTF-8 becomes U+FFFD (WHATWG maximal subpart). */
void jw_str(jw_t *w, const char *utf8, size_t n);
void jw_cstr(jw_t *w, const char *s);
void jw_int(jw_t *w, int64_t v);
void jw_bool(jw_t *w, bool v);
void jw_null(jw_t *w);
/* null when !ok */
void jw_opt_int(jw_t *w, bool ok, int64_t v);
/* Tests only: the n-th buffer growth fails; -1 = off. */
void jw_test_fail_after(int n);

/* Appends valid UTF-8 of s[0..n) to out (cap incl. NUL), replacing invalid
 * sequences with U+FFFD; truncates at a character boundary. Returns length. */
size_t utf8_sanitize(const char *s, size_t n, char *out, size_t cap);

/* --- reader ----------------------------------------------------------- */

typedef struct {
  const char *src;
  size_t len;
  jsmntok_t *tok;
  int ntok;
} jr_t;

/* 0 = ok, < 0 = broken input or more than max_tok tokens. */
int jr_parse(jr_t *r, const char *src, size_t len, jsmntok_t *tok,
             int max_tok);
/* Token index of the value for key among the direct children of obj_tok,
 * or -1. */
int jr_get(const jr_t *r, int obj_tok, const char *key);
/* Unescaped string value; false when not a string or it does not fit. */
bool jr_string(const jr_t *r, int tok, char *out, size_t cap);
bool jr_int(const jr_t *r, int tok, int64_t *out);
bool jr_bool(const jr_t *r, int tok, bool *out);

#endif
