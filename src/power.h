/* Power state of the console and keep-awake.
 *
 * Rule for the whole payload: while the power state is not 1000 ("on"), no
 * module calls any console function except plat_power_state(). Every module
 * that does periodic work asks power_is_on() first.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_POWER_H
#define PS5H_POWER_H

#include <stdbool.h>
#include <stdint.h>

/* Forget everything: state unknown (counts as "on" until the first tick),
 * keep-awake off. */
void power_init(void);
/* Every pass of the main loop: reads the power state, and while keep-awake is
 * on and the console is "on", calls plat_keep_awake() every PS5H_KEEP_AWAKE_S. */
void power_tick(int64_t now_ms);
/* Last good state read == 1000. A failed read keeps the last good state; with
 * none yet it counts as not "on". Only after PS5H_POWER_FAIL_OPEN_READS
 * failed reads in a row spanning PS5H_POWER_FAIL_OPEN_S (a gap of more than
 * PS5H_SUSPEND_GAP_S between reads starts the run over), or on a console
 * without the function, it counts as "on" (as before v0.4). */
bool power_is_on(void);
/* "on" | "going_to_rest" | "rest" | "shutting_down" | "unknown" */
const char *power_state_name(void);
/* Memory only: off after a payload restart. */
void power_set_keep_awake(bool on);
bool power_keep_awake(void);

#endif
