/* The only boundary to the console. Everything that touches sce* functions,
 * sysctl, clocks, randomness or notifications goes through here.
 * Implementations: platform_ps5.c (console), platform_fake.c (tests).
 *
 * Unless noted otherwise: 0 = ok, < 0 = error code from the console.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS5H_PLATFORM_H
#define PS5H_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  bool ok;
  int value;
} plat_opt_int;

typedef struct {
  int app_id;
  char title_id[10];
} plat_running;

typedef struct {
  bool notify, random, standby, fan, temps, language;
} plat_caps;

/* User service, process name, kill an older instance. */
int plat_init(void);
/* Unix time in seconds. */
int64_t plat_wall_s(void);
/* Monotonic milliseconds. */
int64_t plat_mono_ms(void);
/* Seconds since system start. */
int64_t plat_uptime_s(void);
/* Prefix for every absolute path: "" on the PS5, a test folder in the fake. */
const char *plat_root(void);
/* < 0: no randomness source. */
int plat_random(uint8_t *buf, size_t n);
/* Notification on the TV; ASCII only. */
int plat_notify(const char *ascii);
/* 1 = an app runs (out filled), 0 = none, < 0 = error. */
int plat_running_app(plat_running *out);
/* >= 0 = ok (new app id). */
int plat_launch(const char *title_id);
int plat_kill_app(int app_id);
/* 0 = ok and *user != 0. */
int plat_foreground_user(uint32_t *user);
int plat_standby(void);
plat_opt_int plat_cpu_temp(void);
plat_opt_int plat_soc_temp(void);
plat_opt_int plat_fan_raw(void);
/* System language 0..30, or -1. */
int plat_language(void);
int plat_storage_mb(const char *path, int64_t *free_mb, int64_t *total_mb);
/* "" when unknown. */
void plat_model(char out[64]);
/* "13.60" */
void plat_firmware(char out[16]);
/* Number of IPv4 addresses (network byte order) without loopback. */
int plat_ipv4_list(uint32_t *addrs, int max);
/* What resolved on first use. */
plat_caps plat_capabilities(void);

#endif
