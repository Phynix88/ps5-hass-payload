/* Launch, close, standby (design spec section 5).
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
  CTL_DISABLED,
  CTL_RATE_LIMITED,
  CTL_SYSTEM_ERROR
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

#endif
