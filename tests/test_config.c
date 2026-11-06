/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include "config.h"
#include "limits.h"
#include "log.h"
#include "platform_fake.h"

static char dir[700], cfg_path[760];

static void setup(void) {
  fake_reset();
  snprintf(fake.root, sizeof(fake.root), "%s", t_tmpdir());
  snprintf(dir, sizeof(dir), "%s%s", fake.root, PS5H_DATA_DIR);
  snprintf(cfg_path, sizeof(cfg_path), "%s/config.json", dir);
}

static const char *TOKEN64 =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static void test_missing_config_defaults(void) {
  config c;
  setup();
  T_EQ_INT(config_load(&c), 0);
  T_EQ_INT(c.port, PS5H_DEFAULT_PORT);
  T_EQ_INT(c.n_tokens, 0);
  T_EQ_INT(c.n_allow, 0);
  T_ASSERT(!c.allow_broken && !c.power_actions && !c.recovered);
}

static void test_broken_config_renamed_and_notified(void) {
  config c;
  char p[800];
  struct stat st;
  setup();
  t_write_str(cfg_path, "{broken");
  T_EQ_INT(config_load(&c), 0);
  T_ASSERT(c.recovered);
  T_EQ_INT(c.n_tokens, 0);
  snprintf(p, sizeof(p), "%s/config.json.broken-1790000000", dir);
  T_EQ_INT(stat(p, &st), 0);
  T_EQ_INT(stat(cfg_path, &st), -1);
  T_ASSERT(strstr(fake.last_notify, "config"));
  T_ASSERT(log_last_error() != NULL);
}

static void test_wrong_schema_is_broken(void) {
  config c;
  setup();
  t_write_str(cfg_path, "{\"schema\":2}");
  T_EQ_INT(config_load(&c), 0);
  T_ASSERT(c.recovered);
}

static void test_valid_fields_loaded(void) {
  config c;
  char buf[1024];
  setup();
  snprintf(buf, sizeof(buf),
           "{\"schema\":1,\"port\":8182,\"allow\":[\"192.0.2.0/24\","
           "\"10.0.0.5\"],\"hosts\":[\"ps5.lan\"],\"power_actions\":true,"
           "\"tokens\":[{\"token\":\"%s\",\"client_id\":"
           "\"0b7e2f1c-1111-4222-8333-944445555666\",\"label\":\"Home "
           "Assistant\",\"created\":\"2026-11-06T13:00:00Z\",\"last_used\":"
           "\"2026-11-06T14:00:00Z\"}]}",
           TOKEN64);
  t_write_str(cfg_path, buf);
  T_EQ_INT(config_load(&c), 0);
  T_ASSERT(!c.recovered && !c.allow_broken && c.power_actions);
  T_EQ_INT(c.port, 8182);
  T_EQ_INT(c.n_allow, 2);
  T_EQ_INT(c.allow[0].net, 0xc0000200); /* host byte order */
  T_EQ_INT(c.allow[0].mask, 0xffffff00);
  T_EQ_INT(c.allow[1].mask, 0xffffffff);
  T_EQ_INT(c.n_hosts, 1);
  T_EQ_STR(c.hosts[0], "ps5.lan");
  T_EQ_INT(c.n_tokens, 1);
  T_EQ_STR(c.tokens[0].token, TOKEN64);
  T_EQ_STR(c.tokens[0].label, "Home Assistant");
  T_EQ_INT(c.tokens[0].created, 1793970000);
  T_EQ_INT(c.tokens[0].last_used, 1793973600);
}

static void test_invalid_allow_blocks_all(void) {
  config c;
  setup();
  t_write_str(cfg_path, "{\"schema\":1,\"allow\":[\"10.0.0.0/8\",\"nonsense\"]}");
  T_EQ_INT(config_load(&c), 0);
  T_ASSERT(c.allow_broken);
  T_ASSERT(log_last_error() != NULL);
  setup();
  t_write_str(cfg_path, "{\"schema\":1,\"allow\":[\"10.0.0.0/33\"]}");
  T_EQ_INT(config_load(&c), 0);
  T_ASSERT(c.allow_broken);
  setup();
  t_write_str(cfg_path, "{\"schema\":1,\"allow\":\"10.0.0.1\"}");
  T_EQ_INT(config_load(&c), 0);
  T_ASSERT(c.allow_broken);
}

static void test_short_token_dropped(void) {
  config c;
  char buf[1024];
  setup();
  snprintf(buf, sizeof(buf),
           "{\"schema\":1,\"tokens\":[{\"token\":\"abc\",\"client_id\":\"x\"},"
           "{\"token\":\"%.63sG\",\"client_id\":\"y\"},{\"token\":\"%s\","
           "\"client_id\":\"0b7e2f1c-1111-4222-8333-944445555666\","
           "\"label\":\"ok\"}]}",
           TOKEN64, TOKEN64);
  t_write_str(cfg_path, buf);
  T_EQ_INT(config_load(&c), 0);
  T_EQ_INT(c.n_tokens, 1);
  T_EQ_STR(c.tokens[0].label, "ok");
}

static void test_save_load_roundtrip(void) {
  config c, d;
  setup();
  T_EQ_INT(config_load(&c), 0);
  c.port = 8182;
  c.power_actions = true;
  c.n_tokens = 1;
  snprintf(c.tokens[0].token, sizeof(c.tokens[0].token), "%s", TOKEN64);
  snprintf(c.tokens[0].client_id, sizeof(c.tokens[0].client_id),
           "0b7e2f1c-1111-4222-8333-944445555666");
  snprintf(c.tokens[0].label, sizeof(c.tokens[0].label), "K\xc3\xbc" "che");
  c.tokens[0].created = 1793970000;
  c.tokens[0].last_used = 1793973600;
  T_EQ_INT(config_save(&c), 0);
  T_ASSERT(strstr(t_read(cfg_path), "\"created\":\"2026-11-06T13:00:00Z\""));
  T_EQ_INT(config_load(&d), 0);
  T_EQ_INT(d.port, 8182);
  T_ASSERT(d.power_actions);
  T_EQ_INT(d.n_tokens, 1);
  T_EQ_STR(d.tokens[0].label, "K\xc3\xbc" "che");
  T_EQ_INT(d.tokens[0].last_used, 1793973600);
}

static void test_save_is_atomic(void) {
  config c;
  const char *before;
  char copy[256];
  setup();
  T_EQ_INT(config_load(&c), 0);
  T_EQ_INT(config_save(&c), 0);
  before = t_read(cfg_path);
  snprintf(copy, sizeof(copy), "%s", before);
  T_EQ_INT(chmod(dir, 0500), 0);
  c.port = 9999;
  T_ASSERT(config_save(&c) < 0);
  T_EQ_INT(chmod(dir, 0700), 0);
  T_EQ_STR(t_read(cfg_path), copy);
}

static void test_file_modes(void) {
  config c;
  struct stat st;
  setup();
  T_EQ_INT(config_load(&c), 0);
  T_EQ_INT(config_save(&c), 0);
  T_EQ_INT(stat(dir, &st), 0);
  T_EQ_INT(st.st_mode & 0777, 0700);
  T_EQ_INT(stat(cfg_path, &st), 0);
  T_EQ_INT(st.st_mode & 0777, 0600);
}

static void test_device_id_stable_and_hex32(void) {
  char a[33], b[33];
  setup();
  T_EQ_INT(device_id_get(a), 0);
  T_EQ_INT(strlen(a), 32);
  for (int i = 0; i < 32; i++) {
    T_ASSERT((a[i] >= '0' && a[i] <= '9') || (a[i] >= 'a' && a[i] <= 'f'));
  }
  fake.random_counter += 1000; /* new random values would differ */
  T_EQ_INT(device_id_get(b), 0);
  T_EQ_STR(a, b);
}

static void test_device_id_survives_config_reset(void) {
  char a[33], b[33];
  config c;
  setup();
  T_EQ_INT(device_id_get(a), 0);
  T_EQ_INT(config_load(&c), 0);
  T_EQ_INT(config_save(&c), 0);
  T_EQ_INT(unlink(cfg_path), 0);
  T_EQ_INT(config_load(&c), 0);
  T_EQ_INT(device_id_get(b), 0);
  T_EQ_STR(a, b);
}

static void test_device_id_without_random_fails(void) {
  char a[33];
  setup();
  fake.random_rc = -1;
  T_ASSERT(device_id_get(a) < 0);
}

static void test_log_rotates_at_limit(void) {
  char p0[800], p1[800], line[200];
  struct stat s0, s1;
  setup();
  memset(line, 'x', sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';
  for (int i = 0; i < 1000; i++) {
    log_msg("%d %s", i, line);
  }
  snprintf(p0, sizeof(p0), "%s/log.txt", dir);
  snprintf(p1, sizeof(p1), "%s/log.txt.1", dir);
  T_EQ_INT(stat(p0, &s0), 0);
  T_EQ_INT(stat(p1, &s1), 0);
  T_ASSERT(s0.st_size + s1.st_size <= PS5H_LOG_MAX);
  T_ASSERT(strstr(t_read(p0), "999 xxx"));
  T_ASSERT(strstr(t_read(p0), "[2026-09-21T"));
}

static void test_log_error_kept(void) {
  setup();
  log_error("bind failed rc=%d", -5);
  T_ASSERT(strstr(log_last_error(), "bind failed rc=-5"));
}

static void test_config_new_fields_default_false(void) {
  config c, d;
  setup();
  T_EQ_INT(config_load(&c), 0);
  T_ASSERT(!c.power_off && !c.fan_control);
  T_EQ_INT(c.fan_target_c, 0);
  T_EQ_INT(c.fan_stock_c, 0);
  c.power_off = true;
  c.fan_control = true;
  c.fan_target_c = 70;
  c.fan_stock_c = 91;
  T_EQ_INT(config_save(&c), 0);
  T_EQ_INT(config_load(&d), 0);
  T_ASSERT(d.power_off && d.fan_control);
  T_EQ_INT(d.fan_target_c, 70);
  T_EQ_INT(d.fan_stock_c, 91);
  t_write_str(cfg_path, "{\"schema\":1,\"fan_target_c\":200,\"fan_stock_c\":-3}");
  T_EQ_INT(config_load(&d), 0);
  T_EQ_INT(d.fan_target_c, 0);
  T_EQ_INT(d.fan_stock_c, 0);
}

int main(void) {
  T_RUN(test_missing_config_defaults);
  T_RUN(test_broken_config_renamed_and_notified);
  T_RUN(test_wrong_schema_is_broken);
  T_RUN(test_valid_fields_loaded);
  T_RUN(test_invalid_allow_blocks_all);
  T_RUN(test_short_token_dropped);
  T_RUN(test_save_load_roundtrip);
  T_RUN(test_save_is_atomic);
  T_RUN(test_file_modes);
  T_RUN(test_device_id_stable_and_hex32);
  T_RUN(test_device_id_survives_config_reset);
  T_RUN(test_device_id_without_random_fails);
  T_RUN(test_log_rotates_at_limit);
  T_RUN(test_log_error_kept);
  T_RUN(test_config_new_fields_default_false);
  T_MAIN();
}
