/* libFuzzer target: jr_parse, jr_string on every string, jw_str round trip.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stddef.h>
#include <stdint.h>

#include "json.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static jsmntok_t tok[4096];
  static char buf[65536];
  jr_t r;
  jw_t w;
  if (jr_parse(&r, (const char *)data, size, tok, 4096)) {
    return 0;
  }
  jw_init(&w);
  jw_arr(&w);
  for (int i = 0; i < r.ntok; i++) {
    int64_t v;
    bool b;
    if (jr_string(&r, i, buf, sizeof(buf))) {
      jw_cstr(&w, buf);
    }
    if (jr_int(&r, i, &v)) {
      jw_int(&w, v);
    }
    if (jr_bool(&r, i, &b)) {
      jw_bool(&w, b);
    }
    if (r.tok[i].type == JSMN_OBJECT) {
      (void)jr_get(&r, i, "titleName");
    }
  }
  jw_end_arr(&w);
  jw_free(&w);
  return 0;
}
