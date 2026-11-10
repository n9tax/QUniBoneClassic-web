# QUniBone, with a web control panel and Ethernet

This is a fork of [QUniBone/QUniBoneClassic](https://github.com/QUniBone/QUniBoneClassic),
the software for Jörg Hoppe's **QBone** (QBUS) and **UniBone** (UNIBUS) cards. They plug a
BeagleBone Black into a PDP-11 backplane and emulate disks, serial lines, memory
and more. The original software is driven by shell scripts and the `demo` menu
program. This fork adds an easier way to run it, and an Ethernet controller.

| What | Where | Status |
|---|---|---|
| **Web control panel**: choose controllers, drives and disk images, press START, watch the console | [`10.05_control`](10.05_control/README.md) | works with a stand-in for `demo`; not yet run on real hardware |
| **`qbmenu`**: the same as a text menu for ssh or the serial console, shown at login | [`10.05_control`](10.05_control/README.md) | as above |
| **Autostart**: a fixed configuration, or one chosen by the DIP switches on the card | [`10.05_control`](10.05_control/README.md) | as above |
| **Live devices**: switch controllers and drives on and off, spin RL drives up and down, change disk images, all while the PDP-11 runs | [`10.05_control`](10.05_control/README.md) | as above |
| **DELQA Ethernet** (QBone): puts the PDP-11 on your LAN | [`10.02_devices/DELQA.md`](10.02_devices/DELQA.md) | new, tested on a PC against simulated PDP-11 memory; first hardware tests pending |
| **net11**: a small TCP/IP system for the PDP-11 itself: DHCP, ping, DNS, NTP, a web server and telnet, booted from the panel | [`10.06_net11`](10.06_net11/README.md) | new, runs in a PDP-11/23 simulator with the QBone's DELQA logic; not yet on hardware |
| **GCC fixes for the PDP-11**: four code generation bugs found while building net11, with a patch and a test generator | [`10.06_net11`](10.06_net11/README.md#gcc-fixes-for-the-pdp-11) | patch for GCC 13.3 |

The changes are on the branch [`control-panel`](https://github.com/n9tax/QUniBoneClassic-web/tree/control-panel).

## Installing and updating on the BeagleBone

Log in as root. In the home directory that holds the QUniBone software, fetch
the update script once:

```sh
cd ~
wget https://raw.githubusercontent.com/n9tax/QUniBoneClassic-web/control-panel/qbweb-update.sh
chmod +x qbweb-update.sh
```

Then, now and for every later update:

```sh
./qbweb-update.sh
```

It downloads this fork, shows which files changed and copies them in, keeping
the previous versions in `~/.qbweb-backup/`. It recompiles `demo` when its
sources changed, and restarts the control panel when that changed. Disk images,
the stock scripts and your panel configurations are left alone. `-n` only shows
what would change, `-y` skips the question, and `-a` recompiles everything.

Once the control panel is installed, the same is on its SYSTEM page: CHECK
lists what would change, UPDATE does it and shows the output.

The first time, also install the control panel:

```sh
cd ~/10.05_control && ./install.sh
```

(`GITREPO=n9tax/QUniBoneClassic-web ./update-code.sh control-panel` works too,
but it unpacks and recompiles everything each time.)

Open `http://<beaglebone>:1170/` in a browser, or type `qbmenu`. If your
`~/.profile` starts `./autostart.sh` in a `screen` session, remove that line
first: only one program may drive the bus. The panel does the autostart now.

## What the panel takes care of

* Writes the `demo` command files, so nobody has to hand-edit scripts.
* Passes the QBUS address width (`--aw 16/18/22`). `demo` stops with a fatal
  error without it, and the stock QBone scripts do not pass it.
* Leaves `demo` cleanly when stopping (`q` through its menus), so the bus
  drivers and the PRU are switched off properly.
* Lists the stock application scripts from files.retrocmp.com (`./update-files.sh`)
  and shows which ones have their disk images present.

---

## Original description

This is the software for both
Linux-to-UNIBUS bridge "UniBone"
and
Linux-to-QBUS bridge "QBone"

"UniBone" connects a BeagleBone Black micro Linux system to ancient DEC UNIBUS,
"QBone" does the same for DEC QBUS.

UniBone/QBone can keep old PDP-11s running, by emulating devices and aiding in repair.

As UNIBUS and QBUS are quite similar, only one software project compiles for both devices.

In-source differentiation is done via "#define UNIBUS" or "#define QBUS".
Source files special to only one bus are marked with suffix "_u" respective "_q".

See project pages at retrocmp.com [for UniBone](http://retrocmp.com/projects/unibone/) and [for QBone](http://retrocmp.com/projects/qbone/)
