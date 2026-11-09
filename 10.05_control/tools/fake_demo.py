#!/usr/bin/env python3
# fake_demo.py - stand-in for 10.03_app_demo/4_deploy/demo, for developing the
# control panel on a PC without any DEC gear. Mimics the command line, the
# command file processing (comments, .wait, .print, .end) and the device menu
# closely enough that start / console / stop can be exercised.
#
#   QBC_DEMO=$PWD/tools/fake_demo.py ./qbcontrold
#   QBC_FAKE_BUS=QBUS   emulate QBone behaviour (needs --aw)

import os
import sys
import time

BUS = os.environ.get("QBC_FAKE_BUS", "QBUS")
args = sys.argv[1:]
cmdfile = None
aw = 18 if BUS == "UNIBUS" else 0
i = 0
while i < len(args):
    a = args[i].lstrip("-").lower()
    if a in ("cf", "cmdfile"):
        cmdfile = args[i + 1]
        i += 1
    elif a in ("aw", "addresswidth"):
        aw = int(args[i + 1])
        i += 1
    elif a == "leds":
        i += 1
    i += 1

src = open(cmdfile) if cmdfile else None


def readline(prompt):
    global src
    while src:
        line = src.readline()
        if not line:
            src = None
            break
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        low = line.lower()
        if low.startswith(".wait"):
            ms = int(line[5:].split()[0])
            print("<<<\n<<< Input: waiting for %d milli seconds >>>\n<<<" % ms)
            sys.stdout.flush()
            time.sleep(ms / 1000.0)
            continue
        if low.startswith(".print"):
            print("<<< " + line[7:])
            continue
        if low.startswith(".end"):
            src = None
            break
        print(line)
        return line
    sys.stdout.write(prompt)
    sys.stdout.flush()
    s = sys.stdin.readline()
    if not s:
        time.sleep(0.2)     # demo loops on EOF too
        return ""
    return s.strip()


print("*** QUniBone %s technology demonstrator (FAKE, no hardware)" % BUS)
if os.geteuid() != 0:
    print("(fake) would FATAL: demo must be run as root to use prussdrv")

devices = {}


def device_menu(code):
    if BUS == "QBUS" and not aw:
        print("FATAL: Select address width of CPU via global parameter\n(command line -aw 16/18/22)")
        sys.exit(1)
    print("\n*** Test of device parameter interface and states.")
    cur = None
    while True:
        s = readline("\n%s>>>" % code)
        if not s:
            continue
        f = s.split()
        op = f[0].lower()
        if op == "q":
            print("Disabling devices, stopping PRU.")
            return
        elif op == "pwr":
            print("Power cycle: ACLO/DCLO" if BUS == "UNIBUS" else "Power cycle: DCOK/POK")
        elif op == "init":
            print("INIT pulsed.")
        elif op == "m" and len(f) > 1 and f[1] == "i":
            top = {16: "157776", 18: "757776", 22: "17757776"}.get(aw, "757776")
            print("Now emulating QBone memory in range 00000000..%s with DDR memory." % top)
        elif op == "m" and len(f) > 2 and f[1] == "ll":
            ok = os.path.exists(f[2])
            print(("Loaded MACRO-11 listing from file \"%s\" into memory: 63 words from 010000 to 010174."
                   % f[2]) if ok else "Error: file \"%s\" not found" % f[2])
        elif op == "en" and len(f) == 2:
            devices[f[1].lower()] = True
        elif op == "dis" and len(f) == 2:
            devices[f[1].lower()] = False
        elif op == "sd" and len(f) == 2:
            cur = f[1].lower()
            print("Current device is \"%s\"" % cur)
        elif op == "p" and cur:
            if len(f) == 3:
                print("  %-20s %s" % (f[1], f[2]))
                if f[1] in ("image", "img") and not (os.path.exists(f[2]) or os.path.exists(f[2] + ".gz")):
                    print("Error: image file \"%s\" not found" % f[2])
            elif len(f) == 1:
                print("Parameters of device %s: (fake)" % cur)
        elif op == "ld":
            print("Enabled devices:")
            for k, v in sorted(devices.items()):
                if v:
                    print("- %s" % k)
        elif op == "e" and len(f) == 2:
            print("EXAM %s -> 000000" % f[1])
        elif op == "d" and len(f) == 3:
            print("DEPOSIT %s <- %s" % (f[1], f[2]))
        else:
            print("Unknown command \"%s\"!" % s)


while True:
    print("\n\n*** QUniBone %s technology demonstrator build (fake)" % BUS)
    print("d           Emulate devices, with PDP-11 CPU arbitration")
    print("dc          Emulate devices and CPU, PDP-11 must be disabled.")
    print("q           Quit")
    s = readline("\n>>>").lower()
    if s == "q":
        break
    if s in ("d", "dc"):
        device_menu(s.upper())
sys.exit(0)
