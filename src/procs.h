/* Running payloads: process names minus Sony's own.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_PROCS_H
#define PS5H_PROCS_H

#include "platform.h"

/* Keeps names ending in ".elf" or cut to 19 characters, drops Sony
 * processes, sorts, removes duplicates. Returns the number written. */
int procs_filter(plat_name *in, int n, plat_name *out, int max);
/* procs_filter over the console's process list. */
int procs_payloads(plat_name *out, int max);

#endif
