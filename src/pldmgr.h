/* Starts and stops payloads through the Payload Manager (its HTTP interface
 * on this console). Not a Sony call: plain TCP to a local port. Only on
 * request, never in the loop; every phase is bounded by
 * PS5H_PLDMGR_TIMEOUT_MS.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_PLDMGR_H
#define PS5H_PLDMGR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  char slot[256];
  char name[64]; /* display name */
  char version[32]; /* the Payload Manager's version string, may be "" */
  bool running;
  bool protected_; /* never started or stopped from here */
  int pid;         /* of the running process, else 0 */
} pld_entry;

/* Number of entries (<= max), or < 0: the Payload Manager does not answer
 * (-ECONNREFUSED, -ETIMEDOUT), answers nonsense (-EPROTO) or too much
 * (-EMSGSIZE). Slots that do not fit pld_entry, or are not printable UTF-8,
 * are left out. */
int pldmgr_list(pld_entry *out, int max);
/* Everything (list and action) happens within PS5H_PLDMGR_TIMEOUT_MS in total.
 * 0 = started. -EPERM protected (no request is made), -EEXIST already runs,
 * -EBUSY an app or game runs (or the console does not tell; no request is
 * made), -ESRCH no such slot, -EINVAL bad slot, other < 0: see pldmgr_list, or -EIO
 * when the Payload Manager refuses. */
int pldmgr_start(const char *slot);
/* 0 = stopped (the first process of the payload gets killed). -ENOENT does
 * not run; other codes as pldmgr_start. */
int pldmgr_stop(const char *slot);
/* Case-insensitive substring: kstuff, etahen, elfldr, pldmgr, ps5-hass, cheat,
 * ps5debug. */
bool pldmgr_is_protected(const char *s);
/* Base name of a payload file or slot: path and extension (.elf, .bin) cut,
 * then a version part cut from the last "_" when it starts with a digit or
 * "v" and a digit ("CheatRunner_v0.18.1.elf" -> "CheatRunner",
 * "ShadowMountPlus_1.7beta7.elf" -> "ShadowMountPlus"). A slot without a
 * file ("repo/ftpsrv@e8cce5d1") gives its id ("ftpsrv"). */
void pldmgr_base_name(const char *slot, char out[64]);
/* Does the process (as the console lists it, with or without ".elf")
 * belong to a payload with this id and/or file name (either may be NULL)?
 * Case-insensitive, on the first 19 characters; a name cut inside ".elf"
 * (exactly 19 characters ending in ".", ".e" or ".el") counts as without it. */
bool pldmgr_process_matches(const char *proc, const char *id,
                            const char *filename);
/* True when the last contact with the Payload Manager worked. */
bool pldmgr_last_ok(void);
/* Tests and the host server: other address and port (IPv4 literal). */
void pldmgr_set_target(const char *ip, uint16_t port);

#endif
