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
  bool notify, random, standby, fan, temps, language, power_off, fan_control;
  bool focus, power_state, suspend, keep_awake, soc_power;
} plat_caps;

/* SoC power rails in mW, mV, mA (mw = mv * ma / 1000). Only rails with data
 * are non-zero. */
typedef struct {
  bool ok;
  int mw[8];
  int mv[8];
  int ma[8];
} plat_soc_power;

/* Process names are at most 19 characters (FreeBSD COMMLEN). */
typedef char plat_name[20];

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
/* 1 = an app runs (out filled), 0 = none, < 0 = error. A minimised game
 * counts as none (the console reports no app for it). */
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
/* Names (ki_comm) of all processes; returns the count, < 0 = the list could
 * not be read. */
int plat_proc_names(plat_name *names, int max);
/* Power the console off (not rest mode). */
int plat_power_off(void);
/* Fan threshold of the system driver in C (read/set). */
int plat_fan_get(int *target_c);
int plat_fan_set(int target_c);

/* Calls added in v0.4. All optional: a missing console function gives a
 * negative return value and the matching plat_caps flag is false. Apart from
 * plat_power_state(), none of them may be called while the console is not
 * "on" (power state 1000). */

/* 1000 on, 500 rest mode, 300 going to rest, 100 shutting down, < 0 error. */
int plat_power_state(void);
/* App id that has the focus (ShellUI id when no game is in front); 0 = ok. */
int plat_focused_app(int *app_id);
/* Minimise the app (it stays open, paused). The first call initialises the
 * launch utility, never earlier. 0 = ok. */
int plat_suspend_app(int app_id);
/* Reset the auto power-down timer and tick; 0 = ok. */
int plat_keep_awake(void);
/* 0 = ok and out->ok; otherwise out->ok is false. */
int plat_soc_power_read(plat_soc_power *out);

#endif
