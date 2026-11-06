/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "json.h"
#include "log.h"
#include "platform.h"

#define MAX_TOKENS 4096

bool config_is_hex(const char *s, int n) {
  for (int i = 0; i < n; i++) {
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
      return false;
    }
  }
  return s[n] == '\0';
}

void config_iso_time(int64_t t, char out[21]) {
  time_t tt = (time_t)t;
  struct tm tm;
  if (!gmtime_r(&tt, &tm)) {
    snprintf(out, 21, "1970-01-01T00:00:00Z");
    return;
  }
  strftime(out, 21, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static int64_t parse_iso(const char *s) {
  struct tm tm;
  char z = 0;
  memset(&tm, 0, sizeof(tm));
  if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%c", &tm.tm_year, &tm.tm_mon,
             &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &z) != 7 ||
      z != 'Z') {
    return 0;
  }
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  return (int64_t)timegm(&tm);
}

int config_data_dir(char *out, size_t cap) {
  char data[600];
  snprintf(data, sizeof(data), "%s/data", plat_root());
  if (mkdir(data, 0755) && errno != EEXIST) {
    return -1;
  }
  if ((size_t)snprintf(out, cap, "%s%s", plat_root(), PS5H_DATA_DIR) >= cap) {
    return -1;
  }
  if (mkdir(out, 0700) && errno != EEXIST) {
    return -1;
  }
  return 0;
}

static int path_in_data(const char *name, char *out, size_t cap) {
  char dir[600];
  if (config_data_dir(dir, sizeof(dir))) {
    return -1;
  }
  return (size_t)snprintf(out, cap, "%s/%s", dir, name) >= cap ? -1 : 0;
}

/* Whole regular file (no symlink) up to max bytes; malloc'd, NUL-terminated.
 * errno ENOENT when missing. */
static char *read_small(const char *path, size_t max, size_t *len) {
  struct stat st;
  char *buf;
  ssize_t n;
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    return NULL;
  }
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (size_t)st.st_size > max ||
      !(buf = malloc((size_t)st.st_size + 1))) {
    close(fd);
    errno = EINVAL;
    return NULL;
  }
  n = read(fd, buf, (size_t)st.st_size);
  close(fd);
  if (n < 0) {
    free(buf);
    errno = EIO;
    return NULL;
  }
  buf[n] = '\0';
  *len = (size_t)n;
  return buf;
}

/* temp file + fsync + rename, mode 0600. */
static int write_atomic(const char *path, const char *data, size_t len) {
  char tmp[800];
  int fd;
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    return -1;
  }
  if (write(fd, data, len) != (ssize_t)len || fsync(fd)) {
    close(fd);
    unlink(tmp);
    return -1;
  }
  close(fd);
  if (rename(tmp, path)) {
    unlink(tmp);
    return -1;
  }
  return 0;
}

static void defaults(config *c) {
  memset(c, 0, sizeof(*c));
  c->port = PS5H_DEFAULT_PORT;
}

static bool parse_cidr(const char *s, cidr *out) {
  char ip[32];
  const char *slash = strchr(s, '/');
  long bits = 32;
  struct in_addr a;
  size_t n = slash ? (size_t)(slash - s) : strlen(s);
  if (n >= sizeof(ip)) {
    return false;
  }
  memcpy(ip, s, n);
  ip[n] = '\0';
  if (slash) {
    char *end;
    if (!slash[1]) {
      return false;
    }
    bits = strtol(slash + 1, &end, 10);
    if (*end || bits < 0 || bits > 32) {
      return false;
    }
  }
  if (inet_pton(AF_INET, ip, &a) != 1) {
    return false;
  }
  out->mask = bits ? 0xffffffffu << (32 - bits) : 0;
  out->net = ntohl(a.s_addr) & out->mask;
  return true;
}

static void load_allow(config *c, const jr_t *r, int t) {
  if (t < 0) {
    return;
  }
  if (r->tok[t].type != JSMN_ARRAY || r->tok[t].size > PS5H_MAX_ALLOW) {
    c->allow_broken = true;
    return;
  }
  for (int i = t + 1; i < r->ntok && r->tok[i].start < r->tok[t].end; i++) {
    char s[64];
    if (r->tok[i].parent != t) {
      continue;
    }
    if (!jr_string(r, i, s, sizeof(s)) || !parse_cidr(s, &c->allow[c->n_allow])) {
      c->allow_broken = true;
      return;
    }
    c->n_allow++;
  }
}

static void load_hosts(config *c, const jr_t *r, int t) {
  if (t < 0 || r->tok[t].type != JSMN_ARRAY) {
    return;
  }
  for (int i = t + 1; i < r->ntok && r->tok[i].start < r->tok[t].end; i++) {
    char *h;
    if (r->tok[i].parent != t || c->n_hosts >= PS5H_MAX_HOSTS) {
      continue;
    }
    h = c->hosts[c->n_hosts];
    if (jr_string(r, i, h, sizeof(c->hosts[0])) && h[0] && !strpbrk(h, " /:")) {
      c->n_hosts++;
    } else {
      log_error("config: ignoring invalid hosts entry");
    }
  }
}

static void load_tokens(config *c, const jr_t *r, int t) {
  if (t < 0 || r->tok[t].type != JSMN_ARRAY) {
    return;
  }
  for (int i = t + 1; i < r->ntok && r->tok[i].start < r->tok[t].end; i++) {
    token_slot *s = &c->tokens[c->n_tokens];
    char ts[32];
    if (r->tok[i].parent != t || c->n_tokens >= PS5H_MAX_TOKENS) {
      continue;
    }
    memset(s, 0, sizeof(*s));
    if (r->tok[i].type != JSMN_OBJECT ||
        !jr_string(r, jr_get(r, i, "token"), s->token, sizeof(s->token)) ||
        !config_is_hex(s->token, 64)) {
      log_error("config: dropping token entry with invalid token");
      continue;
    }
    jr_string(r, jr_get(r, i, "client_id"), s->client_id, sizeof(s->client_id));
    if (jr_string(r, jr_get(r, i, "label"), s->label, sizeof(s->label))) {
      char clean[sizeof(s->label)];
      utf8_sanitize(s->label, strlen(s->label), clean, sizeof(clean));
      memcpy(s->label, clean, sizeof(clean));
    }
    if (jr_string(r, jr_get(r, i, "created"), ts, sizeof(ts))) {
      s->created = parse_iso(ts);
    }
    if (jr_string(r, jr_get(r, i, "last_used"), ts, sizeof(ts))) {
      s->last_used = parse_iso(ts);
    }
    s->last_written = s->last_used;
    c->n_tokens++;
  }
}

static void set_aside(const char *path, config *c, const char *why) {
  char broken[800];
  snprintf(broken, sizeof(broken), "%s.broken-%lld", path,
           (long long)plat_wall_s());
  rename(path, broken);
  defaults(c);
  c->recovered = true;
  log_error("config: %s; moved to %s, pair again", why, broken);
  plat_notify("ps5-hass: config was broken, pair again");
}

int config_load(config *c) {
  static jsmntok_t tok[MAX_TOKENS];
  char path[700];
  size_t len;
  char *buf;
  jr_t r;
  int64_t v;
  bool b;

  defaults(c);
  if (path_in_data("config.json", path, sizeof(path))) {
    return -1;
  }
  if (!(buf = read_small(path, 65536, &len))) {
    if (errno != ENOENT) {
      set_aside(path, c, "unreadable");
    }
    return 0;
  }
  if (jr_parse(&r, buf, len, tok, MAX_TOKENS) || tok[0].type != JSMN_OBJECT) {
    free(buf);
    set_aside(path, c, "not valid JSON");
    return 0;
  }
  if (jr_int(&r, jr_get(&r, 0, "schema"), &v) && v != 1) {
    free(buf);
    set_aside(path, c, "unknown schema");
    return 0;
  }
  if (jr_int(&r, jr_get(&r, 0, "port"), &v)) {
    if (v > 0 && v < 65536) {
      c->port = (uint16_t)v;
    } else {
      log_error("config: invalid port, using %d", PS5H_DEFAULT_PORT);
    }
  }
  load_allow(c, &r, jr_get(&r, 0, "allow"));
  if (c->allow_broken) {
    c->n_allow = 0;
    log_error("config: invalid allow list, blocking every address");
  }
  load_hosts(c, &r, jr_get(&r, 0, "hosts"));
  c->power_actions = jr_bool(&r, jr_get(&r, 0, "power_actions"), &b) && b;
  load_tokens(c, &r, jr_get(&r, 0, "tokens"));
  free(buf);
  return 0;
}

int config_save(const config *c) {
  char path[700], s[64], ts[21];
  jw_t w;
  int rc;

  /* Saving would turn a broken allow list into a smaller (or empty, i.e.
   * open) one. Every request is refused in this state anyway. */
  if (c->allow_broken) {
    return -1;
  }
  if (path_in_data("config.json", path, sizeof(path))) {
    return -1;
  }
  jw_init(&w);
  jw_obj(&w);
  jw_key(&w, "schema");
  jw_int(&w, 1);
  jw_key(&w, "port");
  jw_int(&w, c->port);
  jw_key(&w, "allow");
  jw_arr(&w);
  for (int i = 0; i < c->n_allow; i++) {
    struct in_addr a;
    char ip[INET_ADDRSTRLEN];
    int bits = 0;
    for (uint32_t m = c->allow[i].mask; m; m <<= 1) {
      bits++;
    }
    a.s_addr = htonl(c->allow[i].net);
    inet_ntop(AF_INET, &a, ip, sizeof(ip));
    snprintf(s, sizeof(s), "%s/%d", ip, bits);
    jw_cstr(&w, s);
  }
  jw_end_arr(&w);
  jw_key(&w, "hosts");
  jw_arr(&w);
  for (int i = 0; i < c->n_hosts; i++) {
    jw_cstr(&w, c->hosts[i]);
  }
  jw_end_arr(&w);
  jw_key(&w, "power_actions");
  jw_bool(&w, c->power_actions);
  jw_key(&w, "tokens");
  jw_arr(&w);
  for (int i = 0; i < c->n_tokens; i++) {
    const token_slot *t = &c->tokens[i];
    jw_obj(&w);
    jw_key(&w, "token");
    jw_cstr(&w, t->token);
    jw_key(&w, "client_id");
    jw_cstr(&w, t->client_id);
    jw_key(&w, "label");
    jw_cstr(&w, t->label);
    jw_key(&w, "created");
    config_iso_time(t->created, ts);
    jw_cstr(&w, ts);
    jw_key(&w, "last_used");
    config_iso_time(t->last_used, ts);
    jw_cstr(&w, ts);
    jw_end_obj(&w);
  }
  jw_end_arr(&w);
  jw_end_obj(&w);
  if (w.failed) {
    jw_free(&w);
    return -1;
  }
  rc = write_atomic(path, w.buf, w.len);
  jw_free(&w);
  if (rc) {
    log_error("config: could not write %s", path);
  }
  return rc;
}

int device_id_get(char out[33]) {
  char path[700];
  uint8_t raw[16];
  size_t len;
  char *buf;
  if (path_in_data("device_id", path, sizeof(path))) {
    return -1;
  }
  if ((buf = read_small(path, 64, &len))) {
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
      buf[--len] = '\0';
    }
    if (config_is_hex(buf, 32)) {
      memcpy(out, buf, 33);
      free(buf);
      return 0;
    }
    free(buf);
    log_error("device_id: invalid file, creating a new id");
  }
  if (plat_random(raw, sizeof(raw)) < 0) {
    return -1;
  }
  for (int i = 0; i < 16; i++) {
    snprintf(out + 2 * i, 3, "%02x", raw[i]);
  }
  return write_atomic(path, out, 32);
}
