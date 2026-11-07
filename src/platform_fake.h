/* Fake console for tests: every field can be set directly by a test.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_PLATFORM_FAKE_H
#define PS5H_PLATFORM_FAKE_H

#include "platform.h"

struct fake_state {
  int64_t wall_s, mono_ms, uptime_s;
  int random_rc;
  uint64_t random_counter;
  int notify_rc;
  char last_notify[256];
  int notify_count;
  plat_running running; /* app_id 0 = nothing runs */
  int next_app_id;
  int launch_rc; /* < 0: launch fails with this code */
  int kill_rc;
  int user_rc;
  uint32_t user_id;
  int standby_rc;
  int standby_calls;
  plat_opt_int cpu, soc, fan;
  int language;
  int storage_rc;
  int64_t free_mb, total_mb;
  /* plat_storage_mb: paths under /mnt/ fail unless listed here (no drive);
   * a listed path answers with its own numbers (rc < 0: fails). */
  struct {
    char path[32];
    int rc;
    int64_t free_mb, total_mb;
  } storage_over[12];
  int n_storage_over;
  char model[64];
  char firmware[16];
  uint32_t ipv4[4];
  int n_ipv4;
  char root[512];
  plat_caps caps;
  plat_name procs[32];
  int n_procs;
  int power_off_rc;
  int power_off_calls;
  int fan_value;   /* what the "device" holds */
  int fan_get_rc;
  int fan_set_rc;
  int fan_set_calls;
  /* Every call that reaches the console, old and new (not the clocks, root,
   * capabilities, addresses, randomness, plat_power_state):
   * what a test checks to prove the console is left alone. */
  int reads;
  /* v0.4: every call below except plat_power_state() also adds to sony_calls
   * (what a test checks to prove the console is left alone). A capability
   * that is false makes the call fail without counting. */
  int sony_calls;
  int power_state; /* 1000 on */
  bool power_from_file; /* host server: <root>/fake-power-state overrides it */
  int power_state_rc;
  int power_state_calls;
  int focused_app_id;
  int focused_app_rc;
  int focused_app_calls;
  int focus_block_s; /* a call that blocks: both clocks advance by this */
  int running_block_s; /* same, for plat_running_app */
  int suspend_rc;
  int suspend_calls;
  int suspended_app_id; /* last app id passed to plat_suspend_app */
  int keep_awake_rc;
  int keep_awake_calls;
  plat_soc_power soc_power;
  int soc_power_rc;
  int soc_power_calls;
  char title_id[10]; /* "" = the running app's title id, if the id matches */
  int title_of_rc;
  int title_of_calls;
};

extern struct fake_state fake;

/* Defaults: idle console, user logged in, all sensors present, root "". */
void fake_reset(void);
/* PS5HASS_ROOT, PS5HASS_FAKE_NO_USER (for the host server). The host server
 * also follows the number in <root>/fake-power-state, if the file exists. */
void fake_init_from_env(void);

#endif
