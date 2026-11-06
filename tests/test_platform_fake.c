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

int main(void) {
  T_RUN(test_fake_clock);
  T_RUN(test_fake_random_deterministic);
  T_RUN(test_fake_launch_requires_folder);
  T_RUN(test_fake_notify_records);
  T_MAIN();
}
