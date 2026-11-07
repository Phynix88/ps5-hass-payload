# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "make-payloads-json.py"


def run(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *args],
                          capture_output=True, text=True)


def test_fields_and_checksum(tmp_path):
    elf = tmp_path / "ps5-hass.elf"
    elf.write_bytes(b"\x7fELF test payload")
    r = run("--version", "v0.1.0", "--elf", str(elf),
            "--repo", "Phynix88/ps5-hass-payload", "--date", "2026-11-06")
    assert r.returncode == 0, r.stderr
    doc = json.loads(r.stdout)
    assert doc["name"] == "ps5-hass"
    (p,) = doc["payloads"]
    assert re.fullmatch(r"[a-z0-9._-]+", p["id"])
    assert p["id"] == "ps5-hass"
    assert p["filename"] == "ps5-hass.elf"
    assert p["url"] == ("https://github.com/Phynix88/ps5-hass-payload/releases/"
                        "download/v0.1.0/ps5-hass.elf")
    assert p["version"] == "v0.1.0"
    assert p["last_update"] == "2026-11-06"
    assert p["checksum"] == hashlib.sha256(elf.read_bytes()).hexdigest()
    assert p["category"] == "Networking"
    assert p["details_url"] == "https://github.com/Phynix88/ps5-hass-payload#readme"
    assert p["source"] == "https://github.com/Phynix88/ps5-hass-payload"
    for words in ("Home Assistant", "messages on the TV", "power off", "fan threshold",
                  "minimise", "play time", "keep awake", "start and stop payloads"):
        assert words in p["description"]


def test_rejects_bad_version(tmp_path):
    elf = tmp_path / "ps5-hass.elf"
    elf.write_bytes(b"x")
    r = run("--version", "1.0", "--elf", str(elf), "--repo", "a/b")
    assert r.returncode != 0
    r = run("--version", "v1.0", "--elf", str(elf), "--repo", "a/b")
    assert r.returncode != 0


def test_rejects_missing_elf(tmp_path):
    r = run("--version", "v0.1.0", "--elf", str(tmp_path / "nope.elf"),
            "--repo", "a/b")
    assert r.returncode != 0
