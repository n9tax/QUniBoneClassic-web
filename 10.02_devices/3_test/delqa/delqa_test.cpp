/* delqa_test.cpp: tests the DELQA controller logic (delqa_core) on a PC.

 A simulated PDP-11 memory and a small "driver" exercise the controller the
 way the DEQNA/DELQA drivers of RT-11, RSX-11 and 2.11BSD do: read the
 station address, reset, program the vector, send a setup packet, hand over
 receive buffers, transmit and receive frames.

   make && ./delqa_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "delqa_core.hpp"

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
	printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---------------------------------------------------------------------------
// simulated bus: 256 KB memory, interrupt recorder, wire

class test_bus_c: public delqa_bus_if {
public:
	std::vector<uint16_t> mem; // word addressed
	uint16_t regs[8];
	unsigned interrupts = 0, cancels = 0;
	uint16_t last_vector = 0;
	std::vector<std::vector<uint8_t> > wire; // transmitted frames
	bool verbose = false;

	test_bus_c() :
			mem(0x40000 / 2, 0) {
		memset(regs, 0, sizeof(regs));
	}
	bool dma_read(uint32_t addr, uint16_t *buffer, unsigned wordcount) override {
		if (addr & 1 || addr + 2 * wordcount > mem.size() * 2)
			return false;
		memcpy(buffer, &mem[addr / 2], 2 * wordcount);
		return true;
	}
	bool dma_write(uint32_t addr, uint16_t *buffer, unsigned wordcount) override {
		if (addr & 1 || addr + 2 * wordcount > mem.size() * 2)
			return false;
		memcpy(&mem[addr / 2], buffer, 2 * wordcount);
		return true;
	}
	void publish(const uint16_t *dati, uint16_t vector, bool raise, bool cancel) override {
		memcpy(regs, dati, sizeof(regs));
		if (raise) {
			interrupts++;
			last_vector = vector;
		}
		if (cancel)
			cancels++;
	}
	bool transmit(const uint8_t *frame, unsigned len) override {
		wire.push_back(std::vector<uint8_t>(frame, frame + len));
		return true;
	}
	void log(int level, const char *msg) override {
		if (verbose || level <= 2)
			printf("  [delqa %d] %s\n", level, msg);
	}
	uint16_t w(uint32_t addr) {
		return mem[addr / 2];
	}
	void setw(uint32_t addr, uint16_t v) {
		mem[addr / 2] = v;
	}
	void setb(uint32_t addr, uint8_t b) {
		uint16_t &x = mem[addr / 2];
		if (addr & 1)
			x = (x & 0x00ff) | (b << 8);
		else
			x = (x & 0xff00) | b;
	}
	uint8_t b(uint32_t addr) {
		return (mem[addr / 2] >> ((addr & 1) * 8)) & 0xff;
	}
};

// ---------------------------------------------------------------------------
// driver helpers

enum {
	R_SA0 = 0, R_SA1, R_RBDL_LO, R_RBDL_HI, R_TBDL_LO, R_TBDL_HI, R_VAR, R_CSR
};

struct env_t {
	test_bus_c bus;
	delqa_core_c q;
	uint8_t mac[6] = { 0x08, 0x00, 0x2b, 0x12, 0x34, 0x56 };
	env_t() :
			q(&bus) {
		q.set_mac(mac);
		q.set_link(true);
		q.bus_reset();
	}
	void wr(unsigned reg, uint16_t v) {
		q.bus_write(reg, v, delqa_core_c::ACCESS_WORD);
	}
	uint16_t csr() {
		return bus.regs[R_CSR];
	}
	void run() {
		q.service();
	}
};

// write a descriptor at "at"
static void desc(test_bus_c &bus, uint32_t at, uint16_t bits, uint32_t buf, unsigned bytes) {
	bus.setw(at + 0, 0x8000); // flag: not used
	bus.setw(at + 2, bits | ((buf >> 16) & 0x3f));
	bus.setw(at + 4, buf & 0xffff);
	bus.setw(at + 6, (uint16_t) (-(int) ((bytes + 1) / 2)));
	bus.setw(at + 8, 0x8000); // status 1: not yet
	bus.setw(at + 10, 0);
}

static void end_of_list(test_bus_c &bus, uint32_t at) {
	bus.setw(at + 0, 0x8000);
	bus.setw(at + 2, 0); // not valid
	bus.setw(at + 4, 0);
	bus.setw(at + 6, 0);
	bus.setw(at + 8, 0x8000);
	bus.setw(at + 10, 0);
}

static void load_list(env_t &e, unsigned lo_reg, uint32_t list) {
	e.wr(lo_reg, list & 0xffff);
	e.wr(lo_reg + 1, (list >> 16) & 0x3f);
}

static void put_bytes(test_bus_c &bus, uint32_t addr, const std::vector<uint8_t> &data) {
	for (size_t i = 0; i < data.size(); i++)
		bus.setb(addr + i, data[i]);
}

static std::vector<uint8_t> frame(const uint8_t *dst, const uint8_t *src, unsigned len,
		uint8_t fill) {
	std::vector<uint8_t> f(len, fill);
	memcpy(&f[0], dst, 6);
	memcpy(&f[6], src, 6);
	f[12] = 0x60;
	f[13] = 0x03; // DECnet
	for (unsigned i = 14; i < len; i++)
		f[i] = (uint8_t) (fill + i);
	return f;
}

// setup packet as built by the DEC drivers: own address, broadcast, extra addresses
static std::vector<uint8_t> setup_packet(const std::vector<const uint8_t *> &addrs,
		unsigned len = 128) {
	std::vector<uint8_t> p(len < 128 ? 128 : len, 0);
	for (unsigned n = 0; n < 14; n++) {
		const uint8_t *a = addrs[n < addrs.size() ? n : 0]; // unused: repeat first
		unsigned base = n < 7 ? 0 : 64;
		unsigned col = (n % 7) + 1;
		for (unsigned j = 0; j < 6; j++)
			p[base + j * 8 + col] = a[j];
	}
	p.resize(len);
	return p;
}

static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t OTHER[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x99 };
static const uint8_t DECNET[6] = { 0xaa, 0x00, 0x04, 0x00, 0x05, 0x04 }; // node 1.5
static const uint8_t MCAST[6] = { 0xab, 0x00, 0x00, 0x03, 0x00, 0x00 };

// memory map of the test driver
static const uint32_t TX_LIST = 0x01000, RX_LIST = 0x02000;
static const uint32_t TX_BUF = 0x10000, RX_BUF = 0x20000; // above 64K: tests address bits
static const unsigned RX_BUFSIZE = 1518;

// standard bring-up: reset, vector, csr, rx ring of n buffers, setup packet
static void bring_up(env_t &e, unsigned n_rx, std::vector<const uint8_t *> addrs,
		unsigned setup_len = 128) {
	e.wr(R_CSR, DELQA_CSR_SR);
	e.wr(R_CSR, 0);
	e.wr(R_VAR, 0120);
	e.wr(R_CSR, DELQA_CSR_RE | DELQA_CSR_IE | DELQA_CSR_IL);
	for (unsigned i = 0; i < n_rx; i++)
		desc(e.bus, RX_LIST + 12 * i, DELQA_DSC_V, RX_BUF + i * 0x800, RX_BUFSIZE);
	end_of_list(e.bus, RX_LIST + 12 * n_rx);
	load_list(e, R_RBDL_LO, RX_LIST);
	std::vector<uint8_t> sp = setup_packet(addrs, setup_len);
	put_bytes(e.bus, TX_BUF, sp);
	desc(e.bus, TX_LIST, DELQA_DSC_V | DELQA_DSC_E | DELQA_DSC_S
			| ((sp.size() & 1) ? DELQA_DSC_L : 0), TX_BUF, sp.size());
	end_of_list(e.bus, TX_LIST + 12);
	load_list(e, R_TBDL_LO, TX_LIST);
	e.run();
}

static unsigned rx_len(test_bus_c &bus, uint32_t d) {
	return ((bus.w(d + 8) & DELQA_RSW1_RBLHI) | (bus.w(d + 10) & 0xff)) + 60;
}

// ---------------------------------------------------------------------------

static void test_registers(void) {
	printf("registers, reset, vector\n");
	env_t e;
	CHECK((e.bus.regs[0] & 0xff) == 0x08 && (e.bus.regs[5] & 0xff) == 0x56,
			"station address %06o %06o", e.bus.regs[0], e.bus.regs[5]);
	CHECK((e.bus.regs[0] & 0xff00) == 0xff00, "high byte of SA reg %06o", e.bus.regs[0]);
	CHECK(e.csr() == (DELQA_CSR_XL | DELQA_CSR_RL | DELQA_CSR_OK), "csr after reset %06o",
			e.csr());
	e.wr(R_VAR, 0120);
	CHECK(e.bus.regs[R_VAR] == 0120 && e.q.get_vector() == 0120, "var %06o",
			e.bus.regs[R_VAR]);
	// DELQA mode, self test
	e.wr(R_VAR, DELQA_VAR_MS | DELQA_VAR_RS | 0120);
	CHECK(e.bus.regs[R_VAR] == (DELQA_VAR_MS | 0120), "var self test %06o",
			e.bus.regs[R_VAR]);
	// checksum in external loopback (EL) mode
	e.wr(R_CSR, DELQA_CSR_EL | DELQA_CSR_IL);
	uint32_t c = 0;
	for (int i = 0; i < 6; i += 2) {
		c <<= 1;
		if (c > 0xffff)
			c -= 0xffff;
		c += (e.mac[i] << 8) | e.mac[i + 1];
		if (c > 0xffff)
			c -= 0xffff;
	}
	CHECK((e.bus.regs[0] & 0xff) == (c & 0xff) && (e.bus.regs[1] & 0xff) == ((c >> 8) & 0xff),
			"checksum regs %06o %06o expected %06o", e.bus.regs[0], e.bus.regs[1], c);
	// software reset on SR 1 -> 0 keeps VAR
	e.wr(R_CSR, DELQA_CSR_SR | DELQA_CSR_RE);
	e.wr(R_CSR, DELQA_CSR_RE);
	CHECK(e.csr() == (DELQA_CSR_XL | DELQA_CSR_RL | DELQA_CSR_OK), "csr after SR %06o",
			e.csr());
	CHECK(e.bus.regs[R_VAR] == (DELQA_VAR_MS | 0120), "var kept %06o", e.bus.regs[R_VAR]);
	// bus INIT clears VAR
	e.q.bus_reset();
	CHECK(e.bus.regs[R_VAR] == 0, "var after INIT %06o", e.bus.regs[R_VAR]);
}

static void test_setup_and_receive(void) {
	printf("setup packet, loopback of setup, receive filter\n");
	env_t e;
	bring_up(e, 4, { e.mac, BCAST });
	// transmit status, XI
	CHECK(e.bus.w(TX_LIST) == 0xffff, "tx flag %06o", e.bus.w(TX_LIST));
	CHECK(e.bus.w(TX_LIST + 8) == 0, "tx sw1 %06o", e.bus.w(TX_LIST + 8));
	CHECK(e.csr() & DELQA_CSR_XL, "XL after list end %06o", e.csr());
	CHECK(e.bus.wire.empty(), "setup packet must not go to the wire");
	// setup looped back into first receive buffer
	CHECK(e.bus.w(RX_LIST) == 0xffff, "rx flag %06o", e.bus.w(RX_LIST));
	CHECK(e.bus.w(RX_LIST + 8) & DELQA_RSW1_ESETUP, "rx sw1 ESETUP %06o", e.bus.w(RX_LIST + 8));
	CHECK(!(e.bus.w(RX_LIST + 8) & 0xc000), "rx sw1 last, no error %06o", e.bus.w(RX_LIST + 8));
	CHECK(rx_len(e.bus, RX_LIST) == 128, "setup loopback len %u", rx_len(e.bus, RX_LIST));
	CHECK(e.bus.b(RX_BUF + 1) == e.mac[0], "setup data looped");
	CHECK((e.csr() & (DELQA_CSR_RI | DELQA_CSR_XI)) == (DELQA_CSR_RI | DELQA_CSR_XI),
			"RI XI %06o", e.csr());
	CHECK(e.bus.interrupts == 1 && e.bus.last_vector == 0120, "one interrupt, vector %o (%u)",
			e.bus.last_vector, e.bus.interrupts);
	// ISR clears RI and XI: condition falls, interrupt withdrawn
	e.wr(R_CSR, e.csr());
	CHECK(!(e.csr() & (DELQA_CSR_RI | DELQA_CSR_XI)), "RI XI cleared %06o", e.csr());
	CHECK(e.csr() & DELQA_CSR_RE, "RE kept %06o", e.csr());

	// frames from the wire
	CHECK(e.q.receive_frame(&frame(BCAST, OTHER, 100, 1)[0], 100), "broadcast accepted");
	CHECK(e.q.receive_frame(&frame(e.mac, OTHER, 1514, 2)[0], 1514), "unicast accepted");
	CHECK(!e.q.receive_frame(&frame(OTHER, e.mac, 100, 3)[0], 100), "other unicast filtered");
	CHECK(!e.q.receive_frame(&frame(MCAST, OTHER, 100, 3)[0], 100), "multicast filtered");
	CHECK(!e.q.receive_frame(&frame(e.mac, OTHER, 1600, 3)[0], 1600), "oversize dropped");
	CHECK(e.q.wait_for_work(0), "work pending");
	e.run();
	uint32_t d1 = RX_LIST + 12, d2 = RX_LIST + 24;
	CHECK(e.bus.w(d1 + 8) == 0 && rx_len(e.bus, d1) == 100, "rx1 status %06o len %u",
			e.bus.w(d1 + 8), rx_len(e.bus, d1));
	CHECK(e.bus.b(RX_BUF + 0x800) == 0xff && e.bus.b(RX_BUF + 0x800 + 99) == (uint8_t) (1 + 99),
			"rx1 data");
	CHECK(rx_len(e.bus, d2) == 1514, "rx2 len %u (sw1 %06o sw2 %06o)", rx_len(e.bus, d2),
			e.bus.w(d2 + 8), e.bus.w(d2 + 10));
	CHECK(e.bus.b(RX_BUF + 0x1000 + 1513) == (uint8_t) (2 + 1513), "rx2 last byte");
	CHECK(e.bus.interrupts == 2, "interrupt for received frames (%u)", e.bus.interrupts);
	CHECK(e.q.stat_rx_filtered == 2 && e.q.stat_rx_dropped == 1, "stats filtered %lu dropped %lu",
			e.q.stat_rx_filtered, e.q.stat_rx_dropped);

	// runt from the wire is padded to 60
	e.wr(R_CSR, e.csr());
	CHECK(e.q.receive_frame(&frame(BCAST, OTHER, 42, 4)[0], 42), "runt accepted");
	e.run();
	uint32_t d3 = RX_LIST + 36;
	CHECK(rx_len(e.bus, d3) == 60, "runt padded, len %u", rx_len(e.bus, d3));

	// list exhausted: RL, frame waits until new buffers arrive
	CHECK(e.q.receive_frame(&frame(BCAST, OTHER, 200, 5)[0], 200), "accepted");
	e.run();
	CHECK(e.csr() & DELQA_CSR_RL, "RL when out of buffers %06o", e.csr());
	CHECK(!e.q.wait_for_work(0), "no work while RL");
	desc(e.bus, RX_LIST, DELQA_DSC_V, RX_BUF, RX_BUFSIZE);
	end_of_list(e.bus, RX_LIST + 12);
	load_list(e, R_RBDL_LO, RX_LIST);
	e.run();
	CHECK(!(e.csr() & DELQA_CSR_RL) || e.bus.w(RX_LIST + 8) == 0, "frame delivered after RBDL");
	CHECK(rx_len(e.bus, RX_LIST) == 200, "waiting frame len %u", rx_len(e.bus, RX_LIST));
}

static void test_transmit(void) {
	printf("transmit: chained buffers, odd bytes, padding, chain descriptor\n");
	env_t e;
	bring_up(e, 2, { e.mac, BCAST });
	e.bus.wire.clear();
	e.wr(R_CSR, e.csr());

	// frame of 102 bytes in 3 buffers: 14 header (even), 51 data starting on a
	// high byte, 37 bytes ending on a low byte
	std::vector<uint8_t> f = frame(OTHER, e.mac, 102, 7);
	put_bytes(e.bus, TX_BUF, std::vector<uint8_t>(f.begin(), f.begin() + 14));
	put_bytes(e.bus, TX_BUF + 0x101, std::vector<uint8_t>(f.begin() + 14, f.begin() + 65));
	put_bytes(e.bus, TX_BUF + 0x200, std::vector<uint8_t>(f.begin() + 65, f.end()));
	desc(e.bus, TX_LIST, DELQA_DSC_V, TX_BUF, 14);
	// buffer at 0x100, starts on high byte: 52 bytes of words, 51 used
	desc(e.bus, TX_LIST + 12, DELQA_DSC_V | DELQA_DSC_H, TX_BUF + 0x100, 52);
	desc(e.bus, TX_LIST + 24, DELQA_DSC_V | DELQA_DSC_E | DELQA_DSC_L, TX_BUF + 0x200, 37);
	// chain to a second list with a short frame (padded to 60)
	desc(e.bus, TX_LIST + 36, DELQA_DSC_V | DELQA_DSC_C, TX_LIST + 0x100, 0);
	std::vector<uint8_t> f2 = frame(BCAST, e.mac, 20, 9);
	put_bytes(e.bus, TX_BUF + 0x400, f2);
	desc(e.bus, TX_LIST + 0x100, DELQA_DSC_V | DELQA_DSC_E, TX_BUF + 0x400, 20);
	end_of_list(e.bus, TX_LIST + 0x10c);
	load_list(e, R_TBDL_LO, TX_LIST);
	e.run();

	CHECK(e.bus.wire.size() == 2, "frames on wire: %u", (unsigned ) e.bus.wire.size());
	if (e.bus.wire.size() == 2) {
		CHECK(e.bus.wire[0] == f, "gathered frame differs (len %u)",
				(unsigned ) e.bus.wire[0].size());
		CHECK(e.bus.wire[1].size() == 60, "padded to 60: %u", (unsigned ) e.bus.wire[1].size());
		CHECK(!memcmp(&e.bus.wire[1][0], &f2[0], 20), "short frame data");
	}
	CHECK(e.bus.w(TX_LIST + 8) == DELQA_SW1_LASTNOT, "not-last status %06o",
			e.bus.w(TX_LIST + 8));
	CHECK(e.bus.w(TX_LIST + 32) == 0, "last status %06o", e.bus.w(TX_LIST + 32));
	CHECK(e.bus.w(TX_LIST + 0x108) == 0, "chained list status %06o", e.bus.w(TX_LIST + 0x108));
	CHECK(e.csr() & DELQA_CSR_XL, "XL at end %06o", e.csr());
	CHECK(e.q.stat_tx_frames == 2, "tx stats %lu", e.q.stat_tx_frames);
}

static void test_spanning_receive(void) {
	printf("receive across several small buffers\n");
	env_t e;
	e.wr(R_CSR, DELQA_CSR_SR);
	e.wr(R_CSR, 0);
	e.wr(R_VAR, 0120);
	e.wr(R_CSR, DELQA_CSR_RE | DELQA_CSR_IE | DELQA_CSR_IL);
	// first receive buffer large enough for the setup echo, then 64 byte buffers
	desc(e.bus, RX_LIST, DELQA_DSC_V, RX_BUF, 256);
	for (unsigned i = 1; i < 6; i++)
		desc(e.bus, RX_LIST + 12 * i, DELQA_DSC_V, RX_BUF + i * 0x100, 64);
	end_of_list(e.bus, RX_LIST + 72);
	load_list(e, R_RBDL_LO, RX_LIST);
	std::vector<uint8_t> sp = setup_packet( { e.mac, BCAST });
	put_bytes(e.bus, TX_BUF, sp);
	desc(e.bus, TX_LIST, DELQA_DSC_V | DELQA_DSC_E | DELQA_DSC_S, TX_BUF, 128);
	end_of_list(e.bus, TX_LIST + 12);
	load_list(e, R_TBDL_LO, TX_LIST);
	e.run();
	std::vector<uint8_t> f = frame(BCAST, OTHER, 150, 3);
	e.q.receive_frame(&f[0], f.size());
	e.run();
	CHECK(e.bus.w(RX_LIST + 12 + 8) == DELQA_SW1_LASTNOT, "buf1 %06o", e.bus.w(RX_LIST + 20));
	CHECK(e.bus.w(RX_LIST + 24 + 8) == DELQA_SW1_LASTNOT, "buf2 %06o", e.bus.w(RX_LIST + 32));
	CHECK(e.bus.w(RX_LIST + 36 + 8) == 0 && rx_len(e.bus, RX_LIST + 36) == 150,
			"buf3 last %06o len %u", e.bus.w(RX_LIST + 44), rx_len(e.bus, RX_LIST + 36));
	bool same = true;
	for (unsigned i = 0; i < 150; i++)
		if (e.bus.b(RX_BUF + 0x100 * (1 + i / 64) + i % 64) != f[i])
			same = false;
	CHECK(same, "data spread over buffers");
	CHECK(e.bus.w(RX_LIST + 48 + 8) == 0x8000, "buf4 untouched %06o", e.bus.w(RX_LIST + 56));

	// frame split by end of list: continues in the next list
	std::vector<uint8_t> g = frame(BCAST, OTHER, 200, 8);
	e.q.receive_frame(&g[0], g.size());
	e.run(); // buffers 4,5 = 128 bytes, then list end
	CHECK(e.csr() & DELQA_CSR_RL, "RL mid frame %06o", e.csr());
	desc(e.bus, RX_LIST, DELQA_DSC_V, RX_BUF, 256);
	end_of_list(e.bus, RX_LIST + 12);
	load_list(e, R_RBDL_LO, RX_LIST);
	e.run();
	CHECK(e.bus.w(RX_LIST + 8) == 0 && rx_len(e.bus, RX_LIST) == 200,
			"rest of frame, sw1 %06o len %u", e.bus.w(RX_LIST + 8), rx_len(e.bus, RX_LIST));
	CHECK(e.bus.b(RX_BUF) == g[128] && e.bus.b(RX_BUF + 71) == g[199], "rest data");
}

static void test_loopback_and_modes(void) {
	printf("internal loopback, promiscuous and multicast setup, DECnet address\n");
	env_t e;
	// IL = 0: internal loopback
	e.wr(R_CSR, DELQA_CSR_SR);
	e.wr(R_CSR, 0);
	e.wr(R_VAR, 0120);
	e.wr(R_CSR, DELQA_CSR_RE | DELQA_CSR_IE);
	desc(e.bus, RX_LIST, DELQA_DSC_V, RX_BUF, RX_BUFSIZE);
	desc(e.bus, RX_LIST + 12, DELQA_DSC_V, RX_BUF + 0x800, RX_BUFSIZE);
	end_of_list(e.bus, RX_LIST + 24);
	load_list(e, R_RBDL_LO, RX_LIST);
	std::vector<uint8_t> f = frame(e.mac, e.mac, 80, 5);
	put_bytes(e.bus, TX_BUF, f);
	desc(e.bus, TX_LIST, DELQA_DSC_V | DELQA_DSC_E, TX_BUF, 80);
	end_of_list(e.bus, TX_LIST + 12);
	load_list(e, R_TBDL_LO, TX_LIST);
	e.run();
	CHECK(e.bus.wire.empty(), "loopback frame must not reach the wire");
	CHECK((e.bus.w(RX_LIST + 8) & DELQA_RSW1_ESETUP) && rx_len(e.bus, RX_LIST) == 80,
			"looped frame sw1 %06o len %u", e.bus.w(RX_LIST + 8), rx_len(e.bus, RX_LIST));
	CHECK(!e.q.receive_frame(&frame(e.mac, OTHER, 100, 1)[0], 100),
			"no wire frames in loopback mode");

	// promiscuous: setup length 130 (bit 1); all multicast: 129 (bit 0)
	env_t p;
	bring_up(p, 4, { p.mac, BCAST }, 130);
	CHECK(p.q.get_promiscuous(), "promiscuous");
	CHECK(p.q.receive_frame(&frame(OTHER, p.mac, 100, 1)[0], 100), "promiscuous accepts all");
	env_t m;
	bring_up(m, 4, { m.mac, BCAST }, 129);
	CHECK(m.q.receive_frame(&frame(MCAST, OTHER, 100, 1)[0], 100), "all multicast");
	CHECK(!m.q.receive_frame(&frame(OTHER, m.mac, 100, 1)[0], 100), "but not other unicast");

	// DECnet Phase IV changes the address via setup: AA-00-04-00-xx-yy
	env_t d;
	bring_up(d, 4, { DECNET, BCAST, MCAST });
	CHECK(d.q.receive_frame(&frame(DECNET, OTHER, 100, 1)[0], 100), "DECnet address");
	CHECK(d.q.receive_frame(&frame(MCAST, OTHER, 100, 1)[0], 100), "listed multicast");
	CHECK(!d.q.receive_frame(&frame(d.mac, OTHER, 100, 1)[0], 100),
			"hardware address no longer in filter");
}

static void test_csr_bytes_and_nxm(void) {
	printf("byte writes to CSR, nonexistent memory\n");
	env_t e;
	bring_up(e, 2, { e.mac, BCAST });
	uint16_t before = e.csr();
	CHECK(before & DELQA_CSR_RI, "RI set %06o", before);
	// low byte write (MOVB) with bit 7 (XI) set: clears XI only, RI stays
	e.q.bus_write(R_CSR, (before & 0xff) | 0xff00, delqa_core_c::ACCESS_LOW_BYTE);
	CHECK((e.csr() & DELQA_CSR_RI) && !(e.csr() & DELQA_CSR_XI), "MOVB: RI kept, XI cleared %06o",
			e.csr());
	// interrupts disabled: no new interrupt request
	unsigned n = e.bus.interrupts;
	e.wr(R_CSR, DELQA_CSR_RE | DELQA_CSR_IL | DELQA_CSR_RI);
	e.q.receive_frame(&frame(BCAST, OTHER, 100, 1)[0], 100);
	e.run();
	CHECK(e.bus.interrupts == n, "no interrupt without IE");
	// setting IE while RI pending requests one
	e.wr(R_CSR, DELQA_CSR_RE | DELQA_CSR_IL | DELQA_CSR_IE);
	CHECK(e.bus.interrupts == n + 1, "interrupt when IE set with RI pending");

	// transmit list pointing to nonexistent memory
	e.wr(R_CSR, DELQA_CSR_RE | DELQA_CSR_IL | DELQA_CSR_IE | DELQA_CSR_RI | DELQA_CSR_XI);
	load_list(e, R_TBDL_LO, 0x3f0000);
	e.run();
	CHECK((e.csr() & (DELQA_CSR_NI | DELQA_CSR_XI | DELQA_CSR_XL)) ==
			(DELQA_CSR_NI | DELQA_CSR_XI | DELQA_CSR_XL), "NXM %06o", e.csr());
}

int main(int argc, char **argv) {
	(void) argc;
	(void) argv;
	test_registers();
	test_setup_and_receive();
	test_transmit();
	test_spanning_receive();
	test_loopback_and_modes();
	test_csr_bytes_and_nxm();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
