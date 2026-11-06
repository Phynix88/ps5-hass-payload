/* Permissions granted from Home Assistant (design v0.3 §5).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include <sys/stat.h>

#include "auth.h"
#include "config.h"
#include "fan.h"
#include "json.h"
#include "limits.h"
#include "log.h"
#include "perm.h"
#include "platform_fake.h"

static config cfg;
static char dir[760];

static void setup(void) {
  fake_reset();
  snprintf(fake.root, sizeof(fake.root), "%s", t_tmpdir());
  snprintf(dir, sizeof(dir), "%s%s", fake.root, PS5H_DATA_DIR);
  config_load(&cfg);
  config_save(&cfg);
  fan_init(&cfg);
}

static int parse(const char *src, unsigned *out) {
  static jsmntok_t tok[32];
  jr_t r;
  if (jr_parse(&r, src, strlen(src), tok, 32)) {
    return -2; /* test input broken */
  }
  return perm_parse(&r, jr_get(&r, 0, "permissions"), out);
}

static void test_parse_valid_names(void) {
  unsigned p = 0;
  setup();
  T_EQ_INT(parse("{\"permissions\":[\"power_off\",\"fan_control\"]}", &p), 0);
  T_EQ_INT(p, PERM_POWER_OFF | PERM_FAN_CONTROL);
  T_EQ_INT(parse("{\"permissions\":[\"power_actions\"]}", &p), 0);
  T_EQ_INT(p, PERM_POWER_ACTIONS);
}

static void test_parse_rejects_empty_unknown_duplicate_nonarray(void) {
  unsigned p;
  setup();
  T_EQ_INT(parse("{\"permissions\":[]}", &p), -1);
  T_EQ_INT(parse("{\"permissions\":[\"x\"]}", &p), -1);
  T_EQ_INT(parse("{\"permissions\":[\"power_off\",\"power_off\"]}", &p), -1);
  T_EQ_INT(parse("{\"permissions\":\"power_off\"}", &p), -1);
  T_EQ_INT(parse("{\"permissions\":[1]}", &p), -1);
  T_EQ_INT(parse("{\"permissions\":[[\"power_off\"]]}", &p), -1);
  T_EQ_INT(parse("{\"other\":[\"power_off\"]}", &p), -1);
}

static void test_available_follows_caps(void) {
  setup();
  T_EQ_INT(perm_available(), PERM_ALL);
  fake.caps.power_off = false;
  T_EQ_INT(perm_available(), PERM_POWER_ACTIONS | PERM_FAN_CONTROL);
  fake.caps.standby = false;
  fake.caps.fan_control = false;
  T_EQ_INT(perm_available(), 0);
}

static void test_apply_grant_saves(void) {
  config d;
  setup();
  T_EQ_INT(perm_apply(&cfg, PERM_POWER_OFF, 0), 0);
  T_ASSERT(cfg.power_off);
  T_EQ_INT(perm_allowed(&cfg), PERM_POWER_OFF);
  config_load(&d);
  T_ASSERT(d.power_off);
  T_ASSERT(!d.power_actions && !d.fan_control);
}

static void test_apply_save_fails_rolls_back(void) {
  setup();
  T_EQ_INT(chmod(dir, 0500), 0);
  T_EQ_INT(perm_apply(&cfg, PERM_POWER_OFF | PERM_POWER_ACTIONS, 0), -1);
  T_EQ_INT(chmod(dir, 0700), 0);
  T_ASSERT(!cfg.power_off && !cfg.power_actions);
}

static void fan_on_at(int target) {
  int rc;
  T_EQ_INT(perm_apply(&cfg, PERM_FAN_CONTROL, 0), 0);
  T_EQ_INT(fan_set(&cfg, target, &rc), FAN_OK);
  T_EQ_INT(fake.fan_value, target);
}

static void test_apply_revoke_fan_resets_after_save(void) {
  config d;
  setup();
  fan_on_at(70);
  T_EQ_INT(perm_apply(&cfg, 0, PERM_FAN_CONTROL), 0);
  T_EQ_INT(fake.fan_value, 91);
  T_ASSERT(!cfg.fan_control);
  T_EQ_INT(cfg.fan_target_c, 0);
  config_load(&d);
  T_ASSERT(!d.fan_control);
  T_EQ_INT(d.fan_target_c, 0);
}

static void test_apply_revoke_fan_save_fails_fan_untouched(void) {
  int calls;
  setup();
  fan_on_at(70);
  calls = fake.fan_set_calls;
  T_EQ_INT(chmod(dir, 0500), 0);
  T_EQ_INT(perm_apply(&cfg, 0, PERM_FAN_CONTROL), -1);
  T_EQ_INT(chmod(dir, 0700), 0);
  T_EQ_INT(fake.fan_value, 70);
  T_EQ_INT(fake.fan_set_calls, calls);
  T_ASSERT(cfg.fan_control);
  T_EQ_INT(cfg.fan_target_c, 70);
}

static void test_apply_revoke_fan_write_fails_still_revoked(void) {
  setup();
  fan_on_at(70);
  fake.fan_set_rc = -1;
  T_EQ_INT(perm_apply(&cfg, 0, PERM_FAN_CONTROL), 0);
  T_ASSERT(!cfg.fan_control);
  T_ASSERT(strstr(log_last_error(), "stays until the console resets it"));
}

static void test_apply_revoke_not_granted_no_fan_write(void) {
  int calls;
  setup();
  calls = fake.fan_set_calls;
  T_EQ_INT(perm_apply(&cfg, 0, PERM_ALL), 0);
  T_EQ_INT(fake.fan_set_calls, calls);
}

static void test_text_order(void) {
  char t[64];
  perm_text(PERM_ALL, t, sizeof(t));
  T_EQ_STR(t, "rest mode, power off, fan threshold");
  perm_text(PERM_FAN_CONTROL | PERM_POWER_ACTIONS, t, sizeof(t));
  T_EQ_STR(t, "rest mode, fan threshold");
  perm_text(PERM_POWER_OFF, t, sizeof(t));
  T_EQ_STR(t, "power off");
}

static void test_json_shape(void) {
  jw_t w;
  setup();
  fake.caps.power_off = false;
  cfg.power_actions = true;
  jw_init(&w);
  jw_obj(&w);
  perm_json(&w, &cfg);
  jw_end_obj(&w);
  T_ASSERT(!w.failed);
  T_ASSERT(strstr(w.buf, "\"permissions\":{\"power_actions\":{\"allowed\":true,"
                         "\"available\":true},\"power_off\":{\"allowed\":false,"
                         "\"available\":false},\"fan_control\":{\"allowed\":false,"
                         "\"available\":true}}"));
  jw_free(&w);
}

int main(void) {
  T_RUN(test_parse_valid_names);
  T_RUN(test_parse_rejects_empty_unknown_duplicate_nonarray);
  T_RUN(test_available_follows_caps);
  T_RUN(test_apply_grant_saves);
  T_RUN(test_apply_save_fails_rolls_back);
  T_RUN(test_apply_revoke_fan_resets_after_save);
  T_RUN(test_apply_revoke_fan_save_fails_fan_untouched);
  T_RUN(test_apply_revoke_fan_write_fails_still_revoked);
  T_RUN(test_apply_revoke_not_granted_no_fan_write);
  T_RUN(test_text_order);
  T_RUN(test_json_shape);
  T_MAIN();
}
