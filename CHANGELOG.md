# Changelog

## v0.1.0

First release.

- HTTP API `/api/v1`: console info, state (running app, temperatures, fan, storage, uptime), installed apps with
  names in the system language and covers, launch/switch/close apps, rest mode (opt-in).
- Pairing with a 6-digit code shown on the TV; tokens per client, failure budget with growing lock.
- Browser guard (Origin, JSON + marker header on POST, Host check), optional address allow list.
- mDNS announcement `_ps5-hass._tcp`.
- Tested on FW 13.60 (CFI-2116): see `TESTING.md`.
