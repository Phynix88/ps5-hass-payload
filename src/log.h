/* Ring log in <root>/data/ps5-hass/log.txt (+ log.txt.1), last error.
 * Thread-safe (the mDNS thread logs too).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_LOG_H
#define PS5H_LOG_H

void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Like log_msg; also remembered as last_error. */
void log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* NULL when there was none. The pointer stays valid; content may change. */
const char *log_last_error(void);

#endif
