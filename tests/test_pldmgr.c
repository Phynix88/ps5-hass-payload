/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>

#include "limits.h"
#include "platform_fake.h"
#include "pldmgr.h"

/* --- mini Payload Manager ------------------------------------------------ */

enum { M_NORMAL, M_STALL, M_GARBAGE, M_HUGE, M_HUGE_NO_LENGTH, M_TRUNCATED,
       M_FAIL_ACTION, M_STALL_ACTION };

static struct {
  pthread_t th;
  int listen_fd;
  uint16_t port;
  volatile int stop;
  int mode;
  const char *list_body;
  const char *procs_body;
  pthread_mutex_t mu;
  char reqs[16][300]; /* request lines */
  int n_reqs;
  int markers; /* requests with the X-PLDMGR-Request: 1 header */
} srv;

/* Real answers of Payload Manager 0.6.3 (shortened). */
static const char LIST[] =
    "{\"self\":{\"id\":\"pldmgr\",\"running_version\":\"0.6.3\"},\"items\":["
    "{\"slot\":\"repo/ftpsrv@e8cce5d1\",\"path\":\"/data/pldmgr/payloads/repo/"
    "ftpsrv@e8cce5d1/ftpsrv_v0.21.1.elf\",\"kind\":\"repo\",\"filename\":"
    "\"ftpsrv_v0.21.1.elf\",\"id\":\"ftpsrv\",\"name\":\"FTP Server\"},"
    "{\"slot\":\"repo/websrv@11111111\",\"kind\":\"repo\",\"filename\":"
    "\"websrv_v0.34.elf\",\"id\":\"websrv\",\"name\":\"Web Server\"},"
    "{\"slot\":\"repo/CheatRunner@22222222\",\"kind\":\"repo\",\"filename\":"
    "\"CheatRunner_v0.18.1.elf\",\"id\":\"CheatRunner\"},"
    "{\"slot\":\"repo/smp@33333333\",\"kind\":\"repo\",\"filename\":"
    "\"ShadowMountPlus_1.7beta7.elf\",\"id\":\"smp\",\"name\":\"Shadow Mount\"},"
    "{\"slot\":\"repo/kstuff@44444444\",\"kind\":\"repo\",\"filename\":"
    "\"kstuff-lite_v1.11.elf\",\"id\":\"kstuff\",\"name\":\"kstuff\"},"
    "{\"slot\":\"repo/etaHEN@55555555\",\"kind\":\"repo\",\"filename\":"
    "\"etaHEN_2.5B.bin\",\"id\":\"etaHEN\"},"
    "{\"slot\":\"repo/ps5upload@66666666\",\"kind\":\"repo\",\"filename\":"
    "\"ps5upload_v7.1.19.elf\",\"id\":\"ps5upload\"},"
    "{\"slot\":\"repo/wkal@77777777\",\"kind\":\"repo\",\"filename\":"
    "\"WebKit-Autoloader-Installer_v0.6.1.elf\",\"id\":\"WebKit-Autoloader-Installer\"},"
    "{\"slot\":\"local/ps5-hass.elf\",\"kind\":\"local\",\"filename\":"
    "\"ps5-hass.elf\"},"
    "{\"slot\":\"local/my_tool_v2.elf\",\"kind\":\"local\",\"filename\":"
    "\"my_tool_v2.elf\"},"
    "{\"slot\":\"repo/innocent@88888888\",\"kind\":\"repo\",\"filename\":"
    "\"innocent_v1.elf\",\"id\":\"elfldr\",\"name\":\"Innocent\"}"
    "],\"usb\":[],\"payloads\":[\"/data/pldmgr/payloads/x.elf\"]}";

static const char PROCS[] =
    "{\"processes\":[{\"pid\":1,\"name\":\"SceSysCore.elf\",\"memory\":1.5,"
    "\"is_daemon\":true},{\"pid\":97,\"name\":\"ftpsrv.elf\",\"memory\":2.1,"
    "\"is_daemon\":false},{\"pid\":98,\"name\":\"shadowmountplus.elf\","
    "\"memory\":9.0,\"is_daemon\":false},{\"pid\":99,\"name\":\"kstuff.elf\","
    "\"memory\":1.0,\"is_daemon\":false},{\"pid\":100,\"name\":\"eboot.bin\","
    "\"memory\":2863.2,\"is_daemon\":false},{\"pid\":101,\"name\":"
    "\"WebKit-Autoloader-I\",\"memory\":1.0,\"is_daemon\":false},"
    "{\"pid\":102,\"name\":\"my_tool.elf\",\"memory\":1.0,\"is_daemon\":false}"
    "]}";

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void send_all(int fd, const char *p, size_t n) {
  while (n) {
    ssize_t k = send(fd, p, n, 0);
    if (k <= 0) {
      return;
    }
    p += k;
    n -= (size_t)k;
  }
}

static void reply(int fd, int status, const char *body) {
  char head[160];
  int n = snprintf(head, sizeof(head),
                   "HTTP/1.1 %d X\r\nContent-Type: application/json\r\n"
                   "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                   status, strlen(body));
  send_all(fd, head, (size_t)n);
  send_all(fd, body, strlen(body));
}

static void serve(int fd) {
  char buf[2048] = "";
  size_t len = 0;
  int mode;
  const char *line_end;
  while (len < sizeof(buf) - 1 && !strstr(buf, "\r\n\r\n")) {
    ssize_t k;
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, 1000) <= 0) {
      return;
    }
    k = recv(fd, buf + len, sizeof(buf) - 1 - len, 0);
    if (k <= 0) {
      return;
    }
    len += (size_t)k;
    buf[len] = '\0';
  }
  line_end = strstr(buf, "\r\n");
  pthread_mutex_lock(&srv.mu);
  if (srv.n_reqs < 16) {
    size_t n = line_end ? (size_t)(line_end - buf) : 0;
    if (n > 250) {
      n = 250;
    }
    memcpy(srv.reqs[srv.n_reqs], buf, n);
    srv.reqs[srv.n_reqs][n] = '\0';
    srv.n_reqs++;
  }
  if (strstr(buf, "X-PLDMGR-Request: 1\r\n")) {
    srv.markers++;
  }
  mode = srv.mode;
  pthread_mutex_unlock(&srv.mu);

  if (mode == M_STALL || (mode == M_STALL_ACTION && !strncmp(buf, "POST", 4))) {
    for (int i = 0; i < 400 && !srv.stop; i++) {
      usleep(10000);
    }
    return;
  }
  if (mode == M_GARBAGE) {
    send_all(fd, "this is not http\r\n\r\n<<<", 23);
    return;
  }
  if (mode == M_HUGE || mode == M_HUGE_NO_LENGTH) {
    static char chunk[65536];
    char head[128];
    int n;
    memset(chunk, ' ', sizeof(chunk));
    n = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\n%s\r\n",
                 mode == M_HUGE ? "Content-Length: 900000\r\n" : "");
    send_all(fd, head, (size_t)n);
    for (int i = 0; i < 6; i++) {
      send_all(fd, chunk, sizeof(chunk));
    }
    return;
  }
  if (mode == M_TRUNCATED) {
    send_all(fd, "HTTP/1.1 200 OK\r\nContent-Length: 500\r\n\r\n{\"items\":[", 47);
    return;
  }
  if (!strncmp(buf, "GET /list_payloads ", 19)) {
    reply(fd, 200, srv.list_body);
  } else if (!strncmp(buf, "GET /processes_list ", 20)) {
    reply(fd, 200, srv.procs_body);
  } else if (!strncmp(buf, "POST /loadpayload:", 18)) {
    reply(fd, mode == M_FAIL_ACTION ? 500 : 200, "OK\n");
  } else if (!strncmp(buf, "POST /process_kill?pid=", 23)) {
    reply(fd, mode == M_FAIL_ACTION ? 500 : 200, "{\"ok\":true}");
  } else {
    reply(fd, 404, "{}");
  }
}

static void *srv_main(void *arg) {
  (void)arg;
  while (!srv.stop) {
    struct pollfd p = {srv.listen_fd, POLLIN, 0};
    if (poll(&p, 1, 20) > 0) {
      int fd = accept(srv.listen_fd, NULL, NULL);
      if (fd >= 0) {
        serve(fd);
        close(fd);
      }
    }
  }
  return NULL;
}

/* list/procs NULL = the real answers above. Everything the thread reads is set
 * before it starts. */
static void srv_start_with(int mode, const char *list, const char *procs) {
  struct sockaddr_in a;
  socklen_t l = sizeof(a);
  int one = 1;
  memset(&srv.reqs, 0, sizeof(srv.reqs));
  srv.n_reqs = srv.markers = 0;
  srv.mode = mode;
  srv.stop = 0;
  srv.list_body = list ? list : LIST;
  srv.procs_body = procs ? procs : PROCS;
  pthread_mutex_init(&srv.mu, NULL);
  srv.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  setsockopt(srv.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(srv.listen_fd, (struct sockaddr *)&a, sizeof(a)) ||
      listen(srv.listen_fd, 4) ||
      getsockname(srv.listen_fd, (struct sockaddr *)&a, &l)) {
    perror("mini server");
    exit(2);
  }
  srv.port = ntohs(a.sin_port);
  pthread_create(&srv.th, NULL, srv_main, NULL);
  pldmgr_set_target("127.0.0.1", srv.port);
}

static void srv_start(int mode) { srv_start_with(mode, NULL, NULL); }

static void srv_stop(void) {
  srv.stop = 1;
  pthread_join(srv.th, NULL);
  close(srv.listen_fd);
  pthread_mutex_destroy(&srv.mu);
}

/* Requests whose line starts with prefix. */
static int count_reqs(const char *prefix) {
  int n = 0;
  pthread_mutex_lock(&srv.mu);
  for (int i = 0; i < srv.n_reqs; i++) {
    if (!strncmp(srv.reqs[i], prefix, strlen(prefix))) {
      n++;
    }
  }
  pthread_mutex_unlock(&srv.mu);
  return n;
}

static int total_reqs(void) {
  int n;
  pthread_mutex_lock(&srv.mu);
  n = srv.n_reqs;
  pthread_mutex_unlock(&srv.mu);
  return n;
}

static const pld_entry *find(const pld_entry *e, int n, const char *slot) {
  for (int i = 0; i < n; i++) {
    if (!strcmp(e[i].slot, slot)) {
      return &e[i];
    }
  }
  return NULL;
}

/* --- names ----------------------------------------------------------------- */

static void test_match_names(void) {
  char b[64];
  static const struct {
    const char *in, *base;
  } base[] = {
      {"ftpsrv_v0.21.1.elf", "ftpsrv"},
      {"/data/pldmgr/payloads/repo/ftpsrv@e8cce5d1/ftpsrv_v0.21.1.elf", "ftpsrv"},
      {"websrv_v0.34.elf", "websrv"},
      {"CheatRunner_v0.18.1.elf", "CheatRunner"},
      {"/x/CheatRunner_v0.18.elf", "CheatRunner"},
      {"ShadowMountPlus_1.7beta7.elf", "ShadowMountPlus"},
      {"kstuff-lite_v1.11.elf", "kstuff-lite"},
      {"etaHEN_2.5B.bin", "etaHEN"},
      {"ps5upload_v7.1.19.elf", "ps5upload"},
      {"WebKit-Autoloader-Installer_v0.6.1.elf", "WebKit-Autoloader-Installer"},
      {"ps5-hass.elf", "ps5-hass"},
      {"my_tool.elf", "my_tool"},        /* "tool" is no version */
      {"my_tool_v2.elf", "my_tool"},
      {"my_value.elf", "my_value"},      /* "v" without a digit */
      {"plain", "plain"},
      {"repo/ftpsrv@e8cce5d1", "ftpsrv"}, /* a slot */
      {"", ""},
  };
  for (size_t i = 0; i < sizeof(base) / sizeof(base[0]); i++) {
    pldmgr_base_name(base[i].in, b);
    T_EQ_STR(b, base[i].base);
  }
  /* ki_comm names against id and file name */
  T_ASSERT(pldmgr_process_matches("ftpsrv.elf", "ftpsrv", "ftpsrv_v0.21.1.elf"));
  T_ASSERT(pldmgr_process_matches("ftpsrv.elf", NULL, "ftpsrv_v0.21.1.elf"));
  T_ASSERT(pldmgr_process_matches("FTPSRV.ELF", "ftpsrv", NULL));
  T_ASSERT(pldmgr_process_matches("websrv.elf", "websrv", "websrv_v0.34.elf"));
  T_ASSERT(pldmgr_process_matches("CheatRunner.elf", NULL, "CheatRunner_v0.18.1.elf"));
  T_ASSERT(pldmgr_process_matches("CheatRunner.elf", NULL, "CheatRunner_v0.18.elf"));
  T_ASSERT(pldmgr_process_matches("shadowmountplus.elf", "smp",
                                  "ShadowMountPlus_1.7beta7.elf"));
  T_ASSERT(pldmgr_process_matches("kstuff.elf", "kstuff", "kstuff-lite_v1.11.elf"));
  T_ASSERT(pldmgr_process_matches("ps5upload.elf", NULL, "ps5upload_v7.1.19.elf"));
  T_ASSERT(pldmgr_process_matches("ps5-hass.elf", NULL, "ps5-hass.elf"));
  /* cut to 19 characters */
  T_ASSERT(pldmgr_process_matches("WebKit-Autoloader-I", "WebKit-Autoloader-Installer",
                                  "WebKit-Autoloader-Installer_v0.6.1.elf"));
  T_ASSERT(pldmgr_process_matches("WebKit-Autoloader-I", NULL,
                                  "WebKit-Autoloader-Installer_v0.6.1.elf"));
  /* "<name>.elf" cut at 19 characters by the kernel: base of 16, 17, 18, 19+ */
  T_ASSERT(pldmgr_process_matches("abcdefghijklmnop.el", "abcdefghijklmnop", NULL));
  T_ASSERT(pldmgr_process_matches("abcdefghijklmnopq.e", NULL, "abcdefghijklmnopq_v1.elf"));
  T_ASSERT(pldmgr_process_matches("abcdefghijklmnopqr.", "abcdefghijklmnopqr", NULL));
  T_ASSERT(pldmgr_process_matches("abcdefghijklmnopqrs", "abcdefghijklmnopqrs", NULL));
  T_ASSERT(pldmgr_process_matches("abcdefghijklmnopqrs", "abcdefghijklmnopqrstuvw", NULL));
  T_ASSERT(!pldmgr_process_matches("abcdefghijklmnop.el", "abcdefghijklmnopq", NULL));
  T_ASSERT(!pldmgr_process_matches("abcdefghijklmnop.el", "abcdefghijklmno", NULL));
  T_ASSERT(!pldmgr_process_matches("abcdefghijklmnopq.e", "abcdefghijklmnop", NULL));
  /* no match */
  T_ASSERT(!pldmgr_process_matches("ftpsrv.elf", "websrv", "websrv_v0.34.elf"));
  T_ASSERT(!pldmgr_process_matches("ftpsrv2.elf", "ftpsrv", "ftpsrv_v1.elf"));
  T_ASSERT(!pldmgr_process_matches("ftp.elf", "ftpsrv", NULL));
  T_ASSERT(!pldmgr_process_matches("eboot.bin", "ftpsrv", "ftpsrv_v1.elf"));
  T_ASSERT(!pldmgr_process_matches("ftpsrv.elf", NULL, NULL));
  T_ASSERT(!pldmgr_process_matches("ftpsrv.elf", "", ""));
}

static void test_protected_names(void) {
  static const char *const prot[] = {
      "kstuff", "kstuff.elf", "KSTUFF", "kstuff-lite_v1.11.elf", "repo/kstuff@44444444",
      "etaHEN_2.5B.bin", "ETAHEN", "elfldr.elf", "my-elfldr-x", "pldmgr", "PLDMGR.elf",
      "ps5-hass.elf", "Ps5-Hass", "CheatRunner", "CheatRunner_v0.18.1.elf", "cheatrunner",
      "repo/CheatRunner@22222222", "my-cheat-tool", "ps5debug", "ps5debug-ng", "PS5Debug_v1.elf"};
  static const char *const open[] = {"ftpsrv.elf", "websrv_v0.34.elf", "ps5upload_v7.1.19.elf", "ShadowMountPlus_1.7beta7.elf",
                                     "", "ps5"};
  for (size_t i = 0; i < sizeof(prot) / sizeof(prot[0]); i++) {
    T_ASSERT(pldmgr_is_protected(prot[i]));
  }
  for (size_t i = 0; i < sizeof(open) / sizeof(open[0]); i++) {
    T_ASSERT(!pldmgr_is_protected(open[i]));
  }
  T_ASSERT(!pldmgr_is_protected(NULL));
}

/* --- list / start / stop --------------------------------------------------- */

static void test_list_flags(void) {
  pld_entry e[PS5H_PLDMGR_MAX_ENTRIES];
  const pld_entry *x;
  int n;
  srv_start(M_NORMAL);
  n = pldmgr_list(e, PS5H_PLDMGR_MAX_ENTRIES);
  T_EQ_INT(n, 11);
  T_ASSERT(pldmgr_last_ok());
  T_EQ_INT(count_reqs("GET /list_payloads"), 1);
  T_EQ_INT(count_reqs("GET /processes_list"), 1);
  T_EQ_INT(srv.markers, 2);
  x = find(e, n, "repo/ftpsrv@e8cce5d1");
  T_ASSERT(x && x->running && x->pid == 97 && !x->protected_);
  T_EQ_STR(x->name, "FTP Server");
  x = find(e, n, "repo/websrv@11111111");
  T_ASSERT(x && !x->running && x->pid == 0);
  x = find(e, n, "repo/CheatRunner@22222222"); /* no name: the id */
  T_ASSERT(x && !x->running && x->protected_); /* cheat payloads: protected since v0.4.1 */
  T_EQ_STR(x->name, "CheatRunner");
  x = find(e, n, "repo/smp@33333333");
  T_ASSERT(x && x->running && x->pid == 98); /* only the file name matches */
  x = find(e, n, "repo/kstuff@44444444");
  T_ASSERT(x && x->protected_ && x->running);
  x = find(e, n, "repo/etaHEN@55555555");
  T_ASSERT(x && x->protected_ && !x->running);
  x = find(e, n, "repo/wkal@77777777");
  T_ASSERT(x && x->running && x->pid == 101);
  x = find(e, n, "local/ps5-hass.elf"); /* local, no id: base of the file */
  T_ASSERT(x && x->protected_);
  T_EQ_STR(x->name, "ps5-hass");
  x = find(e, n, "local/my_tool_v2.elf");
  T_ASSERT(x && x->running && x->pid == 102 && !x->protected_);
  T_EQ_STR(x->name, "my_tool");
  x = find(e, n, "repo/innocent@88888888"); /* protected through its id */
  T_ASSERT(x && x->protected_);
  T_EQ_INT(pldmgr_list(e, 3), 3); /* max respected */
  srv_stop();
}

static void test_protected_never_started(void) {
  srv_start(M_NORMAL);
  T_EQ_INT(pldmgr_start("repo/kstuff@44444444"), -EPERM);
  T_EQ_INT(pldmgr_start("repo/etaHEN@55555555"), -EPERM);
  T_EQ_INT(pldmgr_start("elfldr"), -EPERM);
  T_EQ_INT(pldmgr_start("local/ps5-hass.elf"), -EPERM);
  T_EQ_INT(pldmgr_stop("repo/kstuff@44444444"), -EPERM);
  T_EQ_INT(pldmgr_stop("repo/etaHEN@55555555"), -EPERM);
  T_EQ_INT(total_reqs(), 0); /* nothing reached the Payload Manager */
  /* protected through id or name only: found in the list, still refused */
  T_EQ_INT(pldmgr_start("repo/innocent@88888888"), -EPERM);
  T_EQ_INT(pldmgr_stop("repo/innocent@88888888"), -EPERM);
  T_EQ_INT(count_reqs("POST"), 0);
  srv_stop();
}

static void test_start_refuses_running(void) {
  srv_start(M_NORMAL);
  T_EQ_INT(pldmgr_start("repo/ftpsrv@e8cce5d1"), -EEXIST);
  T_EQ_INT(count_reqs("POST"), 0);
  T_EQ_INT(pldmgr_start("repo/nope@00000000"), -ESRCH);
  T_EQ_INT(count_reqs("POST"), 0);
  T_EQ_INT(pldmgr_start(""), -EINVAL);
  srv_stop();
}

/* A payload that is stopped or started while a game runs can hang the
 * console (v0.4.0 incident): nothing is sent then. */
static void test_app_running_blocks_start_and_stop(void) {
  fake_reset();
  fake.running.app_id = 7;
  srv_start(M_NORMAL);
  T_EQ_INT(pldmgr_stop("repo/ftpsrv@e8cce5d1"), -EBUSY);
  T_EQ_INT(pldmgr_start("repo/websrv@11111111"), -EBUSY);
  T_EQ_INT(pldmgr_stop("repo/nope@00000000"), -EBUSY); /* before any lookup */
  T_EQ_INT(total_reqs(), 0); /* not even a list request */
  /* protected stays 403 (checked first), a bad slot stays a bad slot */
  T_EQ_INT(pldmgr_stop("repo/kstuff@44444444"), -EPERM);
  T_EQ_INT(pldmgr_start("repo/CheatRunner@22222222"), -EPERM);
  T_EQ_INT(pldmgr_start(""), -EINVAL);
  T_EQ_INT(total_reqs(), 0);
  /* the game closes: payload control works again */
  fake.running.app_id = 0;
  T_EQ_INT(pldmgr_stop("repo/ftpsrv@e8cce5d1"), 0);
  T_EQ_INT(count_reqs("POST /process_kill?pid=97 HTTP/1.1"), 1);
  srv_stop();
  fake_reset();
}

static void test_unreadable_running_app_blocks(void) {
  fake_reset();
  fake.running.app_id = 0;
  srv_start(M_NORMAL);
  /* plat_running_app() fails when its capability is missing or the call
   * errors: the fake reports < 0 through running_rc */
  fake.running_rc = -5;
  T_EQ_INT(pldmgr_stop("repo/ftpsrv@e8cce5d1"), -EBUSY);
  T_EQ_INT(pldmgr_start("repo/websrv@11111111"), -EBUSY);
  T_EQ_INT(total_reqs(), 0);
  srv_stop();
  fake_reset();
}

static void test_start_posts_loadpayload(void) {
  srv_start(M_NORMAL);
  T_EQ_INT(pldmgr_start("repo/websrv@11111111"), 0);
  T_EQ_INT(count_reqs("POST /loadpayload:repo/websrv%4011111111 HTTP/1.1"), 1);
  T_EQ_INT(srv.markers, 3); /* list, processes, load */
  srv_stop();
  srv_start(M_FAIL_ACTION);
  T_EQ_INT(pldmgr_start("repo/websrv@11111111"), -EIO);
  srv_stop();
}

static void test_stop_uses_pid_from_process_list(void) {
  srv_start(M_NORMAL);
  T_EQ_INT(pldmgr_stop("repo/ftpsrv@e8cce5d1"), 0);
  T_EQ_INT(count_reqs("POST /process_kill?pid=97 HTTP/1.1"), 1);
  T_EQ_INT(pldmgr_stop("repo/smp@33333333"), 0);
  T_EQ_INT(count_reqs("POST /process_kill?pid=98 HTTP/1.1"), 1);
  T_EQ_INT(pldmgr_stop("repo/websrv@11111111"), -ENOENT); /* not running */
  T_EQ_INT(pldmgr_stop("repo/nope@00000000"), -ESRCH);
  T_EQ_INT(count_reqs("POST"), 2);
  srv_stop();
  srv_start(M_FAIL_ACTION);
  T_EQ_INT(pldmgr_stop("repo/ftpsrv@e8cce5d1"), -EIO);
  srv_stop();
}

/* --- trouble --------------------------------------------------------------- */

static void test_unreachable_returns_error_fast(void) {
  pld_entry e[4];
  int64_t t0;
  srv_start(M_NORMAL);
  srv_stop(); /* the port is closed now */
  t0 = now_ms();
  T_ASSERT(pldmgr_list(e, 4) < 0);
  T_ASSERT(!pldmgr_last_ok());
  T_ASSERT(pldmgr_start("repo/ftpsrv@e8cce5d1") < 0);
  T_ASSERT(pldmgr_stop("repo/ftpsrv@e8cce5d1") < 0);
  T_ASSERT(now_ms() - t0 <= 2500);
}

static void test_slow_server_times_out(void) {
  pld_entry e[4];
  int64_t t0;
  int rc;
  srv_start(M_STALL);
  t0 = now_ms();
  rc = pldmgr_list(e, 4);
  T_EQ_INT(rc, -ETIMEDOUT);
  T_ASSERT(now_ms() - t0 >= 1800);
  T_ASSERT(now_ms() - t0 <= 2500);
  T_ASSERT(!pldmgr_last_ok());
  srv_stop();
}

static void test_garbage_answer_rejected(void) {
  pld_entry e[4];
  static const char *const bad[] = {
      "not json at all",
      "{}",
      "[]",
      "{\"items\":5}",
      "{\"items\":[{\"slot\":\"a/b\"}",           /* cut off */
      "{\"items\":\"x\",\"payloads\":[\"/a.elf\"]}", /* old shape: no objects */
  };
  srv_start(M_GARBAGE);
  T_EQ_INT(pldmgr_list(e, 4), -EPROTO);
  T_ASSERT(!pldmgr_last_ok());
  srv_stop();
  srv_start(M_TRUNCATED);
  T_ASSERT(pldmgr_list(e, 4) < 0);
  srv_stop();
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    srv_start_with(M_NORMAL, bad[i], NULL);
    T_EQ_INT(pldmgr_list(e, 4), -EPROTO);
    T_ASSERT(!pldmgr_last_ok());
    T_EQ_INT(pldmgr_start("a/b"), -EPROTO);
    srv_stop();
  }
  /* broken process list */
  srv_start_with(M_NORMAL, NULL, "{\"processes\":7}");
  T_EQ_INT(pldmgr_list(e, 4), -EPROTO);
  srv_stop();
}

static void test_oversized_answer_rejected(void) {
  pld_entry e[4];
  srv_start(M_HUGE);
  T_EQ_INT(pldmgr_list(e, 4), -EMSGSIZE);
  srv_stop();
  srv_start(M_HUGE_NO_LENGTH);
  T_EQ_INT(pldmgr_list(e, 4), -EMSGSIZE);
  T_ASSERT(!pldmgr_last_ok());
  srv_stop();
}

static void test_token_bound(void) {
  static char big[200000];
  pld_entry e[4];
  size_t n = (size_t)snprintf(big, sizeof(big), "{\"items\":[");
  for (int i = 0; i < 12000; i++) {
    n += (size_t)snprintf(big + n, sizeof(big) - n, "%s0", i ? "," : "");
  }
  snprintf(big + n, sizeof(big) - n, "]}");
  srv_start_with(M_NORMAL, big, NULL);
  T_EQ_INT(pldmgr_list(e, 4), -EPROTO);
  srv_stop();
}

static void test_odd_items_skipped(void) {
  pld_entry e[8];
  srv_start_with(
      M_NORMAL,
      "{\"items\":[5,{\"filename\":\"x.elf\"},{\"slot\":\"ok/one\",\"filename\":"
      "\"one_v1.elf\"},{\"slot\":\"ok/two\",\"name\":\"T\\u00e4\\\"x\"}]}",
      NULL);
  T_EQ_INT(pldmgr_list(e, 8), 2);
  T_EQ_STR(e[0].slot, "ok/one");
  T_EQ_STR(e[0].name, "one");
  T_EQ_STR(e[1].name, "T\xc3\xa4\"x");
  srv_stop();
}

static void test_broken_fields_never_overrun(void) {
  static char list[4096];
  char longs[301];
  pld_entry e[8];
  int n;
  memset(longs, 'n', 300);
  longs[300] = '\0';
  /* too long for their buffers: id, name, filename, version; slot too long;
   * control character and invalid UTF-8 in slots; a good one at the end */
  snprintf(list, sizeof(list),
           "{\"items\":["
           "{\"slot\":\"a/long\",\"id\":\"%s\",\"name\":\"%s\",\"filename\":\"%s\","
           "\"version\":\"%s\"},"
           "{\"slot\":\"%s\",\"name\":\"slot too long\"},"
           "{\"slot\":\"c/\\u0001ctl\",\"name\":\"control\"},"
           "{\"slot\":\"b/\xff\xfe\",\"name\":\"bytes\"},"
           "{\"slot\":\"z/good\",\"name\":\"Good\",\"version\":\"1.2\"}]}",
           longs, longs, longs, longs, longs);
  srv_start_with(M_NORMAL, list, NULL);
  n = pldmgr_list(e, 8);
  T_EQ_INT(n, 2);
  T_EQ_STR(e[0].slot, "a/long");
  T_EQ_STR(e[0].name, "long"); /* name and id unusable: the slot's base */
  T_EQ_STR(e[0].version, "");
  T_ASSERT(!e[0].running && !e[0].protected_);
  T_EQ_STR(e[1].slot, "z/good");
  T_EQ_STR(e[1].version, "1.2");
  srv_stop();
  /* a bad escape makes the parser refuse the whole answer */
  srv_start_with(M_NORMAL, "{\"items\":[{\"slot\":\"a/b\",\"name\":\"abc\\x\"}]}", NULL);
  T_EQ_INT(pldmgr_list(e, 8), -EPROTO);
  srv_stop();
  srv_start_with(M_NORMAL, "{\"items\":[{\"slot\":\"a/b\",\"id\":\"ab\\u12g4\"}]}", NULL);
  T_EQ_INT(pldmgr_list(e, 8), -EPROTO);
  srv_stop();
}

static void test_start_total_time_is_bounded(void) {
  int64_t t0;
  srv_start(M_STALL_ACTION); /* list works, the action never answers */
  t0 = now_ms();
  T_EQ_INT(pldmgr_start("repo/websrv@11111111"), -ETIMEDOUT);
  T_ASSERT(now_ms() - t0 <= 2500);
  t0 = now_ms();
  T_EQ_INT(pldmgr_stop("repo/ftpsrv@e8cce5d1"), -ETIMEDOUT);
  T_ASSERT(now_ms() - t0 <= 2500);
  srv_stop();
}

static void test_version_in_list(void) {
  pld_entry e[PS5H_PLDMGR_MAX_ENTRIES];
  int n;
  srv_start_with(M_NORMAL,
                 "{\"items\":[{\"slot\":\"r/a\",\"version\":\"v0.21.1\"},"
                 "{\"slot\":\"r/b\"}]}",
                 NULL);
  n = pldmgr_list(e, PS5H_PLDMGR_MAX_ENTRIES);
  T_EQ_INT(n, 2);
  T_EQ_STR(e[0].version, "v0.21.1");
  T_EQ_STR(e[1].version, "");
  srv_stop();
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  T_RUN(test_match_names);
  T_RUN(test_protected_names);
  T_RUN(test_list_flags);
  T_RUN(test_protected_never_started);
  T_RUN(test_start_refuses_running);
  T_RUN(test_app_running_blocks_start_and_stop);
  T_RUN(test_unreadable_running_app_blocks);
  T_RUN(test_start_posts_loadpayload);
  T_RUN(test_stop_uses_pid_from_process_list);
  T_RUN(test_unreachable_returns_error_fast);
  T_RUN(test_slow_server_times_out);
  T_RUN(test_garbage_answer_rejected);
  T_RUN(test_oversized_answer_rejected);
  T_RUN(test_token_bound);
  T_RUN(test_odd_items_skipped);
  T_RUN(test_broken_fields_never_overrun);
  T_RUN(test_start_total_time_is_bounded);
  T_RUN(test_version_in_list);
  T_MAIN();
}
