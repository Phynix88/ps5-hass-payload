/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include "apps.h"
#include "auth.h"
#include "config.h"
#include "json.h"
#include "log.h"
#include "platform_fake.h"
#include "state.h"

static app_list apps;
static config cfg;
static jsmntok_t tok[1024];

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
  config_load(&cfg);
  auth_init(&cfg);
}

/* state_json/info_json write fields into an open object. */
static const char *render_state(jr_t *r) {
  static char out[4096];
  jw_t w;
  jw_init(&w);
  jw_obj(&w);
  state_json(&w, &apps, 0);
  jw_end_obj(&w);
  snprintf(out, sizeof(out), "%s", w.buf);
  jw_free(&w);
  if (jr_parse(r, out, strlen(out), tok, 1024)) {
    return NULL;
  }
  return out;
}

static const char *render_info(jr_t *r) {
  static char out[4096];
  jw_t w;
  jw_init(&w);
  jw_obj(&w);
  info_json(&w, &cfg, "00112233445566778899aabbccddeeff", true);
  jw_end_obj(&w);
  snprintf(out, sizeof(out), "%s", w.buf);
  jw_free(&w);
  if (jr_parse(r, out, strlen(out), tok, 1024)) {
    return NULL;
  }
  return out;
}

static void test_state_idle(void) {
  jr_t r;
  const char *s;
  setup();
  fake.mono_ms = 121000;
  fake.uptime_s = 284;
  T_ASSERT((s = render_state(&r)));
  T_ASSERT(strstr(s, "\"in_app\":false,\"app\":null,"));
  T_ASSERT(strstr(s, "\"temps\":{\"cpu\":41,\"soc\":38},\"fan_raw\":131,"));
  T_ASSERT(strstr(s, "\"storage\":{\"data_free_mb\":1000,\"data_total_mb\":2000,"
                     "\"user_free_mb\":1000,\"user_total_mb\":2000},"));
  T_ASSERT(strstr(s, "\"uptime_s\":284,\"payload_uptime_s\":121,"));
  T_ASSERT(strstr(s, apps.etag));
}

static void test_state_running_named(void) {
  jr_t r;
  const char *s;
  setup();
  fake.language = 4;
  apps_free(&apps);
  apps_scan(&apps);
  fake.running = (plat_running){24, "PPSA00001"};
  T_ASSERT((s = render_state(&r)));
  T_ASSERT(strstr(s, "\"in_app\":true,\"app\":{\"title_id\":\"PPSA00001\","
                     "\"name\":\"Testspiel Eins\",\"app_id\":24},"));
  /* unknown title: name = title_id */
  fake.running = (plat_running){25, "ABCD12345"};
  T_ASSERT((s = render_state(&r)));
  T_ASSERT(strstr(s, "\"name\":\"ABCD12345\""));
}

static void test_state_all_sensors_fail(void) {
  jr_t r;
  const char *s;
  setup();
  fake.cpu.ok = fake.soc.ok = fake.fan.ok = false;
  fake.storage_rc = -1;
  T_ASSERT((s = render_state(&r)));
  T_ASSERT(strstr(s, "\"temps\":{\"cpu\":null,\"soc\":null},\"fan_raw\":null,"));
  T_ASSERT(strstr(s, "\"storage\":{\"data_free_mb\":null,\"data_total_mb\":null,"
                     "\"user_free_mb\":null,\"user_total_mb\":null},"));
}

static void test_info_fields_and_capabilities(void) {
  jr_t r;
  const char *s;
  setup();
  fake.caps.random = false;
  fake.caps.standby = false;
  T_ASSERT((s = render_info(&r)));
  T_ASSERT(strstr(s, "{\"name\":\"ps5-hass\",\"version\":\"host\","
                     "\"commit\":\"host\",\"api\":1,"
                     "\"device_id\":\"00112233445566778899aabbccddeeff\","
                     "\"model\":\"CFI-TEST\",\"firmware\":\"13.60\","
                     "\"paired\":false,\"power_actions\":false,\"config\":\"ok\","));
  T_ASSERT(strstr(s, "\"capabilities\":{\"temps\":true,\"fan\":true,"
                     "\"notify\":true,\"random\":false,\"launch\":true,"
                     "\"standby\":false,\"mdns\":true,\"power_off\":false,"
                     "\"fan_control\":false}"));
  T_ASSERT(strstr(s, "\"last_error\":"));
}

static void test_info_recovered_and_last_error(void) {
  jr_t r;
  const char *s;
  setup();
  cfg.recovered = true;
  cfg.power_actions = true;
  log_error("something broke");
  T_ASSERT((s = render_info(&r)));
  T_ASSERT(strstr(s, "\"power_actions\":true,\"config\":\"recovered\""));
  T_ASSERT(strstr(s, "something broke\"}"));
}

static void test_state_payloads(void) {
  jr_t r;
  const char *s;
  setup();
  snprintf(fake.procs[0], 20, "kstuff.elf");
  snprintf(fake.procs[1], 20, "SceSysCore.elf");
  snprintf(fake.procs[2], 20, "ftpsrv.elf");
  fake.n_procs = 3;
  T_ASSERT((s = render_state(&r)));
  T_ASSERT(strstr(s, "\"payloads\":[\"ftpsrv.elf\",\"kstuff.elf\"]"));
}

static void test_info_power_off_needs_permission(void) {
  jr_t r;
  const char *s;
  setup();
  T_ASSERT((s = render_info(&r)));
  T_ASSERT(strstr(s, "\"power_off\":false"));
  cfg.power_off = true;
  T_ASSERT((s = render_info(&r)));
  T_ASSERT(strstr(s, "\"power_off\":true"));
  fake.caps.power_off = false;
  T_ASSERT((s = render_info(&r)));
  T_ASSERT(strstr(s, "\"power_off\":false"));
}

int main(void) {
  T_RUN(test_state_idle);
  T_RUN(test_state_running_named);
  T_RUN(test_state_all_sensors_fail);
  T_RUN(test_info_fields_and_capabilities);
  T_RUN(test_state_payloads);
  T_RUN(test_info_power_off_needs_permission);
  T_RUN(test_info_recovered_and_last_error);
  apps_free(&apps);
  T_MAIN();
}
