/* Play time per title: counted while the title is in the foreground, kept in
 * <root>/data/ps5-hass/playtime.json.
 *
 * A session is one title, in front, without a break. Durations come from the
 * monotonic clock between two samples; the start is wall time. While the power
 * state is not "on" nothing here calls the console.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_PLAYTIME_H
#define PS5H_PLAYTIME_H

#include <stddef.h>
#include <stdint.h>

#include "apps.h"
#include "json.h"

/* Forget everything in memory, then read the file. A missing file is fine; a
 * broken or oversized one is renamed to playtime.json.bad (last_error set)
 * and counting starts empty. 0 unless the data folder cannot be used. */
int playtime_load(void);
/* The loader on a buffer (also the fuzz target): 0 = ok, < 0 = not usable,
 * in which case the state is empty. Replaces what is in memory. */
int playtime_load_buf(const char *buf, size_t len, int64_t now_wall_s);
/* Every pass of the main loop; samples at most every PS5H_PLAYTIME_POLL_S and
 * saves the running session at most every PS5H_PLAYTIME_SAVE_S. apps gives
 * the kind (game/app) of a title; NULL or unknown = "app". */
void playtime_tick(const app_list *apps, int64_t now_ms);
/* Write the file now (shutdown): the running session is included. */
void playtime_flush(void);
/* Writes "titles":[...],"sessions":[...],"current":{...}|null into the open
 * object: sessions that end at or after since_wall_s; the totals include the
 * running session. */
void playtime_json(jw_t *w, const app_list *apps, int64_t since_wall_s);
/* Total of a title including the running session; -1 = unknown title. */
int64_t playtime_total_s(const char *title_id);
/* Number of finished sessions in memory. */
int playtime_session_count(void);

#endif
