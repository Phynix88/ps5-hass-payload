/* Running payloads: process names minus Sony's own.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_PROCS_H
#define PS5H_PROCS_H

#include <stdbool.h>
#include <sys/types.h>

#include "platform.h"

/* Keeps names ending in ".elf" or cut to 19 characters, drops Sony
 * processes, sorts, removes duplicates. Returns the number written. */
int procs_filter(plat_name *in, int n, plat_name *out, int max);
/* procs_filter over the console's process list. */
int procs_payloads(plat_name *out, int max);
/* True when a game or app is in memory, also minimised (it then runs as
 * "eboot.bin"; the console reports no running app for it). True too when the
 * process list cannot be read. */
bool procs_game_in_memory(void);
/* The decision behind procs_game_in_memory for a process list: true when n is
 * not a usable count (an unreadable list is < 0, and an empty one cannot be
 * right, our own process is always listed), when n reaches cap (the list may
 * be cut) or when a process is named exactly "eboot.bin". */
bool procs_list_suggests_game(const plat_name *names, int n, int cap);
/* sceSystemServiceGetAppIdOfRunningBigApp() as plat_running_app() code before
 * the title lookup: -1 (nothing runs, also after minimising) = 0, any other
 * value <= 0 (0 or a 0x80xxxxxx console error) = -1, an app id = 1. */
int running_app_from_id(int app_id);
/* Asks the processes to end (SIGTERM), waits up to wait_ms for them to go,
 * then ends the rest with SIGKILL. Returns how many needed SIGKILL. */
int procs_stop(const pid_t *pids, int n, int wait_ms);

#endif
