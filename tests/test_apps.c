/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include <sys/time.h>

#include "apps.h"
#include "json.h"
#include "limits.h"
#include "platform_fake.h"

static char root[512];

/* Copy of tests/data/apps plus the files that cannot live in git. */
static void setup(void) {
  char cmd[1200], p[800];
  static char big[2 * 1024 * 1024];
  fake_reset();
  snprintf(root, sizeof(root), "%s", t_tmpdir());
  snprintf(cmd, sizeof(cmd), "cp -R tests/data/apps/. '%s'", root);
  if (system(cmd)) {
    exit(2);
  }
  snprintf(fake.root, sizeof(fake.root), "%s", root);

  /* TEST00004: param.json is a symlink */
  snprintf(p, sizeof(p), "%s/elsewhere.json", root);
  t_write_str(p, "{\"localizedParameters\":{\"defaultLanguage\":\"en-US\","
                 "\"en-US\":{\"titleName\":\"Via Symlink\"}}}");
  snprintf(p, sizeof(p), "%s/user/app/TEST00004/sce_sys", root);
  t_mkdirs(p);
  snprintf(p, sizeof(p), "%s/user/app/TEST00004/sce_sys/param.json", root);
  if (symlink("../../../../elsewhere.json", p)) {
    exit(2);
  }
  /* TEST00005: 70 000 byte param.json */
  memset(big, ' ', sizeof(big));
  memcpy(big, "{\"localizedParameters\":{\"defaultLanguage\":\"en-US\","
              "\"en-US\":{\"titleName\":\"Too Big\"}}}", 70);
  snprintf(p, sizeof(p), "%s/user/app/TEST00005/sce_sys/param.json", root);
  t_write(p, big, 70000);
  /* TEST00008: 2 MB icon with PNG signature */
  memcpy(big, "\x89PNG\r\n\x1a\n", 8);
  snprintf(p, sizeof(p), "%s/user/app/TEST00008/sce_sys/icon0.png", root);
  t_write(p, big, sizeof(big));
  snprintf(p, sizeof(p), "%s/user/app/TEST00008/sce_sys/param.json", root);
  t_write_str(p, "{\"localizedParameters\":{\"defaultLanguage\":\"en-US\","
                 "\"en-US\":{\"titleName\":\"Big Icon\"}}}");
}

static app_list list;

static const app_t *scan_find(const char *id) {
  apps_free(&list);
  if (apps_scan(&list)) {
    return NULL;
  }
  return apps_find(&list, id);
}

static void test_valid_id(void) {
  T_ASSERT(apps_valid_id("PPSA96708"));
  T_ASSERT(apps_valid_id("CUSA00001"));
  T_ASSERT(!apps_valid_id("ppsa96708"));
  T_ASSERT(!apps_valid_id("PPSA9670"));
  T_ASSERT(!apps_valid_id("PPSA967080"));
  T_ASSERT(!apps_valid_id("PPS196708"));
  T_ASSERT(!apps_valid_id(""));
}

static void test_scan_union_and_order(void) {
  const char *want[] = {"CUSA00010", "PLDM00001", "PPSA00001", "PPSA00002",
                        "PPSA00009", "TEST00004", "TEST00005", "TEST00006",
                        "TEST00007", "TEST00008"};
  setup();
  apps_free(&list);
  T_EQ_INT(apps_scan(&list), 0);
  T_EQ_INT(list.count, 10);
  for (int i = 0; i < 10; i++) {
    T_EQ_STR(list.items[i].title_id, want[i]);
  }
  T_EQ_INT(strlen(list.etag), 16);
  apps_free(&list);
}

static void test_scan_skips_npxs_and_non_ids(void) {
  setup();
  T_ASSERT(scan_find("PPSA00001"));
  T_ASSERT(!apps_find(&list, "NPXS00001"));
  T_ASSERT(!apps_find(&list, "notanid"));
}

static void test_appmeta_wins(void) {
  const app_t *a;
  setup();
  T_ASSERT((a = scan_find("PPSA00009")));
  T_EQ_STR(a->name, "From Appmeta");
}

static void test_name_system_language(void) {
  const app_t *a;
  setup();
  fake.language = 4; /* de-DE */
  T_ASSERT((a = scan_find("PPSA00001")));
  T_EQ_STR(a->name, "Testspiel Eins");
}

static void test_name_default_language_en_gb(void) {
  const app_t *a;
  setup();
  fake.language = 2; /* fr-FR: not present, defaultLanguage en-GB */
  T_ASSERT((a = scan_find("PPSA00001")));
  T_EQ_STR(a->name, "Test Game One");
  fake.language = -1;
  T_ASSERT((a = scan_find("PPSA00001")));
  T_EQ_STR(a->name, "Test Game One");
}

static void test_name_fallback_title_id(void) {
  const app_t *a;
  setup();
  T_ASSERT((a = scan_find("TEST00007")));
  T_EQ_STR(a->name, "TEST00007");
}

static void test_kind_from_drm_type(void) {
  setup();
  T_ASSERT(scan_find("PPSA00001")->game);
  T_ASSERT(!apps_find(&list, "PPSA00002")->game);
  T_ASSERT(!apps_find(&list, "PLDM00001")->game);
  T_ASSERT(apps_find(&list, "PPSA00001")->has_category);
  T_EQ_INT(apps_find(&list, "PPSA00002")->category, 65536);
  T_ASSERT(!apps_find(&list, "TEST00007")->has_category);
}

static void test_version_null_when_missing(void) {
  setup();
  T_EQ_STR(scan_find("PPSA00001")->version, "01.007.000");
  T_EQ_STR(apps_find(&list, "PPSA00002")->version, "");
}

static void test_png_and_jpeg_icons(void) {
  const app_t *a;
  setup();
  T_ASSERT((a = scan_find("PLDM00001")));
  T_EQ_INT(a->icon, ICON_JPEG);
  T_ASSERT(strstr(a->icon_path, "/user/app/PLDM00001/sce_sys/icon0.png"));
  a = apps_find(&list, "PPSA00001");
  T_EQ_INT(a->icon, ICON_PNG);
  T_ASSERT(strstr(a->icon_path, "/user/appmeta/PPSA00001/icon0.png"));
  T_ASSERT(a->icon_size > 0 && a->icon_mtime > 0);
  T_EQ_INT(apps_find(&list, "TEST00007")->icon, ICON_NONE);
}

static void test_symlink_param_ignored(void) {
  const app_t *a;
  setup();
  T_ASSERT((a = scan_find("TEST00004")));
  T_EQ_STR(a->name, "TEST00004");
}

static void test_oversize_param_ignored(void) {
  const app_t *a;
  setup();
  T_ASSERT((a = scan_find("TEST00005")));
  T_EQ_STR(a->name, "TEST00005");
}

static void test_invalid_utf8_name_sanitized(void) {
  const app_t *a;
  jw_t w;
  setup();
  T_ASSERT((a = scan_find("TEST00006")));
  jw_init(&w);
  jw_cstr(&w, a->name);
  T_ASSERT(w.buf && strstr(w.buf, "Caf\xef\xbf\xbd"));
  jw_free(&w);
}

static void test_oversize_icon_has_no_icon(void) {
  const app_t *a;
  setup();
  T_ASSERT((a = scan_find("TEST00008")));
  T_EQ_STR(a->name, "Big Icon");
  T_EQ_INT(a->icon, ICON_NONE);
}

static void test_cusa_without_param_is_game(void) {
  const app_t *a;
  setup();
  T_ASSERT((a = scan_find("CUSA00010")));
  T_ASSERT(a->game);
  T_EQ_STR(a->name, "CUSA00010");
}

static void test_etag_changes_on_mtime(void) {
  char e1[17], e2[17], p[700];
  struct timeval tv[2] = {{1000000000, 0}, {1000000000, 0}};
  setup();
  apps_free(&list);
  T_EQ_INT(apps_scan(&list), 0);
  T_EQ_INT(apps_current_etag(e1), 0);
  T_EQ_STR(e1, list.etag);
  snprintf(p, sizeof(p), "%s/user/app/PPSA00002/sce_sys/param.json", root);
  T_EQ_INT(utimes(p, tv), 0);
  T_EQ_INT(apps_current_etag(e2), 0);
  T_ASSERT(strcmp(e1, e2));
  /* refresh picks the change up */
  T_EQ_INT(apps_refresh(&list), 1);
  T_EQ_STR(list.etag, e2);
  T_EQ_INT(apps_refresh(&list), 0);
  apps_free(&list);
}

static void test_missing_folders_empty_list(void) {
  fake_reset();
  snprintf(fake.root, sizeof(fake.root), "%s", t_tmpdir());
  apps_free(&list);
  T_EQ_INT(apps_scan(&list), 0);
  T_EQ_INT(list.count, 0);
  apps_free(&list);
}

static void test_lang_codes(void) {
  T_EQ_STR(apps_lang_code(0), "ja-JP");
  T_EQ_STR(apps_lang_code(1), "en-US");
  T_EQ_STR(apps_lang_code(4), "de-DE");
  T_EQ_STR(apps_lang_code(15), "nb-NO");
  T_EQ_STR(apps_lang_code(18), "en-GB");
  T_EQ_STR(apps_lang_code(30), "uk-UA");
  T_ASSERT(apps_lang_code(31) == NULL);
  T_ASSERT(apps_lang_code(-1) == NULL);
}

int main(void) {
  T_RUN(test_valid_id);
  T_RUN(test_scan_union_and_order);
  T_RUN(test_scan_skips_npxs_and_non_ids);
  T_RUN(test_appmeta_wins);
  T_RUN(test_name_system_language);
  T_RUN(test_name_default_language_en_gb);
  T_RUN(test_name_fallback_title_id);
  T_RUN(test_kind_from_drm_type);
  T_RUN(test_version_null_when_missing);
  T_RUN(test_png_and_jpeg_icons);
  T_RUN(test_symlink_param_ignored);
  T_RUN(test_oversize_param_ignored);
  T_RUN(test_invalid_utf8_name_sanitized);
  T_RUN(test_oversize_icon_has_no_icon);
  T_RUN(test_cusa_without_param_is_game);
  T_RUN(test_etag_changes_on_mtime);
  T_RUN(test_missing_folders_empty_list);
  T_RUN(test_lang_codes);
  apps_free(&list);
  T_MAIN();
}
