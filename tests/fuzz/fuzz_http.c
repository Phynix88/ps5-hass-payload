/* libFuzzer target: http_parse on arbitrary bytes.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stddef.h>
#include <stdint.h>

#include "http.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  http_req r;
  int rc = http_parse((const char *)data, size, &r);
  if (rc > 0 && r.body_len) {
    volatile char c = r.body[r.body_len - 1];
    (void)c;
  }
  return 0;
}
