# Fuzix: a small Unix with TCP/IP for the PDP-11/23

[Fuzix](https://www.fuzix.org/) is a Unix-like operating system for small
computers, by Alan Cox and others. This directory has a Fuzix for PDP-11s with
DEC memory management (11/23, 11/23+, and alike) that runs on a QBone: it boots
from an emulated RL02 and talks to the network through the QBone's emulated
DELQA.

```
FUZIX version 0.5
...
248KiB total RAM, 176KiB available to processes (14 processes max)
Mounting root fs (root_dev=0, rw): OK
Starting /init
qe0: DEQNA/DELQA at 174440, 08:00:2b:7e:c9:08
network: inet 192.168.1.80  netmask 255.255.255.0 ...
# ls /bin | wc -l
     101
# ping google.com
45 bytes from google.com (142.251.215.142) req=0
```

What it has:

* a V7 Bourne shell, pipes, job control in the background, signals
* about 100 programs: `ls cp mv rm cat grep sed sort tr wc head tail od dd
  tar ed levee (vi) more ps kill free df du date cal factor ubasic ...`
  and Colossal Cave (`/usr/games/advent`)
* TCP/IP: `netd` (uIP) with kernel sockets, started at boot
* a **telnet server**: log in from any computer on the LAN
* a **web server**: serves the files in `/www`
* `ping`, `dig`, `htget` (fetch a web page), `ntpdate` (sets the clock at
  boot), `telnet`, `tinyirc`, `ifconfig`
* up to 14 processes in 176 KB: four resident 44 KB programs, the others
  swapped to the RL02

> **Status: new.** Tested in SIMH as an 11/23 with 256 KB and a DELQA, on a
> real network. It has not run on a real PDP-11 yet.

## Running it on a QBone

1. Update the QBone software (`./qbweb-update.sh`, or UPDATE in the web
   panel): this directory comes with `fuzix.rl02.gz`, ready built.
2. In the web panel, CONFIGURE, make a new configuration:
   * SYSTEM: address width **18** (also for an 11/23+)
   * MEMORY: emulate memory (all of it: Fuzix uses 248 KB)
   * SERIAL & CLOCK: **KW11 line clock** on, **60 Hz** (Fuzix counts 60 ticks
     a second; at 50 Hz its clock runs slow)
   * NETWORK: **DELQA** on
   * DISKS: RL11, drive 0, type RL02, image
     `/root/10.07_fuzix/fuzix.rl02` (the first start unpacks it from
     `fuzix.rl02.gz`; your changes stay in `fuzix.rl02` after that)
   * BOOTSTRAP: automatic (the RL boot loader)
3. START. At the ODT prompt `@` on the PDP-11 console, type `10000g`.
4. Fuzix boots to a `#` prompt on the console: you are root.

### The network

Set the machine's address for your LAN before you use the network, on the
console (with `ed /etc/netrc`, or `levee /etc/netrc` on a VT100):

```
ipaddr  192.168.1.80
gateway 192.168.1.1
netmask 255.255.255.0
```

and your DNS server in `/etc/resolv.conf` (`nameserver 192.168.1.1`). Then
reboot (or kill `netd` and start `netd -d`, `telnetd &` and `httpd &` again,
as `/etc/rc` does). There is no DHCP. The Ethernet address is the DELQA's.

From another computer on the LAN (not the BeagleBone itself):

* `telnet 192.168.1.80`, log in as `root` (no password: set one with
  `passwd`)
* `http://192.168.1.80/` in a browser: the files in `/www`

On the PDP-11: `ping host`, `dig name`, `htget http://host/path file`,
`ntpdate -s pool.ntp.org`, `telnet host port`.

### Stopping

Type `sync`, then halt the PDP-11 (HALT on the front panel, or in the web
panel). A kernel panic halts the processor too: `10000g` boots again.

## Running it in SIMH

```sh
gunzip -k fuzix.rl02.gz
pdp11 fuzix.ini                # telnet localhost 2323, http://localhost:8080/
```

[`fuzix.ini`](fuzix.ini) sets up an 11/23 with 256 KB, an RL02 and a DELQA
on SIMH's NAT network. Inside, the address is 10.0.2.15, the defaults of
`/etc/netrc` and `/etc/resolv.conf`.

## How it works, and what changed

The port of PR #1205 ([codeberg.org/EtchedPixels/FUZIX](https://codeberg.org/EtchedPixels/FUZIX/pulls/1205),
by Stensat Group) runs Fuzix on an FPGA PDP-11/40 with 128 KB. Its platform
`pdp11dec` here is for DEC processors:

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
commit messages). It also needs the GCC fixes of
[`../10.06_net11`](../10.06_net11/README.md#gcc-fixes-for-the-pdp-11), one of
which (32 bit loads through a pointer) Fuzix ran into.

## Building

```sh
./build.sh
```

clones the PDP-11 port, applies [`patches/`](patches/), and builds the kernel,
the programs and `fuzix.rl02.gz`. It needs the patched GCC from
[`../10.06_net11/toolchain`](../10.06_net11/toolchain/build-toolchain.sh).

Fuzix is GPL 2; see the Fuzix sources.
