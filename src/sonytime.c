/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sonytime.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>
#include <unistd.h>

#include "apps.h"
#include "config.h"
#include "limits.h"
#include "log.h"
#include "platform.h"
#include "power.h"

#define SRC_PATH "/system_data/priv/mms/app.db"
#define COPY_NAME "app.db.copy"
#define NEW_NAME "app.db.copy.new" /* the attempt; becomes COPY_NAME on success */
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
  int64_t last_ms; /* of the last attempt */
  int64_t next_ms; /* earliest next attempt */
  bool fail_logged; /* one log line per streak of failures */
  int64_t retry_s; /* wait after the next failure; 0 = the first one */
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
  st.next_ms = 0;
  st.fail_logged = false;
  st.retry_s = 0;
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

/* Sony's file to <data>/app.db.copy.new; NULL = ok, else why not. The last
 * good app.db.copy is not touched. */
static const char *copy_file(char *dst, size_t cap, char *final, size_t fcap) {
  char dir[600], src[600];
  size_t len = 0;
  char *buf;
  int rc;
  snprintf(src, sizeof(src), "%s%s", plat_root(), SRC_PATH);
  if (config_data_dir(dir, sizeof(dir)) ||
      (size_t)snprintf(dst, cap, "%s/%s", dir, NEW_NAME) >= cap ||
      (size_t)snprintf(final, fcap, "%s/%s", dir, COPY_NAME) >= fcap) {
    return "data folder not usable";
  }
  if (!(buf = config_read_small(src, PS5H_SONYTIME_MAX_COPY, &len))) {
    return errno == ENOENT ? "file missing"
         : errno == EINVAL ? "file too large or not a regular file"
                           : "file not readable";
  }
  rc = config_write_atomic(dst, buf, len);
  free(buf);
  return rc ? "copy could not be written" : NULL;
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

/* tbl_iconinfo_<hex user id>; anything else is none of our business. */
static bool is_user_table(const char *name) {
  const char *p = name + sizeof(TABLE_PREFIX) - 1;
  if (strncmp(name, TABLE_PREFIX, sizeof(TABLE_PREFIX) - 1) || !*p) {
    return false;
  }
  for (; *p; p++) {
    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
          (*p >= 'A' && *p <= 'F'))) {
      return false;
    }
  }
  return true;
}

/* Names of the user tables (not logged, not output). -1 = error, -2 = more
 * than MAX_TABLES. */
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
    if (name && strlen(name) < MAX_NAME && is_user_table(name)) {
      if (n == MAX_TABLES) {
        sqlite3_finalize(s);
        return -2;
      }
      strcpy(names[n++], name);
    }
  }
  sqlite3_finalize(s);
  return rc == SQLITE_DONE ? n : -1;
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

/* Reads the copy at path; NULL = ok (the list is replaced), else why not. */
static const char *read_db(const char *path) {
  static char names[MAX_TABLES][MAX_NAME];
  char uri[1500];
  sqlite3 *db = NULL;
  entry *list = NULL;
  const char *why = NULL;
  int n = 0, tables;
  if (!make_uri(path, uri, sizeof(uri))) {
    return "path too long";
  }
  if (sqlite3_open_v2(uri, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL)) {
    why = "copy could not be opened";
    goto out;
  }
  tables = list_tables(db, names);
  if (tables == -2) {
    why = "more than 64 user tables";
    goto out;
  }
  if (tables <= 0) {
    why = "no user tables found";
    goto out;
  }
  for (int i = 0; i < tables; i++) {
    if (!has_columns(db, names[i]) || !sum_table(db, names[i], &list, &n)) {
      why = "layout not as expected";
      goto out;
    }
  }
  qsort(list, (size_t)n, sizeof(entry), cmp_entry);
  clear_list();
  st.list = list;
  st.n = n;
  list = NULL;
out:
  free(list);
  sqlite3_close(db);
  return why;
}

int sonytime_refresh(int64_t now_ms) {
  char attempt[700] = "", copy[700] = "";
  const char *why;
  if (!power_is_on()) {
    return 1;
  }
  if (st.have_run && now_ms >= st.last_ms && now_ms < st.next_ms) {
    return 1;
  }
  st.have_run = true;
  st.last_ms = now_ms;
  why = copy_file(attempt, sizeof(attempt), copy, sizeof(copy));
  if (!why) {
    why = read_db(attempt);
  }
  if (why) {
    /* Sony's number only grows: what was read before stays. A short wait
     * that doubles per failure in a row, and one log line per streak. */
    if (attempt[0]) {
      unlink(attempt);
    }
    st.retry_s = st.retry_s ? st.retry_s * 2 : PS5H_SONYTIME_RETRY_S;
    if (st.retry_s > PS5H_SONYTIME_RETRY_MAX_S) {
      st.retry_s = PS5H_SONYTIME_RETRY_MAX_S;
    }
    st.next_ms = now_ms + st.retry_s * 1000;
    if (!st.fail_logged) {
      log_msg("app.db: %s", why);
      st.fail_logged = true;
    }
    return -1;
  }
  if (rename(attempt, copy)) {
    unlink(attempt);
  }
  st.next_ms = now_ms + (int64_t)PS5H_SONYTIME_REFRESH_S * 1000;
  st.fail_logged = false;
  st.retry_s = 0;
  return 0;
}
