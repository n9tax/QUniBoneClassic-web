/* delqa_core.cpp: DEC DELQA / DEQNA QBUS Ethernet controller, device logic.

 Copyright (c) 2026, the QUniBone contributors
 Permission is hereby granted, free of charge, to any person obtaining a
 copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:
 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED. See delqa_core.hpp.

 Behaviour follows the DEQNA / DELQA user guides (EK-DEQNA-UG, EK-DELQA-UG)
 as used by the RT-11, RSX-11 (DECnet) and 2.11BSD drivers:

 - The host builds descriptor lists in its memory and writes the list address
   into RBDL / TBDL. Writing the high word starts list processing.
 - Before using a descriptor the controller sets its flag word to 177777.
   A descriptor without V ends the list: CSR<XL> resp. CSR<RL> is set and the
   host must write the list address again. C chains to another descriptor.
 - Transmit: buffers are gathered until a descriptor with E. With S the frame
   is a setup packet: it loads the address filter, and is looped back into
   the receive list (receive status ESETUP).
 - Receive: a frame may fill several buffers; all but the last get status
   140000. The last gets the received byte length - 60 in RBL.
 - CSR<RI> / CSR<XI> request an interrupt when CSR<IE> is set.
 - CSR<IL> = 0 is internal loopback, CSR<EL> external loopback: transmitted
   frames go back into the receive list instead of to the wire.

 Not emulated: boot/diagnostic ROM (CSR<BD>), sanity timer (CSR<SE>), DELQA
 extended "normal mode" status bits (the controller behaves as in DEQNA-lock
 mode, which all PDP-11 drivers use), collision statistics.

 Locking: "mtx" protects all state. It is held only for short register
 updates, never during DMA: the PDP-11 may be stalled in a register access
 that waits for the lock, and DMA needs the bus.
 */

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <poll.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "delqa_core.hpp"

delqa_core_c::delqa_core_c(delqa_bus_if *_bus) {
	bus = _bus;
	stat_tx_frames = stat_rx_frames = stat_rx_dropped = stat_rx_filtered = 0;
	static const uint8_t default_mac[6] = { 0x08, 0x00, 0x2b, 0x00, 0x00, 0x01 };
	memcpy(mac, default_mac, 6);
	link_ok = false;
	csr = DELQA_CSR_XL | DELQA_CSR_RL;
	var = 0;
	rbdl_lo = rbdl_hi = tbdl_lo = tbdl_hi = 0;
	rbdl_ba = tbdl_ba = 0;
	rx_gen = tx_gen = 0;
	tx_kick = false;
	intr_level = false;
	setup_valid = promiscuous = all_multicast = false;
	memset(filter, 0, sizeof(filter));
	rx_used = 0;
	warned_bootrom = false;
	warned_oversize = 0;
	wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

delqa_core_c::~delqa_core_c() {
	if (wake_fd >= 0)
		close(wake_fd);
}

void delqa_core_c::wake(void) {
	uint64_t one = 1;
	if (wake_fd >= 0 && write(wake_fd, &one, sizeof(one)) < 0) {
		// counter full: the worker is awake anyway
	}
}

void delqa_core_c::logf(int level, const char *fmt, ...) {
	char buffer[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	bus->log(level, buffer);
}

void delqa_core_c::set_mac(const uint8_t *_mac) {
	std::lock_guard<std::mutex> lock(mtx);
	memcpy(mac, _mac, 6);
	publish_locked();
}

void delqa_core_c::get_mac(uint8_t *_mac) {
	std::lock_guard<std::mutex> lock(mtx);
	memcpy(_mac, mac, 6);
}

void delqa_core_c::set_link(bool ok) {
	std::lock_guard<std::mutex> lock(mtx);
	link_ok = ok;
	if (ok)
		csr |= DELQA_CSR_OK;
	else
		csr &= ~DELQA_CSR_OK;
	publish_locked();
}

uint16_t delqa_core_c::get_csr(void) {
	std::lock_guard<std::mutex> lock(mtx);
	return csr;
}

uint16_t delqa_core_c::get_vector(void) {
	std::lock_guard<std::mutex> lock(mtx);
	return var & DELQA_VAR_IV;
}

bool delqa_core_c::get_promiscuous(void) {
	std::lock_guard<std::mutex> lock(mtx);
	return promiscuous;
}

// Register contents as the PDP-11 reads them.
void delqa_core_c::compute_regs_locked(uint16_t *regs) {
	// Station address PROM checksum, read instead of bytes 0,1 in external
	// loopback mode (algorithm of the DEC boot drivers: rotate, add with
	// end-around carry).
	uint32_t checksum = 0;
	for (unsigned i = 0; i < 6; i += 2) {
		checksum <<= 1;
		if (checksum > 0xffff)
			checksum -= 0xffff;
		checksum += ((uint32_t) mac[i] << 8) | mac[i + 1];
		if (checksum > 0xffff)
			checksum -= 0xffff;
	}
	if (checksum == 0xffff)
		checksum = 0;
	for (unsigned i = 0; i < 6; i++)
		regs[i] = 0xff00 | mac[i];
	if (csr & DELQA_CSR_EL) {
		regs[0] = 0xff00 | (checksum & 0xff);
		regs[1] = 0xff00 | ((checksum >> 8) & 0xff);
	}
	regs[6] = var;
	regs[7] = csr;
}

uint16_t delqa_core_c::dati(unsigned index) {
	std::lock_guard<std::mutex> lock(mtx);
	uint16_t regs[8];
	compute_regs_locked(regs);
	return index < 8 ? regs[index] : 0;
}

// Hand register contents and the interrupt condition to the bus.
void delqa_core_c::publish_locked(void) {
	uint16_t regs[8];
	compute_regs_locked(regs);
	uint16_t vector = var & DELQA_VAR_IV;
	bool level = (csr & DELQA_CSR_IE) && (csr & (DELQA_CSR_RI | DELQA_CSR_XI)) && vector != 0;
	bool raise = level && !intr_level;
	bool cancel = !level && intr_level;
	intr_level = level;
	bus->publish(regs, vector, raise, cancel);
}

bool delqa_core_c::rx_pending_locked(void) {
	return !rxq.empty() && !(csr & DELQA_CSR_RL);
}

// ---------------------------------------------------------------------------
// resets and register writes
// ---------------------------------------------------------------------------

void delqa_core_c::software_reset_locked(void) {
	csr = DELQA_CSR_XL | DELQA_CSR_RL | (link_ok ? DELQA_CSR_OK : 0);
	rxq.clear();
	rx_used = 0;
	setup_valid = promiscuous = all_multicast = false;
	memset(filter, 0, sizeof(filter));
	tx_kick = false;
	rx_gen++;
	tx_gen++;
}

void delqa_core_c::bus_reset(void) {
	std::lock_guard<std::mutex> lock(mtx);
	software_reset_locked();
	var = 0; // DEQNA-lock mode, no vector
	rbdl_lo = rbdl_hi = tbdl_lo = tbdl_hi = 0;
	publish_locked();
}

void delqa_core_c::write_var_locked(uint16_t value) {
	var = (var & ~DELQA_VAR_RW) | (value & DELQA_VAR_RW);
	if (!(var & DELQA_VAR_MS)) {
		// DEQNA-lock mode: VAR<14:10> read as zero
		var &= ~(DELQA_VAR_OS | DELQA_VAR_RS | DELQA_VAR_ST);
	} else if (var & DELQA_VAR_RS) {
		// self test requested: completes at once, status 000 = passed
		var &= ~(DELQA_VAR_RS | DELQA_VAR_ST);
	}
}

void delqa_core_c::write_csr_locked(uint16_t value, access_e access) {
	// A byte write must not clear the write-one-to-clear bits of the other byte.
	if (access == ACCESS_LOW_BYTE)
		value = (value & 0x00ff) | (csr & 0xff00 & ~DELQA_CSR_W1C);
	else if (access == ACCESS_HIGH_BYTE)
		value = (value & 0xff00) | (csr & 0x00ff & ~DELQA_CSR_W1C);

	uint16_t old = csr;
	csr &= ~(value & DELQA_CSR_W1C);
	csr = (csr & ~DELQA_CSR_RW) | (value & DELQA_CSR_RW);

	if ((old & DELQA_CSR_SR) && !(value & DELQA_CSR_SR)) {
		// software reset happens when SR goes from 1 to 0
		software_reset_locked();
		logf(4, "software reset");
	}
	if ((csr & DELQA_CSR_BD) && !warned_bootrom) {
		warned_bootrom = true;
		logf(2, "boot/diagnostic ROM (CSR<BD>) is not emulated");
	}
}

void delqa_core_c::bus_write(unsigned index, uint16_t value, access_e access) {
	std::unique_lock<std::mutex> lock(mtx);
	bool kick = false;
	switch (index) {
	case 0:
	case 1:
		break; // station address: read only
	case 2:
		rbdl_lo = value;
		break;
	case 3:
		rbdl_hi = value;
		rbdl_ba = ((uint32_t) (rbdl_hi & DELQA_DSC_HIADDR) << 16) | (rbdl_lo & 0xfffe);
		csr &= ~DELQA_CSR_RL;
		rx_gen++;
		kick = true;
		break;
	case 4:
		tbdl_lo = value;
		break;
	case 5:
		tbdl_hi = value;
		tbdl_ba = ((uint32_t) (tbdl_hi & DELQA_DSC_HIADDR) << 16) | (tbdl_lo & 0xfffe);
		csr &= ~DELQA_CSR_XL;
		tx_gen++;
		tx_kick = true;
		kick = true;
		break;
	case 6:
		write_var_locked(value);
		break;
	case 7:
		write_csr_locked(value, access);
		kick = rx_pending_locked();
		break;
	}
	publish_locked();
	lock.unlock();
	if (kick)
		wake();
}

// ---------------------------------------------------------------------------
// receive side: address filter and queue
// ---------------------------------------------------------------------------

bool delqa_core_c::address_match_locked(const uint8_t *dst) {
	if (!setup_valid)
		return memcmp(dst, mac, 6) == 0; // until the first setup: station address only
	if (promiscuous)
		return true;
	if ((dst[0] & 1) && all_multicast)
		return true;
	for (unsigned i = 0; i < DELQA_SETUP_ADDRS; i++)
		if (memcmp(dst, filter[i], 6) == 0)
			return true;
	return false;
}

void delqa_core_c::enqueue_locked(const std::vector<uint8_t> &data, frame_type_e type) {
	if (rxq.size() >= RXQ_MAX) {
		stat_rx_dropped++;
		return;
	}
	frame_t f;
	f.data = data;
	f.type = type;
	rxq.push_back(f);
}

bool delqa_core_c::receive_frame(const uint8_t *frame, unsigned len) {
	if (len < 14)
		return false;
	std::unique_lock<std::mutex> lock(mtx);
	if (len > DELQA_FRAME_MAX) {
		// Linux GRO/LRO can merge frames; the PDP-11 can not take them.
		stat_rx_dropped++;
		if (warned_oversize++ < 3)
			logf(2, "dropped %u byte frame (> %u). Disable GRO: ethtool -K <if> gro off", len,
			DELQA_FRAME_MAX);
		return false;
	}
	// receiver off, or in loopback mode: nothing from the wire
	if (!(csr & DELQA_CSR_RE) || !(csr & DELQA_CSR_IL) || (csr & DELQA_CSR_EL)) {
		stat_rx_filtered++;
		return false;
	}
	if (!address_match_locked(frame)) {
		stat_rx_filtered++;
		return false;
	}
	if (rxq.size() >= RXQ_MAX) {
		stat_rx_dropped++;
		return false;
	}
	frame_t f;
	f.data.assign(frame, frame + len);
	f.type = FRAME_NORMAL;
	rxq.push_back(f);
	bool kick = rx_pending_locked();
	lock.unlock();
	if (kick)
		wake();
	return true;
}

// Setup packet: two 64 byte blocks, each an 8x8 byte matrix. Address n of
// block 0 (n = 0..6) is in column n+1: byte j at offset j*8 + n + 1.
// Block 1 holds addresses 7..13 the same way, starting at offset 64.
// A packet longer than 128 bytes encodes modes in its length:
// bit 0 all multicast, bit 1 promiscuous (bits 2..6 LEDs, sanity timer).
void delqa_core_c::process_setup_locked(const std::vector<uint8_t> &pkt) {
	unsigned count = pkt.size();
	memset(filter, 0, sizeof(filter));
	for (unsigned n = 0; n < 7; n++)
		for (unsigned j = 0; j < 6; j++) {
			unsigned o0 = j * 8 + n + 1;
			unsigned o1 = 64 + j * 8 + n + 1;
			if (o0 < count)
				filter[n][j] = pkt[o0];
			if (o1 < count)
				filter[n + 7][j] = pkt[o1];
		}
	if (count > 128) {
		all_multicast = (count & 1) != 0;
		promiscuous = (count & 2) != 0;
	} else
		promiscuous = false; // as VMS expects; all_multicast unchanged
	setup_valid = true;
	char line[200];
	int pos = snprintf(line, sizeof(line), "setup: %u bytes%s%s, addresses", count,
			promiscuous ? ", promiscuous" : "", all_multicast ? ", all multicast" : "");
	for (unsigned n = 0; n < DELQA_SETUP_ADDRS && pos < (int) sizeof(line) - 20; n++) {
		const uint8_t *a = filter[n];
		bool dup = false;
		for (unsigned k = 0; k < n; k++)
			if (!memcmp(filter[k], a, 6))
				dup = true;
		if (dup || !(a[0] | a[1] | a[2] | a[3] | a[4] | a[5]))
			continue;
		pos += snprintf(line + pos, sizeof(line) - pos, " %02x-%02x-%02x-%02x-%02x-%02x", a[0],
				a[1], a[2], a[3], a[4], a[5]);
	}
	logf(3, "%s", line);
}

// ---------------------------------------------------------------------------
// worker
// ---------------------------------------------------------------------------

bool delqa_core_c::wait_for_work(unsigned timeout_ms) {
	std::unique_lock<std::mutex> lock(mtx);
	if (tx_kick || rx_pending_locked())
		return true;
	lock.unlock();
	// A wake() after the check above leaves the eventfd readable: not lost.
	struct pollfd p;
	p.fd = wake_fd;
	p.events = POLLIN;
	p.revents = 0;
	if (wake_fd >= 0)
		poll(&p, 1, (int) timeout_ms);
	else
		usleep(timeout_ms * 1000);
	uint64_t count;
	if (wake_fd >= 0 && read(wake_fd, &count, sizeof(count)) < 0) {
		// nothing pending (EAGAIN)
	}
	lock.lock();
	return tx_kick || rx_pending_locked();
}

bool delqa_core_c::service(void) {
	// loopback and setup frames produced by the transmitter are received in turn
	bool any = false;
	for (unsigned guard = 0; guard < 1000; guard++) {
		bool did_tx = process_tx();
		bool did_rx = process_rx();
		if (!did_tx && !did_rx)
			break;
		any = true;
	}
	return any;
}

std::string delqa_core_c::state_text(void) {
	std::lock_guard<std::mutex> lock(mtx);
	char buf[160];
	snprintf(buf, sizeof(buf), "CSR %06o, tx_kick %d, rx queue %u, rx pending %d, RBDL %06o, TBDL %06o",
			csr, tx_kick ? 1 : 0, (unsigned) rxq.size(), rx_pending_locked() ? 1 : 0,
			(unsigned) rbdl_ba, (unsigned) tbdl_ba);
	return buf;
}

// DMA to/from nonexistent memory
void delqa_core_c::nxm(bool transmit, unsigned gen) {
	std::lock_guard<std::mutex> lock(mtx);
	if (transmit ? (gen != tx_gen) : (gen != rx_gen))
		return;
	csr |= DELQA_CSR_NI | DELQA_CSR_XI | (transmit ? DELQA_CSR_XL : DELQA_CSR_RL);
	logf(2, "%s: nonexistent memory", transmit ? "transmit" : "receive");
	publish_locked();
}

static void words_to_bytes(const uint16_t *w, unsigned bytecount, uint8_t *b) {
	for (unsigned i = 0; i < bytecount; i++)
		b[i] = (w[i / 2] >> ((i & 1) * 8)) & 0xff; // PDP-11: low byte first
}

static void bytes_to_words(const uint8_t *b, unsigned bytecount, uint16_t *w) {
	for (unsigned i = 0; i < (bytecount + 1) / 2; i++) {
		uint16_t lo = b[2 * i];
		uint16_t hi = (2 * i + 1 < bytecount) ? b[2 * i + 1] : 0;
		w[i] = lo | (hi << 8);
	}
}

bool delqa_core_c::process_tx(void) {
	uint32_t addr;
	unsigned gen;
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (!tx_kick)
			return false;
		tx_kick = false;
		if (csr & DELQA_CSR_XL)
			return false;
		addr = tbdl_ba;
		gen = tx_gen;
	}
	std::vector<uint8_t> pkt;
	std::vector<uint16_t> wbuf;
	bool overflow = false;

	for (unsigned guard = 0; guard < 10000; guard++) {
		if (bus->in_init())
			return true;
		uint16_t d[6];
		if (!bus->dma_read(addr, d, 6)) {
			nxm(true, gen);
			return true;
		}
		uint16_t flag = 0xffff; // descriptor in use
		if (!bus->dma_write(addr, &flag, 1)) {
			nxm(true, gen);
			return true;
		}
		if (!(d[1] & DELQA_DSC_V)) {
			// end of list
			std::lock_guard<std::mutex> lock(mtx);
			if (gen == tx_gen) {
				tbdl_ba = addr;
				csr |= DELQA_CSR_XL;
				publish_locked();
			}
			return true;
		}
		uint32_t ba = ((uint32_t) (d[1] & DELQA_DSC_HIADDR) << 16) | d[2];
		if (d[1] & DELQA_DSC_C) {
			addr = ba & ~1u;
			std::lock_guard<std::mutex> lock(mtx);
			if (gen != tx_gen)
				return true;
			tbdl_ba = addr;
			continue;
		}
		unsigned words = (0x10000u - d[3]) & 0xffff;
		if (words) {
			wbuf.resize(words);
			if (!bus->dma_read(ba & ~1u, wbuf.data(), words)) {
				nxm(true, gen);
				return true;
			}
			unsigned start = (d[1] & DELQA_DSC_H) ? 1 : 0;
			unsigned end = words * 2 - ((d[1] & DELQA_DSC_L) ? 1 : 0);
			if (end > start) {
				unsigned n = end - start;
				if (pkt.size() + n > 2048) {
					overflow = true;
				} else {
					std::vector<uint8_t> bytes(words * 2);
					words_to_bytes(wbuf.data(), words * 2, bytes.data());
					pkt.insert(pkt.end(), bytes.begin() + start, bytes.begin() + end);
				}
			}
		}

		uint16_t sw[2] = { DELQA_SW1_LASTNOT, 0 };
		bool end_of_message = (d[1] & DELQA_DSC_E) != 0;
		if (end_of_message) {
			sw[0] = 0; // last descriptor, no error. TSW2 (TDR) = 0
			if (d[1] & DELQA_DSC_S) {
				std::lock_guard<std::mutex> lock(mtx);
				if (gen != tx_gen)
					return true;
				process_setup_locked(pkt);
				enqueue_locked(pkt, FRAME_SETUP); // setup packets are looped back
			} else if (overflow || pkt.size() > DELQA_FRAME_MAX) {
				logf(2, "transmit: frame too long (%u bytes), not sent", (unsigned) pkt.size());
				sw[0] = DELQA_SW1_ERROR;
			} else {
				if (pkt.size() < DELQA_FRAME_MIN)
					pkt.resize(DELQA_FRAME_MIN, 0);
				bool loop;
				{
					std::lock_guard<std::mutex> lock(mtx);
					loop = !(csr & DELQA_CSR_IL) || (csr & DELQA_CSR_EL);
					if (loop)
						enqueue_locked(pkt, FRAME_LOOPBACK);
				}
				if (!loop && !bus->transmit(pkt.data(), pkt.size()))
					sw[0] = DELQA_SW1_ERROR | DELQA_TSW1_NOCAR;
				stat_tx_frames++;
			}
			pkt.clear();
			overflow = false;
		}
		if (!bus->dma_write(addr + 8, sw, 2)) {
			nxm(true, gen);
			return true;
		}
		addr += 12;
		std::lock_guard<std::mutex> lock(mtx);
		if (gen != tx_gen)
			return true; // host reloaded the list, or reset
		tbdl_ba = addr;
		if (end_of_message) {
			csr |= DELQA_CSR_XI;
			publish_locked();
		}
	}
	logf(1, "transmit: descriptor list loops, stopped");
	std::lock_guard<std::mutex> lock(mtx);
	csr |= DELQA_CSR_XL;
	publish_locked();
	return true;
}

bool delqa_core_c::process_rx(void) {
	bool did = false;
	std::vector<uint16_t> wbuf;

	for (unsigned frames = 0; frames < RXQ_MAX + 4; frames++) {
		frame_t f;
		uint32_t addr;
		unsigned gen;
		size_t used;
		{
			std::lock_guard<std::mutex> lock(mtx);
			if (!rx_pending_locked())
				return did;
			f = rxq.front();
			addr = rbdl_ba;
			gen = rx_gen;
			used = rx_used;
		}
		did = true;
		if (f.type == FRAME_NORMAL && f.data.size() < DELQA_FRAME_MIN)
			f.data.resize(DELQA_FRAME_MIN, 0); // runt from the wire: pad
		size_t total = f.data.size();
		unsigned rbl = total >= 60 ? (unsigned) (total - 60) : 0;

		bool restart = false;
		for (unsigned guard = 0; guard < 10000 && !restart; guard++) {
			if (bus->in_init())
				return did;
			uint16_t d[6];
			if (!bus->dma_read(addr, d, 6)) {
				nxm(false, gen);
				return did;
			}
			uint16_t flag = 0xffff;
			if (!bus->dma_write(addr, &flag, 1)) {
				nxm(false, gen);
				return did;
			}
			if (!(d[1] & DELQA_DSC_V)) {
				// out of buffers: keep the frame (and how much of it is placed)
				std::lock_guard<std::mutex> lock(mtx);
				if (gen == rx_gen) {
					rbdl_ba = addr;
					rx_used = used;
					csr |= DELQA_CSR_RL;
					publish_locked();
				}
				return did;
			}
			uint32_t ba = ((uint32_t) (d[1] & DELQA_DSC_HIADDR) << 16) | d[2];
			if (d[1] & DELQA_DSC_C) {
				addr = ba & ~1u;
				std::lock_guard<std::mutex> lock(mtx);
				if (gen != rx_gen) {
					restart = true;
					break;
				}
				rbdl_ba = addr;
				continue;
			}
			unsigned words = (0x10000u - d[3]) & 0xffff;
			size_t n = words * 2;
			if (n > total - used)
				n = total - used;
			if (n) {
				wbuf.resize((n + 1) / 2);
				bytes_to_words(f.data.data() + used, n, wbuf.data());
				if (!bus->dma_write(ba & ~1u, wbuf.data(), wbuf.size())) {
					nxm(false, gen);
					return did;
				}
			}
			used += n;
			bool last = used >= total;
			uint16_t sw[2];
			if (last)
				sw[0] = (f.type != FRAME_NORMAL ? DELQA_RSW1_ESETUP : 0) | (rbl & DELQA_RSW1_RBLHI);
			else
				sw[0] = DELQA_SW1_LASTNOT;
			sw[1] = (rbl & 0xff) | ((rbl & 0xff) << 8);
			if (!bus->dma_write(addr + 8, sw, 2)) {
				nxm(false, gen);
				return did;
			}
			addr += 12;
			std::lock_guard<std::mutex> lock(mtx);
			if (gen != rx_gen) {
				restart = true; // host reloaded the list, or reset
				break;
			}
			rbdl_ba = addr;
			rx_used = used;
			if (last) {
				rxq.pop_front();
				rx_used = 0;
				if (f.type == FRAME_NORMAL)
					stat_rx_frames++;
				csr |= DELQA_CSR_RI;
				publish_locked();
				break;
			}
		}
	}
	return did;
}
