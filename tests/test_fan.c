/* Fan threshold and watcher.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include "config.h"
#include "fan.h"
#include "json.h"
#include "limits.h"
#include "log.h"
#include "platform_fake.h"

static config cfg;

static void setup(int device_value) {
  fake_reset();
  snprintf(fake.root, sizeof(fake.root), "%s", t_tmpdir());
  config_load(&cfg);
  fake.fan_value = device_value;
  fan_init(&cfg);
}

static void tick(int seconds) {
  fake.mono_ms += seconds * 1000;
  fan_tick(&cfg, fake.mono_ms);
}

static void test_fan_set_allowed_without_config(void) {
  int rc;
  setup(91); /* fresh config: no permission switch anywhere */
  T_EQ_INT(fan_set(&cfg, 70, &rc), FAN_OK);
  T_EQ_INT(fake.fan_value, 70);
}

static void test_fan_unavailable(void) {
  int rc;
  setup(91);
  fake.caps.fan_control = false;
  T_EQ_INT(fan_set(&cfg, 70, &rc), FAN_UNAVAILABLE);
  T_EQ_INT(fake.fan_set_calls, 0);
}

static void test_fan_learns_stock_once(void) {
  config d;
  setup(91);
  T_EQ_INT(cfg.fan_stock_c, 91);
  config_load(&d);
  T_EQ_INT(d.fan_stock_c, 91); /* saved */
  fake.fan_value = 80;
  fan_init(&cfg);
  T_EQ_INT(cfg.fan_stock_c, 91); /* not re-learned */
}

static void test_fan_stock_not_overwritten_by_own_target(void) {
  int rc;
  fake_reset();
  snprintf(fake.root, sizeof(fake.root), "%s", t_tmpdir());
  config_load(&cfg);
  cfg.fan_target_c = 70; /* payload restarted while our 70 is still set */
  fake.fan_value = 70;
  fan_init(&cfg);
  T_EQ_INT(cfg.fan_stock_c, 0); /* our own value is not the factory value */
  T_EQ_INT(fan_set(&cfg, 0, &rc), FAN_OK);
  T_EQ_INT(fake.fan_set_calls, 0); /* unknown stock: nothing written back */
}

static void test_fan_range_and_above_stock(void) {
  int rc;
  setup(80); /* a board whose factory value is 80 */
  T_EQ_INT(fan_set(&cfg, 44, &rc), FAN_BAD_VALUE);
  T_EQ_INT(fan_set(&cfg, 86, &rc), FAN_BAD_VALUE);
  T_EQ_INT(fan_set(&cfg, 81, &rc), FAN_ABOVE_STOCK);
  T_EQ_INT(fan_set(&cfg, 80, &rc), FAN_OK);
  T_EQ_INT(fan_set(&cfg, 45, &rc), FAN_OK);
  T_EQ_INT(fake.fan_value, 45);
  T_EQ_INT(cfg.fan_target_c, 45);
}

static void test_fan_reset_to_stock(void) {
  int rc;
  setup(91);
  T_EQ_INT(fan_set(&cfg, 70, &rc), FAN_OK);
  T_EQ_INT(fake.fan_value, 70);
  T_EQ_INT(fan_set(&cfg, 0, &rc), FAN_OK);
  T_EQ_INT(fake.fan_value, 91);
  T_EQ_INT(cfg.fan_target_c, 0);
  fake.fan_value = 91;
  tick(10);
  T_EQ_INT(fake.fan_value, 91); /* watcher idle */
}

static void test_fan_tick_restores_after_console_reset(void) {
  int rc, calls;
  setup(91);
  fan_set(&cfg, 70, &rc);
  calls = fake.fan_set_calls;
  tick(2);
  T_EQ_INT(fake.fan_set_calls, calls); /* checks only every 5 s */
  fake.fan_value = 91; /* game start: console back to its own value */
  tick(5);
  T_EQ_INT(fake.fan_value, 70);
  T_EQ_INT(fake.fan_set_calls, calls + 1);
}

static void test_fan_other_tool_stops(void) {
  int rc, calls;
  setup(91);
  fan_set(&cfg, 70, &rc);
  calls = fake.fan_set_calls;
  fake.fan_value = 75; /* ps5upload or etaHEN set their own */
  tick(5);
  tick(5);
  T_EQ_INT(fake.fan_value, 75);
  T_EQ_INT(fake.fan_set_calls, calls);
  T_ASSERT(log_last_error() && strstr(log_last_error(), "another tool"));
  /* an explicit set takes over again */
  T_EQ_INT(fan_set(&cfg, 72, &rc), FAN_OK);
  T_EQ_INT(fake.fan_value, 72);
}

static void test_fan_gives_up_after_5_failures(void) {
  int rc, calls;
  setup(91);
  fan_set(&cfg, 70, &rc);
  fake.fan_value = 91;
  fake.fan_set_rc = -1;
  for (int i = 0; i < 5; i++) {
    tick(5);
  }
  calls = fake.fan_set_calls;
  tick(5);
  tick(5);
  T_EQ_INT(fake.fan_set_calls, calls);
  T_ASSERT(log_last_error() != NULL);
}

static void test_fan_unknown_stock_zero_writes_nothing(void) {
  int rc;
  setup(120); /* implausible reading: factory value unknown */
  T_EQ_INT(cfg.fan_stock_c, 0);
  T_EQ_INT(fan_set(&cfg, 85, &rc), FAN_OK); /* only the fixed range applies */
  T_EQ_INT(fan_set(&cfg, 0, &rc), FAN_OK);
  T_EQ_INT(fake.fan_value, 85);
}

static void test_fan_json(void) {
  jw_t w;
  int rc;
  setup(91);
  fan_set(&cfg, 70, &rc);
  jw_init(&w);
  jw_obj(&w);
  fan_json(&w, &cfg);
  jw_end_obj(&w);
  T_EQ_STR(w.buf, "{\"fan\":{\"target_c\":70,\"current_c\":70,\"stock_c\":91}}");
  jw_free(&w);
  setup(91);
  jw_init(&w);
  jw_obj(&w);
  fan_json(&w, &cfg);
  jw_end_obj(&w);
  T_EQ_STR(w.buf, "{\"fan\":{\"target_c\":null,\"current_c\":91,\"stock_c\":91}}");
  jw_free(&w);
}

static void test_fan_device_missing(void) {
  jw_t w;
  int rc;
  fake_reset();
  snprintf(fake.root, sizeof(fake.root), "%s", t_tmpdir());
  config_load(&cfg);
  fake.fan_get_rc = -1;
  fan_init(&cfg);
  T_EQ_INT(fan_set(&cfg, 70, &rc), FAN_OK); /* write may still work */
  jw_init(&w);
  jw_obj(&w);
  fan_json(&w, &cfg);
  jw_end_obj(&w);
  T_ASSERT(strstr(w.buf, "\"stock_c\":null"));
  jw_free(&w);
}


/* --- review fixes ---------------------------------------------------- */

static void restart(void) { fan_init(&cfg); }

static void test_fan_never_learns_own_written_value(void) {
  int rc;
  fake_reset();
  snprintf(fake.root, sizeof(fake.root), "%s", t_tmpdir());
  config_load(&cfg);
  fake.fan_get_rc = -1; /* first read fails: factory value unknown */
  fan_init(&cfg);
  T_EQ_INT(fan_set(&cfg, 70, &rc), FAN_OK);
  T_EQ_INT(fan_set(&cfg, 0, &rc), FAN_OK); /* unknown: 70 stays on the device */
  fake.fan_get_rc = 0;
  restart();
  T_EQ_INT(cfg.fan_stock_c, 0); /* 70 is our own value, never the factory one */
}

static void test_fan_target_above_learned_stock_is_cleared(void) {
  int rc;
  fake_reset();
  snprintf(fake.root, sizeof(fake.root), "%s", t_tmpdir());
  config_load(&cfg);
  fake.fan_get_rc = -1;
  fan_init(&cfg);
  T_EQ_INT(fan_set(&cfg, 85, &rc), FAN_OK);
  fake.fan_get_rc = 0;
  fake.fan_value = 69; /* reboot: the console's own value on this board */
  restart();
  T_EQ_INT(cfg.fan_stock_c, 69);
  T_EQ_INT(cfg.fan_target_c, 0); /* 85 would be hotter than Sony intends */
  tick(5);
  T_EQ_INT(fake.fan_value, 69);
}

static void test_fan_fight_detected(void) {
  int rc, calls;
  setup(70); /* another tool had set 70 before our first start */
  T_EQ_INT(cfg.fan_stock_c, 70);
  T_EQ_INT(fan_set(&cfg, 60, &rc), FAN_OK);
  for (int i = 0; i < 6; i++) {
    fake.fan_value = 70; /* the other tool puts its value back */
    tick(5);
  }
  calls = fake.fan_set_calls;
  fake.fan_value = 70;
  tick(5);
  tick(5);
  T_EQ_INT(fake.fan_set_calls, calls);
  T_ASSERT(calls <= 4); /* first set + at most 3 restores */
  T_ASSERT(log_last_error() && strstr(log_last_error(), "another tool"));
}

static void test_fan_stock_above_91_unknown(void) {
  setup(95);
  T_EQ_INT(cfg.fan_stock_c, 0);
}

static void test_fan_current_updates_without_target(void) {
  jw_t w;
  setup(91);
  fake.fan_value = 70; /* changed by someone else */
  tick(5);
  jw_init(&w);
  jw_obj(&w);
  fan_json(&w, &cfg);
  jw_end_obj(&w);
  T_ASSERT(strstr(w.buf, "\"current_c\":70"));
  jw_free(&w);
}

int main(void) {
  T_RUN(test_fan_set_allowed_without_config);
  T_RUN(test_fan_unavailable);
  T_RUN(test_fan_learns_stock_once);
  T_RUN(test_fan_stock_not_overwritten_by_own_target);
  T_RUN(test_fan_range_and_above_stock);
  T_RUN(test_fan_reset_to_stock);
  T_RUN(test_fan_tick_restores_after_console_reset);
  T_RUN(test_fan_other_tool_stops);
  T_RUN(test_fan_gives_up_after_5_failures);
  T_RUN(test_fan_unknown_stock_zero_writes_nothing);
  T_RUN(test_fan_json);
  T_RUN(test_fan_device_missing);
  T_RUN(test_fan_never_learns_own_written_value);
  T_RUN(test_fan_target_above_learned_stock_is_cleared);
  T_RUN(test_fan_fight_detected);
  T_RUN(test_fan_stock_above_91_unknown);
  T_RUN(test_fan_current_updates_without_target);
  T_MAIN();
}
