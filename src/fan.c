/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fan.h"

#include <stdbool.h>

#include "limits.h"
#include "log.h"
#include "platform.h"

static struct {
  int current;      /* last value read, -1 = unknown */
  bool gave_up;     /* watcher stopped (other tool or repeated failures) */
  int fails;        /* failed writes in a row */
  int64_t last_check_ms;
} st = {-1, false, 0, 0};

static bool plausible_stock(int v) {
  return v >= PS5H_FAN_STOCK_MIN_C && v <= PS5H_FAN_STOCK_MAX_C;
}

static int read_value(void) {
  int v = -1;
  if (plat_fan_get(&v) < 0) {
    return -1;
  }
  st.current = v;
  return v;
}

static int write_value(int v) {
  int rc = plat_fan_set(v);
  if (rc >= 0) {
    st.current = v;
    st.fails = 0;
  }
  return rc;
}

void fan_init(config *c) {
  int v;
  st.current = -1;
  st.gave_up = false;
  st.fails = 0;
  st.last_check_ms = 0;
  if ((v = read_value()) < 0) {
    return;
  }
  /* The factory value is the first plausible reading that is not our own
   * target (a restarted payload may still find its earlier target set). */
  if (!c->fan_stock_c && plausible_stock(v) && v != c->fan_target_c) {
    c->fan_stock_c = v;
    if (config_save(c)) {
      log_error("fan: could not save the factory value %d C", v);
    }
    log_msg("fan: factory threshold %d C", v);
  }
}

fan_rc fan_set(config *c, int target_c, int *rc_out) {
  int rc;
  *rc_out = 0;
  if (!c->fan_control) {
    return FAN_DISABLED;
  }
  if (target_c != 0 && (target_c < PS5H_FAN_MIN_C || target_c > PS5H_FAN_MAX_C)) {
    return FAN_BAD_VALUE;
  }
  if (target_c != 0 && c->fan_stock_c && target_c > c->fan_stock_c) {
    return FAN_ABOVE_STOCK; /* never hotter than Sony intends */
  }
  c->fan_target_c = target_c;
  st.gave_up = false;
  st.fails = 0;
  if (config_save(c)) {
    log_error("fan: could not save the target");
  }
  if (target_c == 0) {
    if (!c->fan_stock_c) {
      return FAN_OK; /* unknown factory value: just stop overriding */
    }
    target_c = c->fan_stock_c;
  }
  if ((rc = write_value(target_c)) < 0) {
    *rc_out = rc;
    log_error("fan: setting %d C failed rc=%d", target_c, rc);
    return FAN_SYSTEM_ERROR;
  }
  log_msg("fan: threshold %d C", target_c);
  return FAN_OK;
}

void fan_tick(config *c, int64_t now_ms) {
  int v;
  if (!c->fan_control || !c->fan_target_c || st.gave_up) {
    return;
  }
  if (st.last_check_ms && now_ms - st.last_check_ms < PS5H_FAN_CHECK_S * 1000) {
    return;
  }
  st.last_check_ms = now_ms;
  if ((v = read_value()) < 0 || v == c->fan_target_c) {
    return;
  }
  if (c->fan_stock_c && v != c->fan_stock_c) {
    st.gave_up = true;
    log_error("fan: threshold is %d C, set by another tool; not fighting it", v);
    return;
  }
  /* back on the console's own value (every game start): ours again */
  if (write_value(c->fan_target_c) < 0 && ++st.fails >= PS5H_FAN_MAX_FAILS) {
    st.gave_up = true;
    log_error("fan: %d failed writes, leaving the console's own threshold",
              PS5H_FAN_MAX_FAILS);
  }
}

void fan_json(jw_t *w, const config *c) {
  jw_key(w, "fan");
  jw_obj(w);
  jw_key(w, "target_c");
  jw_opt_int(w, c->fan_control && c->fan_target_c > 0, c->fan_target_c);
  jw_key(w, "current_c");
  jw_opt_int(w, st.current >= 0, st.current);
  jw_key(w, "stock_c");
  jw_opt_int(w, c->fan_stock_c > 0, c->fan_stock_c);
  jw_end_obj(w);
}
