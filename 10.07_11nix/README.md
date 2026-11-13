# 11nix: a small Unix with TCP/IP for the PDP-11/23

11nix is a Unix-like operating system for PDP-11s with DEC memory management
(11/23, 11/23+, and alike) that runs on a QBone: it boots from an emulated
RL02 and talks to the network through the QBone's emulated DELQA.

11nix is by **N9TAX RetroLAB** (2026). It is based on
[Fuzix](https://www.fuzix.org/), the Unix-like system for small computers by
Alan Cox and others, and on the PDP-11 port of Fuzix, (c) 2026 Stensat Group
LLC, written for an FPGA PDP-11/40. N9TAX RetroLAB added the DEC MMU support,
the DEQNA/DELQA driver and the QBone port, a telnet and a web server, and
many fixes to the network layer and the C library (see
[below](#how-it-works-and-what-changed)).

```
Loading 11nix...
11nix version 0.1, based on FUZIX 0.5
...
PDP-11 port (c) 2026 Stensat Group LLC; DEC MMU, DEQNA/DELQA and QBone port (c) 2026 N9TAX RetroLAB
248KiB total RAM, 176KiB available to processes (14 processes max)
Mounting root fs (root_dev=0, rw): OK
Starting /init
qe0: DEQNA/DELQA at 174440, 08:00:2B:7E:C9:08
network: inet 192.168.1.80  netmask 255.255.255.0 ...
# uname -a
11nix 0.1  pdp11dec
# ping google.com
45 bytes from google.com (142.251.215.142) req=0
```

All the commands on four pages: [`11nix-reference.pdf`](11nix-reference.pdf)
(built by [`doc/make_reference.py`](doc/make_reference.py)).

What it has:

* a V7 Bourne shell, pipes, job control in the background, signals
* about 95 programs: `ls cp mv rm cat grep sed sort tr wc head tail od dd
  tar ed vi (levee) more ps kill free df du fsck date cal factor ubasic ...`
  and Colossal Cave (`/usr/games/advent`)
* TCP/IP: `netd` (uIP) with kernel sockets, started at boot
* a **telnet server**: log in from any computer on the LAN
* a **web server**: serves the files in `/www`
* `ping`, `dig`, `htget` (fetch a web page), `ntpdate` (sets the clock at
  boot), `telnet`, `tinyirc`, `ifconfig`
* up to 14 processes in 176 KB: four resident 44 KB programs, the others
  swapped to the RL02

> **Status: new.** Tested in SIMH as an 11/23 with 256 KB and a DELQA, on a
> real network, and boots on a real PDP-11/23 with a QBone. The network on
> the real machine is still being tested.

## Running it on a QBone

1. Update the QBone software (`./qbweb-update.sh`, or UPDATE in the web
   panel): this directory comes with `11nix.rl02.gz`, ready built.
2. In the web panel, CONFIGURE, make a new configuration:
   * SYSTEM: address width **18** (also for an 11/23+)
   * MEMORY: emulate memory (all of it: 11nix uses 248 KB)
   * SERIAL & CLOCK: **KW11 line clock** on, **60 Hz** (11nix counts 60 ticks
     a second; at 50 Hz its clock runs slow)
   * NETWORK: **DELQA** on
   * DISKS: RL11, drive 0, type RL02, image
     `/root/10.07_11nix/11nix.rl02` (the first start unpacks it from
     `11nix.rl02.gz`; your changes stay in `11nix.rl02` after that)
   * BOOTSTRAP: automatic (the RL boot loader)
3. START. At the ODT prompt `@` on the PDP-11 console, type `10000g`.
4. 11nix boots to a `#` prompt on the console: you are root.

A newer `11nix.rl02.gz` from an update is only unpacked when there is no
`11nix.rl02`: to start over with it, stop the emulation and delete
`/root/10.07_11nix/11nix.rl02` (your files on the old disk are lost).

### The network

Set the machine's address for your LAN before you use the network, on the
console (with `ed /etc/netrc`, or `vi /etc/netrc` on a VT100: `vi` is levee):

```
ipaddr  192.168.1.80
gateway 192.168.1.1
netmask 255.255.255.0
```

and your DNS server in `/etc/resolv.conf` (`nameserver 192.168.1.1`). Then
`halt`, and boot again (START, `10000g`). There is no DHCP. The Ethernet address is the DELQA's.

From another computer on the LAN (not the BeagleBone itself):

* `telnet 192.168.1.80`, log in as `root` (no password: set one with
  `passwd`)
* `http://192.168.1.80/` in a browser: the files in `/www`

On the PDP-11: `ping host` (until ctrl-C), `dig name`,
`htget http://host/path file`, `ntpdate -s pool.ntp.org`, `telnet host port`.

### Stopping

Type `halt` (or `reboot`: both stop the processor). They write everything
to the disk and mark it clean. `sync` alone is not enough: the disk stays
marked "in use". A kernel panic halts the processor too. To boot again, START
the configuration again (that reloads the RL boot loader, which the 11nix
kernel overwrites), then `10000g`.

If the machine stopped any other way (HALT switch, power, a crash), the next
boot finds the file system dirty and mounts it read-only (`warning: mounting
dirty file system`, and `WARNING: the disk is READ-ONLY`); the network does
not start then either. Type `fsck /dev/hd0` and answer `y` to its questions
(sockets that were open show up as inodes "not of correct type": zap them).
When it has repaired the disk it halts the processor: START again and
`10000g` boots, read-write.

## Running it in SIMH

```sh
gunzip -k 11nix.rl02.gz
pdp11 11nix.ini                # telnet localhost 2323, http://localhost:8080/
```

[`11nix.ini`](11nix.ini) sets up an 11/23 with 256 KB, an RL02 and a DELQA
on SIMH's NAT network. Inside, the address is 10.0.2.15, the defaults of
`/etc/netrc` and `/etc/resolv.conf`.

## How it works, and what changed

The Fuzix port of PR #1205 ([codeberg.org/EtchedPixels/FUZIX](https://codeberg.org/EtchedPixels/FUZIX/pulls/1205),
(c) 2026 Stensat Group LLC) runs on an FPGA PDP-11/40 with 128 KB. 11nix
adds the platform `pdp11dec` (the name in the sources), for DEC processors:

* a 48 KB kernel (pages 0-5), a movable 8 KB window (page 6) and the I/O page
* the kernel reaches user memory with MFPI/MTPI
* user programs in 44 KB banks above the kernel, as many as the memory holds,
  swapped to the RL02 when there are more processes than banks
* a DEQNA/DELQA driver (`/dev/eth`) with its buffers above the banks
* traps in user mode become signals; a kernel panic halts to ODT

On the way, testing turned up bugs in Fuzix's network layer and C library,
fixed in [`patches/`](patches/): incoming TCP connections were not implemented;
every socket file descriptor led to socket 0; sockets 4 and up used negative
buffer offsets; a closed socket could free a live one or corrupt an unrelated
file; datagrams went into the wrong buffer slot; `setjmp` did not save the
return address; failed system calls did not return -1; and more (see the
commit messages). The telnet and web servers are new. It also needs the GCC
fixes of [`../10.06_net11`](../10.06_net11/README.md#gcc-fixes-for-the-pdp-11),
one of which (32 bit loads through a pointer) 11nix ran into.

## Building

```sh
./build.sh
```

clones the Fuzix PDP-11 port, applies [`patches/`](patches/), and builds the
kernel, the programs and `11nix.rl02.gz`. It needs the patched GCC from
[`../10.06_net11/toolchain`](../10.06_net11/toolchain/build-toolchain.sh).

11nix: DEC MMU, DEQNA/DELQA and QBone port (c) 2026 N9TAX RetroLAB. PDP-11
port (c) 2026 Stensat Group LLC. Fuzix (c) Alan Cox and others. 11nix, like
Fuzix, is GPL 2; see the Fuzix sources for all the authors and the license.
