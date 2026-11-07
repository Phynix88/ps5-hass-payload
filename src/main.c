/* ps5-hass: Home Assistant bridge for jailbroken PS5 consoles.
 * Start order and main loop.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps.h"
#include "auth.h"
#include "config.h"
#include "control.h"
#include "fan.h"
#include "log.h"
#include "mdns.h"
#include "platform.h"
#include "playtime.h"
#include "power.h"
#include "router.h"
#include "server.h"
#include "pldmgr.h"
#include "state.h"

#ifdef PS5H_HOST
#include "platform_fake.h"
#endif

#ifndef PS5H_VERSION
#define PS5H_VERSION "dev"
#endif

static volatile sig_atomic_t stop;

static void on_signal(int sig) {
  (void)sig;
  stop = 1;
}

int main(void) {
  static config cfg;
  static app_list apps;
  static route_ctx ctx;
  static char device_id[33];
  server *s;

#ifdef PS5H_HOST
  fake_init_from_env();
  if (getenv("PS5HASS_PLDMGR_PORT")) { /* e2e tests: a stand-in Payload Manager */
    pldmgr_set_target("127.0.0.1", (uint16_t)atoi(getenv("PS5HASS_PLDMGR_PORT")));
  }
#endif
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);
  if (plat_init()) {
    log_error("platform init failed");
  }
  config_load(&cfg);
  if (device_id_get(device_id)) {
    log_error("could not create a device id (no randomness source)");
    snprintf(device_id, sizeof(device_id), "%032d", 0);
  }
  auth_init(&cfg);
  control_init();
  power_init();
  power_tick(plat_mono_ms());
  playtime_load();
  info_cache_refresh();
  fan_init(&cfg);
  apps_scan(&apps);

  ctx.cfg = &cfg;
  ctx.apps = &apps;
  ctx.device_id = device_id;
  ctx.start_mono_ms = plat_mono_ms();
  log_msg("ps5-hass %s starting on port %u", PS5H_VERSION, (unsigned)cfg.port);

  if (!(s = server_new(&ctx, cfg.port))) {
    log_error("out of memory");
    return 1;
  }
  if (mdns_start(device_id, cfg.port, auth_paired())) {
    log_error("mDNS could not start; enter the IP address in Home Assistant");
  }
  while (!stop) {
    server_step(s, 1000);
  }
  playtime_flush();
  server_free(s);
  return 0;
}
