/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include "platform.h"
#include "platform_fake.h"

static void test_fake_clock(void) {
  fake_reset();
  fake.mono_ms = 5000;
  fake.wall_s = 1790000000;
  T_EQ_INT(plat_mono_ms(), 5000);
  T_EQ_INT(plat_wall_s(), 1790000000);
}

static void test_fake_random_deterministic(void) {
  uint8_t a[16], b[16];
  fake_reset();
  T_EQ_INT(plat_random(a, sizeof(a)), 0);
  fake_reset();
  T_EQ_INT(plat_random(b, sizeof(b)), 0);
  T_ASSERT(memcmp(a, b, sizeof(a)) == 0);
  /* consecutive calls differ */
  T_EQ_INT(plat_random(b, sizeof(b)), 0);
  T_ASSERT(memcmp(a, b, sizeof(a)) != 0);
  fake.random_rc = -1;
  T_ASSERT(plat_random(a, sizeof(a)) < 0);
}

static void test_fake_launch_requires_folder(void) {
  char path[600];
  plat_running r;
  fake_reset();
  snprintf(fake.root, sizeof(fake.root), "%s", t_tmpdir());
  T_ASSERT(plat_launch("PPSA00001") < 0);
  T_EQ_INT(plat_running_app(&r), 0);

  snprintf(path, sizeof(path), "%s/user/app/PPSA00001", fake.root);
  T_EQ_INT(t_mkdirs(path), 0);
  T_ASSERT(plat_launch("PPSA00001") >= 0);
  T_EQ_INT(plat_running_app(&r), 1);
  T_EQ_STR(r.title_id, "PPSA00001");
  T_EQ_INT(plat_kill_app(r.app_id), 0);
  T_EQ_INT(plat_running_app(&r), 0);
}

static void test_fake_notify_records(void) {
  char path[600];
  fake_reset();
  snprintf(fake.root, sizeof(fake.root), "%s", t_tmpdir());
  T_EQ_INT(plat_notify("hello 123456"), 0);
  T_EQ_STR(fake.last_notify, "hello 123456");
  T_EQ_INT(fake.notify_count, 1);
  snprintf(path, sizeof(path), "%s/fake-notify.txt", fake.root);
  T_EQ_STR(t_read(path), "hello 123456");
  fake.notify_rc = -5;
  T_EQ_INT(plat_notify("x"), -5);
  T_EQ_INT(fake.notify_count, 1);
}

static void test_new_functions_count_sony_calls(void) {
  int app = 0;
  char title[10];
  plat_soc_power soc;
  fake_reset();
  fake.power_state = 500;
  fake.focused_app_id = 0x2007;
  fake.soc_power.ok = true;
  fake.soc_power.mw[0] = 15000;
  fake.soc_power.mv[0] = 900;
  fake.soc_power.ma[0] = 16000;
  snprintf(fake.title_id, sizeof(fake.title_id), "PPSA00001");

  T_EQ_INT(plat_power_state(), 500);
  T_EQ_INT(fake.sony_calls, 0); /* the state read is not a "sony call" */
  T_EQ_INT(fake.power_state_calls, 1);

  T_EQ_INT(plat_focused_app(&app), 0);
  T_EQ_INT(app, 0x2007);
  T_EQ_INT(plat_suspend_app(123), 0);
  T_EQ_INT(fake.suspended_app_id, 123);
  T_EQ_INT(plat_keep_awake(), 0);
  T_EQ_INT(plat_soc_power_read(&soc), 0);
  T_ASSERT(soc.ok);
  T_EQ_INT(soc.mw[0], 15000);
  T_EQ_INT(plat_title_of(123, title), 0);
  T_EQ_STR(title, "PPSA00001");

  T_EQ_INT(fake.sony_calls, 5);
  T_EQ_INT(fake.focused_app_calls, 1);
  T_EQ_INT(fake.suspend_calls, 1);
  T_EQ_INT(fake.keep_awake_calls, 1);
  T_EQ_INT(fake.soc_power_calls, 1);
  T_EQ_INT(fake.title_of_calls, 1);
  T_EQ_INT(plat_power_state(), 500);
  T_EQ_INT(fake.sony_calls, 5);
}

static void test_new_functions_report_errors(void) {
  int app = 7;
  plat_soc_power soc;
  fake_reset();
  fake.power_state_rc = -3;
  T_EQ_INT(plat_power_state(), -3);
  fake.focused_app_rc = -4;
  T_EQ_INT(plat_focused_app(&app), -4);
  fake.suspend_rc = -5;
  T_EQ_INT(plat_suspend_app(1), -5);
  fake.keep_awake_rc = -6;
  T_EQ_INT(plat_keep_awake(), -6);
  fake.soc_power_rc = -7;
  T_EQ_INT(plat_soc_power_read(&soc), -7);
  T_ASSERT(!soc.ok);
  T_EQ_INT(fake.sony_calls, 4); /* failed calls still reached the console */
}

static void test_new_functions_missing_capability(void) {
  int app = 0;
  plat_soc_power soc;
  fake_reset();
  fake.caps.focus = fake.caps.suspend = fake.caps.keep_awake = false;
  fake.caps.soc_power = fake.caps.power_state = false;
  T_ASSERT(plat_power_state() < 0);
  T_ASSERT(plat_focused_app(&app) < 0);
  T_ASSERT(plat_suspend_app(1) < 0);
  T_ASSERT(plat_keep_awake() < 0);
  T_ASSERT(plat_soc_power_read(&soc) < 0);
  T_EQ_INT(fake.sony_calls, 0); /* nothing reached the console */
}

int main(void) {
  T_RUN(test_fake_clock);
  T_RUN(test_fake_random_deterministic);
  T_RUN(test_fake_launch_requires_folder);
  T_RUN(test_fake_notify_records);
  T_RUN(test_new_functions_count_sony_calls);
  T_RUN(test_new_functions_report_errors);
  T_RUN(test_new_functions_missing_capability);
  T_MAIN();
}
