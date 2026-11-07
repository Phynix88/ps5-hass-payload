/* Compatibility answer for v0.3 clients: /info.permissions. There are no
 * permissions in v0.4; pairing is the only barrier.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_PERM_H
#define PS5H_PERM_H

#include "json.h"

/* "permissions":{"power_actions":{"allowed":b,"available":b},...} into an
 * open object. allowed is always equal to available (what the console can do
 * according to plat_capabilities). */
void perm_json(jw_t *w);

#endif
