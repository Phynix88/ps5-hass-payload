/* Play time: foreground counting, sessions, gaps, clock jumps, the file.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include <dirent.h>

#include "apps.h"
#include "config.h"
#include "limits.h"
#include "log.h"
#include "platform_fake.h"
#include "power.h"
#include "playtime.h"
#include "sonytime.h"
#include "sqlite3.h"

#define GAME "PPSA00002"
#define GAME2 "PPSA00009"
#define MEDIA "CUSA00010"
#define SHELL_ID 5000
#define DAY 86400

static app_t items[3];
static app_list list = {.items = items, .count = 3};
static char path[700];

static void setup(void) {
  fake_reset();
  snprintf(fake.root, sizeof(fake.root), "%s", t_tmpdir());
  snprintf(path, sizeof(path), "%s/data/ps5-hass/playtime.json", fake.root);
  memset(items, 0, sizeof(items));
  snprintf(items[0].title_id, 10, "%s", GAME);
  snprintf(items[0].name, sizeof(items[0].name), "Test Game");
  items[0].game = true;
  snprintf(items[1].title_id, 10, "%s", GAME2);
  items[1].game = true;
  snprintf(items[2].title_id, 10, "%s", MEDIA);
  power_init();
  sonytime_reset();
  playtime_load();
}

/* Files "playtime.json.bad*" in the data folder. */
static int count_bad(void) {
  char dir[700];
  int n = 0;
  struct dirent *e;
  DIR *d;
  snprintf(dir, sizeof(dir), "%s/data/ps5-hass", fake.root);
  if (!(d = opendir(dir))) {
    return 0;
  }
  while ((e = readdir(d))) {
    n += !strncmp(e->d_name, "playtime.json.bad", 17);
  }
  closedir(d);
  return n;
}

static void run(const char *title_id, int app_id) {
  if (!title_id) {
    fake.running.app_id = 0;
    return;
  }
  fake.running.app_id = app_id;
  snprintf(fake.running.title_id, sizeof(fake.running.title_id), "%s", title_id);
}

/* n seconds in steps of 5 s; a tick happens at each step (also at the end). */
static void pass(int seconds) {
  for (int i = 0; i < seconds / PS5H_PLAYTIME_POLL_S; i++) {
    fake.mono_ms += PS5H_PLAYTIME_POLL_S * 1000;
    fake.wall_s += PS5H_PLAYTIME_POLL_S;
    power_tick(fake.mono_ms);
    playtime_tick(&list, fake.mono_ms);
  }
}

static void tick_now(void) {
  power_tick(fake.mono_ms);
  playtime_tick(&list, fake.mono_ms);
}

static void json_of(char *out, size_t cap, int64_t since) {
  jw_t w;
  jw_init(&w);
  jw_obj(&w);
  playtime_json(&w, &list, since);
  jw_end_obj(&w);
  snprintf(out, cap, "%s", w.buf);
  jw_free(&w);
}

static char js[1 << 20];

static void test_counts_only_foreground(void) {
  setup();
  run(GAME, 100);
  fake.focused_app_id = SHELL_ID; /* home screen in front */
  tick_now();
  pass(30);
  T_EQ_INT(playtime_total_s(GAME), -1); /* nothing counted, no title yet */
  fake.focused_app_id = 100;
  pass(5); /* the session opens at this sample */
  pass(60);
  T_EQ_INT(playtime_total_s(GAME), 60);
  fake.focused_app_id = SHELL_ID; /* back to the home screen: closes */
  pass(30);
  T_EQ_INT(playtime_total_s(GAME), 60);
  T_EQ_INT(playtime_session_count(), 1);
}

static void test_control_centre_keeps_focus_and_counts(void) {
  setup();
  run(GAME, 100);
  fake.focused_app_id = 100; /* the control centre leaves the game's focus */
  tick_now();
  pass(120);
  T_EQ_INT(playtime_total_s(GAME), 120);
}

static void test_switch_title_closes_session(void) {
  setup();
  run(GAME, 100);
  fake.focused_app_id = 100;
  tick_now();
  pass(40);
  run(GAME2, 101);
  fake.focused_app_id = 101;
  pass(5); /* opens the second session at this sample */
  pass(20);
  run(GAME, 102); /* same title again: a new session */
  fake.focused_app_id = 102;
  pass(5);
  pass(10);
  T_EQ_INT(playtime_total_s(GAME), 50);
  T_EQ_INT(playtime_total_s(GAME2), 20);
  T_EQ_INT(playtime_session_count(), 2); /* the third is still running */
}

static void test_stop_closes_session(void) {
  char *p;
  setup();
  run(GAME, 100);
  fake.focused_app_id = 100;
  tick_now();
  pass(60);
  run(NULL, 0);
  pass(10);
  T_EQ_INT(playtime_session_count(), 1);
  T_EQ_INT(playtime_total_s(GAME), 60);
  json_of(js, sizeof(js), 0);
  T_ASSERT(strstr(js, "\"current\":null"));
  p = strstr(js, "\"sessions\":[{");
  T_ASSERT(p);
  T_ASSERT(strstr(p, "\"duration_s\":60"));
  T_ASSERT(strstr(p, "\"kind\":\"game\""));
}

static void test_gap_closes_session_at_last_sample(void) {
  setup();
  run(GAME, 100);
  fake.focused_app_id = 100;
  tick_now();
  pass(10);
  /* the loop did not run for 45 s (say, rest mode): no time for the gap */
  fake.mono_ms += 45000;
  fake.wall_s += 45;
  tick_now();
  T_EQ_INT(playtime_session_count(), 1);
  T_EQ_INT(playtime_total_s(GAME), 10);
  pass(20);
  T_EQ_INT(playtime_total_s(GAME), 30); /* a new session counts on */
}

static void test_wall_jump_back_closes_session(void) {
  char *p, want[128];
  int64_t restart;
  setup();
  run(GAME, 100);
  fake.focused_app_id = 100;
  tick_now();
  pass(20);
  fake.wall_s -= 3600;
  pass(5);
  restart = fake.wall_s; /* the new session starts at this sample */
  T_EQ_INT(playtime_session_count(), 1);
  T_EQ_INT(playtime_total_s(GAME), 20); /* never negative */
  pass(15);
  T_EQ_INT(playtime_total_s(GAME), 35);
  json_of(js, sizeof(js), 0);
  p = strstr(js, "\"current\":{");
  T_ASSERT(p);
  T_ASSERT(!strstr(p, "-"));
  snprintf(want, sizeof(want),
           "\"current\":{\"title_id\":\"" GAME "\",\"kind\":\"game\",\"start\":%lld,"
           "\"duration_s\":15}",
           (long long)restart);
  T_ASSERT(strstr(js, want)); /* the new start is the new wall time */
}

static void test_wall_jump_forward_closes_session(void) {
  setup();
  run(GAME, 100);
  fake.focused_app_id = 100;
  tick_now();
  pass(20);
  fake.wall_s += 7200; /* NTP, a wake-up */
  pass(5);
  pass(10);
  T_EQ_INT(playtime_total_s(GAME), 30); /* not inflated by two hours */
  T_EQ_INT(playtime_session_count(), 1);
}

static void test_not_on_pauses(void) {
  setup();
  run(GAME, 100);
  fake.focused_app_id = 100;
  tick_now();
  pass(30);
  fake.power_state = 500;
  tick_now();
  T_EQ_INT(playtime_session_count(), 1); /* closed at the last sample */
  fake.reads = fake.sony_calls = 0;
  pass(300);
  T_EQ_INT(fake.reads, 0);
  T_EQ_INT(fake.sony_calls, 0);
  T_EQ_INT(playtime_total_s(GAME), 30);
  fake.power_state = 1000;
  tick_now();
  pass(30);
  T_EQ_INT(playtime_total_s(GAME), 60);
}

static void test_persist_and_reload(void) {
  setup();
  run(GAME, 100);
  fake.focused_app_id = 100;
  tick_now();
  pass(60);
  run(MEDIA, 101);
  fake.focused_app_id = 101;
  pass(5);
  pass(15);
  run(NULL, 0);
  pass(5);
  playtime_flush();
  T_ASSERT(t_read(path) != NULL);
  {
    struct stat st;
    T_ASSERT(!stat(path, &st));
    T_EQ_INT(st.st_mode & 0777, 0600);
  }
  playtime_load(); /* forgets memory, reads the file */
  T_EQ_INT(playtime_total_s(GAME), 60);
  T_EQ_INT(playtime_total_s(MEDIA), 15);
  T_EQ_INT(playtime_session_count(), 2);
  json_of(js, sizeof(js), 0);
  T_ASSERT(strstr(js, "\"title_id\":\"" MEDIA "\",\"name\":\"" MEDIA "\",\"kind\":\"app\",\"total_s\":15"));
  T_ASSERT(strstr(js, "\"name\":\"Test Game\",\"kind\":\"game\",\"total_s\":60"));
}

static void test_checkpoint_every_60s(void) {
  struct stat st;
  setup();
  run(GAME, 100);
  fake.focused_app_id = 100;
  tick_now();
  pass(PS5H_PLAYTIME_SAVE_S - PS5H_PLAYTIME_POLL_S);
  T_ASSERT(stat(path, &st)); /* not yet */
  pass(PS5H_PLAYTIME_POLL_S);
  T_ASSERT(!stat(path, &st));
  T_ASSERT(strstr(t_read(path), "\"" GAME "\""));
  T_ASSERT(strstr(t_read(path), "\"total_s\":60"));
  /* a crash now: the running session comes back as a finished one */
  playtime_load();
  T_EQ_INT(playtime_session_count(), 1);
  T_EQ_INT(playtime_total_s(GAME), 60);
}

static void test_retention_folds_old_sessions(void) {
  char file[512];
  int64_t now;
  setup();
  now = fake.wall_s;
  snprintf(file, sizeof(file),
           "{\"schema\":1,\"titles\":{\"" GAME "\":{\"kind\":\"game\",\"total_s\":4000}},"
           "\"sessions\":[[\"" GAME "\",\"game\",%lld,1000],[\"" GAME "\",\"game\",%lld,3000]]}",
           (long long)(now - 401 * DAY), (long long)(now - 2 * DAY));
  T_ASSERT(!t_write_str(path, file));
  playtime_load();
  T_EQ_INT(playtime_session_count(), 1);
  T_EQ_INT(playtime_total_s(GAME), 4000); /* the old one lives on in the total */
  json_of(js, sizeof(js), 0);
  T_ASSERT(!strstr(js, "\"duration_s\":1000"));
  T_ASSERT(strstr(js, "\"duration_s\":3000"));
  /* the since filter: the 2-day-old session ended before it */
  json_of(js, sizeof(js), now - DAY);
  T_ASSERT(!strstr(js, "\"duration_s\":3000"));
}

static void test_max_sessions_drops_oldest(void) {
  static char file[2 << 20];
  size_t n = 0;
  int64_t base;
  setup();
  base = fake.wall_s - 10 * DAY;
  n += (size_t)snprintf(file + n, sizeof(file) - n,
                        "{\"schema\":1,\"titles\":{\"" GAME "\":{\"kind\":\"game\",\"total_s\":%d}},\"sessions\":[",
                        PS5H_PLAYTIME_MAX_SESSIONS * 7);
  for (int i = 0; i < PS5H_PLAYTIME_MAX_SESSIONS; i++) {
    n += (size_t)snprintf(file + n, sizeof(file) - n, "%s[\"" GAME "\",\"game\",%lld,7]",
                          i ? "," : "", (long long)(base + i));
  }
  n += (size_t)snprintf(file + n, sizeof(file) - n, "]}");
  T_ASSERT(!t_write(path, file, n));
  playtime_load();
  T_EQ_INT(playtime_session_count(), PS5H_PLAYTIME_MAX_SESSIONS);
  run(GAME2, 100);
  fake.focused_app_id = 100;
  tick_now();
  pass(10);
  run(NULL, 0);
  pass(5);
  T_EQ_INT(playtime_session_count(), PS5H_PLAYTIME_MAX_SESSIONS);
  T_EQ_INT(playtime_total_s(GAME2), 10);
  T_EQ_INT(playtime_total_s(GAME), PS5H_PLAYTIME_MAX_SESSIONS * 7); /* totals stay */
  /* the oldest went: the first start (base) is gone, base + 1 is the oldest */
  {
    static char big[8 << 20];
    jw_t w;
    char needle[64];
    jw_init(&w);
    jw_obj(&w);
    playtime_json(&w, &list, 0);
    jw_end_obj(&w);
    snprintf(big, sizeof(big), "%s", w.buf);
    jw_free(&w);
    snprintf(needle, sizeof(needle), "\"start\":%lld,", (long long)base);
    T_ASSERT(!strstr(big, needle));
    snprintf(needle, sizeof(needle), "\"start\":%lld,", (long long)(base + 1));
    T_ASSERT(strstr(big, needle));
  }
}

static void test_corrupt_file_set_aside(void) {
  setup();
  T_ASSERT(!t_write_str(path, "{\"schema\":1,\"titles\":{\"PPSA"));
  T_EQ_INT(playtime_load(), 0); /* never fails the start */
  T_EQ_INT(count_bad(), 1);
  T_ASSERT(t_read(path) == NULL);
  T_ASSERT(log_last_error() && strstr(log_last_error(), "playtime"));
  T_EQ_INT(playtime_session_count(), 0);
  /* counting works from scratch */
  run(GAME, 100);
  fake.focused_app_id = 100;
  tick_now();
  pass(10);
  T_EQ_INT(playtime_total_s(GAME), 10);
}

static void test_huge_file_set_aside(void) {
  static char big[PS5H_PLAYTIME_MAX_FILE + 1024];
  setup();
  memset(big, ' ', sizeof(big));
  memcpy(big, "{\"schema\":1}", 12);
  T_ASSERT(!t_write(path, big, sizeof(big)));
  T_EQ_INT(playtime_load(), 0);
  T_EQ_INT(count_bad(), 1);
  T_EQ_INT(playtime_session_count(), 0);
}

static void test_wrong_schema_and_bad_entries(void) {
  setup();
  T_ASSERT(!t_write_str(path, "{\"schema\":2,\"titles\":{},\"sessions\":[]}"));
  playtime_load();
  T_EQ_INT(count_bad(), 1);
  /* bad entries are dropped one by one, the rest stays */
  T_ASSERT(!t_write_str(path,
      "{\"schema\":1,\"titles\":{\"" GAME "\":{\"kind\":\"game\",\"total_s\":9},"
      "\"junk\":{\"kind\":\"game\",\"total_s\":5},\"" MEDIA "\":{\"kind\":\"x\",\"total_s\":-4}},"
      "\"sessions\":[[\"" GAME "\",\"game\",1790000000,9],[\"" GAME "\",\"game\",1790000000,-1],"
      "[\"nope\",\"game\",1790000000,5],[\"" GAME "\",\"toy\",1790000000,5],5,[\"" GAME "\"]]}"));
  playtime_load();
  T_EQ_INT(playtime_session_count(), 1);
  T_EQ_INT(playtime_total_s(GAME), 9);
  T_EQ_INT(playtime_total_s(MEDIA), -1);
  T_EQ_INT(playtime_total_s("junk"), -1);
}

static void test_no_focus_capability_counts_open_game(void) {
  setup();
  fake.caps.focus = false;
  run(GAME, 100);
  fake.focused_app_id = SHELL_ID; /* would not count with focus */
  tick_now();
  pass(60);
  T_EQ_INT(playtime_total_s(GAME), 60);
  T_EQ_INT(fake.focused_app_calls, 0);
}

static void test_unknown_title_is_an_app(void) {
  setup();
  run("PPSA77777", 100);
  fake.focused_app_id = 100;
  tick_now();
  pass(10);
  run(NULL, 0);
  pass(5);
  json_of(js, sizeof(js), 0);
  T_ASSERT(strstr(js, "\"title_id\":\"PPSA77777\",\"name\":\"PPSA77777\",\"kind\":\"app\",\"total_s\":10"));
}

static void test_samples_every_5s_only(void) {
  setup();
  run(GAME, 100);
  fake.focused_app_id = 100;
  tick_now();
  T_EQ_INT(fake.focused_app_calls, 1);
  for (int i = 0; i < 10; i++) {
    fake.mono_ms += 400; /* the loop runs more often than that */
    tick_now();
  }
  T_EQ_INT(fake.focused_app_calls, 1); /* 4 s: not yet */
  fake.mono_ms += 1000;
  fake.wall_s += 5;
  tick_now();
  T_EQ_INT(fake.focused_app_calls, 2);
}

static void test_read_error_closes_session(void) {
  setup();
  run(GAME, 100);
  fake.focused_app_id = 100;
  tick_now();
  pass(10);
  fake.focused_app_rc = -1; /* closes at the last good sample, no bridging */
  pass(10);
  T_EQ_INT(playtime_session_count(), 1);
  T_EQ_INT(playtime_total_s(GAME), 10);
  fake.focused_app_rc = 0;
  pass(5); /* a new session opens */
  pass(10);
  T_EQ_INT(playtime_total_s(GAME), 20);
  T_EQ_INT(playtime_session_count(), 1);
}

/* The focus call keeps failing: after PS5H_PLAYTIME_FOCUS_FAILS failures in a
 * row "game open" counts and the answer says foreground false, until a focus
 * read works again. */
static void test_focus_failing_falls_back_to_game_open(void) {
  setup();
  run(GAME, 100);
  fake.focused_app_id = 100;
  tick_now();
  pass(10);
  T_ASSERT(playtime_foreground());
  fake.focused_app_rc = -1;
  pass((PS5H_PLAYTIME_FOCUS_FAILS - 1) * PS5H_PLAYTIME_POLL_S);
  T_EQ_INT(playtime_total_s(GAME), 10); /* closed, not bridged */
  T_ASSERT(playtime_foreground());
  pass(PS5H_PLAYTIME_POLL_S); /* the last failure in a row: "game open" */
  T_ASSERT(!playtime_foreground());
  pass(30);
  T_EQ_INT(playtime_total_s(GAME), 40);
  T_ASSERT(!playtime_foreground());
  /* focus read works again: the home screen is in front, nothing counts */
  fake.focused_app_rc = 0;
  fake.focused_app_id = SHELL_ID;
  pass(5);
  T_ASSERT(playtime_foreground());
  pass(30);
  T_EQ_INT(playtime_total_s(GAME), 40);
}

static void test_focus_without_capability_is_not_foreground(void) {
  setup();
  fake.caps.focus = false;
  T_ASSERT(!playtime_foreground());
}

/* A console call that blocks (Sony calls do, for ~35 s after waking): both
 * clocks run on during it. The wall/monotonic check must not take that for a
 * clock jump. */
static void test_blocking_call_does_not_split_session(void) {
  setup();
  run(GAME, 100);
  fake.focused_app_id = 100;
  tick_now();
  pass(10);
  fake.focus_block_s = 10;
  pass(5); /* this sample took 10 s inside the console call */
  fake.focus_block_s = 0;
  pass(5);
  T_EQ_INT(playtime_session_count(), 0); /* one session, not split */
  T_EQ_INT(playtime_total_s(GAME), 10 + 15 + 5);
}

static void test_block_right_after_turning_on_loses_no_session(void) {
  setup();
  run(GAME, 100);
  fake.focused_app_id = 100;
  tick_now();
  pass(10);
  fake.power_state = 500;
  pass(10);
  T_EQ_INT(playtime_session_count(), 1);
  fake.power_state = 1000;
  fake.running_block_s = 35; /* the first call after waking */
  power_tick(fake.mono_ms);
  playtime_tick(&list, fake.mono_ms);
  fake.running_block_s = 0;
  pass(10);
  /* the new session starts after the block: 10 s, no 0 s session dropped */
  T_EQ_INT(playtime_session_count(), 1);
  T_EQ_INT(playtime_total_s(GAME), 10 + 10);
}

static void test_kind_never_flips_from_game_to_app(void) {
  setup();
  run(GAME, 100);
  fake.focused_app_id = 100;
  tick_now();
  pass(10);
  run(NULL, 0);
  pass(5);
  items[0].game = false; /* the list no longer says so (rescan, odd data) */
  run(GAME, 101);
  fake.focused_app_id = 101;
  pass(5);
  pass(10);
  json_of(js, sizeof(js), 0);
  T_ASSERT(strstr(js, "\"title_id\":\"" GAME "\",\"name\":\"Test Game\",\"kind\":\"game\",\"total_s\":20"));
  T_ASSERT(strstr(js, "\"current\":{\"title_id\":\"" GAME "\",\"kind\":\"game\""));
  playtime_flush();
  T_ASSERT(strstr(t_read(path), "\"" GAME "\":{\"kind\":\"game\""));
  run(NULL, 0);
  pass(5);
  json_of(js, sizeof(js), 0);
  T_ASSERT(!strstr(js, "\"kind\":\"app\""));
}

static void test_second_bad_file_keeps_the_first(void) {
  setup();
  T_ASSERT(!t_write_str(path, "{\"schema\":1,\"titles\":{\"PPSA"));
  playtime_load();
  T_EQ_INT(count_bad(), 1);
  T_ASSERT(!t_write_str(path, "not json at all"));
  playtime_load(); /* same second: still a new name */
  T_EQ_INT(count_bad(), 2);
  fake.wall_s += 100;
  T_ASSERT(!t_write_str(path, "[1]"));
  playtime_load();
  T_EQ_INT(count_bad(), 3);
}

static const char GOOD_FILE_FMT[] =
    "{\"schema\":1,\"titles\":{\"" GAME "\":{\"kind\":\"game\",\"total_s\":50}},"
    "\"sessions\":[[\"" GAME "\",\"game\",%lld,50]]}";

/* The file is fine but could not be read or allocated: the history must
 * survive, nothing is written until a load worked, then old and new merge. */
static void check_history_survives(char *good, void (*heal)(void)) {
  int64_t counted;
  T_EQ_INT(count_bad(), 0);
  {
    struct stat sb;
    T_ASSERT(!stat(path, &sb)); /* still where it was */
  }
  T_ASSERT(log_last_error() && strstr(log_last_error(), "could not load"));
  run(GAME2, 100);
  fake.focused_app_id = 100;
  tick_now();
  pass(PS5H_PLAYTIME_SAVE_S + 30); /* several checkpoint attempts */
  run(NULL, 0);
  pass(5);
  counted = playtime_total_s(GAME2);
  T_ASSERT(counted >= PS5H_PLAYTIME_SAVE_S);
  pass(PS5H_PLAYTIME_SAVE_S + 5); /* still blocked: file untouched */
  heal();
  T_EQ_STR(t_read(path), good);
  pass(PS5H_PLAYTIME_SAVE_S + 5); /* this checkpoint recovers and merges */
  T_EQ_INT(count_bad(), 0);
  T_EQ_INT(playtime_total_s(GAME), 50);
  T_EQ_INT(playtime_total_s(GAME2), counted);
  T_EQ_INT(playtime_session_count(), 2);
  playtime_load(); /* what is on disk now has both */
  T_EQ_INT(playtime_total_s(GAME), 50);
  T_EQ_INT(playtime_total_s(GAME2), counted);
}

static void heal_alloc(void) { playtime_test_fail_alloc(false); }

static void test_alloc_failure_keeps_file_and_merges_later(void) {
  static char good[400];
  setup();
  snprintf(good, sizeof(good), GOOD_FILE_FMT, (long long)(fake.wall_s - 1000));
  T_ASSERT(!t_write_str(path, good));
  playtime_test_fail_alloc(true);
  T_EQ_INT(playtime_load(), 0);
  check_history_survives(good, heal_alloc);
}

static void heal_perm(void) { chmod(path, 0600); }

static void test_unreadable_file_keeps_file_and_merges_later(void) {
  static char good[400];
  setup();
  if (geteuid() == 0) {
    return; /* root reads anything */
  }
  snprintf(good, sizeof(good), GOOD_FILE_FMT, (long long)(fake.wall_s - 1000));
  T_ASSERT(!t_write_str(path, good));
  T_ASSERT(!chmod(path, 0));
  T_EQ_INT(playtime_load(), 0);
  check_history_survives(good, heal_perm);
}

/* console_total_s is Sony's own count, or null where it is unknown. */
static void test_console_total_in_json(void) {
  char db_path[700], dir[700];
  sqlite3 *db = NULL;
  setup();
  run(GAME, 100);
  fake.focused_app_id = 100;
  tick_now();
  pass(60);
  run(GAME2, 101);
  fake.focused_app_id = 101;
  pass(10);
  json_of(js, sizeof(js), 0);
  T_ASSERT(strstr(js, "\"console_total_s\":null"));
  T_ASSERT(!strstr(js, "\"console_total_s\":1"));
  snprintf(dir, sizeof(dir), "%s/system_data/priv/mms", fake.root);
  snprintf(db_path, sizeof(db_path), "%s/app.db", dir);
  t_mkdirs(dir);
  T_EQ_INT(sqlite3_open(db_path, &db), 0);
  T_EQ_INT(sqlite3_exec(db,
                        "CREATE TABLE tbl_iconinfo_1 (titleId TEXT, playedTimeOnConsole INT);"
                        "INSERT INTO tbl_iconinfo_1 VALUES ('" GAME "', 7200);",
                        0, 0, NULL),
           0);
  sqlite3_close(db);
  T_EQ_INT(sonytime_refresh(fake.mono_ms), 0);
  json_of(js, sizeof(js), 0);
  T_ASSERT(strstr(js, "\"title_id\":\"" GAME "\""));
  T_ASSERT(strstr(js, "\"console_total_s\":7200"));
  T_ASSERT(strstr(js, "\"console_total_s\":null")); /* GAME2: not in the file */
}

int main(void) {
  T_RUN(test_counts_only_foreground);
  T_RUN(test_control_centre_keeps_focus_and_counts);
  T_RUN(test_focus_failing_falls_back_to_game_open);
  T_RUN(test_focus_without_capability_is_not_foreground);
  T_RUN(test_switch_title_closes_session);
  T_RUN(test_stop_closes_session);
  T_RUN(test_gap_closes_session_at_last_sample);
  T_RUN(test_wall_jump_back_closes_session);
  T_RUN(test_wall_jump_forward_closes_session);
  T_RUN(test_not_on_pauses);
  T_RUN(test_persist_and_reload);
  T_RUN(test_checkpoint_every_60s);
  T_RUN(test_retention_folds_old_sessions);
  T_RUN(test_max_sessions_drops_oldest);
  T_RUN(test_corrupt_file_set_aside);
  T_RUN(test_huge_file_set_aside);
  T_RUN(test_wrong_schema_and_bad_entries);
  T_RUN(test_no_focus_capability_counts_open_game);
  T_RUN(test_unknown_title_is_an_app);
  T_RUN(test_samples_every_5s_only);
  T_RUN(test_read_error_closes_session);
  T_RUN(test_blocking_call_does_not_split_session);
  T_RUN(test_block_right_after_turning_on_loses_no_session);
  T_RUN(test_kind_never_flips_from_game_to_app);
  T_RUN(test_second_bad_file_keeps_the_first);
  T_RUN(test_alloc_failure_keeps_file_and_merges_later);
  T_RUN(test_unreadable_file_keeps_file_and_merges_later);
  T_RUN(test_console_total_in_json);
  T_MAIN();
}
