/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

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

int main(void) {
  T_RUN(test_filter_drops_sony_and_dupes);
  T_RUN(test_filter_keeps_truncated_19);
  T_RUN(test_filter_respects_max);
  T_RUN(test_payloads_from_platform);
  T_MAIN();
}
