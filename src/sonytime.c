/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sonytime.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "apps.h"
#include "config.h"
#include "limits.h"
#include "log.h"
#include "platform.h"
#include "power.h"

#define SRC_PATH "/system_data/priv/mms/app.db"
#define COPY_NAME "app.db.copy"
#define TABLE_PREFIX "tbl_iconinfo_"
#define MAX_TABLES 64
#define MAX_NAME 128 /* a table name is "tbl_iconinfo_" + a hex user id */
#define MAX_SECS 10000000000LL /* sanity cap per title */

typedef struct {
  char id[10];
  int64_t secs;
} entry;

static struct {
  entry *list; /* sorted by id; only titles with a known value */
  int n;
  bool have_run;
  int64_t last_ms;
} st;

static void clear_list(void) {
  free(st.list);
  st.list = NULL;
  st.n = 0;
}

void sonytime_reset(void) {
  clear_list();
  st.have_run = false;
  st.last_ms = 0;
}

static int cmp_entry(const void *a, const void *b) {
  return strcmp(((const entry *)a)->id, ((const entry *)b)->id);
}

int64_t sonytime_of(const char *title_id) {
  entry key, *hit;
  if (!title_id || !apps_valid_id(title_id) || !st.n) {
    return -1;
  }
  snprintf(key.id, sizeof(key.id), "%s", title_id);
  hit = bsearch(&key, st.list, (size_t)st.n, sizeof(entry), cmp_entry);
  return hit ? hit->secs : -1;
}

/* --- the copy ----------------------------------------------------------- */

static int copy_file(char *copy, size_t cap) {
  char dir[600], src[600];
  size_t len = 0;
  char *buf;
  int rc;
  snprintf(src, sizeof(src), "%s%s", plat_root(), SRC_PATH);
  if (config_data_dir(dir, sizeof(dir)) ||
      (size_t)snprintf(copy, cap, "%s/%s", dir, COPY_NAME) >= cap) {
    return -1;
  }
  if (!(buf = config_read_small(src, PS5H_SONYTIME_MAX_COPY, &len))) {
    return -1;
  }
  rc = config_write_atomic(copy, buf, len);
  free(buf);
  return rc ? -1 : 0;
}

/* file:/path?immutable=1 with the few characters that end a URI path escaped. */
static bool make_uri(const char *path, char *out, size_t cap) {
  size_t n = (size_t)snprintf(out, cap, "file:");
  for (; *path; path++) {
    const char *esc = *path == '%' ? "%25" : *path == '?' ? "%3f"
                    : *path == '#' ? "%23" : NULL;
    size_t add = esc ? 3 : 1;
    if (n + add + sizeof("?immutable=1") >= cap) {
      return false;
    }
    if (esc) {
      memcpy(out + n, esc, 3);
    } else {
      out[n] = *path;
    }
    n += add;
  }
  snprintf(out + n, cap - n, "?immutable=1");
  return true;
}

/* --- reading ------------------------------------------------------------ */

/* Names of the tbl_iconinfo_* tables. Not logged, not output. */
static int list_tables(sqlite3 *db, char names[][MAX_NAME]) {
  sqlite3_stmt *s;
  int n = 0, rc;
  if (sqlite3_prepare_v2(db,
                         "SELECT name FROM sqlite_master WHERE type='table'",
                         -1, &s, NULL)) {
    return -1;
  }
  while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(s, 0);
    if (name && !strncmp(name, TABLE_PREFIX, sizeof(TABLE_PREFIX) - 1) &&
        strlen(name) < MAX_NAME) {
      if (n == MAX_TABLES) {
        n = -1;
        break;
      }
      strcpy(names[n++], name);
    }
  }
  sqlite3_finalize(s);
  return rc == SQLITE_DONE || n < 0 ? n : -1;
}

/* Both columns we use exist (looked up with PRAGMA table_info only). */
static bool has_columns(sqlite3 *db, const char *table) {
  sqlite3_stmt *s;
  char *sql = sqlite3_mprintf("PRAGMA table_info(\"%w\")", table);
  bool title = false, played = false;
  int rc;
  if (!sql || sqlite3_prepare_v2(db, sql, -1, &s, NULL)) {
    sqlite3_free(sql);
    return false;
  }
  sqlite3_free(sql);
  while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
    const char *col = (const char *)sqlite3_column_text(s, 1);
    title |= col && !sqlite3_stricmp(col, "titleId");
    played |= col && !sqlite3_stricmp(col, "playedTimeOnConsole");
  }
  sqlite3_finalize(s);
  return rc == SQLITE_DONE && title && played;
}

static entry *find_or_add(entry **list, int *n, const char *id) {
  for (int i = 0; i < *n; i++) {
    if (!strcmp((*list)[i].id, id)) {
      return &(*list)[i];
    }
  }
  if (*n >= PS5H_SONYTIME_MAX_TITLES) {
    return NULL;
  }
  {
    entry *grown = realloc(*list, (size_t)(*n + 1) * sizeof(entry));
    if (!grown) {
      return NULL;
    }
    *list = grown;
  }
  snprintf((*list)[*n].id, sizeof((*list)[*n].id), "%s", id);
  (*list)[*n].secs = 0;
  return &(*list)[(*n)++];
}

/* Adds one user table to the sums; false = not usable. */
static bool sum_table(sqlite3 *db, const char *table, entry **list, int *n) {
  sqlite3_stmt *s;
  char *sql = sqlite3_mprintf(
      "SELECT titleId, SUM(playedTimeOnConsole), COUNT(playedTimeOnConsole) "
      "FROM \"%w\" WHERE titleId IS NOT NULL GROUP BY titleId",
      table);
  int rc;
  if (!sql || sqlite3_prepare_v2(db, sql, -1, &s, NULL)) {
    sqlite3_free(sql);
    return false;
  }
  sqlite3_free(sql);
  while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
    const char *id = (const char *)sqlite3_column_text(s, 0);
    int64_t v = sqlite3_column_int64(s, 1);
    entry *e;
    if (!id || !apps_valid_id(id) || sqlite3_column_int64(s, 2) <= 0) {
      continue; /* a title of only NULLs adds no entry: unknown */
    }
    if (!(e = find_or_add(list, n, id))) {
      sqlite3_finalize(s);
      return false;
    }
    v = v < 0 ? 0 : v;
    e->secs = e->secs + v > MAX_SECS ? MAX_SECS : e->secs + v;
  }
  sqlite3_finalize(s);
  return rc == SQLITE_DONE;
}

static int read_db(const char *copy_path) {
  static char names[MAX_TABLES][MAX_NAME];
  char uri[1500];
  sqlite3 *db = NULL;
  entry *list = NULL;
  int n = 0, tables, rc = -1;
  if (!make_uri(copy_path, uri, sizeof(uri))) {
    return -1;
  }
  if (sqlite3_open_v2(uri, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL)) {
    log_msg("app.db: copy could not be opened");
    goto out;
  }
  if ((tables = list_tables(db, names)) <= 0) {
    log_msg("app.db: no user tables found");
    goto out;
  }
  for (int i = 0; i < tables; i++) {
    if (!has_columns(db, names[i]) || !sum_table(db, names[i], &list, &n)) {
      log_msg("app.db: layout not as expected");
      goto out;
    }
  }
  qsort(list, (size_t)n, sizeof(entry), cmp_entry);
  clear_list();
  st.list = list;
  st.n = n;
  list = NULL;
  rc = 0;
out:
  free(list);
  sqlite3_close(db);
  return rc;
}

int sonytime_refresh(int64_t now_ms) {
  char copy[700];
  int rc;
  if (!power_is_on()) {
    return 1;
  }
  if (st.have_run && now_ms >= st.last_ms &&
      now_ms - st.last_ms < (int64_t)PS5H_SONYTIME_REFRESH_S * 1000) {
    return 1;
  }
  st.have_run = true;
  st.last_ms = now_ms;
  if (copy_file(copy, sizeof(copy))) {
    log_msg("app.db: not readable (%s)", errno == ENOENT ? "missing" : "error");
    clear_list();
    return -1;
  }
  rc = read_db(copy);
  if (rc) {
    clear_list();
  }
  return rc;
}
