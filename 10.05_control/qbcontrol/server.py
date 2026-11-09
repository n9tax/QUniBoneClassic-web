# server.py - qbcontrold: HTTP daemon with JSON API and the web panel.
#
#   python3 -m qbcontrol.server [--port N] [--bind ADDR] [--no-autostart]
#
# Runs as root on the BeagleBone (demo needs root for the PRUs).

import argparse
import base64
import json
import mimetypes
import os
import signal
import socket
import socketserver
import subprocess
import sys
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs, unquote

from . import VERSION, core, autostart
from .runner import Runner

_log_lock = threading.Lock()


def log(msg):
    line = "%s %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg)
    with _log_lock:
        sys.stderr.write(line + "\n")
        sys.stderr.flush()
        try:
            with open(core.LOG_FILE, "a") as f:
                f.write(line + "\n")
        except (IOError, OSError):
            pass


RUNNER = Runner(log=log)
SETTINGS_LOCK = threading.Lock()


def ip_addresses():
    try:
        out = subprocess.check_output(["hostname", "-I"], stderr=subprocess.DEVNULL, timeout=2)
        ips = out.decode().split()
        if ips:
            return ips
    except Exception:
        pass
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ip = s.getsockname()[0]
        s.close()
        return [ip]
    except Exception:
        return []


def system_status():
    settings = core.load_settings()
    platform = core.detect_platform(settings)
    a = settings["autostart"]
    st = {
        "version": VERSION,
        "platform": platform,
        "address_width": settings.get("address_width") if platform == "QBUS" else 18,
        "hostname": socket.gethostname(),
        "ips": ip_addresses(),
        "port": settings.get("http_port", core.DEFAULT_PORT),
        "home": core.QUNIBONE_HOME,
        "data_dir": core.DATA_DIR,
        "demo_bin": core.DEMO_BIN,
        "demo_ok": os.path.exists(core.DEMO_BIN),
        "is_root": hasattr(os, "geteuid") and os.geteuid() == 0,
        "fake_demo": bool(os.environ.get("QBC_DEMO")),
        "password": bool(settings.get("password_hash")),
        "switches": autostart.read_switches(),
        "autostart": {"mode": a.get("mode"), "target": a.get("target"),
                      "target_label": core.describe_target(a.get("target"), settings)
                      if a.get("target") else ""},
        "runner": RUNNER.status(),
    }
    return st


# ---------------------------------------------------------------------------
# Request handler
# ---------------------------------------------------------------------------

class ApiError(Exception):
    def __init__(self, code, msg):
        Exception.__init__(self, msg)
        self.code = code


class Handler(BaseHTTPRequestHandler):
    server_version = "qbcontrold/" + VERSION
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass   # quiet; errors are logged explicitly

    # ---- plumbing -------------------------------------------------------

    def _send(self, code, body, ctype="application/json", extra=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj))

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n > 4 * 1024 * 1024:
            raise ApiError(413, "Request too large")
        raw = self.rfile.read(n) if n else b""
        if not raw:
            return {}
        try:
            return json.loads(raw.decode("utf-8"))
        except ValueError:
            raise ApiError(400, "Body is not valid JSON")

    def _authorized(self):
        settings = core.load_settings()
        if not settings.get("password_hash"):
            return True
        if self.headers.get("X-QBC-Token") == core.local_token():
            return True
        auth = self.headers.get("Authorization") or ""
        if auth.startswith("Basic "):
            try:
                user, _, pw = base64.b64decode(auth[6:].strip()).decode("utf-8").partition(":")
            except Exception:
                return False
            return core.check_password(settings, pw)
        return False

    def _dispatch(self):
        url = urlparse(self.path)
        path = unquote(url.path)
        query = parse_qs(url.query)
        if not self._authorized():
            self._send(401, json.dumps({"error": "Password required"}),
                       extra={"WWW-Authenticate": 'Basic realm="QBone control panel"'})
            return
        try:
            if path.startswith("/api/"):
                self._api(self.command, path[5:].strip("/").split("/"), query)
            elif self.command in ("GET", "HEAD"):
                self._static(path)
            else:
                raise ApiError(405, "Method not allowed")
        except ApiError as e:
            self._json({"error": str(e)}, e.code)
        except core.QbcError as e:
            self._json({"error": str(e)}, 400)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:
            log("ERROR %s %s: %s\n%s" % (self.command, self.path, e, traceback.format_exc()))
            try:
                self._json({"error": "Internal error: %s" % e}, 500)
            except Exception:
                pass

    do_GET = do_POST = do_PUT = do_DELETE = do_HEAD = _dispatch

    def _static(self, path):
        if path in ("", "/"):
            path = "/index.html"
        full = os.path.normpath(os.path.join(core.WWW_DIR, path.lstrip("/")))
        if not full.startswith(core.WWW_DIR + os.sep) or not os.path.isfile(full):
            raise ApiError(404, "Not found")
        ctype = mimetypes.guess_type(full)[0] or "application/octet-stream"
        if ctype.startswith("text/") or ctype in ("application/javascript",):
            ctype += "; charset=utf-8"
        with open(full, "rb") as f:
            self._send(200, f.read(), ctype)

    # ---- API ------------------------------------------------------------

    def _api(self, method, parts, query):
        head = parts[0] if parts else ""
        q1 = lambda k, d=None: (query.get(k) or [d])[0]

        if head == "status" and method == "GET":
            return self._json(system_status())

        if head == "catalog" and method == "GET":
            return self._json(core.catalog(core.detect_platform(core.load_settings())))

        # -- configurations
        if head == "configs":
            if len(parts) == 1:
                if method == "GET":
                    return self._json({"configs": core.list_configs()})
                if method == "POST":
                    b = self._body()
                    return self._json(create_config(b))
            elif len(parts) == 2:
                cid = parts[1]
                if method == "GET":
                    cfg = core.load_config(cid)
                    return self._json({"config": cfg})
                if method == "PUT":
                    b = self._body()
                    cfg = b.get("config", b)
                    core.save_config(cid, cfg)
                    return self._json({"ok": True, "id": cid})
                if method == "DELETE":
                    if RUNNER.running() and (RUNNER.status().get("target") == "config:" + cid):
                        raise ApiError(409, "Configuration is running")
                    core.delete_config(cid)
                    return self._json({"ok": True})

        if head == "preview" and method == "POST":
            b = self._body()
            cfg = b.get("config", b)
            platform = core.detect_platform(core.load_settings())
            try:
                return self._json({"cmd": core.generate_cmd(cfg, platform, b.get("id") or "preview")})
            except core.QbcError as e:
                return self._json({"cmd": "", "error": str(e)})

        # -- stock scripts
        if head == "stock":
            platform = core.detect_platform(core.load_settings())
            if len(parts) == 1 and method == "GET":
                return self._json({"stock": core.scan_stock(platform), "apps_dir": core.APPS_DIR})
            if len(parts) == 2 and parts[1] == "cmd" and method == "GET":
                info = core.find_stock(platform, q1("id", ""))
                return self._json({"cmd": core.read_stock_cmd(info), "info": info})
            if len(parts) == 2 and parts[1] == "copy" and method == "POST":
                b = self._body()
                info = core.find_stock(platform, b.get("id", ""))
                return self._json(create_config({
                    "name": b.get("name") or ("%s (copy)" % info["name"]),
                    "kind": "raw", "cmd_text": core.read_stock_cmd(info),
                    "workdir": info["rundir"],
                    "description": info.get("title", "")}))

        # -- run control
        if head == "start" and method == "POST":
            b = self._body()
            target = b.get("target", "")
            spec = core.launch_spec(target, core.load_settings(), leds=b.get("leds"))
            return self._json(RUNNER.start(spec))
        if head == "stop" and method == "POST":
            b = self._body()
            return self._json(RUNNER.stop(force=bool(b.get("force"))))

        if head == "console":
            if method == "GET":
                since = q1("since")
                since = int(since) if since not in (None, "") else None
                wait = min(float(q1("wait", "0") or 0), 25.0)
                data, end, base = RUNNER.read(since, wait)
                return self._json({"data": data.decode("latin-1"), "next": end, "base": base,
                                   "state": RUNNER.state, "session": RUNNER.session})
            if method == "POST":
                b = self._body()
                RUNNER.send(b.get("text", ""))
                return self._json({"ok": True})

        # -- autostart
        if head == "autostart":
            if len(parts) == 1 and method == "GET":
                s = core.load_settings()
                target, leds, reason = autostart.resolve(s)
                refs = set(v for v in (s["autostart"].get("switch_map") or {}).values() if v)
                if s["autostart"].get("target"):
                    refs.add(s["autostart"]["target"])
                labels = dict((t, core.describe_target(t, s)) for t in refs)
                return self._json({"autostart": s["autostart"], "switches": autostart.read_switches(),
                                   "labels": labels,
                                   "would_start": target, "would_start_label":
                                   core.describe_target(target, s) if target else "",
                                   "reason": reason})
            if len(parts) == 1 and method == "PUT":
                b = self._body()
                with SETTINGS_LOCK:
                    s = core.load_settings()
                    a = s["autostart"]
                    if b.get("mode") not in ("off", "fixed", "switches"):
                        raise ApiError(400, "mode must be off, fixed or switches")
                    a["mode"] = b["mode"]
                    a["target"] = b.get("target", "") or ""
                    sm = b.get("switch_map", {}) or {}
                    a["switch_map"] = dict((str(k), v) for k, v in sm.items()
                                           if v and str(k).isdigit() and 1 <= int(k) <= 15)
                    a["delay_s"] = max(0, min(600, int(b.get("delay_s", 5) or 0)))
                    a["leds"] = bool(b.get("leds", True))
                    core.save_settings(s)
                log("autostart set: mode=%s target=%s" % (a["mode"], a["target"]))
                return self._json({"ok": True})
            if len(parts) == 2 and parts[1] == "run" and method == "POST":
                return self._json(run_autostart(delay=False))

        if head == "images" and method == "GET":
            return self._json({"images": core.scan_images(core.load_settings()),
                               "image_dir": core.IMAGE_DIR})

        # -- settings
        if head == "settings":
            if method == "GET":
                s = core.load_settings()
                return self._json({
                    "platform": s.get("platform"), "detected": core.detect_platform(None),
                    "address_width": s.get("address_width"), "verbose": s.get("verbose"),
                    "http_port": s.get("http_port"), "http_bind": s.get("http_bind"),
                    "image_dirs": s.get("image_dirs", []),
                    "password": bool(s.get("password_hash"))})
            if method == "PUT":
                b = self._body()
                with SETTINGS_LOCK:
                    s = core.load_settings()
                    if "platform" in b:
                        if b["platform"] not in ("auto", "QBUS", "UNIBUS"):
                            raise ApiError(400, "platform must be auto, QBUS or UNIBUS")
                        s["platform"] = b["platform"]
                    if "address_width" in b:
                        aw = int(b["address_width"])
                        if aw not in (16, 18, 22):
                            raise ApiError(400, "address width must be 16, 18 or 22")
                        s["address_width"] = aw
                    if "verbose" in b:
                        s["verbose"] = bool(b["verbose"])
                    if "http_port" in b:
                        port = int(b["http_port"])
                        if not 1 <= port <= 65535:
                            raise ApiError(400, "bad port")
                        s["http_port"] = port
                    if "image_dirs" in b:
                        s["image_dirs"] = [d for d in b["image_dirs"] if isinstance(d, str) and d]
                    if "password" in b:      # "" clears the password
                        core.set_password(s, b["password"] or "")
                    core.save_settings(s)
                return self._json({"ok": True})

        if head == "log" and method == "GET":
            try:
                with open(core.LOG_FILE) as f:
                    lines = f.readlines()[-200:]
            except (IOError, OSError):
                lines = []
            return self._json({"log": "".join(lines)})

        raise ApiError(404, "No such API: %s %s" % (method, "/".join(parts)))


def create_config(b):
    name = (b.get("name") or "").strip()
    if not name:
        raise ApiError(400, "Name required")
    platform = core.detect_platform(core.load_settings())
    kind = b.get("kind", "builder")
    if b.get("clone"):
        cfg = core.load_config(b["clone"])
        cfg.pop("id", None)
        cfg["name"] = name
    elif kind == "raw":
        cfg = {"kind": "raw", "name": name, "description": b.get("description", ""),
               "cmd_text": b.get("cmd_text") or "d\t\t# device menu\npwr\n.wait 3000\nm i\n",
               "workdir": b.get("workdir", "")}
    else:
        cfg = core.new_builder_config(name, platform)
    cid = core.unique_id(name)
    core.save_config(cid, cfg)
    cfg["id"] = cid
    return {"id": cid, "config": cfg}


# ---------------------------------------------------------------------------
# Autostart and main
# ---------------------------------------------------------------------------

def run_autostart(delay=True):
    s = core.load_settings()
    target, leds, reason = autostart.resolve(s)
    if not target:
        log("autostart: " + reason)
        return {"started": False, "reason": reason}
    if RUNNER.running():
        return {"started": False, "reason": "an emulation is already running"}
    d = s["autostart"].get("delay_s", 5) if delay else 0
    if d:
        RUNNER.note("AUTOSTART in %d s: %s (%s)" % (d, core.describe_target(target, s), reason))
        time.sleep(d)
        if RUNNER.running():
            return {"started": False, "reason": "an emulation was started meanwhile"}
    log("autostart: %s -> %s" % (reason, target))
    spec = core.launch_spec(target, s, leds=leds)
    RUNNER.start(spec)
    return {"started": True, "target": target, "reason": reason}


class ThreadingServer(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main(argv=None):
    ap = argparse.ArgumentParser(description="QBone/UniBone control panel daemon")
    ap.add_argument("--port", type=int, help="HTTP port (default from settings, %d)" % core.DEFAULT_PORT)
    ap.add_argument("--bind", help="bind address (default 0.0.0.0)")
    ap.add_argument("--no-autostart", action="store_true", help="do not run autostart")
    args = ap.parse_args(argv)

    core.ensure_dirs()
    core.local_token()
    s = core.load_settings()
    port = args.port or int(s.get("http_port") or core.DEFAULT_PORT)
    bind = args.bind or s.get("http_bind") or "0.0.0.0"

    httpd = ThreadingServer((bind, port), Handler)
    log("qbcontrold %s listening on %s:%d, home %s, platform %s"
        % (VERSION, bind, port, core.QUNIBONE_HOME, core.detect_platform(s)))
    if os.geteuid() != 0 and not os.environ.get("QBC_DEMO"):
        log("WARNING: not running as root; demo will refuse to start")

    def shutdown(signum, frame):
        def worker():
            log("signal %d: stopping emulation and server" % signum)
            RUNNER.stop(wait=True)
            httpd.shutdown()
        t = threading.Thread(target=worker)
        t.daemon = True
        t.start()

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    if not args.no_autostart:
        def auto():
            try:
                run_autostart(delay=True)
            except Exception as e:
                log("autostart failed: %s" % e)
                RUNNER.note("AUTOSTART failed: %s" % e)
        t = threading.Thread(target=auto, name="autostart")
        t.daemon = True
        t.start()

    try:
        httpd.serve_forever()
    finally:
        httpd.server_close()
        log("qbcontrold stopped")


if __name__ == "__main__":
    main()
