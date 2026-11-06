/* Pairing code, failure budget, tokens (design spec section 6).
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

#endif
