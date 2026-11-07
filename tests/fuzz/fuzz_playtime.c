/* libFuzzer target: the playtime.json loader on arbitrary bytes, then the
 * answer built from whatever it accepted.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stddef.h>
#include <stdint.h>

#include "json.h"
#include "playtime.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  jw_t w;
  if (playtime_load_buf((const char *)data, size, 1790000000) == 0) {
    jw_init(&w);
    jw_obj(&w);
    playtime_json(&w, NULL, 0);
    jw_end_obj(&w);
    jw_free(&w);
    (void)playtime_total_s("PPSA00001");
    (void)playtime_session_count();
  }
  return 0;
}
