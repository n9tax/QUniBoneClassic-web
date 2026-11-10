# net11: TCP/IP for the PDP-11 with a DEQNA/DELQA

net11 is a small stand-alone network system for the PDP-11. It boots without an
operating system and puts the machine on your LAN:

* gets an address from your router with **DHCP**
* answers **ping**, and pings other hosts (by name: it has a **DNS** resolver)
* sets its clock from **pool.ntp.org**
* serves a **web page** about itself on port 80 (`/`, `/stats`, `/about`)
* runs a **command monitor** on the console terminal and over **telnet** (port 23)

It needs a CPU with the EIS instructions (MUL, DIV, ASH): 11/23, 11/23+, 11/53,
11/73, 11/83 and others. It uses neither memory management nor floating point.
The Ethernet controller is a DEQNA or DELQA at 174440: on a QBone, the emulated
DELQA ([`10.02_devices/DELQA.md`](../10.02_devices/DELQA.md)).

> **Status: new.** net11 runs in emu11 (below), a PDP-11/23 simulator built
> around the same DELQA logic the QBone uses, connected to a real network. It
> has not run on a real PDP-11 yet.

## Running it on a QBone

1. Update the QBone software (`./qbweb-update.sh`): it brings
   `10.02_devices/5_boot/net11.lda`, ready built.
2. In the web panel, CONFIGURE, make a new configuration:
   * SYSTEM: the address width of your CPU (11/23: 18 bit, 11/23+: 22 bit)
   * MEMORY: emulate memory (unless the machine has its own)
   * SERIAL: **KW11 line clock** on. net11 keeps time with it.
     If the machine has no console card, switch on the DL11 too.
   * NETWORK: **DELQA** on
   * BOOTSTRAP: *Choose file*, `net11.lda`
   * no disk controllers needed
3. START. The console shows `Program loaded: start it at 1000 in ODT (1000g).`
4. On the PDP-11 console, at the ODT prompt `@`, type `1000g`.

```
net11 0.1 - TCP/IP for the PDP-11 with a DEQNA/DELQA
DELQA/DEQNA at 174440, Ethernet address 08:00:2b:4c:91:07
Asking for an address with DHCP ...
Type help for commands.
pdp11>
DHCP: address 192.168.1.57, gateway 192.168.1.1, DNS 192.168.1.1, lease 86400 s

Time: 2026-11-10 08:15:42 UTC
pdp11> ping google.com
PING 142.250.185.78: 56 data bytes
56 bytes from 142.250.185.78: icmp_seq=0 time=33 ms
```

Then, from any other computer on the LAN (not the BeagleBone itself, see
DELQA.md): open `http://192.168.1.57/` in a browser, or `telnet 192.168.1.57`.

The QBone's KW11 runs at 50 Hz unless told otherwise. In the US, add
`sd kw11` and `p freq 60` to the configuration's extra commands, or type
`hz 50` in net11.

## Commands

| | |
|---|---|
| `help` | the list |
| `sys` | uptime, time, Ethernet and IP addresses, counters, memory |
| `ip` | address configuration; `ip A.B.C.D MASK GATEWAY [DNS]` sets a static one |
| `dhcp` | ask for an address again |
| `ping HOST [COUNT]` | ICMP echo; ctrl-C stops |
| `host NAME` | DNS lookup |
| `date` | date and time (UTC) |
| `arp` | hardware address cache |
| `tcp` | TCP connections |
| `debug` | log TCP events on the console |
| `hz 50` / `hz 60` | line clock frequency |
| `mem ADDR [N]` | show memory, octal |
| `halt` | (console) back to ODT; start again with `1000g` |
| `quit` | (telnet) close the session |

## Limits

No IP fragments, no IPv6. Six TCP connections at a time, three of them telnet.
TCP receives in order only; it resends what was lost, with backoff. One web
page answer is built at a time. At the speed of an 11/23 a page takes about
half a second.

## Building

net11 is C with a little assembler, compiled with GCC for the `pdp11-aout`
target. [`toolchain/build-toolchain.sh`](toolchain/build-toolchain.sh) builds
binutils 2.42 and GCC 13.3.0 with the fixes below (10 to 20 minutes on a PC):

```sh
cd 10.06_net11/toolchain && ./build-toolchain.sh
export PATH=$PWD/pdp11-tools/bin:$PATH
cd ../src && make             # net11.lda
cp net11.lda ../../10.02_devices/5_boot/
```

### GCC fixes for the PDP-11

Testing net11 turned up four code generation bugs in GCC's PDP-11 back end
(13.3 and 15.2 alike). [`toolchain/gcc-13-pdp11-shift-mul-cmp.patch`](toolchain/gcc-13-pdp11-shift-mul-cmp.patch)
fixes them:

1. **Right shifts by `ash`/`ashc` were described as left shifts by a negative
   count.** That is undefined in GCC's RTL, and the optimizer concluded things
   like "the low bit of `x >> n` is zero". `if (s == 3 || s == 5 || s == 8)`
   (which GCC turns into `(0x128 >> s) & 1`) was compiled to "never".
   They are opaque now (`unspec`).
2. **Logical right shift of a byte was a left shift** (`ash $4` instead of
   `ash $-4`), and arithmetic right shift of a byte zero-extended it.
3. **Signed 32 bit comparisons** compared the low words signed when the high
   words were equal. libgcc's 32 bit division contains such a comparison:
   quotients above 0x3fff were wrong. Multiword compares now use GCC's
   generic word by word code.
4. **32 bit multiply** sometimes left the multiplicand in the odd register of
   the `mul` register pair, which `mul` does not read.

`tools/gen_cctest.py` generates the test programs that found them: about
17,000 checks of shifts, compares, multiply and divide for every integer type
at -O0, -O1, -Os and -O2, run in emu11 against results computed in Python.
Build net11 with the patched compiler only. (It also has its own 32 bit
division in assembler, `src/lib32.S`, and avoids byte shifts and signed
32 bit compares, but bugs 1 and 4 can strike anywhere.)

## emu11: testing without a PDP-11

`emu11/` is a small PDP-11/23 simulator for testing on a PC (Linux): the base
instruction set with EIS, the DL11 console, the KW11-L clock, and the DELQA
logic of the QBone (`10.02_devices/2_src/delqa_core.cpp`), connected to a
user mode NAT network with DHCP and DNS (libslirp, Debian: `libslirp-dev`).

```sh
cd emu11 && make
./emu11 ../src/net11.lda        # console on this terminal, ctrl-E quits
# from another terminal: http://127.0.0.1:8080/  and  telnet 127.0.0.1 2323
```

| option | |
|---|---|
| `-f host:guest` | forward a host TCP port to the PDP-11 (default 8080:80, 2323:23) |
| `-S ips` | run at a real PDP-11's speed, e.g. `-S 300000` for an 11/23 |
| `-L pct` | lose this percentage of the frames, both ways |
| `-F n` | lose the n-th TCP FIN the PDP-11 sends |
| `-D` | log every frame (TCP flags, sequence numbers) |
| `-T lo:hi` | trace the instructions between two addresses (octal) |
| `-t s` | stop after s seconds |
| `-s` | console on telnet port 11123 instead of the terminal |
| `-k`, `-n` | no line clock, no DELQA |

`tools/test_net11.py` boots net11 in emu11 and tests DHCP, NTP, ping, DNS,
HTTP and telnet.

## Files

| | |
|---|---|
| `src/crt0.S` | start, trap and interrupt entry, `probe()` for missing devices |
| `src/qe.c` | DEQNA/DELQA driver: descriptor lists, setup frame, polled |
| `src/net.c` | Ethernet, ARP, IPv4, ICMP and ping, UDP |
| `src/tcp.c` | TCP (server side) |
| `src/dhcp.c`, `dns.c`, `sntp.c` | DHCP client, DNS resolver, SNTP |
| `src/http.c`, `telnet.c`, `monitor.c` | web server, telnet server, command monitor |
| `src/lib.c`, `lib32.S`, `console.c`, `clock.c` | printing, strings, 32 bit division, DL11, KW11 |
| `tools/aout2lda.py` | a.out to absolute loader format (what `m lp` loads) |
