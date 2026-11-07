/* The console's own play time per title, read from Sony's app.db.
 *
 * Sony's file is never opened in place: it is copied to
 * <root>/data/ps5-hass/app.db.copy (0600) and the copy is opened read-only.
 * Per title the column playedTimeOnConsole is summed over all tables
 * tbl_iconinfo_* (one per console user). NULL counts as 0, but a title with
 * only NULLs is unknown. Table names contain user ids and are never output
 * or logged. Reading the file counts as touching the console: nothing happens
 * while the power state is not "on".
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_SONYTIME_H
#define PS5H_SONYTIME_H

#include <stdint.h>

/* Forget everything and allow a read at once. */
void sonytime_reset(void);
/* Reads the file when power_is_on() and the wait is over: PS5H_SONYTIME_REFRESH_S
 * after a good read, PS5H_SONYTIME_RETRY_S after a failed one, doubled per
 * failure in a row up to PS5H_SONYTIME_RETRY_MAX_S (an attempt that was skipped because the console is not "on" does not count). A failure keeps
 * the values of the last good read (Sony's number only grows) and the last
 * good app.db.copy. now_ms is the monotonic clock. 0 = read, 1 = skipped,
 * -1 = could not be read or does not look as expected. */
int sonytime_refresh(int64_t now_ms);
/* Seconds from the last good read; -1 = unknown (also for an invalid id). */
int64_t sonytime_of(const char *title_id);

#endif
