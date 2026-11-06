#!/usr/bin/env python3
# Writes the Payload Manager source (format: itsPLK/ps5-payload-manager
# CUSTOM_REPOSITORIES.md, object wrapper) for one release to stdout.
#   make-payloads-json.py --version vX.Y.Z --elf ps5-hass.elf --repo owner/name
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import datetime
import hashlib
import json
import pathlib
import re
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True, help="release tag, vX.Y.Z")
    ap.add_argument("--elf", required=True, type=pathlib.Path)
    ap.add_argument("--repo", required=True, help="owner/name on GitHub")
    ap.add_argument("--date", help="YYYY-MM-DD (default: today, UTC)")
    a = ap.parse_args()

    if not re.fullmatch(r"v\d+\.\d+\.\d+", a.version):
        sys.exit(f"version must look like v1.2.3, got {a.version!r}")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", a.repo):
        sys.exit(f"repo must look like owner/name, got {a.repo!r}")
    if not a.elf.is_file():
        sys.exit(f"{a.elf} does not exist")
    date = a.date or datetime.datetime.now(datetime.timezone.utc).date().isoformat()

    base = f"https://github.com/{a.repo}"
    doc = {
        "name": "ps5-hass",
        "payloads": [
            {
                "id": "ps5-hass",
                "name": "PS5 Home Assistant bridge",
                "filename": "ps5-hass.elf",
                "url": f"{base}/releases/download/{a.version}/ps5-hass.elf",
                "description": "Console state, installed apps with covers, launch and "
                               "close apps, rest mode - for Home Assistant (HACS "
                               "integration ps5-hass). Pair with a code shown on the TV.",
                "version": a.version,
                "last_update": date,
                "category": "Networking",
                "checksum": hashlib.sha256(a.elf.read_bytes()).hexdigest(),
                "author": "Phynix88",
                "details_url": f"{base}#readme",
                "source": base,
            }
        ],
    }
    json.dump(doc, sys.stdout, indent=2, ensure_ascii=False)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
