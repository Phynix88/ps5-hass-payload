/* Fan threshold of the system fan driver, with a watcher.
 * Technique as documented by phantomptr/ps5upload (fan_servo.h, fan_map.h).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_FAN_H
#define PS5H_FAN_H

#include <stdint.h>

#include "config.h"
#include "json.h"

typedef enum { FAN_OK, FAN_UNAVAILABLE, FAN_BAD_VALUE, FAN_ABOVE_STOCK, FAN_SYSTEM_ERROR } fan_rc;

/* Reads the current value; learns the factory value once (never our own). */
void fan_init(config *c);
/* 45..85 (never above the factory value) or 0 = back to the factory value.
 * rc_out: console code on FAN_SYSTEM_ERROR. Saves the target in config.json. */
fan_rc fan_set(config *c, int target_c, int *rc_out);
/* Watcher, called from the main loop: every PS5H_FAN_CHECK_S rewrite our
 * target after the console reset it to its own value; stop when another tool
 * sets a third value or after PS5H_FAN_MAX_FAILS failed writes. */
void fan_tick(config *c, int64_t now_ms);
/* "fan": {"target_c":..,"current_c":..,"stock_c":..} into an open object. */
void fan_json(jw_t *w, const config *c);

#endif
