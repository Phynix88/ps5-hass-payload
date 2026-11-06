# Changelog

## v0.3.1

- Client labels: control characters become spaces when pairing, in the log and in the grant text on the TV.
- Permission log lines name the client (`permissions: granted power off (by "Home Assistant")`).
- 403 messages for rest mode, power off and fan control name the Home Assistant options as the first way.

## v0.3.0

- Grant rest mode, power off and the fan threshold from a paired client with a code shown on the TV:
  `POST /grant/start`, `/grant/finish`; take them away without a code: `POST /grant/revoke`. Editing `config.json`
  over FTP still works.
- Grant codes have their own slot and failure budget, separate from pairing; a code belongs to the client that asked.
- `/info.permissions`: for each permission `allowed` and `available`.
- Taking fan control away writes the console's factory threshold back.

## v0.2.0

- `POST /notify`: messages from Home Assistant as notifications on the TV (UTF-8, 5 per 30 s).
- `/state.payloads`: running payloads (kstuff, ftpsrv, ShadowMount, …), Sony processes left out.
- `POST /power/off`: power off, with its own permission `power_off` in `config.json`.
- `POST /fan` and `/state.fan`: fan threshold 45–85 °C, never above the console's factory value, kept after game
  starts, stops when another tool controls the fan; permission `fan_control` in `config.json`.
- Controller battery was investigated and dropped: the console refuses pad access from a payload.

## v0.1.0

First release.

- HTTP API `/api/v1`: console info, state (running app, temperatures, fan, storage, uptime), installed apps with
  names in the system language and covers, launch/switch/close apps, rest mode (opt-in).
- Pairing with a 6-digit code shown on the TV; tokens per client, failure budget with growing lock.
- Browser guard (Origin, JSON + marker header on POST, Host check), optional address allow list.
- mDNS announcement `_ps5-hass._tcp`.
- Tested on FW 13.60 (CFI-2116): see `TESTING.md`.
