# ps5-hass API v1

HTTP/1.1 on port 8181 (configurable), plain text, local network only. Every path starts with `/api/v1`. Bodies are
JSON in UTF-8, except the icon. Each request gets one answer, then the connection is closed (`Connection: close`).

Every response body is described in [`api/schema.json`](api/schema.json) (JSON Schema 2020-12, one `$defs` entry per
answer). The end-to-end tests and the Home Assistant integration validate against it.

## Finding the console

The payload announces `_ps5-hass._tcp.local` via mDNS:

| | |
|---|---|
| Instance | `PS5 <first 6 characters of device_id>` |
| TXT | `id=<device_id>`, `version=<payload version>`, `api=1`, `paired=0\|1` |

It announces again after waking up, after an address change and after pairing changes. Entering the IP address by
hand always works too. Use `device_id` (from `/info` or TXT `id`) as the stable identity of a console; it does not
change with the network interface or the IP address.

## Pairing

1. `POST /api/v1/pair/start` → the console shows `ps5-hass pairing code: 123456` on the TV for 120 s. The
   notification also appears with "Do Not Disturb" switched on.
2. `POST /api/v1/pair/finish` with `{"code":"123456","client_id":"<uuid>","label":"Home Assistant"}` → `{"token":"…"}`.
3. Send the token on every other request: `Authorization: Bearer <token>`. Never in the query string or a cookie.

Rules:
- One code at a time; a new code at most every 30 s (`429 rate_limited`).
- 5 wrong attempts use a code up (`409 code_exhausted`).
- 10 wrong attempts in a row, across codes, lock pairing for 15 minutes. Each further lock doubles, up to 24 hours
  (`429 locked`). A successful pairing resets the counter.
- At most 8 tokens. Pairing again with the same `client_id` replaces that client's token. When all 8 slots are used,
  other clients get `409 token_limit`.
- `client_id`: lowercase UUID. `label`: 1 to 64 characters.
- `POST /api/v1/pair/revoke` deletes the calling client's token.
- To reset all pairings, delete `/data/ps5-hass/config.json` over FTP and restart the payload.

## Browser guard

These checks run before anything else:

1. A request with an `Origin` header → `403 origin_forbidden`.
2. Every `POST` needs `Content-Type: application/json` and `X-PS5-Hass: 1`, and a JSON object as body (at least `{}`).
   Otherwise → `403 browser_guard`. Browsers cannot send these cross-origin without a preflight, which is never
   answered.
3. `Host` must be an IP address, a name ending in `.local`, or listed in `hosts` in `config.json` → otherwise
   `403 host_forbidden` (against DNS rebinding).
4. When `allow` in `config.json` is not empty, only those addresses (`"192.0.2.0/24"`, `"192.0.2.20"`) are
   accepted. An invalid `allow` entry blocks every address → `403 address_forbidden`.

There are no CORS headers.

## Endpoints

| Method and path | Token | Answer |
|---|---|---|
| `GET /info` | no | `name`, `version`, `commit`, `api` (1), `device_id`, `model`, `firmware`, `paired`, `power_actions` (`true` when the console can enter rest mode), `config` (`ok`/`recovered`), `capabilities{temps,fan,notify,random,launch,standby,mdns,power_off,fan_control}`, `permissions{power_actions,power_off,fan_control}` (each `{allowed,available}`; kept for v0.3 clients, `allowed` always equals `available`), `last_error` |
| `POST /pair/start` | no | `{"expires_in":120}` |
| `POST /pair/finish` | no | `{"token":"<64 hex>"}` |
| `POST /pair/revoke` | yes | `{}` |
| `GET /state` | yes | see below |
| `GET /apps` | yes | `{"etag":"…","apps":[{title_id,name,version,kind,category,has_icon}]}`; with `If-None-Match` → `304` |
| `GET /apps/<ID>/icon` | yes | PNG or JPEG (`Content-Type` from the file signature), `ETag`; with `If-None-Match` → `304` |
| `POST /apps/<ID>/launch` | yes | body `{"replace_running":false}` (optional) → `{"already_running":false,"app_id":24}` |
| `POST /apps/running/close` | yes | body `{"title_id":"PPSA…"}` (optional) |
| `POST /power/standby` | yes | rest mode; at most once per 10 s |
| `POST /power/off` | yes | power off (not rest mode; since v0.2); shares the 10 s limit with standby |
| `POST /notify` | yes | body `{"message":"Dinner is ready"}`: shown as a notification on the TV (UTF-8, cut to 200 bytes, control characters removed); at most 5 per 30 s (since v0.2) |
| `POST /fan` | yes | body `{"threshold_c":70}` (45..85) or `{"threshold_c":0}` (back to the console's own value); answer `{"fan":{…}}` (since v0.2) |
| `POST /grant/start` | yes | compatibility only: always `409 already_allowed` (see [Permissions](#permissions-removed-in-v04)) |
| `POST /grant/finish` | yes | compatibility only: always `409 no_code` |
| `POST /grant/revoke` | yes | compatibility only: `200` with `{"permissions":{…}}`, nothing is revoked |

Success answers carry `"result":"ok"`. `<ID>` must match `^[A-Z]{4}[0-9]{5}$`.

### `/state`

```json
{"result":"ok","in_app":true,
 "app":{"title_id":"PPSA96708","name":"Hikari","app_id":24},
 "temps":{"cpu":41,"soc":38},"fan_raw":131,
 "storage":{"data_free_mb":76731,"data_total_mb":838000,"user_free_mb":76731,"user_total_mb":838000},
 "uptime_s":284,"payload_uptime_s":120,"apps_etag":"8c1f…"}
```

- `app` is `null` when nothing runs.
- Every reading is `null` when the console function is missing or fails; `capabilities` in `/info` tells which
  functions exist.
- `fan_raw` is the raw value from the console. Reading it as "value / 10 ≈ percent" is an interpretation, not
  documented by Sony.
- When `apps_etag` changes, fetch `/apps` again.
- `payloads` (since v0.2): names of running payload processes (`kstuff.elf`, `ftpsrv.elf`, …), Sony's own
  processes left out; names longer than 19 characters are cut by the kernel.
- `fan` (since v0.2): `{"target_c": …, "current_c": …, "stock_c": …}` – our target (null = the console's own),
  the value the fan driver uses now, and the console's factory value (91 °C on FW 13.60), each null when unknown.

### Fan threshold (since v0.2)

The PS5's fan driver speeds up above a temperature threshold. `POST /fan` changes that threshold, the same way
[ps5upload](https://github.com/phantomptr/ps5upload) and ps5-fan-control do (`/dev/icc_fan`). There is no fan curve
and no direct speed control.

- Needs a console whose fan driver can be read (`capabilities.fan_control`), else `409 not_available`.
- Allowed: 45 to 85 °C, and **never above the console's own value** (`409 fan_above_stock`): the console must never
  run hotter than Sony intends. The factory value is learned once (the first reading that is not our own target)
  and kept in `config.json`.
- The console may reset the threshold at game starts (described by ps5upload; not seen on FW 13.60); the payload
  checks every 5 s and writes the target again. If a different tool sets another value, or the value keeps
  changing back (3 times within a minute), the payload stops and reports it in `last_error` instead of fighting.
- The factory value is never taken from a value the payload wrote itself, and readings above 91 °C (the highest
  known factory value) are treated as unknown. To learn it again, remove `fan_stock_c` from `config.json` and
  restart the payload.
- `{"threshold_c": 0}` writes the factory value back and stops overriding.

### Permissions (removed in v0.4)

There are no permissions. Pairing, with the code shown on the TV, is the only barrier: a paired client may enter rest
mode, power off and set the fan threshold, as far as the console can do it (`capabilities.standby`, `power_off`,
`fan_control`; a function the console lacks answers `409 not_available`). The `config.json` keys `power_actions`,
`power_off` and `fan_control` are ignored and left out the next time the file is saved. For v0.3 clients the payload
still answers: `/info.permissions` has `allowed` equal to `available`, `POST /grant/start` is `409 already_allowed`
(nothing to ask for), `POST /grant/finish` is `409 no_code`, and `POST /grant/revoke` is `200` without effect. These
fields and endpoints go away in a later version.

### `/apps`

- The list joins `/user/appmeta/<ID>/` and `/user/app/<ID>/sce_sys/` (homebrew often has only the latter). System
  apps (`NPXS…`) are left out.
- `name` is in the console's system language, then the title's default language, then `en-US`, then the first
  name in the file, then the title id.
- `kind` is `game` for retail games (`applicationDrmType` = `standard`) and `app` for everything else, including
  homebrew with a `PPSA…` id. PS4 titles (`CUSA…`) without readable metadata count as `game`.
- `version` and `category` are `null` when the title does not declare them.

## Launch and close

| Situation | Answer |
|---|---|
| The same app already runs | `200`, `already_running: true` |
| Another app runs, `replace_running` false | `409 other_app_running` |
| Another app runs, `replace_running` true | it is closed first, then the new app starts |
| Not installed | `404 not_installed` |
| No user logged in on the console | `409 no_user` |
| `close` with a `title_id` that is not the running app | `409 title_mismatch` |
| `close` while nothing runs | `409 no_app` |

## Errors

```json
{"result":"error","error":"wrong_code","message":"the code does not match","attempts_left":4}
```

| HTTP | `error` | Extra fields |
|---|---|---|
| 400 | `bad_request`, `bad_title_id` | |
| 401 | `unauthorized` (token missing or wrong), `wrong_code` | `attempts_left` for `wrong_code` |
| 403 | `origin_forbidden`, `browser_guard`, `host_forbidden`, `address_forbidden` | |
| 404 | `not_found`, `not_installed` | |
| 405 | `method_not_allowed` | |
| 409 | `no_code`, `code_exhausted`, `token_limit`, `other_app_running`, `no_user`, `title_mismatch`, `no_app`, `fan_above_stock`, `not_available`, `already_allowed` | |
| 411 | `length_required` (chunked body, or `POST` without `Content-Length`) | |
| 413 | `body_too_large` (more than 4096 bytes) | |
| 429 | `locked`, `rate_limited` | `retry_after` (seconds) |
| 431 | `headers_too_large` (more than 8 KB) | |
| 500 | `notify_failed`, `random_unavailable`, `system_error`, `internal` | `rc`, `rc_hex` for `system_error` |

`rc` is the console's return code (for example `-2137784315`), `rc_hex` the same as `0x80940005`.

## Limits

- At most 8 connections at the same time; a ninth is closed at once.
- The request must be complete within 3 s, the answer within 10 s.
- Headers up to 8 KB, body up to 4096 bytes, header names are case-insensitive.
- In rest mode the payload cannot be reached. Home Assistant detects rest mode through Sony's own status query
  (UDP 9302). After waking up, `/state` answers again within 10 s.
