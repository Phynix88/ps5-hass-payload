/* SPDX-License-Identifier: GPL-3.0-or-later */
/* jsmn implementation lives here, once; same options as in json.h. */
#define JSMN_PARENT_LINKS
#define JSMN_STRICT
#include "jsmn.h"

#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- writer ----------------------------------------------------------- */

static int fail_after = -1;

void jw_test_fail_after(int n) { fail_after = n; }

void jw_init(jw_t *w) { memset(w, 0, sizeof(*w)); }

void jw_free(jw_t *w) {
  free(w->buf);
  memset(w, 0, sizeof(*w));
}

static bool reserve(jw_t *w, size_t extra) {
  size_t cap;
  char *nb;
  if (w->failed) {
    return false;
  }
  if (w->len + extra + 1 <= w->cap) {
    return true;
  }
  if (fail_after >= 0 && fail_after-- == 0) {
    w->failed = true;
    return false;
  }
  cap = w->cap ? w->cap : 256;
  while (cap < w->len + extra + 1) {
    cap *= 2;
  }
  if (!(nb = realloc(w->buf, cap))) {
    w->failed = true;
    return false;
  }
  w->buf = nb;
  w->cap = cap;
  return true;
}

static void put(jw_t *w, const char *s, size_t n) {
  if (!reserve(w, n)) {
    return;
  }
  memcpy(w->buf + w->len, s, n);
  w->len += n;
  w->buf[w->len] = '\0';
}

static void puts_(jw_t *w, const char *s) { put(w, s, strlen(s)); }

/* Comma handling before any value. */
static void before_value(jw_t *w) {
  if (w->after_key) {
    w->after_key = false;
    return;
  }
  if (w->depth > 0 && w->need_comma[w->depth - 1]) {
    put(w, ",", 1);
  }
}

static void after_value(jw_t *w) {
  if (w->depth > 0) {
    w->need_comma[w->depth - 1] = true;
  }
}

static void open_(jw_t *w, const char *c) {
  before_value(w);
  put(w, c, 1);
  if (w->depth < JW_MAX_DEPTH) {
    w->need_comma[w->depth] = false;
    w->depth++;
  } else {
    w->failed = true;
  }
}

static void close_(jw_t *w, const char *c) {
  put(w, c, 1);
  if (w->depth > 0) {
    w->depth--;
  }
  after_value(w);
}

void jw_obj(jw_t *w) { open_(w, "{"); }
void jw_end_obj(jw_t *w) { close_(w, "}"); }
void jw_arr(jw_t *w) { open_(w, "["); }
void jw_end_arr(jw_t *w) { close_(w, "]"); }

/* Length of the valid UTF-8 sequence at s (1..4), or the negative length of
 * the maximal invalid subpart (-1..-3). */
static int utf8_seq(const unsigned char *s, size_t n) {
  unsigned char b = s[0];
  int need;
  unsigned char lo = 0x80, hi = 0xbf;
  if (b < 0x80) {
    return 1;
  }
  if (b >= 0xc2 && b <= 0xdf) {
    need = 1;
  } else if (b >= 0xe0 && b <= 0xef) {
    need = 2;
    if (b == 0xe0) {
      lo = 0xa0;
    } else if (b == 0xed) {
      hi = 0x9f;
    }
  } else if (b >= 0xf0 && b <= 0xf4) {
    need = 3;
    if (b == 0xf0) {
      lo = 0x90;
    } else if (b == 0xf4) {
      hi = 0x8f;
    }
  } else {
    return -1;
  }
  for (int i = 1; i <= need; i++) {
    if ((size_t)i >= n || s[i] < lo || s[i] > hi) {
      return -i;
    }
    lo = 0x80;
    hi = 0xbf;
  }
  return need + 1;
}

static const char REPLACEMENT[] = "\xef\xbf\xbd";

size_t utf8_sanitize(const char *s, size_t n, char *out, size_t cap) {
  const unsigned char *p = (const unsigned char *)s;
  size_t i = 0, o = 0;
  if (!cap) {
    return 0;
  }
  while (i < n) {
    int k = utf8_seq(p + i, n - i);
    const char *src = k > 0 ? s + i : REPLACEMENT;
    size_t len = k > 0 ? (size_t)k : 3;
    if (o + len + 1 > cap) {
      break;
    }
    memcpy(out + o, src, len);
    o += len;
    i += (size_t)(k > 0 ? k : -k);
  }
  out[o] = '\0';
  return o;
}

static void put_escaped(jw_t *w, const char *s, size_t n) {
  const unsigned char *p = (const unsigned char *)s;
  size_t i = 0;
  put(w, "\"", 1);
  while (i < n) {
    unsigned char c = p[i];
    int k;
    if (c < 0x80) {
      char esc[8];
      switch (c) {
      case '"': puts_(w, "\\\""); break;
      case '\\': puts_(w, "\\\\"); break;
      case '\n': puts_(w, "\\n"); break;
      case '\r': puts_(w, "\\r"); break;
      case '\t': puts_(w, "\\t"); break;
      case '\b': puts_(w, "\\b"); break;
      case '\f': puts_(w, "\\f"); break;
      default:
        if (c < 0x20) {
          snprintf(esc, sizeof(esc), "\\u%04x", c);
          puts_(w, esc);
        } else {
          put(w, (const char *)p + i, 1);
        }
      }
      i++;
      continue;
    }
    k = utf8_seq(p + i, n - i);
    if (k > 0) {
      put(w, (const char *)p + i, (size_t)k);
      i += (size_t)k;
    } else {
      put(w, REPLACEMENT, 3);
      i += (size_t)-k;
    }
  }
  put(w, "\"", 1);
}

void jw_key(jw_t *w, const char *key) {
  before_value(w);
  put_escaped(w, key, strlen(key));
  put(w, ":", 1);
  w->after_key = true;
}

void jw_str(jw_t *w, const char *utf8, size_t n) {
  before_value(w);
  put_escaped(w, utf8, n);
  after_value(w);
}

void jw_cstr(jw_t *w, const char *s) { jw_str(w, s, strlen(s)); }

void jw_int(jw_t *w, int64_t v) {
  char b[24];
  before_value(w);
  snprintf(b, sizeof(b), "%lld", (long long)v);
  puts_(w, b);
  after_value(w);
}

void jw_bool(jw_t *w, bool v) {
  before_value(w);
  puts_(w, v ? "true" : "false");
  after_value(w);
}

void jw_null(jw_t *w) {
  before_value(w);
  puts_(w, "null");
  after_value(w);
}

void jw_opt_int(jw_t *w, bool ok, int64_t v) {
  if (ok) {
    jw_int(w, v);
  } else {
    jw_null(w);
  }
}

/* --- reader ----------------------------------------------------------- */

int jr_parse(jr_t *r, const char *src, size_t len, jsmntok_t *tok,
             int max_tok) {
  jsmn_parser p;
  int n;
  memset(r, 0, sizeof(*r));
  jsmn_init(&p);
  n = jsmn_parse(&p, src, len, tok, (unsigned)max_tok);
  if (n <= 0) {
    return n < 0 ? n : -1;
  }
  /* jsmn stops at NUL and may leave containers open. */
  for (int i = 0; i < n; i++) {
    if (tok[i].end < 0) {
      return -1;
    }
  }
  r->src = src;
  r->len = len;
  r->tok = tok;
  r->ntok = n;
  return 0;
}

static bool valid_tok(const jr_t *r, int t) { return t >= 0 && t < r->ntok; }

int jr_get(const jr_t *r, int obj_tok, const char *key) {
  size_t klen = strlen(key);
  if (!valid_tok(r, obj_tok) || r->tok[obj_tok].type != JSMN_OBJECT) {
    return -1;
  }
  for (int i = obj_tok + 1;
       i < r->ntok && r->tok[i].start < r->tok[obj_tok].end; i++) {
    const jsmntok_t *t = &r->tok[i];
    if (t->parent == obj_tok && t->type == JSMN_STRING &&
        (size_t)(t->end - t->start) == klen &&
        !memcmp(r->src + t->start, key, klen) && i + 1 < r->ntok) {
      return i + 1;
    }
  }
  return -1;
}

static int hex4(const char *s) {
  int v = 0;
  for (int i = 0; i < 4; i++) {
    char c = s[i];
    v <<= 4;
    if (c >= '0' && c <= '9') {
      v |= c - '0';
    } else if (c >= 'a' && c <= 'f') {
      v |= c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      v |= c - 'A' + 10;
    } else {
      return -1;
    }
  }
  return v;
}

static size_t encode_utf8(unsigned cp, char out[4]) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xc0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3f));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xe0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[2] = (char)(0x80 | (cp & 0x3f));
    return 3;
  }
  out[0] = (char)(0xf0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
  out[3] = (char)(0x80 | (cp & 0x3f));
  return 4;
}

/* Decodes into out; false on a bad escape or when it does not fit. The caller
 * (jr_string) empties out on false, so a partial write never stays. */
static bool decode_string(const jr_t *r, int tok, char *out, size_t cap) {
  const char *s, *end;
  size_t o = 0;
  s = r->src + r->tok[tok].start;
  end = r->src + r->tok[tok].end;
  while (s < end) {
    char enc[4];
    size_t n = 1;
    if (*s != '\\') {
      enc[0] = *s++;
    } else {
      if (end - s < 2) {
        return false;
      }
      s++;
      switch (*s) {
      case '"': enc[0] = '"'; break;
      case '\\': enc[0] = '\\'; break;
      case '/': enc[0] = '/'; break;
      case 'b': enc[0] = '\b'; break;
      case 'f': enc[0] = '\f'; break;
      case 'n': enc[0] = '\n'; break;
      case 'r': enc[0] = '\r'; break;
      case 't': enc[0] = '\t'; break;
      case 'u': {
        int cp;
        if (end - s < 5 || (cp = hex4(s + 1)) < 0) {
          return false;
        }
        s += 4;
        if (cp >= 0xd800 && cp <= 0xdbff && end - s >= 7 && s[1] == '\\' &&
            s[2] == 'u') {
          int lo = hex4(s + 3);
          if (lo >= 0xdc00 && lo <= 0xdfff) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
            s += 6;
          }
        }
        if (cp >= 0xd800 && cp <= 0xdfff) {
          cp = 0xfffd;
        }
        n = encode_utf8((unsigned)cp, enc);
        break;
      }
      default:
        return false;
      }
      s++;
    }
    if (o + n + 1 > cap) {
      return false;
    }
    memcpy(out + o, enc, n);
    o += n;
  }
  out[o] = '\0';
  return true;
}

bool jr_string(const jr_t *r, int tok, char *out, size_t cap) {
  if (!cap) {
    return false;
  }
  out[0] = '\0';
  if (!valid_tok(r, tok) || r->tok[tok].type != JSMN_STRING ||
      !decode_string(r, tok, out, cap)) {
    out[0] = '\0'; /* never leave unterminated or partial bytes behind */
    return false;
  }
  return true;
}

bool jr_int(const jr_t *r, int tok, int64_t *out) {
  char buf[24];
  char *endp;
  long long v;
  size_t n;
  if (!valid_tok(r, tok) || r->tok[tok].type != JSMN_PRIMITIVE) {
    return false;
  }
  n = (size_t)(r->tok[tok].end - r->tok[tok].start);
  if (!n || n >= sizeof(buf)) {
    return false;
  }
  memcpy(buf, r->src + r->tok[tok].start, n);
  buf[n] = '\0';
  if (!(buf[0] == '-' || (buf[0] >= '0' && buf[0] <= '9'))) {
    return false;
  }
  v = strtoll(buf, &endp, 10);
  if (*endp) {
    return false;
  }
  *out = v;
  return true;
}

bool jr_bool(const jr_t *r, int tok, bool *out) {
  const char *s;
  size_t n;
  if (!valid_tok(r, tok) || r->tok[tok].type != JSMN_PRIMITIVE) {
    return false;
  }
  s = r->src + r->tok[tok].start;
  n = (size_t)(r->tok[tok].end - r->tok[tok].start);
  if (n == 4 && !memcmp(s, "true", 4)) {
    *out = true;
    return true;
  }
  if (n == 5 && !memcmp(s, "false", 5)) {
    *out = false;
    return true;
  }
  return false;
}
