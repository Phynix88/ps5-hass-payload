/* Permissions granted from Home Assistant with a code on the TV (design v0.3 §5).
 * Names as in config.json: power_actions, power_off, fan_control.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_PERM_H
#define PS5H_PERM_H

#include <stddef.h>

#include "auth.h"
#include "config.h"
#include "json.h"

/* JSON array of permission names → PERM_* bits. -1: missing, not an array,
 * empty, unknown or duplicate name, or an element that is not a string. */
int perm_parse(const jr_t *r, int arr_tok, unsigned *out);
/* What this console can do (standby, power off, fan readable). */
unsigned perm_available(void);
unsigned perm_allowed(const config *c);
/* "rest mode, power off, fan threshold" for the bits set, in this order. */
void perm_text(unsigned perms, char *out, size_t cap);
/* Sets `grant` bits, clears `revoke` bits and saves config.json. Save fails:
 * config restored, -1, fan untouched. After a successful save that took
 * fan_control away: the fan goes back to the factory value (fan_release). */
int perm_apply(config *c, unsigned grant, unsigned revoke);
/* "permissions":{"power_actions":{"allowed":b,"available":b},...} into an
 * open object. */
void perm_json(jw_t *w, const config *c);

#endif
