/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "power.h"

#include <string.h>

#include "limits.h"
#include "log.h"
#include "platform.h"

#define STATE_ON 1000
#define STATE_GOING_TO_REST 300
#define STATE_REST 500
#define STATE_SHUTTING_DOWN 100

static struct {
  int state;     /* last good raw value; < 0 = none yet */
  bool ticked;   /* at least one tick ran */
  bool missing;  /* no power state function: counts as "on" */
  bool fail_open; /* failed long enough: counts as "on" */
  bool failing;  /* the last read failed */
  int fail_reads; /* failed reads in the current run */
  int64_t fail_since_ms; /* first failure of the current run */
  bool read_once; /* last_read_ms is valid */
  int64_t last_read_ms;
  bool keep_awake;
  bool armed; /* next_ms is valid */
  int64_t next_ms;
} st = {.state = -1};

void power_init(void) {
  memset(&st, 0, sizeof(st));
  st.state = -1;
}

/* A failed read keeps the last good state (none yet = not "on"): treating it
 * as "on" would open the gate exactly while the console may be falling
 * asleep. Only a long run of failed reads falls back to "on", so a function
 * that breaks for good cannot switch the payload off for good. The run counts
 * reads, not time: the monotonic clock keeps running in rest mode while the
 * process is frozen, so a long gap between two reads proves nothing and starts
 * the run over. */
static void read_state(int64_t now_ms) {
  int s;
  st.ticked = true;
  if (st.read_once && now_ms - st.last_read_ms > PS5H_SUSPEND_GAP_S * 1000) {
    st.failing = st.fail_open = false;
    st.fail_reads = 0;
  }
  st.read_once = true;
  st.last_read_ms = now_ms;
  st.missing = !plat_capabilities().power_state;
  if (st.missing) {
    st.failing = st.fail_open = false;
    return;
  }
  s = plat_power_state();
  if (s < 0) {
    if (!st.failing) {
      st.failing = true;
      st.fail_reads = 0;
      st.fail_since_ms = now_ms;
      log_error("power state read failed (%d)", s);
    }
    if (st.fail_reads < PS5H_POWER_FAIL_OPEN_READS + 1) {
      st.fail_reads++;
    }
    if (!st.fail_open && st.fail_reads >= PS5H_POWER_FAIL_OPEN_READS &&
        now_ms - st.fail_since_ms >= PS5H_POWER_FAIL_OPEN_S * 1000) {
      st.fail_open = true;
      log_error("power state unreadable for %d s, counting as on",
                PS5H_POWER_FAIL_OPEN_S);
    }
    return;
  }
  if (s != st.state || st.failing) {
    log_msg("power state %d", s);
  }
  st.failing = st.fail_open = false;
  st.state = s;
}

void power_tick(int64_t now_ms) {
  read_state(now_ms);
  if (!st.keep_awake || !power_is_on()) {
    return;
  }
  if (!st.armed) {
    st.armed = true;
    st.next_ms = now_ms + PS5H_KEEP_AWAKE_S * 1000;
    return;
  }
  if (now_ms < st.next_ms) {
    return;
  }
  /* a failure waits for the next interval, too: no call storm */
  st.next_ms = now_ms + PS5H_KEEP_AWAKE_S * 1000;
  if (plat_keep_awake() < 0) {
    log_error("keep-awake call failed");
  }
}

bool power_is_on(void) {
  if (!st.ticked || st.missing || st.fail_open) {
    return true;
  }
  return st.state == STATE_ON;
}

const char *power_state_name(void) {
  if (st.missing || st.fail_open) {
    return "unknown";
  }
  switch (st.state) {
  case STATE_ON: return "on";
  case STATE_GOING_TO_REST: return "going_to_rest";
  case STATE_REST: return "rest";
  case STATE_SHUTTING_DOWN: return "shutting_down";
  default: return "unknown";
  }
}

void power_set_keep_awake(bool on) {
  if (on && !st.keep_awake) {
    st.armed = false; /* first call one interval after switching on */
  }
  st.keep_awake = on;
}

bool power_keep_awake(void) { return st.keep_awake; }
