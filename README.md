# ps5-hass-payload

A payload for jailbroken PS5 consoles that connects the console to your own
[Home Assistant](https://www.home-assistant.io/). It reports what the console is doing, lists the installed games and
apps with their names and covers, starts and closes apps, shows messages from Home Assistant on the TV and reports
which payloads are running. If you allow it, it also puts the console into rest mode, powers it off and sets the
temperature at which the fan speeds up.

The Home Assistant side is a separate integration, [ps5-hass](https://github.com/Phynix88/ps5-hass), that only
uses the HTTP API described in [API.md](API.md).

- No MQTT broker, no PSN account, no cloud.
- Pairing with a code shown on the TV, like a smart TV.
- Found automatically via mDNS; entering the IP address works too.

![PS5 dashboard in Home Assistant](https://raw.githubusercontent.com/Phynix88/ps5-hass/main/docs/images/dashboard.png)

## Requirements

- PS5 with a jailbreak and an ELF loader. Tested on firmware 13.60 (Relapse, kstuff-lite 1.11, Payload Manager
  0.6.3). Other firmware versions should work; functions the firmware lacks show up as `null` (see `capabilities` in
  `/api/v1/info`).
- Home Assistant in the same local network, with the [ps5-hass integration](https://github.com/Phynix88/ps5-hass)
  (v0.3.0 or later for Console permissions).

## Installation

With the [Payload Manager](https://github.com/itsPLK/ps5-payload-manager) (tested with 0.6.3):

1. **Settings** → switch on **Multiple Payload Sources** (without it, the Payload Manager only shows its official
   repository).
2. **Settings** → **Manage Sources** → **Add Source**, paste
   `https://raw.githubusercontent.com/Phynix88/ps5-hass-payload/main/payloads.json` and press **Add**.
3. In the **Manage** tab open the catalog **ps5-hass** and install **PS5 Home Assistant bridge**.
4. Add it to the autoload and load it. It listens on port 8181. Updates show up in the same catalog.

Or download `ps5-hass.elf` from the [releases](https://github.com/Phynix88/ps5-hass-payload/releases) (check it
against `SHA256SUMS`) and send it to any ELF loader (elfldr listens on port 9021). A new start replaces a running
copy. Without an autoload you load it again after every console restart; pairings and permissions are kept in
`/data/ps5-hass/`.

**Restarting the payload** (for example after editing `config.json`): load it again, from the Payload Manager or the
ELF loader. The new copy replaces the running one.

### Removal

1. In Home Assistant turn fan control off (the console gets its factory threshold back) and delete the integration
   while the payload runs (this frees the pairing slot).
2. Payload Manager: take ps5-hass out of the autoload and uninstall it.
3. Restart the console (or stop the process in the Payload Manager).
4. Optionally delete `/data/ps5-hass/` over FTP.

If the payload stops while a fan threshold is set, the threshold stays until the console resets it (at the latest
after a restart).

## Pairing

Install the [ps5-hass integration](https://github.com/Phynix88/ps5-hass) first (Marketplace or HACS, see its README). Then add
the integration in Home Assistant. It finds "PS5 …" on its own, or you enter the IP address. The console shows
`ps5-hass pairing code: 123456` on the TV; type the code into Home Assistant within two minutes. The notification
also appears with "Do Not Disturb" switched on.

**Missed the code or too slow?** Nothing is lost, just ask for a new one: in the Home Assistant dialog tick
**"Show a new code"** and press *Submit*. The TV shows a fresh code (at most one new code every 30 seconds; if you
are too early, Home Assistant tells you how long to wait). An expired code is refused with "The code has expired",
then do the same. You can also close the dialog and start again from **Settings → Devices & services**.

- Each code works for 2 minutes and for 5 tries.
- 10 wrong codes in a row lock pairing for 15 minutes (each further lock doubles, up to 24 hours). Wait, or load the
  payload again (the lock is kept in memory only).

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

Everything lives in `/data/ps5-hass/` on the console (reachable via FTP, port 2121 with ftpsrv):

| File | Content |
|---|---|
| `config.json` | port, pairings, `allow` (address allow list), `hosts` (extra host names), `power_actions`, `power_off`, `fan_control`, `fan_target_c`, `fan_stock_c` |
| `device_id` | random id of this console for Home Assistant; survives a reset |
| `log.txt` | recent log, at most 64 KB together with `log.txt.1` |

- **Allow rest mode, power off or the fan threshold (since v0.3):** in Home Assistant open the PS5 integration's
  options → **Console permissions** and tick the box. The TV shows `ps5-hass: "Home Assistant" asks to allow …
  Code: 123456`; enter the code in Home Assistant. Unticking takes a permission away at once, without a code.
  Without Home Assistant: `POST /grant/start` and `/grant/finish` (see [API.md](API.md#permissions-since-v03)).
- **The same over FTP:** set `"power_actions"`, `"power_off"` or `"fan_control"` to `true` in `config.json` and
  restart the payload.
- **Power off:** switching the console on again needs the power button (or the controller's PS button).
- **Fan threshold:** the payload
  remembers the console's factory value in `fan_stock_c` (91 °C on FW 13.60) and never sets a higher one; allowed
  range 45–85 °C. `fan_target_c` is the threshold Home Assistant asked for (`0` = factory value). If the console
  resets the value, the payload sets it again; if another tool changes it, the payload stops and reports it in
  `last_error`; setting a threshold again takes over again. To learn the factory value again, remove `fan_stock_c`
  while no threshold is set and restart the payload.
- **Reset all pairings:** delete `config.json` and restart the payload. This also resets the permissions, port,
  allow list, host names and fan settings. To free a single slot, delete the integration in Home Assistant while the
  console is on.
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
- Not reachable in rest mode, and it cannot wake the console. Home Assistant detects rest mode with Sony's own status
  query. For waking from rest mode, the [PlayStation 5 integration](https://github.com/SH1FT-W/playstation-5) (Remote
  Play pairing) can run alongside ps5-hass.
- PS4 titles (`CUSA…`) are listed with their title id as name; untested.
- No controller input, no screenshots, no controller battery (the console refuses pad access from a payload).
- Messages on the TV: at most 200 bytes, 5 per 30 seconds.
- Fan threshold: tested on FW 13.60 (CFI-2116). Other firmware versions may use other values; if reading the fan
  fails, `capabilities.fan_control` is `false`.

## Problems

- **Home Assistant cannot reach the payload with a host name:** only IP addresses and `.local` names are accepted;
  add other names to `"hosts"` in `config.json`.
- **Not found automatically:** mDNS does not cross subnets; enter the IP address.
- **TV shows `ps5-hass: port 8181 busy, retrying`:** another payload uses the port. Stop it, or set `"port"` in
  `config.json` and enter that port in Home Assistant.
- **Gone after a console restart:** run the jailbreak again; the Payload Manager's autoload then starts ps5-hass.
- **TV shows `ps5-hass: config was broken, pair again`:** see *Configuration and reset*; pair again.
- **Payload Manager does not show the ps5-hass catalog:** switch on **Multiple Payload Sources** in its settings.

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

Only needed if you want to build the payload yourself; users download the ELF.

### With Nix (recommended)

[Nix](https://nixos.org/download/) is a package manager for any Linux and for macOS; NixOS is not required.
`shell.nix` pins the whole toolchain (PS5 payload SDK v0.45 with checksum, LLVM 21, meson, Python), so every build
uses the same tools as the release builds in CI:

```sh
nix-shell --run 'scripts/build-microdns.sh build/microdns && make'   # ps5-hass.elf
nix-shell --run 'make -f Makefile.host test e2e'                      # unit + end-to-end tests
nix-shell --run 'make -f Makefile.host fuzz FUZZ_SECONDS=60'          # libFuzzer (Linux)
```

### Without Nix

Tested on Ubuntu 24.04 (not part of CI). You need LLVM 21 (clang, ld.lld, llvm-ar, llvm-objcopy, llvm-strip, all in
the folder `llvm-config --bindir` prints), meson, ninja and the PS5 payload SDK v0.45:

```sh
sudo apt install git make meson ninja-build pkg-config unzip patch curl wget python3-pytest python3-jsonschema
wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh && sudo bash /tmp/llvm.sh 21     # LLVM 21 from apt.llvm.org

curl -LO https://github.com/ps5-payload-dev/sdk/releases/download/v0.45/ps5-payload-sdk.zip
echo "5fac64093d0e3e69f2584ce7935f24f703f622d9681f1495c85b9f00d1291a2c  ps5-payload-sdk.zip" | sha256sum -c
unzip -q ps5-payload-sdk.zip -d ~/ps5sdk

export PS5_PAYLOAD_SDK=~/ps5sdk/ps5-payload-sdk
export LLVM_CONFIG=/usr/lib/llvm-21/bin/llvm-config
scripts/build-microdns.sh build/microdns && make            # ps5-hass.elf
make -f Makefile.host test e2e CC=clang-21                  # tests; run them as a normal user, not as root
```

Other systems work the same way as long as `llvm-config --bindir` contains all the LLVM 21 tools (Homebrew, for
example, ships `lld` as a separate package; put symlinks to everything into one folder and point a small
`llvm-config` wrapper at it, as `shell.nix` does). Untested.

API: [API.md](API.md). Testing on a console: [TESTING.md](TESTING.md).

## License

GPL-3.0-or-later. Third-party code: [jsmn](third_party/jsmn/) (MIT), microdns (LGPL-2.1, built from source,
see `scripts/build-microdns.sh`), [SQLite](third_party/sqlite/) (public domain, amalgamation, unmodified).
