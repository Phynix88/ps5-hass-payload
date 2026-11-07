/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "procs.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "limits.h"

/* Sony processes seen on FW 13.60 (checked 2026-11-06); everything starting
 * with "Sce" is Sony's too (also the ones cut to 19 characters). */
static const char *const SONY[] = {"fs_cleaner.elf", "AgcCompositor.elf",
                                   "orbis_audiod.elf", "mini-syscore.elf"};

static int is_payload(const char *name) {
  size_t len = strnlen(name, sizeof(plat_name));
  if (len == 0 || len >= sizeof(plat_name) || !strncmp(name, "Sce", 3)) {
    return 0;
  }
  for (size_t i = 0; i < sizeof(SONY) / sizeof(SONY[0]); i++) {
    if (!strcmp(name, SONY[i])) {
      return 0;
    }
  }
  return (len > 4 && !strcmp(name + len - 4, ".elf")) || len == 19;
}

static int cmp(const void *a, const void *b) { return strcmp(a, b); }

int procs_filter(plat_name *in, int n, plat_name *out, int max) {
  int count = 0;
  for (int i = 0; i < n; i++) {
    int dup = 0;
    if (!is_payload(in[i])) {
      continue;
    }
    for (int j = 0; j < count && !dup; j++) {
      dup = !strcmp(out[j], in[i]);
    }
    if (!dup && count < max) {
      memcpy(out[count++], in[i], sizeof(plat_name));
    }
  }
  qsort(out, (size_t)count, sizeof(plat_name), cmp);
  return count;
}

int procs_payloads(plat_name *out, int max) {
  plat_name *all = calloc(PS5H_MAX_PROCS, sizeof(plat_name));
  int n;
  if (!all) {
    return 0;
  }
  n = plat_proc_names(all, PS5H_MAX_PROCS);
  n = n > 0 ? procs_filter(all, n, out, max) : 0;
  free(all);
  return n;
}

bool procs_list_suggests_game(const plat_name *names, int n, int cap) {
  if (n <= 0 || n >= cap) {
    return true; /* unreadable, empty or possibly cut: fail safe */
  }
  for (int i = 0; i < n; i++) {
    if (!strncmp(names[i], "eboot.bin", sizeof(plat_name))) {
      return true;
    }
  }
  return false;
}

int running_app_from_id(int app_id) {
  if (app_id == -1) {
    return 0; /* nothing runs (measured on FW 13.60), also after minimising */
  }
  return app_id > 0 ? 1 : -1; /* any other value is a console error (0x80xxxxxx) */
}

bool procs_game_in_memory(void) {
  plat_name *all = calloc(PS5H_MAX_PROCS, sizeof(plat_name));
  bool found;
  if (!all) {
    return true;
  }
  found = procs_list_suggests_game(all, plat_proc_names(all, PS5H_MAX_PROCS),
                                   PS5H_MAX_PROCS);
  free(all);
  return found;
}

static bool alive(pid_t pid) { return kill(pid, 0) == 0 || errno != ESRCH; }

int procs_stop(const pid_t *pids, int n, int wait_ms) {
  int killed = 0;
  bool left = false;
  for (int i = 0; i < n; i++) {
    kill(pids[i], SIGTERM);
  }
  for (int waited = 0;; waited += 50) {
    left = false;
    for (int i = 0; i < n && !left; i++) {
      left = alive(pids[i]);
    }
    if (!left || waited >= wait_ms) {
      break;
    }
    usleep(50 * 1000);
  }
  for (int i = 0; left && i < n; i++) {
    if (alive(pids[i])) {
      kill(pids[i], SIGKILL);
      killed++;
    }
  }
  return killed;
}
