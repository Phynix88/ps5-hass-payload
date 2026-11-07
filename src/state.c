/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "state.h"

#include <string.h>

#include "auth.h"
#include "limits.h"
#include "fan.h"
#include "procs.h"
#include "log.h"
#include "perm.h"
#include "platform.h"
#include "pldmgr.h"
#include "power.h"

#ifndef PS5H_VERSION
#define PS5H_VERSION "dev"
#endif
#ifndef PS5H_COMMIT
#define PS5H_COMMIT "unknown"
#endif

static void storage(jw_t *w, const char *prefix, const char *path) {
  int64_t free_mb = 0, total_mb = 0;
  bool ok = plat_storage_mb(path, &free_mb, &total_mb) == 0;
  char key[32];
  strcpy(key, prefix);
  strcat(key, "_free_mb");
  jw_key(w, key);
  jw_opt_int(w, ok, free_mb);
  strcpy(key, prefix);
  strcat(key, "_total_mb");
  jw_key(w, key);
  jw_opt_int(w, ok, total_mb);
}

/* SoC power and external drives change slowly and cost a console call each:
 * kept for PS5H_SOC_POWER_CACHE_S / PS5H_DRIVES_CACHE_S. Outside "on" nothing
 * is read and the caches are dropped, so the first answer after the console
 * is "on" again is a fresh reading. */
static struct {
  bool have;
  int64_t at_ms;
  plat_soc_power value; /* value.ok false = the last read gave nothing */
} soc_cache;

#define MAX_DRIVES 10
static const struct {
  const char *path, *name;
} drive_paths[MAX_DRIVES] = {
    {"/mnt/ext1", "m2"},     {"/mnt/ext0", "extended"}, {"/mnt/usb0", "usb0"},
    {"/mnt/usb1", "usb1"},   {"/mnt/usb2", "usb2"},     {"/mnt/usb3", "usb3"},
    {"/mnt/usb4", "usb4"},   {"/mnt/usb5", "usb5"},     {"/mnt/usb6", "usb6"},
    {"/mnt/usb7", "usb7"},
};
static struct {
  bool have;
  int64_t at_ms;
  int n;
  struct {
    const char *name;
    int64_t free_mb, total_mb;
  } d[MAX_DRIVES];
} drive_cache;

static void soc_power_json(jw_t *w, bool read) {
  int64_t now = plat_mono_ms();
  plat_soc_power *p = &soc_cache.value;
  if (!read) {
    soc_cache.have = false;
  } else if (!soc_cache.have ||
             now - soc_cache.at_ms >= PS5H_SOC_POWER_CACHE_S * 1000 ||
             now < soc_cache.at_ms) {
    memset(p, 0, sizeof(*p));
    if (plat_capabilities().soc_power && plat_soc_power_read(p) != 0) {
      p->ok = false;
    }
    soc_cache.have = true;
    soc_cache.at_ms = now;
  }
  jw_key(w, "soc_power");
  if (!read || !p->ok) {
    jw_null(w);
    return;
  }
  {
    int64_t total = 0;
    for (int i = 0; i < 8; i++) {
      total += p->mw[i] > 0 ? p->mw[i] : 0;
    }
    jw_obj(w);
    jw_key(w, "total_mw");
    jw_int(w, total);
    jw_key(w, "rails");
    jw_arr(w);
    for (int i = 0; i < 8; i++) {
      if (p->mw[i] > 0) {
        jw_obj(w);
        jw_key(w, "mw");
        jw_int(w, p->mw[i]);
        jw_key(w, "mv");
        jw_int(w, p->mv[i]);
        jw_key(w, "ma");
        jw_int(w, p->ma[i]);
        jw_end_obj(w);
      }
    }
    jw_end_arr(w);
    jw_end_obj(w);
  }
}

static void drives_json(jw_t *w, bool read) {
  int64_t now = plat_mono_ms();
  if (!read) {
    drive_cache.have = false;
  } else if (!drive_cache.have ||
             now - drive_cache.at_ms >= PS5H_DRIVES_CACHE_S * 1000 ||
             now < drive_cache.at_ms) {
    drive_cache.n = 0;
    for (int i = 0; i < MAX_DRIVES; i++) {
      int64_t free_mb = 0, total_mb = 0;
      if (plat_storage_mb(drive_paths[i].path, &free_mb, &total_mb) == 0 &&
          total_mb >= PS5H_DRIVE_MIN_MB) {
        drive_cache.d[drive_cache.n].name = drive_paths[i].name;
        drive_cache.d[drive_cache.n].free_mb = free_mb;
        drive_cache.d[drive_cache.n].total_mb = total_mb;
        drive_cache.n++;
      }
    }
    drive_cache.have = true;
    drive_cache.at_ms = now;
  }
  jw_key(w, "drives");
  jw_arr(w);
  for (int i = 0; read && i < drive_cache.n; i++) {
    jw_obj(w);
    jw_key(w, "name");
    jw_cstr(w, drive_cache.d[i].name);
    jw_key(w, "free_mb");
    jw_int(w, drive_cache.d[i].free_mb);
    jw_key(w, "total_mb");
    jw_int(w, drive_cache.d[i].total_mb);
    jw_end_obj(w);
  }
  jw_end_arr(w);
}

/* Outside "on" none of these is read: the console is left alone, every
 * reading is null. */
static void console_json(jw_t *w, const app_list *apps, bool read) {
  plat_running run = {0};
  bool in_app = read && plat_running_app(&run) == 1;
  plat_opt_int cpu = {0}, soc = {0}, fan = {0};
  if (read) {
    cpu = plat_cpu_temp();
    soc = plat_soc_temp();
    fan = plat_fan_raw();
  }

  jw_key(w, "in_app");
  jw_bool(w, in_app);
  jw_key(w, "app");
  if (in_app) {
    const app_t *a = apps_find(apps, run.title_id);
    jw_obj(w);
    jw_key(w, "title_id");
    jw_cstr(w, run.title_id);
    jw_key(w, "name");
    jw_cstr(w, a ? a->name : run.title_id);
    jw_key(w, "app_id");
    jw_int(w, run.app_id);
    jw_end_obj(w);
  } else {
    jw_null(w);
  }
  jw_key(w, "temps");
  jw_obj(w);
  jw_key(w, "cpu");
  jw_opt_int(w, cpu.ok, cpu.value);
  jw_key(w, "soc");
  jw_opt_int(w, soc.ok, soc.value);
  jw_end_obj(w);
  jw_key(w, "fan_raw");
  jw_opt_int(w, fan.ok, fan.value);
  jw_key(w, "storage");
  jw_obj(w);
  if (read) {
    storage(w, "data", "/data");
    storage(w, "user", "/user");
  } else {
    static const char *const keys[] = {"data_free_mb", "data_total_mb",
                                       "user_free_mb", "user_total_mb"};
    for (int i = 0; i < 4; i++) {
      jw_key(w, keys[i]);
      jw_null(w);
    }
  }
  jw_end_obj(w);
  soc_power_json(w, read);
  drives_json(w, read);
}

void state_json(jw_t *w, const app_list *apps, int64_t start_ms) {
  bool on = power_is_on();
  console_json(w, apps, on);
  jw_key(w, "power_state");
  jw_cstr(w, power_state_name());
  jw_key(w, "keep_awake");
  jw_bool(w, power_keep_awake());
  jw_key(w, "uptime_s");
  jw_int(w, plat_uptime_s());
  jw_key(w, "payload_uptime_s");
  jw_int(w, (plat_mono_ms() - start_ms) / 1000);
  jw_key(w, "apps_etag");
  jw_cstr(w, apps->etag);
  {
    plat_name names[PS5H_MAX_PAYLOADS];
    int n = on ? procs_payloads(names, PS5H_MAX_PAYLOADS) : 0;
    jw_key(w, "payloads");
    jw_arr(w);
    for (int i = 0; i < n; i++) {
      jw_cstr(w, names[i]);
    }
    jw_end_arr(w);
  }
}

/* Model and firmware never change: asked once, and only while "on". */
static struct {
  bool have;
  char model[64], fw[16];
} cache;

void info_cache_refresh(void) {
  if (cache.have || !power_is_on()) {
    return;
  }
  plat_model(cache.model);
  plat_firmware(cache.fw);
  cache.have = true;
}

void info_cache_reset(void) {
  memset(&cache, 0, sizeof(cache));
  memset(&soc_cache, 0, sizeof(soc_cache));
  memset(&drive_cache, 0, sizeof(drive_cache));
}

void info_json(jw_t *w, const config *c, const char *device_id,
               bool mdns_running) {
  plat_caps caps = plat_capabilities();
  const char *err = log_last_error();

  info_cache_refresh();
  jw_key(w, "name");
  jw_cstr(w, "ps5-hass");
  jw_key(w, "version");
  jw_cstr(w, PS5H_VERSION);
  jw_key(w, "commit");
  jw_cstr(w, PS5H_COMMIT);
  jw_key(w, "api");
  jw_int(w, 1);
  jw_key(w, "device_id");
  jw_cstr(w, device_id);
  jw_key(w, "model");
  jw_cstr(w, cache.model);
  jw_key(w, "firmware");
  jw_cstr(w, cache.fw);
  jw_key(w, "paired");
  jw_bool(w, auth_paired());
  jw_key(w, "power_actions"); /* v0.3 name: the console can enter rest mode */
  jw_bool(w, caps.standby);
  jw_key(w, "config");
  jw_cstr(w, c->recovered ? "recovered" : "ok");
  jw_key(w, "capabilities");
  jw_obj(w);
  jw_key(w, "temps");
  jw_bool(w, caps.temps);
  jw_key(w, "fan");
  jw_bool(w, caps.fan);
  jw_key(w, "notify");
  jw_bool(w, caps.notify);
  jw_key(w, "random");
  jw_bool(w, caps.random);
  jw_key(w, "launch");
  jw_bool(w, true);
  jw_key(w, "standby");
  jw_bool(w, caps.standby);
  jw_key(w, "mdns");
  jw_bool(w, mdns_running);
  jw_key(w, "power_off");
  jw_bool(w, caps.power_off);
  jw_key(w, "fan_control");
  jw_bool(w, caps.fan_control);
  jw_key(w, "focus");
  jw_bool(w, caps.focus);
  jw_key(w, "power_state");
  jw_bool(w, caps.power_state);
  jw_key(w, "suspend");
  jw_bool(w, caps.suspend);
  jw_key(w, "keep_awake");
  jw_bool(w, caps.keep_awake);
  jw_key(w, "soc_power");
  jw_bool(w, caps.soc_power);
  jw_key(w, "playtime");
  jw_bool(w, true);
  jw_key(w, "payload_control"); /* the last list call worked */
  jw_bool(w, pldmgr_last_ok());
  jw_end_obj(w);
  perm_json(w);
  jw_key(w, "last_error");
  if (err) {
    jw_cstr(w, err);
  } else {
    jw_null(w);
  }
}
