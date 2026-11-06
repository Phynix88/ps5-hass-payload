/* Installed apps: union of /user/appmeta/<ID>/ and /user/app/<ID>/sce_sys/.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_APPS_H
#define PS5H_APPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { ICON_NONE, ICON_PNG, ICON_JPEG } icon_type;

typedef struct {
  char title_id[10];
  char name[256];
  char version[16]; /* "" = null */
  bool game;
  bool has_category;
  int64_t category;
  char icon_path[700];
  icon_type icon;
  int64_t icon_mtime, icon_size;
} app_t;

typedef struct {
  app_t *items;
  int count;
  char etag[17];
} app_list;

/* ^[A-Z]{4}[0-9]{5}$ */
bool apps_valid_id(const char *s);
/* 0 = ok (also when both folders are missing); sorted by title_id. */
int apps_scan(app_list *out);
void apps_free(app_list *list);
const app_t *apps_find(const app_list *list, const char *title_id);
/* ETag from stat only; equals list.etag right after apps_scan. */
int apps_current_etag(char out[17]);
/* Rescan when the ETag changed: 1 = rescanned, 0 = unchanged, < 0 error. */
int apps_refresh(app_list *list);
/* Pure; also used by the fuzzer. Fills name/version/game/category. */
void apps_parse_param(const char *buf, size_t len, int lang, app_t *inout);
/* 0..30 -> "ja-JP" ...; NULL otherwise. */
const char *apps_lang_code(int lang);

#endif
