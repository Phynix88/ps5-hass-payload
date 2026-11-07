/* The console's own play time from app.db: sums over users, odd files, the
 * hourly limit, nothing while the console is not "on".
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include <sys/stat.h>

#include "limits.h"
#include "platform_fake.h"
#include "power.h"
#include "sonytime.h"
#include "sqlite3.h"

#define T0 100000
#define HOUR_MS (PS5H_SONYTIME_REFRESH_S * 1000LL)
#define RETRY_MS (PS5H_SONYTIME_RETRY_S * 1000LL)

static char db_path[700], copy_path[700], new_path[700];

static void setup(void) {
  fake_reset();
  snprintf(fake.root, sizeof(fake.root), "%s", t_tmpdir());
  snprintf(db_path, sizeof(db_path), "%s/system_data/priv/mms/app.db",
           fake.root);
  snprintf(copy_path, sizeof(copy_path), "%s/data/ps5-hass/app.db.copy",
           fake.root);
  snprintf(new_path, sizeof(new_path), "%s.new", copy_path);
  power_init();
  sonytime_reset();
}

/* A whole file (binary), malloc'd; NULL when missing. */
static char *slurp(const char *path, size_t *len) {
  struct stat st;
  char *buf;
  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  if (fstat(fileno(f), &st) || !(buf = malloc((size_t)st.st_size + 1))) {
    fclose(f);
    return NULL;
  }
  *len = fread(buf, 1, (size_t)st.st_size, f);
  fclose(f);
  return buf;
}

static void mms_dir(void) {
  char dir[700];
  snprintf(dir, sizeof(dir), "%s/system_data/priv/mms", fake.root);
  t_mkdirs(dir);
}

/* Replaces the synthetic app.db by one built from this SQL. */
static void make_db(const char *sql) {
  sqlite3 *db = NULL;
  mms_dir();
  unlink(db_path);
  if (sqlite3_open(db_path, &db) || sqlite3_exec(db, sql, 0, 0, NULL)) {
    fprintf(stderr, "synthetic db failed\n");
    exit(2);
  }
  sqlite3_close(db);
}

#define USER_TABLE(name)                                                   \
  "CREATE TABLE " name " (titleId TEXT, playedTimeOnConsole INT,"          \
  " gltTotalPlayedTime INT, lastPlayedDateOnConsole TEXT);"

#define ONE_USER(secs)                                                     \
  USER_TABLE("tbl_iconinfo_1a2b3c4d")                                      \
  "INSERT INTO tbl_iconinfo_1a2b3c4d VALUES ('PPSA00002', " #secs ", 0, 'x');"

static void test_sums_over_users(void) {
  struct stat st;
  setup();
  make_db(USER_TABLE("tbl_iconinfo_1a2b3c4d")
          USER_TABLE("tbl_iconinfo_5e6f7a8b")
          USER_TABLE("tbl_other")
          "INSERT INTO tbl_iconinfo_1a2b3c4d VALUES"
          " ('PPSA00002', 3600, 999999, 'x'),"
          " ('PPSA00009', 100, 5, 'x'),"
          " ('CUSA00010', NULL, 7, 'x'),"
          " ('PPSA00011', NULL, 7, 'x'),"
          " ('bad-id', 50, 0, 'x'),"
          " (NULL, 50, 0, 'x'),"
          " ('PPSA00002', NULL, 0, 'x');"
          "INSERT INTO tbl_iconinfo_5e6f7a8b VALUES"
          " ('PPSA00002', 1800, 111111, 'x'),"
          " ('PPSA00011', 0, 0, 'x'),"
          " ('PPSA00012', 42, 0, 'x');"
          "INSERT INTO tbl_other VALUES ('PPSA00009', 77000, 0, 'x');");
  T_EQ_INT(sonytime_refresh(T0), 0);
  T_EQ_INT(sonytime_of("PPSA00002"), 5400);
  T_EQ_INT(sonytime_of("PPSA00009"), 100);
  T_EQ_INT(sonytime_of("PPSA00012"), 42);
  T_EQ_INT(sonytime_of("PPSA00011"), 0);  /* a real 0 in one table */
  T_EQ_INT(sonytime_of("CUSA00010"), -1); /* only NULLs */
  T_EQ_INT(sonytime_of("PPSA99999"), -1); /* not in the file */
  T_EQ_INT(sonytime_of("bad-id"), -1);
  T_EQ_INT(sonytime_of(NULL), -1);
  /* the copy is private */
  T_EQ_INT(stat(copy_path, &st), 0);
  T_EQ_INT(st.st_mode & 0777, 0600);
}

static void test_missing_column_unknown(void) {
  setup();
  make_db("CREATE TABLE tbl_iconinfo_1a2b3c4d (titleId TEXT, gltTotalPlayedTime INT);"
          "INSERT INTO tbl_iconinfo_1a2b3c4d VALUES ('PPSA00002', 3600);");
  T_EQ_INT(sonytime_refresh(T0), -1);
  T_EQ_INT(sonytime_of("PPSA00002"), -1);
  /* one table fine, one without the column: nothing is trusted */
  setup();
  make_db(ONE_USER(3600)
          "CREATE TABLE tbl_iconinfo_5e6f7a8b (titleId TEXT, x INT);");
  T_EQ_INT(sonytime_refresh(T0), -1);
  T_EQ_INT(sonytime_of("PPSA00002"), -1);
  /* no user table at all */
  setup();
  make_db("CREATE TABLE tbl_other (titleId TEXT, playedTimeOnConsole INT);");
  T_EQ_INT(sonytime_refresh(T0), -1);
  T_EQ_INT(sonytime_of("PPSA00002"), -1);
}

static void test_missing_file_unknown(void) {
  setup();
  T_EQ_INT(sonytime_refresh(T0), -1);
  T_EQ_INT(sonytime_of("PPSA00002"), -1);
  /* not a database at all */
  setup();
  mms_dir();
  t_write_str(db_path, "this is not sqlite, just some text of some length");
  T_EQ_INT(sonytime_refresh(T0), -1);
  T_EQ_INT(sonytime_of("PPSA00002"), -1);
}

static void test_refresh_at_most_hourly(void) {
  setup();
  make_db(ONE_USER(60));
  T_EQ_INT(sonytime_refresh(T0), 0);
  T_EQ_INT(sonytime_of("PPSA00002"), 60);
  make_db(ONE_USER(120));
  T_EQ_INT(sonytime_refresh(T0 + 1000), 1); /* skipped */
  T_EQ_INT(sonytime_refresh(T0 + HOUR_MS - 1), 1);
  T_EQ_INT(sonytime_of("PPSA00002"), 60);
  T_EQ_INT(sonytime_refresh(T0 + HOUR_MS), 0);
  T_EQ_INT(sonytime_of("PPSA00002"), 120);
}

/* Sony's number only grows: a failed read keeps what was read before. */
static void test_failure_keeps_values(void) {
  setup();
  make_db(ONE_USER(60));
  T_EQ_INT(sonytime_refresh(T0), 0);
  T_EQ_INT(sonytime_of("PPSA00002"), 60);
  unlink(db_path); /* gone */
  T_EQ_INT(sonytime_refresh(T0 + HOUR_MS), -1);
  T_EQ_INT(sonytime_of("PPSA00002"), 60);
  mms_dir(); /* torn: not a database */
  t_write_str(db_path, "this is not sqlite, just some text of some length");
  T_EQ_INT(sonytime_refresh(T0 + HOUR_MS + RETRY_MS), -1);
  T_EQ_INT(sonytime_of("PPSA00002"), 60);
  make_db("CREATE TABLE tbl_iconinfo_1a2b3c4d (titleId TEXT, x INT);");
  T_EQ_INT(sonytime_refresh(T0 + HOUR_MS + 2 * RETRY_MS), -1); /* layout */
  T_EQ_INT(sonytime_of("PPSA00002"), 60);
  make_db(ONE_USER(90)); /* and it recovers */
  T_EQ_INT(sonytime_refresh(T0 + HOUR_MS + 3 * RETRY_MS), 0);
  T_EQ_INT(sonytime_of("PPSA00002"), 90);
}

/* A failure is retried after PS5H_SONYTIME_RETRY_S, a success waits an hour. */
static void test_retry_after_60s(void) {
  setup();
  T_EQ_INT(PS5H_SONYTIME_RETRY_S, 60);
  T_EQ_INT(sonytime_refresh(T0), -1); /* nothing there yet */
  make_db(ONE_USER(60));
  T_EQ_INT(sonytime_refresh(T0 + 1000), 1);
  T_EQ_INT(sonytime_refresh(T0 + RETRY_MS - 1), 1);
  T_EQ_INT(sonytime_of("PPSA00002"), -1);
  T_EQ_INT(sonytime_refresh(T0 + RETRY_MS), 0);
  T_EQ_INT(sonytime_of("PPSA00002"), 60);
  /* success: the long wait applies again */
  make_db(ONE_USER(120));
  T_EQ_INT(sonytime_refresh(T0 + RETRY_MS + HOUR_MS - 1), 1);
  T_EQ_INT(sonytime_refresh(T0 + RETRY_MS + HOUR_MS), 0);
  T_EQ_INT(sonytime_of("PPSA00002"), 120);
  /* a failure after a success: short wait, value kept */
  unlink(db_path);
  T_EQ_INT(sonytime_refresh(T0 + RETRY_MS + 2 * HOUR_MS), -1);
  make_db(ONE_USER(180));
  T_EQ_INT(sonytime_refresh(T0 + RETRY_MS + 2 * HOUR_MS + RETRY_MS - 1), 1);
  T_EQ_INT(sonytime_of("PPSA00002"), 120);
  T_EQ_INT(sonytime_refresh(T0 + RETRY_MS + 2 * HOUR_MS + RETRY_MS), 0);
  T_EQ_INT(sonytime_of("PPSA00002"), 180);
}

/* A failed copy or read never replaces the last good app.db.copy. */
static void test_failed_copy_keeps_old_copy(void) {
  size_t before_len = 0, after_len = 0;
  char *before, *after;
  setup();
  make_db(ONE_USER(60));
  T_EQ_INT(sonytime_refresh(T0), 0);
  before = slurp(copy_path, &before_len);
  T_ASSERT(before && before_len > 0);
  /* torn source: copied fine, but not a database */
  t_write_str(db_path, "this is not sqlite, just some text of some length");
  T_EQ_INT(sonytime_refresh(T0 + HOUR_MS), -1);
  after = slurp(copy_path, &after_len);
  T_ASSERT(after && after_len == before_len && !memcmp(before, after, before_len));
  free(after);
  T_ASSERT(access(new_path, F_OK) != 0); /* no leftover of the attempt */
  /* source gone */
  unlink(db_path);
  T_EQ_INT(sonytime_refresh(T0 + HOUR_MS + RETRY_MS), -1);
  after = slurp(copy_path, &after_len);
  T_ASSERT(after && after_len == before_len && !memcmp(before, after, before_len));
  free(after);
  free(before);
  /* a good read replaces it */
  make_db(ONE_USER(61));
  T_EQ_INT(sonytime_refresh(T0 + HOUR_MS + 2 * RETRY_MS), 0);
  T_ASSERT(access(new_path, F_OK) != 0);
  T_EQ_INT(sonytime_of("PPSA00002"), 61);
}

/* app.db above PS5H_SONYTIME_MAX_COPY is not copied. */
static void test_size_cap(void) {
  sqlite3 *db = NULL;
  setup();
  make_db(ONE_USER(60));
  T_EQ_INT(sqlite3_open(db_path, &db), 0);
  T_EQ_INT(sqlite3_exec(db,
                        "CREATE TABLE pad (b BLOB);"
                        "INSERT INTO pad VALUES (zeroblob(9 * 1024 * 1024));",
                        0, 0, NULL),
           0);
  sqlite3_close(db);
  T_EQ_INT(PS5H_SONYTIME_MAX_COPY, 8 * 1024 * 1024);
  T_EQ_INT(sonytime_refresh(T0), -1);
  T_EQ_INT(sonytime_of("PPSA00002"), -1);
  T_ASSERT(access(copy_path, F_OK) != 0);
  T_ASSERT(access(new_path, F_OK) != 0);
}

/* Only tbl_iconinfo_<hex> counts; other names are ignored, not fatal. */
static void test_odd_table_names_ignored(void) {
  setup();
  make_db(ONE_USER(60)
          "CREATE TABLE \"tbl_iconinfo_1\"\"; DROP TABLE x; --\" (titleId TEXT, y INT);"
          "CREATE TABLE tbl_iconinfo_xyz (titleId TEXT, y INT);"
          "CREATE TABLE tbl_iconinfo_ (titleId TEXT, y INT);"
          "CREATE TABLE TBL_ICONINFO_1A (titleId TEXT, playedTimeOnConsole INT);"
          "INSERT INTO TBL_ICONINFO_1A VALUES ('PPSA00002', 1000);"
          "CREATE TABLE tbl_iconinfo_1a2b3c4d_old (titleId TEXT, y INT);");
  T_EQ_INT(sonytime_refresh(T0), 0);
  T_EQ_INT(sonytime_of("PPSA00002"), 60);
}

/* More than 64 user tables: refused (and kept values stay), not read in part. */
static void test_too_many_tables(void) {
  static char sql[16384];
  size_t n = 0;
  setup();
  for (int i = 0; i < 65; i++) {
    n += (size_t)snprintf(sql + n, sizeof(sql) - n,
                          "CREATE TABLE tbl_iconinfo_%x (titleId TEXT,"
                          " playedTimeOnConsole INT);", 0x100 + i);
  }
  make_db(sql);
  T_EQ_INT(sonytime_refresh(T0), -1);
  T_EQ_INT(sonytime_of("PPSA00002"), -1);
}

static void test_skipped_when_not_on(void) {
  setup();
  make_db(ONE_USER(60));
  fake.power_state = 500;
  power_tick(fake.mono_ms);
  T_ASSERT(!power_is_on());
  T_EQ_INT(sonytime_refresh(T0), 1);
  T_EQ_INT(sonytime_of("PPSA00002"), -1);
  T_ASSERT(access(copy_path, F_OK) != 0); /* Sony's file was not even read */
  /* skipping did not use up the hour: the first run after "on" works */
  fake.power_state = 1000;
  power_tick(fake.mono_ms);
  T_EQ_INT(sonytime_refresh(T0 + 10), 0);
  T_EQ_INT(sonytime_of("PPSA00002"), 60);
  /* and what was read stays while the console rests */
  fake.power_state = 300;
  power_tick(fake.mono_ms);
  T_EQ_INT(sonytime_refresh(T0 + 10 + HOUR_MS), 1);
  T_EQ_INT(sonytime_of("PPSA00002"), 60);
}

static void test_wal_mode_database(void) {
  sqlite3 *db = NULL;
  setup();
  make_db(ONE_USER(60));
  T_EQ_INT(sqlite3_open(db_path, &db), 0);
  T_EQ_INT(sqlite3_exec(db,
                        "PRAGMA journal_mode=WAL;"
                        "INSERT INTO tbl_iconinfo_1a2b3c4d VALUES"
                        " ('PPSA00009', 7, 0, 'x');",
                        0, 0, NULL),
           0);
  sqlite3_close(db); /* checkpoints and removes the -wal file */
  T_EQ_INT(sonytime_refresh(T0), 0);
  T_EQ_INT(sonytime_of("PPSA00002"), 60);
  T_EQ_INT(sonytime_of("PPSA00009"), 7);
}

int main(void) {
  T_RUN(test_sums_over_users);
  T_RUN(test_missing_column_unknown);
  T_RUN(test_missing_file_unknown);
  T_RUN(test_refresh_at_most_hourly);
  T_RUN(test_failure_keeps_values);
  T_RUN(test_retry_after_60s);
  T_RUN(test_failed_copy_keeps_old_copy);
  T_RUN(test_size_cap);
  T_RUN(test_odd_table_names_ignored);
  T_RUN(test_too_many_tables);
  T_RUN(test_skipped_when_not_on);
  T_RUN(test_wal_mode_database);
  T_MAIN();
}
