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

/* One code slot with its own failure budget and lock. Pairing and grants
 * each have one, so a device without a token cannot block granting. */
typedef struct {
  const char *what; /* "pairing" / "grant", for the log */
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
} code_state;

static code_state pair_st, grant_st;
static char grant_client[37]; /* the grant code belongs to this client */
static unsigned grant_perms;

void auth_init(config *c) {
  cfg = c;
  memset(&pair_st, 0, sizeof(pair_st));
  memset(&grant_st, 0, sizeof(grant_st));
  pair_st.what = "pairing";
  grant_st.what = "grant";
  grant_client[0] = '\0';
  grant_perms = 0;
}

bool auth_paired(void) { return cfg && cfg->n_tokens > 0; }

static int seconds_left(int64_t until_ms, int64_t now_ms) {
  int64_t ms = until_ms - now_ms;
  return ms <= 0 ? 0 : (int)((ms + 999) / 1000);
}

static bool locked(const code_state *cs, int64_t now, int *retry_after) {
  if (now < cs->lock_until_ms) {
    *retry_after = seconds_left(cs->lock_until_ms, now);
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

/* Shows `prefix` + a new code on the TV. */
static auth_rc code_start(code_state *cs, const char *prefix, int *expires_in,
                          int *retry_after) {
  int64_t now = plat_mono_ms();
  char code[7], text[256];
  *retry_after = 0;
  if (locked(cs, now, retry_after)) {
    return AUTH_LOCKED;
  }
  if (cs->started && now - cs->last_start_ms < PS5H_CODE_RESTART_S * 1000) {
    *retry_after = seconds_left(cs->last_start_ms + PS5H_CODE_RESTART_S * 1000,
                                now);
    return AUTH_RATE_LIMITED;
  }
  if (make_code(code)) {
    log_error("%s: no randomness source", cs->what);
    return AUTH_RANDOM_UNAVAILABLE;
  }
  cs->active = false;
  snprintf(text, sizeof(text), "%s%s", prefix, code);
  if (plat_notify(text) < 0) {
    log_error("%s: notification failed", cs->what);
    return AUTH_NOTIFY_FAILED;
  }
  memcpy(cs->code, code, sizeof(code));
  cs->active = true;
  cs->exhausted = false;
  cs->fails = 0;
  cs->expires_ms = now + PS5H_CODE_TTL_S * 1000;
  cs->started = true;
  cs->last_start_ms = now;
  *expires_in = PS5H_CODE_TTL_S;
  return AUTH_OK;
}

auth_rc auth_pair_start(int *expires_in, int *retry_after) {
  auth_rc rc = code_start(&pair_st, "ps5-hass pairing code: ", expires_in,
                          retry_after);
  if (rc == AUTH_OK) {
    log_msg("pairing: code shown on TV");
  }
  return rc;
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
  /* no control characters: the label goes into the log and onto the TV */
  for (char *q = out; *q; q++) {
    if ((unsigned char)*q < 0x20 || *q == 0x7f) {
      *q = ' ';
    }
  }
  for (const unsigned char *p = (const unsigned char *)out; *p; p++) {
    chars += (*p & 0xc0) != 0x80;
  }
  return chars >= 1 && chars <= PS5H_MAX_LABEL && strlen(in) < cap;
}

static void count_failure(code_state *cs, int64_t now) {
  cs->global_fails++;
  if (cs->global_fails >= PS5H_GLOBAL_MAX_FAILS) {
    int64_t secs = PS5H_LOCK_BASE_S;
    for (int i = 0; i < cs->lock_level && secs < PS5H_LOCK_MAX_S; i++) {
      secs *= 2;
    }
    if (secs > PS5H_LOCK_MAX_S) {
      secs = PS5H_LOCK_MAX_S;
    }
    cs->lock_until_ms = now + secs * 1000;
    cs->lock_level++;
    cs->global_fails = 0;
    log_error("%s: %d failures in a row, locked for %lld s", cs->what,
              PS5H_GLOBAL_MAX_FAILS, (long long)secs);
  }
}

/* Checks a code against the slot. AUTH_OK uses the code up and resets the
 * failure budget. */
static auth_rc code_check(code_state *cs, const char *code, int *attempts_left,
                          int *retry_after) {
  int64_t now = plat_mono_ms();
  if (locked(cs, now, retry_after)) {
    return AUTH_LOCKED;
  }
  if (cs->active && now > cs->expires_ms) {
    cs->active = false;
  }
  if (!cs->active) {
    return AUTH_NO_CODE;
  }
  if (cs->exhausted) {
    return AUTH_CODE_EXHAUSTED;
  }
  if (!ct_equal(code, cs->code, 6)) {
    cs->fails++;
    *attempts_left = PS5H_CODE_MAX_FAILS - cs->fails;
    if (cs->fails >= PS5H_CODE_MAX_FAILS) {
      cs->exhausted = true;
    }
    count_failure(cs, now);
    if (locked(cs, now, retry_after)) {
      return AUTH_LOCKED;
    }
    return AUTH_WRONG_CODE;
  }
  cs->active = false;
  cs->global_fails = 0;
  cs->lock_level = 0;
  return AUTH_OK;
}

auth_rc auth_pair_finish(const char *code, const char *client_id,
                         const char *label, char token_out[65],
                         int *attempts_left, int *retry_after) {
  char clean[sizeof(cfg->tokens[0].label)];
  token_slot backup[PS5H_MAX_TOKENS];
  int n_backup, slot = -1;
  uint8_t raw[32];
  auth_rc rc;

  *attempts_left = 0;
  *retry_after = 0;
  if (!valid_code(code) || !valid_uuid(client_id) ||
      !clean_label(label, clean, sizeof(clean))) {
    return AUTH_BAD_INPUT;
  }
  if ((rc = code_check(&pair_st, code, attempts_left, retry_after)) != AUTH_OK) {
    return rc;
  }
  /* Correct code: it is used up whatever happens next. */
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

/* --- grants (v0.3) ------------------------------------------------------ */

/* Copies at most `max` bytes of `in`, never cutting a UTF-8 character;
 * control characters become spaces (the label is shown on the TV). */
static void utf8_prefix(const char *in, size_t max, char *out) {
  size_t n = strlen(in);
  if (n > max) {
    n = max;
    while (n > 0 && ((unsigned char)in[n] & 0xc0) == 0x80) {
      n--;
    }
  }
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)in[i];
    out[i] = ch < 0x20 || ch == 0x7f ? ' ' : (char)ch;
  }
  out[n] = '\0';
}

static bool valid_slot(int slot) {
  return cfg && slot >= 0 && slot < cfg->n_tokens;
}

auth_rc auth_grant_start(int slot, unsigned perms, const char *perms_text,
                         int *expires_in, int *retry_after) {
  char label[PS5H_GRANT_LABEL_MAX_BYTES + 1], prefix[200];
  auth_rc rc;
  *retry_after = 0;
  if (!valid_slot(slot) || !perms || (perms & ~(unsigned)PERM_ALL) ||
      !perms_text) {
    return AUTH_BAD_INPUT;
  }
  utf8_prefix(cfg->tokens[slot].label, PS5H_GRANT_LABEL_MAX_BYTES, label);
  snprintf(prefix, sizeof(prefix), "ps5-hass: \"%s\" asks to allow %s. Code: ",
           label, perms_text);
  rc = code_start(&grant_st, prefix, expires_in, retry_after);
  if (rc == AUTH_OK) {
    memcpy(grant_client, cfg->tokens[slot].client_id, sizeof(grant_client));
    grant_perms = perms;
    log_msg("grant: code shown for \"%s\"", label);
  }
  return rc;
}

auth_rc auth_grant_finish(int slot, const char *code, unsigned *perms_out,
                          int *attempts_left, int *retry_after) {
  auth_rc rc;
  *perms_out = 0;
  *attempts_left = 0;
  *retry_after = 0;
  if (!valid_slot(slot) || !valid_code(code)) {
    return AUTH_BAD_INPUT;
  }
  if (locked(&grant_st, plat_mono_ms(), retry_after)) {
    return AUTH_LOCKED;
  }
  /* A code belongs to the client that asked for it; others do not even
   * learn whether one is open, and do not use up its attempts. */
  if (!grant_st.active || strcmp(grant_client, cfg->tokens[slot].client_id)) {
    return AUTH_NO_CODE;
  }
  if ((rc = code_check(&grant_st, code, attempts_left, retry_after)) != AUTH_OK) {
    return rc;
  }
  *perms_out = grant_perms;
  grant_perms = 0;
  grant_client[0] = '\0';
  return AUTH_OK;
}
