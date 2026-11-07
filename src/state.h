/* Fields of /state and /info.
 * Both write key/value pairs into an object the caller has opened.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_STATE_H
#define PS5H_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "apps.h"
#include "config.h"
#include "json.h"

void state_json(jw_t *w, const app_list *apps, int64_t payload_start_mono_ms);
/* Reads model and firmware once, while the console is "on"; until then /info
 * shows "". Called from the main loop and from info_json. */
void info_cache_refresh(void);
/* Forget the cached values (tests). */
void info_cache_reset(void);
void info_json(jw_t *w, const config *c, const char *device_id,
               bool mdns_running);

#endif
