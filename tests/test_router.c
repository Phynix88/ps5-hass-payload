/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "t.h"

#include <sys/time.h>

#include "apps.h"
#include "auth.h"
#include "config.h"
#include "control.h"
#include "fan.h"
#include "http.h"
#include "json.h"
#include "limits.h"
#include "platform_fake.h"
#include "playtime.h"
#include "pldmgr.h"
#include "power.h"
#include "router.h"
#include "state.h"

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
  power_init();
  playtime_load();
  info_cache_reset();
  apps_free(&apps);
  memset(&ctx, 0, sizeof(ctx));
  ctx.cfg = &cfg;
  ctx.apps = &apps;
  ctx.device_id = "00112233445566778899aabbccddeeff";
  token[0] = '\0';
  pldmgr_set_target("127.0.0.1", 1); /* nothing listens there */
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

static void test_suspend_codes(void) {
  setup();
  pair();
  T_EQ_INT(call("GET", "/api/v1/apps/running/suspend", NULL), 405);
  T_EQ_INT(call("POST", "/api/v1/apps/running/suspend", "{}"), 409);
  T_ASSERT(strstr(body, "\"error\":\"not_running\""));
  T_EQ_INT(fake.suspend_calls, 0);
  fake.running = (plat_running){5, "PPSA00002"};
  fake.caps.suspend = false;
  T_EQ_INT(call("POST", "/api/v1/apps/running/suspend", "{}"), 409);
  T_ASSERT(strstr(body, "\"error\":\"not_available\""));
  fake.caps.suspend = true;
  T_EQ_INT(call("POST", "/api/v1/apps/running/suspend", "{}"), 200);
  T_ASSERT(strstr(body, "\"result\":\"ok\""));
  T_EQ_INT(fake.suspended_app_id, 5);
  fake.suspend_rc = -3;
  T_EQ_INT(call("POST", "/api/v1/apps/running/suspend", "{}"), 500);
}

static void test_suspend_not_on(void) {
  setup();
  pair();
  fake.running = (plat_running){5, "PPSA00002"};
  fake.power_state = 500;
  power_tick(fake.mono_ms);
  fake.reads = fake.sony_calls = 0;
  T_EQ_INT(call("POST", "/api/v1/apps/running/suspend", "{}"), 409);
  T_ASSERT(strstr(body, "\"error\":\"not_on\""));
  T_EQ_INT(fake.suspend_calls, 0);
  T_EQ_INT(fake.reads, 0);
  T_EQ_INT(fake.sony_calls, 0);
}

static void test_standby_codes(void) {
  setup();
  pair();
  T_EQ_INT(call("POST", "/api/v1/power/standby", "{}"), 200);
  T_EQ_INT(call("POST", "/api/v1/power/standby", "{}"), 429);
  T_ASSERT(strstr(body, "\"retry_after\":10"));
}

static void test_standby_unavailable_409(void) {
  setup();
  pair();
  fake.caps.standby = false;
  T_EQ_INT(call("POST", "/api/v1/power/standby", "{}"), 409);
  T_ASSERT(strstr(body, "\"error\":\"not_available\""));
  T_EQ_INT(fake.standby_calls, 0);
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
  T_EQ_INT(call("POST", "/api/v1/power/off", "{}"), 200);
  T_EQ_INT(fake.power_off_calls, 1);
  T_EQ_INT(call("POST", "/api/v1/power/off", "{}"), 429);
}

static void test_power_off_unavailable_409(void) {
  setup();
  pair();
  fake.caps.power_off = false;
  T_EQ_INT(call("POST", "/api/v1/power/off", "{}"), 409);
  T_ASSERT(strstr(body, "\"error\":\"not_available\""));
  T_EQ_INT(fake.power_off_calls, 0);
}

static void test_fan_codes(void) {
  setup();
  pair();
  fake.fan_value = 91;
  fan_init(&cfg);
  T_EQ_INT(call("POST", "/api/v1/fan", "{\"threshold_c\":70}"), 200);
  T_ASSERT(strstr(body, "\"target_c\":70"));
  T_EQ_INT(fake.fan_value, 70);
  T_EQ_INT(call("POST", "/api/v1/fan", "{\"threshold_c\":95}"), 400);
  T_EQ_INT(call("POST", "/api/v1/fan", "{\"threshold_c\":\"x\"}"), 400);
  cfg.fan_stock_c = 80;
  T_EQ_INT(call("POST", "/api/v1/fan", "{\"threshold_c\":84}"), 409);
  T_ASSERT(strstr(body, "\"error\":\"fan_above_stock\""));
  T_EQ_INT(call("POST", "/api/v1/fan", "{\"threshold_c\":0}"), 200);
  T_EQ_INT(fake.fan_value, 80);
  T_EQ_INT(call("GET", "/api/v1/state", NULL), 200);
  T_ASSERT(strstr(body, "\"fan\":{\"target_c\":null,\"current_c\":80,\"stock_c\":80}"));
}

/* --- v0.3 grant endpoints: compatibility answers ------------------------- */

static void test_keep_awake_toggle(void) {
  setup();
  T_EQ_INT(call("POST", "/api/v1/keep_awake", "{\"on\":true}"), 401);
  pair();
  T_EQ_INT(call("GET", "/api/v1/state", NULL), 200);
  T_ASSERT(strstr(body, "\"keep_awake\":false"));
  T_EQ_INT(call("POST", "/api/v1/keep_awake", "{\"on\":true}"), 200);
  T_EQ_STR(body, "{\"result\":\"ok\",\"keep_awake\":true}");
  T_ASSERT(power_keep_awake());
  T_EQ_INT(call("GET", "/api/v1/state", NULL), 200);
  T_ASSERT(strstr(body, "\"keep_awake\":true"));
  T_EQ_INT(call("POST", "/api/v1/keep_awake", "{\"on\":false}"), 200);
  T_ASSERT(strstr(body, "\"keep_awake\":false"));
  T_ASSERT(!power_keep_awake());
  T_EQ_INT(call("POST", "/api/v1/keep_awake", "{\"on\":1}"), 400);
  T_EQ_INT(call("POST", "/api/v1/keep_awake", "{}"), 400);
  T_EQ_INT(call("GET", "/api/v1/keep_awake", NULL), 405);
  T_EQ_INT(fake.keep_awake_calls, 0); /* switching does not call the console */
}

static void test_keep_awake_unavailable_409(void) {
  setup();
  pair();
  fake.caps.keep_awake = false;
  T_EQ_INT(call("POST", "/api/v1/keep_awake", "{\"on\":true}"), 409);
  T_ASSERT(strstr(body, "\"error\":\"not_available\""));
  T_ASSERT(!power_keep_awake());
}

static void test_state_has_power_state(void) {
  setup();
  pair();
  power_tick(fake.mono_ms);
  T_EQ_INT(call("GET", "/api/v1/state", NULL), 200);
  T_ASSERT(strstr(body, "\"power_state\":\"on\""));
  T_ASSERT(strstr(body, "\"cpu\":41"));
}

static void test_state_outside_on_leaves_console_alone(void) {
  setup();
  pair();
  fake.running = (plat_running){.app_id = 7, .title_id = "PPSA00001"};
  fake.n_procs = 1;
  strcpy(fake.procs[0], "x.elf");
  fake.power_state = 500;
  power_tick(fake.mono_ms);
  fake.reads = fake.sony_calls = 0;
  T_EQ_INT(call("GET", "/api/v1/state", NULL), 200);
  T_EQ_INT(fake.reads, 0);
  T_EQ_INT(fake.sony_calls, 0);
  T_ASSERT(strstr(body, "\"power_state\":\"rest\""));
  T_ASSERT(strstr(body, "\"in_app\":false,\"app\":null"));
  T_ASSERT(strstr(body, "\"temps\":{\"cpu\":null,\"soc\":null}"));
  T_ASSERT(strstr(body, "\"fan_raw\":null"));
  T_ASSERT(strstr(body, "\"data_free_mb\":null,\"data_total_mb\":null,"
                        "\"user_free_mb\":null,\"user_total_mb\":null"));
  T_ASSERT(strstr(body, "\"payloads\":[]"));
  T_ASSERT(strstr(body, "\"uptime_s\":100"));
}

static void rest(int state) {
  fake.power_state = state;
  power_tick(fake.mono_ms);
}

static void test_gated_routes_refuse_outside_on(void) {
  static const struct {
    const char *method, *path, *body;
  } r[] = {
      {"POST", "/api/v1/apps/PPSA00001/launch", "{}"},
      {"POST", "/api/v1/apps/running/close", "{}"},
      {"POST", "/api/v1/apps/running/suspend", "{}"},
      {"POST", "/api/v1/power/standby", "{}"},
      {"POST", "/api/v1/power/off", "{}"},
      {"POST", "/api/v1/notify", "{\"message\":\"x\"}"},
      {"POST", "/api/v1/fan", "{\"threshold_c\":70}"},
      {"POST", "/api/v1/keep_awake", "{\"on\":true}"},
      {"POST", "/api/v1/payloads/start", "{\"slot\":\"repo/ftpsrv@e8cce5d1\"}"},
      {"POST", "/api/v1/payloads/stop", "{\"slot\":\"repo/ftpsrv@e8cce5d1\"}"},
      {"POST", "/api/v1/pair/start", "{}"},
      {"POST", "/api/v1/pair/finish",
       "{\"code\":\"123456\",\"client_id\":\"0b7e2f1c-1111-4222-8333-944445555666\","
       "\"label\":\"x\"}"},
  };
  static const int states[] = {500, 300, 100, 42};
  for (size_t k = 0; k < sizeof(states) / sizeof(states[0]); k++) {
    setup();
    pair();
    rest(states[k]);
    fake.reads = fake.sony_calls = fake.notify_count = 0;
    for (size_t i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
      T_EQ_INT(call(r[i].method, r[i].path, r[i].body), 409);
      T_ASSERT(strstr(body, "\"error\":\"not_on\""));
    }
    T_EQ_INT(fake.reads, 0);
    T_EQ_INT(fake.sony_calls, 0);
    T_EQ_INT(fake.notify_count, 0);
    T_ASSERT(!power_keep_awake());
  }
}

static void test_payloads_unavailable_without_manager(void) {
  setup();
  T_EQ_INT(call("GET", "/api/v1/payloads", NULL), 401);
  pair();
  T_EQ_INT(call("GET", "/api/v1/payloads", NULL), 200);
  T_ASSERT(strstr(body, "\"available\":false"));
  T_ASSERT(strstr(body, "\"payloads\":[]"));
  T_EQ_INT(call("POST", "/api/v1/payloads/start", "{\"slot\":\"repo/ftpsrv@e8cce5d1\"}"), 503);
  T_ASSERT(strstr(body, "\"error\":\"payload_manager_unavailable\""));
  T_EQ_INT(call("POST", "/api/v1/payloads/stop", "{\"slot\":\"repo/ftpsrv@e8cce5d1\"}"), 503);
  T_EQ_INT(call("POST", "/api/v1/payloads/stop", "{}"), 400);
  T_EQ_INT(call("POST", "/api/v1/payloads/stop", "{\"slot\":5}"), 400);
  T_EQ_INT(call("POST", "/api/v1/payloads/stop", "{\"slot\":\"\"}"), 400);
  T_EQ_INT(call("POST", "/api/v1/payloads", "{}"), 405);
  T_EQ_INT(call("GET", "/api/v1/payloads/start", NULL), 405);
  T_EQ_INT(call("GET", "/api/v1/info", NULL), 200);
  T_ASSERT(strstr(body, "\"payload_control\":false"));
}

static void test_payloads_protected_is_403_without_contact(void) {
  setup();
  pair();
  /* the manager is unreachable, yet a protected slot is answered 403 (not 503) */
  T_EQ_INT(call("POST", "/api/v1/payloads/start", "{\"slot\":\"repo/kstuff@44444444\"}"), 403);
  T_ASSERT(strstr(body, "\"error\":\"protected\""));
  T_EQ_INT(call("POST", "/api/v1/payloads/stop", "{\"slot\":\"repo/etaHEN@55555555\"}"), 403);
  T_EQ_INT(call("POST", "/api/v1/payloads/start", "{\"slot\":\"local/ps5-hass.elf\"}"), 403);
}

static void test_payloads_refused_while_app_runs(void) {
  setup();
  pair();
  /* the manager is unreachable, yet a running app is answered 409 (not 503):
   * the check comes before any contact */
  fake.running.app_id = 7;
  T_EQ_INT(call("POST", "/api/v1/payloads/stop", "{\"slot\":\"repo/ftpsrv@e8cce5d1\"}"), 409);
  T_ASSERT(strstr(body, "\"error\":\"app_running\""));
  T_EQ_INT(call("POST", "/api/v1/payloads/start", "{\"slot\":\"repo/ftpsrv@e8cce5d1\"}"), 409);
  T_ASSERT(strstr(body, "\"error\":\"app_running\""));
  /* protected is still answered first */
  T_EQ_INT(call("POST", "/api/v1/payloads/stop", "{\"slot\":\"repo/CheatRunner@22222222\"}"), 403);
  T_ASSERT(strstr(body, "\"error\":\"protected\""));
  T_EQ_INT(call("POST", "/api/v1/payloads/stop", "{\"slot\":\"repo/ps5debug-ng@12121212\"}"), 403);
  /* a read error fails safe */
  fake.running.app_id = 0;
  fake.running_rc = -5;
  T_EQ_INT(call("POST", "/api/v1/payloads/start", "{\"slot\":\"repo/ftpsrv@e8cce5d1\"}"), 409);
  T_ASSERT(strstr(body, "\"error\":\"app_running\""));
  /* no app: the usual answer (the manager is not there) */
  fake.running_rc = 0;
  T_EQ_INT(call("POST", "/api/v1/payloads/stop", "{\"slot\":\"repo/ftpsrv@e8cce5d1\"}"), 503);
  /* outside "on" the console is not asked at all */
  fake.running.app_id = 7;
  rest(500);
  fake.reads = fake.sony_calls = 0;
  T_EQ_INT(call("POST", "/api/v1/payloads/stop", "{\"slot\":\"repo/ftpsrv@e8cce5d1\"}"), 409);
  T_ASSERT(strstr(body, "\"error\":\"not_on\""));
  T_EQ_INT(fake.sony_calls, 0);
}

static void test_payloads_outside_on_leave_manager_alone(void) {
  setup();
  pair();
  rest(500);
  fake.reads = fake.sony_calls = 0;
  T_EQ_INT(call("GET", "/api/v1/payloads", NULL), 200);
  T_ASSERT(strstr(body, "\"available\":false"));
  T_EQ_INT(fake.sony_calls, 0);
}

static void test_unauthenticated_pair_refused_outside_on(void) {
  setup();
  rest(500);
  T_EQ_INT(call("POST", "/api/v1/pair/start", "{}"), 409);
  T_ASSERT(strstr(body, "\"error\":\"not_on\""));
  T_EQ_INT(fake.notify_count, 0);
}

static void test_keep_awake_off_and_reads_work_outside_on(void) {
  setup();
  pair();
  T_EQ_INT(call("POST", "/api/v1/keep_awake", "{\"on\":true}"), 200);
  rest(500);
  fake.reads = 0;
  T_EQ_INT(call("POST", "/api/v1/keep_awake", "{\"on\":false}"), 200);
  T_ASSERT(!power_keep_awake());
  T_EQ_INT(call("GET", "/api/v1/apps", NULL), 200);
  T_EQ_INT(call("GET", "/api/v1/info", NULL), 200);
  T_EQ_INT(call("GET", "/api/v1/state", NULL), 200);
  T_EQ_INT(fake.reads, 0);
}

static void test_info_outside_on_makes_no_console_call(void) {
  setup();
  rest(500);
  T_EQ_INT(call("GET", "/api/v1/info", NULL), 200);
  T_EQ_INT(fake.reads, 0);
  T_EQ_INT(fake.sony_calls, 0);
  T_ASSERT(strstr(body, "\"model\":\"\""));
  rest(1000);
  T_EQ_INT(call("GET", "/api/v1/info", NULL), 200);
  T_ASSERT(strstr(body, "\"model\":\"CFI-TEST\""));
}

static void test_playtime_since_param(void) {
  char file[512];
  setup();
  T_EQ_INT(call("GET", "/api/v1/playtime", NULL), 401);
  pair();
  snprintf(file, sizeof(file),
           "{\"schema\":1,\"titles\":{\"PPSA00002\":{\"kind\":\"game\",\"total_s\":90}},"
           "\"sessions\":[[\"PPSA00002\",\"game\",%lld,30],[\"PPSA00002\",\"game\",%lld,60]]}",
           (long long)(fake.wall_s - 20 * 86400), (long long)(fake.wall_s - 3600));
  T_ASSERT(!playtime_load_buf(file, strlen(file), fake.wall_s));
  /* default: the last 8 days */
  T_EQ_INT(call("GET", "/api/v1/playtime", NULL), 200);
  T_ASSERT(strstr(body, "{\"result\":\"ok\",\"foreground\":true,\"titles\":["));
  T_ASSERT(strstr(body, "\"total_s\":90,\"console_total_s\":null"));
  T_ASSERT(strstr(body, "\"duration_s\":60"));
  T_ASSERT(!strstr(body, "\"duration_s\":30"));
  T_ASSERT(strstr(body, "\"current\":null"));
  T_EQ_INT(call("GET", "/api/v1/playtime?since=0", NULL), 200);
  T_ASSERT(strstr(body, "\"duration_s\":30"));
  T_EQ_INT(call("GET", "/api/v1/playtime?x=1&since=0&y=2", NULL), 200);
  T_ASSERT(strstr(body, "\"duration_s\":30"));
  T_EQ_INT(call("GET", "/api/v1/playtime?other=abc", NULL), 200);
  {
    char q[128];
    snprintf(q, sizeof(q), "/api/v1/playtime?since=%lld", (long long)fake.wall_s);
    T_EQ_INT(call("GET", q, NULL), 200);
    T_ASSERT(strstr(body, "\"sessions\":[]"));
  }
  /* invalid since */
  T_EQ_INT(call("GET", "/api/v1/playtime?since=abc", NULL), 400);
  T_ASSERT(strstr(body, "\"error\":\"bad_request\""));
  T_EQ_INT(call("GET", "/api/v1/playtime?since=", NULL), 400);
  T_EQ_INT(call("GET", "/api/v1/playtime?since=-5", NULL), 400);
  T_EQ_INT(call("GET", "/api/v1/playtime?since=12x", NULL), 400);
  T_EQ_INT(call("GET", "/api/v1/playtime?since=99999999999999999999", NULL), 400);
  T_EQ_INT(call("GET", "/api/v1/playtime?since=1&since=abc", NULL), 200); /* first wins */
  T_EQ_INT(call("POST", "/api/v1/playtime", "{}"), 405);
}

static void test_playtime_foreground_follows_capability(void) {
  setup();
  pair();
  fake.caps.focus = false;
  T_EQ_INT(call("GET", "/api/v1/playtime", NULL), 200);
  T_ASSERT(strstr(body, "\"foreground\":false"));
}

static void test_playtime_foreground_false_while_focus_fails(void) {
  setup();
  pair();
  fake.running = (plat_running){.app_id = 7, .title_id = "PPSA00001"};
  fake.focused_app_rc = -1;
  power_tick(fake.mono_ms);
  for (int i = 0; i < PS5H_PLAYTIME_FOCUS_FAILS; i++) {
    fake.mono_ms += PS5H_PLAYTIME_POLL_S * 1000;
    fake.wall_s += PS5H_PLAYTIME_POLL_S;
    playtime_tick(ctx.apps, fake.mono_ms);
  }
  T_EQ_INT(call("GET", "/api/v1/playtime", NULL), 200);
  T_ASSERT(strstr(body, "\"foreground\":false"));
  fake.focused_app_rc = 0;
  fake.focused_app_id = 7;
  fake.mono_ms += PS5H_PLAYTIME_POLL_S * 1000;
  playtime_tick(ctx.apps, fake.mono_ms);
  T_EQ_INT(call("GET", "/api/v1/playtime", NULL), 200);
  T_ASSERT(strstr(body, "\"foreground\":true"));
}

static void test_playtime_served_outside_on_without_console(void) {
  setup();
  pair();
  fake.power_state = 500;
  power_tick(fake.mono_ms);
  fake.reads = fake.sony_calls = 0;
  T_EQ_INT(call("GET", "/api/v1/playtime", NULL), 200);
  T_EQ_INT(fake.reads, 0);
  T_EQ_INT(fake.sony_calls, 0);
}

static void test_info_has_playtime_capability(void) {
  setup();
  T_EQ_INT(call("GET", "/api/v1/info", NULL), 200);
  T_ASSERT(strstr(body, "\"playtime\":true"));
}

static void test_grant_start_answers_already_allowed(void) {
  int shown;
  setup();
  pair();
  shown = fake.notify_count;
  T_EQ_INT(call("POST", "/api/v1/grant/start",
                "{\"permissions\":[\"power_off\",\"fan_control\"]}"),
           409);
  T_ASSERT(strstr(body, "\"error\":\"already_allowed\""));
  T_EQ_INT(fake.notify_count, shown); /* no code on the TV any more */
}

static void test_grant_finish_answers_no_code(void) {
  setup();
  pair();
  T_EQ_INT(call("POST", "/api/v1/grant/finish", "{\"code\":\"123456\"}"), 409);
  T_ASSERT(strstr(body, "\"error\":\"no_code\""));
}

static void test_grant_revoke_is_noop_200(void) {
  setup();
  pair();
  T_EQ_INT(call("POST", "/api/v1/grant/revoke",
                "{\"permissions\":[\"power_off\",\"fan_control\"]}"),
           200);
  T_ASSERT(strstr(body, "\"power_off\":{\"allowed\":true,\"available\":true}"));
  T_EQ_INT(call("POST", "/api/v1/power/off", "{}"), 200);
}

static void test_grant_needs_token_401(void) {
  setup();
  T_EQ_INT(call("POST", "/api/v1/grant/start", "{\"permissions\":[\"power_off\"]}"),
           401);
  T_EQ_INT(call("POST", "/api/v1/grant/finish", "{\"code\":\"123456\"}"), 401);
  T_EQ_INT(call("POST", "/api/v1/grant/revoke", "{\"permissions\":[\"power_off\"]}"),
           401);
}

static void test_info_permissions_all_allowed_when_available(void) {
  static const char *const names[] = {"power_actions", "power_off", "fan_control"};
  char want[96];
  setup();
  fake.caps.power_off = false;
  T_EQ_INT(call("GET", "/api/v1/info", NULL), 200);
  for (size_t i = 0; i < 3; i++) {
    const char *avail = (i == 1) ? "false" : "true";
    snprintf(want, sizeof(want), "\"%s\":{\"allowed\":%s,\"available\":%s}",
             names[i], avail, avail);
    T_ASSERT(strstr(body, want));
  }
  T_ASSERT(strstr(body, "\"power_actions\":true,\"config\""));
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
  T_RUN(test_suspend_codes);
  T_RUN(test_suspend_not_on);
  T_RUN(test_standby_codes);
  T_RUN(test_standby_unavailable_409);
  T_RUN(test_revoke_then_401);
  T_RUN(test_system_error_has_rc_hex);
  T_RUN(test_alloc_failure_is_500_internal);
  T_RUN(test_apps_rescanned_on_change);
  T_RUN(test_notify_codes);
  T_RUN(test_power_off_codes);
  T_RUN(test_power_off_unavailable_409);
  T_RUN(test_fan_codes);
  T_RUN(test_keep_awake_toggle);
  T_RUN(test_keep_awake_unavailable_409);
  T_RUN(test_state_has_power_state);
  T_RUN(test_state_outside_on_leaves_console_alone);
  T_RUN(test_gated_routes_refuse_outside_on);
  T_RUN(test_payloads_unavailable_without_manager);
  T_RUN(test_payloads_protected_is_403_without_contact);
  T_RUN(test_payloads_refused_while_app_runs);
  T_RUN(test_payloads_outside_on_leave_manager_alone);
  T_RUN(test_unauthenticated_pair_refused_outside_on);
  T_RUN(test_keep_awake_off_and_reads_work_outside_on);
  T_RUN(test_info_outside_on_makes_no_console_call);
  T_RUN(test_playtime_since_param);
  T_RUN(test_playtime_foreground_follows_capability);
  T_RUN(test_playtime_foreground_false_while_focus_fails);
  T_RUN(test_playtime_served_outside_on_without_console);
  T_RUN(test_info_has_playtime_capability);
  T_RUN(test_grant_start_answers_already_allowed);
  T_RUN(test_grant_finish_answers_no_code);
  T_RUN(test_grant_revoke_is_noop_200);
  T_RUN(test_grant_needs_token_401);
  T_RUN(test_info_permissions_all_allowed_when_available);
  apps_free(&apps);
  T_MAIN();
}
