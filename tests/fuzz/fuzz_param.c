/* libFuzzer target: apps_parse_param on arbitrary bytes.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "apps.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  app_t a;
  memset(&a, 0, sizeof(a));
  apps_parse_param((const char *)data, size, size ? data[0] % 33 - 1 : 1, &a);
  return 0;
}
