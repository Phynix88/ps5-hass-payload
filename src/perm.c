/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "perm.h"

#include "platform.h"

void perm_json(jw_t *w) {
  plat_caps caps = plat_capabilities();
  const struct {
    const char *name;
    bool available;
  } perms[] = {
      {"power_actions", caps.standby},
      {"power_off", caps.power_off},
      {"fan_control", caps.fan_control},
  };
  jw_key(w, "permissions");
  jw_obj(w);
  for (size_t i = 0; i < sizeof(perms) / sizeof(perms[0]); i++) {
    jw_key(w, perms[i].name);
    jw_obj(w);
    jw_key(w, "allowed");
    jw_bool(w, perms[i].available);
    jw_key(w, "available");
    jw_bool(w, perms[i].available);
    jw_end_obj(w);
  }
  jw_end_obj(w);
}
