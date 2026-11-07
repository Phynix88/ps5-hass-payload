# End-to-end tests: the host server (fake console) against api/schema.json.
# Every row of the endpoint table in API.md has a test that
# checks the HTTP status and validates the JSON against the schema.
# SPDX-License-Identifier: GPL-3.0-or-later
import http.client
import json
import os
import pathlib
import shutil
import socket
import subprocess
import time

import jsonschema
import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HOST_BIN = ROOT / "build" / "ps5-hass-host"
SCHEMA = json.loads((ROOT / "api" / "schema.json").read_text())
CLIENT_ID = "0b7e2f1c-1111-4222-8333-944445555666"


def validate(name, doc):
    schema = dict(SCHEMA)
    schema["$ref"] = f"#/$defs/{name}"
    jsonschema.validate(doc, schema)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Console:
    """A running ps5-hass-host with its own fake root."""

    def __init__(self, tmp, env_extra=None, config_extra=None):
        self.root = tmp
        shutil.copytree(ROOT / "tests" / "data" / "apps", tmp, dirs_exist_ok=True)
        (tmp / "data" / "ps5-hass").mkdir(parents=True, exist_ok=True)
        self.port = free_port()
        (tmp / "data" / "ps5-hass" / "config.json").write_text(
            json.dumps({"schema": 1, "port": self.port, **(config_extra or {})})
        )
        env = dict(os.environ, PS5HASS_ROOT=str(tmp), **(env_extra or {}))
        self.proc = subprocess.Popen([str(HOST_BIN)], env=env)
        self.token = None
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                if self.get("/api/v1/info")[0] == 200:
                    return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("host server did not start")

    def stop(self):
        self.proc.terminate()
        self.proc.wait(timeout=5)

    def request(self, method, path, body=None, headers=None, guard=True):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        h = {"Host": f"127.0.0.1:{self.port}"}
        if self.token:
            h["Authorization"] = f"Bearer {self.token}"
        data = None
        if method == "POST":
            data = json.dumps({} if body is None else body).encode()
            if guard:
                h["Content-Type"] = "application/json"
                h["X-PS5-Hass"] = "1"
        h.update(headers or {})
        conn.request(method, path, body=data, headers=h)
        resp = conn.getresponse()
        raw = resp.read()
        conn.close()
        ctype = resp.getheader("Content-Type") or ""
        doc = json.loads(raw) if ctype == "application/json" else raw
        return resp.status, doc, resp

    def get(self, path, **kw):
        return self.request("GET", path, **kw)

    def post(self, path, body=None, **kw):
        return self.request("POST", path, body, **kw)

    def shown_code(self):
        return (self.root / "fake-notify.txt").read_text()[-6:]

    def pair(self):
        status, doc, _ = self.post("/api/v1/pair/start")
        assert status == 200, doc
        status, doc, _ = self.post(
            "/api/v1/pair/finish",
            {"code": self.shown_code(), "client_id": CLIENT_ID, "label": "HA"},
        )
        assert status == 200, doc
        self.token = doc["token"]


@pytest.fixture
def console(tmp_path):
    c = Console(tmp_path)
    yield c
    c.stop()


@pytest.fixture
def paired(console):
    console.pair()
    return console


def raw_request(console, data):
    with socket.create_connection(("127.0.0.1", console.port), timeout=5) as s:
        s.sendall(data)
        out = b""
        while chunk := s.recv(65536):
            out += chunk
    head, _, body = out.partition(b"\r\n\r\n")
    status = int(head.split(b" ")[1])
    return status, json.loads(body) if body else None


def test_info(console):
    status, doc, _ = console.get("/api/v1/info")
    assert status == 200
    validate("info", doc)
    assert doc["paired"] is False


def test_pairing(console):
    status, doc, _ = console.post("/api/v1/pair/start")
    assert status == 200
    validate("pair_start", doc)
    assert doc["expires_in"] == 120
    status, doc, _ = console.post(
        "/api/v1/pair/finish",
        {"code": "000000" if console.shown_code() != "000000" else "111111",
         "client_id": CLIENT_ID, "label": "HA"},
    )
    assert status == 401
    validate("error", doc)
    assert doc["error"] == "wrong_code" and doc["attempts_left"] == 4
    status, doc, _ = console.post(
        "/api/v1/pair/finish",
        {"code": console.shown_code(), "client_id": CLIENT_ID, "label": "HA"},
    )
    assert status == 200
    validate("pair_finish", doc)
    status, doc, _ = console.post("/api/v1/pair/start")
    assert status == 429
    validate("error", doc)
    assert doc["error"] == "rate_limited" and doc["retry_after"] > 0
    assert console.get("/api/v1/info")[1]["paired"] is True


def test_state_before_and_after_launch(paired):
    status, doc, _ = paired.get("/api/v1/state")
    assert status == 200
    validate("state", doc)
    assert doc["in_app"] is False and doc["app"] is None
    assert paired.post("/api/v1/apps/PPSA00001/launch")[0] == 200
    status, doc, _ = paired.get("/api/v1/state")
    validate("state", doc)
    assert doc["app"]["title_id"] == "PPSA00001"
    assert doc["app"]["name"] == "Test Game One US"


def test_apps_and_304(paired):
    status, doc, resp = paired.get("/api/v1/apps")
    assert status == 200
    validate("apps", doc)
    ids = [a["title_id"] for a in doc["apps"]]
    assert "PPSA00001" in ids and "NPXS00001" not in ids
    assert resp.getheader("ETag") == f'"{doc["etag"]}"'
    status, body, _ = paired.get(
        "/api/v1/apps", headers={"If-None-Match": f'"{doc["etag"]}"'}
    )
    assert status == 304 and body == b""


def test_icon_png_and_jpeg(paired):
    status, body, resp = paired.get("/api/v1/apps/PPSA00001/icon")
    assert status == 200 and resp.getheader("Content-Type") == "image/png"
    assert body.startswith(b"\x89PNG")
    etag = resp.getheader("ETag")
    assert paired.get("/api/v1/apps/PPSA00001/icon",
                      headers={"If-None-Match": etag})[0] == 304
    status, body, resp = paired.get("/api/v1/apps/PLDM00001/icon")
    assert status == 200 and resp.getheader("Content-Type") == "image/jpeg"
    status, doc, _ = paired.get("/api/v1/apps/TEST00007/icon")
    assert status == 404
    validate("error", doc)


def test_launch_conflict_and_replace(paired):
    status, doc, _ = paired.post("/api/v1/apps/PPSA00001/launch")
    assert status == 200
    validate("launch", doc)
    assert doc["already_running"] is False
    status, doc, _ = paired.post("/api/v1/apps/PPSA00001/launch")
    validate("launch", doc)
    assert doc["already_running"] is True
    status, doc, _ = paired.post("/api/v1/apps/PPSA00002/launch")
    assert status == 409 and doc["error"] == "other_app_running"
    validate("error", doc)
    status, doc, _ = paired.post("/api/v1/apps/PPSA00002/launch",
                                 {"replace_running": True})
    assert status == 200
    status, doc, _ = paired.post("/api/v1/apps/PPSA99999/launch")
    assert status == 404 and doc["error"] == "not_installed"
    status, doc, _ = paired.post("/api/v1/apps/bad/launch")
    assert status == 400 and doc["error"] == "bad_title_id"


def test_close_mismatch_and_ok(paired):
    status, doc, _ = paired.post("/api/v1/apps/running/close")
    assert status == 409 and doc["error"] == "no_app"
    paired.post("/api/v1/apps/PPSA00001/launch")
    status, doc, _ = paired.post("/api/v1/apps/running/close",
                                 {"title_id": "PPSA00002"})
    assert status == 409 and doc["error"] == "title_mismatch"
    validate("error", doc)
    status, doc, _ = paired.post("/api/v1/apps/running/close",
                                 {"title_id": "PPSA00001"})
    assert status == 200
    validate("ok", doc)


def test_revoke(paired):
    status, doc, _ = paired.post("/api/v1/pair/revoke")
    assert status == 200
    validate("ok", doc)
    status, doc, _ = paired.get("/api/v1/state")
    assert status == 401 and doc["error"] == "unauthorized"
    validate("error", doc)


def test_browser_guard(paired):
    status, doc, _ = paired.get("/api/v1/info", headers={"Origin": "http://x"})
    assert status == 403 and doc["error"] == "origin_forbidden"
    status, doc, _ = paired.post("/api/v1/pair/start", guard=False,
                                 headers={"Content-Type": "application/json"})
    assert status == 403 and doc["error"] == "browser_guard"
    status, doc, _ = paired.get("/api/v1/info", headers={"Host": "evil.example"})
    assert status == 403 and doc["error"] == "host_forbidden"
    validate("error", doc)


def test_limits(console):
    status, doc = raw_request(
        console,
        b"POST /api/v1/pair/start HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        b"Transfer-Encoding: chunked\r\n\r\n",
    )
    assert status == 411 and doc["error"] == "length_required"
    validate("error", doc)
    status, doc = raw_request(
        console,
        b"POST /api/v1/pair/start HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        b"Content-Type: application/json\r\nX-PS5-Hass: 1\r\n"
        b"Content-Length: 4097\r\n\r\n" + b" " * 4097,
    )
    assert status == 413 and doc["error"] == "body_too_large"
    status, doc = raw_request(
        console,
        b"GET /api/v1/info HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Big: "
        + b"a" * 9000 + b"\r\n\r\n",
    )
    assert status == 431 and doc["error"] == "headers_too_large"


def test_no_user(tmp_path):
    c = Console(tmp_path, {"PS5HASS_FAKE_NO_USER": "1"})
    try:
        c.pair()
        status, doc, _ = c.post("/api/v1/apps/PPSA00001/launch")
        assert status == 409 and doc["error"] == "no_user"
        validate("error", doc)
    finally:
        c.stop()


def test_notify(paired):
    status, doc, _ = paired.post("/api/v1/notify", {"message": "Essen ist fertig, Grüße"})
    assert status == 200
    validate("ok", doc)
    assert (paired.root / "fake-notify.txt").read_text() == "Essen ist fertig, Grüße"
    for _ in range(4):
        paired.post("/api/v1/notify", {"message": "x"})
    status, doc, _ = paired.post("/api/v1/notify", {"message": "x"})
    assert status == 429 and doc["error"] == "rate_limited"
    validate("error", doc)


def test_state_has_payloads_and_fan(paired):
    status, doc, _ = paired.get("/api/v1/state")
    validate("state", doc)
    assert doc["payloads"] == []
    assert doc["fan"] == {"target_c": None, "current_c": 91, "stock_c": 91}


def test_power_and_fan_allowed_after_pairing(paired):
    info = paired.get("/api/v1/info")[1]
    validate("info", info)
    caps = info["capabilities"]
    assert caps["standby"] and caps["power_off"] and caps["fan_control"]
    assert info["power_actions"] is True
    status, doc, _ = paired.post("/api/v1/fan", {"threshold_c": 70})
    assert status == 200
    validate("fan", doc)
    assert doc["fan"] == {"target_c": 70, "current_c": 70, "stock_c": 91}
    status, doc, _ = paired.post("/api/v1/fan", {"threshold_c": 95})
    assert status == 400
    status, doc, _ = paired.post("/api/v1/fan", {"threshold_c": 0})
    assert status == 200 and doc["fan"]["current_c"] == 91
    state = paired.get("/api/v1/state")[1]
    validate("state", state)
    assert state["fan"]["stock_c"] == 91
    status, doc, _ = paired.post("/api/v1/power/standby")
    assert status == 200
    validate("ok", doc)
    status, doc, _ = paired.post("/api/v1/power/off")
    assert status == 429 and doc["error"] == "rate_limited"  # shares the 10 s limit
    validate("error", doc)


def test_old_permission_keys_in_config_are_ignored(tmp_path):
    c = Console(tmp_path, config_extra={"power_actions": False, "power_off": False,
                                        "fan_control": False})
    try:
        c.pair()
        status, doc, _ = c.post("/api/v1/power/off")
        assert status == 200
        validate("ok", doc)
    finally:
        c.stop()


def test_grant_endpoints_compat(paired):
    info = paired.get("/api/v1/info")[1]
    validate("info", info)
    for name in ("power_actions", "power_off", "fan_control"):
        assert info["permissions"][name] == {"allowed": True, "available": True}
    shown = paired.shown_code()
    status, doc, _ = paired.post("/api/v1/grant/start",
                                 {"permissions": ["power_off", "fan_control"]})
    assert status == 409 and doc["error"] == "already_allowed"
    validate("error", doc)
    assert paired.shown_code() == shown  # no new code on the TV
    status, doc, _ = paired.post("/api/v1/grant/finish", {"code": "123456"})
    assert status == 409 and doc["error"] == "no_code"
    validate("error", doc)
    status, doc, _ = paired.post("/api/v1/grant/revoke", {"permissions": ["power_off"]})
    assert status == 200
    validate("grant_revoke", doc)
    assert doc["permissions"]["power_off"] == {"allowed": True, "available": True}
    status, doc, _ = paired.post("/api/v1/power/off")
    assert status == 200
    paired.token = None
    status, doc, _ = paired.post("/api/v1/grant/start", {"permissions": ["power_off"]})
    assert status == 401
