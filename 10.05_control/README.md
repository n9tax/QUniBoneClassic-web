# QBone / UniBone control panel

A web panel and a terminal menu for starting, stopping and configuring device
emulations on a QBone or UniBone, without hand-editing scripts.

* **Web panel** on port **1170** (as in PDP-11/70): choose controllers and drives,
  press START, and watch the `demo` console on a VT220-style screen.
* **`qbmenu`**: the same functions as a text menu for ssh or the serial
  console. After `install.sh` it comes up by itself when you log in.
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
* `demo` runs on a pseudo-terminal. Once its command file is done, it reads the
  console, which the web panel and `qbmenu` both share. Ctrl-C is not passed
  through, so a stray key can't kill `demo` while it drives the bus.
* **STOP** types `q` until `demo` has left its menus, so the PRU and the bus
  drivers are switched off properly. It falls back to SIGTERM and then SIGKILL.
  **KILL** goes straight to the signals.

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
| `qbcontrol/runner.py` | the `demo` process on a pty, console ring buffer, graceful stop |
| `qbcontrol/autostart.py` | DIP switch reading (GPIO 36–39) and autostart choice |
| `qbcontrol/server.py` | HTTP server and API |
| `qbcontrol/menu.py` | `qbmenu` |
| `www/` | the web panel (plain HTML/CSS/JS, no build step) |
| `tools/fake_demo.py` | stand-in for `demo`, for development without DEC gear |
| `~/.qbcontrol/` | settings, configurations, generated command files, logs |

`qbmenu` also takes commands: `qbmenu status`, `qbmenu start config:<id>`,
`qbmenu start stock:rt11.rl02/rt11v5.5fb_dl0.sh`, `qbmenu stop`, `qbmenu console`.

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
