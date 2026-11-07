/* Server loop with real sockets on 127.0.0.1. Server and clients run in one
 * thread: clients are non-blocking, server_step is pumped by the test.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>

#include "apps.h"
#include "auth.h"
#include "config.h"
#include "control.h"
#include "fan.h"
#include "platform_fake.h"
#include "power.h"
#include "router.h"
#include "server.h"
#include "state.h"

static config cfg;
static app_list apps;
static route_ctx ctx;

static server *setup_in_state(uint16_t port, int power_state) {
  server *s;
  fake_reset();
  fake.power_state = power_state;
  info_cache_reset();
  power_init();
  snprintf(fake.root, sizeof(fake.root), "%s", t_tmpdir());
  config_load(&cfg);
  auth_init(&cfg);
  control_init();
  apps_free(&apps);
  apps_scan(&apps);
  memset(&ctx, 0, sizeof(ctx));
  ctx.cfg = &cfg;
  ctx.apps = &apps;
  ctx.device_id = "00112233445566778899aabbccddeeff";
  s = server_new(&ctx, port);
  server_step(s, 0); /* binds */
  return s;
}

static server *setup(uint16_t port) { return setup_in_state(port, 1000); }

static void pump(server *s, int n) {
  for (int i = 0; i < n; i++) {
    server_step(s, 2);
  }
}

static int client_rcvbuf(server *s, int rcvbuf) {
  struct sockaddr_in a;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (rcvbuf) {
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  }
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(server_bound_port(s));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&a, sizeof(a))) {
    perror("connect");
    exit(2);
  }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  return fd;
}

static int client(server *s) { return client_rcvbuf(s, 0); }

static void send_str(int fd, const char *s) {
  if (send(fd, s, strlen(s), 0) != (ssize_t)strlen(s)) {
    perror("send");
    exit(2);
  }
}

/* Reads until the server closes; pumps the server meanwhile.
 * Returns bytes read, -1 if the connection is still open after many pumps. */
static ssize_t read_all(server *s, int fd, char *buf, size_t cap) {
  size_t n = 0;
  for (int i = 0; i < 5000; i++) {
    ssize_t r = recv(fd, buf + n, cap - n - 1, 0);
    if (r > 0) {
      n += (size_t)r;
      continue;
    }
    if (r == 0) {
      buf[n] = '\0';
      return (ssize_t)n;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      buf[n] = '\0';
      return (ssize_t)n; /* reset counts as closed */
    }
    server_step(s, 2);
  }
  buf[n] = '\0';
  return -1;
}

/* true when the peer has closed (EOF or reset) without pumping. */
static bool closed(int fd) {
  char c;
  ssize_t r = recv(fd, &c, 1, 0);
  return r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
}

static const char *INFO = "GET /api/v1/info HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";

static void test_server_answers_info(void) {
  static char buf[65536];
  server *s = setup(0);
  int fd = client(s);
  send_str(fd, INFO);
  T_ASSERT(read_all(s, fd, buf, sizeof(buf)) > 0);
  T_ASSERT(strstr(buf, "HTTP/1.1 200 OK\r\n") == buf);
  T_ASSERT(strstr(buf, "Content-Type: application/json\r\n"));
  T_ASSERT(strstr(buf, "\"name\":\"ps5-hass\""));
  close(fd);
  server_free(s);
}

static void test_server_parse_error_answered(void) {
  static char buf[65536];
  server *s = setup(0);
  int fd = client(s);
  send_str(fd, "POST /api/v1/pair/start HTTP/1.1\r\nHost: 127.0.0.1\r\n"
               "Transfer-Encoding: chunked\r\n\r\n");
  T_ASSERT(read_all(s, fd, buf, sizeof(buf)) > 0);
  T_ASSERT(strstr(buf, "HTTP/1.1 411 Length Required\r\n") == buf);
  T_ASSERT(strstr(buf, "length_required"));
  close(fd);
  server_free(s);
}

static void test_server_slow_client_does_not_block(void) {
  static char buf[65536];
  server *s = setup(0);
  int a = client(s), b;
  send_str(a, "GET /api/v1/info HTTP/1.1\r\n");
  pump(s, 5);
  b = client(s);
  send_str(b, INFO);
  T_ASSERT(read_all(s, b, buf, sizeof(buf)) > 0);
  T_ASSERT(strstr(buf, "200 OK"));
  T_ASSERT(!closed(a));
  fake.mono_ms += 3001;
  pump(s, 5);
  T_ASSERT(closed(a));
  close(a);
  close(b);
  server_free(s);
}

static void test_server_total_deadline(void) {
  int conns = -1, binds = -1;
  server *s = setup(0);
  int fd = client(s);
  /* a request that never completes its body keeps the read phase busy;
   * keep feeding bytes so the read deadline is not what closes it */
  send_str(fd, "POST /api/v1/pair/start HTTP/1.1\r\nHost: 127.0.0.1\r\n"
               "Content-Type: application/json\r\nX-PS5-Hass: 1\r\n"
               "Content-Length: 100\r\n\r\n{");
  pump(s, 3);
  server_debug(s, &conns, &binds);
  T_EQ_INT(conns, 1);
  fake.mono_ms += 2500;
  pump(s, 3);
  T_ASSERT(!closed(fd));
  /* read deadline is measured from the start: 3 s total for the request */
  fake.mono_ms += 600;
  pump(s, 3);
  T_ASSERT(closed(fd));
  close(fd);
  server_free(s);
}

static void test_server_write_deadline(void) {
  static char big[900 * 1024];
  char p[800], req[300], tok[65];
  int conns, binds, left, retry, exp, fd, rcv = 4096;
  server *s = setup(0);
  /* an app with a large icon */
  snprintf(p, sizeof(p), "%s/user/app/BIGG00001/sce_sys/param.json", fake.root);
  t_write_str(p, "{}");
  memset(big, 7, sizeof(big));
  memcpy(big, "\x89PNG\r\n\x1a\n", 8);
  snprintf(p, sizeof(p), "%s/user/app/BIGG00001/sce_sys/icon0.png", fake.root);
  t_write(p, big, sizeof(big));
  auth_pair_start(&exp, &retry);
  T_EQ_INT(auth_pair_finish(fake.last_notify + strlen(fake.last_notify) - 6,
                            "0b7e2f1c-1111-4222-8333-944445555666", "t", tok,
                            &left, &retry),
           AUTH_OK);
  fd = client_rcvbuf(s, rcv);
  snprintf(req, sizeof(req), "GET /api/v1/apps/BIGG00001/icon HTTP/1.1\r\n"
           "Host: 127.0.0.1\r\nAuthorization: Bearer %s\r\n\r\n", tok);
  send_str(fd, req);
  pump(s, 20); /* client does not read: server is stuck writing */
  server_debug(s, &conns, &binds);
  T_EQ_INT(conns, 1);
  fake.mono_ms += 10001;
  pump(s, 3);
  server_debug(s, &conns, &binds);
  T_EQ_INT(conns, 0);
  close(fd);
  server_free(s);
}

static void test_server_ninth_connection_closed(void) {
  int fds[9], conns, binds;
  server *s = setup(0);
  for (int i = 0; i < 8; i++) {
    fds[i] = client(s);
    pump(s, 2);
  }
  server_debug(s, &conns, &binds);
  T_EQ_INT(conns, 8);
  fds[8] = client(s);
  pump(s, 5);
  T_ASSERT(closed(fds[8]));
  T_ASSERT(!closed(fds[0]));
  server_debug(s, &conns, &binds);
  T_EQ_INT(conns, 8);
  for (int i = 0; i < 9; i++) {
    close(fds[i]);
  }
  server_free(s);
}

static void test_server_streams_large_icon(void) {
  static char big[900 * 1024], buf[1024 * 1024];
  char p[800], req[300], tok[65];
  int left, retry, exp, fd;
  ssize_t n;
  const char *bodyp;
  server *s = setup(0);
  snprintf(p, sizeof(p), "%s/user/app/BIGG00001/sce_sys/param.json", fake.root);
  t_write_str(p, "{}");
  for (size_t i = 0; i < sizeof(big); i++) {
    big[i] = (char)(i * 31);
  }
  memcpy(big, "\x89PNG\r\n\x1a\n", 8);
  snprintf(p, sizeof(p), "%s/user/app/BIGG00001/sce_sys/icon0.png", fake.root);
  t_write(p, big, sizeof(big));
  auth_pair_start(&exp, &retry);
  auth_pair_finish(fake.last_notify + strlen(fake.last_notify) - 6,
                   "0b7e2f1c-1111-4222-8333-944445555666", "t", tok, &left,
                   &retry);
  fd = client(s);
  snprintf(req, sizeof(req), "GET /api/v1/apps/BIGG00001/icon HTTP/1.1\r\n"
           "Host: 127.0.0.1\r\nAuthorization: Bearer %s\r\n\r\n", tok);
  send_str(fd, req);
  n = read_all(s, fd, buf, sizeof(buf));
  T_ASSERT(n > 0);
  T_ASSERT(strstr(buf, "Content-Type: image/png\r\n"));
  T_ASSERT(strstr(buf, "Content-Length: 921600\r\n"));
  T_ASSERT(strstr(buf, "ETag: \""));
  bodyp = strstr(buf, "\r\n\r\n") + 4;
  T_EQ_INT(n - (bodyp - buf), (ssize_t)sizeof(big));
  T_ASSERT(!memcmp(bodyp, big, sizeof(big)));
  close(fd);
  server_free(s);
}

static void test_server_wake_rebinds(void) {
  static char buf[65536];
  int conns, binds, fd;
  server *s = setup(0);
  server_debug(s, &conns, &binds);
  T_EQ_INT(binds, 1);
  fake.wall_s += 60; /* wall clock jumped, monotonic did not */
  pump(s, 2);
  server_debug(s, &conns, &binds);
  T_EQ_INT(binds, 2);
  fd = client(s);
  send_str(fd, INFO);
  T_ASSERT(read_all(s, fd, buf, sizeof(buf)) > 0);
  T_ASSERT(strstr(buf, "200 OK"));
  /* small drift does not count as waking up */
  fake.wall_s += 2;
  pump(s, 2);
  server_debug(s, &conns, &binds);
  T_EQ_INT(binds, 2);
  close(fd);
  server_free(s);
}


/* PS5: the monotonic clock keeps running in rest mode, so
 * both clocks jump together. A long gap between two loop passes means the
 * process was suspended. */
static void test_server_suspend_gap_rebinds(void) {
  static char buf[65536];
  int conns, binds, fd;
  server *s = setup(0);
  fake.mono_ms += 60000;
  fake.wall_s += 60;
  pump(s, 2);
  server_debug(s, &conns, &binds);
  T_EQ_INT(binds, 2);
  fd = client(s);
  send_str(fd, INFO);
  T_ASSERT(read_all(s, fd, buf, sizeof(buf)) > 0);
  T_ASSERT(strstr(buf, "200 OK"));
  close(fd);
  server_free(s);
}

static void test_server_iface_change_rebinds(void) {
  int conns, binds;
  server *s = setup(0);
  fake.ipv4[0] = 0x0201a8c0;
  for (int i = 0; i < 31; i++) { /* no suspend gap: one second per pass */
    fake.mono_ms += 1000;
    fake.wall_s += 1;
    pump(s, 1);
  }
  server_debug(s, &conns, &binds);
  T_EQ_INT(binds, 2);
  server_free(s);
}

static void test_server_accept_error_rebinds_with_backoff(void) {
  char p[800];
  int conns, binds, a, b;
  const char *log;
  int lines = 0;
  server *s = setup(0);
  fake.mono_ms += 2000; /* the error comes a while after the last bind */
  fake.wall_s += 2;
  server_debug_fail_accept(s, 163, 1000); /* errno seen on the PS5 */
  a = client(s);
  pump(s, 3);
  server_debug(s, &conns, &binds);
  T_EQ_INT(binds, 2);
  /* the error persists: no rebind faster than once a second */
  b = client(s);
  pump(s, 3);
  server_debug(s, &conns, &binds);
  T_EQ_INT(binds, 2);
  fake.mono_ms += 1001;
  pump(s, 3);
  close(b);
  b = client(s);
  pump(s, 3);
  server_debug(s, &conns, &binds);
  T_EQ_INT(binds, 3);
  /* and the log is not flooded */
  snprintf(p, sizeof(p), "%s/data/ps5-hass/log.txt", fake.root);
  log = t_read(p);
  for (const char *q = log; q && (q = strstr(q, "accept failed")); q++) {
    lines++;
  }
  T_EQ_INT(lines, 1);
  close(a);
  close(b);
  server_free(s);
}

static void test_server_bind_busy_retries_and_notifies_once(void) {
  struct sockaddr_in a;
  socklen_t alen = sizeof(a);
  int blocker = socket(AF_INET, SOCK_STREAM, 0), conns, binds;
  server *s;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  T_EQ_INT(bind(blocker, (struct sockaddr *)&a, sizeof(a)), 0);
  T_EQ_INT(listen(blocker, 1), 0);
  getsockname(blocker, (struct sockaddr *)&a, &alen);
  s = setup(ntohs(a.sin_port));
  server_debug(s, &conns, &binds);
  T_EQ_INT(binds, 0);
  T_EQ_INT(fake.notify_count, 1);
  T_ASSERT(strstr(fake.last_notify, "busy"));
  fake.mono_ms += 10001;
  pump(s, 2);
  T_EQ_INT(fake.notify_count, 1);
  close(blocker);
  pump(s, 2);
  server_debug(s, &conns, &binds);
  T_EQ_INT(binds, 0); /* waits for the next retry */
  fake.mono_ms += 10001;
  pump(s, 2);
  server_debug(s, &conns, &binds);
  T_EQ_INT(binds, 1);
  server_free(s);
}

static void test_server_bind_busy_outside_on_does_not_notify(void) {
  struct sockaddr_in a;
  socklen_t alen = sizeof(a);
  int blocker = socket(AF_INET, SOCK_STREAM, 0), conns, binds;
  server *s;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  T_EQ_INT(bind(blocker, (struct sockaddr *)&a, sizeof(a)), 0);
  T_EQ_INT(listen(blocker, 1), 0);
  getsockname(blocker, (struct sockaddr *)&a, &alen);
  s = setup_in_state(ntohs(a.sin_port), 500);
  server_debug(s, &conns, &binds);
  T_EQ_INT(binds, 0);
  T_EQ_INT(fake.notify_count, 0);
  T_EQ_INT(fake.reads, 0);
  fake.mono_ms += 10001;
  pump(s, 2);
  T_EQ_INT(fake.notify_count, 0);
  /* the console is on again and the port is still busy: now it tells */
  fake.power_state = 1000;
  fake.mono_ms += 10001;
  pump(s, 2);
  T_EQ_INT(fake.notify_count, 1);
  T_ASSERT(strstr(fake.last_notify, "busy"));
  close(blocker);
  server_free(s);
}

static void test_fan_init_deferred_until_on(void) {
  server *s;
  fake.fan_value = 0;
  s = setup_in_state(0, 500);
  fake.fan_value = 91;
  fan_init(&cfg); /* outside "on": does nothing */
  T_EQ_INT(cfg.fan_stock_c, 0);
  fake.power_state = 1000;
  fake.mono_ms += 6000;
  fake.wall_s += 6;
  pump(s, 2);
  T_EQ_INT(cfg.fan_stock_c, 91); /* learned by the first tick while on */
  server_free(s);
}

static void test_fan_tick_runs(void) {
  int rc;
  server *s = setup(0);
  fake.fan_value = 91;
  fan_init(&cfg);
  fan_set(&cfg, 70, &rc);
  fake.fan_value = 91; /* the console reset it */
  fake.mono_ms += 6000;
  fake.wall_s += 6;
  pump(s, 2);
  T_EQ_INT(fake.fan_value, 70);
  server_free(s);
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  T_RUN(test_server_answers_info);
  T_RUN(test_server_parse_error_answered);
  T_RUN(test_server_slow_client_does_not_block);
  T_RUN(test_server_total_deadline);
  T_RUN(test_server_write_deadline);
  T_RUN(test_server_ninth_connection_closed);
  T_RUN(test_server_streams_large_icon);
  T_RUN(test_server_wake_rebinds);
  T_RUN(test_server_bind_busy_retries_and_notifies_once);
  T_RUN(test_server_bind_busy_outside_on_does_not_notify);
  T_RUN(test_fan_init_deferred_until_on);
  T_RUN(test_server_suspend_gap_rebinds);
  T_RUN(test_server_iface_change_rebinds);
  T_RUN(test_server_accept_error_rebinds_with_backoff);
  T_RUN(test_fan_tick_runs);
  apps_free(&apps);
  T_MAIN();
}
