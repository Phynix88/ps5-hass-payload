# Testing on the console

Unit tests, fuzzing and end-to-end tests run on Linux/macOS (`make -f Makefile.host test e2e`, `fuzz`). This file is
the checklist for a real PS5. It sends files to the console and changes its settings: use your own console only.

## Setup (port 8182 while ps5-status still uses 8181)

```sh
PS5=<console IP>
# config with the test port; the payload creates everything else
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
| 12 | `power_actions: true`, `POST /power/standby` | console goes to rest mode (separate yes) |
| 13 | `no_user` after waking via Remote Play | only if easy to set up |
| 14 | `POST /notify` with umlauts | message on the TV, umlauts correct |
| 15 | `/state.payloads` | kstuff, ftpsrv, ShadowMount, websrv listed; no Sony processes |
| 16 | `fan_control: true`, `POST /fan` 70, start a game, `POST /fan` 0 | 70 set, restored after the game start, back to 91 |
| 17 | `power_off: true`, `POST /power/off` (separate yes) | console powers off |
| 18 | HA options → Console permissions: untick power off, tick it again, enter the code from the TV | button disappears, code shown with the label, button comes back, `config.json` updated |
| 19 | untick fan control while a threshold is set | threshold back to the factory value (91 °C) |

## Results

| Date | Build | Passed | Not tested |
|---|---|---|---|
| 2026-11-06 | `5c8efbd` | 1–9, 12 | 10, 11, 13 |

Details and findings (e.g. the PS5's monotonic clock keeps running in rest mode; recovery happens through the
`accept` error path, errno 163) are in section 15 of the design spec.
