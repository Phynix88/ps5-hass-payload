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
| `GET /info` | no | `name`, `version`, `commit`, `api` (1), `device_id`, `model`, `firmware`, `paired`, `power_actions` (`true` when the console can enter rest mode), `config` (`ok`/`recovered`), `capabilities{temps,fan,notify,random,launch,standby,mdns,power_off,fan_control,focus,power_state,suspend,keep_awake,soc_power,playtime,payload_control}`, `permissions{power_actions,power_off,fan_control}` (each `{allowed,available}`; kept for v0.3 clients, `allowed` always equals `available`), `last_error` |
| `POST /pair/start` | no | `{"expires_in":120}` |
| `POST /pair/finish` | no | `{"token":"<64 hex>"}` |
| `POST /pair/revoke` | yes | `{}` |
| `GET /state` | yes | see below |
| `GET /apps` | yes | `{"etag":"…","apps":[{title_id,name,version,kind,category,has_icon}]}`; with `If-None-Match` → `304` |
| `GET /apps/<ID>/icon` | yes | PNG or JPEG (`Content-Type` from the file signature), `ETag`; with `If-None-Match` → `304` |
| `POST /apps/<ID>/launch` | yes | body `{"replace_running":false}` (optional) → `{"already_running":false,"app_id":24}` |
| `POST /apps/running/close` | yes | body `{"title_id":"PPSA…"}` (optional) |
| `POST /apps/running/suspend` | yes | minimise the running game (it stays open, paused; the console then reports no running app, so `/state` shows `in_app` false until it is opened again from the console's home screen). Answer `{"result":"ok"}`. Nothing running: `409 not_running`; without `capabilities.suspend`: `409 not_available`; outside `on`: `409 not_on` (since v0.4) |
| `POST /power/standby` | yes | rest mode; at most once per 10 s |
| `POST /power/off` | yes | power off (not rest mode; since v0.2); shares the 10 s limit with standby |
| `POST /notify` | yes | body `{"message":"Dinner is ready"}`: shown as a notification on the TV (UTF-8, cut to 200 bytes, control characters removed); at most 5 per 30 s (since v0.2) |
| `POST /fan` | yes | body `{"threshold_c":70}` (45..85) or `{"threshold_c":0}` (back to the console's own value); answer `{"fan":{…}}` (since v0.2) |
| `POST /keep_awake` | yes | body `{"on":true}`; answer `{"keep_awake":true}`. While on, the payload resets the console's auto power-down timer every 240 s. Without `capabilities.keep_awake`: `409 not_available`. The setting lives in memory only (off after a payload restart) and does not stop rest mode started by a button or a request (since v0.4) |
| `GET /playtime` | yes | `?since=<unix seconds>` (default: now minus 8 days; not a number of 1 to 11 digits: `400 bad_request`). Play time per title, see [Play time](#play-time-since-v04) (since v0.4) |
| `GET /payloads` | yes | the payloads the Payload Manager has installed: `{"available":true,"payloads":[{"slot","name","version","running","protected"}]}`, see [Payloads](#payloads-since-v04). Payload Manager not reachable, or the console not `on`: `available` false and an empty list (since v0.4) |
| `POST /payloads/start` | yes | body `{"slot":"repo/ftpsrv@e8cce5d1"}`: starts that payload. `409 already_running`, `403 protected`, `404 unknown_payload`, `503 payload_manager_unavailable`; outside `on`: `409 not_on` (since v0.4) |
| `POST /payloads/stop` | yes | body `{"slot":"…"}`: stops it (kills its process). `409 not_running`, `403 protected`, `404 unknown_payload`, `503 payload_manager_unavailable`; outside `on`: `409 not_on` (since v0.4) |
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
 "soc_power":{"total_mw":48000,"rails":[{"mw":30000,"mv":1000,"ma":30000},{"mw":18000,"mv":1200,"ma":15000}]},
 "drives":[{"name":"m2","free_mb":500000,"total_mb":1000000}],
 "uptime_s":284,"payload_uptime_s":120,"apps_etag":"8c1f…",
 "power_state":"on","keep_awake":false}
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
- `power_state` (since v0.4): `on`, `going_to_rest`, `rest`, `shutting_down` or `unknown` (no reading, or a value
  that is not known; a failed read repeats the last state, see below). `keep_awake` (since v0.4): whether `POST /keep_awake` is on.
- `soc_power` (since v0.4): power drawn by the console's chip, from the console's own sensor: `total_mw` is the sum
  of the rails in milliwatts (about 48 W idle and 60 W in a game on a CFI-2116), `rails` the rails that carry power
  (`mw`, `mv`, `ma`). It is the chip only, not what the wall socket delivers. `null` without
  `capabilities.soc_power`, when the reading fails, or outside `on`. Read at most every 10 s; between reads the
  answer repeats the last reading.
- `drives` (since v0.4): external and extended storage that is present, as `{"name","free_mb","total_mb"}`:
  `m2` (`/mnt/ext1`), `extended` (`/mnt/ext0`), `usb0` … `usb7` (`/mnt/usb0` … `/mnt/usb7`). A mount point whose
  total is under 1024 MB counts as "no drive" and is left out, as is one that cannot be read; the list is empty
  when there is none, and outside `on`. Checked at most every 60 s. The internal storage stays in `storage`.
  There is no temperature for the M.2 drive.
- **Nothing is read from the console while `power_state` is not `on`.** The payload then calls no console function
  except the one that reports the power state. A `/state` answer in that time has `in_app` false, `app` null, `temps`, `fan_raw`, `soc_power` and all
  `storage` values null and `payloads` and `drives` empty; `uptime_s`, `payload_uptime_s`, `apps_etag` (the last known one) and
  `fan` (the last known values) come from memory or the clock. The fan watcher, the app rescan and keep-awake pause
  too, and continue when the state is `on` again. A console without the function that reports the power state
  counts as `on`. When reading it fails, the last state read stays in force (with none yet: not `on`); after 60 s of
  failed reads in a row it counts as `on` and `power_state` is `unknown`.
  See [Outside "on"](#outside-on) for what requests do then.

### Outside "on"

While `power_state` is not `on`, requests that would call a console function are refused with
`409 not_on`: `POST /apps/<ID>/launch`, `/apps/running/close`, `/apps/running/suspend`, `/power/standby`, `/power/off`, `/notify`, `/fan`,
`/payloads/start`, `/payloads/stop`, `/pair/start`, `/pair/finish` (pairing shows its code on the TV) and `/keep_awake` with `{"on":true}`. Switching
keep-awake off is always allowed. `GET /info`, `/state`, `/apps`, `/playtime` and `/payloads` (which then reports `available` false) keep working without any console call: `/info`
shows `model` and `firmware` as read while the console was `on` (empty strings if that has not happened yet),
`/apps` the last list, `GET /playtime` serves what is stored. A client that wants to act on a console in rest mode wakes it first (for example with the
PlayStation 5 integration) and retries. The payload's own notice about a busy port is skipped outside `on` and
sent once the console is `on` again.

### Play time (since v0.4)

The payload counts how long each title is **in the foreground** and keeps it in `data/ps5-hass/playtime.json` on the
console, so the count survives restarts of the payload and has no gaps while Home Assistant is down.

```json
{"result":"ok","foreground":true,
 "titles":[{"title_id":"PPSA96708","name":"Hikari","kind":"game","total_s":5400,"console_total_s":null}],
 "sessions":[{"title_id":"PPSA96708","kind":"game","start":1790000000,"duration_s":1800}],
 "current":{"title_id":"PPSA96708","kind":"game","start":1790003000,"duration_s":95}}
```

- `titles`: every title with its total, including the running session. `name` is the title id when the title is not
  installed. `kind` is `game` or `app` (as in `/apps`; a title the payload does not know counts as `app`, so it never
  adds to game time). `console_total_s` is the console's own count of play time for the title (Sony's `playedTimeOnConsole`, seconds,
  summed over all console users); it is `null` when unknown: the title is not in Sony's data, has no value there, the
  console has not been `on` since the payload started, or the file has not been readable yet. Sony's file is copied to
  `data/ps5-hass/app.db.copy` and read at most once an hour (a failed read keeps the last good values and is retried after a minute, then after twice as long per failure in a row, at most an hour), only while the console is `on`. It is Sony's
  number, not ours: it can differ from `total_s` (which only counts the foreground since the payload started
  counting).
- `sessions`: finished sessions that end at or after `since`; `start` is Unix time (UTC, the console's clock),
  `duration_s` seconds. `current` is the running session (`null` when none), `duration_s` as of the last sample.
  Time zone and day or week limits are the client's business.
- `foreground` is `capabilities.focus`, and false while the focus call keeps failing. A session counts only while the
  console is `on`, the title runs **and** has the focus (home screen and settings do not count; the control centre
  keeps the game's focus and does count; a minimised game does not run any more). Without `capabilities.focus` the
  payload cannot tell and counts "title open". The same holds after 3 failed focus reads in a row: until a focus read
  works again, "title open" counts and `foreground` is false.
- The payload samples every 5 s. A session ends at its last good sample when another title (or none) is in front,
  when a read from the console fails, when the console leaves `on`, after more than 30 s without a sample, or when
  the wall clock jumps against the monotonic one (NTP, waking up). A new one starts at the next good sample, so a
  duration is never negative or inflated. Durations come from the monotonic clock; a console call that blocks for a
  while (as after waking) does not count as a clock jump. A title once recorded as `game` stays a `game`.
- The running session is saved at least every 60 s and when the payload stops; after a crash at most a minute is
  lost. Sessions older than 400 days are folded into `total_s`; at most 20000 sessions and 2048 titles are kept (the
  oldest sessions go first, totals stay). A file in a wrong format or too large is renamed to
  `playtime.json.bad-<unix time>` (a second one never replaces the first), `last_error` says so, and counting starts
  again. A file that merely cannot be read right now (out of memory, no file handle) is left alone: nothing is written
  until a later attempt reads it, and what was counted meanwhile is added to it then.

### Payloads (since v0.4)

The payload starts and stops other payloads through the Payload Manager (its HTTP interface on `127.0.0.1:8084`,
the same one its web page uses). It talks to it only when asked, never in the background.

- `slot` is the Payload Manager's own key (for example `repo/ftpsrv@e8cce5d1`); use the value from `GET /payloads`.
  `name` is the Payload Manager's name for it, else its id, else the file name without extension and version;
  `version` is the Payload Manager's version string for it (`""` when it gives none). Slots that are not valid
  printable UTF-8 are left out of the list.
- `running` is true when a process of that payload exists. A process counts when its name (without `.elf`,
  case-insensitive, first 19 characters) equals the payload's id or its file name without extension and without a
  trailing version part (from the last `_` when a digit or `v` and a digit follows); a name the kernel cut
  inside `.elf` (19 characters ending in `.`, `.e` or `.el`) counts as without it. If the same payload is
  running several times, `stop` ends the first process found.
- `protected` is true for anything whose slot, id, file name, name or process name contains `kstuff`, `etahen`,
  `elfldr`, `pldmgr` or `ps5-hass` (case-insensitive). Such a payload is listed but never started or stopped
  (`403 protected`; the Payload Manager is not even asked), so the payload cannot switch off kstuff, the loader, the
  Payload Manager or itself.
- The Payload Manager must answer within 2 s in total (for start and stop the list lookup and the action together)
  with at most 256 KB of JSON; anything else counts as not reachable. `capabilities.payload_control` in `/info` is true when the last such call worked (false until the
  first one).
- Outside `on` the payload does not contact the Payload Manager at all (the console may be freezing processes):
  `GET /payloads` answers `available` false, start and stop `409 not_on`.

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
| `suspend` while nothing runs | `409 not_running` |

## Errors

```json
{"result":"error","error":"wrong_code","message":"the code does not match","attempts_left":4}
```

| HTTP | `error` | Extra fields |
|---|---|---|
| 400 | `bad_request`, `bad_title_id` | |
| 401 | `unauthorized` (token missing or wrong), `wrong_code` | `attempts_left` for `wrong_code` |
| 403 | `origin_forbidden`, `browser_guard`, `host_forbidden`, `address_forbidden`, `protected` | |
| 404 | `not_found`, `not_installed`, `unknown_payload` | |
| 405 | `method_not_allowed` | |
| 409 | `no_code`, `code_exhausted`, `token_limit`, `other_app_running`, `no_user`, `title_mismatch`, `no_app`, `not_running`, `fan_above_stock`, `not_available`, `already_allowed`, `not_on`, `already_running` | |
| 411 | `length_required` (chunked body, or `POST` without `Content-Length`) | |
| 413 | `body_too_large` (more than 4096 bytes) | |
| 429 | `locked`, `rate_limited` | `retry_after` (seconds) |
| 431 | `headers_too_large` (more than 8 KB) | |
| 500 | `notify_failed`, `random_unavailable`, `system_error`, `internal` | `rc`, `rc_hex` for `system_error` |
| 503 | `payload_manager_unavailable` | |

`rc` is the console's return code (for example `-2137784315`), `rc_hex` the same as `0x80940005`.

## Limits

- At most 8 connections at the same time; a ninth is closed at once.
- The request must be complete within 3 s, the answer within 10 s.
- Headers up to 8 KB, body up to 4096 bytes, header names are case-insensitive.
- In rest mode the payload cannot be reached. Home Assistant detects rest mode through Sony's own status query
  (UDP 9302). After waking up, `/state` answers again within 10 s.
