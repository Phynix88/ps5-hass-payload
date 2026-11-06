/* Pairing code, failure budget, tokens.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_AUTH_H
#define PS5H_AUTH_H

#include <stdbool.h>

#include "config.h"

typedef enum {
  AUTH_OK,
  AUTH_RATE_LIMITED,
  AUTH_LOCKED,
  AUTH_NOTIFY_FAILED,
  AUTH_RANDOM_UNAVAILABLE,
  AUTH_NO_CODE,
  AUTH_WRONG_CODE,
  AUTH_CODE_EXHAUSTED,
  AUTH_TOKEN_LIMIT,
  AUTH_BAD_INPUT,
  AUTH_SAVE_FAILED
} auth_rc;

/* Permissions that a client can ask for (v0.3), named like config.json. */
enum {
  PERM_POWER_ACTIONS = 1u,
  PERM_POWER_OFF = 2u,
  PERM_FAN_CONTROL = 4u,
  PERM_ALL = 7u
};

/* Keeps a pointer to the loaded config; resets all pairing state. */
void auth_init(config *c);
/* retry_after: seconds, set for AUTH_RATE_LIMITED and AUTH_LOCKED. */
auth_rc auth_pair_start(int *expires_in, int *retry_after);
auth_rc auth_pair_finish(const char *code, const char *client_id,
                         const char *label, char token_out[65],
                         int *attempts_left, int *retry_after);
/* Slot index or -1. Compares every slot in constant time. */
int auth_check(const char *bearer_header);
auth_rc auth_revoke(int slot);
bool auth_paired(void);

/* Grant code (own slot and lock, separate from pairing): shows
 * `ps5-hass: "<label>" asks to allow <perms_text>. Code: 123456` for the
 * client of token slot `slot`. */
auth_rc auth_grant_start(int slot, unsigned perms, const char *perms_text,
                         int *expires_in, int *retry_after);
/* AUTH_OK: *perms_out = the set from start; the code is used up. A code of
 * another client: AUTH_NO_CODE. */
auth_rc auth_grant_finish(int slot, const char *code, unsigned *perms_out,
                          int *attempts_left, int *retry_after);

#endif
