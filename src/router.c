/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "router.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "auth.h"
#include "control.h"
#include "limits.h"
#include "log.h"
#include "mdns.h"
#include "state.h"

#define PREFIX "/api/v1"

static const char INTERNAL[] =
    "{\"result\":\"error\",\"error\":\"internal\","
    "\"message\":\"could not build the answer\"}";

/* --- answers ----------------------------------------------------------- */

static void reset(route_resp *out) {
  jw_free(&out->body);
  jw_init(&out->body);
  out->fixed = NULL;
  out->ctype = "application/json";
  if (out->file_fd >= 0) {
    close(out->file_fd);
  }
  out->file_fd = -1;
  out->file_size = 0;
  out->etag[0] = '\0';
}

/* Opens {"result":"ok" ... ; caller adds fields and calls done(). */
static jw_t *ok(route_resp *out) {
  reset(out);
  out->status = 200;
  jw_obj(&out->body);
  jw_key(&out->body, "result");
  jw_cstr(&out->body, "ok");
  return &out->body;
}

/* Opens {"result":"error","error":code,"message":msg ... */
static jw_t *err(route_resp *out, int status, const char *code,
                 const char *msg) {
  reset(out);
  out->status = status;
  jw_obj(&out->body);
  jw_key(&out->body, "result");
  jw_cstr(&out->body, "error");
  jw_key(&out->body, "error");
  jw_cstr(&out->body, code);
  jw_key(&out->body, "message");
  jw_cstr(&out->body, msg);
  return &out->body;
}

static void done(route_resp *out) {
  jw_end_obj(&out->body);
  if (out->body.failed) {
    reset(out);
    out->status = 500;
    out->fixed = INTERNAL;
  }
}

static void fail(route_resp *out, int status, const char *code,
                 const char *msg) {
  err(out, status, code, msg);
  done(out);
}

static void fail_retry(route_resp *out, int status, const char *code,
                       const char *msg, int retry_after) {
  jw_t *w = err(out, status, code, msg);
  jw_key(w, "retry_after");
  jw_int(w, retry_after);
  done(out);
}

static void system_error(route_resp *out, int rc) {
  char hex[16];
  jw_t *w = err(out, 500, "system_error", "the console reported an error");
  snprintf(hex, sizeof(hex), "0x%08x", (unsigned)rc);
  jw_key(w, "rc");
  jw_int(w, rc);
  jw_key(w, "rc_hex");
  jw_cstr(w, hex);
  done(out);
}

static void not_modified(route_resp *out, const char *etag) {
  reset(out);
  out->status = 304;
  out->ctype = NULL;
  snprintf(out->etag, sizeof(out->etag), "%s", etag);
}

void route_resp_free(route_resp *r) {
  jw_free(&r->body);
  if (r->file_fd >= 0) {
    close(r->file_fd);
  }
  r->file_fd = -1;
}

void route_parse_error(int status, route_resp *out) {
  memset(out, 0, sizeof(*out));
  out->file_fd = -1;
  switch (status) {
  case 411:
    fail(out, 411, "length_required", "send Content-Length, no chunked body");
    break;
  case 413:
    fail(out, 413, "body_too_large", "the body is larger than 4096 bytes");
    break;
  case 431:
    fail(out, 431, "headers_too_large", "the headers are larger than 8 KB");
    break;
  default:
    fail(out, 400, "bad_request", "malformed request");
  }
}

/* --- guards ------------------------------------------------------------ */

static bool host_ok(const config *c, const char *host) {
  char name[256];
  struct in_addr a;
  size_t n = strlen(host);
  const char *colon = strrchr(host, ':');
  /* strip :port */
  if (colon && colon[1]) {
    bool digits = true;
    for (const char *p = colon + 1; *p; p++) {
      digits &= *p >= '0' && *p <= '9';
    }
    if (digits) {
      n = (size_t)(colon - host);
    }
  }
  if (!n || n >= sizeof(name)) {
    return false;
  }
  memcpy(name, host, n);
  name[n] = '\0';
  if (inet_pton(AF_INET, name, &a) == 1) {
    return true;
  }
  if (n > 6 && !strcasecmp(name + n - 6, ".local")) {
    return true;
  }
  for (int i = 0; i < c->n_hosts; i++) {
    if (!strcasecmp(name, c->hosts[i])) {
      return true;
    }
  }
  return false;
}

static bool address_ok(const config *c, uint32_t peer) {
  if (c->allow_broken) {
    return false;
  }
  if (!c->n_allow) {
    return true;
  }
  for (int i = 0; i < c->n_allow; i++) {
    if ((peer & c->allow[i].mask) == c->allow[i].net) {
      return true;
    }
  }
  return false;
}

static bool json_ctype(const char *ct) {
  size_t n = strlen("application/json");
  return !strncasecmp(ct, "application/json", n) &&
         (ct[n] == '\0' || ct[n] == ';' || ct[n] == ' ');
}

/* --- routes ------------------------------------------------------------ */

typedef enum {
  R_NONE,
  R_INFO,
  R_PAIR_START,
  R_PAIR_FINISH,
  R_PAIR_REVOKE,
  R_STATE,
  R_APPS,
  R_ICON,
  R_LAUNCH,
  R_CLOSE,
  R_STANDBY
} route_id;

/* Fills id (the <ID> path segment) for icon/launch. */
static route_id match(const char *path, http_method *want, char *id,
                      size_t idcap) {
  static const struct {
    const char *path;
    http_method method;
    route_id r;
  } fixed[] = {
      {PREFIX "/info", HM_GET, R_INFO},
      {PREFIX "/pair/start", HM_POST, R_PAIR_START},
      {PREFIX "/pair/finish", HM_POST, R_PAIR_FINISH},
      {PREFIX "/pair/revoke", HM_POST, R_PAIR_REVOKE},
      {PREFIX "/state", HM_GET, R_STATE},
      {PREFIX "/apps", HM_GET, R_APPS},
      {PREFIX "/apps/running/close", HM_POST, R_CLOSE},
      {PREFIX "/power/standby", HM_POST, R_STANDBY},
  };
  const char *apps = PREFIX "/apps/";
  size_t alen = strlen(apps);
  for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
    if (!strcmp(path, fixed[i].path)) {
      *want = fixed[i].method;
      return fixed[i].r;
    }
  }
  if (!strncmp(path, apps, alen)) {
    const char *seg = path + alen;
    const char *slash = strchr(seg, '/');
    size_t n;
    if (!slash || slash == seg) {
      return R_NONE;
    }
    n = (size_t)(slash - seg);
    if (n >= idcap) {
      n = idcap - 1; /* too long: fails the id check later */
    }
    memcpy(id, seg, n);
    id[n] = '\0';
    if (!strcmp(slash, "/icon")) {
      *want = HM_GET;
      return R_ICON;
    }
    if (!strcmp(slash, "/launch")) {
      *want = HM_POST;
      return R_LAUNCH;
    }
  }
  return R_NONE;
}

static bool needs_token(route_id r) {
  return r != R_INFO && r != R_PAIR_START && r != R_PAIR_FINISH;
}

/* --- handlers ---------------------------------------------------------- */

static void h_info(route_ctx *ctx, route_resp *out) {
  info_json(ok(out), ctx->cfg, ctx->device_id, ctx->mdns_running);
  done(out);
}

static void h_pair_start(route_resp *out) {
  int expires_in = 0, retry = 0;
  jw_t *w;
  switch (auth_pair_start(&expires_in, &retry)) {
  case AUTH_OK:
    w = ok(out);
    jw_key(w, "expires_in");
    jw_int(w, expires_in);
    done(out);
    return;
  case AUTH_RATE_LIMITED:
    fail_retry(out, 429, "rate_limited", "wait before asking for a new code",
               retry);
    return;
  case AUTH_LOCKED:
    fail_retry(out, 429, "locked", "too many wrong codes", retry);
    return;
  case AUTH_NOTIFY_FAILED:
    fail(out, 500, "notify_failed", "could not show the code on the TV");
    return;
  default:
    fail(out, 500, "random_unavailable", "no randomness source on the console");
  }
}

static void h_pair_finish(route_resp *out, const jr_t *r) {
  char code[16], client[64], label[PS5H_MAX_LABEL * 4 + 8], token[65];
  int left = 0, retry = 0;
  jw_t *w;
  if (!jr_string(r, jr_get(r, 0, "code"), code, sizeof(code)) ||
      !jr_string(r, jr_get(r, 0, "client_id"), client, sizeof(client)) ||
      !jr_string(r, jr_get(r, 0, "label"), label, sizeof(label))) {
    fail(out, 400, "bad_request", "code, client_id and label are required");
    return;
  }
  switch (auth_pair_finish(code, client, label, token, &left, &retry)) {
  case AUTH_OK:
    w = ok(out);
    jw_key(w, "token");
    jw_cstr(w, token);
    done(out);
    mdns_reannounce(true);
    return;
  case AUTH_WRONG_CODE:
    w = err(out, 401, "wrong_code", "the code does not match");
    jw_key(w, "attempts_left");
    jw_int(w, left);
    done(out);
    return;
  case AUTH_NO_CODE:
    fail(out, 409, "no_code", "no active code, start pairing first");
    return;
  case AUTH_CODE_EXHAUSTED:
    fail(out, 409, "code_exhausted", "too many wrong attempts, start again");
    return;
  case AUTH_LOCKED:
    fail_retry(out, 429, "locked", "too many wrong codes", retry);
    return;
  case AUTH_TOKEN_LIMIT:
    fail(out, 409, "token_limit", "all 8 pairing slots are used");
    return;
  case AUTH_BAD_INPUT:
    fail(out, 400, "bad_request",
         "code: 6 digits, client_id: lowercase UUID, label: 1-64 characters");
    return;
  case AUTH_RANDOM_UNAVAILABLE:
    fail(out, 500, "random_unavailable", "no randomness source on the console");
    return;
  default:
    fail(out, 500, "internal", "could not save the pairing");
  }
}

static void h_revoke(route_resp *out, int slot) {
  if (auth_revoke(slot) != AUTH_OK) {
    fail(out, 500, "internal", "could not save the change");
    return;
  }
  ok(out);
  done(out);
  mdns_reannounce(auth_paired());
}

static void h_state(route_ctx *ctx, route_resp *out) {
  apps_refresh(ctx->apps);
  state_json(ok(out), ctx->apps, ctx->start_mono_ms);
  done(out);
}

static void h_apps(route_ctx *ctx, const http_req *req, route_resp *out) {
  char etag[24];
  jw_t *w;
  apps_refresh(ctx->apps);
  snprintf(etag, sizeof(etag), "\"%s\"", ctx->apps->etag);
  if (!strcmp(req->if_none_match, etag)) {
    not_modified(out, etag);
    return;
  }
  w = ok(out);
  jw_key(w, "etag");
  jw_cstr(w, ctx->apps->etag);
  jw_key(w, "apps");
  jw_arr(w);
  for (int i = 0; i < ctx->apps->count; i++) {
    const app_t *a = &ctx->apps->items[i];
    jw_obj(w);
    jw_key(w, "title_id");
    jw_cstr(w, a->title_id);
    jw_key(w, "name");
    jw_cstr(w, a->name);
    jw_key(w, "version");
    if (a->version[0]) {
      jw_cstr(w, a->version);
    } else {
      jw_null(w);
    }
    jw_key(w, "kind");
    jw_cstr(w, a->game ? "game" : "app");
    jw_key(w, "category");
    jw_opt_int(w, a->has_category, a->category);
    jw_key(w, "has_icon");
    jw_bool(w, a->icon != ICON_NONE);
    jw_end_obj(w);
  }
  jw_end_arr(w);
  done(out);
  if (out->status == 200) {
    snprintf(out->etag, sizeof(out->etag), "%s", etag);
  }
}

static void h_icon(route_ctx *ctx, const http_req *req, const char *id,
                   route_resp *out) {
  const app_t *a;
  char etag[48];
  struct stat st;
  int fd;
  apps_refresh(ctx->apps);
  if (!(a = apps_find(ctx->apps, id))) {
    fail(out, 404, "not_installed", "no such app");
    return;
  }
  if (a->icon == ICON_NONE) {
    fail(out, 404, "not_found", "the app has no icon");
    return;
  }
  snprintf(etag, sizeof(etag), "\"%llx-%llx\"", (unsigned long long)a->icon_mtime,
           (unsigned long long)a->icon_size);
  if (!strcmp(req->if_none_match, etag)) {
    not_modified(out, etag);
    return;
  }
  fd = open(a->icon_path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
      st.st_size > PS5H_ICON_MAX) {
    if (fd >= 0) {
      close(fd);
    }
    fail(out, 404, "not_found", "the icon cannot be read");
    return;
  }
  reset(out);
  out->status = 200;
  out->ctype = a->icon == ICON_PNG ? "image/png" : "image/jpeg";
  out->file_fd = fd;
  out->file_size = (int64_t)st.st_size;
  snprintf(out->etag, sizeof(out->etag), "%s", etag);
}

static void h_launch(route_ctx *ctx, const jr_t *r, const char *id,
                     route_resp *out) {
  bool replace = false;
  int tok = jr_get(r, 0, "replace_running"), rc = 0;
  jw_t *w;
  if (tok >= 0 && !jr_bool(r, tok, &replace)) {
    fail(out, 400, "bad_request", "replace_running must be true or false");
    return;
  }
  apps_refresh(ctx->apps);
  switch (control_launch(ctx->apps, id, replace, &rc)) {
  case CTL_OK:
    w = ok(out);
    jw_key(w, "already_running");
    jw_bool(w, false);
    jw_key(w, "app_id");
    jw_int(w, rc);
    done(out);
    return;
  case CTL_ALREADY_RUNNING:
    w = ok(out);
    jw_key(w, "already_running");
    jw_bool(w, true);
    done(out);
    return;
  case CTL_OTHER_APP_RUNNING:
    fail(out, 409, "other_app_running",
         "another app runs; send replace_running: true");
    return;
  case CTL_NOT_INSTALLED:
    fail(out, 404, "not_installed", "no such app");
    return;
  case CTL_NO_USER:
    fail(out, 409, "no_user", "no user is logged in on the console");
    return;
  default:
    system_error(out, rc);
  }
}

static void h_close(const jr_t *r, route_resp *out) {
  char want[16];
  int tok = jr_get(r, 0, "title_id"), rc = 0;
  bool has = tok >= 0;
  if (has && (!jr_string(r, tok, want, sizeof(want)) || !apps_valid_id(want))) {
    fail(out, 400, "bad_title_id", "title_id must look like PPSA12345");
    return;
  }
  switch (control_close(has ? want : NULL, &rc)) {
  case CTL_OK:
    ok(out);
    done(out);
    return;
  case CTL_NO_APP:
    fail(out, 409, "no_app", "no app is running");
    return;
  case CTL_TITLE_MISMATCH:
    fail(out, 409, "title_mismatch", "a different app is running");
    return;
  default:
    system_error(out, rc);
  }
}

static void h_standby(route_ctx *ctx, route_resp *out) {
  int rc = 0;
  switch (control_standby(ctx->cfg, &rc)) {
  case CTL_OK:
    ok(out);
    done(out);
    return;
  case CTL_DISABLED:
    fail(out, 403, "power_actions_disabled",
         "set power_actions to true in config.json");
    return;
  case CTL_RATE_LIMITED:
    fail_retry(out, 429, "rate_limited", "at most one request per 10 s", rc);
    return;
  default:
    system_error(out, rc);
  }
}

void route(route_ctx *ctx, const http_req *req, uint32_t peer,
           route_resp *out) {
  static jsmntok_t tok[256];
  http_method want = HM_GET;
  char id[16] = "";
  route_id r;
  jr_t body;
  int slot = -1;

  memset(out, 0, sizeof(*out));
  out->file_fd = -1;
  out->ctype = "application/json";

  /* 1-4: browser guard, Host, allow list -- before anything else */
  if (req->has_origin) {
    fail(out, 403, "origin_forbidden", "requests from browsers are refused");
    return;
  }
  if (req->method == HM_POST &&
      (!json_ctype(req->content_type) || !req->has_marker)) {
    fail(out, 403, "browser_guard",
         "POST needs Content-Type: application/json and X-PS5-Hass: 1");
    return;
  }
  if (!host_ok(ctx->cfg, req->host)) {
    fail(out, 403, "host_forbidden",
         "Host must be an IP address, a .local name or listed in hosts");
    return;
  }
  if (!address_ok(ctx->cfg, peer)) {
    fail(out, 403, "address_forbidden", "this address is not allowed");
    return;
  }

  /* 5: route */
  if ((r = match(req->path, &want, id, sizeof(id))) == R_NONE) {
    fail(out, 404, "not_found", "unknown path");
    return;
  }
  if (req->method != want) {
    fail(out, 405, "method_not_allowed",
         want == HM_GET ? "use GET" : "use POST");
    return;
  }

  /* 6: token */
  if (needs_token(r) && (slot = auth_check(req->authorization)) < 0) {
    fail(out, 401, "unauthorized", "missing or invalid token");
    return;
  }

  /* 7: input */
  if ((r == R_ICON || r == R_LAUNCH) && !apps_valid_id(id)) {
    fail(out, 400, "bad_title_id", "title id must look like PPSA12345");
    return;
  }
  if (req->method == HM_POST &&
      (jr_parse(&body, req->body, req->body_len, tok, 256) ||
       tok[0].type != JSMN_OBJECT)) {
    fail(out, 400, "bad_request", "the body must be a JSON object");
    return;
  }

  switch (r) {
  case R_INFO: h_info(ctx, out); break;
  case R_PAIR_START: h_pair_start(out); break;
  case R_PAIR_FINISH: h_pair_finish(out, &body); break;
  case R_PAIR_REVOKE: h_revoke(out, slot); break;
  case R_STATE: h_state(ctx, out); break;
  case R_APPS: h_apps(ctx, req, out); break;
  case R_ICON: h_icon(ctx, req, id, out); break;
  case R_LAUNCH: h_launch(ctx, &body, id, out); break;
  case R_CLOSE: h_close(&body, out); break;
  case R_STANDBY: h_standby(ctx, out); break;
  default: fail(out, 404, "not_found", "unknown path");
  }
}
