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

static char db_path[700], copy_path[700];

static void setup(void) {
  fake_reset();
  snprintf(fake.root, sizeof(fake.root), "%s", t_tmpdir());
  snprintf(db_path, sizeof(db_path), "%s/system_data/priv/mms/app.db",
           fake.root);
  snprintf(copy_path, sizeof(copy_path), "%s/data/ps5-hass/app.db.copy",
           fake.root);
  power_init();
  sonytime_reset();
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
  /* a file that vanished after a good read is unknown again */
  setup();
  make_db(ONE_USER(60));
  T_EQ_INT(sonytime_refresh(T0), 0);
  T_EQ_INT(sonytime_of("PPSA00002"), 60);
  unlink(db_path);
  T_EQ_INT(sonytime_refresh(T0 + HOUR_MS), -1);
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
  /* a failed run also waits an hour */
  unlink(db_path);
  T_EQ_INT(sonytime_refresh(T0 + 2 * HOUR_MS), -1);
  make_db(ONE_USER(180));
  T_EQ_INT(sonytime_refresh(T0 + 2 * HOUR_MS + 5000), 1);
  T_EQ_INT(sonytime_refresh(T0 + 3 * HOUR_MS), 0);
  T_EQ_INT(sonytime_of("PPSA00002"), 180);
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
  T_RUN(test_skipped_when_not_on);
  T_RUN(test_wal_mode_database);
  T_MAIN();
}
