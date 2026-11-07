/* Permission list kept for v0.3 clients: allowed always equals available.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include "json.h"
#include "perm.h"
#include "platform_fake.h"

static void render(jw_t *w) {
  jw_init(w);
  jw_obj(w);
  perm_json(w);
  jw_end_obj(w);
  T_ASSERT(!w->failed);
}

static void test_json_all_available(void) {
  jw_t w;
  fake_reset();
  render(&w);
  T_EQ_STR(w.buf,
           "{\"permissions\":{\"power_actions\":{\"allowed\":true,"
           "\"available\":true},\"power_off\":{\"allowed\":true,"
           "\"available\":true},\"fan_control\":{\"allowed\":true,"
           "\"available\":true}}}");
  jw_free(&w);
}

static void test_json_allowed_follows_capabilities(void) {
  jw_t w;
  fake_reset();
  fake.caps.power_off = false;
  render(&w);
  T_ASSERT(strstr(w.buf, "\"power_actions\":{\"allowed\":true,\"available\":true}"));
  T_ASSERT(strstr(w.buf, "\"power_off\":{\"allowed\":false,\"available\":false}"));
  T_ASSERT(strstr(w.buf, "\"fan_control\":{\"allowed\":true,\"available\":true}"));
  jw_free(&w);
  fake.caps.standby = false;
  fake.caps.fan_control = false;
  render(&w);
  T_ASSERT(strstr(w.buf, "\"power_actions\":{\"allowed\":false,\"available\":false}"));
  T_ASSERT(strstr(w.buf, "\"fan_control\":{\"allowed\":false,\"available\":false}"));
  jw_free(&w);
}

int main(void) {
  T_RUN(test_json_all_available);
  T_RUN(test_json_allowed_follows_capabilities);
  T_MAIN();
}
