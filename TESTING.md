# Testing on the console

Unit tests, fuzzing and end-to-end tests run on Linux/macOS (`make -f Makefile.host test e2e`, `fuzz`). This file is
the checklist for a real PS5. It sends files to the console and changes its settings: use your own console only.

## Setup (test port 8182, so a running copy on 8181 is not disturbed)

```sh
PS5=<console IP>
# config with the test port (overwrites config.json: pairings are lost)
printf '{"schema":1,"port":8182}' > /tmp/ps5-hass-config.json
curl -s --ftp-create-dirs -T /tmp/ps5-hass-config.json ftp://$PS5:2121/data/ps5-hass/config.json
curl -s --ftp-create-dirs -T ps5-hass.elf ftp://$PS5:2121/data/ps5-hass-test/ps5-hass.elf
# start once (not in the autoload)
curl -s "http://$PS5:8080/hbldr?pipe=0&daemon=1&path=/data/ps5-hass-test/ps5-hass.elf"
# stop
pid=$(curl -s http://$PS5:8084/processes_list | jq '.processes[] | select(.name=="ps5-hass.elf") | .pid')
curl -s -X POST -H "X-PLDMGR-Request: 1" "http://$PS5:8084/process_kill?pid=$pid"
```

Requests from a shell need the browser-guard headers on every POST:
`-H 'Content-Type: application/json' -H 'X-PS5-Hass: 1' -d '{}'`.

Diagnostics: `/api/v1/info` (`last_error`, `capabilities`) and `ftp://$PS5:2121/data/ps5-hass/log.txt`.

## Checklist

| # | Check | Expected |
|---|---|---|
| 1 | `GET /info` | 200, `firmware` 13.60, `model` CFI-…, `capabilities` all true except where noted |
| 2 | `pair/start` | code visible on the TV (also with "Do Not Disturb") |
| 3 | 5 wrong codes, then the right one | `wrong_code` 4…0, then `code_exhausted` |
| 4 | new code after 30 s, correct | token |
| 5 | `GET /apps` | all installed titles, names in the system language, retail games `kind: game`, Payload Manager icon `image/jpeg` |
| 6 | `GET /state` | running app with title and name, temperatures, storage |
| 7 | launch A, launch A again, launch B without/with `replace_running`, close | 200 / `already_running` / 409 / 200 / 200 |
| 8 | `dns-sd -B _ps5-hass._tcp` on a Mac | `PS5 <id>` with `api=1`, `paired=1` |
| 9 | rest mode via the controller, wake up | `/state` answers within 10 s after waking |
| 10 | cable ↔ Wi-Fi | new address announced via mDNS |
| 11 | restart with autoload | payload comes up, pairing kept |
| 12 | `POST /power/standby` (no permission to set up since v0.4) | console goes to rest mode (separate yes) |
| 13 | `no_user` after waking via Remote Play | only if easy to set up |
| 14 | `POST /notify` with umlauts | message on the TV, umlauts correct |
| 15 | `/state.payloads` | kstuff, ftpsrv, ShadowMount, websrv listed; no Sony processes |
| 16 | `POST /fan` 70, start a game, `POST /fan` 0 | 70 set, restored after the game start, back to 91 |
| 17 | `POST /power/off` (separate yes) | console powers off |
| 18 | upgrade from a v0.3 `config.json` that has `power_actions`, `power_off`, `fan_control`; `GET /info`, then change a setting that saves the file (pair another client) | `permissions` all `allowed` true; the three keys gone from `config.json` afterwards |
| 19 | `POST /grant/start`, `/grant/finish`, `/grant/revoke` | `409 already_allowed`, `409 no_code`, `200` without effect |
| 20 | `GET /state` with the game in front; then home screen, settings, back to the game, control centre over the game, `GET /playtime` after each | `in_app` true; home screen and settings: session ends, nothing added; control centre: still counted; `current` is `null` outside the foreground |
| 21 | game in front for 2 minutes, restart the payload, `GET /playtime` | `total_s` kept; at most a minute lost; `console_total_s` a number once the console has been `on` for a while, otherwise `null` |
| 22 | `POST /apps/running/suspend` with a game running, then without | `200`; the game is paused and `/state` shows `in_app` false, no running app; then `409 not_running` |
| 23 | with no game running: `GET /payloads`; stop the FTP payload (`POST /payloads/stop` with its slot), check that FTP no longer answers, start it again, check that it answers | list with `running` true for it; stop and start `200`; second stop or start in a row `409 not_running` / `409 already_running`; kstuff, etaHEN, the loader, the Payload Manager, ps5-hass and ps5debug `protected` (`403 protected` on a request), CheatRunner not; with no game in memory CheatRunner can be started and stopped (`200`) |
| 24 | `POST /keep_awake` `{"on":true}`, leave the console idle for longer than the auto rest time set in the console's power settings, then `{"on":false}` | console stays on while switched on; goes to rest mode after switching off and waiting the auto rest time |
| 25 | `/state`: `soc_power`, `drives` with an M.2 or USB drive attached and without | `total_mw` plausible (tens of watts), drives with `free_mb` and `total_mb`; list empty without |
| 26 | rest mode **with v0.4 alone** (no other payload that keeps the console awake), twice: first let the console fall asleep (auto rest time or `POST /power/standby`), then once more and wake it with the controller; `GET /state` and `/info` while it rests (if reachable), after waking | no crash, no console freeze; while not `on`: `409 not_on` for requests that need the console, no console calls; after waking `/state` answers within 10 s and play time continues without a long session across the rest |
| 27 | with a game running (CheatRunner loaded, if you use it): `POST /payloads/stop` and `/payloads/start` with the FTP payload's slot, then with the CheatRunner slot; `GET /payloads`; then minimise the game (`POST /apps/running/suspend`) and repeat | `409 app_running` for both slots, also with the game minimised; nothing stops, the console stays up; with the game closed (no `eboot.bin` process in the Payload Manager's list) stop and start work again, CheatRunner included |

## Known console behaviour

- The PS5's monotonic clock keeps running in rest mode; after waking up, the payload recovers through the `accept`
  error path (errno 163).
