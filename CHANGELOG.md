# Changelog

## v0.4.0

Upgrading from v0.3: **there are no permissions any more.** Pairing with the code on the TV is the only step, and a
paired client may enter rest mode, power off and set the fan threshold. A console that had `power_actions`,
`power_off` or `fan_control` set to `false` (or not set) in `config.json` is therefore open to every paired client
after the upgrade. The three keys are ignored and dropped the next time the file is saved. If you do not want a
client to have this, remove its pairing.

- No permissions after pairing. `POST /grant/start`, `/grant/finish`, `/grant/revoke` and `/info.permissions` stay for
  v0.3 clients as compatibility answers without effect and go away in a later version.
- Power state: `/state.power_state` (`on`, `going_to_rest`, `rest`, `shutting_down`, `unknown`). Outside `on` the
  payload makes no console calls: requests that need one answer `409 not_on`, `/state` shows only what is in memory.
- Keep awake: `POST /keep_awake` resets the console's auto power-down timer while on; `/state.keep_awake`.
- Play time in the foreground: `GET /playtime` (titles, finished sessions, running session), counted by the payload
  and stored on the console in `playtime.json`; home screen and settings do not count, the control centre does.
- The console's own play time per title (`console_total_s`), read from a copy of its app database with the bundled
  SQLite (about 0.9 MB larger payload).
- Minimise the running game: `POST /apps/running/suspend`.
- `/state.soc_power` (power drawn by the console's chip) and `/state.drives` (M.2, extended and USB storage).
- Start and stop other payloads through the Payload Manager: `GET /payloads`, `POST /payloads/start`,
  `/payloads/stop`. kstuff, the ELF loader, the Payload Manager and ps5-hass are protected.
- New capabilities: `focus`, `power_state`, `suspend`, `keep_awake`, `soc_power`, `playtime`, `payload_control`.

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
