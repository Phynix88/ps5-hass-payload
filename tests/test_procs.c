/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "limits.h"
#include "platform_fake.h"
#include "procs.h"

static void test_filter_drops_sony_and_dupes(void) {
  char in[][20] = {"websrv.elf", "SceSysCore.elf", "kstuff.elf", "fs_cleaner.elf",
                   "AgcCompositor.elf", "orbis_audiod.elf", "mini-syscore.elf",
                   "ScePsNowClientDaemo", "kstuff.elf", "eboot.bin", "ftpsrv.elf"};
  char out[8][20];
  int n = procs_filter(in, 11, out, 8);
  T_EQ_INT(n, 3);
  T_EQ_STR(out[0], "ftpsrv.elf");
  T_EQ_STR(out[1], "kstuff.elf");
  T_EQ_STR(out[2], "websrv.elf");
}

static void test_filter_keeps_truncated_19(void) {
  char in[][20] = {"ps5upload-elfloader", "short", "SceCameraDriverMain"};
  char out[4][20];
  int n = procs_filter(in, 3, out, 4);
  T_EQ_INT(n, 1);
  T_EQ_STR(out[0], "ps5upload-elfloader");
}

static void test_filter_respects_max(void) {
  char in[][20] = {"a.elf", "b.elf", "c.elf"};
  char out[2][20];
  T_EQ_INT(procs_filter(in, 3, out, 2), 2);
}

static void test_payloads_from_platform(void) {
  char out[8][20];
  fake_reset();
  snprintf(fake.procs[0], 20, "kstuff.elf");
  snprintf(fake.procs[1], 20, "SceSysCore.elf");
  fake.n_procs = 2;
  T_EQ_INT(procs_payloads(out, 8), 1);
  T_EQ_STR(out[0], "kstuff.elf");
}

static void test_game_in_memory(void) {
  fake_reset();
  T_ASSERT(!procs_game_in_memory()); /* the default list: Sony's own process */
  snprintf(fake.procs[1], 20, "ftpsrv.elf");
  fake.n_procs = 2;
  T_ASSERT(!procs_game_in_memory()); /* only system processes and payloads */
  snprintf(fake.procs[2], 20, "eboot.bin");
  fake.n_procs = 3;
  T_ASSERT(procs_game_in_memory());
  /* exact and case-sensitive */
  snprintf(fake.procs[2], 20, "eboot.bin2");
  T_ASSERT(!procs_game_in_memory());
  snprintf(fake.procs[2], 20, "Eboot.bin");
  T_ASSERT(!procs_game_in_memory());
  snprintf(fake.procs[2], 20, "eboot");
  T_ASSERT(!procs_game_in_memory());
  fake_reset();
}

/* our own process is always listed: an empty list means the read went wrong */
static void test_empty_process_list_counts_as_game(void) {
  fake_reset();
  fake.n_procs = 0;
  T_ASSERT(procs_game_in_memory());
  fake_reset();
}

static void test_list_suggests_game(void) {
  plat_name l[4];
  memset(l, 0, sizeof(l));
  snprintf(l[0], 20, "mini-syscore.elf");
  snprintf(l[1], 20, "ftpsrv.elf");
  T_ASSERT(!procs_list_suggests_game(l, 2, 512));
  T_ASSERT(procs_list_suggests_game(l, 0, 512)); /* empty */
  T_ASSERT(procs_list_suggests_game(l, -1, 512)); /* unreadable */
  T_ASSERT(procs_list_suggests_game(l, 2, 2)); /* as long as the cap: may be cut */
  T_ASSERT(procs_list_suggests_game(l, 3, 2));
  T_ASSERT(!procs_list_suggests_game(l, 2, 3));
  snprintf(l[2], 20, "eboot.bin");
  T_ASSERT(procs_list_suggests_game(l, 3, 512));
  T_ASSERT(!procs_list_suggests_game(l, 2, 512)); /* only what n covers */
}

static void test_running_app_from_id(void) {
  T_EQ_INT(running_app_from_id(-1), 0); /* nothing runs, also after minimising */
  T_EQ_INT(running_app_from_id(0), -1);
  T_EQ_INT(running_app_from_id(-2), -1);
  T_EQ_INT(running_app_from_id((int)0x80020010u), -1); /* console error */
  T_EQ_INT(running_app_from_id(-2147352560), -1);
  T_EQ_INT(running_app_from_id(24), 1);
  T_EQ_INT(running_app_from_id(1), 1);
}

static void test_game_in_memory_fails_safe(void) {
  fake_reset();
  fake.procs_rc = -1; /* the list cannot be read */
  T_ASSERT(procs_game_in_memory());
  fake_reset();
}

static void test_payloads_empty_on_unreadable_list(void) {
  char out[8][20];
  fake_reset();
  fake.procs_rc = -1;
  T_EQ_INT(procs_payloads(out, 8), 0);
  fake_reset();
}

static int64_t real_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void on_term_flush(int sig) {
  (void)sig;
  usleep(100 * 1000); /* writes its file, then ends */
  _exit(0);
}

/* A child that ends on SIGTERM (mode 0), ends after a short flush (1), or
 * ignores SIGTERM (2). */
static pid_t child(int mode) {
  pid_t pid = fork();
  if (pid == 0) {
    if (mode == 1) {
      signal(SIGTERM, on_term_flush);
    } else if (mode == 2) {
      signal(SIGTERM, SIG_IGN);
    }
    for (;;) {
      pause();
    }
  }
  return pid;
}

static bool gone(pid_t pid) { return kill(pid, 0) != 0 && errno == ESRCH; }

static pid_t kids[3];

static void stop_body(void) {
  int64_t t;
  kids[0] = child(0);
  kids[1] = child(1);
  T_ASSERT(kids[0] > 0 && kids[1] > 0);
  usleep(50 * 1000); /* handlers installed */
  t = real_ms();
  T_EQ_INT(procs_stop(kids, 2, 1500), 0); /* nobody needed SIGKILL */
  T_ASSERT(real_ms() - t < 1000);
  T_ASSERT(gone(kids[0]) && gone(kids[1]));
  kids[2] = child(2);
  T_ASSERT(kids[2] > 0);
  usleep(50 * 1000);
  t = real_ms();
  T_EQ_INT(procs_stop(&kids[2], 1, 300), 1);
  T_ASSERT(real_ms() - t >= 300);
  usleep(50 * 1000);
  T_ASSERT(gone(kids[2]));
  T_EQ_INT(PS5H_STOP_WAIT_MS, 1500);
}

/* An older instance gets SIGTERM first (it saves its play time then); only
 * one that is still there after the wait gets SIGKILL. */
static void test_stop_terms_first_then_kills(void) {
  memset(kids, 0, sizeof(kids));
  signal(SIGCHLD, SIG_IGN); /* children are reaped at once: no zombies */
  stop_body();
  for (int i = 0; i < 3; i++) { /* nothing left behind, even on failure */
    if (kids[i] > 0) {
      kill(kids[i], SIGKILL);
    }
  }
  signal(SIGCHLD, SIG_DFL);
}

int main(void) {
  T_RUN(test_filter_drops_sony_and_dupes);
  T_RUN(test_filter_keeps_truncated_19);
  T_RUN(test_filter_respects_max);
  T_RUN(test_payloads_from_platform);
  T_RUN(test_game_in_memory);
  T_RUN(test_empty_process_list_counts_as_game);
  T_RUN(test_list_suggests_game);
  T_RUN(test_running_app_from_id);
  T_RUN(test_game_in_memory_fails_safe);
  T_RUN(test_payloads_empty_on_unreadable_list);
  T_RUN(test_stop_terms_first_then_kills);
  T_MAIN();
}
