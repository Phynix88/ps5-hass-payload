/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include "auth.h"
#include "config.h"
#include "json.h"
#include "limits.h"
#include "platform_fake.h"

static config cfg;
static char cfg_path[760];

static const char *CLIENT = "0b7e2f1c-1111-4222-8333-944445555666";
static const char *CLIENT2 = "0b7e2f1c-1111-4222-8333-944445555667";

static void setup(void) {
  fake_reset();
  snprintf(fake.root, sizeof(fake.root), "%s", t_tmpdir());
  snprintf(cfg_path, sizeof(cfg_path), "%s%s/config.json", fake.root,
           PS5H_DATA_DIR);
  config_load(&cfg);
  auth_init(&cfg);
}

/* The code is the last 6 characters of the notification. */
static const char *shown_code(void) {
  size_t n = strlen(fake.last_notify);
  return n >= 6 ? fake.last_notify + n - 6 : "";
}

static const char *wrong_code(void) {
  static char w[7];
  snprintf(w, sizeof(w), "%s", shown_code());
  w[0] = w[0] == '9' ? '0' : (char)(w[0] + 1);
  return w;
}

static auth_rc finish(const char *code, const char *client, char tok[65],
                      int *left, int *retry) {
  return auth_pair_finish(code, client, "Home Assistant", tok, left, retry);
}

/* Start a new code; moves the clock past the restart limit. */
static void start(void) {
  int exp, retry;
  fake.mono_ms += (PS5H_CODE_RESTART_S + 1) * 1000;
  auth_pair_start(&exp, &retry);
}

static void test_pair_start_shows_code_on_tv(void) {
  int exp = 0, retry = 0;
  setup();
  T_EQ_INT(auth_pair_start(&exp, &retry), AUTH_OK);
  T_EQ_INT(exp, 120);
  T_ASSERT(strstr(fake.last_notify, "ps5-hass pairing code: ") ==
           fake.last_notify);
  T_EQ_INT(strlen(shown_code()), 6);
  for (int i = 0; i < 6; i++) {
    T_ASSERT(shown_code()[i] >= '0' && shown_code()[i] <= '9');
  }
}

static void test_pair_start_notify_failed_holds_no_code(void) {
  int exp, retry, left;
  char tok[65];
  setup();
  fake.notify_rc = -1;
  T_EQ_INT(auth_pair_start(&exp, &retry), AUTH_NOTIFY_FAILED);
  T_EQ_INT(finish("123456", CLIENT, tok, &left, &retry), AUTH_NO_CODE);
  /* may retry at once */
  fake.notify_rc = 0;
  T_EQ_INT(auth_pair_start(&exp, &retry), AUTH_OK);
}

static void test_pair_start_random_unavailable(void) {
  int exp, retry;
  setup();
  fake.random_rc = -1;
  T_EQ_INT(auth_pair_start(&exp, &retry), AUTH_RANDOM_UNAVAILABLE);
  T_EQ_INT(fake.notify_count, 0);
}

static void test_pair_restart_within_30s_rate_limited(void) {
  int exp, retry = 0;
  setup();
  T_EQ_INT(auth_pair_start(&exp, &retry), AUTH_OK);
  fake.mono_ms += 29000;
  T_EQ_INT(auth_pair_start(&exp, &retry), AUTH_RATE_LIMITED);
  T_EQ_INT(retry, 1);
  fake.mono_ms += 1000;
  T_EQ_INT(auth_pair_start(&exp, &retry), AUTH_OK);
}

static void test_code_expires_after_120s(void) {
  int exp, retry, left;
  char tok[65], code[7];
  setup();
  auth_pair_start(&exp, &retry);
  snprintf(code, sizeof(code), "%s", shown_code());
  fake.mono_ms += 120001;
  T_EQ_INT(finish(code, CLIENT, tok, &left, &retry), AUTH_NO_CODE);
}

static void test_success_returns_token_and_saves(void) {
  int exp, retry, left;
  char tok[65];
  setup();
  auth_pair_start(&exp, &retry);
  T_EQ_INT(finish(shown_code(), CLIENT, tok, &left, &retry), AUTH_OK);
  T_EQ_INT(strlen(tok), 64);
  T_ASSERT(auth_paired());
  T_EQ_INT(cfg.n_tokens, 1);
  T_ASSERT(strstr(t_read(cfg_path), tok));
  /* code is used up */
  T_EQ_INT(finish(shown_code(), CLIENT, tok, &left, &retry), AUTH_NO_CODE);
}

static void test_bad_input(void) {
  int exp, retry, left;
  char tok[65], label[100];
  setup();
  auth_pair_start(&exp, &retry);
  T_EQ_INT(finish("12345", CLIENT, tok, &left, &retry), AUTH_BAD_INPUT);
  T_EQ_INT(finish("12345a", CLIENT, tok, &left, &retry), AUTH_BAD_INPUT);
  T_EQ_INT(finish(shown_code(), "not-a-uuid", tok, &left, &retry),
           AUTH_BAD_INPUT);
  T_EQ_INT(finish(shown_code(), "0B7E2F1C-1111-4222-8333-944445555666", tok,
                  &left, &retry),
           AUTH_BAD_INPUT);
  T_EQ_INT(auth_pair_finish(shown_code(), CLIENT, "", tok, &left, &retry),
           AUTH_BAD_INPUT);
  memset(label, 'a', 65);
  label[65] = '\0';
  T_EQ_INT(auth_pair_finish(shown_code(), CLIENT, label, tok, &left, &retry),
           AUTH_BAD_INPUT);
  /* 64 characters of two bytes each are fine */
  {
    char wide[200] = "";
    for (int i = 0; i < 64; i++) {
      strcat(wide, "\xc3\xa4");
    }
    T_EQ_INT(auth_pair_finish(shown_code(), CLIENT, wide, tok, &left, &retry),
             AUTH_OK);
  }
}

static void test_wrong_code_counts_down(void) {
  int exp, retry, left = -1;
  char tok[65];
  setup();
  auth_pair_start(&exp, &retry);
  for (int want = 4; want >= 1; want--) {
    T_EQ_INT(finish(wrong_code(), CLIENT, tok, &left, &retry),
             AUTH_WRONG_CODE);
    T_EQ_INT(left, want);
  }
}

static void test_code_exhausted_after_5(void) {
  int exp, retry, left;
  char tok[65], code[7];
  setup();
  auth_pair_start(&exp, &retry);
  snprintf(code, sizeof(code), "%s", shown_code());
  for (int i = 0; i < 5; i++) {
    T_EQ_INT(finish(wrong_code(), CLIENT, tok, &left, &retry),
             AUTH_WRONG_CODE);
  }
  T_EQ_INT(left, 0);
  T_EQ_INT(finish(code, CLIENT, tok, &left, &retry), AUTH_CODE_EXHAUSTED);
}

static void fail_n(int n) {
  int left, retry;
  char tok[65];
  for (int i = 0; i < n; i++) {
    if (i % 5 == 0) {
      start();
    }
    finish(wrong_code(), CLIENT, tok, &left, &retry);
  }
}

static void test_global_lock_after_10_and_doubling(void) {
  int exp, retry = 0, left;
  char tok[65];
  setup();
  fail_n(9);
  start();
  T_EQ_INT(finish(wrong_code(), CLIENT, tok, &left, &retry), AUTH_LOCKED);
  T_EQ_INT(retry, 900);
  T_EQ_INT(auth_pair_start(&exp, &retry), AUTH_LOCKED);
  T_EQ_INT(finish(shown_code(), CLIENT, tok, &left, &retry), AUTH_LOCKED);
  fake.mono_ms += 900 * 1000;
  /* second lock doubles */
  fail_n(10);
  T_EQ_INT(auth_pair_start(&exp, &retry), AUTH_LOCKED);
  T_ASSERT(retry > 1700 && retry <= 1800);
  /* cap at 24 h */
  for (int i = 0; i < 10; i++) {
    fake.mono_ms += (int64_t)PS5H_LOCK_MAX_S * 1000;
    fail_n(10);
  }
  T_EQ_INT(auth_pair_start(&exp, &retry), AUTH_LOCKED);
  T_ASSERT(retry > 86000 && retry <= 86400);
}

static void test_success_resets_budget(void) {
  int retry, left;
  char tok[65];
  setup();
  fail_n(9);
  start();
  T_EQ_INT(finish(shown_code(), CLIENT, tok, &left, &retry), AUTH_OK);
  /* without the reset, the 10th failure in a row would come in fail_n */
  fail_n(8);
  start();
  T_EQ_INT(finish(wrong_code(), CLIENT, tok, &left, &retry), AUTH_WRONG_CODE);
}

static void test_pair_same_client_replaces(void) {
  int retry, left;
  char tok1[65], tok2[65], hdr[80];
  setup();
  start();
  T_EQ_INT(finish(shown_code(), CLIENT, tok1, &left, &retry), AUTH_OK);
  start();
  T_EQ_INT(finish(shown_code(), CLIENT, tok2, &left, &retry), AUTH_OK);
  T_EQ_INT(cfg.n_tokens, 1);
  T_ASSERT(strcmp(tok1, tok2));
  snprintf(hdr, sizeof(hdr), "Bearer %s", tok1);
  T_EQ_INT(auth_check(hdr), -1);
  snprintf(hdr, sizeof(hdr), "Bearer %s", tok2);
  T_EQ_INT(auth_check(hdr), 0);
}

static void test_token_limit_409(void) {
  int retry, left;
  char tok[65], client[40];
  setup();
  for (int i = 0; i < PS5H_MAX_TOKENS; i++) {
    snprintf(client, sizeof(client), "0b7e2f1c-1111-4222-8333-94444555%04d", i);
    start();
    T_EQ_INT(finish(shown_code(), client, tok, &left, &retry), AUTH_OK);
  }
  start();
  T_EQ_INT(finish(shown_code(), CLIENT2, tok, &left, &retry), AUTH_TOKEN_LIMIT);
  T_EQ_INT(cfg.n_tokens, PS5H_MAX_TOKENS);
  /* an existing client may still re-pair */
  start();
  T_EQ_INT(finish(shown_code(), "0b7e2f1c-1111-4222-8333-944445550003", tok,
                  &left, &retry),
           AUTH_OK);
}

static void test_check_rejects_malformed(void) {
  int retry, left;
  char tok[65], hdr[100];
  setup();
  start();
  T_EQ_INT(finish(shown_code(), CLIENT, tok, &left, &retry), AUTH_OK);
  T_EQ_INT(auth_check(""), -1);
  T_EQ_INT(auth_check("Bearer "), -1);
  T_EQ_INT(auth_check("Bearer abc"), -1);
  snprintf(hdr, sizeof(hdr), "bearer %s", tok);
  T_EQ_INT(auth_check(hdr), -1);
  snprintf(hdr, sizeof(hdr), "Bearer %sx", tok);
  T_EQ_INT(auth_check(hdr), -1);
  snprintf(hdr, sizeof(hdr), "Basic %s", tok);
  T_EQ_INT(auth_check(hdr), -1);
  snprintf(hdr, sizeof(hdr), "Bearer %s", tok);
  T_EQ_INT(auth_check(hdr), 0);
}

static void test_revoke_removes_slot(void) {
  int retry, left;
  char tok[65], hdr[100];
  setup();
  start();
  finish(shown_code(), CLIENT, tok, &left, &retry);
  snprintf(hdr, sizeof(hdr), "Bearer %s", tok);
  T_EQ_INT(auth_revoke(auth_check(hdr)), AUTH_OK);
  T_EQ_INT(auth_check(hdr), -1);
  T_ASSERT(!auth_paired());
  T_ASSERT(!strstr(t_read(cfg_path), tok));
}

static void test_last_used_write_throttled(void) {
  int retry, left;
  char tok[65], hdr[100];
  setup();
  start();
  finish(shown_code(), CLIENT, tok, &left, &retry);
  snprintf(hdr, sizeof(hdr), "Bearer %s", tok);
  T_ASSERT(strstr(t_read(cfg_path), "\"last_used\":\"2026-09-21T14:13:20Z\""));
  fake.wall_s += 599;
  T_EQ_INT(auth_check(hdr), 0);
  T_ASSERT(strstr(t_read(cfg_path), "\"last_used\":\"2026-09-21T14:13:20Z\""));
  T_EQ_INT(cfg.tokens[0].last_used, 1790000599);
  fake.wall_s += 1;
  T_EQ_INT(auth_check(hdr), 0);
  T_ASSERT(strstr(t_read(cfg_path), "\"last_used\":\"2026-09-21T14:23:20Z\""));
}

/* --- grants (v0.3) ------------------------------------------------------ */

/* Pair CLIENT (slot 0) with the given label. */
static void pair_as(const char *client, const char *label) {
  int retry, left;
  char tok[65];
  start();
  T_EQ_INT(auth_pair_finish(shown_code(), client, label, tok, &left, &retry),
           AUTH_OK);
}

static auth_rc gstart(int slot, unsigned perms, const char *text) {
  int exp, retry;
  fake.mono_ms += (PS5H_CODE_RESTART_S + 1) * 1000;
  return auth_grant_start(slot, perms, text, &exp, &retry);
}

static auth_rc gfinish(int slot, const char *code, unsigned *perms, int *left) {
  int retry;
  return auth_grant_finish(slot, code, perms, left, &retry);
}

static void test_grant_start_shows_label_perms_and_code(void) {
  int exp = 0, retry = 0;
  const char *want = "ps5-hass: \"Home Assistant\" asks to allow power off, "
                     "fan threshold. Code: ";
  setup();
  pair_as(CLIENT, "Home Assistant");
  T_EQ_INT(auth_grant_start(0, PERM_POWER_OFF | PERM_FAN_CONTROL,
                            "power off, fan threshold", &exp, &retry),
           AUTH_OK);
  T_EQ_INT(exp, 120);
  T_ASSERT(strncmp(fake.last_notify, want, strlen(want)) == 0);
  T_EQ_INT(strlen(fake.last_notify), strlen(want) + 6);
  for (int i = 0; i < 6; i++) {
    T_ASSERT(shown_code()[i] >= '0' && shown_code()[i] <= '9');
  }
}

static void test_grant_finish_correct_returns_perms(void) {
  unsigned perms = 0;
  int left;
  char code[7];
  setup();
  pair_as(CLIENT, "Home Assistant");
  T_EQ_INT(gstart(0, PERM_POWER_OFF | PERM_FAN_CONTROL, "x"), AUTH_OK);
  snprintf(code, sizeof(code), "%s", shown_code());
  T_EQ_INT(gfinish(0, code, &perms, &left), AUTH_OK);
  T_EQ_INT(perms, PERM_POWER_OFF | PERM_FAN_CONTROL);
  T_EQ_INT(gfinish(0, code, &perms, &left), AUTH_NO_CODE);
}

static void test_pair_code_not_accepted_for_grant_and_back(void) {
  unsigned perms;
  int left, retry, exp;
  char tok[65], code[7];
  setup();
  pair_as(CLIENT, "Home Assistant");
  /* open pairing code, no grant code: grant finish must refuse it */
  start();
  snprintf(code, sizeof(code), "%s", shown_code());
  T_ASSERT(gfinish(0, code, &perms, &left) != AUTH_OK);
  /* open grant code: pairing finish must refuse it */
  fake.mono_ms += (PS5H_CODE_RESTART_S + 1) * 1000;
  auth_pair_start(&exp, &retry); /* replaces the pairing code */
  T_EQ_INT(gstart(0, PERM_POWER_OFF, "power off"), AUTH_OK);
  snprintf(code, sizeof(code), "%s", shown_code());
  T_ASSERT(finish(code, CLIENT2, tok, &left, &retry) != AUTH_OK);
  T_EQ_INT(cfg.n_tokens, 1);
}

static void gfail_n(int n) {
  unsigned perms;
  int left;
  for (int i = 0; i < n; i++) {
    if (i % 5 == 0) {
      gstart(0, PERM_POWER_OFF, "power off");
    }
    gfinish(0, wrong_code(), &perms, &left);
  }
}

static void test_grant_wrong_codes_lock_grant_not_pairing(void) {
  int exp, retry = 0;
  setup();
  pair_as(CLIENT, "Home Assistant");
  gfail_n(10);
  T_EQ_INT(gstart(0, PERM_POWER_OFF, "power off"), AUTH_LOCKED);
  fake.mono_ms += (PS5H_CODE_RESTART_S + 1) * 1000;
  T_EQ_INT(auth_pair_start(&exp, &retry), AUTH_OK);
}

static void test_pairing_lock_does_not_block_grant(void) {
  int exp, retry = 0;
  setup();
  pair_as(CLIENT, "Home Assistant");
  fail_n(10);
  T_EQ_INT(auth_pair_start(&exp, &retry), AUTH_LOCKED);
  T_EQ_INT(gstart(0, PERM_POWER_OFF, "power off"), AUTH_OK);
}

static void test_grant_restart_within_30s_rate_limited(void) {
  int exp, retry = 0;
  setup();
  pair_as(CLIENT, "Home Assistant");
  T_EQ_INT(gstart(0, PERM_POWER_OFF, "power off"), AUTH_OK);
  fake.mono_ms += 29000;
  T_EQ_INT(auth_grant_start(0, PERM_POWER_OFF, "power off", &exp, &retry),
           AUTH_RATE_LIMITED);
  T_EQ_INT(retry, 1);
}

static void test_grant_code_expires_after_120s(void) {
  unsigned perms;
  int left;
  char code[7];
  setup();
  pair_as(CLIENT, "Home Assistant");
  gstart(0, PERM_POWER_OFF, "power off");
  snprintf(code, sizeof(code), "%s", shown_code());
  fake.mono_ms += 120001;
  T_EQ_INT(gfinish(0, code, &perms, &left), AUTH_NO_CODE);
}

static void test_grant_code_exhausted_after_5(void) {
  unsigned perms;
  int left;
  char code[7];
  setup();
  pair_as(CLIENT, "Home Assistant");
  gstart(0, PERM_POWER_OFF, "power off");
  snprintf(code, sizeof(code), "%s", shown_code());
  for (int want = 4; want >= 0; want--) {
    T_EQ_INT(gfinish(0, wrong_code(), &perms, &left), AUTH_WRONG_CODE);
    T_EQ_INT(left, want);
  }
  T_EQ_INT(gfinish(0, code, &perms, &left), AUTH_CODE_EXHAUSTED);
}

static void test_grant_finish_other_client_no_code(void) {
  unsigned perms;
  int left;
  char code[7];
  setup();
  pair_as(CLIENT, "Home Assistant");
  pair_as(CLIENT2, "Other");
  gstart(0, PERM_POWER_OFF, "power off");
  snprintf(code, sizeof(code), "%s", shown_code());
  T_EQ_INT(gfinish(1, code, &perms, &left), AUTH_NO_CODE);
  T_EQ_INT(gfinish(0, code, &perms, &left), AUTH_OK);
}

static void test_grant_finish_after_token_revoked(void) {
  unsigned perms;
  int left;
  char code[7];
  setup();
  pair_as(CLIENT, "Home Assistant");
  pair_as(CLIENT2, "Other");
  gstart(1, PERM_POWER_OFF, "power off");
  snprintf(code, sizeof(code), "%s", shown_code());
  T_EQ_INT(auth_revoke(1), AUTH_OK);
  T_EQ_INT(gfinish(0, code, &perms, &left), AUTH_NO_CODE);
  T_EQ_INT(gfinish(1, code, &perms, &left), AUTH_BAD_INPUT);
}

static void test_grant_text_long_label_keeps_code(void) {
  char label[64 * 2 + 1] = "", clean[300], *q;
  setup();
  for (int i = 0; i < 64; i++) {
    strcat(label, "\xc3\xa4");
  }
  pair_as(CLIENT, label);
  T_EQ_INT(gstart(0, PERM_POWER_OFF, "power off"), AUTH_OK);
  T_EQ_INT(utf8_sanitize(fake.last_notify, strlen(fake.last_notify), clean,
                         sizeof(clean)),
           strlen(fake.last_notify));
  T_ASSERT(strcmp(clean, fake.last_notify) == 0);
  q = strchr(fake.last_notify + 11, '"'); /* end of the label */
  T_ASSERT(q && q - (fake.last_notify + 11) <= PS5H_GRANT_LABEL_MAX_BYTES);
  for (int i = 0; i < 6; i++) {
    T_ASSERT(shown_code()[i] >= '0' && shown_code()[i] <= '9');
  }
}

static void test_grant_bad_input(void) {
  unsigned perms;
  int left;
  setup();
  pair_as(CLIENT, "Home Assistant");
  T_EQ_INT(gstart(0, 0, "x"), AUTH_BAD_INPUT);
  T_EQ_INT(gstart(0, 8, "x"), AUTH_BAD_INPUT);
  T_EQ_INT(gstart(1, PERM_POWER_OFF, "x"), AUTH_BAD_INPUT);
  T_EQ_INT(gstart(-1, PERM_POWER_OFF, "x"), AUTH_BAD_INPUT);
  T_EQ_INT(gstart(0, PERM_POWER_OFF, "power off"), AUTH_OK);
  T_EQ_INT(gfinish(0, "12a456", &perms, &left), AUTH_BAD_INPUT);
}

int main(void) {
  T_RUN(test_pair_start_shows_code_on_tv);
  T_RUN(test_pair_start_notify_failed_holds_no_code);
  T_RUN(test_pair_start_random_unavailable);
  T_RUN(test_pair_restart_within_30s_rate_limited);
  T_RUN(test_code_expires_after_120s);
  T_RUN(test_success_returns_token_and_saves);
  T_RUN(test_bad_input);
  T_RUN(test_wrong_code_counts_down);
  T_RUN(test_code_exhausted_after_5);
  T_RUN(test_global_lock_after_10_and_doubling);
  T_RUN(test_success_resets_budget);
  T_RUN(test_pair_same_client_replaces);
  T_RUN(test_token_limit_409);
  T_RUN(test_check_rejects_malformed);
  T_RUN(test_revoke_removes_slot);
  T_RUN(test_last_used_write_throttled);
  T_RUN(test_grant_start_shows_label_perms_and_code);
  T_RUN(test_grant_finish_correct_returns_perms);
  T_RUN(test_pair_code_not_accepted_for_grant_and_back);
  T_RUN(test_grant_wrong_codes_lock_grant_not_pairing);
  T_RUN(test_pairing_lock_does_not_block_grant);
  T_RUN(test_grant_restart_within_30s_rate_limited);
  T_RUN(test_grant_code_expires_after_120s);
  T_RUN(test_grant_code_exhausted_after_5);
  T_RUN(test_grant_finish_other_client_no_code);
  T_RUN(test_grant_finish_after_token_revoked);
  T_RUN(test_grant_text_long_label_keeps_code);
  T_RUN(test_grant_bad_input);
  T_MAIN();
}
