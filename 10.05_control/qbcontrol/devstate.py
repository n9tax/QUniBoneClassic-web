# devstate.py - which devices are on, and how their drives are set, in the
# running demo.
#
# demo cannot be asked for its device state in a machine readable way, so the
# daemon follows every command that reaches the demo device menu: first the
# command file, then each line typed on the console (web panel, qbmenu, or
# the device buttons). Only en / dis / sd / p change the model.
#
# The live device panel then turns its buttons into the same commands an
# operator would type:  en rl / dis rl / sd rl0 / p runstopbutton 1 / ...

import re

from . import core

DEVICE_MENUS = ("D", "DC")


def menu_hint(menu):
    """Why the device buttons are not available at this demo prompt."""
    if menu is None:
        return "demo is still working through its command file."
    if menu == "":
        return ("demo is at its main menu (>>>). Type d in the console (dc with the emulated "
                "CPU) to get back to the device menu; all devices start switched off then.")
    return ("demo is in another menu (prompt \"%s>>>\"). Type q in the console until you "
            "are back at D>>>." % menu)


def _norm(value):
    return str(value).strip().lower()


def _bool(value):
    v = _norm(value)
    if v in ("1", "true", "yes", "on", "y", "t"):
        return True
    if v in ("0", "false", "no", "off", "n", "f"):
        return False
    return None


# demo accepts the full parameter name and the short name (case-insensitive).
PARAM_ALIASES = {
    "img": "image",
    "rb": "runstopbutton",
    "wpb": "writeprotectbutton",
    "pwr": "powerswitch",
    "shd": "shared_dir",
    "shfs": "shared_filesystem",
    "if": "interface",
}


class DeviceModel(object):
    """Tracks enabled devices and a few parameters per device."""

    def __init__(self):
        self.enabled = {}       # device name -> bool
        self.params = {}        # device name -> {param: value}
        self.current = None     # device selected with "sd"
        self.menu = "main"      # only used when parsing a command file

    def to_dict(self):
        return {"enabled": dict(self.enabled), "params": dict((k, dict(v)) for k, v in self.params.items()),
                "current": self.current}

    # ---- commands -------------------------------------------------------

    def device_command(self, line):
        """Apply one device menu command line. Returns True if it changed the model."""
        line = line.split("#", 1)[0].strip()
        if not line:
            return False
        f = line.split()
        op = f[0].lower()
        if op == "en" and len(f) == 2:
            self.enabled[_norm(f[1])] = True
            return True
        if op == "dis" and len(f) == 2:
            dev = _norm(f[1])
            self.enabled[dev] = False
            # demo: disabling an RL drive switches its power off
            if dev in self.params and "powerswitch" in self.params[dev]:
                self.params[dev]["powerswitch"] = "0"
            return True
        if op == "sd" and len(f) == 2:
            self.current = _norm(f[1])
            return True
        if op == "p" and self.current and len(f) in (2, 3):
            name = _norm(f[1])
            if name == "panel":
                return False
            name = PARAM_ALIASES.get(name, name)
            if len(f) == 2:
                # "p image" with no value: string parameters are cleared
                if name == "image":
                    self.params.setdefault(self.current, {})["image"] = ""
                    return True
                return False
            self.params.setdefault(self.current, {})[name] = f[2]
            return True
        return False

    def script_line(self, line):
        """Apply one command file line, following the main / device menu."""
        line = line.split("#", 1)[0].strip()
        if not line:
            return True
        low = line.lower()
        if low.startswith(".end"):
            return False                        # rest of the file is ignored
        if low.startswith("."):
            return True
        if self.menu == "main":
            if low in ("d", "dc"):
                self.menu = low.upper()
            elif low == "q":
                self.menu = "exit"
                return False
            return True
        if self.menu in DEVICE_MENUS:
            if low == "q":
                self.menu = "main"
            else:
                self.device_command(line)
        return True

    @classmethod
    def from_cmdfile(cls, path):
        m = cls()
        try:
            with open(path) as f:
                for line in f:
                    if not m.script_line(line):
                        break
        except (IOError, OSError, UnicodeDecodeError):
            pass
        return m

    # ---- queries --------------------------------------------------------

    def is_on(self, dev):
        return bool(self.enabled.get(dev))

    def param(self, dev, name, default=""):
        return self.params.get(dev, {}).get(name, default)


# ---------------------------------------------------------------------------
# Live device panel: the device list shown to the user, and the commands for
# each button.
# ---------------------------------------------------------------------------

def device_list(model, platform):
    """Controllers with their drives, serial lines, line clock: what can be
    switched in the running demo, and how it is set now."""
    cat = core.catalog(platform)
    ctrls = []
    for c in cat["controllers"]:
        drives = []
        for unit in range(c["drive_count"]):
            dev = "%s%d" % (c["drive_prefix"], unit)
            p = model.params.get(dev, {})
            known = dev in model.enabled or dev in model.params
            d = {
                "name": dev, "unit": unit,
                "on": model.is_on(dev) if not c.get("box") else model.is_on(c["key"]),
                "known": known,
                "image": p.get("image", ""),
                "type": p.get("type", ""),
            }
            if "speed" in c["features"]:     # RL01/02: front panel buttons
                d["run"] = _bool(p.get("runstopbutton", "0")) is True
                d["power"] = _bool(p.get("powerswitch", "0")) is True
                d["writeprotect"] = _bool(p.get("writeprotectbutton", "0")) is True
            drives.append(d)
        entry = {
            "key": c["key"], "label": c["label"], "title": c["title"],
            "on": model.is_on(c["key"]),
            "box": c.get("box"),
            "rl_buttons": "speed" in c["features"],
            "drives": drives,
        }
        if c.get("box"):
            entry["box_power"] = _bool(model.param(c["box"], "powerswitch", "0")) is True
        ctrls.append(entry)
    others = []
    for s in cat["serial"]:
        p = model.params.get(s["key"], {})
        others.append({"key": s["key"], "label": s["label"], "on": model.is_on(s["key"]),
                       "info": " ".join(x for x in (p.get("serialport", ""),
                                                    p.get("baudrate", "")) if x)})
    others.append({"key": "kw11", "label": "KW11 line clock", "on": model.is_on("kw11"), "info": ""})
    for n in cat.get("network", []):
        p = model.params.get(n["key"], {})
        others.append({"key": n["key"], "label": n["label"] + " Ethernet", "on": model.is_on(n["key"]),
                       "info": " ".join(x for x in (p.get("interface", ""), p.get("mac", "")) if x)})
    return {"controllers": ctrls, "others": others, "current": model.current}


SIMPLE_DEVICES = ("dl11", "dl11b", "kw11", "delqa")


def _find(platform, name):
    """-> (controller, drive unit or None) for a device name, or (None, None)."""
    cat = core.catalog(platform)
    for c in cat["controllers"]:
        if name == c["key"]:
            return c, None
        if name.startswith(c["drive_prefix"]):
            rest = name[len(c["drive_prefix"]):]
            if rest.isdigit() and int(rest) < c["drive_count"]:
                return c, int(rest)
    return None, None


def action_commands(model, platform, device, action, value=None):
    """Device menu command lines for one button press."""
    dev = _norm(device or "")
    action = _norm(action or "")
    if not re.match(r"^[a-z0-9]{1,12}$", dev):
        raise core.QbcError("Bad device name")

    if dev in SIMPLE_DEVICES:
        if action == "on":
            return ["en %s" % dev]
        if action == "off":
            return ["dis %s" % dev]
        raise core.QbcError("%s can only be switched on or off" % dev)

    c, unit = _find(platform, dev)
    if c is None:
        raise core.QbcError("Unknown device \"%s\"" % dev)

    if unit is None:                                    # a controller
        if action == "on":
            cmds = ["en %s" % dev]
            if c.get("box"):
                cmds += ["en %s" % c["box"], "sd %s" % c["box"], "p powerswitch 1"]
            return cmds
        if action == "off":
            cmds = []
            if c.get("box"):
                cmds += ["sd %s" % c["box"], "p powerswitch 0", "dis %s" % c["box"]]
            return cmds + ["dis %s" % dev]
        raise core.QbcError("A controller can only be switched on or off")

    rl = "speed" in c["features"]
    if action in ("on", "off") and c.get("box"):
        raise core.QbcError("%s drives are powered with their controller" % c["label"])
    if action == "on":
        cmds = ["en %s" % dev]
        if rl:      # enabling an RL drive leaves its power switch off
            cmds += ["sd %s" % dev, "p powerswitch 1"]
        return cmds
    if action == "off":
        return ["dis %s" % dev]
    if action in ("run", "load"):
        if not rl:
            raise core.QbcError("Only RL drives have a RUN/LOAD button")
        return ["sd %s" % dev, "p runstopbutton %d" % (1 if action == "run" else 0)]
    if action in ("wp", "wpoff"):
        if "writeprotect" not in c["features"]:
            raise core.QbcError("This drive has no WRITE PROT button")
        return ["sd %s" % dev, "p writeprotectbutton %d" % (1 if action == "wp" else 0)]
    if action == "image":
        image = (value or "").strip()
        if rl and _bool(model.param(dev, "runstopbutton", "0")):
            raise core.QbcError("%s is spinning: press LOAD first, then change the cartridge" % dev)
        if not image:
            return ["sd %s" % dev, "p image"]           # empty value: unload
        cmd = "p image %s" % core._tok(image, "Image path")
        if len(cmd) > core.MAX_CMD_LINE:
            raise core.QbcError("Image path is too long for the demo command line")
        return ["sd %s" % dev, cmd]
    raise core.QbcError("Unknown action \"%s\"" % action)


def restore_current(model, cmds):
    """After the panel's commands, select the device the operator had selected,
    so e.g. "p c 1" on the emulated CPU still works as expected."""
    before = model.current
    if not before or not any(c.startswith("sd ") for c in cmds):
        return cmds
    last = [c for c in cmds if c.startswith("sd ")][-1][3:]
    if last != before:
        return cmds + ["sd %s" % before]
    return cmds
