/* config.json and device_id.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_CONFIG_H
#define PS5H_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "limits.h"

typedef struct {
  char token[65];
  char client_id[37];
  char label[PS5H_MAX_LABEL * 4 + 1];
  int64_t created, last_used, last_written;
} token_slot;

typedef struct {
  uint32_t net, mask; /* host byte order */
} cidr;

typedef struct {
  uint16_t port;
  cidr allow[PS5H_MAX_ALLOW];
  int n_allow;
  bool allow_broken; /* an invalid entry: block every address */
  char hosts[PS5H_MAX_HOSTS][256];
  int n_hosts;
  int fan_target_c;  /* 0 = console's own value */
  int fan_stock_c;   /* learned factory value, 0 = unknown */
  int fan_written_c; /* last value we wrote ourselves, never learned as factory */
  token_slot tokens[PS5H_MAX_TOKENS];
  int n_tokens;
  bool recovered; /* the file was broken and has been set aside */
} config;

/* Missing file: defaults. Broken file: renamed to config.json.broken-<time>,
 * notification on the TV, recovered=true, defaults. Always 0 unless the
 * data folder cannot be used at all. */
int config_load(config *c);
/* temp file + fsync + rename; folder 0700, file 0600. */
int config_save(const config *c);
/* Read or create once from plat_random: 32 lowercase hex characters. */
int device_id_get(char out[33]);

/* <root>/data/ps5-hass, created (0700) when missing. */
int config_data_dir(char *out, size_t cap);
/* Whole regular file (no symlink) of at most max bytes; malloc'd and
 * NUL-terminated, *len set. NULL with errno ENOENT when missing, EINVAL when
 * too large or not a regular file. */
char *config_read_small(const char *path, size_t max, size_t *len);
/* temp file + fsync + rename, mode 0600; 0 = ok. */
int config_write_atomic(const char *path, const char *data, size_t len);
/* Shared helpers (also used by auth). */
bool config_is_hex(const char *s, int n);
/* "2026-11-06T13:00:00Z" */
void config_iso_time(int64_t t, char out[21]);

#endif
