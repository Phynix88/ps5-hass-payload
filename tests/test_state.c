/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include "apps.h"
#include "auth.h"
#include "config.h"
#include "json.h"
#include "log.h"
#include "platform_fake.h"
#include "power.h"
#include "state.h"

static app_list apps;
static config cfg;
static jsmntok_t tok[1024];

static void setup(void) {
  char cmd[700];
  fake_reset();
  info_cache_reset();
  power_init();
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
                     "\"standby\":false,\"mdns\":true,\"power_off\":true,"
                     "\"fan_control\":true,\"focus\":true,"
                     "\"power_state\":true,\"suspend\":true,"
                     "\"keep_awake\":true,\"soc_power\":true,"
                     "\"playtime\":true,\"payload_control\":false}"));
  T_ASSERT(strstr(s, "\"last_error\":"));
}

static void test_info_recovered_and_last_error(void) {
  jr_t r;
  const char *s;
  setup();
  cfg.recovered = true;
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

static void test_info_power_and_fan_follow_capabilities(void) {
  jr_t r;
  const char *s;
  setup();
  T_ASSERT((s = render_info(&r))); /* fresh config: nothing to allow */
  T_ASSERT(strstr(s, "\"power_actions\":true,"));
  T_ASSERT(strstr(s, "\"power_off\":true,\"fan_control\":true,"));
  fake.caps.power_off = false;
  fake.caps.fan_control = false;
  fake.caps.standby = false;
  T_ASSERT((s = render_info(&r)));
  T_ASSERT(strstr(s, "\"power_actions\":false,"));
  T_ASSERT(strstr(s, "\"power_off\":false,\"fan_control\":false,"));
}

static void test_info_has_permissions(void) {
  jr_t r;
  const char *s;
  setup();
  fake.caps.power_off = false;
  T_ASSERT((s = render_info(&r)));
  T_ASSERT(strstr(s, "\"permissions\":{\"power_actions\":{\"allowed\":true,"
                     "\"available\":true},\"power_off\":{\"allowed\":false,"
                     "\"available\":false},\"fan_control\":{\"allowed\":true,"
                     "\"available\":true}}"));
}

static void set_rails(void) {
  /* four active rails; the others stay zero */
  fake.soc_power.ok = true;
  fake.soc_power.mw[0] = 30000, fake.soc_power.mv[0] = 1000, fake.soc_power.ma[0] = 30000;
  fake.soc_power.mw[1] = 12000, fake.soc_power.mv[1] = 1200, fake.soc_power.ma[1] = 10000;
  fake.soc_power.mw[3] = 4500, fake.soc_power.mv[3] = 900, fake.soc_power.ma[3] = 5000;
  fake.soc_power.mw[6] = 1500, fake.soc_power.mv[6] = 1500, fake.soc_power.ma[6] = 1000;
}

static void add_drive(const char *path, int64_t free_mb, int64_t total_mb) {
  int i = fake.n_storage_over++;
  snprintf(fake.storage_over[i].path, sizeof(fake.storage_over[i].path), "%s", path);
  fake.storage_over[i].free_mb = free_mb;
  fake.storage_over[i].total_mb = total_mb;
}

static void test_soc_power_sum_and_rails(void) {
  jr_t r;
  const char *s;
  setup();
  set_rails();
  T_ASSERT((s = render_state(&r)));
  T_ASSERT(strstr(s, "\"soc_power\":{\"total_mw\":48000,\"rails\":["
                     "{\"mw\":30000,\"mv\":1000,\"ma\":30000},"
                     "{\"mw\":12000,\"mv\":1200,\"ma\":10000},"
                     "{\"mw\":4500,\"mv\":900,\"ma\":5000},"
                     "{\"mw\":1500,\"mv\":1500,\"ma\":1000}]}"));
  /* a failing or empty reading is null */
  info_cache_reset();
  fake.soc_power_rc = -1;
  T_ASSERT((s = render_state(&r)));
  T_ASSERT(strstr(s, "\"soc_power\":null"));
  info_cache_reset();
  fake.soc_power_rc = 0;
  fake.soc_power.ok = false;
  T_ASSERT((s = render_state(&r)));
  T_ASSERT(strstr(s, "\"soc_power\":null"));
}

static void test_soc_power_cached_10s(void) {
  jr_t r;
  const char *s;
  setup();
  set_rails();
  T_ASSERT((s = render_state(&r)));
  T_EQ_INT(fake.soc_power_calls, 1);
  fake.soc_power.mw[0] = 31000;
  fake.mono_ms += 9999;
  T_ASSERT((s = render_state(&r)));
  T_EQ_INT(fake.soc_power_calls, 1);
  T_ASSERT(strstr(s, "\"total_mw\":48000"));
  fake.mono_ms += 1;
  T_ASSERT((s = render_state(&r)));
  T_EQ_INT(fake.soc_power_calls, 2);
  T_ASSERT(strstr(s, "\"total_mw\":49000"));
  /* a console without the function: null, and no call */
  fake.caps.soc_power = false;
  fake.mono_ms += 10000;
  T_ASSERT((s = render_state(&r)));
  T_ASSERT(strstr(s, "\"soc_power\":null"));
  T_EQ_INT(fake.soc_power_calls, 2);
}

static void test_drives_ignore_placeholders(void) {
  jr_t r;
  const char *s;
  setup();
  /* mount points without a drive report about 2 MB; /mnt/ext1 is missing */
  add_drive("/mnt/ext0", 1, 2);
  add_drive("/mnt/usb0", 1, 2);
  add_drive("/mnt/usb7", 0, 1023);
  T_ASSERT((s = render_state(&r)));
  T_ASSERT(strstr(s, "\"drives\":[]"));
}

static void test_drives_report_real_drive(void) {
  jr_t r;
  const char *s;
  setup();
  add_drive("/mnt/ext1", 500000, 1000000);
  add_drive("/mnt/ext0", 1, 2);
  add_drive("/mnt/usb3", 2048, 3000);
  add_drive("/mnt/usb0", 10, 1024);
  T_ASSERT((s = render_state(&r)));
  T_ASSERT(strstr(s, "\"drives\":[{\"name\":\"m2\",\"free_mb\":500000,"
                     "\"total_mb\":1000000},{\"name\":\"usb0\",\"free_mb\":10,"
                     "\"total_mb\":1024},{\"name\":\"usb3\",\"free_mb\":2048,"
                     "\"total_mb\":3000}]"));
  info_cache_reset();
  fake.storage_over[1].free_mb = 70000;
  fake.storage_over[1].total_mb = 80000;
  T_ASSERT((s = render_state(&r)));
  T_ASSERT(strstr(s, "{\"name\":\"extended\",\"free_mb\":70000,\"total_mb\":80000}"));
}

static void test_drives_cached_60s(void) {
  jr_t r;
  const char *s;
  int first, second;
  setup();
  add_drive("/mnt/ext1", 500000, 1000000);
  fake.reads = 0;
  T_ASSERT((s = render_state(&r)));
  first = fake.reads;
  fake.reads = 0;
  fake.storage_over[0].free_mb = 400000;
  fake.mono_ms += 59999;
  T_ASSERT((s = render_state(&r)));
  T_ASSERT(strstr(s, "\"free_mb\":500000"));
  second = fake.reads;
  /* the ten mount points are not asked again */
  T_ASSERT(first - second >= 10);
  fake.mono_ms += 1;
  T_ASSERT((s = render_state(&r)));
  T_ASSERT(strstr(s, "\"free_mb\":400000"));
}

static void test_soc_power_and_drives_untouched_outside_on(void) {
  jr_t r;
  const char *s;
  setup();
  set_rails();
  add_drive("/mnt/ext1", 500000, 1000000);
  T_ASSERT((s = render_state(&r)));
  T_ASSERT(strstr(s, "\"total_mw\":48000"));
  fake.power_state = 500;
  power_tick(fake.mono_ms);
  fake.reads = fake.sony_calls = fake.soc_power_calls = 0;
  fake.mono_ms += 600000;
  T_ASSERT((s = render_state(&r)));
  T_ASSERT(strstr(s, "\"soc_power\":null"));
  T_ASSERT(strstr(s, "\"drives\":[]"));
  T_EQ_INT(fake.reads, 0);
  T_EQ_INT(fake.sony_calls, 0);
  T_EQ_INT(fake.soc_power_calls, 0);
  /* back on: read fresh, not served from before the rest */
  fake.power_state = 1000;
  power_tick(fake.mono_ms);
  fake.soc_power.mw[0] = 40000;
  fake.storage_over[0].free_mb = 1234;
  fake.mono_ms += 1000;
  T_ASSERT((s = render_state(&r)));
  T_ASSERT(strstr(s, "\"total_mw\":58000"));
  T_ASSERT(strstr(s, "\"free_mb\":1234"));
}

int main(void) {
  T_RUN(test_state_idle);
  T_RUN(test_state_running_named);
  T_RUN(test_state_all_sensors_fail);
  T_RUN(test_info_fields_and_capabilities);
  T_RUN(test_state_payloads);
  T_RUN(test_info_power_and_fan_follow_capabilities);
  T_RUN(test_info_recovered_and_last_error);
  apps_free(&apps);
  T_RUN(test_info_has_permissions);
  T_RUN(test_soc_power_sum_and_rails);
  T_RUN(test_soc_power_cached_10s);
  T_RUN(test_drives_ignore_placeholders);
  T_RUN(test_drives_report_real_drive);
  T_RUN(test_drives_cached_60s);
  T_RUN(test_soc_power_and_drives_untouched_outside_on);
  T_MAIN();
}
