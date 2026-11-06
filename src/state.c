/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "state.h"

#include <string.h>

#include "auth.h"
#include "limits.h"
#include "procs.h"
#include "log.h"
#include "platform.h"

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

void state_json(jw_t *w, const app_list *apps, int64_t start_ms) {
  plat_running run;
  bool in_app = plat_running_app(&run) == 1;
  plat_opt_int cpu = plat_cpu_temp(), soc = plat_soc_temp(),
               fan = plat_fan_raw();

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
  storage(w, "data", "/data");
  storage(w, "user", "/user");
  jw_end_obj(w);
  jw_key(w, "uptime_s");
  jw_int(w, plat_uptime_s());
  jw_key(w, "payload_uptime_s");
  jw_int(w, (plat_mono_ms() - start_ms) / 1000);
  jw_key(w, "apps_etag");
  jw_cstr(w, apps->etag);
  {
    plat_name names[PS5H_MAX_PAYLOADS];
    int n = procs_payloads(names, PS5H_MAX_PAYLOADS);
    jw_key(w, "payloads");
    jw_arr(w);
    for (int i = 0; i < n; i++) {
      jw_cstr(w, names[i]);
    }
    jw_end_arr(w);
  }
}

void info_json(jw_t *w, const config *c, const char *device_id,
               bool mdns_running) {
  char model[64], fw[16];
  plat_caps caps = plat_capabilities();
  const char *err = log_last_error();

  plat_model(model);
  plat_firmware(fw);
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
  jw_cstr(w, model);
  jw_key(w, "firmware");
  jw_cstr(w, fw);
  jw_key(w, "paired");
  jw_bool(w, auth_paired());
  jw_key(w, "power_actions");
  jw_bool(w, c->power_actions);
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
  jw_bool(w, caps.power_off && c->power_off);
  jw_key(w, "fan_control");
  jw_bool(w, caps.fan_control && c->fan_control);
  jw_end_obj(w);
  jw_key(w, "last_error");
  if (err) {
    jw_cstr(w, err);
  } else {
    jw_null(w);
  }
}
