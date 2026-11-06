/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "control.h"

#include <stdint.h>
#include <string.h>

#include "json.h"
#include "limits.h"
#include "log.h"
#include "platform.h"

static bool standby_done;
static int64_t standby_last_ms;
static int64_t notify_times[PS5H_NOTIFY_BURST]; /* ring, 0 = unused */
static int notify_next;

void control_init(void) {
  standby_done = false;
  memset(notify_times, 0, sizeof(notify_times));
  notify_next = 0;
}

ctl_rc control_launch(const app_list *apps, const char *title_id,
                      bool replace_running, int *rc_out) {
  plat_running run;
  bool running = plat_running_app(&run) == 1;
  uint32_t user = 0;
  int rc;

  *rc_out = 0;
  if (!apps_find(apps, title_id)) {
    return CTL_NOT_INSTALLED;
  }
  if (running && !strcmp(run.title_id, title_id)) {
    return CTL_ALREADY_RUNNING;
  }
  if (running && !replace_running) {
    return CTL_OTHER_APP_RUNNING;
  }
  if (plat_foreground_user(&user) || !user) {
    return CTL_NO_USER;
  }
  if (running && (rc = plat_kill_app(run.app_id)) < 0) {
    *rc_out = rc;
    log_error("launch %s: closing %s failed rc=%d", title_id, run.title_id, rc);
    return CTL_SYSTEM_ERROR;
  }
  /* Success is rc >= 0 (the new app id), checked on FW 13.60. */
  if ((rc = plat_launch(title_id)) < 0) {
    *rc_out = rc;
    log_error("launch %s failed rc=%d", title_id, rc);
    return CTL_SYSTEM_ERROR;
  }
  *rc_out = rc;
  log_msg("launched %s (app id %d)", title_id, rc);
  return CTL_OK;
}

ctl_rc control_close(const char *expected, int *rc_out) {
  plat_running run;
  int rc;
  *rc_out = 0;
  if (plat_running_app(&run) != 1) {
    return CTL_NO_APP;
  }
  if (expected && strcmp(expected, run.title_id)) {
    return CTL_TITLE_MISMATCH;
  }
  if ((rc = plat_kill_app(run.app_id)) < 0) {
    *rc_out = rc;
    log_error("close %s failed rc=%d", run.title_id, rc);
    return CTL_SYSTEM_ERROR;
  }
  log_msg("closed %s", run.title_id);
  return CTL_OK;
}

ctl_rc control_standby(const config *c, int *rc_out) {
  int64_t now = plat_mono_ms();
  int rc;
  *rc_out = 0;
  if (!c->power_actions) {
    return CTL_DISABLED;
  }
  if (standby_done && now - standby_last_ms < PS5H_STANDBY_INTERVAL_S * 1000) {
    *rc_out = (int)((standby_last_ms + PS5H_STANDBY_INTERVAL_S * 1000 - now +
                     999) / 1000);
    return CTL_RATE_LIMITED;
  }
  standby_done = true;
  standby_last_ms = now;
  log_msg("entering rest mode");
  if ((rc = plat_standby()) < 0) {
    *rc_out = rc;
    log_error("rest mode failed rc=%d", rc);
    return CTL_SYSTEM_ERROR;
  }
  return CTL_OK;
}

ctl_rc control_notify(const char *utf8, int *retry_after) {
  char clean[PS5H_NOTIFY_MAX * 2 + 8], text[PS5H_NOTIFY_MAX + 1];
  size_t o = 0;
  int64_t now = plat_mono_ms(), oldest = notify_times[notify_next];
  int rc;

  *retry_after = 0;
  /* drop control characters except newline, then repair UTF-8 and cut */
  for (const char *p = utf8; *p && o < sizeof(clean) - 1; p++) {
    unsigned char c = (unsigned char)*p;
    if ((c < 0x20 && c != '\n') || c == 0x7f) {
      continue;
    }
    clean[o++] = (char)c;
  }
  clean[o] = '\0';
  utf8_sanitize(clean, o, text, sizeof(text));
  if (!text[0]) {
    return CTL_BAD_INPUT;
  }
  if (oldest && now - oldest < PS5H_NOTIFY_WINDOW_S * 1000) {
    *retry_after = (int)((oldest + PS5H_NOTIFY_WINDOW_S * 1000 - now + 999) / 1000);
    return CTL_RATE_LIMITED;
  }
  if ((rc = plat_notify(text)) < 0) {
    log_error("notification failed rc=%d", rc);
    return CTL_SYSTEM_ERROR;
  }
  notify_times[notify_next] = now ? now : 1;
  notify_next = (notify_next + 1) % PS5H_NOTIFY_BURST;
  return CTL_OK;
}
