# menu.py - qbmenu: terminal menu for the QBone/UniBone control daemon.
#
# Talks to qbcontrold over HTTP on localhost, so the web panel and the
# terminal always show the same state. Starts the daemon if it is not up.
# Works on any VT100-compatible terminal: ssh, serial console, tty1.

import json
import os
import select
import shutil
import subprocess
import sys
import threading
import time
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError

from . import VERSION, core

CTRL_RBRACKET = b"\x1d"

USE_ANSI = sys.stdout.isatty() and os.environ.get("TERM", "dumb") != "dumb"


def ansi(code, text):
    return "\x1b[%sm%s\x1b[0m" % (code, text) if USE_ANSI else text


def bold(t):
    return ansi("1", t)


def rev(t):
    return ansi("7", t)


def dim(t):
    return ansi("2", t)


# ---------------------------------------------------------------------------
# HTTP client
# ---------------------------------------------------------------------------

class Client(object):
    def __init__(self):
        s = core.load_settings()
        self.base = "http://127.0.0.1:%d" % int(s.get("http_port") or core.DEFAULT_PORT)
        self.token = None
        try:
            with open(core.TOKEN_FILE) as f:
                self.token = f.read().strip()
        except (IOError, OSError):
            pass

    def call(self, method, path, body=None, timeout=10):
        data = json.dumps(body).encode("utf-8") if body is not None else None
        req = Request(self.base + path, data=data, method=method)
        req.add_header("Content-Type", "application/json")
        if self.token:
            req.add_header("X-QBC-Token", self.token)
        try:
            with urlopen(req, timeout=timeout) as r:
                return json.loads(r.read().decode("utf-8"))
        except HTTPError as e:
            try:
                msg = json.loads(e.read().decode("utf-8")).get("error", str(e))
            except Exception:
                msg = str(e)
            raise core.QbcError(msg)

    def get(self, path, **kw):
        return self.call("GET", path, **kw)

    def post(self, path, body=None, **kw):
        return self.call("POST", path, body if body is not None else {}, **kw)

    def put(self, path, body, **kw):
        return self.call("PUT", path, body, **kw)

    def alive(self):
        try:
            self.get("/api/status", timeout=3)
            return True
        except (URLError, OSError, core.QbcError, ValueError):
            return False


def ensure_daemon(client):
    if client.alive():
        return True
    print("The control daemon (qbcontrold) is not running.")
    if os.geteuid() != 0 and not os.environ.get("QBC_DEMO"):
        print("Run qbmenu as root to start it, or: sudo systemctl start qbcontrol")
        return False
    unit = "/etc/systemd/system/qbcontrol.service"
    if os.path.exists(unit) and shutil.which("systemctl"):
        print("Starting it with systemctl ...")
        subprocess.call(["systemctl", "start", "qbcontrol"])
    else:
        print("Starting it in the background ...")
        core.ensure_dirs()
        logf = open(core.LOG_FILE, "a")
        subprocess.Popen([sys.executable, "-m", "qbcontrol.server"], cwd=core.CONTROL_DIR,
                         stdin=subprocess.DEVNULL, stdout=logf, stderr=logf,
                         start_new_session=True, close_fds=True)
    for _ in range(40):
        time.sleep(0.25)
        if client.alive():
            client.__init__()     # re-read token, created by the daemon
            return True
    print("Daemon did not come up, see %s" % core.LOG_FILE)
    return False


# ---------------------------------------------------------------------------
# Screens
# ---------------------------------------------------------------------------

def clear():
    if USE_ANSI:
        sys.stdout.write("\x1b[H\x1b[2J")
        sys.stdout.flush()


def ask(prompt, default=""):
    try:
        s = input(prompt)
    except EOFError:
        raise KeyboardInterrupt
    return s.strip() or default


def pause():
    ask(dim("  Press RETURN to continue "))


def fmt_time(t):
    return time.strftime("%H:%M:%S", time.localtime(t)) if t else "-"


def banner(st):
    w = 72
    plat = st["platform"]
    if plat == "QBUS":
        plat += " %d-bit" % st.get("address_width", 22)
    title = "Q B O N E   C O N T R O L   P A N E L" if st["platform"] == "QBUS" \
        else "U N I B O N E   C O N T R O L   P A N E L"
    print(" +" + "-" * (w - 2) + "+")
    inner = "  " + title
    tail = plat + "  V" + VERSION + "  "
    print(" |" + bold(inner) + " " * (w - 2 - len(inner) - len(tail)) + tail + "|")
    print(" +" + "-" * (w - 2) + "+")


def status_lines(st):
    r = st["runner"]
    state = r["state"].upper()
    if r["state"] == "running":
        s = rev(" RUN ") + "  %s   since %s  pid %s" % (r.get("label", ""), fmt_time(r.get("started_at")), r.get("pid"))
    elif r["state"] == "stopping":
        s = rev(" STOPPING ") + "  %s" % r.get("label", "")
    else:
        s = "HALT  (nothing running)"
        if r.get("exit_code") is not None:
            s += "  last: %s exited %s at %s" % (r.get("label", "?"), r["exit_code"], fmt_time(r.get("stopped_at")))
    print("   Status : " + s if state else "")
    ips = st.get("ips") or [st.get("hostname")]
    print("   Web    : " + "  ".join("http://%s:%d/" % (ip, st["port"]) for ip in ips[:2]))
    a = st.get("autostart", {})
    auto = a.get("mode", "off")
    if auto == "fixed":
        auto += " -> " + (a.get("target_label") or "?")
    elif auto == "switches":
        auto += " (DIP switches now = %s)" % (st.get("switches") if st.get("switches") is not None else "?")
    print("   Auto   : " + auto)
    if not st.get("demo_ok"):
        print("   " + rev(" WARNING ") + " demo program missing: %s  (run ./compile.sh)" % st["demo_bin"])
    elif not st.get("is_root") and not st.get("fake_demo"):
        print("   " + rev(" WARNING ") + " daemon not running as root; demo cannot reach the PRUs")


def choose_target(c, title, allow_none=False):
    """List configs and stock scripts, return target string or None."""
    cfgs = c.get("/api/configs")["configs"]
    stock = c.get("/api/stock")["stock"]
    items = []
    clear()
    print()
    print("  " + bold(title))
    print()
    if cfgs:
        print("  " + dim("-- My configurations ---------------------------------------------"))
        for cf in cfgs:
            items.append(("config:" + cf["id"], cf["name"]))
            print("  %3d  %-34s %s" % (len(items), cf["name"][:34], dim(cf.get("description", "")[:30])))
    if stock:
        group = None
        print("  " + dim("-- Stock application scripts (5_applications) --------------------"))
        for s in stock:
            if s["group"] != group:
                group = s["group"]
                print("       " + dim(group))
            items.append(("stock:" + s["id"], s["name"]))
            flag = "" if s["ready"] else ansi("33", " [images missing]")
            print("  %3d    %-32s%s%s" % (len(items), s["name"][:32], " CPU" if s["cpu"] else "", flag))
    if not items:
        print("  No configurations and no stock scripts found.")
        print("  Create one in the web panel, or run ./update-files.sh for the stock scripts.")
    print()
    hint = "number, 0 = none, RETURN = cancel" if allow_none else "number, RETURN = cancel"
    s = ask("  Select (%s): " % hint)
    if not s:
        return None
    if allow_none and s == "0":
        return ""
    try:
        n = int(s)
        if 1 <= n <= len(items):
            return items[n - 1][0]
    except ValueError:
        pass
    print("  No such entry.")
    pause()
    return None


def do_start(c):
    st = c.get("/api/status")
    if st["runner"]["state"] != "stopped":
        print("  \"%s\" is running." % st["runner"].get("label"))
        if ask("  Stop it and start another one? [y/N] ").lower() != "y":
            return
        c.post("/api/stop")
        if not wait_stopped(c):
            return
    t = choose_target(c, "START EMULATION")
    if not t:
        return
    try:
        c.post("/api/start", {"target": t})
    except core.QbcError as e:
        print("  " + rev(" ERROR ") + " " + str(e))
        pause()
        return
    if ask("  Started. Attach console? [Y/n] ").lower() != "n":
        attach(c)


def wait_stopped(c, timeout=30):
    print("  Stopping ", end="")
    sys.stdout.flush()
    end = time.time() + timeout
    while time.time() < end:
        if c.get("/api/status")["runner"]["state"] == "stopped":
            print(" stopped.")
            return True
        print(".", end="")
        sys.stdout.flush()
        time.sleep(0.5)
    print(" still running!")
    pause()
    return False


def do_stop(c, force=False):
    st = c.get("/api/status")
    if st["runner"]["state"] == "stopped":
        print("  Nothing running.")
        pause()
        return
    c.post("/api/stop", {"force": force})
    wait_stopped(c)


def do_restart(c):
    st = c.get("/api/status")
    t = st["runner"].get("target")
    if not t:
        print("  Nothing was started yet.")
        pause()
        return
    if st["runner"]["state"] != "stopped":
        c.post("/api/stop")
        if not wait_stopped(c):
            return
    c.post("/api/start", {"target": t})
    attach(c)


def attach(c):
    """Raw console: keys go to demo, output comes back. Ctrl-] detaches."""
    import termios
    import tty
    st = c.get("/api/status")["runner"]
    if st["state"] == "stopped":
        print("  Nothing running; showing the last session output.")
    print(rev(" CONSOLE ") + "  %s   " % (st.get("label") or "") + bold("Ctrl-]") + " returns to the menu")
    sys.stdout.flush()
    stop_flag = threading.Event()
    out = sys.stdout.buffer if hasattr(sys.stdout, "buffer") else sys.stdout

    # replay the last few KB, then follow
    first = c.get("/api/console?since=0")
    since = max(first["base"], first["next"] - 4096)
    state = {"since": since, "ended": False}

    def pump():
        while not stop_flag.is_set():
            try:
                r = c.get("/api/console?since=%d&wait=5" % state["since"], timeout=15)
            except Exception:
                time.sleep(1)
                continue
            if r["data"]:
                out.write(r["data"].encode("latin-1"))
                out.flush()
            state["since"] = r["next"]
            if r["state"] == "stopped" and not r["data"]:
                state["ended"] = True
                time.sleep(0.5)

    t = threading.Thread(target=pump)
    t.daemon = True
    t.start()

    fd = sys.stdin.fileno()
    if not os.isatty(fd):
        t.join()
        return
    old = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        while True:
            r, _, _ = select.select([fd], [], [], 0.2)
            if state["ended"]:
                break
            if not r:
                continue
            data = os.read(fd, 256)
            if CTRL_RBRACKET in data:
                data = data.split(CTRL_RBRACKET)[0]
                if data:
                    c.post("/api/console", {"text": data.decode("latin-1")})
                break
            try:
                c.post("/api/console", {"text": data.decode("latin-1")})
            except core.QbcError:
                state["ended"] = True
                break
    finally:
        stop_flag.set()
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
    print()
    if state["ended"]:
        print("  demo has exited.")
        pause()


def do_autostart(c):
    while True:
        a = c.get("/api/autostart")
        cfg = a["autostart"]
        labels = a.get("labels", {})
        clear()
        print()
        print("  " + bold("AUTOSTART") + dim("   what qbcontrold starts when the BeagleBone boots"))
        print()
        print("   Mode     : %s" % bold(cfg["mode"]))
        if cfg["mode"] == "fixed":
            print("   Target   : %s" % (labels.get(cfg.get("target"), cfg.get("target")) or "-"))
        print("   Delay    : %d s after boot" % cfg.get("delay_s", 5))
        sw = a.get("switches")
        print("   Switches : %s" % ("%d  (%s)" % (sw, format(sw, "04b")) if sw is not None else "not readable"))
        if cfg["mode"] == "switches":
            print("   LEDs     : %s" % ("show switch value" if cfg.get("leds", True) else "off"))
            print()
            print("   DIP  Target")
            smap = cfg.get("switch_map", {})
            for n in range(1, 16):
                mark = "<" if sw == n else " "
                t = smap.get(str(n))
                print("   %2d %s %s" % (n, mark, labels.get(t, t) if t else dim("-")))
        print()
        print("   Now: " + (a.get("would_start_label") or "nothing") + dim("  (" + a.get("reason", "") + ")"))
        print()
        print("   M  Change mode   T  Set fixed target   S  Edit switch slot   D  Delay")
        print("   L  Toggle LEDs   R  Run autostart now  RETURN  back")
        k = ask("  > ").lower()
        if not k:
            return
        if k == "m":
            m = ask("  Mode: 0 = off, 1 = fixed, 2 = DIP switches: ")
            mode = {"0": "off", "1": "fixed", "2": "switches"}.get(m)
            if mode:
                cfg["mode"] = mode
                if mode == "fixed" and not cfg.get("target"):
                    t = choose_target(c, "AUTOSTART TARGET")
                    if t:
                        cfg["target"] = t
        elif k == "t":
            t = choose_target(c, "AUTOSTART TARGET (fixed mode)", allow_none=True)
            if t is not None:
                cfg["target"] = t
                if t:
                    cfg["mode"] = "fixed"
        elif k == "s":
            n = ask("  Switch value 1..15: ")
            if n.isdigit() and 1 <= int(n) <= 15:
                t = choose_target(c, "TARGET FOR SWITCH VALUE %s" % n, allow_none=True)
                if t is not None:
                    smap = cfg.setdefault("switch_map", {})
                    if t:
                        smap[n] = t
                    else:
                        smap.pop(n, None)
        elif k == "d":
            d = ask("  Delay in seconds: ")
            if d.isdigit():
                cfg["delay_s"] = int(d)
        elif k == "l":
            cfg["leds"] = not cfg.get("leds", True)
        elif k == "r":
            r = c.post("/api/autostart/run", timeout=30)
            print("  " + ("Started " + r.get("target", "") if r.get("started") else r.get("reason", "")))
            pause()
            continue
        else:
            continue
        try:
            c.put("/api/autostart", cfg)
        except core.QbcError as e:
            print("  " + str(e))
            pause()


# ---------------------------------------------------------------------------
# Live devices: switch controllers and drives of the running demo
# ---------------------------------------------------------------------------

def device_rows(d):
    """Numbered rows: (device name, kind, text) for the devices screen."""
    rows = []
    for ctl in d["controllers"]:
        rows.append((ctl["key"], "ctrl", ctl))
        if not ctl["on"]:
            continue
        for drv in ctl["drives"]:
            if drv["known"] or drv["on"]:
                rows.append((drv["name"], "drive", (ctl, drv)))
    for o in d["others"]:
        rows.append((o["key"], "other", o))
    return rows


def onoff(on):
    return bold("ON ") if on else dim("off")


def pick_image(c, dev):
    """Ask for an image: a path, or search text to pick from the image list."""
    s = ask("  Image file (path, or part of a name to search; RETURN = cancel): ")
    if not s:
        return None
    if s.startswith("/") or os.path.exists(s):
        return s
    imgs = [i for i in c.get("/api/images", timeout=60)["images"] if s.lower() in i["path"].lower()]
    if not imgs:
        print("  No image matches \"%s\"." % s)
        pause()
        return None
    for n, i in enumerate(imgs[:30], 1):
        print("   %2d  %-30s %s" % (n, i["name"], dim(i["dir"].replace(core.QUNIBONE_HOME, "~", 1))))
    if len(imgs) > 30:
        print(dim("   ... %d more, search more precisely" % (len(imgs) - 30)))
    k = ask("  Number: ")
    if k.isdigit() and 1 <= int(k) <= min(30, len(imgs)):
        return imgs[int(k) - 1]["path"]
    return None


def do_devices(c):
    msg = ""
    while True:
        d = c.get("/api/devices")
        rows = device_rows(d)
        clear()
        print()
        state = ("LIVE  demo prompt %s>>>" % d["menu"]) if d["ready"] else ("WAIT" if d["running"] else "")
        print("  " + bold("DEVICES") + "   " + state + dim("   " + (d.get("label") or "")))
        print()
        if d.get("reason"):
            print("   " + d["reason"])
            print()
        for n, (name, kind, x) in enumerate(rows, 1):
            if kind == "ctrl":
                print("   %2d  %-12s %s  %s" % (n, x["label"], onoff(x["on"]), dim(x["title"])))
            elif kind == "drive":
                ctl, drv = x
                flags = ""
                if ctl["rl_buttons"] and drv["on"]:
                    flags = ("RUN " if drv["run"] else "LOAD") + (" WP" if drv["writeprotect"] else "   ")
                media = os.path.basename(drv["image"]) if drv["image"] else dim("no media")
                print("   %2d    %-10s %s  %-7s  %s" % (n, drv["name"], onoff(drv["on"]), flags, media if drv["on"] else ""))
            else:
                print("   %2d  %-20s %s  %s" % (n, x["label"], onoff(x["on"]), dim(x.get("info", ""))))
        bus = d.get("bus") or {}
        if bus:
            print()
            if bus.get("can_halt"):
                print("   PDP-11  HALT %s   " % (rev(" HALT ") if bus.get("halt") else "ENABLE") +
                      dim("H  HALT/ENABLE   INIT  pulse INIT   RESTART  power cycle"))
            else:
                print("   PDP-11  " + dim("INIT  pulse INIT   RESTART  power cycle"))
        print()
        print("   <n> or <name>  switch on/off   (any drive by name, e.g. rk3)")
        print("   R <n>  RUN/LOAD (RL)   W <n>  write protect (RL)")
        print("   I <n>  change image    U <n>  unload image     RETURN  back")
        if msg:
            print()
            print("   " + msg)
            msg = ""
        k = ask("  > ").strip()
        if not k:
            return
        low = k.lower()
        if low in ("h", "init", "restart"):
            try:
                if low == "h":
                    c.post("/api/devices", {"device": "bus", "action": "enable" if bus.get("halt") else "halt"})
                elif low == "init":
                    c.post("/api/devices", {"device": "bus", "action": "init"})
                elif ask("  Power cycle the PDP-11? A running operating system is lost. [y/N] ").lower() == "y":
                    c.post("/api/devices", {"device": "bus", "action": "restart"})
            except core.QbcError as e:
                msg = rev(" ERROR ") + " " + str(e)
            continue
        f = k.split()
        verb = f[0].lower() if len(f) == 2 and f[0].lower() in ("r", "w", "i", "u") else ""
        ref = f[1] if verb else k
        if ref.isdigit() and 1 <= int(ref) <= len(rows):
            name, kind, x = rows[int(ref) - 1]
        else:
            name = ref.lower()
            found = [r for r in rows if r[0] == name]
            kind, x = (found[0][1], found[0][2]) if found else ("drive", None)
        try:
            if not verb:
                if kind == "ctrl" or kind == "other":
                    on = x["on"]
                elif x is not None:
                    on = x[1]["on"]
                else:
                    on = False       # drive not listed yet: switch it on
                action = "off" if on else "on"
                c.post("/api/devices", {"device": name, "action": action})
            elif verb == "r":
                run = x is not None and kind == "drive" and x[1].get("run")
                c.post("/api/devices", {"device": name, "action": "load" if run else "run"})
            elif verb == "w":
                wp = x is not None and kind == "drive" and x[1].get("writeprotect")
                c.post("/api/devices", {"device": name, "action": "wpoff" if wp else "wp"})
            elif verb == "u":
                c.post("/api/devices", {"device": name, "action": "image", "value": ""})
            elif verb == "i":
                img = pick_image(c, name)
                if img:
                    c.post("/api/devices", {"device": name, "action": "image", "value": img})
        except core.QbcError as e:
            msg = rev(" ERROR ") + " " + str(e)


def do_show(c):
    st = c.get("/api/status")["runner"]
    p = st.get("cmdfile")
    clear()
    if p and os.path.exists(p):
        print(bold("  Command file " + p))
        print()
        with open(p) as f:
            print(f.read())
    else:
        print("  No command file yet.")
    print(dim("  Session log: " + core.SESSION_LOG))
    pause()


def main_menu(c):
    while True:
        try:
            st = c.get("/api/status")
        except (URLError, OSError, core.QbcError) as e:
            print("  Lost connection to qbcontrold: %s" % e)
            if not ensure_daemon(c):
                return 1
            continue
        clear()
        print()
        banner(st)
        print()
        status_lines(st)
        print()
        running = st["runner"]["state"] != "stopped"
        print("    1  Start emulation ...")
        print("    2  Stop emulation" + ("" if running else dim("   (nothing running)")))
        print("    3  Console            " + dim("attach to the demo program, Ctrl-] returns"))
        print("    4  Autostart ...")
        print("    5  Restart last       " + dim(st["runner"].get("label") or ""))
        print("    6  Show command file")
        print("    7  Devices ...        " + dim("switch controllers and drives of the running emulation"))
        print("    K  Kill emulation     " + dim("hard stop, if demo hangs"))
        print("    X  Exit to shell")
        print()
        k = ask("  Selection: ").lower()
        try:
            if k == "1":
                do_start(c)
            elif k == "2":
                do_stop(c)
            elif k == "3":
                attach(c)
            elif k == "4":
                do_autostart(c)
            elif k == "5":
                do_restart(c)
            elif k == "6":
                do_show(c)
            elif k == "7":
                do_devices(c)
            elif k == "k":
                if ask("  Kill demo without leaving the menus? [y/N] ").lower() == "y":
                    do_stop(c, force=True)
            elif k in ("x", "q", "exit"):
                return 0
        except core.QbcError as e:
            print("  " + rev(" ERROR ") + " " + str(e))
            pause()


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    c = Client()
    if argv and argv[0] in ("-h", "--help"):
        print("qbmenu [status|start <target>|stop|console|autostart|devices|dev <device> <action> [value]]")
        print("  without arguments: interactive menu")
        print("  target: config:<id> or stock:<path below 5_applications>")
        print("  dev:    qbmenu dev rk on | dev rl0 run | dev rl0 load | dev rl0 wp | dev rl0 wpoff")
        print("          qbmenu dev rl1 image /root/disks/x.rl02 | dev rl1 image   (unload)")
        print("          qbmenu dev bus halt | dev bus enable | dev bus init | dev bus restart")
        return 0
    if not ensure_daemon(c):
        return 1
    try:
        if not argv:
            return main_menu(c)
        cmd = argv[0]
        if cmd == "status":
            st = c.get("/api/status")
            banner(st)
            status_lines(st)
        elif cmd == "start" and len(argv) > 1:
            print(json.dumps(c.post("/api/start", {"target": argv[1]}), indent=1))
        elif cmd == "stop":
            c.post("/api/stop")
            wait_stopped(c)
        elif cmd == "console":
            attach(c)
        elif cmd == "devices":
            d = c.get("/api/devices")
            if d.get("reason"):
                print(d["reason"])
            for name, kind, x in device_rows(d):
                if kind == "drive":
                    ctl, drv = x
                    print("  %-6s %-3s %s" % (name, "on" if drv["on"] else "off", drv["image"]))
                else:
                    print("%-8s %-3s %s" % (name, "on" if x["on"] else "off", x.get("label", "")))
            if (d.get("bus") or {}).get("can_halt"):
                print("%-8s %s" % ("halt", "asserted" if d["bus"]["halt"] else "released"))
        elif cmd == "dev" and len(argv) >= 3:
            r = c.post("/api/devices", {"device": argv[1], "action": argv[2],
                                        "value": argv[3] if len(argv) > 3 else ""})
            print("sent: " + "; ".join(r.get("sent", [])))
        elif cmd == "autostart":
            r = c.post("/api/autostart/run", timeout=30)
            print(r.get("reason") or r)
        else:
            print("Unknown command, see --help")
            return 2
    except KeyboardInterrupt:
        print()
    except core.QbcError as e:
        print("Error: %s" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
