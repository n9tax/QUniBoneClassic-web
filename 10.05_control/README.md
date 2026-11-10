# QBone / UniBone control panel

A web panel and a terminal menu for starting, stopping and configuring device
emulations on a QBone or UniBone, without hand-editing scripts.

* **Web panel** on port **1170** (as in PDP-11/70): choose controllers and drives,
  press START, and watch the `demo` console on a VT220-style screen.
* **Ethernet** (QBone): a DELQA in the NETWORK section of a configuration
  puts the PDP-11 on your LAN, see [DELQA.md](../10.02_devices/DELQA.md).
* **Live devices**: switch controllers, drives, serial lines, the line
  clock and the DELQA on and off while the emulation runs; spin RL drives up and down, set
  write protect, change or unload disk images. Front panel controls: HALT/ENABLE
  (QBone), INIT and RESTART (power cycle).
* **`qbmenu`**: the same functions as a text menu for ssh or the serial
  console. After `install.sh` it comes up by itself when you log in.
* **Update**: SYSTEM → UPDATE runs `~/qbweb-update.sh` from the browser. CHECK
  lists what would change. UPDATE copies the new files, recompiles `demo` when
  needed and restarts the panel. Its output shows on the page.
* **Autostart**: the emulation that starts at boot, either a fixed choice or
  selected by the four DIP switches on the board, as `autostart.sh.example` did.

Only the Python 3 standard library is used (3.4 or newer, so the Debian 8
BeagleBone images work). Nothing needs to be installed from pip.

## Install on the BeagleBone

The QUniBone tree lives in root's home directory, so this directory is
`~/10.05_control`.

```sh
cd ~/10.05_control
sudo ./install.sh
```

This will:

1. install and start the systemd service `qbcontrol` (`qbcontrold` runs as root, because `demo` needs root for the PRUs),
2. link `qbmenu` and `qbcontrold` into `/usr/local/bin`,
3. add a small block to `~/.profile` so `qbmenu` appears on interactive logins (`--no-menu` skips this).

Open `http://<beaglebone>:1170/` in a browser.

**If your `~/.profile` starts `./autostart.sh` in `screen` on tty1, remove
that line.** Only one program can drive the bus; autostart is now done by
the daemon. `install.sh` warns if it finds that line.

`sudo ./install.sh --uninstall` removes the service, the commands and the login
hook. Your configurations stay in `~/.qbcontrol/`.

## How it works

`demo` (in `10.03_app_demo/4_deploy`) is still the engine. The panel only
writes its input and runs it:

```
browser ──HTTP──┐
                ├── qbcontrold ──pty── demo --aw 22 --cmdfile <generated>.cmd
qbmenu ──HTTP───┘
```

* A **configuration** is stored as JSON in `~/.qbcontrol/configs/`. When it
  starts, the panel generates the `.cmd` file that the stock scripts use
  (`d`, `pwr`, `m i`, `m ll dl.lst`, `en rl`, `sd rl0`, `p image …`, …). The editor
  shows that file as a printout before you start it.
* A **raw** configuration is a hand-written `.cmd` file plus a working directory.
  In the LIBRARY, any stock script can be copied into one.
* **Stock scripts** in `10.03_app_demo/5_applications` (from `./update-files.sh`)
  appear in the LIBRARY, with a lamp showing whether their disk images are present.
  They run `demo` directly with the same working directory and command file
  as their `.sh` wrapper.
* A configuration with the DELQA switched on adds `sd delqa`, `p interface eth0`,
  an optional `p mac …` and `en delqa` to the command file.
* `demo` runs on a pseudo-terminal. Once its command file is done, it reads the
  console, which the web panel and `qbmenu` both share. Ctrl-C is not passed
  through, so a stray key can't kill `demo` while it drives the bus.
* **STOP** types `q` until `demo` has left its menus, so the PRU and the bus
  drivers are switched off properly. It falls back to SIGTERM and then SIGKILL.
  **KILL** goes straight to the signals.

### Switching devices while the emulation runs

The DEVICES rack under the console (and `qbmenu` → 7) switches devices of the
running `demo`. Each button types the same commands you would type at the
`D>>>` prompt:

| Button | Commands |
|---|---|
| controller on / off | `en rl` / `dis rl` (RX: also `rxbox` and its power switch) |
| DELQA, DL11, KW11 on / off | `en delqa` / `dis delqa` (DELQA uses its `interface` and `mac` parameters) |
| drive on / off | `en rl1` (RL: plus `p powerswitch 1`) / `dis rl1` |
| RUN / LOAD (RL) | `sd rl0`, `p runstopbutton 1` / `0` |
| WP (RL) | `sd rl0`, `p writeprotectbutton 1` / `0` |
| CHANGE / UNLOAD | `sd rl0`, `p image <file>` / `p image` |
| HALT switch (QBUS) | `h 1` / `h 0`: asserts or releases the HALT line. The CPU enters ODT and stays there while HALT is asserted; after releasing it, type `P` or `<addr>G` on the PDP-11 console |
| INIT | `init`: pulses bus INIT, which resets the devices |
| RESTART | `pwr`: power cycles the bus (QBUS DCOK/POK, UNIBUS ACLO/DCLO); the CPU powers up as its jumpers say (ODT, or boot) |

The panel then selects your previous `sd` device again, so e.g. `p c 1` on
the emulated CPU keeps working. Changes last until `demo` stops; they are
not saved into the configuration.

`demo` cannot report its device state, so the daemon follows the commands:
first the command file, then every line typed on the console. The buttons
only work while `demo` waits at `D>>>` or `DC>>>`. In any other menu they are
refused with a hint. After leaving the device menu with `q`, `demo` deletes all
devices; typing `d` creates them again, all switched off, and the panel starts
over too.

### Updating from the web page

SYSTEM → UPDATE runs `qbweb-update.sh` (in the QUniBone home directory, see
the main README) with `-n` for CHECK or `-y` for UPDATE. It runs as its own systemd unit
(`systemd-run`), outside the `qbcontrol` service, so it keeps going when it
restarts the panel at the end. Its output and exit code are kept in
`~/.qbcontrol/update/`, and the restarted panel shows them. UPDATE is
refused while an emulation runs, and START is refused while an update runs.
When it is done, RELOAD PAGE loads the new version of the page.

### Things the panel takes care of

* **QBone address width.** On QBUS, `demo` stops with `FATAL` unless it gets
  `--aw 16|18|22`. The stock QBone scripts don't pass it. The panel always adds
  it, from SYSTEM → *QBUS CPU address width* (default 22 bit).
* **Command line length.** `demo` reads command-file lines into a 255-byte
  buffer, and comments count. The generator rejects lines that would be split,
  and image names with blanks or `#`.
* **Compressed images.** `demo` unpacks `image.gz` on first use, so the image
  browser lists `.gz`-only images under their plain name.

## Files

| Path | What |
|---|---|
| `qbcontrold` | daemon: web panel and JSON API, owns the `demo` process, runs autostart |
| `qbmenu` | terminal menu, a client of the daemon; starts the daemon if needed |
| `qbcontrol/core.py` | paths, platform detection, device catalog, `.cmd` generator, stock script scan |
| `qbcontrol/runner.py` | the `demo` process on a pty, console ring buffer, graceful stop, prompt tracking |
| `qbcontrol/devstate.py` | which devices are on, and the commands behind the device buttons |
| `qbcontrol/autostart.py` | DIP switch reading (GPIO 36–39) and autostart choice |
| `qbcontrol/server.py` | HTTP server and API |
| `qbcontrol/menu.py` | `qbmenu` |
| `www/` | the web panel (plain HTML/CSS/JS, no build step) |
| `tools/fake_demo.py` | stand-in for `demo`, for development without DEC gear |
| `~/.qbcontrol/` | settings, configurations, generated command files, logs |

`qbmenu` also takes commands: `qbmenu status`, `qbmenu start config:<id>`,
`qbmenu start stock:rt11.rl02/rt11v5.5fb_dl0.sh`, `qbmenu stop`, `qbmenu console`,
`qbmenu devices`, and `qbmenu dev <device> <action> [image]` with the actions
`on`, `off`, `run`, `load`, `wp`, `wpoff`, `image`. For example,
`qbmenu dev rl1 image /root/disks/games.rl02`, or `qbmenu dev rl1 image` to unload.
The front panel is the device `bus`: `qbmenu dev bus halt`, `enable`, `init`, `restart`.

## Developing on a PC

```sh
cd 10.05_control
QBC_HOME=/path/to/a/qunibone/tree \
QBC_DEMO=$PWD/tools/fake_demo.py \
QBC_FAKE_SWITCHES=3 \
./qbcontrold --port 11170 --no-autostart
```

`QBC_HOME` is the QUniBone tree, by default the parent of this directory.
`QBC_DEMO` replaces the `demo` binary, `QBC_DATA` moves `~/.qbcontrol`, and
`QBC_FAKE_SWITCHES` pretends a DIP switch value. `QBC_FAKE_BUS=UNIBUS` makes
the fake demo act like a UniBone.

## Security

The panel controls a root process. By default it has no password, so on a shared
network set one under SYSTEM. The browser then asks for it; any user name works.
`qbmenu` on the BeagleBone authenticates with a token in
`~/.qbcontrol/local.token`, which only root can read.
