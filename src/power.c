/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "power.h"

#include "limits.h"
#include "log.h"
#include "platform.h"

#define STATE_ON 1000
#define STATE_GOING_TO_REST 300
#define STATE_REST 500
#define STATE_SHUTTING_DOWN 100

static struct {
  int state; /* last raw value; < 0 = unknown or read failed */
  bool keep_awake;
  bool armed; /* next_ms is valid */
  int64_t next_ms;
} st = {.state = -1};

void power_init(void) {
  st.state = -1;
  st.keep_awake = false;
  st.armed = false;
  st.next_ms = 0;
}

void power_tick(int64_t now_ms) {
  int s = plat_power_state();
  if (s != st.state && s >= 0) {
    log_msg("power state %d", s);
  }
  st.state = s;
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

bool power_is_on(void) { return st.state < 0 || st.state == STATE_ON; }

const char *power_state_name(void) {
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
