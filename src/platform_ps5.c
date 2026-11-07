/* The console side of platform.h (PS5, payload SDK v0.45, tested FW 13.60).
 *
 * Core functions are linked directly. Optional ones (randomness, rest mode,
 * fan, system language) are resolved on first use with dlopen/dlsym, so a
 * firmware without them still loads the payload; the matching field is then
 * null and /info.capabilities says so.
 *
 * Checked on FW 13.60 with small probe payloads (notification, randomness,
 * language, model, FW); other sources:
 * ps5-status (running app, launch/close, temperatures, older instance),
 * ps5-payload-dev/websrv and ftpsrv (launch context, kill).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/user.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "limits.h"
#include "procs.h"

#define PROC_NAME "ps5-hass.elf"

/* --- console functions --------------------------------------------------- */

typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;

/* Layout as in shsrv ps.c: title_id at byte 16 (the SDK sample "ps" reads it
 * 4 bytes late on FW 13.60). */
typedef struct app_info {
  uint32_t app_id;
  uint64_t unknown1;
  char title_id[14];
  char unknown2[0x3c];
} app_info_t;

typedef struct app_launch_ctx {
  uint32_t structsize;
  uint32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint32_t check_flag;
} app_launch_ctx_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
int sceKernelGetAppInfo(pid_t pid, app_info_t *info);
int sceKernelGetCpuTemperature(int *);
int sceKernelGetSocSensorTemperature(int, int *);
int sceKernelGetHwModelName(char *);
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);
int sceSystemServiceLaunchApp(const char *title_id, char **argv,
                              app_launch_ctx_t *ctx);
int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(uint32_t *user_id);

/* --- optional functions ------------------------------------------------- */

static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static int (*opt_random)(uint8_t *, size_t);
static int (*opt_standby)(void);
static int (*opt_param_int)(int, int *);
/* Signature disputed between sources; called with oversized buffers. */
static int (*opt_fan)(void *, void *);
static int (*opt_turnoff)(void);
static bool temps_ok, fan_device_ok;
/* v0.4. Signatures and libraries as measured on FW 13.60. */
static int (*opt_state)(void);
static int (*opt_focus)(void *, void *);
static int (*opt_lnc_init)(void);
static int (*opt_suspend)(int, int);
static int (*opt_reset_timer)(void);
static int (*opt_power_tick)(void);
static int (*opt_soc_power)(void *);

/* Console functions take buffers of unknown size; give them this much,
 * zeroed and 16-byte aligned, and read only the bytes that are meant. */
#define SONY_BUF 4096

/* Fan threshold of the system driver. Layout and ioctls as documented by
 * phantomptr/ps5upload (payload/include/fan_servo.h): 28 bytes, target in
 * byte 5; GET 0xC01C8F08, SET 0xC01C8F07. Checked on FW 13.60: factory 91 C. */
#define FAN_DEVICE "/dev/icc_fan"
#define FAN_LEN 28
#define FAN_TARGET 5
#define FAN_IOCTL_GET 0xC01C8F08UL
#define FAN_IOCTL_SET 0xC01C8F07UL

static int fan_ioctl(unsigned long req, uint8_t buf[FAN_LEN]) {
  int fd = open(FAN_DEVICE, O_RDONLY, 0);
  int rc;
  if (fd < 0) {
    return -1;
  }
  rc = ioctl(fd, req, buf);
  close(fd);
  return rc < 0 ? -1 : 0;
}

static void *sym(const char *lib, const char *name) {
  void *h = dlopen(lib, RTLD_LAZY);
  return h ? dlsym(h, name) : NULL;
}

static void resolve(void) {
  int t = -1;
  opt_random = sym("libSceRandom.sprx", "sceRandomGetRandomNumber");
  opt_standby = sym("libSceSystemService.sprx", "sceSystemStateMgrEnterStandby");
  opt_param_int = sym("libSceSystemService.sprx", "sceSystemServiceParamGetInt");
  opt_fan = sym("libkernel_sys.sprx", "sceKernelGetCurrentFanDuty");
  opt_turnoff = sym("libSceSystemService.sprx", "sceSystemStateMgrTurnOff");
  opt_state = sym("libSceSystemService.sprx", "sceSystemStateMgrGetCurrentState");
  opt_focus =
      sym("libSceSystemService.sprx", "sceSystemServiceGetAppFocusedAppStatus");
  opt_lnc_init = sym("libSceSystemService.sprx", "sceLncUtilInitialize");
  opt_suspend = sym("libSceSystemService.sprx", "sceLncUtilSuspendApp");
  opt_reset_timer =
      sym("libSceSystemService.sprx", "sceShellCoreUtilResetAutoPowerDownTimer");
  opt_power_tick = sym("libSceSystemService.sprx", "sceSystemServicePowerTick");
  opt_soc_power = sym("libkernel_sys.sprx", "sceKernelGetSocPowerConsumption");
  temps_ok = sceKernelGetCpuTemperature(&t) == 0 && t >= 0 && t <= 130;
  {
    uint8_t buf[FAN_LEN] = {0};
    fan_device_ok = fan_ioctl(FAN_IOCTL_GET, buf) == 0;
  }
}

plat_caps plat_capabilities(void) {
  plat_caps c;
  pthread_once(&resolve_once, resolve);
  c.notify = true;
  c.random = opt_random != NULL;
  c.standby = opt_standby != NULL;
  c.fan = opt_fan != NULL;
  c.temps = temps_ok;
  c.language = opt_param_int != NULL;
  c.power_off = opt_turnoff != NULL;
  c.fan_control = fan_device_ok;
  c.focus = opt_focus != NULL;
  c.power_state = opt_state != NULL;
  c.suspend = opt_lnc_init != NULL && opt_suspend != NULL;
  c.keep_awake = opt_reset_timer != NULL && opt_power_tick != NULL;
  c.soc_power = opt_soc_power != NULL;
  return c;
}

/* --- process list -------------------------------------------------------- */

static uint8_t *proc_list(size_t *size) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  uint8_t *buf;
  if (sysctl(mib, 4, NULL, size, NULL, 0)) {
    return NULL;
  }
  *size += 16 * sizeof(struct kinfo_proc); /* the list may grow */
  if (!(buf = malloc(*size))) {
    return NULL;
  }
  if (sysctl(mib, 4, buf, size, NULL, 0)) {
    free(buf);
    return NULL;
  }
  return buf;
}

/* A record must lie inside the buffer; its size is what the kernel reports
 * (1096 bytes on FW 13.60, not sizeof(struct kinfo_proc)). */
static bool record_ok(const struct kinfo_proc *ki, const uint8_t *p,
                      const uint8_t *end) {
  size_t need = offsetof(struct kinfo_proc, ki_comm) + COMMLEN + 1;
  if ((size_t)(end - p) < need) {
    return false;
  }
  return ki->ki_structsize >= (int)need &&
         (size_t)ki->ki_structsize <= (size_t)(end - p);
}

static void kill_older_instances(void) {
  pid_t self = getpid();
  size_t size;
  uint8_t *buf = proc_list(&size);
  pid_t old[8];
  int n = 0;
  if (!buf) {
    return;
  }
  for (uint8_t *p = buf; p < buf + size;) {
    struct kinfo_proc *ki = (struct kinfo_proc *)p;
    if (!record_ok(ki, p, buf + size)) {
      break;
    }
    p += ki->ki_structsize;
    if (ki->ki_pid != self && !strcmp(ki->ki_comm, PROC_NAME) &&
        n < (int)(sizeof(old) / sizeof(old[0]))) {
      old[n++] = ki->ki_pid;
    }
  }
  free(buf);
  if (n > 0) {
    /* SIGTERM first: the old one saves its play time; SIGKILL if it hangs */
    procs_stop(old, n, PS5H_STOP_WAIT_MS);
    sleep(1); /* let the old one release its port */
  }
}

int plat_init(void) {
  syscall(SYS_thr_set_name, -1, PROC_NAME);
  kill_older_instances();
  pthread_once(&resolve_once, resolve);
  return sceUserServiceInitialize(NULL) < 0 ? -1 : 0;
}

/* --- clocks -------------------------------------------------------------- */

int64_t plat_wall_s(void) { return (int64_t)time(NULL); }

int64_t plat_mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t plat_uptime_s(void) {
  int mib[2] = {CTL_KERN, KERN_BOOTTIME};
  struct timeval boot;
  size_t len = sizeof(boot);
  if (sysctl(mib, 2, &boot, &len, NULL, 0) || boot.tv_sec <= 0) {
    return plat_mono_ms() / 1000;
  }
  return plat_wall_s() - (int64_t)boot.tv_sec;
}

const char *plat_root(void) { return ""; }

/* --- randomness, notification -------------------------------------------- */

int plat_random(uint8_t *buf, size_t n) {
  pthread_once(&resolve_once, resolve);
  if (!opt_random) {
    return -1;
  }
  return opt_random(buf, n) == 0 ? 0 : -1;
}

int plat_notify(const char *ascii) {
  notify_request_t req;
  memset(&req, 0, sizeof(req));
  snprintf(req.message, sizeof(req.message), "%s", ascii);
  return sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* --- apps ---------------------------------------------------------------- */

static bool title_of(int app_id, char out[10]) {
  size_t size;
  uint8_t *buf = proc_list(&size);
  bool found = false;
  if (!buf) {
    return false;
  }
  for (uint8_t *p = buf; p < buf + size && !found;) {
    struct kinfo_proc *ki = (struct kinfo_proc *)p;
    union {
      app_info_t info;
      uint8_t raw[0x200]; /* how much libkernel writes is not documented */
    } u;
    if (!record_ok(ki, p, buf + size)) {
      break;
    }
    p += ki->ki_structsize;
    memset(&u, 0, sizeof(u));
    if (sceKernelGetAppInfo(ki->ki_pid, &u.info) == 0 &&
        (int)u.info.app_id == app_id && u.info.title_id[0]) {
      memcpy(out, u.info.title_id, 9);
      out[9] = '\0';
      found = true;
    }
  }
  free(buf);
  return found;
}

int plat_running_app(plat_running *out) {
  int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
  if (app_id <= 0) {
    return 0;
  }
  memset(out, 0, sizeof(*out));
  out->app_id = app_id;
  /* without a title id the answer would not be useful */
  return title_of(app_id, out->title_id) ? 1 : -1;
}

int plat_launch(const char *title_id) {
  app_launch_ctx_t ctx;
  char *argv[] = {NULL};
  memset(&ctx, 0, sizeof(ctx));
  if (sceUserServiceGetForegroundUser(&ctx.user_id)) {
    return -1;
  }
  /* success returns the new app id (>= 0), checked on FW 13.60 */
  return sceSystemServiceLaunchApp(title_id, argv, &ctx);
}

int plat_kill_app(int app_id) {
  return sceSystemServiceKillApp(app_id, -1, 0, 0);
}

int plat_foreground_user(uint32_t *user) {
  *user = 0;
  return sceUserServiceGetForegroundUser(user);
}

int plat_standby(void) {
  pthread_once(&resolve_once, resolve);
  return opt_standby ? opt_standby() : -1;
}

/* --- readings ------------------------------------------------------------ */

static plat_opt_int temp_value(int rc, int v) {
  plat_opt_int r = {rc == 0 && v >= 0 && v <= 130, v};
  return r;
}

plat_opt_int plat_cpu_temp(void) {
  int v = -1;
  int rc = sceKernelGetCpuTemperature(&v);
  return temp_value(rc, v);
}

plat_opt_int plat_soc_temp(void) {
  int v = -1;
  int rc = sceKernelGetSocSensorTemperature(0, &v);
  return temp_value(rc, v);
}

plat_opt_int plat_fan_raw(void) {
  uint32_t duty[8] = {0};
  uint8_t scratch[256] = {0};
  plat_opt_int r = {false, 0};
  pthread_once(&resolve_once, resolve);
  if (opt_fan && opt_fan(duty, scratch) == 0) {
    r.ok = true;
    r.value = (int)(duty[0] & 0xffff);
  }
  return r;
}

int plat_language(void) {
  int v = -1;
  pthread_once(&resolve_once, resolve);
  if (!opt_param_int || opt_param_int(1, &v) != 0) {
    return -1;
  }
  return v;
}

int plat_storage_mb(const char *path, int64_t *free_mb, int64_t *total_mb) {
  struct statvfs st;
  if (statvfs(path, &st)) {
    return -1;
  }
  *free_mb = (int64_t)st.f_bavail * (int64_t)st.f_frsize / (1024 * 1024);
  *total_mb = (int64_t)st.f_blocks * (int64_t)st.f_frsize / (1024 * 1024);
  return 0;
}

void plat_model(char out[64]) {
  char buf[128] = {0};
  out[0] = '\0';
  if (sceKernelGetHwModelName(buf)) {
    return;
  }
  for (char *p = buf; *p; p++) {
    if (*p < 0x20 || *p > 0x7e) {
      *p = '?';
    }
  }
  snprintf(out, 64, "%s", buf);
}

void plat_firmware(char out[16]) {
  /* BCD: 0x13600007 -> "13.60" */
  uint32_t v = (uint32_t)kernel_get_fw_version();
  snprintf(out, 16, "%x.%02x", (v >> 24) & 0xff, (v >> 16) & 0xff);
}

int plat_ipv4_list(uint32_t *addrs, int max) {
  struct ifaddrs *ifs, *i;
  int n = 0;
  if (getifaddrs(&ifs)) {
    return 0;
  }
  for (i = ifs; i && n < max; i = i->ifa_next) {
    if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET &&
        !(i->ifa_flags & IFF_LOOPBACK)) {
      addrs[n++] = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
    }
  }
  freeifaddrs(ifs);
  return n;
}

/* --- v0.2 ---------------------------------------------------------------- */

int plat_proc_names(plat_name *names, int max) {
  size_t size;
  uint8_t *buf = proc_list(&size);
  int n = 0;
  if (!buf) {
    return 0;
  }
  for (uint8_t *p = buf; p < buf + size && n < max;) {
    struct kinfo_proc *ki = (struct kinfo_proc *)p;
    if (!record_ok(ki, p, buf + size)) {
      break;
    }
    p += ki->ki_structsize;
    snprintf(names[n++], sizeof(plat_name), "%s", ki->ki_comm);
  }
  free(buf);
  return n;
}

int plat_power_off(void) {
  pthread_once(&resolve_once, resolve);
  return opt_turnoff ? opt_turnoff() : -1;
}

int plat_fan_get(int *target_c) {
  uint8_t buf[FAN_LEN] = {0};
  if (fan_ioctl(FAN_IOCTL_GET, buf)) {
    return -1;
  }
  *target_c = buf[FAN_TARGET];
  return 0;
}

int plat_fan_set(int target_c) {
  uint8_t buf[FAN_LEN] = {0};
  if (target_c < 1 || target_c > 255) {
    return -1;
  }
  buf[FAN_TARGET] = (uint8_t)target_c;
  return fan_ioctl(FAN_IOCTL_SET, buf);
}

/* --- v0.4 ---------------------------------------------------------------- */

int plat_power_state(void) {
  pthread_once(&resolve_once, resolve);
  return opt_state ? opt_state() : -1;
}

int plat_focused_app(int *app_id) {
  /* polled, so no heap allocation per call */
  _Alignas(16) uint8_t b1[SONY_BUF];
  _Alignas(16) uint8_t b2[SONY_BUF];
  int rc;
  pthread_once(&resolve_once, resolve);
  if (!opt_focus) {
    return -1;
  }
  memset(b1, 0, sizeof(b1));
  memset(b2, 0, sizeof(b2));
  rc = opt_focus(b1, b2);
  if (rc == 0) {
    int32_t id;
    memcpy(&id, b1, sizeof(id)); /* the first 4 bytes are the app id */
    *app_id = id;
  }
  return rc;
}

int plat_suspend_app(int app_id) {
  static bool lnc_ready;
  static pthread_mutex_t lnc_lock = PTHREAD_MUTEX_INITIALIZER;
  int rc = 0;
  pthread_once(&resolve_once, resolve);
  if (!opt_lnc_init || !opt_suspend) {
    return -1;
  }
  /* Only the first minimise initialises the launch utility; the console once
   * crashed entering rest mode while it was initialised, so nothing else may
   * start it. */
  pthread_mutex_lock(&lnc_lock);
  if (!lnc_ready) {
    rc = opt_lnc_init();
    lnc_ready = rc == 0;
  }
  pthread_mutex_unlock(&lnc_lock);
  return rc != 0 ? rc : opt_suspend(app_id, 0);
}

int plat_keep_awake(void) {
  int r1, r2;
  pthread_once(&resolve_once, resolve);
  if (!opt_reset_timer || !opt_power_tick) {
    return -1;
  }
  r1 = opt_reset_timer();
  r2 = opt_power_tick();
  return r1 != 0 ? r1 : r2;
}

int plat_soc_power_read(plat_soc_power *out) {
  uint32_t *buf;
  int rc;
  memset(out, 0, sizeof(*out));
  pthread_once(&resolve_once, resolve);
  if (!opt_soc_power) {
    return -1;
  }
  buf = aligned_alloc(16, SONY_BUF);
  if (!buf) {
    return -1;
  }
  memset(buf, 0, SONY_BUF);
  rc = opt_soc_power(buf);
  if (rc == 0) {
    /* 8 rails, each {mW, mV, mA} as u32; whatever follows is not used. */
    for (int i = 0; i < 8; i++) {
      out->mw[i] = (int)buf[3 * i];
      out->mv[i] = (int)buf[3 * i + 1];
      out->ma[i] = (int)buf[3 * i + 2];
    }
    out->ok = true;
  }
  free(buf);
  return rc;
}
