/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fan.h"

#include <stdbool.h>
#include <string.h>

#include "limits.h"
#include "log.h"
#include "platform.h"
#include "power.h"

static struct {
  int current;      /* last value read, -1 = unknown */
  bool gave_up;     /* watcher stopped (other tool or repeated failures) */
  int fails;        /* failed writes in a row */
  int64_t last_check_ms;
  int64_t restores[PS5H_FAN_FIGHT_RESTORES]; /* ring of restore times, 0 = unused */
  int next_restore;
  bool init_pending; /* fan_init ran outside "on" and did nothing */
} st;

static bool plausible_stock(int v) {
  return v >= PS5H_FAN_STOCK_MIN_C && v <= PS5H_FAN_STOCK_MAX_C;
}

static void reset_watch(void) {
  st.gave_up = false;
  st.fails = 0;
  memset(st.restores, 0, sizeof(st.restores));
  st.next_restore = 0;
}

static int read_value(void) {
  int v = -1;
  if (plat_fan_get(&v) < 0) {
    return -1;
  }
  st.current = v;
  return v;
}

/* Writes and remembers the value as ours (it must never be learned as the
 * factory value later, e.g. after a restart with an unknown factory value). */
static int write_value(config *c, int v) {
  int rc = plat_fan_set(v);
  if (rc >= 0) {
    st.current = v;
    st.fails = 0;
    if (c->fan_written_c != v) {
      c->fan_written_c = v;
      if (config_save(c)) {
        log_error("fan: could not save the written value");
      }
    }
  }
  return rc;
}

void fan_init(config *c) {
  int v;
  memset(&st, 0, sizeof(st));
  st.current = -1;
  if (!power_is_on()) {
    st.init_pending = true; /* done by the first fan_tick while on */
    return;
  }
  if ((v = read_value()) >= 0 && !c->fan_stock_c && plausible_stock(v) &&
      v != c->fan_target_c && v != c->fan_written_c) {
    /* the console's own value: not our target, not anything we wrote */
    c->fan_stock_c = v;
    if (config_save(c)) {
      log_error("fan: could not save the factory value %d C", v);
    }
    log_msg("fan: factory threshold %d C (remove fan_stock_c from config.json to "
            "learn it again)", v);
  }
  if (c->fan_stock_c && c->fan_target_c > c->fan_stock_c) {
    /* a target set while the factory value was unknown may be too high */
    log_error("fan: target %d C is above the factory value %d C, dropped",
              c->fan_target_c, c->fan_stock_c);
    c->fan_target_c = 0;
    if (config_save(c)) {
      log_error("fan: could not save the target");
    }
  }
}

fan_rc fan_set(config *c, int target_c, int *rc_out) {
  int rc;
  *rc_out = 0;
  if (!plat_capabilities().fan_control) {
    return FAN_UNAVAILABLE;
  }
  if (target_c != 0 && (target_c < PS5H_FAN_MIN_C || target_c > PS5H_FAN_MAX_C)) {
    return FAN_BAD_VALUE;
  }
  if (target_c != 0 && c->fan_stock_c && target_c > c->fan_stock_c) {
    return FAN_ABOVE_STOCK; /* never hotter than Sony intends */
  }
  c->fan_target_c = target_c;
  reset_watch();
  if (config_save(c)) {
    log_error("fan: could not save the target");
  }
  if (target_c == 0) {
    if (!c->fan_stock_c) {
      return FAN_OK; /* unknown factory value: just stop overriding */
    }
    target_c = c->fan_stock_c;
  }
  if ((rc = write_value(c, target_c)) < 0) {
    *rc_out = rc;
    log_error("fan: setting %d C failed rc=%d", target_c, rc);
    return FAN_SYSTEM_ERROR;
  }
  log_msg("fan: threshold %d C", target_c);
  return FAN_OK;
}

/* Three restores within a minute: something else keeps setting its value
 * (the console itself only resets at game starts, if at all). */
static bool fighting(int64_t now_ms) {
  int64_t oldest;
  st.restores[st.next_restore] = now_ms ? now_ms : 1;
  st.next_restore = (st.next_restore + 1) % PS5H_FAN_FIGHT_RESTORES;
  oldest = st.restores[st.next_restore]; /* oldest of the last N, incl. this one */
  return oldest && now_ms - oldest < PS5H_FAN_FIGHT_WINDOW_S * 1000;
}

void fan_tick(config *c, int64_t now_ms) {
  int v;
  if (!power_is_on()) {
    return;
  }
  if (st.init_pending) {
    fan_init(c);
  }
  if (st.last_check_ms && now_ms - st.last_check_ms < PS5H_FAN_CHECK_S * 1000) {
    return;
  }
  st.last_check_ms = now_ms;
  v = read_value(); /* always: /state shows the value in use (reading is harmless) */
  if (!c->fan_target_c || st.gave_up || v < 0 ||
      v == c->fan_target_c) {
    return;
  }
  if (c->fan_stock_c && (v != c->fan_stock_c || c->fan_target_c > c->fan_stock_c)) {
    st.gave_up = true;
    log_error("fan: threshold is %d C, set by another tool; not fighting it", v);
    return;
  }
  /* back on the console's own value (game start): ours again */
  if (write_value(c, c->fan_target_c) < 0) {
    if (++st.fails >= PS5H_FAN_MAX_FAILS) {
      st.gave_up = true;
      log_error("fan: %d failed writes, leaving the console's own threshold",
                PS5H_FAN_MAX_FAILS);
    }
    return;
  }
  if (fighting(now_ms)) {
    st.gave_up = true;
    log_error("fan: threshold keeps changing back, probably another tool; stopped");
  }
}

void fan_json(jw_t *w, const config *c) {
  jw_key(w, "fan");
  jw_obj(w);
  jw_key(w, "target_c");
  jw_opt_int(w, c->fan_target_c > 0, c->fan_target_c);
  jw_key(w, "current_c");
  jw_opt_int(w, st.current >= 0, st.current);
  jw_key(w, "stock_c");
  jw_opt_int(w, c->fan_stock_c > 0, c->fan_stock_c);
  jw_end_obj(w);
}
