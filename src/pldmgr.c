/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pldmgr.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "json.h"
#include "limits.h"
#include "log.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define COMM_LEN 19 /* FreeBSD ki_comm */
#define PATH_MAX_REQ 512

static struct {
  char ip[16];
  uint16_t port;
  bool ok;
} target = {"127.0.0.1", PS5H_PLDMGR_PORT, false};

void pldmgr_set_target(const char *ip, uint16_t port) {
  snprintf(target.ip, sizeof(target.ip), "%s", ip);
  target.port = port;
}

bool pldmgr_last_ok(void) { return target.ok; }

/* --- names --------------------------------------------------------------- */

static bool contains_ci(const char *s, const char *needle) {
  size_t n = strlen(needle);
  for (; s && *s; s++) {
    if (!strncasecmp(s, needle, n)) {
      return true;
    }
  }
  return false;
}

bool pldmgr_is_protected(const char *s) {
  static const char *const names[] = {"kstuff", "etahen", "elfldr", "pldmgr",
                                      "ps5-hass"};
  for (size_t i = 0; s && i < sizeof(names) / sizeof(names[0]); i++) {
    if (contains_ci(s, names[i])) {
      return true;
    }
  }
  return false;
}

static bool ends_with_ci(const char *s, size_t n, const char *suffix) {
  size_t k = strlen(suffix);
  return n >= k && !strncasecmp(s + n - k, suffix, k);
}

void pldmgr_base_name(const char *slot, char out[64]) {
  const char *p = slot ? strrchr(slot, '/') : NULL, *at;
  size_t n;
  p = p ? p + 1 : (slot ? slot : "");
  n = strlen(p);
  at = memchr(p, '@', n);
  if (at) { /* a slot "<id>@<hash>" */
    n = (size_t)(at - p);
  } else if (ends_with_ci(p, n, ".elf") || ends_with_ci(p, n, ".bin")) {
    n -= 4;
  }
  /* version part: from the last "_" when a digit or "v" + digit follows */
  for (size_t i = n; i > 0; i--) {
    if (p[i - 1] == '_') {
      const char *v = p + i;
      if (i < n && ((v[0] >= '0' && v[0] <= '9') ||
                    (v[0] == 'v' && i + 1 < n && v[1] >= '0' && v[1] <= '9'))) {
        n = i - 1;
      }
      break;
    }
  }
  if (n > 63) {
    n = 63;
  }
  memcpy(out, p, n);
  out[n] = '\0';
}

static bool same_comm(const char *proc, const char *key) {
  size_t n = strlen(proc);
  if (!*key) {
    return false;
  }
  if (ends_with_ci(proc, n, ".elf")) {
    char tmp[COMM_LEN + 5];
    n -= 4;
    if (n > COMM_LEN) {
      n = COMM_LEN;
    }
    memcpy(tmp, proc, n);
    tmp[n] = '\0';
    return !strncasecmp(tmp, key, COMM_LEN);
  }
  return !strncasecmp(proc, key, COMM_LEN);
}

bool pldmgr_process_matches(const char *proc, const char *id,
                            const char *filename) {
  char base[64];
  if (id && same_comm(proc, id)) {
    return true;
  }
  if (filename && *filename) {
    pldmgr_base_name(filename, base);
    return same_comm(proc, base);
  }
  return false;
}

/* --- HTTP client: one request, non-blocking, one deadline ---------------- */

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_fd(int fd, short ev, int64_t deadline) {
  for (;;) {
    int64_t left = deadline - now_ms();
    struct pollfd p = {fd, ev, 0};
    int rc;
    if (left <= 0) {
      return -ETIMEDOUT;
    }
    rc = poll(&p, 1, (int)left);
    if (rc > 0) {
      return 0;
    }
    if (rc == 0) {
      return -ETIMEDOUT;
    }
    if (errno != EINTR) {
      return -errno;
    }
  }
}

static int dial(int64_t deadline) {
  struct sockaddr_in a;
  int fd, err = 0, rc;
  socklen_t l = sizeof(err);
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(target.port);
  if (inet_pton(AF_INET, target.ip, &a.sin_addr) != 1) {
    return -EINVAL;
  }
  if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    return -errno;
  }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &(int){1}, sizeof(int));
#endif
  if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
    if (errno != EINPROGRESS && errno != EINTR) {
      rc = -errno;
      close(fd);
      return rc;
    }
    if ((rc = wait_fd(fd, POLLOUT, deadline)) < 0) {
      close(fd);
      return rc;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) < 0 || err) {
      close(fd);
      return err ? -err : -EIO;
    }
  }
  return fd;
}

typedef struct {
  char *buf; /* head + body + NUL, PS5H_PLDMGR_MAX_ANSWER + 1 bytes */
  char *body;
  size_t body_len;
  int status;
} answer;

static void answer_free(answer *a) {
  free(a->buf);
  memset(a, 0, sizeof(*a));
}

/* Value of a header in the head (NUL-terminated up to the blank line). */
static const char *header(const char *head, const char *name, size_t *vlen) {
  size_t n = strlen(name);
  const char *p = strstr(head, "\r\n");
  while (p) {
    p += 2;
    if (!strncasecmp(p, name, n) && p[n] == ':') {
      const char *v = p + n + 1, *e;
      while (*v == ' ' || *v == '\t') {
        v++;
      }
      e = strstr(v, "\r\n");
      *vlen = e ? (size_t)(e - v) : strlen(v);
      return v;
    }
    p = strstr(p, "\r\n");
  }
  return NULL;
}

/* Reads until the answer is complete. 0 = ok. */
static int read_answer(int fd, int64_t deadline, answer *a) {
  size_t len = 0, head_len = 0;
  bool have_head = false, have_len = false;
  size_t want = 0;
  a->buf = malloc(PS5H_PLDMGR_MAX_ANSWER + 1);
  if (!a->buf) {
    return -ENOMEM;
  }
  for (;;) {
    ssize_t k;
    int rc;
    if (have_head && have_len && len >= head_len + want) {
      break; /* complete; do not wait for the close */
    }
    if (len >= PS5H_PLDMGR_MAX_ANSWER) {
      return -EMSGSIZE;
    }
    if ((rc = wait_fd(fd, POLLIN, deadline)) < 0) {
      return rc;
    }
    k = recv(fd, a->buf + len, PS5H_PLDMGR_MAX_ANSWER - len, 0);
    if (k < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      return -errno;
    }
    if (k == 0) {
      break;
    }
    len += (size_t)k;
    a->buf[len] = '\0';
    if (!have_head) {
      char *end = strstr(a->buf, "\r\n\r\n");
      const char *v;
      size_t vl;
      if (!end) {
        continue;
      }
      head_len = (size_t)(end - a->buf) + 4;
      have_head = true;
      if (strncmp(a->buf, "HTTP/1.", 7) || a->buf[8] != ' ' ||
          a->buf[9] < '1' || a->buf[9] > '5') {
        return -EPROTO;
      }
      a->status = atoi(a->buf + 9);
      *end = '\0'; /* head as a C string for header() */
      if (header(a->buf, "Transfer-Encoding", &vl)) {
        return -EPROTO; /* we ask for plain bodies; chunked is not handled */
      }
      if ((v = header(a->buf, "Content-Length", &vl))) {
        char num[16];
        char *e2;
        unsigned long long cl;
        if (vl == 0 || vl >= sizeof(num)) {
          return -EPROTO;
        }
        memcpy(num, v, vl);
        num[vl] = '\0';
        cl = strtoull(num, &e2, 10);
        if (*e2) {
          return -EPROTO;
        }
        if (cl > PS5H_PLDMGR_MAX_ANSWER) {
          return -EMSGSIZE;
        }
        want = (size_t)cl;
        have_len = true;
      }
      *end = '\r'; /* restore for the length arithmetic */
    }
  }
  if (!have_head) {
    return -EPROTO;
  }
  if (have_len && len < head_len + want) {
    return -EPROTO; /* closed early */
  }
  a->body = a->buf + head_len;
  a->body_len = have_len ? want : len - head_len;
  a->body[a->body_len] = '\0';
  return 0;
}

/* One request, all of it inside `deadline`. 0 = got an answer (any status). */
static int http_call(const char *method, const char *path, int64_t deadline,
                     answer *a) {
  char req[PATH_MAX_REQ + 256];
  size_t off = 0, n;
  int fd, rc;
  memset(a, 0, sizeof(*a));
  n = (size_t)snprintf(req, sizeof(req),
                       "%s %s HTTP/1.1\r\nHost: %s:%u\r\n"
                       "X-PLDMGR-Request: 1\r\nConnection: close\r\n%s\r\n",
                       method, path, target.ip, (unsigned)target.port,
                       strcmp(method, "POST") ? "" : "Content-Length: 0\r\n");
  if (n >= sizeof(req)) {
    return -EINVAL;
  }
  if ((fd = dial(deadline)) < 0) {
    return fd;
  }
  while (off < n) {
    ssize_t k;
    if ((rc = wait_fd(fd, POLLOUT, deadline)) < 0) {
      goto out;
    }
    k = send(fd, req + off, n - off, MSG_NOSIGNAL);
    if (k < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      rc = -errno;
      goto out;
    }
    off += (size_t)k;
  }
  rc = read_answer(fd, deadline, a);
out:
  close(fd);
  if (rc < 0) {
    answer_free(a);
  }
  return rc;
}

/* --- list ---------------------------------------------------------------- */

typedef struct {
  int pid;
  char name[64];
} proc_t;

static bool tok_str(const jr_t *r, int obj, const char *key, char *out,
                    size_t cap) {
  out[0] = '\0';
  return jr_string(r, jr_get(r, obj, key), out, cap);
}

/* Process list: "processes":[{"pid":N,"name":"..."}]. */
static int read_procs(int64_t deadline, proc_t *out, int max, jsmntok_t *tok) {
  answer a;
  jr_t r;
  int arr, n = 0, rc = http_call("GET", "/processes_list", deadline, &a);
  if (rc < 0) {
    return rc;
  }
  if (a.status != 200) {
    answer_free(&a);
    return -EIO;
  }
  if (jr_parse(&r, a.body, a.body_len, tok, PS5H_PLDMGR_MAX_TOKENS) ||
      (arr = jr_get(&r, 0, "processes")) < 0 || tok[arr].type != JSMN_ARRAY) {
    answer_free(&a);
    return -EPROTO;
  }
  for (int i = arr + 1; i < r.ntok && n < max; i++) {
    int64_t pid;
    if (tok[i].parent != arr || tok[i].type != JSMN_OBJECT) {
      continue;
    }
    if (jr_int(&r, jr_get(&r, i, "pid"), &pid) && pid > 0 && pid < 0x7fffffff &&
        tok_str(&r, i, "name", out[n].name, sizeof(out[n].name))) {
      out[n].pid = (int)pid;
      n++;
    }
  }
  answer_free(&a);
  return n;
}

/* Builds one entry from the i-th item; false = not usable. */
static bool make_entry(const jr_t *r, int i, const proc_t *procs, int nproc,
                       pld_entry *e) {
  char slot[256], id[128], file[256], name[128];
  tok_str(r, i, "id", id, sizeof(id));
  tok_str(r, i, "filename", file, sizeof(file));
  tok_str(r, i, "name", name, sizeof(name));
  if (!tok_str(r, i, "slot", slot, sizeof(slot)) || !slot[0]) {
    return false;
  }
  memset(e, 0, sizeof(*e));
  snprintf(e->slot, sizeof(e->slot), "%s", slot);
  if (name[0] || id[0]) {
    const char *s = name[0] ? name : id;
    utf8_sanitize(s, strlen(s), e->name, sizeof(e->name));
  } else {
    char base[64];
    pldmgr_base_name(file[0] ? file : slot, base);
    utf8_sanitize(base, strlen(base), e->name, sizeof(e->name));
  }
  e->protected_ = pldmgr_is_protected(slot) || pldmgr_is_protected(id) ||
                  pldmgr_is_protected(file) || pldmgr_is_protected(name);
  for (int k = 0; k < nproc; k++) {
    if (pldmgr_process_matches(procs[k].name, id[0] ? id : NULL,
                               file[0] ? file : slot)) {
      e->running = true;
      e->pid = procs[k].pid;
      if (pldmgr_is_protected(procs[k].name)) {
        e->protected_ = true;
      }
      break;
    }
  }
  return true;
}

static int list_all(pld_entry *out, int max) {
  answer a;
  jr_t r;
  jsmntok_t *tok = malloc(PS5H_PLDMGR_MAX_TOKENS * sizeof(*tok));
  proc_t *procs = malloc(PS5H_MAX_PROCS * sizeof(*procs));
  int64_t deadline = now_ms() + PS5H_PLDMGR_TIMEOUT_MS;
  int arr, n = 0, nproc, rc;
  if (!tok || !procs) {
    free(tok);
    free(procs);
    return -ENOMEM;
  }
  if ((rc = nproc = read_procs(deadline, procs, PS5H_MAX_PROCS, tok)) < 0 ||
      (rc = http_call("GET", "/list_payloads", deadline, &a)) < 0) {
    goto fail;
  }
  if (a.status != 200) {
    answer_free(&a);
    rc = -EIO;
    goto fail;
  }
  if (jr_parse(&r, a.body, a.body_len, tok, PS5H_PLDMGR_MAX_TOKENS) ||
      (arr = jr_get(&r, 0, "items")) < 0 || tok[arr].type != JSMN_ARRAY) {
    answer_free(&a);
    rc = -EPROTO;
    goto fail;
  }
  for (int i = arr + 1; i < r.ntok && n < max; i++) {
    if (tok[i].parent == arr && tok[i].type == JSMN_OBJECT &&
        make_entry(&r, i, procs, nproc, &out[n])) {
      n++;
    }
  }
  answer_free(&a);
  free(tok);
  free(procs);
  target.ok = true;
  return n;
fail:
  free(tok);
  free(procs);
  target.ok = false;
  log_msg("payload manager: %s", strerror(-rc));
  return rc;
}

int pldmgr_list(pld_entry *out, int max) {
  return max > 0 ? list_all(out, max) : -EINVAL;
}

/* --- start / stop ---------------------------------------------------------- */

static const pld_entry *find_slot(const pld_entry *e, int n, const char *slot) {
  for (int i = 0; i < n; i++) {
    if (!strcmp(e[i].slot, slot)) {
      return &e[i];
    }
  }
  return NULL;
}

/* %-encodes everything but unreserved characters and "/". */
static bool encode_slot(const char *slot, char *out, size_t cap) {
  size_t o = 0;
  for (; *slot; slot++) {
    unsigned char c = (unsigned char)*slot;
    bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                 c == '~' || c == '/';
    if (o + (plain ? 1 : 3) >= cap) {
      return false;
    }
    if (plain) {
      out[o++] = (char)c;
    } else {
      o += (size_t)snprintf(out + o, cap - o, "%%%02X", c);
    }
  }
  out[o] = '\0';
  return true;
}

/* Looks the slot up (one list call) and applies the protection and state
 * checks; on 0 *e is the entry. */
static int resolve(const char *slot, bool want_running, pld_entry *e) {
  pld_entry *all;
  const pld_entry *x;
  int n, rc = 0;
  if (!slot || !*slot || strlen(slot) >= sizeof(e->slot)) {
    return -EINVAL;
  }
  if (pldmgr_is_protected(slot)) {
    return -EPERM; /* nothing is sent */
  }
  if (!(all = malloc(PS5H_PLDMGR_MAX_ENTRIES * sizeof(*all)))) {
    return -ENOMEM;
  }
  if ((n = list_all(all, PS5H_PLDMGR_MAX_ENTRIES)) < 0) {
    rc = n;
  } else if (!(x = find_slot(all, n, slot))) {
    rc = -ESRCH;
  } else if (x->protected_) {
    rc = -EPERM;
  } else if (want_running != x->running) {
    rc = want_running ? -ENOENT : -EEXIST;
  } else {
    *e = *x;
  }
  free(all);
  return rc;
}

static int act(const char *path) {
  answer a;
  int rc = http_call("POST", path, now_ms() + PS5H_PLDMGR_TIMEOUT_MS, &a);
  if (rc < 0) {
    target.ok = false;
    return rc;
  }
  rc = a.status == 200 ? 0 : -EIO;
  answer_free(&a);
  return rc;
}

int pldmgr_start(const char *slot) {
  pld_entry e;
  char enc[PATH_MAX_REQ - 32], path[PATH_MAX_REQ];
  int rc = resolve(slot, false, &e);
  if (rc < 0) {
    return rc;
  }
  if (!encode_slot(e.slot, enc, sizeof(enc))) {
    return -EINVAL;
  }
  snprintf(path, sizeof(path), "/loadpayload:%s", enc);
  rc = act(path);
  log_msg("start payload %s: %s", e.name, rc ? strerror(-rc) : "ok");
  return rc;
}

int pldmgr_stop(const char *slot) {
  pld_entry e;
  char path[64];
  int rc = resolve(slot, true, &e);
  if (rc < 0) {
    return rc;
  }
  snprintf(path, sizeof(path), "/process_kill?pid=%d", e.pid);
  rc = act(path);
  log_msg("stop payload %s: %s", e.name, rc ? strerror(-rc) : "ok");
  return rc;
}
