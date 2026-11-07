/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "playtime.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "limits.h"
#include "log.h"
#include "platform.h"
#include "power.h"
#include "sonytime.h"

#define DAY_S 86400
#define MAX_DUR_S (31 * DAY_S)      /* one session longer than this is bogus */
#define MAX_TOTAL_S 10000000000LL   /* about 300 years */
#define MAX_WALL_S 4102444800LL     /* 2100-01-01 */
#define LOAD_NO_MEMORY (-2) /* not a verdict on the file */
/* title: key + object + 2 keys + 2 values; session: array + 4 values */
#define MAX_TOKENS \
  ((PS5H_PLAYTIME_MAX_SESSIONS + 16) * 5 + (PS5H_PLAYTIME_MAX_TITLES + 16) * 6 + 16)

typedef struct {
  char id[10];
  bool game;
  int64_t total_s; /* finished sessions, plus whatever was folded away */
  int64_t sum;     /* scratch while loading */
} title_rec;

typedef struct {
  char id[10];
  bool game;
  int64_t start, dur; /* wall start, seconds */
} sess_rec;

static struct {
  title_rec titles[PS5H_PLAYTIME_MAX_TITLES];
  int n_titles;
  sess_rec *sess; /* oldest first */
  int n_sess, cap_sess;
  struct {
    bool active;
    char id[10];
    int app_id;
    bool game;
    int64_t start_wall, dur_ms;
    int64_t last_mono_ms, last_wall_s; /* previous sample */
  } cur;
  bool dirty;
  int64_t dirty_ms; /* when it became dirty: the checkpoint timer */
  bool have_poll;
  int64_t last_poll_ms;
  /* The file exists but could not be read: nothing is written until a load
   * worked, or the history in it would be overwritten. */
  bool blocked;
  int focus_fails; /* focus call failures in a row */
} st;

static bool test_fail_alloc;

/* Titles and finished sessions only; the running session stays. */
static void reset_data(void) {
  free(st.sess);
  st.sess = NULL;
  st.n_sess = st.cap_sess = 0;
  st.n_titles = 0;
}

static void reset(void) {
  free(st.sess);
  memset(&st, 0, sizeof(st));
}

static const char *kind_name(bool game) { return game ? "game" : "app"; }

static void mark_dirty(int64_t now_ms) {
  if (!st.dirty) {
    st.dirty = true;
    st.dirty_ms = now_ms;
  }
}

/* --- titles and sessions ----------------------------------------------- */

static title_rec *find_title(const char *id) {
  for (int i = 0; i < st.n_titles; i++) {
    if (!strcmp(st.titles[i].id, id)) {
      return &st.titles[i];
    }
  }
  return NULL;
}

/* Kind of a title for display and saving: the running session may know it
 * better than the stored record. */
static bool title_is_game(const title_rec *t) {
  return t->game || (st.cur.active && !strcmp(st.cur.id, t->id) && st.cur.game);
}

static void drop_sessions_of(const char *id) {
  int k = 0;
  for (int i = 0; i < st.n_sess; i++) {
    if (strcmp(st.sess[i].id, id)) {
      st.sess[k++] = st.sess[i];
    }
  }
  st.n_sess = k;
}

/* The title, created when new; at the limit the one with the smallest total
 * goes (with its sessions). NULL only for an id we do not accept. */
static title_rec *get_title(const char *id, bool game) {
  title_rec *t = find_title(id);
  if (!t) {
    if (st.n_titles >= PS5H_PLAYTIME_MAX_TITLES) {
      int low = 0;
      for (int i = 1; i < st.n_titles; i++) {
        if (st.titles[i].total_s < st.titles[low].total_s) {
          low = i;
        }
      }
      drop_sessions_of(st.titles[low].id);
      st.titles[low] = st.titles[--st.n_titles];
    }
    t = &st.titles[st.n_titles++];
    memset(t, 0, sizeof(*t));
    snprintf(t->id, sizeof(t->id), "%s", id);
  }
  t->game = t->game || game; /* once a game, always a game */
  return t;
}

static bool push_session(const char *id, bool game, int64_t start, int64_t dur) {
  sess_rec *s;
  if (st.n_sess >= PS5H_PLAYTIME_MAX_SESSIONS) {
    /* the oldest goes; its time stays in the total */
    memmove(st.sess, st.sess + 1,
            (size_t)(st.n_sess - 1) * sizeof(st.sess[0]));
    st.n_sess--;
  }
  if (st.n_sess >= st.cap_sess) {
    int cap = st.cap_sess ? st.cap_sess * 2 : 64;
    sess_rec *n;
    if (cap > PS5H_PLAYTIME_MAX_SESSIONS) {
      cap = PS5H_PLAYTIME_MAX_SESSIONS;
    }
    if (!(n = realloc(st.sess, (size_t)cap * sizeof(*n)))) {
      return false;
    }
    st.sess = n;
    st.cap_sess = cap;
  }
  s = &st.sess[st.n_sess++];
  snprintf(s->id, sizeof(s->id), "%s", id);
  s->game = game;
  s->start = start;
  s->dur = dur;
  return true;
}

/* Sessions older than the retention are folded: only the total keeps them. */
static void prune(int64_t now_wall_s) {
  int64_t limit = now_wall_s - (int64_t)PS5H_PLAYTIME_KEEP_DAYS * DAY_S;
  int k = 0;
  for (int i = 0; i < st.n_sess; i++) {
    if (st.sess[i].start + st.sess[i].dur >= limit) {
      st.sess[k++] = st.sess[i];
    }
  }
  st.n_sess = k;
}

static int64_t cur_dur_s(void) { return st.cur.dur_ms / 1000; }

static void close_cur(int64_t now_ms) {
  int64_t dur;
  if (!st.cur.active) {
    return;
  }
  dur = cur_dur_s();
  st.cur.active = false;
  mark_dirty(now_ms);
  if (dur <= 0) {
    return;
  }
  {
    title_rec *t = get_title(st.cur.id, st.cur.game);
    if (t->total_s + dur <= MAX_TOTAL_S) {
      t->total_s += dur;
    }
  }
  if (!push_session(st.cur.id, st.cur.game, st.cur.start_wall, dur)) {
    log_error("playtime: out of memory, a session was lost");
  }
}

/* --- file ---------------------------------------------------------------- */

static bool valid_total(int64_t v) { return v >= 0 && v <= MAX_TOTAL_S; }

static void load_titles(const jr_t *r, int t) {
  if (t < 0 || r->tok[t].type != JSMN_OBJECT) {
    return;
  }
  for (int i = t + 1; i < r->ntok && r->tok[i].start < r->tok[t].end; i++) {
    char id[16], kind[8];
    int64_t total;
    int v;
    title_rec *rec;
    if (r->tok[i].parent != t || r->tok[i].type != JSMN_STRING) {
      continue;
    }
    v = i + 1;
    if (v >= r->ntok || r->tok[v].type != JSMN_OBJECT ||
        !jr_string(r, i, id, sizeof(id)) || !apps_valid_id(id) ||
        !jr_string(r, jr_get(r, v, "kind"), kind, sizeof(kind)) ||
        (strcmp(kind, "game") && strcmp(kind, "app")) ||
        !jr_int(r, jr_get(r, v, "total_s"), &total) || !valid_total(total)) {
      continue;
    }
    if (find_title(id) || st.n_titles >= PS5H_PLAYTIME_MAX_TITLES) {
      continue;
    }
    rec = &st.titles[st.n_titles++];
    memset(rec, 0, sizeof(*rec));
    snprintf(rec->id, sizeof(rec->id), "%s", id);
    rec->game = !strcmp(kind, "game");
    rec->total_s = total;
  }
}

/* false = out of memory half way (the history would be incomplete). */
static bool load_sessions(const jr_t *r, int t) {
  if (t < 0 || r->tok[t].type != JSMN_ARRAY) {
    return true;
  }
  for (int e = t + 1; e < r->ntok && r->tok[e].start < r->tok[t].end; e++) {
    char id[16], kind[8];
    int64_t start, dur;
    title_rec *rec;
    bool game;
    if (r->tok[e].parent != t) {
      continue;
    }
    if (r->tok[e].type != JSMN_ARRAY || r->tok[e].size != 4 ||
        e + 4 >= r->ntok || r->tok[e + 1].parent != e ||
        r->tok[e + 2].parent != e || r->tok[e + 3].parent != e ||
        r->tok[e + 4].parent != e || !jr_string(r, e + 1, id, sizeof(id)) ||
        !apps_valid_id(id) || !jr_string(r, e + 2, kind, sizeof(kind)) ||
        (strcmp(kind, "game") && strcmp(kind, "app")) ||
        !jr_int(r, e + 3, &start) || start < 0 || start > MAX_WALL_S ||
        !jr_int(r, e + 4, &dur) || dur < 0 || dur > MAX_DUR_S) {
      continue;
    }
    game = !strcmp(kind, "game");
    if (!(rec = find_title(id))) {
      if (st.n_titles >= PS5H_PLAYTIME_MAX_TITLES) {
        continue;
      }
      rec = get_title(id, game);
    }
    rec->sum += dur;
    if (!push_session(id, game, start, dur)) {
      return false;
    }
  }
  return true;
}

int playtime_load_buf(const char *buf, size_t len, int64_t now_wall_s) {
  jsmntok_t *tok;
  jr_t r;
  int64_t v;
  reset_data();
  if (test_fail_alloc || !(tok = malloc(sizeof(*tok) * MAX_TOKENS))) {
    return LOAD_NO_MEMORY;
  }
  if (jr_parse(&r, buf, len, tok, MAX_TOKENS) || tok[0].type != JSMN_OBJECT ||
      !jr_int(&r, jr_get(&r, 0, "schema"), &v) || v != 1) {
    free(tok);
    return -1;
  }
  load_titles(&r, jr_get(&r, 0, "titles"));
  if (!load_sessions(&r, jr_get(&r, 0, "sessions"))) {
    free(tok);
    reset_data();
    return LOAD_NO_MEMORY;
  }
  free(tok);
  /* a total that is lower than its own sessions is repaired */
  for (int i = 0; i < st.n_titles; i++) {
    if (st.titles[i].total_s < st.titles[i].sum) {
      st.titles[i].total_s = st.titles[i].sum;
    }
    st.titles[i].sum = 0;
  }
  prune(now_wall_s);
  return 0;
}

static int data_path(char *out, size_t cap) {
  char dir[600];
  if (config_data_dir(dir, sizeof(dir))) {
    return -1;
  }
  return (size_t)snprintf(out, cap, "%s/playtime.json", dir) >= cap ? -1 : 0;
}

/* A file with a real format problem goes to playtime.json.bad-<time> (a name
 * of its own, so a second event cannot destroy the first). */
static void set_aside(const char *path, const char *why) {
  char bad[780];
  long long t = (long long)plat_wall_s();
  reset_data();
  for (int n = 0; n < 100; n++) {
    if (n) {
      snprintf(bad, sizeof(bad), "%s.bad-%lld-%d", path, t, n);
    } else {
      snprintf(bad, sizeof(bad), "%s.bad-%lld", path, t);
    }
    if (access(bad, F_OK)) {
      break;
    }
  }
  if (rename(path, bad)) {
    log_error("playtime: %s, and it could not be moved away", why);
    return;
  }
  log_error("playtime: %s; moved aside (playtime.json.bad-*), counting starts again", why);
}

/* Data replaced by the file's. 0 = loaded (or no file), 1 = the file was not
 * usable and has been set aside, -1 = it could not be read or allocated: the
 * file is untouched and says nothing about its content. */
static int load_file(const char *path) {
  char *buf;
  size_t len;
  int rc;
  reset_data();
  if (!(buf = config_read_small(path, PS5H_PLAYTIME_MAX_FILE, &len))) {
    int e = errno;
    if (e == ENOENT) {
      return 0;
    }
    if (e == EINVAL || e == ELOOP) { /* too large, not a regular file */
      set_aside(path, "file is too large or not a regular file");
      return 1;
    }
    log_error("playtime: could not load the file (errno %d), it is kept", e);
    return -1;
  }
  rc = playtime_load_buf(buf, len, plat_wall_s());
  free(buf);
  if (rc == LOAD_NO_MEMORY) {
    log_error("playtime: could not load the file (out of memory), it is kept");
    return -1;
  }
  if (rc) {
    set_aside(path, "file is not valid");
    return 1;
  }
  return 0;
}

int playtime_load(void) {
  char path[700];
  reset();
  if (data_path(path, sizeof(path))) {
    return -1;
  }
  /* on -1 nothing is written until a later load worked (try_recover) */
  st.blocked = load_file(path) < 0;
  return 0;
}

/* After a failed load: read the file again; when that works, what was
 * counted since then is added to what the file holds. */
static bool try_recover(void) {
  char path[700];
  title_rec *mt;
  sess_rec *ms = st.sess;
  int mn = st.n_titles, mns = st.n_sess, mcap = st.cap_sess;

  if (data_path(path, sizeof(path))) {
    return false;
  }
  if (!(mt = malloc((size_t)(mn ? mn : 1) * sizeof(*mt)))) {
    return false;
  }
  memcpy(mt, st.titles, (size_t)mn * sizeof(*mt));
  st.sess = NULL; /* detached: load_file replaces the data */
  st.n_sess = st.cap_sess = 0;
  if (load_file(path) < 0) {
    free(st.sess);
    memcpy(st.titles, mt, (size_t)mn * sizeof(*mt));
    st.n_titles = mn;
    st.sess = ms;
    st.n_sess = mns;
    st.cap_sess = mcap;
    free(mt);
    return false;
  }
  for (int i = 0; i < mn; i++) {
    title_rec *t = get_title(mt[i].id, mt[i].game);
    t->total_s += mt[i].total_s;
    if (t->total_s > MAX_TOTAL_S) {
      t->total_s = MAX_TOTAL_S;
    }
  }
  for (int i = 0; i < mns; i++) {
    push_session(ms[i].id, ms[i].game, ms[i].start, ms[i].dur);
  }
  free(ms);
  free(mt);
  st.blocked = false;
  log_msg("playtime: file read again, history merged");
  return true;
}

static void write_title(jw_t *w, const char *id, bool game, int64_t total) {
  jw_key(w, id);
  jw_obj(w);
  jw_key(w, "kind");
  jw_cstr(w, kind_name(game));
  jw_key(w, "total_s");
  jw_int(w, total);
  jw_end_obj(w);
}

/* The running session is saved as a finished one: after a crash it counts up
 * to the last checkpoint. */
static void save(int64_t now_ms) {
  char path[700];
  jw_t w;
  bool cur_listed = false;
  int64_t cur_s = st.cur.active ? cur_dur_s() : 0;

  if (st.blocked && !try_recover()) {
    st.dirty_ms = now_ms; /* the file is not overwritten; try again later */
    return;
  }
  prune(plat_wall_s());
  if (data_path(path, sizeof(path))) {
    log_error("playtime: data folder not usable");
    st.dirty = false; /* try again after the next change */
    return;
  }
  jw_init(&w);
  jw_obj(&w);
  jw_key(&w, "schema");
  jw_int(&w, 1);
  jw_key(&w, "titles");
  jw_obj(&w);
  for (int i = 0; i < st.n_titles; i++) {
    const title_rec *t = &st.titles[i];
    bool is_cur = cur_s > 0 && !strcmp(t->id, st.cur.id);
    write_title(&w, t->id, title_is_game(t),
                t->total_s + (is_cur ? cur_s : 0));
    cur_listed |= is_cur;
  }
  if (cur_s > 0 && !cur_listed) {
    write_title(&w, st.cur.id, st.cur.game, cur_s);
  }
  jw_end_obj(&w);
  jw_key(&w, "sessions");
  jw_arr(&w);
  for (int i = 0; i < st.n_sess; i++) {
    const sess_rec *s = &st.sess[i];
    jw_arr(&w);
    jw_cstr(&w, s->id);
    jw_cstr(&w, kind_name(s->game));
    jw_int(&w, s->start);
    jw_int(&w, s->dur);
    jw_end_arr(&w);
  }
  if (cur_s > 0) {
    jw_arr(&w);
    jw_cstr(&w, st.cur.id);
    jw_cstr(&w, kind_name(st.cur.game));
    jw_int(&w, st.cur.start_wall);
    jw_int(&w, cur_s);
    jw_end_arr(&w);
  }
  jw_end_arr(&w);
  jw_end_obj(&w);
  if (w.failed || config_write_atomic(path, w.buf, w.len)) {
    log_error("playtime: could not write %s", path);
  }
  jw_free(&w);
  /* success or not: the next try waits for the next change and its timer */
  st.dirty = false;
}

void playtime_flush(void) {
  if (st.dirty) {
    save(plat_mono_ms());
  }
}

static void maybe_save(int64_t now_ms) {
  if (st.dirty && now_ms - st.dirty_ms >= PS5H_PLAYTIME_SAVE_S * 1000) {
    save(now_ms);
  }
}

/* --- counting ------------------------------------------------------------ */

/* now_ms only paces the polling and the checkpoint timer. Console calls can
 * block for a long time (about 35 s after waking), so the clocks for the
 * session are read after them, both at the same instant: a block then never
 * looks like a wall clock jump. */
void playtime_tick(const app_list *apps, int64_t now_ms) {
  plat_running run;
  bool counts = false, game = false;
  int rc;
  int64_t wall, mono, dm;

  if (!power_is_on()) {
    /* rest mode or changing state: no console call; the session ends at the
     * last sample */
    close_cur(now_ms);
    maybe_save(now_ms);
    return;
  }
  if (st.have_poll && now_ms >= st.last_poll_ms &&
      now_ms - st.last_poll_ms < PS5H_PLAYTIME_POLL_S * 1000) {
    maybe_save(now_ms);
    return;
  }
  st.have_poll = true;
  st.last_poll_ms = now_ms;

  if ((rc = plat_running_app(&run)) < 0) {
    /* no reading: the session ends at the last good sample, the unknown time
     * is not bridged */
    close_cur(now_ms);
    maybe_save(now_ms);
    return;
  }
  if (rc == 1 && apps_valid_id(run.title_id)) {
    if (!plat_capabilities().focus) {
      counts = true; /* no way to tell: "game open" counts */
    } else {
      int focus = 0;
      if (plat_focused_app(&focus) < 0) {
        if (st.focus_fails < PS5H_PLAYTIME_FOCUS_FAILS) {
          st.focus_fails++;
        }
        if (st.focus_fails < PS5H_PLAYTIME_FOCUS_FAILS) {
          close_cur(now_ms);
          maybe_save(now_ms);
          return;
        }
        counts = true; /* keeps failing: "game open" counts until it works */
      } else {
        st.focus_fails = 0;
        counts = focus == run.app_id;
      }
    }
  }
  if (counts) {
    const app_t *a = apps ? apps_find(apps, run.title_id) : NULL;
    const title_rec *t = find_title(run.title_id);
    game = (a && a->game) || (t && t->game); /* never back from game to app */
  }
  mono = plat_mono_ms();
  wall = plat_wall_s();

  if (st.cur.active) {
    dm = mono - st.cur.last_mono_ms;
    if (!counts || strcmp(st.cur.id, run.title_id) || st.cur.app_id != run.app_id ||
        dm < 0 || dm > PS5H_PLAYTIME_GAP_S * 1000 ||
        llabs((wall - st.cur.last_wall_s) * 1000 - dm) > PS5H_WAKE_JUMP_S * 1000) {
      /* ends at the previous sample: the time since then is not counted */
      close_cur(now_ms);
    } else {
      st.cur.dur_ms += dm;
      st.cur.last_mono_ms = mono;
      st.cur.last_wall_s = wall;
      st.cur.game = st.cur.game || game; /* the list may have caught up */
      mark_dirty(now_ms);
    }
  }
  if (!st.cur.active && counts) {
    st.cur.active = true;
    snprintf(st.cur.id, sizeof(st.cur.id), "%s", run.title_id);
    st.cur.app_id = run.app_id;
    st.cur.game = game;
    st.cur.start_wall = wall;
    st.cur.dur_ms = 0;
    st.cur.last_mono_ms = mono;
    st.cur.last_wall_s = wall;
    mark_dirty(now_ms);
  }
  maybe_save(now_ms);
}

/* --- answers ------------------------------------------------------------- */

bool playtime_foreground(void) {
  return plat_capabilities().focus &&
         st.focus_fails < PS5H_PLAYTIME_FOCUS_FAILS;
}

static void write_name(jw_t *w, const app_list *apps, const char *id) {
  const app_t *a = apps ? apps_find(apps, id) : NULL;
  jw_key(w, "name");
  jw_cstr(w, a && a->name[0] ? a->name : id);
}

static void write_title_out(jw_t *w, const app_list *apps, const char *id,
                            bool game, int64_t total) {
  jw_obj(w);
  jw_key(w, "title_id");
  jw_cstr(w, id);
  write_name(w, apps, id);
  jw_key(w, "kind");
  jw_cstr(w, kind_name(game));
  jw_key(w, "total_s");
  jw_int(w, total);
  jw_key(w, "console_total_s");
  {
    int64_t console_s = sonytime_of(id); /* Sony's own count, -1 = unknown */
    if (console_s < 0) {
      jw_null(w);
    } else {
      jw_int(w, console_s);
    }
  }
  jw_end_obj(w);
}

static void write_session_out(jw_t *w, const char *id, bool game, int64_t start,
                              int64_t dur) {
  jw_obj(w);
  jw_key(w, "title_id");
  jw_cstr(w, id);
  jw_key(w, "kind");
  jw_cstr(w, kind_name(game));
  jw_key(w, "start");
  jw_int(w, start);
  jw_key(w, "duration_s");
  jw_int(w, dur);
  jw_end_obj(w);
}

void playtime_json(jw_t *w, const app_list *apps, int64_t since_wall_s) {
  bool cur_listed = false;
  int64_t cur_s = st.cur.active ? cur_dur_s() : 0;

  jw_key(w, "titles");
  jw_arr(w);
  for (int i = 0; i < st.n_titles; i++) {
    const title_rec *t = &st.titles[i];
    bool is_cur = st.cur.active && !strcmp(t->id, st.cur.id);
    write_title_out(w, apps, t->id, title_is_game(t),
                    t->total_s + (is_cur ? cur_s : 0));
    cur_listed |= is_cur;
  }
  if (st.cur.active && !cur_listed) {
    write_title_out(w, apps, st.cur.id, st.cur.game, cur_s);
  }
  jw_end_arr(w);
  jw_key(w, "sessions");
  jw_arr(w);
  for (int i = 0; i < st.n_sess; i++) {
    const sess_rec *s = &st.sess[i];
    if (s->start + s->dur >= since_wall_s) {
      write_session_out(w, s->id, s->game, s->start, s->dur);
    }
  }
  jw_end_arr(w);
  jw_key(w, "current");
  if (st.cur.active) {
    write_session_out(w, st.cur.id, st.cur.game, st.cur.start_wall, cur_s);
  } else {
    jw_null(w);
  }
}

int64_t playtime_total_s(const char *title_id) {
  const title_rec *t = find_title(title_id);
  bool is_cur = st.cur.active && !strcmp(st.cur.id, title_id);
  if (!t && !is_cur) {
    return -1;
  }
  return (t ? t->total_s : 0) + (is_cur ? cur_dur_s() : 0);
}

int playtime_session_count(void) { return st.n_sess; }

void playtime_test_fail_alloc(bool fail) { test_fail_alloc = fail; }
