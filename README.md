# ps5-hass-payload

A payload for jailbroken PS5 consoles that connects the console to your own
[Home Assistant](https://www.home-assistant.io/). It reports what the console is doing, lists the installed games and
apps with their names and covers, starts and closes apps, shows messages from Home Assistant on the TV and reports
which payloads are running. If you allow it, it also puts the console into rest mode, powers it off and sets the
temperature at which the fan speeds up.

The Home Assistant side is a separate HACS integration, [ps5-hass](https://github.com/Phynix88/ps5-hass), that only
uses the HTTP API described in [API.md](API.md).

- No MQTT broker, no PSN account, no cloud.
- Pairing with a code shown on the TV, like a smart TV.
- Found automatically via mDNS; entering the IP address works too.

## Requirements

- PS5 with a jailbreak and an ELF loader. Tested on firmware 13.60 (Relapse, kstuff-lite 1.11, Payload Manager
  0.6.3). Other firmware versions should work; functions the firmware lacks show up as `null` (see `capabilities` in
  `/api/v1/info`).
- Home Assistant in the same local network.

## Installation

1. In the Payload Manager open **Manage Sources** and add
   `https://raw.githubusercontent.com/Phynix88/ps5-hass-payload/main/payloads.json`
   (or open `http://<ps5>:8084/sources_add?url=<that URL>`).
2. Install **PS5 Home Assistant bridge** and add it to the autoload.
3. Load it. It listens on port 8181.

Or download `ps5-hass.elf` from the [releases](https://github.com/Phynix88/ps5-hass-payload/releases) (check it
against `SHA256SUMS`) and load it with any ELF loader. A new start replaces a running copy.

## Pairing

In Home Assistant add the integration. It finds "PS5 …" on its own, or you enter the IP address. The console shows
`ps5-hass pairing code: 123456` on the TV; type the code into Home Assistant within two minutes. The notification
also appears with "Do Not Disturb" switched on.

Without Home Assistant (for testing):

```sh
PS5=192.168.1.50
curl -s -H 'Content-Type: application/json' -H 'X-PS5-Hass: 1' -d '{}' \
  http://$PS5:8181/api/v1/pair/start
curl -s -H 'Content-Type: application/json' -H 'X-PS5-Hass: 1' \
  -d '{"code":"123456","client_id":"0b7e2f1c-1111-4222-8333-944445555666","label":"test"}' \
  http://$PS5:8181/api/v1/pair/finish
curl -s -H "Authorization: Bearer <token>" http://$PS5:8181/api/v1/state
```

## Configuration and reset

Everything lives in `/data/ps5-hass/` on the console (reachable via FTP):

| File | Content |
|---|---|
| `config.json` | port, pairings, `allow` (address allow list), `hosts` (extra host names), `power_actions`, `power_off`, `fan_control`, `fan_target_c`, `fan_stock_c` |
| `device_id` | random id of this console for Home Assistant; survives a reset |
| `log.txt` | recent log, at most 64 KB together with `log.txt.1` |

- **Allow rest mode from Home Assistant:** set `"power_actions": true` in `config.json` and restart the payload.
- **Allow power off from Home Assistant:** set `"power_off": true` and restart the payload. Switching the console on
  again needs the power button (or the controller's PS button).
- **Allow the fan threshold from Home Assistant:** set `"fan_control": true` and restart the payload. The payload
  remembers the console's factory value in `fan_stock_c` (91 °C on FW 13.60) and never sets a higher one; allowed
  range 45–85 °C. `fan_target_c` is the threshold Home Assistant asked for (`0` = factory value). If the console
  resets the value, the payload sets it again; if another tool changes it, the payload stops and reports it in
  `last_error`. To learn the factory value again, remove `fan_stock_c` while no threshold is set.
- **Reset all pairings:** delete `config.json` and restart the payload.
- **Only allow certain addresses:** `"allow": ["192.168.1.0/24"]`. An invalid entry blocks everything, on purpose.
- A broken `config.json` is set aside as `config.json.broken-<time>`; the TV shows a notice and you pair again.

## Security and limits

Know these before you use it:

- Plain HTTP in the home network. Tokens are stored in plain text in `config.json`. Anyone who can reach the
  console over FTP already has full control of it.
- Jailbroken consoles already run services that let any device in the network control the console without a login
  (for example the ELF loader). ps5-hass does not add a way in for web pages: browser requests are refused (see
  [API.md](API.md#browser-guard)), pairing needs the code from the TV, and wrong codes lock pairing for a growing
  time.
- Not reachable in rest mode. Waking the console from rest mode is not supported (it needs Remote Play pairing with
  a PSN login). Home Assistant detects rest mode with Sony's own status query.
- PS4 titles (`CUSA…`) are listed with their title id as name; untested.
- No controller input, no screenshots, no controller battery (the console refuses pad access from a payload).
- Messages on the TV: at most 200 bytes, 5 per 30 seconds.
- Fan threshold: tested on FW 13.60 (CFI-2116). Other firmware versions may use other values; if reading the fan
  fails, `capabilities.fan_control` is `false`.

## Problems

Open an issue with the output of `curl http://<ps5>:8181/api/v1/info` (it contains `last_error` and
`capabilities`, no secrets) and, if possible, `/data/ps5-hass/log.txt`.

## Related projects

- [Marice/ps5-homeassistant](https://github.com/Marice/ps5-homeassistant): payload with many readings and events
  over MQTT. Needs an MQTT broker, has no pairing and no covers. ps5-hass needs no broker, pairs with a code, lists
  apps with covers and is set up in the Home Assistant UI.
- [FunkeyFlo/ps5-mqtt](https://github.com/FunkeyFlo/ps5-mqtt) and the official PlayStation Network integration
  work without a jailbreak; the game name comes from the PSN account. ps5-hass needs neither a PSN account nor the
  cloud.

## Building

Everything comes from `shell.nix` (PS5 payload SDK v0.45 with checksum, LLVM 21, meson, Python):

```sh
nix-shell --run 'scripts/build-microdns.sh build/microdns && make'   # ps5-hass.elf
nix-shell --run 'make -f Makefile.host test e2e'                      # unit + end-to-end tests
nix-shell --run 'make -f Makefile.host fuzz FUZZ_SECONDS=60'          # libFuzzer (Linux)
```

Design: [docs/specs](docs/specs/). Testing on a console: [TESTING.md](TESTING.md).

## License

GPL-3.0-or-later. Third-party code: [jsmn](third_party/jsmn/) (MIT), microdns (LGPL-2.1, built from source,
see `scripts/build-microdns.sh`).
