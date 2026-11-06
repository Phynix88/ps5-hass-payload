/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "auth.h"
#include "http.h"
#include "limits.h"
#include "log.h"
#include "mdns.h"
#include "platform.h"

#define IN_CAP (PS5H_MAX_HEADER + PS5H_MAX_BODY + 1)
#define MAX_IFACES 8

typedef struct {
  int fd; /* -1 = free */
  uint32_t peer;
  int64_t start_ms;
  bool writing;
  char in[IN_CAP];
  size_t in_len;
  char *out; /* head + JSON body, or head only for files */
  size_t out_len, out_off;
  int file_fd;
  int64_t file_left;
  char chunk[PS5H_STREAM_CHUNK];
  size_t chunk_len, chunk_off;
} conn;

struct server {
  route_ctx *ctx;
  uint16_t port, bound_port;
  int listen_fd;
  bool bind_notified;
  int64_t last_bind_ms, next_bind_ms; /* last attempt, earliest next one */
  bool rebind_logged;
  int64_t last_rebind_log_ms;
  int64_t last_step_ms;
  int binds;
  int fail_accept_err, fail_accept_count; /* tests */
  int64_t clock_offset_ms; /* wall - monotonic */
  int64_t last_iface_ms;
  uint32_t ifaces[MAX_IFACES];
  int n_ifaces;
  conn conns[PS5H_MAX_CONNS];
};

static int64_t clock_offset(void) {
  return plat_wall_s() * 1000 - plat_mono_ms();
}

server *server_new(route_ctx *ctx, uint16_t port) {
  server *s = calloc(1, sizeof(*s));
  if (!s) {
    return NULL;
  }
  s->ctx = ctx;
  s->port = port;
  s->listen_fd = -1;
  for (int i = 0; i < PS5H_MAX_CONNS; i++) {
    s->conns[i].fd = -1;
    s->conns[i].file_fd = -1;
  }
  s->clock_offset_ms = clock_offset();
  s->last_iface_ms = s->last_step_ms = plat_mono_ms();
  s->n_ifaces = plat_ipv4_list(s->ifaces, MAX_IFACES);
  return s;
}

static void conn_close(conn *c) {
  if (c->fd >= 0) {
    close(c->fd);
  }
  if (c->file_fd >= 0) {
    close(c->file_fd);
  }
  free(c->out);
  c->fd = -1;
  c->file_fd = -1;
  c->out = NULL;
  c->writing = false;
  c->in_len = c->out_len = c->out_off = 0;
  c->chunk_len = c->chunk_off = 0;
  c->file_left = 0;
}

static void close_listener(server *s) {
  if (s->listen_fd >= 0) {
    close(s->listen_fd);
  }
  s->listen_fd = -1;
}

void server_free(server *s) {
  if (!s) {
    return;
  }
  for (int i = 0; i < PS5H_MAX_CONNS; i++) {
    conn_close(&s->conns[i]);
  }
  close_listener(s);
  free(s);
}

uint16_t server_bound_port(const server *s) { return s->bound_port; }

void server_debug_fail_accept(server *s, int err, int count) {
  s->fail_accept_err = err;
  s->fail_accept_count = count;
}

void server_debug(const server *s, int *conns, int *binds) {
  int n = 0;
  for (int i = 0; i < PS5H_MAX_CONNS; i++) {
    n += s->conns[i].fd >= 0;
  }
  *conns = n;
  *binds = s->binds;
}

static void set_nonblock(int fd) {
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

/* SO_REUSEADDR, no SO_REUSEPORT (spec section 8). */
static int try_bind(server *s) {
  struct sockaddr_in a;
  socklen_t alen = sizeof(a);
  int one = 1;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(s->port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (struct sockaddr *)&a, sizeof(a)) ||
      listen(fd, PS5H_BACKLOG)) {
    int e = errno;
    close(fd);
    errno = e;
    return -1;
  }
  set_nonblock(fd);
  getsockname(fd, (struct sockaddr *)&a, &alen);
  s->bound_port = ntohs(a.sin_port);
  s->listen_fd = fd;
  s->binds++;
  return 0;
}

static void ensure_listener(server *s, int64_t now) {
  if (s->listen_fd >= 0 || now < s->next_bind_ms) {
    return;
  }
  s->last_bind_ms = now;
  s->next_bind_ms = now + PS5H_BIND_RETRY_S * 1000; /* if this one fails */
  if (!try_bind(s)) {
    if (s->bind_notified) {
      log_msg("listening on port %u again", (unsigned)s->bound_port);
    }
    s->bind_notified = false;
    return;
  }
  if (!s->bind_notified) {
    char text[64];
    int e = errno;
    snprintf(text, sizeof(text), "ps5-hass: port %u busy, retrying",
             (unsigned)s->port);
    plat_notify(text);
    log_error("bind port %u failed (errno %d), retrying every %d s",
              (unsigned)s->port, e, PS5H_BIND_RETRY_S);
    s->bind_notified = true;
  }
}

/* Close the listener (and optionally all connections) and bind again: after
 * errors at most once per PS5H_REBIND_MIN_MS, logged at most once per
 * PS5H_REBIND_LOG_S so a persistent error cannot flood the log. */
static void rebind(server *s, int64_t now, bool drop_conns, bool is_error,
                   const char *why) {
  /* only error loops are slowed down; waking up rebinds at once */
  int64_t earliest = is_error ? s->last_bind_ms + PS5H_REBIND_MIN_MS : now;
  if (drop_conns) {
    for (int i = 0; i < PS5H_MAX_CONNS; i++) {
      conn_close(&s->conns[i]);
    }
  }
  close_listener(s);
  s->next_bind_ms = earliest > now ? earliest : now;
  if (!s->rebind_logged || now - s->last_rebind_log_ms >= PS5H_REBIND_LOG_S * 1000) {
    if (is_error) {
      log_error("%s, rebinding", why);
    } else {
      log_msg("%s, rebinding", why);
    }
    s->rebind_logged = true;
    s->last_rebind_log_ms = now;
  }
}

static void periodic(server *s, int64_t now) {
  int64_t off = clock_offset();
  int64_t gap = now - s->last_step_ms;
  s->last_step_ms = now;
  /* Two signs of waking up: the wall clock jumped against the monotonic one,
   * or the loop (which runs at least once a second) was not scheduled for a
   * long time. On the PS5 only the second works: its monotonic clock keeps
   * running in rest mode. */
  if (llabs(off - s->clock_offset_ms) > PS5H_WAKE_JUMP_S * 1000 ||
      gap > PS5H_SUSPEND_GAP_S * 1000) {
    char why[64];
    snprintf(why, sizeof(why), "woke up (gap %lld s)", (long long)(gap / 1000));
    s->clock_offset_ms = off;
    rebind(s, now, true, false, why);
    mdns_reannounce(auth_paired());
  }
  if (now - s->last_iface_ms >= PS5H_IFACE_CHECK_S * 1000) {
    uint32_t cur[MAX_IFACES];
    int n = plat_ipv4_list(cur, MAX_IFACES);
    s->last_iface_ms = now;
    if (n != s->n_ifaces ||
        memcmp(cur, s->ifaces, (size_t)n * sizeof(cur[0]))) {
      memcpy(s->ifaces, cur, (size_t)n * sizeof(cur[0]));
      s->n_ifaces = n;
      rebind(s, now, false, false, "network addresses changed");
      mdns_reannounce(auth_paired());
    }
  }
}

static void deadlines(server *s, int64_t now) {
  for (int i = 0; i < PS5H_MAX_CONNS; i++) {
    conn *c = &s->conns[i];
    if (c->fd < 0) {
      continue;
    }
    if ((!c->writing && now - c->start_ms > PS5H_READ_DEADLINE_MS) ||
        now - c->start_ms > PS5H_TOTAL_DEADLINE_MS) {
      conn_close(c);
    }
  }
}

static bool transient_accept_error(int e) {
  return e == EINTR || e == ECONNABORTED || e == EAGAIN || e == EWOULDBLOCK ||
         e == EMFILE || e == ENFILE;
}

static void accept_all(server *s, int64_t now) {
  for (;;) {
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    conn *slot = NULL;
    int fd;
    if (s->fail_accept_count > 0) {
      s->fail_accept_count--;
      fd = -1;
      errno = s->fail_accept_err;
    } else {
      fd = accept(s->listen_fd, (struct sockaddr *)&peer, &plen);
    }
    if (fd < 0) {
      if (!transient_accept_error(errno)) {
        char why[48];
        snprintf(why, sizeof(why), "accept failed (errno %d)", errno);
        rebind(s, now, false, true, why);
      }
      return;
    }
    for (int i = 0; i < PS5H_MAX_CONNS && !slot; i++) {
      if (s->conns[i].fd < 0) {
        slot = &s->conns[i];
      }
    }
    if (!slot) {
      close(fd); /* ninth connection */
      continue;
    }
    set_nonblock(fd);
    /* bounded kernel memory per connection; also makes slow readers
     * visible to the write deadline */
    {
      int sndbuf = PS5H_STREAM_CHUNK;
      setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    }
    slot->fd = fd;
    slot->peer = ntohl(peer.sin_addr.s_addr);
    slot->start_ms = now;
    slot->writing = false;
    slot->in_len = 0;
  }
}

/* Builds the answer into c->out (and c->file_fd for icons). */
static void respond(server *s, conn *c, int parsed, const http_req *req) {
  route_resp r;
  const char *body = NULL;
  size_t blen = 0, cap;
  char head[512];
  int hlen;

  if (parsed < 0) {
    route_parse_error(-parsed, &r);
  } else {
    s->ctx->mdns_running = mdns_running();
    route(s->ctx, req, c->peer, &r);
  }
  if (r.file_fd >= 0) {
    blen = (size_t)r.file_size;
  } else if (r.fixed) {
    body = r.fixed;
    blen = strlen(body);
  } else if (r.body.buf) {
    body = r.body.buf;
    blen = r.body.len;
  }
  hlen = http_head(head, sizeof(head), r.status, r.ctype, blen,
                   r.etag[0] ? r.etag : NULL);
  cap = (size_t)(hlen > 0 ? hlen : 0) + (body ? blen : 0);
  c->out = malloc(cap ? cap : 1);
  if (hlen < 0 || !c->out) {
    route_resp_free(&r);
    conn_close(c);
    return;
  }
  memcpy(c->out, head, (size_t)hlen);
  if (body) {
    memcpy(c->out + hlen, body, blen);
  }
  c->out_len = cap;
  c->out_off = 0;
  if (r.file_fd >= 0) {
    c->file_fd = r.file_fd;
    c->file_left = r.file_size;
    r.file_fd = -1; /* taken */
  }
  route_resp_free(&r);
  c->writing = true;
}

static void on_readable(server *s, conn *c) {
  http_req req;
  int parsed;
  ssize_t n = recv(c->fd, c->in + c->in_len, IN_CAP - 1 - c->in_len, 0);
  if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                 errno != EINTR)) {
    conn_close(c);
    return;
  }
  if (n < 0) {
    return;
  }
  c->in_len += (size_t)n;
  parsed = http_parse(c->in, c->in_len, &req);
  if (parsed == 0 && c->in_len >= IN_CAP - 1) {
    parsed = -431;
  }
  if (parsed != 0) {
    respond(s, c, parsed, &req);
  }
}

static void on_writable(conn *c) {
  for (;;) {
    const char *p;
    size_t left;
    ssize_t n;
    if (c->out_off < c->out_len) {
      p = c->out + c->out_off;
      left = c->out_len - c->out_off;
    } else if (c->chunk_off < c->chunk_len) {
      p = c->chunk + c->chunk_off;
      left = c->chunk_len - c->chunk_off;
    } else if (c->file_fd >= 0 && c->file_left > 0) {
      ssize_t r = read(c->file_fd, c->chunk, sizeof(c->chunk));
      if (r <= 0) {
        conn_close(c); /* file changed under us */
        return;
      }
      c->chunk_len = (size_t)r;
      c->chunk_off = 0;
      c->file_left -= r;
      continue;
    } else {
      shutdown(c->fd, SHUT_WR);
      conn_close(c);
      return;
    }
    n = send(c->fd, p, left, 0);
    if (n < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        conn_close(c);
      }
      return;
    }
    if (c->out_off < c->out_len) {
      c->out_off += (size_t)n;
    } else {
      c->chunk_off += (size_t)n;
    }
  }
}

int server_step(server *s, int timeout_ms) {
  struct pollfd pfd[PS5H_MAX_CONNS + 1];
  conn *who[PS5H_MAX_CONNS + 1];
  int64_t now = plat_mono_ms();
  int n = 0, rc;

  periodic(s, now);
  ensure_listener(s, now);
  deadlines(s, now);

  if (s->listen_fd >= 0) {
    pfd[n].fd = s->listen_fd;
    pfd[n].events = POLLIN;
    who[n++] = NULL;
  }
  for (int i = 0; i < PS5H_MAX_CONNS; i++) {
    conn *c = &s->conns[i];
    if (c->fd < 0) {
      continue;
    }
    pfd[n].fd = c->fd;
    pfd[n].events = c->writing ? POLLOUT : POLLIN;
    who[n++] = c;
  }
  rc = poll(pfd, (nfds_t)n, timeout_ms);
  if (rc <= 0) {
    return rc < 0 && errno != EINTR ? -1 : 0;
  }
  for (int i = 0; i < n; i++) {
    if (!pfd[i].revents) {
      continue;
    }
    if (!who[i]) {
      if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        rebind(s, now, false, true, "listener failed");
      } else {
        accept_all(s, now);
      }
      continue;
    }
    if (who[i]->fd < 0) {
      continue;
    }
    if (who[i]->writing) {
      on_writable(who[i]);
    } else {
      on_readable(s, who[i]);
    }
  }
  return 0;
}
