/* Power state, keep-awake and the rule that the console is left alone while
 * it is not "on".
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include "apps.h"
#include "auth.h"
#include "config.h"
#include "control.h"
#include "fan.h"
#include "limits.h"
#include "platform_fake.h"
#include "power.h"
#include "router.h"
#include "server.h"
#include "state.h"

static config cfg;
static app_list apps;
static route_ctx ctx;

static void setup(void) {
  fake_reset();
  snprintf(fake.root, sizeof(fake.root), "%s", t_tmpdir());
  config_load(&cfg);
  auth_init(&cfg);
  control_init();
  power_init();
  info_cache_reset();
  apps_free(&apps);
  apps_scan(&apps);
  memset(&ctx, 0, sizeof(ctx));
  ctx.cfg = &cfg;
  ctx.apps = &apps;
  ctx.device_id = "00112233445566778899aabbccddeeff";
}

static void test_power_state_names(void) {
  static const struct {
    int raw;
    const char *name;
  } c[] = {{1000, "on"}, {300, "going_to_rest"}, {500, "rest"},
           {100, "shutting_down"}, {42, "unknown"}, {0, "unknown"}};
  setup();
  for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
    fake.power_state = c[i].raw;
    power_tick(fake.mono_ms);
    T_EQ_STR(power_state_name(), c[i].name);
    T_EQ_INT(power_is_on(), c[i].raw == 1000);
  }
}

/* A failed read must not open the gate: the console may be falling asleep
 * (spec: no console call outside "on"). */
static void test_read_error_after_going_to_rest_stays_not_on(void) {
  setup();
  fake.power_state = 300;
  power_tick(fake.mono_ms);
  T_ASSERT(!power_is_on());
  fake.power_state_rc = -1;
  power_tick(fake.mono_ms + 250);
  T_ASSERT(!power_is_on());
  T_EQ_STR(power_state_name(), "going_to_rest");
  power_tick(fake.mono_ms + 30000);
  T_ASSERT(!power_is_on());
}

static void test_read_error_after_on_stays_on(void) {
  setup();
  power_tick(fake.mono_ms);
  T_ASSERT(power_is_on());
  fake.power_state_rc = -1;
  power_tick(fake.mono_ms + 250);
  T_ASSERT(power_is_on());
  T_EQ_STR(power_state_name(), "on");
}

static void test_read_error_before_any_read_is_not_on(void) {
  setup();
  fake.power_state_rc = -1;
  power_tick(fake.mono_ms);
  T_ASSERT(!power_is_on());
  T_EQ_STR(power_state_name(), "unknown");
}

static void test_errors_for_60s_fall_back_to_on(void) {
  int64_t t;
  setup();
  t = fake.mono_ms;
  fake.power_state = 500;
  power_tick(t);
  fake.power_state_rc = -1;
  for (int64_t ms = 250; ms < PS5H_POWER_FAIL_OPEN_S * 1000 + 250; ms += 250) {
    power_tick(t + ms);
  }
  T_ASSERT(!power_is_on()); /* first failure at +250: 59.75 s of failures */
  power_tick(t + PS5H_POWER_FAIL_OPEN_S * 1000 + 250);
  T_ASSERT(power_is_on());
  T_EQ_STR(power_state_name(), "unknown");
  /* a good read counts again at once */
  fake.power_state_rc = 0;
  power_tick(t + PS5H_POWER_FAIL_OPEN_S * 1000 + 500);
  T_ASSERT(!power_is_on());
  T_EQ_STR(power_state_name(), "rest");
  /* and a new run of failures starts its own 60 s */
  fake.power_state_rc = -1;
  power_tick(t + PS5H_POWER_FAIL_OPEN_S * 1000 + 750);
  T_ASSERT(!power_is_on());
}

/* The monotonic clock keeps running in rest mode while the process is
 * frozen: a long gap is no evidence of 60 s of failures. */
static void test_gap_after_thaw_does_not_open_the_gate(void) {
  server *s;
  int before_sony, before_reads;
  setup();
  s = server_new(&ctx, 0);
  T_ASSERT(s != NULL);
  fake.running = (plat_running){.app_id = 7, .title_id = "PPSA00001"};
  server_step(s, 0);
  fake.power_state = 300;
  fake.mono_ms += 250;
  server_step(s, 0);
  T_ASSERT(!power_is_on());
  fake.power_state_rc = -1; /* the read fails while falling asleep ... */
  fake.mono_ms += 250;
  server_step(s, 0);
  T_ASSERT(!power_is_on());
  fake.mono_ms += 10 * 60 * 1000; /* ... the process is frozen ... */
  server_step(s, 0); /* ... and its first read after thawing fails, too */
  T_ASSERT(!power_is_on());
  T_EQ_STR(power_state_name(), "going_to_rest");
  before_sony = fake.sony_calls;
  before_reads = fake.reads;
  fake.mono_ms += 250;
  server_step(s, 0);
  T_EQ_INT(fake.sony_calls, before_sony);
  T_EQ_INT(fake.reads, before_reads);
  server_free(s);
}

static void test_240_failed_reads_open_the_gate(void) {
  int64_t t;
  setup();
  t = fake.mono_ms;
  fake.power_state = 500;
  power_tick(t);
  fake.power_state_rc = -1;
  for (int i = 1; i <= PS5H_POWER_FAIL_OPEN_READS; i++) {
    power_tick(t + i * (int64_t)PS5H_POWER_READ_MS);
  }
  T_ASSERT(!power_is_on()); /* 240 reads, but only 59.75 s apart */
  power_tick(t + (PS5H_POWER_FAIL_OPEN_READS + 1) * (int64_t)PS5H_POWER_READ_MS);
  T_ASSERT(power_is_on());
  T_EQ_STR(power_state_name(), "unknown");
}

static void test_a_gap_restarts_the_failure_run(void) {
  int64_t t;
  setup();
  t = fake.mono_ms;
  fake.power_state = 500;
  power_tick(t);
  fake.power_state_rc = -1;
  for (int i = 1; i < PS5H_POWER_FAIL_OPEN_READS; i++) { /* 239 reads */
    power_tick(t + i * (int64_t)PS5H_POWER_READ_MS);
  }
  T_ASSERT(!power_is_on());
  t += (PS5H_POWER_FAIL_OPEN_READS - 1) * (int64_t)PS5H_POWER_READ_MS +
       (PS5H_SUSPEND_GAP_S + 5) * 1000;
  for (int i = 0; i < PS5H_POWER_FAIL_OPEN_READS; i++) {
    power_tick(t + i * (int64_t)PS5H_POWER_READ_MS);
    T_ASSERT(!power_is_on()); /* a whole new run is needed: 240 more reads */
  }
  power_tick(t + PS5H_POWER_FAIL_OPEN_READS * (int64_t)PS5H_POWER_READ_MS);
  T_ASSERT(power_is_on());
}

static void test_a_good_read_resets_the_failure_run(void) {
  int64_t t;
  setup();
  t = fake.mono_ms;
  fake.power_state = 500;
  power_tick(t);
  fake.power_state_rc = -1;
  for (int i = 1; i < PS5H_POWER_FAIL_OPEN_READS; i++) {
    power_tick(t + i * (int64_t)PS5H_POWER_READ_MS);
  }
  fake.power_state_rc = 0;
  t += PS5H_POWER_FAIL_OPEN_READS * (int64_t)PS5H_POWER_READ_MS;
  power_tick(t);
  T_EQ_STR(power_state_name(), "rest");
  fake.power_state_rc = -1;
  for (int i = 1; i <= PS5H_POWER_FAIL_OPEN_READS; i++) {
    power_tick(t + i * (int64_t)PS5H_POWER_READ_MS);
    T_ASSERT(!power_is_on()); /* counted from zero again */
  }
  power_tick(t + (PS5H_POWER_FAIL_OPEN_READS + 1) * (int64_t)PS5H_POWER_READ_MS);
  T_ASSERT(power_is_on());
}

static void test_missing_capability_counts_as_on(void) {
  setup();
  fake.power_state = 300;
  fake.caps.power_state = false;
  power_tick(fake.mono_ms);
  T_ASSERT(power_is_on()); /* as before v0.4: a console without the function */
  T_EQ_STR(power_state_name(), "unknown");
}

static void test_state_is_on_before_first_tick(void) {
  setup();
  T_ASSERT(power_is_on());
  T_EQ_STR(power_state_name(), "unknown");
}

static void test_keep_awake_ticks_every_240s_only_when_on(void) {
  int64_t t;
  setup();
  t = fake.mono_ms;
  T_ASSERT(!power_keep_awake());
  power_tick(t);
  T_EQ_INT(fake.keep_awake_calls, 0); /* off: nothing */
  power_set_keep_awake(true);
  T_ASSERT(power_keep_awake());
  power_tick(t); /* arms the timer */
  power_tick(t + (PS5H_KEEP_AWAKE_S * 1000) - 1);
  T_EQ_INT(fake.keep_awake_calls, 0);
  power_tick(t + PS5H_KEEP_AWAKE_S * 1000);
  T_EQ_INT(fake.keep_awake_calls, 1);
  power_tick(t + PS5H_KEEP_AWAKE_S * 1000 + 1000);
  T_EQ_INT(fake.keep_awake_calls, 1);
  power_tick(t + 2 * PS5H_KEEP_AWAKE_S * 1000);
  T_EQ_INT(fake.keep_awake_calls, 2);
  /* rest mode: nothing, however long */
  fake.power_state = 500;
  power_tick(t + 3 * PS5H_KEEP_AWAKE_S * 1000);
  power_tick(t + 9 * PS5H_KEEP_AWAKE_S * 1000);
  T_EQ_INT(fake.keep_awake_calls, 2);
  T_ASSERT(power_keep_awake()); /* the switch stays */
  /* back on: ticks again */
  fake.power_state = 1000;
  power_tick(t + 10 * PS5H_KEEP_AWAKE_S * 1000);
  T_EQ_INT(fake.keep_awake_calls, 3);
  power_set_keep_awake(false);
  power_tick(t + 20 * PS5H_KEEP_AWAKE_S * 1000);
  T_EQ_INT(fake.keep_awake_calls, 3);
}

static void test_keep_awake_failure_does_not_spin(void) {
  int64_t t;
  setup();
  t = fake.mono_ms;
  fake.keep_awake_rc = -1;
  power_set_keep_awake(true);
  power_tick(t);
  power_tick(t + PS5H_KEEP_AWAKE_S * 1000);
  power_tick(t + PS5H_KEEP_AWAKE_S * 1000 + 250);
  T_EQ_INT(fake.keep_awake_calls, 1);
}

static void test_state_json_while_on_reads_console(void) {
  jw_t w;
  setup();
  power_tick(fake.mono_ms);
  T_EQ_INT(fake.reads, 0);
  jw_init(&w);
  jw_obj(&w);
  state_json(&w, &apps, 0);
  jw_end_obj(&w);
  jw_free(&w);
  T_ASSERT(fake.reads > 0); /* the counter does count, so zero means zero */
}

static void test_info_asks_the_console_once_and_only_while_on(void) {
  jw_t w;
  setup();
  fake.power_state = 500;
  power_tick(fake.mono_ms);
  jw_init(&w);
  jw_obj(&w);
  info_json(&w, &cfg, ctx.device_id, true);
  jw_end_obj(&w);
  T_ASSERT(strstr(w.buf, "\"model\":\"\""));
  jw_free(&w);
  T_EQ_INT(fake.reads, 0);
  fake.power_state = 1000;
  power_tick(fake.mono_ms);
  info_cache_refresh();
  T_ASSERT(fake.reads > 0);
  {
    int after = fake.reads;
    jw_init(&w);
    jw_obj(&w);
    info_json(&w, &cfg, ctx.device_id, true);
    jw_end_obj(&w);
    T_ASSERT(strstr(w.buf, "\"model\":\"CFI-TEST\""));
    jw_free(&w);
    fake.power_state = 300; /* and the cached value stays */
    power_tick(fake.mono_ms);
    jw_init(&w);
    jw_obj(&w);
    info_json(&w, &cfg, ctx.device_id, true);
    jw_end_obj(&w);
    T_ASSERT(strstr(w.buf, "\"model\":\"CFI-TEST\""));
    jw_free(&w);
    T_EQ_INT(fake.reads, after);
  }
}

static void test_sony_calls_stop_outside_on(void) {
  server *s;
  int before_sony, before_reads, before_cap, before_sets;
  char p[800];
  setup();
  s = server_new(&ctx, 0);
  T_ASSERT(s != NULL);
  fake.fan_value = 91;
  fan_init(&cfg);
  cfg.fan_target_c = 70; /* the watcher has work to do */
  fake.running = (plat_running){.app_id = 7, .title_id = "PPSA00001"};
  power_set_keep_awake(true);
  server_step(s, 0);

  /* the console goes to rest */
  fake.power_state = 300;
  server_step(s, 0);
  before_sony = fake.sony_calls;
  before_reads = fake.reads;
  before_cap = fake.keep_awake_calls;
  before_sets = fake.fan_set_calls;
  fake.fan_value = 91; /* something the watcher would put back */
  /* a new app appears: the rescan must wait, too */
  snprintf(p, sizeof(p), "%s/user/app/NEWA00001/sce_sys/param.json", fake.root);
  t_write_str(p, "{}");
  for (int i = 0; i < 240; i++) { /* 60 s */
    fake.mono_ms += 250;
    server_step(s, 0);
  }
  T_EQ_INT(apps_refresh(&apps), 0);
  T_EQ_INT(fake.sony_calls, before_sony);
  T_EQ_INT(fake.reads, before_reads);
  T_EQ_INT(fake.keep_awake_calls, before_cap);
  T_EQ_INT(fake.fan_set_calls, before_sets);
  T_ASSERT(power_keep_awake());

  /* back on: the watcher and keep-awake work again, the rescan catches up */
  fake.power_state = 1000;
  for (int i = 0; i < 24; i++) { /* 6 s */
    fake.mono_ms += 250;
    server_step(s, 0);
  }
  T_ASSERT(fake.reads > before_reads);
  T_EQ_INT(fake.fan_value, 70);
  T_EQ_INT(apps_refresh(&apps), 1);
  server_free(s);
}

static void test_apps_scan_with_unknown_language_is_redone(void) {
  /* scanned while not "on": no language lookup; names are fixed up later */
  char p[800];
  setup();
  fake.power_state = 500;
  power_tick(fake.mono_ms);
  snprintf(p, sizeof(p), "%s/user/app/NEWA00001/sce_sys/param.json", fake.root);
  t_write_str(p, "{\"localizedParameters\":{\"defaultLanguage\":\"en-US\","
                 "\"en-US\":{\"titleName\":\"New\"}}}");
  apps_free(&apps);
  fake.reads = 0;
  T_EQ_INT(apps_scan(&apps), 0);
  T_EQ_INT(fake.reads, 0);
  T_EQ_INT(apps_refresh(&apps), 0); /* still resting */
  fake.power_state = 1000;
  power_tick(fake.mono_ms);
  T_EQ_INT(apps_refresh(&apps), 1); /* redone with the language */
  T_EQ_INT(apps_refresh(&apps), 0);
}

int main(void) {
  T_RUN(test_power_state_names);
  T_RUN(test_read_error_after_going_to_rest_stays_not_on);
  T_RUN(test_read_error_after_on_stays_on);
  T_RUN(test_read_error_before_any_read_is_not_on);
  T_RUN(test_errors_for_60s_fall_back_to_on);
  T_RUN(test_gap_after_thaw_does_not_open_the_gate);
  T_RUN(test_240_failed_reads_open_the_gate);
  T_RUN(test_a_gap_restarts_the_failure_run);
  T_RUN(test_a_good_read_resets_the_failure_run);
  T_RUN(test_missing_capability_counts_as_on);
  T_RUN(test_state_is_on_before_first_tick);
  T_RUN(test_keep_awake_ticks_every_240s_only_when_on);
  T_RUN(test_keep_awake_failure_does_not_spin);
  T_RUN(test_state_json_while_on_reads_console);
  T_RUN(test_info_asks_the_console_once_and_only_while_on);
  T_RUN(test_sony_calls_stop_outside_on);
  T_RUN(test_apps_scan_with_unknown_language_is_redone);
  apps_free(&apps);
  T_MAIN();
}
