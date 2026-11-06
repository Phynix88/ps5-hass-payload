/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "auth.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "json.h"
#include "limits.h"
#include "log.h"
#include "platform.h"

static config *cfg;

static struct {
  bool active;      /* a code is held */
  bool exhausted;   /* 5 wrong attempts on this code */
  char code[7];
  int64_t expires_ms;
  int fails;        /* on this code */
  bool started;
  int64_t last_start_ms;
  int global_fails; /* in a row, across codes */
  int lock_level;
  int64_t lock_until_ms;
} st;

void auth_init(config *c) {
  cfg = c;
  memset(&st, 0, sizeof(st));
}

bool auth_paired(void) { return cfg && cfg->n_tokens > 0; }

static int seconds_left(int64_t until_ms, int64_t now_ms) {
  int64_t ms = until_ms - now_ms;
  return ms <= 0 ? 0 : (int)((ms + 999) / 1000);
}

static bool locked(int64_t now, int *retry_after) {
  if (now < st.lock_until_ms) {
    *retry_after = seconds_left(st.lock_until_ms, now);
    return true;
  }
  return false;
}

/* Constant-time equality of n bytes. */
static bool ct_equal(const char *a, const char *b, size_t n) {
  volatile unsigned char d = 0;
  for (size_t i = 0; i < n; i++) {
    d |= (unsigned char)(a[i] ^ b[i]);
  }
  return d == 0;
}

/* 6 digits without modulo bias: reject 32-bit values >= 4 294 000 000. */
static int make_code(char out[7]) {
  for (int tries = 0; tries < 64; tries++) {
    uint32_t v;
    if (plat_random((uint8_t *)&v, sizeof(v)) < 0) {
      return -1;
    }
    if (v < 4294000000u) {
      snprintf(out, 7, "%06u", (unsigned)(v % 1000000u));
      return 0;
    }
  }
  return -1;
}

auth_rc auth_pair_start(int *expires_in, int *retry_after) {
  int64_t now = plat_mono_ms();
  char code[7], text[64];
  *retry_after = 0;
  if (locked(now, retry_after)) {
    return AUTH_LOCKED;
  }
  if (st.started && now - st.last_start_ms < PS5H_CODE_RESTART_S * 1000) {
    *retry_after = seconds_left(st.last_start_ms + PS5H_CODE_RESTART_S * 1000,
                                now);
    return AUTH_RATE_LIMITED;
  }
  if (make_code(code)) {
    log_error("pairing: no randomness source");
    return AUTH_RANDOM_UNAVAILABLE;
  }
  st.active = false;
  snprintf(text, sizeof(text), "ps5-hass pairing code: %s", code);
  if (plat_notify(text) < 0) {
    log_error("pairing: notification failed");
    return AUTH_NOTIFY_FAILED;
  }
  memcpy(st.code, code, sizeof(code));
  st.active = true;
  st.exhausted = false;
  st.fails = 0;
  st.expires_ms = now + PS5H_CODE_TTL_S * 1000;
  st.started = true;
  st.last_start_ms = now;
  *expires_in = PS5H_CODE_TTL_S;
  log_msg("pairing: code shown on TV");
  return AUTH_OK;
}

static bool valid_code(const char *s) {
  for (int i = 0; i < 6; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
  }
  return s[6] == '\0';
}

static bool valid_uuid(const char *s) {
  for (int i = 0; i < 36; i++) {
    bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? s[i] != '-'
             : !((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
      return false;
    }
  }
  return s[36] == '\0';
}

/* Sanitized label with 1..64 characters. */
static bool clean_label(const char *in, char *out, size_t cap) {
  int chars = 0;
  utf8_sanitize(in, strlen(in), out, cap);
  for (const unsigned char *p = (const unsigned char *)out; *p; p++) {
    chars += (*p & 0xc0) != 0x80;
  }
  return chars >= 1 && chars <= PS5H_MAX_LABEL && strlen(in) < cap;
}

static void count_failure(int64_t now) {
  st.global_fails++;
  if (st.global_fails >= PS5H_GLOBAL_MAX_FAILS) {
    int64_t secs = PS5H_LOCK_BASE_S;
    for (int i = 0; i < st.lock_level && secs < PS5H_LOCK_MAX_S; i++) {
      secs *= 2;
    }
    if (secs > PS5H_LOCK_MAX_S) {
      secs = PS5H_LOCK_MAX_S;
    }
    st.lock_until_ms = now + secs * 1000;
    st.lock_level++;
    st.global_fails = 0;
    log_error("pairing: %d failures in a row, locked for %lld s",
              PS5H_GLOBAL_MAX_FAILS, (long long)secs);
  }
}

auth_rc auth_pair_finish(const char *code, const char *client_id,
                         const char *label, char token_out[65],
                         int *attempts_left, int *retry_after) {
  int64_t now = plat_mono_ms();
  char clean[sizeof(cfg->tokens[0].label)];
  token_slot backup[PS5H_MAX_TOKENS];
  int n_backup, slot = -1;
  uint8_t raw[32];

  *attempts_left = 0;
  *retry_after = 0;
  if (!valid_code(code) || !valid_uuid(client_id) ||
      !clean_label(label, clean, sizeof(clean))) {
    return AUTH_BAD_INPUT;
  }
  if (locked(now, retry_after)) {
    return AUTH_LOCKED;
  }
  if (st.active && now > st.expires_ms) {
    st.active = false;
  }
  if (!st.active) {
    return AUTH_NO_CODE;
  }
  if (st.exhausted) {
    return AUTH_CODE_EXHAUSTED;
  }
  if (!ct_equal(code, st.code, 6)) {
    st.fails++;
    *attempts_left = PS5H_CODE_MAX_FAILS - st.fails;
    if (st.fails >= PS5H_CODE_MAX_FAILS) {
      st.exhausted = true;
    }
    count_failure(now);
    if (locked(now, retry_after)) {
      return AUTH_LOCKED;
    }
    return AUTH_WRONG_CODE;
  }

  /* Correct code: it is used up whatever happens next. */
  st.active = false;
  st.global_fails = 0;
  st.lock_level = 0;
  for (int i = 0; i < cfg->n_tokens; i++) {
    if (!strcmp(cfg->tokens[i].client_id, client_id)) {
      slot = i;
    }
  }
  if (slot < 0) {
    if (cfg->n_tokens >= PS5H_MAX_TOKENS) {
      return AUTH_TOKEN_LIMIT;
    }
    slot = cfg->n_tokens;
  }
  if (plat_random(raw, sizeof(raw)) < 0) {
    return AUTH_RANDOM_UNAVAILABLE;
  }
  memcpy(backup, cfg->tokens, sizeof(backup));
  n_backup = cfg->n_tokens;
  if (slot == cfg->n_tokens) {
    cfg->n_tokens++;
  }
  for (int i = 0; i < 32; i++) {
    snprintf(cfg->tokens[slot].token + 2 * i, 3, "%02x", raw[i]);
  }
  memcpy(cfg->tokens[slot].client_id, client_id, 37);
  memcpy(cfg->tokens[slot].label, clean, sizeof(clean));
  cfg->tokens[slot].created = cfg->tokens[slot].last_used =
      cfg->tokens[slot].last_written = plat_wall_s();
  if (config_save(cfg)) {
    memcpy(cfg->tokens, backup, sizeof(backup));
    cfg->n_tokens = n_backup;
    return AUTH_SAVE_FAILED;
  }
  memcpy(token_out, cfg->tokens[slot].token, 65);
  log_msg("pairing: paired \"%s\"", clean);
  return AUTH_OK;
}

int auth_check(const char *bearer) {
  int found = -1;
  int64_t now;
  if (!cfg || strncmp(bearer, "Bearer ", 7) || !config_is_hex(bearer + 7, 64)) {
    return -1;
  }
  for (int i = 0; i < cfg->n_tokens; i++) {
    if (ct_equal(bearer + 7, cfg->tokens[i].token, 64)) {
      found = i;
    }
  }
  if (found < 0) {
    return -1;
  }
  now = plat_wall_s();
  cfg->tokens[found].last_used = now;
  if (now - cfg->tokens[found].last_written >= PS5H_LAST_USED_WRITE_S) {
    if (!config_save(cfg)) {
      cfg->tokens[found].last_written = now;
    }
  }
  return found;
}

auth_rc auth_revoke(int slot) {
  if (!cfg || slot < 0 || slot >= cfg->n_tokens) {
    return AUTH_BAD_INPUT;
  }
  memmove(&cfg->tokens[slot], &cfg->tokens[slot + 1],
          (size_t)(cfg->n_tokens - slot - 1) * sizeof(cfg->tokens[0]));
  cfg->n_tokens--;
  memset(&cfg->tokens[cfg->n_tokens], 0, sizeof(cfg->tokens[0]));
  if (config_save(cfg)) {
    return AUTH_SAVE_FAILED;
  }
  log_msg("pairing: token revoked");
  return AUTH_OK;
}
