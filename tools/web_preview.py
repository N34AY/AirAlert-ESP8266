#!/usr/bin/env python3
"""Local Web UI preview without a board: serves web/ plus an in-memory mock of
the device REST API (docs/API.md). Stdlib only.

  python3 tools/web_preview.py [--port 8080] [--alert]

Open http://127.0.0.1:8080/ and log in with any password. Edits to web/*.html
show up on page reload, no rebuild needed. Config saves live in memory only;
relay tests and played signals are printed to the console, and the dashboard
shows "РЕЛЕ УВІМК" while a tested signal would be sounding.
"""
import argparse
import http.server
import json
import pathlib
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
WEB = ROOT / "web"
VERSION = (ROOT / "VERSION").read_text().strip()

TYPES = ["air_raid", "artillery_shelling", "urban_fights", "chemical", "nuclear",
         "air_raid_yellow", "unknown"]
PRIORITY = {"nuclear": 90, "chemical": 80, "artillery_shelling": 60, "urban_fights": 55,
            "air_raid": 50, "air_raid_yellow": 45, "unknown": 40}
MAX_STEPS, MAX_STEP_MS, MAX_REPEAT = 12, 60000, 20


def default_config():  # mirrors AppConfig defaults (lib/core/src/airalert/Config.h)
    def sig(enabled, repeat, steps):
        return {"enabled": enabled, "repeat": repeat, "steps": steps}
    profiles = {}
    for t in TYPES:
        start = sig(True, 2, [1000, -1000]) if t == "air_raid_yellow" else sig(True, 3, [3000, -1000])
        reminder = sig(False, 2, [1000, -1000])
        reminder["interval_sec"] = 900
        profiles[t] = {"enabled": True, "priority": PRIORITY[t], "start": start,
                       "end": sig(True, 2, [1000, -1000]), "reminder": reminder}
    return {"schema": 2, "device": {"name": "AirAlert"},
            "alerts": {"poll_sec": 20, "stale_sec": 90, "start_conf": 1, "end_conf": 2,
                       "partial_active": True, "partial_siren": True, "partial_led": True,
                       "notify_escalation": True, "notify_additional_location": False,
                       "reminders_when_stale": False},
            "relay": {"single": False, "active_high": True, "max_on_ms": 30000, "test_ms": 600},
            "mute": {"snooze_min": 15, "all_types": False},
            "led": {"alert_steady": False, "alert_active_high": True},
            "locations": [{"uid": 14, "type": "oblast"}], "profiles": profiles}


def bad_signal(s):
    """Same limits the firmware enforces (validateConfig / patternFromJson)."""
    steps = s.get("steps", [])
    if not isinstance(steps, list) or len(steps) > MAX_STEPS:
        return "too many steps"
    if any(not isinstance(v, int) or isinstance(v, bool) or v == 0 or abs(v) > MAX_STEP_MS
           for v in steps):
        return "step out of range"
    if not 0 <= s.get("repeat", 1) <= MAX_REPEAT:
        return "repeat out of range"
    return None


def sounding_windows(steps, repeat, t0):
    """[(on_from, on_to)] in wall time for a played signal."""
    out, t = [], t0
    for _ in range(repeat):
        for v in steps:
            if v > 0:
                out.append((t, t + v / 1000))
            t += abs(v) / 1000
    return out


class State:
    cfg = default_config()
    alert = False
    muted = False
    windows = []
    started = time.time()
    log = []

    @classmethod
    def event(cls, name, detail=""):
        cls.log.append({"ts": int(time.time()), "event": name, "detail": detail})
        print(f"[{time.strftime('%H:%M:%S')}] {name} {detail}", flush=True)

    @classmethod
    def relay_on(cls):
        now = time.time()
        return any(a <= now < b for a, b in cls.windows)


class Handler(http.server.BaseHTTPRequestHandler):
    def send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def error(self, code, kind, message):
        self.send(code, {"error": {"code": kind, "message": message}})

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        try:
            return json.loads(raw or b"{}")
        except ValueError:
            return None

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/", "/index.html"):
            return self.send(200, (WEB / "index.html").read_bytes(), "text/html; charset=utf-8")
        if path == "/setup.html":
            return self.send(200, (WEB / "setup.html").read_bytes(), "text/html; charset=utf-8")
        if path == "/locations.json":
            return self.send(200, (WEB / "locations.json").read_bytes())
        if path == "/api/v1/config":
            return self.send(200, State.cfg)
        if path == "/api/v1/status":
            return self.send(200, self.status())
        if path == "/api/v1/events":
            data = "\n".join(json.dumps(r, ensure_ascii=False) for r in State.log[-200:])
            return self.send(200, data.encode(), "application/x-ndjson")
        if path == "/api/v1/system":
            return self.send(200, {"firmware": VERSION + " (web_preview)", "free_heap": 23456,
                                   "uptime_sec": int(time.time() - State.started)})
        self.error(404, "NOT_FOUND", "unknown endpoint")

    def status(self):
        types = [{"type": "air_raid", "coverage": "full",
                  "started_at": int(State.started)}] if State.alert else []
        return {"firmware": VERSION + "-preview", "uptime_sec": int(time.time() - State.started),
                "wifi": {"connected": True, "rssi": -58},
                "api": {"online": True, "stale": False, "ever_synced": True, "token_present": True,
                        "selected_locations": len(State.cfg.get("locations", [])),
                        "ntp_synced": True, "last_http_code": 200, "last_contact_sec": 4,
                        "waiting_reason": None},
                "alerts": {"active": State.alert, "muted": State.muted, "types": types,
                           "locations": [{"uid": 14, "threats": types}] if types else []},
                "relay": {"active": State.relay_on(), "tripped": False}}

    def do_PUT(self):
        body = self.body()
        if body is None:
            return self.error(400, "BAD_JSON", "invalid JSON body")
        if self.path == "/api/v1/config":
            for t, p in (body.get("profiles") or {}).items():
                for name in ("start", "end", "reminder"):
                    why = bad_signal(p.get(name, {}))
                    if why:
                        return self.error(422, "INVALID_CONFIG", f"{t}.{name}: {why}")
            State.cfg = body
            State.event("CONFIG_CHANGED", "source=web")
            return self.send(200, {"ok": True})
        if self.path == "/api/v1/locations":
            State.cfg["locations"] = body.get("locations", [])
            State.event("CONFIG_CHANGED", "locations")
            return self.send(200, {"ok": True})
        if self.path == "/api/v1/secrets/api-token":
            State.event("CONFIG_CHANGED", "api_token")
            return self.send(200, {"ok": True})
        self.error(404, "NOT_FOUND", "unknown endpoint")

    def do_POST(self):
        path = self.path.split("?")[0]
        if path == "/api/v1/login":
            return self.send(200, {"token": "preview"})
        if path == "/api/v1/test/pattern":
            body = self.body() or {}
            why = bad_signal(body) if isinstance(body.get("steps"), list) else "steps missing"
            if why or not any(v > 0 for v in body["steps"]):
                return self.error(422, "BAD_PATTERN", why or "no ON step")
            if State.relay_on():
                return self.error(409, "BUSY", "a siren pattern is playing")
            State.windows = sounding_windows(body["steps"], body.get("repeat", 1), time.time())
            State.event("TEST", f"pattern steps={body['steps']} repeat={body.get('repeat', 1)}")
            return self.send(200, {"ok": True})
        if path == "/api/v1/test/relay":
            State.event("TEST", "source=web " + self.path.partition("?")[2])
            return self.send(200, {"ok": True})
        if path == "/api/v1/relay/off":
            State.windows = []
            State.event("RELAY_PRESS", "relay=stop reason=web")
            return self.send(200, {"ok": True})
        if path in ("/api/v1/mute", "/api/v1/unmute"):
            State.muted = path.endswith("/mute")
            return self.send(200, {"ok": True})
        if path in ("/api/v1/ota/upload", "/api/v1/ota/url"):
            return self.error(501, "PREVIEW", "OTA is not available in web_preview")
        if path in ("/api/v1/reboot", "/api/v1/wifi/forget"):
            State.event("PREVIEW", path)
            return self.send(200, {"ok": True})
        self.error(404, "NOT_FOUND", "unknown endpoint")

    def log_message(self, *args):
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--alert", action="store_true", help="dashboard shows an active air raid")
    args = ap.parse_args()
    State.alert = args.alert
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"AirAlert Web UI preview: http://127.0.0.1:{args.port}/  (any password)", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
