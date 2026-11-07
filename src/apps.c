/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "apps.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json.h"
#include "limits.h"
#include "platform.h"
#include "power.h"

#define MAX_TOKENS 8192

/* SCE system language ids (ORBIS_SYSTEM_PARAM_LANG_*, OpenOrbis
 * include/orbis/_types/sys_service.h; 30 = Ukrainian on PS5) mapped to the
 * keys used in param.json "localizedParameters" (checked on FW 13.60). */
static const char *const LANGS[] = {
    "ja-JP", "en-US", "fr-FR", "es-ES", "de-DE", "it-IT", "nl-NL", "pt-PT",
    "ru-RU", "ko-KR", "zh-Hant", "zh-Hans", "fi-FI", "sv-SE", "da-DK",
    "nb-NO", "pl-PL", "pt-BR", "en-GB", "tr-TR", "es-419", "ar-AE", "fr-CA",
    "cs-CZ", "hu-HU", "el-GR", "ro-RO", "th-TH", "vi-VN", "id-ID", "uk-UA"};

const char *apps_lang_code(int lang) {
  if (lang < 0 || lang >= (int)(sizeof(LANGS) / sizeof(LANGS[0]))) {
    return NULL;
  }
  return LANGS[lang];
}

bool apps_valid_id(const char *s) {
  for (int i = 0; i < 9; i++) {
    char c = s[i];
    if (i < 4 ? !(c >= 'A' && c <= 'Z') : !(c >= '0' && c <= '9')) {
      return false;
    }
  }
  return s[9] == '\0';
}

/* --- param.json -------------------------------------------------------- */

/* titleName of localizedParameters[key] into out; false if missing/empty. */
static bool title_for(const jr_t *r, int lp, const char *key, char *out,
                      size_t cap) {
  char raw[1024];
  int obj = jr_get(r, lp, key);
  if (obj < 0 || !jr_string(r, jr_get(r, obj, "titleName"), raw, sizeof(raw)) ||
      !raw[0]) {
    return false;
  }
  utf8_sanitize(raw, strlen(raw), out, cap);
  return out[0] != '\0';
}

void apps_parse_param(const char *buf, size_t len, int lang, app_t *a) {
  jsmntok_t *tok = malloc(MAX_TOKENS * sizeof(*tok));
  jr_t r;
  char s[64];
  int lp;
  if (!tok) {
    return;
  }
  if (jr_parse(&r, buf, len, tok, MAX_TOKENS) || tok[0].type != JSMN_OBJECT) {
    free(tok);
    return;
  }
  if (jr_string(&r, jr_get(&r, 0, "applicationDrmType"), s, sizeof(s))) {
    a->game = !strcmp(s, "standard");
  }
  a->has_category =
      jr_int(&r, jr_get(&r, 0, "applicationCategoryType"), &a->category);
  if (jr_string(&r, jr_get(&r, 0, "contentVersion"), s, sizeof(s))) {
    utf8_sanitize(s, strlen(s), a->version, sizeof(a->version));
  }

  lp = jr_get(&r, 0, "localizedParameters");
  if (lp >= 0 && tok[lp].type == JSMN_OBJECT) {
    const char *code = apps_lang_code(lang);
    char def[32];
    bool found = (code && title_for(&r, lp, code, a->name, sizeof(a->name))) ||
                 (jr_string(&r, jr_get(&r, lp, "defaultLanguage"), def,
                            sizeof(def)) &&
                  title_for(&r, lp, def, a->name, sizeof(a->name))) ||
                 title_for(&r, lp, "en-US", a->name, sizeof(a->name));
    /* first entry with a title */
    for (int i = lp + 1; !found && i < r.ntok && tok[i].start < tok[lp].end;
         i++) {
      char key[32];
      if (tok[i].parent == lp && tok[i].type == JSMN_STRING &&
          jr_string(&r, i, key, sizeof(key))) {
        found = title_for(&r, lp, key, a->name, sizeof(a->name));
      }
    }
  }
  free(tok);
}

/* --- files -------------------------------------------------------------- */

/* Regular file, no symlink, at most max bytes; returns fd or -1. */
static int open_regular(const char *path, int64_t max, struct stat *st) {
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    return -1;
  }
  if (fstat(fd, st) || !S_ISREG(st->st_mode) || st->st_size > max) {
    close(fd);
    return -1;
  }
  return fd;
}

/* Same checks with lstat only (for the ETag). */
static bool stat_regular(const char *path, int64_t max, struct stat *st) {
  return !lstat(path, st) && S_ISREG(st->st_mode) && st->st_size <= max;
}

typedef struct {
  char dir[700];
  struct stat dir_st, param_st;
  bool has_param;
} source;

/* appmeta/<ID>/ wins when its param.json is usable, then user/app/<ID>/sce_sys/;
 * without any usable param.json the first existing folder (for the icon). */
static bool choose_source(const char *id, source *src) {
  const char *fmt[2] = {"%s/user/appmeta/%s", "%s/user/app/%s/sce_sys"};
  bool any = false;
  source first;
  for (int i = 0; i < 2; i++) {
    source s;
    char p[760];
    memset(&s, 0, sizeof(s));
    snprintf(s.dir, sizeof(s.dir), fmt[i], plat_root(), id);
    if (stat(s.dir, &s.dir_st) || !S_ISDIR(s.dir_st.st_mode)) {
      continue;
    }
    snprintf(p, sizeof(p), "%s/param.json", s.dir);
    s.has_param = stat_regular(p, PS5H_PARAM_MAX, &s.param_st);
    if (s.has_param) {
      *src = s;
      return true;
    }
    if (!any) {
      first = s;
      any = true;
    }
  }
  if (any) {
    *src = first;
  }
  return any;
}

typedef struct {
  char ids[PS5H_MAX_APPS][10];
  int n;
} id_set;

static void collect(id_set *set, const char *sub) {
  char path[600];
  DIR *d;
  struct dirent *e;
  snprintf(path, sizeof(path), "%s%s", plat_root(), sub);
  if (!(d = opendir(path))) {
    return;
  }
  while ((e = readdir(d)) && set->n < PS5H_MAX_APPS) {
    bool dup = false;
    if (!apps_valid_id(e->d_name) || !strncmp(e->d_name, "NPXS", 4)) {
      continue;
    }
    for (int i = 0; i < set->n && !dup; i++) {
      dup = !strcmp(set->ids[i], e->d_name);
    }
    if (!dup) {
      memcpy(set->ids[set->n++], e->d_name, 10);
    }
  }
  closedir(d);
}

static int cmp_id(const void *a, const void *b) { return strcmp(a, b); }

static id_set *collect_ids(void) {
  id_set *set = calloc(1, sizeof(*set));
  if (!set) {
    return NULL;
  }
  collect(set, "/user/appmeta");
  collect(set, "/user/app");
  qsort(set->ids, (size_t)set->n, sizeof(set->ids[0]), cmp_id);
  return set;
}

/* FNV-1a 64 over title_id, folder mtime, param.json mtime. */
static uint64_t fnv(uint64_t h, const void *data, size_t n) {
  const unsigned char *p = data;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

static uint64_t etag_add(uint64_t h, const char *id, const source *s) {
  int64_t t[2] = {(int64_t)s->dir_st.st_mtime,
                  s->has_param ? (int64_t)s->param_st.st_mtime : 0};
  h = fnv(h, id, 10);
  return fnv(h, t, sizeof(t));
}

#define FNV_INIT 0xcbf29ce484222325ULL

static void etag_hex(uint64_t h, char out[17]) {
  snprintf(out, 17, "%016llx", (unsigned long long)h);
}

int apps_current_etag(char out[17]) {
  id_set *set = collect_ids();
  uint64_t h = FNV_INIT;
  if (!set) {
    return -1;
  }
  for (int i = 0; i < set->n; i++) {
    source s;
    if (choose_source(set->ids[i], &s)) {
      h = etag_add(h, set->ids[i], &s);
    }
  }
  free(set);
  etag_hex(h, out);
  return 0;
}

/* Set when the last scan could not ask the console for the language. */
static bool names_without_language;

/* -1 (no language) outside "on": the console is not asked then. */
static int language(void) {
  if (!power_is_on()) {
    names_without_language = true;
    return -1;
  }
  return plat_language();
}

static void read_param(const source *s, app_t *a) {
  char p[760];
  struct stat st;
  char *buf;
  ssize_t n;
  int fd;
  if (!s->has_param) {
    return;
  }
  snprintf(p, sizeof(p), "%s/param.json", s->dir);
  if ((fd = open_regular(p, PS5H_PARAM_MAX, &st)) < 0) {
    return;
  }
  if ((buf = malloc((size_t)st.st_size + 1))) {
    n = read(fd, buf, (size_t)st.st_size);
    if (n > 0) {
      apps_parse_param(buf, (size_t)n, language(), a);
    }
    free(buf);
  }
  close(fd);
}

static void read_icon(const source *s, app_t *a) {
  static const unsigned char png[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a,
                                       '\n'};
  unsigned char head[8];
  struct stat st;
  int fd;
  snprintf(a->icon_path, sizeof(a->icon_path), "%s/icon0.png", s->dir);
  if ((fd = open_regular(a->icon_path, PS5H_ICON_MAX, &st)) < 0) {
    return;
  }
  if (read(fd, head, sizeof(head)) == (ssize_t)sizeof(head)) {
    if (!memcmp(head, png, 8)) {
      a->icon = ICON_PNG;
    } else if (head[0] == 0xff && head[1] == 0xd8 && head[2] == 0xff) {
      a->icon = ICON_JPEG;
    }
  }
  if (a->icon != ICON_NONE) {
    a->icon_mtime = (int64_t)st.st_mtime;
    a->icon_size = (int64_t)st.st_size;
  }
  close(fd);
}

int apps_scan(app_list *out) {
  id_set *set = collect_ids();
  uint64_t h = FNV_INIT;
  memset(out, 0, sizeof(*out));
  names_without_language = false;
  if (!set) {
    return -1;
  }
  out->items = calloc((size_t)(set->n ? set->n : 1), sizeof(app_t));
  if (!out->items) {
    free(set);
    return -1;
  }
  for (int i = 0; i < set->n; i++) {
    app_t *a = &out->items[out->count];
    source s;
    if (!choose_source(set->ids[i], &s)) {
      continue;
    }
    h = etag_add(h, set->ids[i], &s);
    memcpy(a->title_id, set->ids[i], 10);
    read_param(&s, a);
    if (!a->name[0]) {
      memcpy(a->name, a->title_id, 10);
    }
    if (!s.has_param && !strncmp(a->title_id, "CUSA", 4)) {
      a->game = true;
    }
    read_icon(&s, a);
    out->count++;
  }
  free(set);
  etag_hex(h, out->etag);
  return 0;
}

void apps_free(app_list *list) {
  free(list->items);
  memset(list, 0, sizeof(*list));
}

const app_t *apps_find(const app_list *list, const char *title_id) {
  for (int i = 0; i < list->count; i++) {
    if (!strcmp(list->items[i].title_id, title_id)) {
      return &list->items[i];
    }
  }
  return NULL;
}

int apps_refresh(app_list *list) {
  char e[17];
  app_list fresh;
  if (!power_is_on()) {
    return 0; /* no scan outside "on"; it catches up afterwards */
  }
  if (apps_current_etag(e)) {
    return -1;
  }
  if (list->items && !names_without_language && !strcmp(e, list->etag)) {
    return 0;
  }
  if (apps_scan(&fresh)) {
    return -1;
  }
  apps_free(list);
  *list = fresh;
  return 1;
}
