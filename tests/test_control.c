/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include "apps.h"
#include "config.h"
#include "control.h"
#include "platform_fake.h"

static app_list apps;
static config cfg;

static void setup(void) {
  char cmd[700];
  fake_reset();
  snprintf(fake.root, sizeof(fake.root), "%s", t_tmpdir());
  snprintf(cmd, sizeof(cmd), "cp -R tests/data/apps/. '%s'", fake.root);
  if (system(cmd)) {
    exit(2);
  }
  apps_free(&apps);
  apps_scan(&apps);
  memset(&cfg, 0, sizeof(cfg));
  control_init();
}

static void test_launch_ok(void) {
  int rc = 0;
  setup();
  T_EQ_INT(control_launch(&apps, "PPSA00001", false, &rc), CTL_OK);
  T_EQ_INT(rc, 100);
  T_EQ_STR(fake.running.title_id, "PPSA00001");
}

static void test_launch_already_running(void) {
  int rc;
  setup();
  fake.running = (plat_running){7, "PPSA00001"};
  T_EQ_INT(control_launch(&apps, "PPSA00001", false, &rc), CTL_ALREADY_RUNNING);
  T_EQ_INT(fake.running.app_id, 7);
}

static void test_launch_other_running_409(void) {
  int rc;
  setup();
  fake.running = (plat_running){7, "PPSA00002"};
  T_EQ_INT(control_launch(&apps, "PPSA00001", false, &rc),
           CTL_OTHER_APP_RUNNING);
  T_EQ_STR(fake.running.title_id, "PPSA00002");
}

static void test_launch_replace_kills_first(void) {
  int rc;
  setup();
  fake.running = (plat_running){7, "PPSA00002"};
  T_EQ_INT(control_launch(&apps, "PPSA00001", true, &rc), CTL_OK);
  T_EQ_STR(fake.running.title_id, "PPSA00001");
  T_EQ_INT(fake.running.app_id, rc);
}

static void test_launch_replace_kill_fails(void) {
  int rc;
  setup();
  fake.running = (plat_running){7, "PPSA00002"};
  fake.kill_rc = -2137;
  T_EQ_INT(control_launch(&apps, "PPSA00001", true, &rc), CTL_SYSTEM_ERROR);
  T_EQ_INT(rc, -2137);
}

static void test_launch_not_installed(void) {
  int rc;
  setup();
  T_EQ_INT(control_launch(&apps, "PPSA99999", false, &rc), CTL_NOT_INSTALLED);
}

static void test_launch_no_user(void) {
  int rc;
  setup();
  fake.user_rc = -1;
  T_EQ_INT(control_launch(&apps, "PPSA00001", false, &rc), CTL_NO_USER);
  fake.user_rc = 0;
  fake.user_id = 0;
  T_EQ_INT(control_launch(&apps, "PPSA00001", false, &rc), CTL_NO_USER);
  T_EQ_INT(fake.running.app_id, 0);
}

static void test_launch_rc_negative_is_error(void) {
  int rc;
  setup();
  fake.launch_rc = (int)0x80940005;
  T_EQ_INT(control_launch(&apps, "PPSA00001", false, &rc), CTL_SYSTEM_ERROR);
  T_EQ_INT(rc, (int)0x80940005);
}

static void test_close_no_app(void) {
  int rc;
  setup();
  T_EQ_INT(control_close(NULL, &rc), CTL_NO_APP);
}

static void test_close_title_mismatch(void) {
  int rc;
  setup();
  fake.running = (plat_running){7, "PPSA00002"};
  T_EQ_INT(control_close("PPSA00001", &rc), CTL_TITLE_MISMATCH);
  T_EQ_INT(fake.running.app_id, 7);
}

static void test_close_ok(void) {
  int rc;
  setup();
  fake.running = (plat_running){7, "PPSA00002"};
  T_EQ_INT(control_close("PPSA00002", &rc), CTL_OK);
  T_EQ_INT(fake.running.app_id, 0);
  fake.running = (plat_running){8, "PPSA00002"};
  T_EQ_INT(control_close(NULL, &rc), CTL_OK);
}

static void test_suspend_running_game(void) {
  setup();
  fake.running = (plat_running){7, "PPSA00002"};
  T_EQ_INT(control_suspend(), CTL_OK);
  T_EQ_INT(fake.suspend_calls, 1);
  T_EQ_INT(fake.suspended_app_id, 7);
  fake.suspend_rc = -5;
  T_EQ_INT(control_suspend(), CTL_SYSTEM_ERROR);
}

static void test_suspend_nothing_running(void) {
  setup();
  T_EQ_INT(control_suspend(), CTL_NO_APP);
  T_EQ_INT(fake.suspend_calls, 0);
}

static void test_suspend_unavailable(void) {
  setup();
  fake.running = (plat_running){7, "PPSA00002"};
  fake.caps.suspend = false;
  T_EQ_INT(control_suspend(), CTL_UNAVAILABLE);
  T_EQ_INT(fake.suspend_calls, 0);
}

static void test_standby_allowed_without_config(void) {
  int rc;
  setup(); /* fresh config: no permission switch anywhere */
  T_EQ_INT(control_standby(&cfg, &rc), CTL_OK);
  T_EQ_INT(fake.standby_calls, 1);
}

static void test_standby_unavailable(void) {
  int rc;
  setup();
  fake.caps.standby = false;
  T_EQ_INT(control_standby(&cfg, &rc), CTL_UNAVAILABLE);
  T_EQ_INT(fake.standby_calls, 0);
}

static void test_standby_rate_limited(void) {
  int rc;
  setup();
  T_EQ_INT(control_standby(&cfg, &rc), CTL_OK);
  T_EQ_INT(fake.standby_calls, 1);
  fake.mono_ms += 9000;
  T_EQ_INT(control_standby(&cfg, &rc), CTL_RATE_LIMITED);
  T_EQ_INT(rc, 1); /* retry after, seconds */
  fake.mono_ms += 1000;
  T_EQ_INT(control_standby(&cfg, &rc), CTL_OK);
  T_EQ_INT(fake.standby_calls, 2);
}

static void test_standby_failure_is_system_error(void) {
  int rc;
  setup();
  fake.standby_rc = -3;
  T_EQ_INT(control_standby(&cfg, &rc), CTL_SYSTEM_ERROR);
  T_EQ_INT(rc, -3);
}


static void test_notify_sends_sanitized(void) {
  int retry = 0;
  setup();
  T_EQ_INT(control_notify("Hallo\x01\tWelt\n\xc3\x84\xff", &retry), CTL_OK);
  T_EQ_STR(fake.last_notify, "HalloWelt\n\xc3\x84\xef\xbf\xbd");
}

static void test_notify_truncates_200(void) {
  char msg[400];
  int retry;
  setup();
  for (int i = 0; i < 150; i++) {
    memcpy(msg + 2 * i, "\xc3\xa4", 2);
  }
  msg[300] = '\0';
  T_EQ_INT(control_notify(msg, &retry), CTL_OK);
  T_EQ_INT(strlen(fake.last_notify), 200);
}

static void test_notify_empty_rejected(void) {
  int retry;
  setup();
  T_EQ_INT(control_notify("\x01\x02", &retry), CTL_BAD_INPUT);
  T_EQ_INT(control_notify("", &retry), CTL_BAD_INPUT);
  T_EQ_INT(fake.notify_count, 0);
}

static void test_notify_rate_limited(void) {
  int retry = 0;
  setup();
  for (int i = 0; i < 5; i++) {
    fake.mono_ms += 1000;
    T_EQ_INT(control_notify("x", &retry), CTL_OK);
  }
  T_EQ_INT(control_notify("x", &retry), CTL_RATE_LIMITED);
  T_EQ_INT(retry, 26); /* oldest of the five was 4 s ago */
  fake.mono_ms += 26000;
  T_EQ_INT(control_notify("x", &retry), CTL_OK);
}

static void test_notify_failure_500(void) {
  int retry, rc = 0;
  setup();
  fake.notify_rc = -7;
  T_EQ_INT(control_notify("x", &retry), CTL_SYSTEM_ERROR);
  (void)rc;
}

static void test_power_off_allowed_without_config(void) {
  int rc;
  setup();
  T_EQ_INT(control_power_off(&cfg, &rc), CTL_OK);
  T_EQ_INT(fake.power_off_calls, 1);
}

static void test_power_off_unavailable(void) {
  int rc;
  setup();
  fake.caps.power_off = false;
  T_EQ_INT(control_power_off(&cfg, &rc), CTL_UNAVAILABLE);
  T_EQ_INT(fake.power_off_calls, 0);
}

static void test_power_off_shares_rate_limit(void) {
  int rc;
  setup();
  T_EQ_INT(control_standby(&cfg, &rc), CTL_OK);
  fake.mono_ms += 3000;
  T_EQ_INT(control_power_off(&cfg, &rc), CTL_RATE_LIMITED);
  T_EQ_INT(rc, 7);
  fake.mono_ms += 7000;
  T_EQ_INT(control_power_off(&cfg, &rc), CTL_OK);
  T_EQ_INT(fake.power_off_calls, 1);
  fake.mono_ms += 10000;
  fake.power_off_rc = -4;
  T_EQ_INT(control_power_off(&cfg, &rc), CTL_SYSTEM_ERROR);
  T_EQ_INT(rc, -4);
}

int main(void) {
  T_RUN(test_launch_ok);
  T_RUN(test_launch_already_running);
  T_RUN(test_launch_other_running_409);
  T_RUN(test_launch_replace_kills_first);
  T_RUN(test_launch_replace_kill_fails);
  T_RUN(test_launch_not_installed);
  T_RUN(test_launch_no_user);
  T_RUN(test_launch_rc_negative_is_error);
  T_RUN(test_close_no_app);
  T_RUN(test_close_title_mismatch);
  T_RUN(test_close_ok);
  T_RUN(test_suspend_running_game);
  T_RUN(test_suspend_nothing_running);
  T_RUN(test_suspend_unavailable);
  T_RUN(test_standby_allowed_without_config);
  T_RUN(test_standby_unavailable);
  T_RUN(test_standby_rate_limited);
  T_RUN(test_standby_failure_is_system_error);
  T_RUN(test_notify_sends_sanitized);
  T_RUN(test_notify_truncates_200);
  T_RUN(test_notify_empty_rejected);
  T_RUN(test_notify_rate_limited);
  T_RUN(test_notify_failure_500);
  T_RUN(test_power_off_allowed_without_config);
  T_RUN(test_power_off_unavailable);
  T_RUN(test_power_off_shares_rate_limit);
  apps_free(&apps);
  T_MAIN();
}
