/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "perm.h"

#include <stdio.h>
#include <string.h>

#include "fan.h"
#include "log.h"
#include "platform.h"

static const struct {
  unsigned bit;
  const char *name, *text;
} PERMS[] = {
    {PERM_POWER_ACTIONS, "power_actions", "rest mode"},
    {PERM_POWER_OFF, "power_off", "power off"},
    {PERM_FAN_CONTROL, "fan_control", "fan threshold"},
};
#define N_PERMS (sizeof(PERMS) / sizeof(PERMS[0]))

int perm_parse(const jr_t *r, int arr_tok, unsigned *out) {
  unsigned bits = 0;
  int n, t;
  *out = 0;
  if (arr_tok < 0 || arr_tok >= r->ntok || r->tok[arr_tok].type != JSMN_ARRAY ||
      (n = r->tok[arr_tok].size) < 1) {
    return -1;
  }
  for (t = arr_tok + 1; n > 0; n--, t++) {
    char name[32];
    unsigned bit = 0;
    /* only plain strings: anything nested would shift the token walk */
    if (t >= r->ntok || r->tok[t].type != JSMN_STRING ||
        !jr_string(r, t, name, sizeof(name))) {
      return -1;
    }
    for (size_t i = 0; i < N_PERMS; i++) {
      if (!strcmp(name, PERMS[i].name)) {
        bit = PERMS[i].bit;
      }
    }
    if (!bit || (bits & bit)) {
      return -1;
    }
    bits |= bit;
  }
  *out = bits;
  return 0;
}

unsigned perm_available(void) {
  plat_caps caps = plat_capabilities();
  return (caps.standby ? PERM_POWER_ACTIONS : 0u) |
         (caps.power_off ? PERM_POWER_OFF : 0u) |
         (caps.fan_control ? PERM_FAN_CONTROL : 0u);
}

unsigned perm_allowed(const config *c) {
  return (c->power_actions ? PERM_POWER_ACTIONS : 0u) |
         (c->power_off ? PERM_POWER_OFF : 0u) |
         (c->fan_control ? PERM_FAN_CONTROL : 0u);
}

void perm_text(unsigned perms, char *out, size_t cap) {
  size_t len = 0;
  if (!cap) {
    return;
  }
  out[0] = '\0';
  for (size_t i = 0; i < N_PERMS; i++) {
    if (perms & PERMS[i].bit) {
      int w = snprintf(out + len, cap - len, "%s%s", len ? ", " : "", PERMS[i].text);
      if (w < 0 || (size_t)w >= cap - len) {
        return;
      }
      len += (size_t)w;
    }
  }
}

static void set_bits(config *c, unsigned bits) {
  c->power_actions = bits & PERM_POWER_ACTIONS;
  c->power_off = bits & PERM_POWER_OFF;
  c->fan_control = bits & PERM_FAN_CONTROL;
}

int perm_apply(config *c, unsigned grant, unsigned revoke, const char *who) {
  unsigned before = perm_allowed(c), after = (before | grant) & ~revoke;
  int target_before = c->fan_target_c;
  bool fan_off = (before & PERM_FAN_CONTROL) && !(after & PERM_FAN_CONTROL);
  char text[64];
  set_bits(c, after);
  if (fan_off) {
    c->fan_target_c = 0;
  }
  if (config_save(c)) {
    set_bits(c, before);
    c->fan_target_c = target_before;
    log_error("permissions: could not save config.json");
    return -1;
  }
  if (after & ~before) {
    perm_text(after & ~before, text, sizeof(text));
    log_msg("permissions: granted %s (by \"%s\")", text, who ? who : "?");
  }
  if (before & ~after) {
    perm_text(before & ~after, text, sizeof(text));
    log_msg("permissions: revoked %s (by \"%s\")", text, who ? who : "?");
  }
  if (fan_off && target_before) {
    fan_release(c); /* only if we had set a threshold */
  }
  return 0;
}

void perm_json(jw_t *w, const config *c) {
  unsigned avail = perm_available(), allowed = perm_allowed(c);
  jw_key(w, "permissions");
  jw_obj(w);
  for (size_t i = 0; i < N_PERMS; i++) {
    jw_key(w, PERMS[i].name);
    jw_obj(w);
    jw_key(w, "allowed");
    jw_bool(w, allowed & PERMS[i].bit);
    jw_key(w, "available");
    jw_bool(w, avail & PERMS[i].bit);
    jw_end_obj(w);
  }
  jw_end_obj(w);
}
