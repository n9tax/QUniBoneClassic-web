# DELQA Ethernet controller (QBone)

The QBone can emulate a DEC **DELQA** (M7516). That was DEC's second QBUS Ethernet card, and
it is register compatible with the earlier **DEQNA** (M7504), so drivers for
either one work. The PDP-11 becomes a host on your LAN with its own Ethernet
address, sending and receiving through the BeagleBone's network port.

> **Status: new.** The controller logic passes its tests on a PC against
> simulated PDP-11 memory (`10.02_devices/3_test/delqa`), and the QBone build
> compiles. It has not run against a real QBUS yet. Reports are welcome.

## Switching it on

**Web panel:** CONFIGURE → your configuration → **NETWORK** → switch on the
DELQA. Leave the MAC address empty to get one automatically. The OPERATE page
has a live DELQA switch as well.

**By hand,** in the `demo` device menu or a command file:

```
sd delqa
p interface eth0            # Linux interface, default eth0
p mac 08:00:2b:12:34:56     # optional, default 08:00:2b + end of eth0's address
en delqa
```

`p` on the selected `delqa` shows the frame counters `tx_frames`, `rx_frames` and `rx_dropped`.

| | |
|---|---|
| CSR | 17774440 (standard first DEQNA/DELQA address) |
| Vector | set by the PDP-11 driver through the vector register, usually 120 |
| Interrupt level | BR4 |
| Station address | from `p mac`, or 08-00-2B-xx-xx-xx (DEC's prefix, plus the end of the BeagleBone's own address, so it is unique on your LAN) |

## Operating systems

| System | Software that drives a DEQNA/DELQA |
|---|---|
| RSX-11M-PLUS | DECnet-11M-PLUS (Phase IV) and the TCP/IP for RSX-11M-PLUS by Johnny Billquist |
| RSX-11M | DECnet-11M (Phase IV) |
| RT-11 | DECnet-RT (Phase IV) |
| 2.11BSD | `qe` driver. Note that BSD networking needs a CPU with separate instruction and data space and supervisor mode (11/73, 11/83, 11/84, 11/93/94), so not an 11/23. |
| Unix V6 | No Ethernet support. It would need a new driver and a network stack. |

DECnet Phase IV changes the station address to `AA-00-04-00-xx-yy`
(computed from the node number) through a setup frame. The emulation handles that:
the PDP-11 answers on whatever addresses its driver sets up.

## How it reaches the network

The DELQA uses a raw packet socket on the BeagleBone's interface, in
promiscuous mode. Frames for the PDP-11's addresses are passed to it; its own
frames go out on the cable unchanged.

* **The BeagleBone itself cannot talk to the PDP-11** over that interface. A
  frame sent through a packet socket never reaches Linux's own network stack,
  and the reverse is also true. Every other machine on the LAN can, and so can the
  internet through your router, if the PDP-11's software does IP.
* **GRO is switched off** on the interface when the DELQA is enabled
  (`ethtool -K eth0 gro off`). Linux would otherwise merge received TCP segments into
  frames bigger than Ethernet allows.
* WiFi interfaces usually refuse frames with a foreign source address, so use
  wired Ethernet.

## What is emulated

* Station address PROM, including the checksum that external loopback reads.
* Receive and transmit descriptor lists: chaining, buffers that start or end on
  an odd byte, frames that span several buffers. If the host runs out of
  buffers mid frame, the rest goes into the next list.
* Setup frames: 14 address filter entries, promiscuous and all-multicast
  modes (encoded in the setup frame length). The setup frame itself is looped
  back into the receive list.
* Internal and external loopback.
* Receive and transmit interrupts, and the nonexistent-memory error.
* Vector register with DELQA mode select. Self test reports "passed" at once.

Not emulated: the boot/diagnostic ROM (so no MOP network boot), the sanity
timer, collision statistics, and the extra status bits of the DELQA's own
"normal" mode. The card behaves as in DEQNA-lock mode, which the PDP-11
drivers use.

## Files

| File | |
|---|---|
| `2_src/delqa_core.*` | controller logic, independent of QUniBone |
| `2_src/delqa.*` | QUniBone device: registers, DMA, interrupt, worker threads |
| `2_src/ethernet_raw.*` | Linux packet socket |
| `3_test/delqa/` | tests for the controller logic on a PC: `make test` |
