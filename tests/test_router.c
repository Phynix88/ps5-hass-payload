/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include <sys/time.h>

#include "apps.h"
#include "auth.h"
#include "config.h"
#include "control.h"
#include "http.h"
#include "json.h"
#include "platform_fake.h"
#include "router.h"

static config cfg;
static app_list apps;
static route_ctx ctx;
static route_resp resp;
static char token[65];
static char body[65536];
static const uint32_t PEER = 0xc0000214; /* 192.0.2.20 */

static void setup(void) {
  char cmd[700];
  fake_reset();
  snprintf(fake.root, sizeof(fake.root), "%s", t_tmpdir());
  snprintf(cmd, sizeof(cmd), "cp -R tests/data/apps/. '%s'", fake.root);
  if (system(cmd)) {
    exit(2);
  }
  config_load(&cfg);
  auth_init(&cfg);
  control_init();
  apps_free(&apps);
  memset(&ctx, 0, sizeof(ctx));
  ctx.cfg = &cfg;
  ctx.apps = &apps;
  ctx.device_id = "00112233445566778899aabbccddeeff";
  token[0] = '\0';
}

/* Sends one request; returns the status, the JSON body is in `body`. */
static int call_full(const char *method, const char *path, const char *extra,
                     const char *json, uint32_t peer, bool guard) {
  char req[8192];
  http_req r;
  int n;
  n = snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: 192.0.2.10:8181\r\n",
               method, path);
  if (token[0]) {
    n += snprintf(req + n, sizeof(req) - (size_t)n,
                  "Authorization: Bearer %s\r\n", token);
  }
  if (!strcmp(method, "POST")) {
    if (guard) {
      n += snprintf(req + n, sizeof(req) - (size_t)n,
                    "Content-Type: application/json\r\nX-PS5-Hass: 1\r\n");
    }
    n += snprintf(req + n, sizeof(req) - (size_t)n, "Content-Length: %zu\r\n",
                  json ? strlen(json) : 0);
  }
  n += snprintf(req + n, sizeof(req) - (size_t)n, "%s\r\n%s", extra ? extra : "",
                json ? json : "");
  if (http_parse(req, (size_t)n, &r) <= 0) {
    return -1;
  }
  route(&ctx, &r, peer, &resp);
  body[0] = '\0';
  if (resp.fixed) {
    snprintf(body, sizeof(body), "%s", resp.fixed);
  } else if (resp.body.buf) {
    snprintf(body, sizeof(body), "%s", resp.body.buf);
  }
  n = resp.status;
  route_resp_free(&resp);
  return n;
}

static int call(const char *method, const char *path, const char *json) {
  return call_full(method, path, NULL, json, PEER, true);
}

static void pair(void) {
  char fin[200];
  const char *tok;
  fake.mono_ms += 31000; /* past the restart limit */
  T_EQ_INT(call("POST", "/api/v1/pair/start", "{}"), 200);
  snprintf(fin, sizeof(fin),
           "{\"code\":\"%s\",\"client_id\":\"0b7e2f1c-1111-4222-8333-"
           "944445555666\",\"label\":\"Home Assistant\"}",
           fake.last_notify + strlen(fake.last_notify) - 6);
  T_EQ_INT(call("POST", "/api/v1/pair/finish", fin), 200);
  tok = strstr(body, "\"token\":\"");
  T_ASSERT(tok);
  memcpy(token, tok + 9, 64);
  token[64] = '\0';
}

static void test_origin_blocked_before_everything(void) {
  setup();
  T_EQ_INT(call_full("GET", "/api/v1/info", "Origin: http://evil\r\n", NULL,
                     PEER, true),
           403);
  T_ASSERT(strstr(body, "\"error\":\"origin_forbidden\""));
  cfg.allow_broken = true;
  T_EQ_INT(call_full("GET", "/nothing", "Origin: x\r\n", NULL, PEER, true), 403);
  T_ASSERT(strstr(body, "origin_forbidden"));
}

static void test_post_requires_json_and_marker(void) {
  setup();
  T_EQ_INT(call_full("POST", "/api/v1/pair/start", NULL, "{}", PEER, false), 403);
  T_ASSERT(strstr(body, "\"error\":\"browser_guard\""));
  T_EQ_INT(call_full("POST", "/api/v1/pair/start", "X-PS5-Hass: 1\r\n"
                     "Content-Type: text/plain\r\n", "{}", PEER, false), 403);
  T_EQ_INT(call_full("POST", "/api/v1/pair/start",
                     "X-PS5-Hass: 1\r\nContent-Type: application/json; "
                     "charset=utf-8\r\n", "{}", PEER, false),
           200);
}

static int host_call(const char *host) {
  char req[512];
  http_req r;
  int n = snprintf(req, sizeof(req), "GET /api/v1/info HTTP/1.1\r\nHost: %s\r\n\r\n",
                   host);
  if (http_parse(req, (size_t)n, &r) <= 0) {
    return -1;
  }
  route(&ctx, &r, PEER, &resp);
  n = resp.status;
  if (resp.body.buf) {
    snprintf(body, sizeof(body), "%s", resp.body.buf);
  }
  route_resp_free(&resp);
  return n;
}

static void test_host_ip_local_and_listed_ok(void) {
  setup();
  T_EQ_INT(host_call("192.0.2.10"), 200);
  T_EQ_INT(host_call("192.0.2.10:8181"), 200);
  T_EQ_INT(host_call("PS5-abc.LOCAL:8181"), 200);
  T_EQ_INT(host_call("ps5.lan"), 403);
  snprintf(cfg.hosts[0], sizeof(cfg.hosts[0]), "ps5.lan");
  cfg.n_hosts = 1;
  T_EQ_INT(host_call("PS5.lan:8181"), 200);
}

static void test_host_rebinding_blocked(void) {
  setup();
  T_EQ_INT(host_call("evil.example"), 403);
  T_ASSERT(strstr(body, "host_forbidden"));
  T_EQ_INT(host_call(".local"), 403);
  T_EQ_INT(host_call("1.2.3.4.evil.example"), 403);
  T_EQ_INT(host_call("999.1.1.1"), 403);
  {
    char req[] = "GET /api/v1/info HTTP/1.1\r\n\r\n";
    http_req r;
    http_parse(req, strlen(req), &r);
    route(&ctx, &r, PEER, &resp);
    T_EQ_INT(resp.status, 403);
    route_resp_free(&resp);
  }
}

static void test_allow_cidr(void) {
  setup();
  cfg.allow[0] = (cidr){0xc0000200, 0xffffff00};
  cfg.n_allow = 1;
  T_EQ_INT(call_full("GET", "/api/v1/info", NULL, NULL, 0xc0000214, true), 200);
  T_EQ_INT(call_full("GET", "/api/v1/info", NULL, NULL, 0xc6336414 /* 198.51.100.20 */, true), 403);
  T_ASSERT(strstr(body, "address_forbidden"));
}

static void test_allow_broken_blocks_all(void) {
  setup();
  cfg.allow_broken = true;
  T_EQ_INT(call("GET", "/api/v1/info", NULL), 403);
  T_ASSERT(strstr(body, "address_forbidden"));
}

static void test_unknown_404_wrong_method_405(void) {
  setup();
  T_EQ_INT(call("GET", "/api/v1/nothing", NULL), 404);
  T_ASSERT(strstr(body, "\"error\":\"not_found\""));
  T_EQ_INT(call("POST", "/api/v1/info", "{}"), 405);
  T_ASSERT(strstr(body, "method_not_allowed"));
  T_EQ_INT(call("GET", "/api/v1/pair/start", NULL), 405);
  T_EQ_INT(call("GET", "/api/v1/apps/running/close", NULL), 405);
  T_EQ_INT(call("DELETE", "/api/v1/state", NULL), 405);
}

static void test_token_required_401(void) {
  setup();
  T_EQ_INT(call("GET", "/api/v1/state", NULL), 401);
  T_ASSERT(strstr(body, "\"error\":\"unauthorized\""));
  T_EQ_INT(call("GET", "/api/v1/apps", NULL), 401);
  T_EQ_INT(call("POST", "/api/v1/pair/revoke", "{}"), 401);
  memset(token, 'a', 64);
  token[64] = '\0';
  T_EQ_INT(call("GET", "/api/v1/state", NULL), 401);
}

static void test_bad_title_id_400(void) {
  setup();
  pair();
  T_EQ_INT(call("POST", "/api/v1/apps/ppsa00001/launch", "{}"), 400);
  T_ASSERT(strstr(body, "bad_title_id"));
  T_EQ_INT(call("GET", "/api/v1/apps/../../etc/icon", NULL), 404);
  T_EQ_INT(call("GET", "/api/v1/apps/PPSA0000X/icon", NULL), 400);
  T_EQ_INT(call("POST", "/api/v1/apps/running/close", "{\"title_id\":\"x\"}"),
           400);
}

static void test_bad_body_400(void) {
  setup();
  T_EQ_INT(call("POST", "/api/v1/pair/start", ""), 400);
  T_ASSERT(strstr(body, "bad_request"));
  T_EQ_INT(call("POST", "/api/v1/pair/start", "[]"), 400);
  T_EQ_INT(call("POST", "/api/v1/pair/finish", "{\"code\":1}"), 400);
  pair();
  T_EQ_INT(call("POST", "/api/v1/apps/PPSA00001/launch",
                "{\"replace_running\":\"yes\"}"), 400);
}

static void test_pair_flow_end_to_end(void) {
  setup();
  T_EQ_INT(call("POST", "/api/v1/pair/start", "{}"), 200);
  T_ASSERT(strstr(body, "{\"result\":\"ok\",\"expires_in\":120}"));
  T_EQ_INT(call("POST", "/api/v1/pair/finish",
                "{\"code\":\"000000\",\"client_id\":\"0b7e2f1c-1111-4222-8333-"
                "944445555666\",\"label\":\"x\"}"),
           401);
  T_ASSERT(strstr(body, "\"error\":\"wrong_code\""));
  T_ASSERT(strstr(body, "\"attempts_left\":4"));
  pair();
  T_EQ_INT(call("GET", "/api/v1/state", NULL), 200);
  T_ASSERT(strstr(body, "{\"result\":\"ok\",\"in_app\":false"));
  T_EQ_INT(call("GET", "/api/v1/info", NULL), 200);
  T_ASSERT(strstr(body, "\"paired\":true"));
}

static void test_pair_errors(void) {
  setup();
  T_EQ_INT(call("POST", "/api/v1/pair/start", "{}"), 200);
  T_EQ_INT(call("POST", "/api/v1/pair/start", "{}"), 429);
  T_ASSERT(strstr(body, "\"error\":\"rate_limited\""));
  T_ASSERT(strstr(body, "\"retry_after\":30"));
  fake.mono_ms += 200000;
  T_EQ_INT(call("POST", "/api/v1/pair/finish",
                "{\"code\":\"000000\",\"client_id\":\"0b7e2f1c-1111-4222-8333-"
                "944445555666\",\"label\":\"x\"}"),
           409);
  T_ASSERT(strstr(body, "\"error\":\"no_code\""));
  fake.notify_rc = -1;
  T_EQ_INT(call("POST", "/api/v1/pair/start", "{}"), 500);
  T_ASSERT(strstr(body, "notify_failed"));
  fake.notify_rc = 0;
  fake.random_rc = -1;
  T_EQ_INT(call("POST", "/api/v1/pair/start", "{}"), 500);
  T_ASSERT(strstr(body, "random_unavailable"));
}

static void test_apps_etag_304(void) {
  char etag_hdr[80];
  const char *e;
  setup();
  pair();
  T_EQ_INT(call("GET", "/api/v1/apps", NULL), 200);
  T_ASSERT(strstr(body, "{\"title_id\":\"PPSA00001\",\"name\":\"Test Game One US\","
                        "\"version\":\"01.007.000\",\"kind\":\"game\","
                        "\"category\":0,\"has_icon\":true}"));
  T_ASSERT(strstr(body, "{\"title_id\":\"PPSA00002\",\"name\":\"Homebrew Two\","
                        "\"version\":null,\"kind\":\"app\","
                        "\"category\":65536,\"has_icon\":true}"));
  T_ASSERT(strstr(body, "\"category\":null,\"has_icon\":false"));
  e = strstr(body, "\"etag\":\"");
  T_ASSERT(e);
  snprintf(etag_hdr, sizeof(etag_hdr), "If-None-Match: \"%.16s\"\r\n", e + 8);
  T_EQ_INT(call_full("GET", "/api/v1/apps", etag_hdr, NULL, PEER, true), 304);
  T_EQ_STR(body, "");
}

static void test_icon_png_and_jpeg_ctype(void) {
  char req[512];
  http_req r;
  int n;
  setup();
  pair();
  n = snprintf(req, sizeof(req), "GET /api/v1/apps/PLDM00001/icon HTTP/1.1\r\n"
               "Host: 10.0.0.1\r\nAuthorization: Bearer %s\r\n\r\n", token);
  http_parse(req, (size_t)n, &r);
  route(&ctx, &r, PEER, &resp);
  T_EQ_INT(resp.status, 200);
  T_EQ_STR(resp.ctype, "image/jpeg");
  T_ASSERT(resp.file_fd >= 0);
  T_EQ_INT(resp.file_size, 22);
  T_ASSERT(resp.etag[0] == '"');
  route_resp_free(&resp);
  n = snprintf(req, sizeof(req), "GET /api/v1/apps/PPSA00001/icon HTTP/1.1\r\n"
               "Host: 10.0.0.1\r\nAuthorization: Bearer %s\r\n\r\n", token);
  http_parse(req, (size_t)n, &r);
  route(&ctx, &r, PEER, &resp);
  T_EQ_INT(resp.status, 200);
  T_EQ_STR(resp.ctype, "image/png");
  route_resp_free(&resp);
}

static void test_icon_etag_304(void) {
  char req[512], hdr[80];
  http_req r;
  int n;
  setup();
  pair();
  n = snprintf(req, sizeof(req), "GET /api/v1/apps/PPSA00001/icon HTTP/1.1\r\n"
               "Host: 10.0.0.1\r\nAuthorization: Bearer %s\r\n\r\n", token);
  http_parse(req, (size_t)n, &r);
  route(&ctx, &r, PEER, &resp);
  snprintf(hdr, sizeof(hdr), "If-None-Match: %s\r\n", resp.etag);
  route_resp_free(&resp);
  T_EQ_INT(call_full("GET", "/api/v1/apps/PPSA00001/icon", hdr, NULL, PEER,
                     true),
           304);
}

static void test_icon_missing_404(void) {
  setup();
  pair();
  T_EQ_INT(call("GET", "/api/v1/apps/TEST00007/icon", NULL), 404);
  T_ASSERT(strstr(body, "\"error\":\"not_found\""));
  T_EQ_INT(call("GET", "/api/v1/apps/PPSA99999/icon", NULL), 404);
  T_ASSERT(strstr(body, "\"error\":\"not_installed\""));
}

static void test_launch_codes(void) {
  setup();
  pair();
  T_EQ_INT(call("POST", "/api/v1/apps/PPSA00001/launch", "{}"), 200);
  T_ASSERT(strstr(body, "{\"result\":\"ok\",\"already_running\":false,"
                        "\"app_id\":100}"));
  T_EQ_INT(call("POST", "/api/v1/apps/PPSA00001/launch", "{}"), 200);
  T_ASSERT(strstr(body, "\"already_running\":true"));
  T_EQ_INT(call("POST", "/api/v1/apps/PPSA00002/launch", "{}"), 409);
  T_ASSERT(strstr(body, "other_app_running"));
  T_EQ_INT(call("POST", "/api/v1/apps/PPSA00002/launch",
                "{\"replace_running\":true}"), 200);
  T_EQ_INT(call("POST", "/api/v1/apps/PPSA99999/launch", "{}"), 404);
  T_ASSERT(strstr(body, "not_installed"));
  fake.user_rc = -1;
  T_EQ_INT(call("POST", "/api/v1/apps/PPSA00001/launch",
                "{\"replace_running\":true}"), 409);
  T_ASSERT(strstr(body, "no_user"));
}

static void test_close_codes(void) {
  setup();
  pair();
  T_EQ_INT(call("POST", "/api/v1/apps/running/close", "{}"), 409);
  T_ASSERT(strstr(body, "\"error\":\"no_app\""));
  fake.running = (plat_running){5, "PPSA00002"};
  T_EQ_INT(call("POST", "/api/v1/apps/running/close",
                "{\"title_id\":\"PPSA00001\"}"), 409);
  T_ASSERT(strstr(body, "title_mismatch"));
  T_EQ_INT(call("POST", "/api/v1/apps/running/close",
                "{\"title_id\":\"PPSA00002\"}"), 200);
  T_EQ_INT(fake.running.app_id, 0);
}

static void test_standby_codes(void) {
  setup();
  pair();
  T_EQ_INT(call("POST", "/api/v1/power/standby", "{}"), 403);
  T_ASSERT(strstr(body, "power_actions_disabled"));
  cfg.power_actions = true;
  T_EQ_INT(call("POST", "/api/v1/power/standby", "{}"), 200);
  T_EQ_INT(call("POST", "/api/v1/power/standby", "{}"), 429);
  T_ASSERT(strstr(body, "\"retry_after\":10"));
}

static void test_revoke_then_401(void) {
  setup();
  pair();
  T_EQ_INT(call("POST", "/api/v1/pair/revoke", "{}"), 200);
  T_EQ_INT(call("GET", "/api/v1/state", NULL), 401);
}

static void test_system_error_has_rc_hex(void) {
  setup();
  pair();
  fake.launch_rc = (int)0x80940005;
  T_EQ_INT(call("POST", "/api/v1/apps/PPSA00001/launch", "{}"), 500);
  T_ASSERT(strstr(body, "\"error\":\"system_error\""));
  T_ASSERT(strstr(body, "\"rc\":-2137784315,\"rc_hex\":\"0x80940005\""));
}

static void test_alloc_failure_is_500_internal(void) {
  int status;
  setup();
  pair();
  jw_test_fail_after(1);
  status = call("GET", "/api/v1/apps", NULL);
  jw_test_fail_after(-1);
  T_EQ_INT(status, 500);
  T_ASSERT(strstr(body, "\"error\":\"internal\""));
  T_ASSERT(!strstr(body, "PPSA"));
}

static void test_apps_rescanned_on_change(void) {
  char p[800];
  setup();
  pair();
  T_EQ_INT(call("POST", "/api/v1/apps/NEWA00001/launch", "{}"), 404);
  snprintf(p, sizeof(p), "%s/user/app/NEWA00001/sce_sys/param.json", fake.root);
  t_write_str(p, "{\"localizedParameters\":{\"defaultLanguage\":\"en-US\","
                 "\"en-US\":{\"titleName\":\"New\"}}}");
  T_EQ_INT(call("POST", "/api/v1/apps/NEWA00001/launch", "{}"), 200);
}


static void test_notify_codes(void) {
  setup();
  T_EQ_INT(call("POST", "/api/v1/notify", "{\"message\":\"Essen ist fertig\"}"), 401);
  pair();
  T_EQ_INT(call("POST", "/api/v1/notify", "{\"message\":\"Essen ist fertig\"}"), 200);
  T_EQ_STR(fake.last_notify, "Essen ist fertig");
  T_EQ_INT(call("POST", "/api/v1/notify", "{}"), 400);
  T_EQ_INT(call("POST", "/api/v1/notify", "{\"message\":5}"), 400);
  T_EQ_INT(call("POST", "/api/v1/notify", "{\"message\":\"\"}"), 400);
  for (int i = 0; i < 4; i++) {
    call("POST", "/api/v1/notify", "{\"message\":\"x\"}");
  }
  T_EQ_INT(call("POST", "/api/v1/notify", "{\"message\":\"x\"}"), 429);
  T_ASSERT(strstr(body, "\"error\":\"rate_limited\""));
  T_ASSERT(strstr(body, "\"retry_after\":"));
  T_EQ_INT(call("GET", "/api/v1/notify", NULL), 405);
}

static void test_power_off_codes(void) {
  setup();
  pair();
  T_EQ_INT(call("POST", "/api/v1/power/off", "{}"), 403);
  T_ASSERT(strstr(body, "\"error\":\"power_off_disabled\""));
  cfg.power_off = true;
  T_EQ_INT(call("POST", "/api/v1/power/off", "{}"), 200);
  T_EQ_INT(fake.power_off_calls, 1);
  T_EQ_INT(call("POST", "/api/v1/power/off", "{}"), 429);
}

int main(void) {
  T_RUN(test_origin_blocked_before_everything);
  T_RUN(test_post_requires_json_and_marker);
  T_RUN(test_host_ip_local_and_listed_ok);
  T_RUN(test_host_rebinding_blocked);
  T_RUN(test_allow_cidr);
  T_RUN(test_allow_broken_blocks_all);
  T_RUN(test_unknown_404_wrong_method_405);
  T_RUN(test_token_required_401);
  T_RUN(test_bad_title_id_400);
  T_RUN(test_bad_body_400);
  T_RUN(test_pair_flow_end_to_end);
  T_RUN(test_pair_errors);
  T_RUN(test_apps_etag_304);
  T_RUN(test_icon_png_and_jpeg_ctype);
  T_RUN(test_icon_etag_304);
  T_RUN(test_icon_missing_404);
  T_RUN(test_launch_codes);
  T_RUN(test_close_codes);
  T_RUN(test_standby_codes);
  T_RUN(test_revoke_then_401);
  T_RUN(test_system_error_has_rc_hex);
  T_RUN(test_alloc_failure_is_500_internal);
  T_RUN(test_apps_rescanned_on_change);
  T_RUN(test_notify_codes);
  T_RUN(test_power_off_codes);
  apps_free(&apps);
  T_MAIN();
}
