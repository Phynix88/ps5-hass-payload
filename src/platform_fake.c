/* Fake console for unit tests and the host server.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform_fake.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct fake_state fake;

void fake_reset(void) {
  memset(&fake, 0, sizeof(fake));
  fake.wall_s = 1790000000;
  fake.mono_ms = 1000;
  fake.uptime_s = 100;
  fake.next_app_id = 100;
  fake.user_id = 0x10000001;
  fake.cpu = (plat_opt_int){true, 41};
  fake.soc = (plat_opt_int){true, 38};
  fake.fan = (plat_opt_int){true, 131};
  fake.language = 1;
  fake.free_mb = 1000;
  fake.total_mb = 2000;
  snprintf(fake.model, sizeof(fake.model), "CFI-TEST");
  snprintf(fake.firmware, sizeof(fake.firmware), "13.60");
  fake.ipv4[0] = 0x0100007f + 0x01000000; /* any non-loopback value */
  fake.fan_value = 91;
  fake.n_ipv4 = 1;
  fake.caps = (plat_caps){.notify = true,
                          .random = true,
                          .standby = true,
                          .fan = true,
                          .temps = true,
                          .language = true,
                          .power_off = true,
                          .fan_control = true,
                          .focus = true,
                          .power_state = true,
                          .suspend = true,
                          .keep_awake = true,
                          .soc_power = true};
  fake.power_state = 1000;
}

void fake_init_from_env(void) {
  const char *root = getenv("PS5HASS_ROOT");
  fake_reset();
  if (root) {
    snprintf(fake.root, sizeof(fake.root), "%s", root);
  }
  fake.power_from_file = true;
  /* plausible readings so the end-to-end tests see the non-null shapes */
  fake.soc_power = (plat_soc_power){.ok = true,
                                    .mw = {30000, 12000, 0, 4500, 0, 0, 1500, 0},
                                    .mv = {1000, 1200, 0, 900, 0, 0, 1500, 0},
                                    .ma = {30000, 10000, 0, 5000, 0, 0, 1000, 0}};
  snprintf(fake.storage_over[0].path, sizeof(fake.storage_over[0].path), "/mnt/ext1");
  fake.storage_over[0].free_mb = 500000;
  fake.storage_over[0].total_mb = 1000000;
  fake.n_storage_over = 1;
  if (getenv("PS5HASS_FAKE_NO_USER")) {
    fake.user_rc = -1;
    fake.user_id = 0;
  }
}

int plat_init(void) { return 0; }
int64_t plat_wall_s(void) { return fake.wall_s; }
int64_t plat_mono_ms(void) { return fake.mono_ms; }
int64_t plat_uptime_s(void) { return fake.uptime_s; }
const char *plat_root(void) { return fake.root; }

/* splitmix64: deterministic, so tests can predict pairing codes. */
static uint64_t next_random(void) {
  uint64_t z = (fake.random_counter += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

int plat_random(uint8_t *buf, size_t n) {
  if (fake.random_rc < 0) {
    return fake.random_rc;
  }
  for (size_t i = 0; i < n; i += 8) {
    uint64_t v = next_random();
    size_t k = n - i < 8 ? n - i : 8;
    memcpy(buf + i, &v, k);
  }
  return 0;
}

int plat_notify(const char *ascii) {
  fake.reads++;
  if (fake.notify_rc < 0) {
    return fake.notify_rc;
  }
  snprintf(fake.last_notify, sizeof(fake.last_notify), "%s", ascii);
  fake.notify_count++;
  if (fake.root[0]) {
    char path[600];
    int fd;
    snprintf(path, sizeof(path), "%s/fake-notify.txt", fake.root);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
      ssize_t w = write(fd, ascii, strlen(ascii));
      (void)w;
      close(fd);
    }
  }
  return 0;
}

int plat_running_app(plat_running *out) {
  fake.reads++;
  fake.mono_ms += fake.running_block_s * 1000LL;
  fake.wall_s += fake.running_block_s;
  if (fake.running.app_id <= 0) {
    return 0;
  }
  *out = fake.running;
  return 1;
}

static bool installed(const char *title_id) {
  char path[700];
  struct stat st;
  snprintf(path, sizeof(path), "%s/user/app/%s", fake.root, title_id);
  if (!stat(path, &st)) {
    return true;
  }
  snprintf(path, sizeof(path), "%s/user/appmeta/%s", fake.root, title_id);
  return !stat(path, &st);
}

int plat_launch(const char *title_id) {
  fake.reads++;
  if (fake.launch_rc < 0) {
    return fake.launch_rc;
  }
  if (!installed(title_id)) {
    return -1;
  }
  fake.running.app_id = fake.next_app_id++;
  snprintf(fake.running.title_id, sizeof(fake.running.title_id), "%s",
           title_id);
  return fake.running.app_id;
}

int plat_kill_app(int app_id) {
  fake.reads++;
  if (fake.kill_rc < 0) {
    return fake.kill_rc;
  }
  if (fake.running.app_id == app_id) {
    memset(&fake.running, 0, sizeof(fake.running));
  }
  return 0;
}

int plat_foreground_user(uint32_t *user) {
  fake.reads++;
  *user = fake.user_id;
  return fake.user_rc;
}

int plat_standby(void) {
  fake.reads++;
  fake.standby_calls++;
  return fake.standby_rc;
}

plat_opt_int plat_cpu_temp(void) {
  fake.reads++;
  return fake.cpu;
}
plat_opt_int plat_soc_temp(void) {
  fake.reads++;
  return fake.soc;
}
plat_opt_int plat_fan_raw(void) {
  fake.reads++;
  return fake.fan;
}
int plat_language(void) {
  fake.reads++;
  return fake.language;
}

int plat_storage_mb(const char *path, int64_t *free_mb, int64_t *total_mb) {
  fake.reads++;
  for (int i = 0; i < fake.n_storage_over; i++) {
    if (!strcmp(path, fake.storage_over[i].path)) {
      if (fake.storage_over[i].rc < 0) {
        return fake.storage_over[i].rc;
      }
      *free_mb = fake.storage_over[i].free_mb;
      *total_mb = fake.storage_over[i].total_mb;
      return 0;
    }
  }
  if (!strncmp(path, "/mnt/", 5)) {
    return -1; /* no drive */
  }
  if (fake.storage_rc < 0) {
    return fake.storage_rc;
  }
  *free_mb = fake.free_mb;
  *total_mb = fake.total_mb;
  return 0;
}

void plat_model(char out[64]) {
  fake.reads++;
  snprintf(out, 64, "%s", fake.model);
}
void plat_firmware(char out[16]) {
  fake.reads++;
  snprintf(out, 16, "%s", fake.firmware);
}

int plat_ipv4_list(uint32_t *addrs, int max) {
  int n = fake.n_ipv4 < max ? fake.n_ipv4 : max;
  memcpy(addrs, fake.ipv4, (size_t)n * sizeof(*addrs));
  return n;
}

plat_caps plat_capabilities(void) { return fake.caps; }

int plat_proc_names(plat_name *names, int max) {
  int n = fake.n_procs < max ? fake.n_procs : max;
  fake.reads++;
  memcpy(names, fake.procs, (size_t)n * sizeof(plat_name));
  return n;
}

int plat_fan_get(int *target_c) {
  fake.reads++;
  if (fake.fan_get_rc < 0) {
    return fake.fan_get_rc;
  }
  *target_c = fake.fan_value;
  return 0;
}

int plat_fan_set(int target_c) {
  fake.reads++;
  fake.fan_set_calls++;
  if (fake.fan_set_rc < 0) {
    return fake.fan_set_rc;
  }
  fake.fan_value = target_c;
  return 0;
}

int plat_power_off(void) {
  fake.reads++;
  fake.power_off_calls++;
  return fake.power_off_rc;
}

/* --- v0.4 ---------------------------------------------------------------- */

int plat_power_state(void) {
  fake.power_state_calls++;
  if (fake.power_from_file) {
    char path[600], buf[16] = {0};
    int fd;
    snprintf(path, sizeof(path), "%s/fake-power-state", fake.root);
    if ((fd = open(path, O_RDONLY)) >= 0) {
      if (read(fd, buf, sizeof(buf) - 1) > 0) {
        fake.power_state = atoi(buf);
      }
      close(fd);
    }
  }
  if (!fake.caps.power_state) {
    return -1;
  }
  return fake.power_state_rc < 0 ? fake.power_state_rc : fake.power_state;
}

int plat_focused_app(int *app_id) {
  if (!fake.caps.focus) {
    return -1;
  }
  fake.sony_calls++;
  fake.reads++;
  fake.focused_app_calls++;
  fake.mono_ms += fake.focus_block_s * 1000LL;
  fake.wall_s += fake.focus_block_s;
  if (fake.focused_app_rc < 0) {
    return fake.focused_app_rc;
  }
  *app_id = fake.focused_app_id;
  return 0;
}

int plat_suspend_app(int app_id) {
  if (!fake.caps.suspend) {
    return -1;
  }
  fake.sony_calls++;
  fake.reads++;
  fake.suspend_calls++;
  fake.suspended_app_id = app_id;
  return fake.suspend_rc;
}

int plat_keep_awake(void) {
  if (!fake.caps.keep_awake) {
    return -1;
  }
  fake.sony_calls++;
  fake.reads++;
  fake.keep_awake_calls++;
  return fake.keep_awake_rc;
}

int plat_soc_power_read(plat_soc_power *out) {
  memset(out, 0, sizeof(*out));
  if (!fake.caps.soc_power) {
    return -1;
  }
  fake.sony_calls++;
  fake.reads++;
  fake.soc_power_calls++;
  if (fake.soc_power_rc < 0) {
    return fake.soc_power_rc;
  }
  *out = fake.soc_power;
  return 0;
}
