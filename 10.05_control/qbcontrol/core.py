# core.py - paths, platform, settings, device catalog, configurations,
# ".cmd" file generation and the scan of stock application scripts.
#
# The QUniBone "demo" program is the engine. Everything here produces input
# for it: a command file (demo --cmdfile x.cmd) which is executed line by line
# as if typed in the device menu, after which demo reads stdin interactively.

import hashlib
import json
import os
import re
import time

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

PKG_DIR = os.path.dirname(os.path.abspath(__file__))
CONTROL_DIR = os.path.dirname(PKG_DIR)            # .../10.05_control
WWW_DIR = os.path.join(CONTROL_DIR, "www")

# QUniBone software tree. On the BeagleBone this is the home directory of root,
# which is where update-code.sh unpacks the GitHub archive.
QUNIBONE_HOME = os.path.abspath(os.environ.get("QBC_HOME", os.path.dirname(CONTROL_DIR)))
DATA_DIR = os.path.abspath(os.environ.get("QBC_DATA", os.path.join(QUNIBONE_HOME, ".qbcontrol")))

DEMO_BIN = os.environ.get("QBC_DEMO",
                          os.path.join(QUNIBONE_HOME, "10.03_app_demo", "4_deploy", "demo"))
APPS_DIR = os.path.join(QUNIBONE_HOME, "10.03_app_demo", "5_applications")
BOOT_DIR = os.path.join(QUNIBONE_HOME, "10.02_devices", "5_boot")
PLATFORM_ENV = os.path.join(QUNIBONE_HOME, "qunibone-platform.env")

CONFIG_DIR = os.path.join(DATA_DIR, "configs")
WORK_DIR = os.path.join(DATA_DIR, "work")
IMAGE_DIR = os.path.join(DATA_DIR, "images")
SETTINGS_FILE = os.path.join(DATA_DIR, "settings.json")
TOKEN_FILE = os.path.join(DATA_DIR, "local.token")
LOG_FILE = os.path.join(DATA_DIR, "qbcontrold.log")
SESSION_LOG = os.path.join(DATA_DIR, "last-session.log")

DEFAULT_PORT = 1170     # as in PDP-11/70


def ensure_dirs():
    for d in (DATA_DIR, CONFIG_DIR, WORK_DIR, IMAGE_DIR):
        if not os.path.isdir(d):
            os.makedirs(d)


class QbcError(Exception):
    """Error with a message meant for the operator."""
    pass


# ---------------------------------------------------------------------------
# JSON file helpers
# ---------------------------------------------------------------------------

def read_json(path, default):
    try:
        with open(path, "r") as f:
            return json.load(f)
    except (IOError, OSError, ValueError):
        return default


def write_json(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, indent=2, sort_keys=True)
        f.write("\n")
    os.rename(tmp, path)   # atomic on POSIX


# ---------------------------------------------------------------------------
# Settings
# ---------------------------------------------------------------------------

DEFAULT_SETTINGS = {
    "platform": "auto",          # auto | QBUS | UNIBUS
    "address_width": 22,         # QBUS only: 16 | 18 | 22, passed as "demo --aw"
    "verbose": True,             # pass --verbose to demo
    "http_bind": "0.0.0.0",
    "http_port": DEFAULT_PORT,
    "password_salt": "",
    "password_hash": "",         # empty: no password
    "image_dirs": [],            # extra directories to scan for disk images
    "autostart": {
        "mode": "off",           # off | fixed | switches
        "target": "",            # "config:<id>" or "stock:<path rel. to 5_applications>"
        "switch_map": {},        # "1".."15" -> target
        "delay_s": 5,
        "leds": True,            # show switch value on the 4 LEDs (demo --leds)
    },
}


def load_settings():
    s = json.loads(json.dumps(DEFAULT_SETTINGS))   # deep copy
    stored = read_json(SETTINGS_FILE, {})
    for k, v in stored.items():
        if k == "autostart" and isinstance(v, dict):
            s["autostart"].update(v)
        else:
            s[k] = v
    return s


def save_settings(s):
    ensure_dirs()
    write_json(SETTINGS_FILE, s)


def hash_password(salt, password):
    return hashlib.sha256((salt + ":" + password).encode("utf-8")).hexdigest()


def set_password(settings, password):
    if not password:
        settings["password_salt"] = ""
        settings["password_hash"] = ""
    else:
        salt = hashlib.sha256(os.urandom(32)).hexdigest()[:16]
        settings["password_salt"] = salt
        settings["password_hash"] = hash_password(salt, password)


def check_password(settings, password):
    if not settings.get("password_hash"):
        return True
    return hash_password(settings.get("password_salt", ""), password) == settings["password_hash"]


def local_token():
    """Secret shared with local clients (the terminal menu) through a root-only file."""
    ensure_dirs()
    tok = None
    try:
        with open(TOKEN_FILE) as f:
            tok = f.read().strip()
    except (IOError, OSError):
        pass
    if not tok:
        tok = hashlib.sha256(os.urandom(32)).hexdigest()
        old = os.umask(0o077)
        try:
            with open(TOKEN_FILE, "w") as f:
                f.write(tok + "\n")
        finally:
            os.umask(old)
    return tok


# ---------------------------------------------------------------------------
# Platform
# ---------------------------------------------------------------------------

def detect_platform(settings=None):
    """'QBUS' or 'UNIBUS', from settings or qunibone-platform.env."""
    if settings and settings.get("platform") in ("QBUS", "UNIBUS"):
        return settings["platform"]
    try:
        with open(PLATFORM_ENV) as f:
            for line in f:
                line = line.strip()
                m = re.match(r'^(QUNIBONE_PLATFORM|MAKE_QUNIBUS)\s*=\s*"?(QBUS|UNIBUS)"?', line)
                if m:
                    return m.group(2)
    except (IOError, OSError):
        pass
    # 4_deploy -> 4_deploy_q symlink created by qunibone-platform.sh
    deploy = os.path.join(QUNIBONE_HOME, "10.03_app_demo", "4_deploy")
    if os.path.islink(deploy) and os.readlink(deploy).rstrip("/").endswith("_q"):
        return "QBUS"
    return "UNIBUS"    # qunibone-platform.sh defaults to UniBone, too


# ---------------------------------------------------------------------------
# Device catalog
# ---------------------------------------------------------------------------
# Device names are those the demo "device menu" knows ("en rl", "sd rl0", ...).
# Names are identical on UNIBUS and QBUS; only the DEC model labels differ.

MSCP_TYPES = ["RA81", "RA80", "RA60", "RA70", "RA71", "RA72", "RA73", "RA82", "RA90", "RA92",
              "RD51", "RD52", "RD53", "RD54", "RD31", "RD32", "RC25", "RX50"]

SHARED_FILESYSTEMS = ["RT11", "XXDP"]


def catalog(platform):
    q = (platform == "QBUS")
    ctrls = [
        {
            "key": "rl", "label": "RLV12" if q else "RL11",
            "title": "RL01/RL02 cartridge disk", "drive_prefix": "rl", "drive_count": 4,
            "drive_types": ["RL02", "RL01"], "default_type": "RL02",
            "boot": "dl.lst", "boot_code": "DL",
            "features": ["type", "speed", "shared", "writeprotect"],
            "address": "17774400" if q else "774400",
        },
        {
            "key": "rk", "label": "RKV11" if q else "RK11",
            "title": "RK05 cartridge disk", "drive_prefix": "rk", "drive_count": 8,
            "drive_types": ["RK05"], "default_type": "RK05",
            "boot": "dk.lst", "boot_code": "DK",
            "features": ["shared"],
            "address": "777400",
        },
        {
            "key": "uda", "label": "RQDX3/UDA50" if q else "UDA50",
            "title": "MSCP disk (RA/RD/RX50)", "drive_prefix": "uda", "drive_count": 8,
            "drive_types": MSCP_TYPES, "default_type": "RA81",
            "boot": "du.lst", "boot_code": "DU",
            "features": ["type", "useimagesize", "shared"],
            "address": "772150",
        },
        {
            "key": "rx", "label": "RXV11" if q else "RX11",
            "title": "RX01 floppy", "drive_prefix": "rx", "drive_count": 2, "box": "rxbox",
            "drive_types": ["RX01"], "default_type": "RX01",
            "boot": "dx.lst", "boot_code": "DX",
            "features": [],
            "address": "777170",
        },
        {
            "key": "ry", "label": "RXV21" if q else "RX211",
            "title": "RX02 floppy", "drive_prefix": "ry", "drive_count": 2, "box": "rybox",
            "drive_types": ["RX02"], "default_type": "RX02",
            "boot": "dy.lst", "boot_code": "DY",
            "features": ["imagetrack0"],
            "address": "777170",
        },
    ]
    if not q:
        ctrls.append({
            "key": "rf", "label": "RF11", "title": "RS11 fixed-head disk",
            "drive_prefix": "rs", "drive_count": 1,
            "drive_types": ["RS11"], "default_type": "RS11",
            "boot": "rf11.lst", "boot_code": "DF",
            "features": [],
            "address": "777460",
        })
    serial = [
        {"key": "dl11", "label": "DL11 console SLU", "address": "777560",
         "default_port": "ttyS2", "default_baud": 9600},
        {"key": "dl11b", "label": "DL11 2nd SLU (TU58)", "address": "776500",
         "default_port": "ttyS1", "default_baud": 38400},
    ]
    # Ethernet. DELQA is a QBUS card; the UniBone has no Ethernet device yet.
    network = []
    if q:
        network.append({"key": "delqa", "label": "DELQA", "title": "Ethernet (DEQNA compatible)",
                        "address": "17774440", "vector": "120 (set by the driver)",
                        "default_interface": "eth0"})
    boot_files = []
    if os.path.isdir(BOOT_DIR):
        boot_files = sorted(f for f in os.listdir(BOOT_DIR) if f.endswith((".lst", ".lda")))
    return {
        "platform": platform,
        "bus": "QBUS" if q else "UNIBUS",
        "controllers": ctrls,
        "serial": serial,
        "network": network,
        "shared_filesystems": SHARED_FILESYSTEMS,
        "cpu20": not q,              # emulated PDP-11/20 CPU, UNIBUS only
        "address_widths": [16, 18, 22] if q else [18],
        "boot_files": boot_files,
        "boot_dir": BOOT_DIR,
    }


def controller_by_key(cat, key):
    for c in cat["controllers"]:
        if c["key"] == key:
            return c
    return None


# ---------------------------------------------------------------------------
# Configurations
# ---------------------------------------------------------------------------
# A configuration is a JSON file in DATA_DIR/configs/<id>.json.
#   kind "builder": structured description, a .cmd file is generated from it.
#   kind "raw":     a hand-written .cmd file text, run in "workdir".

ID_RE = re.compile(r"^[a-z0-9][a-z0-9_\-]{0,47}$")
# A token placed on a demo command line: no blanks ("%s" scanning), no "#"
# (comment), no control characters.
TOKEN_RE = re.compile(r"^[A-Za-z0-9._/+\-~:@,=]{1,240}$")
OCTAL_RE = re.compile(r"^[0-7]{1,8}$")


def slugify(name):
    s = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")
    return (s or "config")[:40]


def new_builder_config(name, platform):
    return {
        "kind": "builder",
        "name": name,
        "description": "",
        "cpu20": False,           # emulate PDP-11/20 CPU ("dc" menu); UNIBUS only
        "power_cycle": True,      # "pwr": reset the PDP-11 on start
        "power_wait_ms": 3000,
        "memory": {"emulate": True, "end_addr": ""},
        "boot": {"mode": "auto", "file": ""},   # auto | none | file
        "spinup_wait_ms": 6000,
        "workdir": "",
        "controllers": {},        # key -> {"enabled": bool, "drives": [...]}
        "serial": {
            "dl11": {"enabled": False, "port": "ttyS2", "baud": 9600},
            "dl11b": {"enabled": False, "port": "ttyS1", "baud": 38400},
            "kw11": False,
        },
        "network": {
            "delqa": {"enabled": False, "interface": "eth0", "mac": ""},
        },
        "extra_commands": "",
        "notes": "",
    }


def config_path(cid):
    if not ID_RE.match(cid or ""):
        raise QbcError("Invalid configuration id \"%s\"" % cid)
    return os.path.join(CONFIG_DIR, cid + ".json")


def list_configs():
    ensure_dirs()
    out = []
    for fn in sorted(os.listdir(CONFIG_DIR)):
        if not fn.endswith(".json"):
            continue
        cid = fn[:-5]
        c = read_json(os.path.join(CONFIG_DIR, fn), None)
        if not isinstance(c, dict):
            continue
        out.append({"id": cid, "name": c.get("name", cid), "kind": c.get("kind", "builder"),
                    "description": c.get("description", ""),
                    "modified": os.path.getmtime(os.path.join(CONFIG_DIR, fn))})
    return out


def load_config(cid):
    c = read_json(config_path(cid), None)
    if not isinstance(c, dict):
        raise QbcError("Configuration \"%s\" not found" % cid)
    c["id"] = cid
    return c


def save_config(cid, cfg):
    ensure_dirs()
    cfg = dict(cfg)
    cfg.pop("id", None)
    if cfg.get("kind") not in ("builder", "raw"):
        raise QbcError("Configuration kind must be \"builder\" or \"raw\"")
    if not str(cfg.get("name", "")).strip():
        raise QbcError("Configuration needs a name")
    if cfg["kind"] == "builder":
        generate_cmd(cfg, detect_platform(load_settings()), cid)   # validates
    write_json(config_path(cid), cfg)
    return cid


def delete_config(cid):
    p = config_path(cid)
    if os.path.exists(p):
        os.remove(p)


def unique_id(name):
    base = slugify(name)
    cid = base
    n = 2
    while os.path.exists(os.path.join(CONFIG_DIR, cid + ".json")):
        cid = "%s-%d" % (base, n)
        n += 1
    return cid


def config_workdir(cid, cfg):
    wd = (cfg.get("workdir") or "").strip()
    if wd:
        return os.path.join(QUNIBONE_HOME, os.path.expanduser(wd)) if not os.path.isabs(wd) else wd
    return os.path.join(WORK_DIR, cid)


def _tok(value, what):
    value = str(value).strip()
    if not TOKEN_RE.match(value):
        raise QbcError("%s \"%s\": only letters, digits and ._/+-~:@,= allowed, no blanks or #"
                       % (what, value))
    return value


def _ms(value, what):
    try:
        v = int(value)
    except (TypeError, ValueError):
        raise QbcError("%s must be a number of milliseconds" % what)
    if v < 0 or v > 600000:
        raise QbcError("%s out of range" % what)
    return v


# demo reads command file lines with fgets() into a 255 byte buffer
# (inputline.readline / application_c::getchoice); a longer line is split into
# two bogus commands. Comments count, they are stripped only after reading.
MAX_CMD_LINE = 250


def _fit_lines(text):
    out = []
    for n, line in enumerate(text.split("\n"), 1):
        if len(line.encode("utf-8")) > MAX_CMD_LINE and "#" in line:
            line = line.split("#", 1)[0].rstrip()
        if len(line.encode("utf-8")) > MAX_CMD_LINE:
            raise QbcError("Line %d is longer than %d characters, demo would split it. "
                           "Use a shorter path (e.g. relative to the working directory): %s..."
                           % (n, MAX_CMD_LINE, line[:60]))
        out.append(line)
    return "\n".join(out)


def generate_cmd(cfg, platform, cid="config"):
    """Return the text of a demo command file for a builder or raw configuration."""
    return _fit_lines(_generate_cmd(cfg, platform, cid))


def _generate_cmd(cfg, platform, cid):
    if cfg.get("kind") == "raw":
        text = cfg.get("cmd_text", "")
        if not text.strip():
            raise QbcError("Command file text is empty")
        return text.replace("\r\n", "\n")

    cat = catalog(platform)
    L = []
    add = L.append
    add("# Generated by qbcontrol from configuration \"%s\" (%s)" % (cid, cfg.get("name", "")))
    add("# %s, %s" % (cat["bus"], time.strftime("%Y-%m-%d %H:%M")))
    add("# Edit the configuration in the web panel, not this file.")
    add("")
    cpu = bool(cfg.get("cpu20")) and cat["cpu20"]
    add("dc\t\t\t# device menu, with emulated CPU" if cpu else "d\t\t\t# device menu")

    # serial lines first, as in the stock scripts
    ser = cfg.get("serial", {})
    for key in ("dl11", "dl11b"):
        s = ser.get(key) or {}
        if s.get("enabled"):
            add("sd %s" % key)
            add("en %s" % key)
            if s.get("port"):
                add("p serialport %s" % _tok(s["port"], "Serial port"))
            if s.get("baud"):
                add("p baudrate %d" % int(s["baud"]))
    if ser.get("kw11"):
        add("en kw11\t\t\t# KW11 line clock")

    net = (cfg.get("network") or {}).get("delqa") or {}
    if net.get("enabled"):
        if not cat["network"]:
            raise QbcError("The DELQA Ethernet controller exists only on QBUS (QBone)")
        add("sd delqa\t\t# DELQA Ethernet")
        add("p interface %s" % _tok(net.get("interface") or "eth0", "Network interface"))
        mac = (net.get("mac") or "").strip()
        if mac:
            if not re.match(r"^[0-9a-fA-F]{2}([:-][0-9a-fA-F]{2}){5}$", mac):
                raise QbcError("MAC address must look like 08:00:2b:12:34:56")
            if int(mac[0:2], 16) & 1:
                raise QbcError("MAC address must be a unicast address (first byte even)")
            add("p mac %s" % mac)
        add("en delqa")

    if cfg.get("power_cycle", True):
        add("pwr\t\t\t# power cycle / reset PDP-11")
        add(".wait %d\t\t# wait for PDP-11 to reset" % _ms(cfg.get("power_wait_ms", 3000), "Reset wait"))

    mem = cfg.get("memory", {})
    if mem.get("emulate"):
        end = str(mem.get("end_addr") or "").strip()
        if end:
            if not OCTAL_RE.match(end):
                raise QbcError("Memory end address must be octal")
            add("m i %s\t\t# emulate memory up to %s" % (end, end))
        else:
            add("m i\t\t\t# emulate all missing memory")

    # boot loader
    ctrls = cfg.get("controllers", {})
    boot = cfg.get("boot", {}) or {}
    boot_mode = boot.get("mode", "auto")
    bootfile = ""
    if boot_mode == "file" and boot.get("file"):
        bootfile = _tok(boot["file"], "Boot loader file")
        if not os.path.isabs(bootfile) and os.path.exists(os.path.join(BOOT_DIR, bootfile)):
            bootfile = os.path.join(BOOT_DIR, bootfile)
    elif boot_mode == "auto" or (boot_mode not in ("none", "file")):
        want = boot.get("device") or ""
        order = [want] if want else []
        order += [c["key"] for c in cat["controllers"]]
        for key in order:
            c = controller_by_key(cat, key)
            if c and (ctrls.get(key) or {}).get("enabled"):
                bootfile = os.path.join(BOOT_DIR, c["boot"])
                break
    if bootfile:
        if bootfile.lower().endswith(".lda"):
            add("m lp %s\t# load program (absolute loader format)" % bootfile)
        else:
            add("m ll %s\t# deposit boot loader" % bootfile)

    any_spin = False
    for c in cat["controllers"]:
        cc = ctrls.get(c["key"]) or {}
        if not cc.get("enabled"):
            continue
        add("")
        add("en %s\t\t\t# %s %s" % (c["key"], c["label"], c["title"]))
        if c.get("box"):
            add("en %s" % c["box"])
            add("sd %s" % c["box"])
            add("p powerswitch 1\t\t# power on drive box")
        for d in cc.get("drives", []):
            if not d.get("enabled"):
                continue
            try:
                unit = int(d.get("unit", 0))
            except (TypeError, ValueError):
                raise QbcError("Bad drive unit number")
            if unit < 0 or unit >= c["drive_count"]:
                raise QbcError("%s has no drive unit %d" % (c["label"], unit))
            dev = "%s%d" % (c["drive_prefix"], unit)
            image = (d.get("image") or "").strip()
            if not c.get("box"):
                add("en %s" % dev)
            add("sd %s" % dev)
            if "type" in c["features"] and d.get("type"):
                t = _tok(d["type"], "Drive type")
                if t.upper() not in [x.upper() for x in c["drive_types"]]:
                    raise QbcError("%s: unknown drive type %s" % (dev, t))
                add("p type %s" % t)
            if "speed" in c["features"]:
                add("p emulation_speed %d\t# spin up faster" % max(1, int(d.get("speed", 10) or 10)))
                add("p runstopbutton 0\t\t# released: LOAD")
                add("p powerswitch 1")
            if "imagetrack0" in c["features"] and d.get("imagetrack0", True):
                add("p imagetrack0 1")
            if image:
                add("p image %s" % _tok(image, "%s image" % dev))
            if "useimagesize" in c["features"] and d.get("useimagesize", True):
                add("p useimagesize 1")
            if "shared" in c["features"] and d.get("shared_fs"):
                fs = _tok(d["shared_fs"], "Shared filesystem").upper()
                if fs not in SHARED_FILESYSTEMS:
                    raise QbcError("Shared filesystem must be one of " + ", ".join(SHARED_FILESYSTEMS))
                sdir = _tok(d.get("shared_dir") or ("shared_" + dev), "Shared directory")
                add("p shared_filesystem %s" % fs)
                add("p shared_dir %s" % sdir)
            if "writeprotect" in c["features"] and d.get("writeprotect"):
                add("p writeprotectbutton 1")
            if "speed" in c["features"]:
                add("p runstopbutton 1\t\t# RUN: spin up")
                any_spin = True
            elif image:
                any_spin = True

    if any_spin:
        add("")
        add(".print Waiting for drives to come ready ...")
        add(".wait %d" % _ms(cfg.get("spinup_wait_ms", 6000), "Spin-up wait"))

    if cpu:
        add("")
        add("en cpu20\t\t# emulated PDP-11/20")
        add("sd cpu20")

    extra = cfg.get("extra_commands", "") or ""
    if extra.strip():
        add("")
        add("# --- extra commands ---")
        for line in extra.replace("\r\n", "\n").split("\n"):
            add(line.rstrip())

    add("")
    add(".print Configuration \"%s\" is up." % re.sub(r"[#\r\n]", " ", cfg.get("name", cid)))
    if bootfile and bootfile.lower().endswith(".lda"):
        start = lda_start(bootfile)
        if start is not None:
            add(".print Program loaded: start it at %o in ODT (%og)." % (start, start))
    elif bootfile:
        add(".print Boot loader at 10000: start 10000 for unit 0, 10010 for unit 1, ...")
    if cpu:
        add(".print Emulated CPU selected: \"p c 1\" toggles CONTINUE.")
    return "\n".join(L) + "\n"


def lda_start(path):
    """Start address of an absolute loader (.lda) file, or None."""
    try:
        with open(path, "rb") as f:
            d = bytearray(f.read())
    except (IOError, OSError):
        return None
    i = 0
    while i + 6 <= len(d):
        if d[i] != 1 or d[i + 1] != 0:
            i += 1
            continue
        count = d[i + 2] | (d[i + 3] << 8)
        addr = d[i + 4] | (d[i + 5] << 8)
        if count < 6:
            return None
        if count == 6:
            return None if addr & 1 else addr
        i += count + 1
    return None


# ---------------------------------------------------------------------------
# Stock application scripts (10.03_app_demo/5_applications/*/*.sh)
# ---------------------------------------------------------------------------

IMAGE_LINE_RE = re.compile(r"^\s*p\s+(?:image|img)\s+(\S+)", re.I)
LOADER_LINE_RE = re.compile(r"^\s*m\s+l[lpt]\s+(\S+)", re.I)


def _app_roots(platform):
    roots = [APPS_DIR]
    # development tree where qunibone-platform.sh was not run yet
    sfx = "_q" if platform == "QBUS" else "_u"
    alt = APPS_DIR + sfx
    if os.path.isdir(alt):
        roots.append(alt)
    return [r for r in roots if os.path.isdir(r)]


def _strip_comment(line):
    return line.split("#", 1)[0].strip()


def scan_stock(platform):
    out = []
    seen = set()
    for root in _app_roots(platform):
        for dirpath, dirnames, filenames in os.walk(root, followlinks=True):
            dirnames.sort()
            for fn in sorted(filenames):
                if not fn.endswith(".sh"):
                    continue
                path = os.path.join(dirpath, fn)
                rel = os.path.relpath(path, root)
                if rel in seen:
                    continue
                seen.add(rel)
                info = parse_stock_script(path)
                if info is None:
                    continue
                info["id"] = rel
                info["group"] = os.path.dirname(rel) or "."
                out.append(info)
    return out


def parse_stock_script(path):
    try:
        with open(path, "r") as f:
            text = f.read()
    except (IOError, OSError, UnicodeDecodeError):
        return None
    if "demo" not in text:
        return None
    m = re.search(r"--cmdfile\s+(\S+)", text)
    cmdfile = m.group(1) if m else ""
    title = ""
    for line in text.split("\n"):
        line = line.strip()
        if line.startswith("#") and not line.startswith("#!"):
            title = line.lstrip("#").strip()
            break
    # "cd ~/10.03_app_demo/5_applications/xxx"
    rundir = os.path.dirname(path)
    m = re.search(r"^\s*cd\s+(\S+)", text, re.M)
    if m:
        d = m.group(1)
        if d.startswith("~/"):
            d = os.path.join(QUNIBONE_HOME, d[2:])
        if os.path.isdir(d):
            rundir = d
    info = {"name": os.path.basename(path)[:-3], "title": title, "path": path,
            "rundir": rundir, "cmdfile": cmdfile, "mode": "d", "images": [], "loaders": [],
            "missing": [], "blank": []}
    cmdpath = os.path.join(rundir, cmdfile) if cmdfile else ""
    if not (cmdpath and os.path.exists(cmdpath)):
        cmdpath = os.path.join(os.path.dirname(path), cmdfile) if cmdfile else ""
    if cmdpath and os.path.exists(cmdpath):
        try:
            with open(cmdpath) as f:
                lines = f.read().split("\n")
        except (IOError, OSError, UnicodeDecodeError):
            lines = []
        first = True
        for line in lines:
            code = _strip_comment(line)
            if not code:
                continue
            if first:
                info["mode"] = code.split()[0].lower()
                first = False
            mi = IMAGE_LINE_RE.match(code)
            if mi:
                info["images"].append(mi.group(1))
            ml = LOADER_LINE_RE.match(code)
            if ml:
                info["loaders"].append(ml.group(1))
        for img in info["images"] + info["loaders"]:
            p = img if os.path.isabs(img) else os.path.join(rundir, img)
            if not (os.path.exists(p) or os.path.exists(p + ".gz")):
                # demo creates a missing disk image as an empty file: fine for
                # the scratch disks of the stock scripts, which are not on
                # files.retrocmp.com
                if img in info["images"] and "scratch" in os.path.basename(img).lower():
                    info["blank"].append(img)
                else:
                    info["missing"].append(img)
    elif cmdfile:
        info["missing"].append(cmdfile)
    info["cpu"] = info["mode"] == "dc"
    info["ready"] = not info["missing"]
    return info


def find_stock(platform, sid):
    for s in scan_stock(platform):
        if s["id"] == sid:
            return s
    raise QbcError("Stock script \"%s\" not found" % sid)


def read_stock_cmd(info):
    p = os.path.join(info["rundir"], info["cmdfile"])
    if not os.path.exists(p):
        p = os.path.join(os.path.dirname(info["path"]), info["cmdfile"])
    with open(p) as f:
        return f.read()


# ---------------------------------------------------------------------------
# Disk images
# ---------------------------------------------------------------------------

NON_IMAGE_EXT = set(".sh .cmd .lst .mac .txt .ini .md .html .htm .log .csv .py .pyc .json .url "
                    ".pdf .c .h .cpp .hpp .o .env .dts .dtbo .jpg .png .gif .tar .zip".split())
MIN_IMAGE_SIZE = 32 * 1024


def scan_images(settings):
    roots = [IMAGE_DIR] + _app_roots(detect_platform(settings))
    roots += [d for d in settings.get("image_dirs", []) if os.path.isdir(d)]
    found = {}
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root, followlinks=True):
            dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
            for fn in filenames:
                p = os.path.join(dirpath, fn)
                gz = fn.endswith(".gz")
                base = fn[:-3] if gz else fn
                ext = os.path.splitext(base)[1].lower()
                if ext in NON_IMAGE_EXT:
                    continue
                try:
                    st = os.stat(p)
                except OSError:
                    continue
                if not gz and st.st_size < MIN_IMAGE_SIZE:
                    continue
                key = os.path.join(dirpath, base)
                e = found.get(key)
                if e is None:
                    e = found[key] = {"path": key, "name": base, "dir": dirpath,
                                      "size": 0, "compressed_only": True}
                if gz:
                    e["gz_size"] = st.st_size
                else:
                    e["size"] = st.st_size
                    e["compressed_only"] = False
                if len(found) > 3000:
                    break
    return sorted(found.values(), key=lambda e: e["path"].lower())


# ---------------------------------------------------------------------------
# Launch targets
# ---------------------------------------------------------------------------
# A target string names what to run:
#   "config:<id>"   a configuration of this panel
#   "stock:<rel>"   a stock script, relative to 5_applications

def describe_target(target, settings):
    kind, _, ref = (target or "").partition(":")
    try:
        if kind == "config":
            return load_config(ref).get("name", ref)
        if kind == "stock":
            return find_stock(detect_platform(settings), ref)["name"]
    except QbcError:
        return target + " (missing)"
    return target


def launch_spec(target, settings, leds=None):
    """Return dict(argv, cwd, label, cmdfile, target) to start the target with demo."""
    platform = detect_platform(settings)
    kind, _, ref = (target or "").partition(":")
    common = []
    if settings.get("verbose", True):
        common.append("--verbose")
    if platform == "QBUS":
        aw = int(settings.get("address_width", 22))
        if aw not in (16, 18, 22):
            raise QbcError("QBUS address width must be 16, 18 or 22")
        common += ["--aw", str(aw)]
    if leds is not None:
        common += ["--leds", str(int(leds))]

    if kind == "config":
        cfg = load_config(ref)
        text = generate_cmd(cfg, platform, ref)
        wd = config_workdir(ref, cfg)
        if not os.path.isdir(wd):
            os.makedirs(wd)
        cmdfile = os.path.join(wd, ref + ".cmd")
        with open(cmdfile, "w") as f:
            f.write(text)
        argv = [DEMO_BIN] + common + ["--cmdfile", cmdfile]
        return {"argv": argv, "cwd": wd, "label": cfg.get("name", ref), "cmdfile": cmdfile,
                "target": target}
    if kind == "stock":
        info = find_stock(platform, ref)
        if not info["cmdfile"]:
            raise QbcError("Script %s has no --cmdfile" % info["name"])
        # Run demo directly instead of the .sh wrapper: same cwd and cmdfile,
        # but the panel can add --aw (mandatory on QBone) and keeps one process.
        cmdfile = info["cmdfile"]
        argv = [DEMO_BIN] + common + ["--cmdfile", cmdfile]
        return {"argv": argv, "cwd": info["rundir"], "label": info["name"],
                "cmdfile": os.path.join(info["rundir"], cmdfile), "target": target}
    raise QbcError("Unknown target \"%s\"" % target)
