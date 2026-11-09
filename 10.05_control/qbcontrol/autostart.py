# autostart.py - what to start when the control daemon boots.
#
# Modes (settings["autostart"]["mode"]):
#   off       nothing
#   fixed     always start settings["autostart"]["target"]
#   switches  read the 4 user DIP switches on the UniBone/QBone board
#             (same GPIOs as autostart.sh.example) and start the target
#             mapped to that value 1..15. Value 0 = start nothing.

import os

GPIO_ROOT = "/sys/class/gpio"
SWITCH_GPIOS = (36, 37, 38, 39)    # sw0..sw3, bank1 pins 4..7


def read_switches():
    """Return switch value 0..15, or None if the GPIOs are not accessible."""
    if os.environ.get("QBC_FAKE_SWITCHES"):
        return int(os.environ["QBC_FAKE_SWITCHES"]) & 15
    val = 0
    for bit, gpio in enumerate(SWITCH_GPIOS):
        gdir = os.path.join(GPIO_ROOT, "gpio%d" % gpio)
        try:
            if not os.path.isdir(gdir):
                with open(os.path.join(GPIO_ROOT, "export"), "w") as f:
                    f.write(str(gpio))
            with open(os.path.join(gdir, "direction"), "w") as f:
                f.write("in")
            with open(os.path.join(gdir, "value")) as f:
                if f.read().strip() == "1":       # 1 = switch "down" = ON
                    val |= 1 << bit
        except (IOError, OSError):
            return None
    return val


def resolve(settings):
    """Return (target, leds, reason). target is None if nothing is to be started."""
    a = settings.get("autostart", {})
    mode = a.get("mode", "off")
    if mode == "fixed":
        t = a.get("target") or ""
        if not t:
            return None, None, "autostart mode \"fixed\" but no target selected"
        return t, None, "fixed target"
    if mode == "switches":
        sw = read_switches()
        if sw is None:
            return None, None, "cannot read DIP switches (GPIO 36..39)"
        t = (a.get("switch_map") or {}).get(str(sw), "")
        if sw == 0 or not t:
            return None, None, "switch value %d selects nothing" % sw
        return t, (sw if a.get("leds", True) else None), "switch value %d" % sw
    return None, None, "autostart off"
