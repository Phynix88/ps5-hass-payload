/* Launch, close, standby.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_CONTROL_H
#define PS5H_CONTROL_H

#include <stdbool.h>

#include "apps.h"
#include "config.h"

typedef enum {
  CTL_OK,
  CTL_ALREADY_RUNNING,
  CTL_OTHER_APP_RUNNING,
  CTL_NOT_INSTALLED,
  CTL_NO_USER,
  CTL_TITLE_MISMATCH,
  CTL_NO_APP,
  CTL_UNAVAILABLE, /* the console cannot do this (plat_capabilities) */
  CTL_RATE_LIMITED,
  CTL_SYSTEM_ERROR,
  CTL_BAD_INPUT
} ctl_rc;

/* Resets the standby rate limit. */
void control_init(void);
/* rc_out: new app id on CTL_OK, console code on CTL_SYSTEM_ERROR. */
ctl_rc control_launch(const app_list *apps, const char *title_id,
                      bool replace_running, int *rc_out);
ctl_rc control_close(const char *expected_title_or_null, int *rc_out);
/* rc_out: seconds to wait on CTL_RATE_LIMITED, console code on
 * CTL_SYSTEM_ERROR. */
ctl_rc control_standby(const config *c, int *rc_out);
/* Needs the console to support it; shares the rate limit with control_standby. */
ctl_rc control_power_off(const config *c, int *rc_out);
/* Message on the TV: control characters (except \n) removed, invalid UTF-8
 * replaced, cut to PS5H_NOTIFY_MAX bytes at a character boundary; at most
 * PS5H_NOTIFY_BURST per PS5H_NOTIFY_WINDOW_S. retry_after in seconds on
 * CTL_RATE_LIMITED; CTL_BAD_INPUT when nothing is left to show. */
ctl_rc control_notify(const char *utf8, int *retry_after);

#endif
