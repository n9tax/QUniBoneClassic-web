/* emu11.cpp - a small PDP-11/23 for testing net11 on a PC.

 Copyright (c) 2026, the QUniBone contributors. MIT license, see delqa_core.hpp.

 What it has:
   CPU      KDF11-like: base instruction set, EIS (MUL DIV ASH ASHC XOR SOB),
            SXT MARK MFPS MTPS MFPT, no floating point, no memory management.
            16 bit addresses, 160000-177777 is the I/O page.
   memory   248 KB (18 bit, like a QBone at --aw 18), the CPU sees the low 56 KB
   DL11     console at 177560, vectors 60/64, on stdin/stdout
   KW11-L   line clock at 177546, vector 100, 60 Hz real time
   DELQA    at 174440: the QUniBone DELQA logic (10.02_devices/2_src/delqa_core.cpp),
            connected to a user mode NAT network (libslirp) with DHCP and DNS.
            Host ports are forwarded to the PDP-11 (default 8080->80, 2323->23).

 usage: emu11 [-t seconds] [-f hostport:guestport]... [-q] [-s] program.lda
        -s  console via a TCP port instead of stdin (emu11 prints the port)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <arpa/inet.h>
#include <vector>
#include <string>

#include <libslirp.h>

#include "delqa_core.hpp"

// ---------------------------------------------------------------- memory and bus

static const uint32_t MEMSIZE = 0760000; // bytes, 18 bit system
static uint8_t mem[MEMSIZE];

static int64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static bool quiet = false;
static void note(const char *fmt, ...) {
	if (quiet)
		return;
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "\r[emu11] ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

// ---------------------------------------------------------------- interrupts

struct irq_t {
	int level;
	uint16_t vector;
};
static std::vector<irq_t> irqs;

static void irq_raise(int level, uint16_t vector) {
	for (auto &i : irqs)
		if (i.vector == vector)
			return;
	irqs.push_back( { level, vector });
}
static void irq_cancel(uint16_t vector) {
	for (size_t k = 0; k < irqs.size(); k++)
		if (irqs[k].vector == vector) {
			irqs.erase(irqs.begin() + k);
			return;
		}
}

// ---------------------------------------------------------------- console DL11

static int con_in = 0, con_out = 1;
static int con_listen = -1;
static uint16_t rcsr, rbuf, xcsr = 0200;
static std::string con_inq;
static int64_t con_next_rx_ns;

static void con_write(uint8_t c) {
	c &= 0177;
	if (con_out >= 0 && write(con_out, &c, 1) < 0) {
		// terminal gone
	}
}

static void con_poll(void) {
	uint8_t buf[256];
	if (con_listen >= 0 && con_in < 0) {
		int fd = accept(con_listen, NULL, NULL);
		if (fd >= 0) {
			fcntl(fd, F_SETFL, O_NONBLOCK);
			con_in = con_out = fd;
			// telnet: character mode, no local echo
			static const uint8_t neg[] = { 255, 251, 1, 255, 251, 3 };
			if (write(fd, neg, sizeof neg) < 0) {
			}
		}
	}
	if (con_in >= 0) {
		ssize_t n = read(con_in, buf, sizeof buf);
		if (n > 0) {
			for (ssize_t k = 0; k < n; k++) {
				if (con_listen >= 0 && buf[k] == 255 && k + 2 < n) { // telnet command
					k += 2;
					continue;
				}
				if (buf[k] == '\n' && con_listen >= 0)
					continue;
				con_inq += (char) (buf[k] == '\n' ? '\r' : buf[k]);
			}
		} else if (n == 0 && con_listen >= 0) {
			close(con_in);
			con_in = con_out = -1;
		}
	}
	// one character per 1 ms, like a fast serial line
	if (!con_inq.empty() && !(rcsr & 0200) && now_ns() >= con_next_rx_ns) {
		rbuf = (uint8_t) con_inq[0];
		con_inq.erase(0, 1);
		rcsr |= 0200;
		con_next_rx_ns = now_ns() + 1000000;
		if (rcsr & 0100)
			irq_raise(4, 060);
	}
}

// ---------------------------------------------------------------- line clock KW11-L

static uint16_t lks = 0200;
static bool kw11_present = true;
static int64_t next_tick_ns;

static void clock_poll(void) {
	int64_t t = now_ns();
	if (t < next_tick_ns)
		return;
	next_tick_ns += 1000000000LL / 60;
	if (next_tick_ns < t)
		next_tick_ns = t + 1000000000LL / 60;
	lks |= 0200;
	if (lks & 0100)
		irq_raise(6, 0100);
}

// ---------------------------------------------------------------- DELQA

static Slirp *slirp;
static int loss_pct; // -L: drop this percentage of frames in each direction
static unsigned long lost_frames;
static bool dump_frames; // -D: log every frame
static int drop_fin_n, fin_count; // -F n: drop the n-th TCP FIN the PDP-11 sends
static bool is_fin(const uint8_t *f, unsigned len) {
	if (len < 54 || f[12] != 8 || f[13] != 0 || f[23] != 6)
		return false;
	const uint8_t *l4 = f + 14 + (f[14] & 15) * 4;
	return l4[13] & 1;
}
static void dump(const char *dir, const uint8_t *f, unsigned len, bool lost) {
	if (!dump_frames)
		return;
	double t = (now_ns() % 1000000000000LL) / 1e9;
	char line[200];
	int n = snprintf(line, sizeof line, "%9.3f %s %4u ", t, dir, len);
	unsigned type = len >= 14 ? (f[12] << 8 | f[13]) : 0;
	if (type == 0x0806)
		n += snprintf(line + n, sizeof line - n, "ARP op %u", f[21]);
	else if (type == 0x0800 && len >= 34) {
		const uint8_t *ip = f + 14;
		unsigned hl = (ip[0] & 15) * 4, proto = ip[9];
		const uint8_t *l4 = ip + hl;
		unsigned tot = ip[2] << 8 | ip[3];
		if (proto == 6 && len >= 14 + hl + 20) {
			unsigned thl = (l4[12] >> 4) * 4, fl = l4[13];
			unsigned dlen = tot - hl - thl;
			uint32_t seq = (l4[4] << 24) | (l4[5] << 16) | (l4[6] << 8) | l4[7];
			uint32_t ack = (l4[8] << 24) | (l4[9] << 16) | (l4[10] << 8) | l4[11];
			n += snprintf(line + n, sizeof line - n, "TCP %u>%u %s%s%s%s%s seq %u ack %u win %u len %u",
					l4[0] << 8 | l4[1], l4[2] << 8 | l4[3], fl & 2 ? "S" : "", fl & 16 ? "A" : "",
					fl & 8 ? "P" : "", fl & 1 ? "F" : "", fl & 4 ? "R" : "", seq, ack,
					l4[14] << 8 | l4[15], dlen);
		} else if (proto == 17)
			n += snprintf(line + n, sizeof line - n, "UDP %u>%u", l4[0] << 8 | l4[1], l4[2] << 8 | l4[3]);
		else if (proto == 1)
			n += snprintf(line + n, sizeof line - n, "ICMP type %u", l4[0]);
		else
			n += snprintf(line + n, sizeof line - n, "IP proto %u", proto);
	}
	fprintf(stderr, "\r%s%s\n", line, lost ? "  LOST" : "");
}
static bool lose(void) {
	if (loss_pct && (rand() % 100) < loss_pct) {
		lost_frames++;
		return true;
	}
	return false;
}
static uint16_t qe_dati[8];
static uint16_t qe_dato[8];
static uint16_t qe_vector;
static unsigned long qe_dma_words;

class emu_delqa_c: public delqa_bus_if {
public:
	bool dma_read(uint32_t addr, uint16_t *buffer, unsigned wordcount) override {
		if ((addr & 1) || addr + 2 * wordcount > MEMSIZE)
			return false;
		for (unsigned k = 0; k < wordcount; k++)
			buffer[k] = mem[addr + 2 * k] | (mem[addr + 2 * k + 1] << 8);
		qe_dma_words += wordcount;
		return true;
	}
	bool dma_write(uint32_t addr, uint16_t *buffer, unsigned wordcount) override {
		if ((addr & 1) || addr + 2 * wordcount > MEMSIZE)
			return false;
		for (unsigned k = 0; k < wordcount; k++) {
			mem[addr + 2 * k] = buffer[k] & 0xff;
			mem[addr + 2 * k + 1] = buffer[k] >> 8;
		}
		qe_dma_words += wordcount;
		return true;
	}
	void publish(const uint16_t *dati, uint16_t vector, bool intr_raise, bool intr_cancel)
			override {
		memcpy(qe_dati, dati, sizeof qe_dati);
		if (intr_cancel && qe_vector)
			irq_cancel(qe_vector);
		if (intr_raise && vector) {
			if (qe_vector && qe_vector != vector)
				irq_cancel(qe_vector);
			qe_vector = vector;
			irq_raise(4, vector);
		}
	}
	bool transmit(const uint8_t *frame, unsigned len) override {
		bool l = lose();
		if (drop_fin_n && is_fin(frame, len) && ++fin_count == drop_fin_n)
			l = true;
		dump("11>", frame, len, l);
		if (slirp && !l)
			slirp_input(slirp, frame, (int) len);
		return true;
	}
	void log(int level, const char *msg) override {
		if (level <= 2)
			note("DELQA: %s", msg);
	}
};

static delqa_core_c *qe;
static emu_delqa_c qe_if;

// slirp glue
struct timer_t2 {
	SlirpTimerId id;
	void *cb_opaque;
	int64_t expire_ms; // -1: off
};
static std::vector<timer_t2*> timers;

static slirp_ssize_t sl_send(const void *buf, size_t len, void *) {
	bool l = lose();
	dump("<11", (const uint8_t*) buf, (unsigned) len, l);
	if (!l)
		qe->receive_frame((const uint8_t*) buf, (unsigned) len);
	return (slirp_ssize_t) len;
}
static void sl_guest_error(const char *msg, void *) {
	note("slirp: %s", msg);
}
static int64_t sl_clock(void *) {
	return now_ns();
}
static void* sl_timer_new_opaque(SlirpTimerId id, void *cb_opaque, void *) {
	timer_t2 *t = new timer_t2 { id, cb_opaque, -1 };
	timers.push_back(t);
	return t;
}
static void sl_timer_free(void *timer, void *) {
	for (size_t k = 0; k < timers.size(); k++)
		if (timers[k] == timer) {
			timers.erase(timers.begin() + k);
			break;
		}
	delete (timer_t2*) timer;
}
static void sl_timer_mod(void *timer, int64_t expire_ms, void *) {
	((timer_t2*) timer)->expire_ms = expire_ms;
}
static void sl_register_socket(slirp_os_socket, void *) {
}
static void sl_unregister_socket(slirp_os_socket, void *) {
}
static void sl_notify(void *) {
}

static std::vector<struct pollfd> pfds;
static int sl_add_poll(slirp_os_socket fd, int events, void *) {
	struct pollfd p;
	p.fd = fd;
	p.events = 0;
	if (events & SLIRP_POLL_IN)
		p.events |= POLLIN;
	if (events & SLIRP_POLL_OUT)
		p.events |= POLLOUT;
	if (events & SLIRP_POLL_PRI)
		p.events |= POLLPRI;
	p.revents = 0;
	pfds.push_back(p);
	return (int) pfds.size() - 1;
}
static int sl_get_revents(int idx, void *) {
	int r = 0;
	short e = pfds[idx].revents;
	if (e & POLLIN)
		r |= SLIRP_POLL_IN;
	if (e & POLLOUT)
		r |= SLIRP_POLL_OUT;
	if (e & POLLPRI)
		r |= SLIRP_POLL_PRI;
	if (e & POLLERR)
		r |= SLIRP_POLL_ERR;
	if (e & POLLHUP)
		r |= SLIRP_POLL_HUP;
	return r;
}

static void net_poll(void) {
	if (!slirp)
		return;
	uint32_t timeout = 0;
	pfds.clear();
	slirp_pollfds_fill_socket(slirp, &timeout, sl_add_poll, NULL);
	int r = pfds.empty() ? 0 : poll(pfds.data(), pfds.size(), 0);
	slirp_pollfds_poll(slirp, r < 0, sl_get_revents, NULL);
	int64_t ms = now_ns() / 1000000;
	for (size_t k = 0; k < timers.size(); k++) {
		timer_t2 *t = timers[k];
		if (t->expire_ms >= 0 && t->expire_ms <= ms) {
			t->expire_ms = -1;
			slirp_handle_timer(slirp, t->id, t->cb_opaque);
		}
	}
}

static void net_init(const std::vector<std::pair<int, int>> &fwd, bool restricted) {
	static const SlirpCb cb = { sl_send, sl_guest_error, sl_clock, NULL, sl_timer_free,
			sl_timer_mod, NULL, NULL, sl_notify, NULL, sl_timer_new_opaque, sl_register_socket,
			sl_unregister_socket };
	SlirpConfig cfg;
	memset(&cfg, 0, sizeof cfg);
	cfg.version = 6;
	cfg.restricted = restricted;
	cfg.in_enabled = true;
	inet_pton(AF_INET, "10.0.2.0", &cfg.vnetwork);
	inet_pton(AF_INET, "255.255.255.0", &cfg.vnetmask);
	inet_pton(AF_INET, "10.0.2.2", &cfg.vhost);
	inet_pton(AF_INET, "10.0.2.15", &cfg.vdhcp_start);
	inet_pton(AF_INET, "10.0.2.3", &cfg.vnameserver);
	cfg.vhostname = "emu11";
	cfg.vdomainname = "emu11.test";
	cfg.if_mtu = 1500;
	cfg.if_mru = 1500;
	slirp = slirp_new(&cfg, &cb, NULL);
	struct in_addr host, guest;
	inet_pton(AF_INET, "127.0.0.1", &host);
	inet_pton(AF_INET, "10.0.2.15", &guest);
	for (auto &f : fwd) {
		if (slirp_add_hostfwd(slirp, 0, host, f.first, guest, f.second) < 0)
			note("cannot forward host port %d", f.first);
		else
			note("host 127.0.0.1:%d -> PDP-11 port %d", f.first, f.second);
	}
}

// ---------------------------------------------------------------- CPU

static uint16_t R[8];
static uint16_t PSW;
#define PC R[7]
#define SP R[6]
static bool halted = false, waiting = false;
static unsigned long long icount = 0;
static unsigned trace_lo = 1, trace_hi = 0; // -T lo:hi (octal): trace instructions
static double speed_ips; // -S: limit to this many instructions per second (real PDP-11 speed)

struct bus_error {
};
static uint16_t trap_vector; // set before throwing

enum {
	CC_C = 1, CC_V = 2, CC_Z = 4, CC_N = 8
};

static void io_reset(void) {
	rcsr = 0;
	xcsr = 0200;
	lks = 0200;
	irqs.clear();
	if (qe) {
		qe->bus_reset();
		memset(qe_dato, 0, sizeof qe_dato);
	}
}

// I/O page read, a = 16 bit address >= 160000
static bool io_read(uint16_t a, uint16_t *v) {
	switch (a) {
	case 0177776:
		*v = PSW;
		return true;
	case 0177560:
		*v = rcsr;
		return true;
	case 0177562:
		*v = rbuf;
		rcsr &= ~0200;
		irq_cancel(060);
		return true;
	case 0177564:
		*v = xcsr;
		return true;
	case 0177566:
		*v = 0;
		return true;
	case 0177546:
		if (!kw11_present)
			return false;
		*v = lks;
		return true;
	}
	if (a >= 0174440 && a <= 0174456 && qe) {
		*v = qe->dati((a - 0174440) / 2);
		return true;
	}
	return false;
}

// byte: 0 word, 1 low byte, 2 high byte (a even)
static bool io_write(uint16_t a, uint16_t v, int byte) {
	switch (a) {
	case 0177776:
		if (byte == 1)
			PSW = (PSW & 0xff00) | (v & 0xff);
		else if (byte == 2)
			PSW = (PSW & 0x00ff) | (v & 0xff00);
		else
			PSW = v;
		return true;
	case 0177560:
		if (byte != 2) {
			rcsr = (rcsr & ~0100) | (v & 0100);
			if (!(rcsr & 0100))
				irq_cancel(060);
			else if (rcsr & 0200)
				irq_raise(4, 060);
		}
		return true;
	case 0177562:
		return true;
	case 0177564:
		if (byte != 2) {
			xcsr = (xcsr & ~0100) | (v & 0100);
			if (xcsr & 0100)
				irq_raise(4, 064);
			else
				irq_cancel(064);
		}
		return true;
	case 0177566:
		if (byte != 2)
			con_write(v & 0xff);
		if (xcsr & 0100)
			irq_raise(4, 064);
		return true;
	case 0177546:
		if (!kw11_present)
			return false;
		if (byte != 2) {
			lks = (lks & ~0300) | (v & 0300);
			if (!(lks & 0100))
				irq_cancel(0100);
		}
		return true;
	}
	if (a >= 0174440 && a <= 0174456 && qe) {
		unsigned idx = (a - 0174440) / 2;
		delqa_core_c::access_e acc = delqa_core_c::ACCESS_WORD;
		if (byte == 1) {
			qe_dato[idx] = (qe_dato[idx] & 0xff00) | (v & 0xff);
			acc = delqa_core_c::ACCESS_LOW_BYTE;
		} else if (byte == 2) {
			qe_dato[idx] = (qe_dato[idx] & 0x00ff) | (v & 0xff00);
			acc = delqa_core_c::ACCESS_HIGH_BYTE;
		} else
			qe_dato[idx] = v;
		qe->bus_write(idx, qe_dato[idx], acc);
		return true;
	}
	return false;
}

static uint16_t rw(uint16_t a) {
	if (a & 1) {
		trap_vector = 4;
		throw bus_error();
	}
	if (a < 0160000)
		return mem[a] | (mem[a + 1] << 8);
	uint16_t v;
	if (!io_read(a, &v)) {
		trap_vector = 4;
		throw bus_error();
	}
	return v;
}
static uint8_t rb(uint16_t a) {
	if (a < 0160000)
		return mem[a];
	uint16_t v;
	if (!io_read(a & ~1, &v)) {
		trap_vector = 4;
		throw bus_error();
	}
	return (a & 1) ? v >> 8 : v & 0xff;
}
static void ww(uint16_t a, uint16_t v) {
	if (a & 1) {
		trap_vector = 4;
		throw bus_error();
	}
	if (a < 0160000) {
		mem[a] = v & 0xff;
		mem[a + 1] = v >> 8;
		return;
	}
	if (!io_write(a, v, 0)) {
		trap_vector = 4;
		throw bus_error();
	}
}
static void wb(uint16_t a, uint8_t v) {
	if (a < 0160000) {
		mem[a] = v;
		return;
	}
	if (!io_write(a & ~1, (a & 1) ? (uint16_t) v << 8 : v, (a & 1) ? 2 : 1)) {
		trap_vector = 4;
		throw bus_error();
	}
}

static uint16_t fetch(void) {
	uint16_t w = rw(PC);
	PC += 2;
	return w;
}

// effective address for mode 1..7
static uint16_t ea(int mode, int reg, bool byte) {
	uint16_t a;
	int inc = (byte && reg < 6) ? 1 : 2;
	switch (mode) {
	case 1:
		return R[reg];
	case 2:
		a = R[reg];
		R[reg] += inc;
		return a;
	case 3:
		a = R[reg];
		R[reg] += 2;
		return rw(a);
	case 4:
		R[reg] -= inc;
		return R[reg];
	case 5:
		R[reg] -= 2;
		return rw(R[reg]);
	case 6: {
		uint16_t x = fetch();
		return (uint16_t) (x + R[reg]);
	}
	case 7: {
		uint16_t x = fetch();
		return rw((uint16_t) (x + R[reg]));
	}
	}
	return 0;
}

// operand access helpers: spec = 6 bit mode/reg
struct opnd {
	bool reg;
	int r;
	uint16_t addr;
};
static opnd decode(int spec, bool byte) {
	opnd o;
	int mode = (spec >> 3) & 7, reg = spec & 7;
	if (mode == 0) {
		o.reg = true;
		o.r = reg;
		o.addr = 0;
	} else {
		o.reg = false;
		o.r = reg;
		o.addr = ea(mode, reg, byte);
	}
	return o;
}
static uint16_t getw(const opnd &o) {
	return o.reg ? R[o.r] : rw(o.addr);
}
static uint8_t getb(const opnd &o) {
	return o.reg ? R[o.r] & 0xff : rb(o.addr);
}
static void putw(const opnd &o, uint16_t v) {
	if (o.reg)
		R[o.r] = v;
	else
		ww(o.addr, v);
}
static void putb(const opnd &o, uint8_t v) {
	if (o.reg)
		R[o.r] = (R[o.r] & 0xff00) | v;
	else
		wb(o.addr, v);
}

static void setnz(uint16_t v, bool byte) {
	PSW &= ~(CC_N | CC_Z);
	if (byte) {
		if (v & 0x80)
			PSW |= CC_N;
		if (!(v & 0xff))
			PSW |= CC_Z;
	} else {
		if (v & 0x8000)
			PSW |= CC_N;
		if (!v)
			PSW |= CC_Z;
	}
}
static void setcc(int bit, bool on) {
	if (on)
		PSW |= bit;
	else
		PSW &= ~bit;
}

static void push(uint16_t v) {
	SP -= 2;
	ww(SP, v);
}
static uint16_t pop(void) {
	uint16_t v = rw(SP);
	SP += 2;
	return v;
}

static void trap(uint16_t vec) {
	uint16_t opsw = PSW, opc = PC;
	try {
		push(opsw);
		push(opc);
		PC = rw(vec);
		PSW = rw(vec + 2);
	} catch (bus_error&) {
		note("double bus error at trap to %03o, PC %06o: HALT", vec, opc);
		halted = true;
	}
}

static void illegal(void) {
	trap_vector = 010;
	throw bus_error();
}

static bool cond(uint16_t op) {
	bool N = PSW & CC_N, Z = PSW & CC_Z, V = PSW & CC_V, C = PSW & CC_C;
	switch (op & 0103400) {
	case 0000400:
		return true; // BR
	case 0001000:
		return !Z;
	case 0001400:
		return Z;
	case 0002000:
		return N == V; // BGE
	case 0002400:
		return N != V; // BLT
	case 0003000:
		return !Z && N == V; // BGT
	case 0003400:
		return Z || N != V; // BLE
	case 0100000:
		return !N;
	case 0100400:
		return N;
	case 0101000:
		return !C && !Z; // BHI
	case 0101400:
		return C || Z; // BLOS
	case 0102000:
		return !V;
	case 0102400:
		return V;
	case 0103000:
		return !C; // BCC
	case 0103400:
		return C; // BCS
	}
	return false;
}

static void exec(uint16_t op) {
	bool byte = op & 0100000;
	int top = (op >> 12) & 7;

	// double operand
	if (top >= 1 && top <= 6) {
		if (top == 6) { // ADD / SUB
			opnd s = decode((op >> 6) & 077, false);
			uint16_t src = getw(s);
			opnd d = decode(op & 077, false);
			uint16_t dst = getw(d);
			uint32_t res;
			if (!byte) {
				res = (uint32_t) dst + src;
				setcc(CC_V, !((src ^ dst) & 0x8000) && ((res ^ dst) & 0x8000));
				setcc(CC_C, res > 0xffff);
			} else {
				res = (uint32_t) dst - src;
				setcc(CC_V, ((src ^ dst) & 0x8000) && !((res ^ src) & 0x8000));
				setcc(CC_C, dst < src);
			}
			setnz(res & 0xffff, false);
			putw(d, res & 0xffff);
			return;
		}
		opnd s = decode((op >> 6) & 077, byte);
		uint16_t src = byte ? getb(s) : getw(s);
		opnd d = decode(op & 077, byte);
		switch (top) {
		case 1: // MOV
			if (byte) {
				setnz(src, true);
				if (d.reg)
					R[d.r] = (src & 0x80) ? (0xff00 | src) : src;
				else
					putb(d, src);
			} else {
				setnz(src, false);
				putw(d, src);
			}
			PSW &= ~CC_V;
			return;
		case 2: { // CMP
			uint16_t dst = byte ? getb(d) : getw(d);
			uint16_t m = byte ? 0x80 : 0x8000, mask = byte ? 0xff : 0xffff;
			uint16_t res = (src - dst) & mask;
			setnz(res, byte);
			setcc(CC_V, ((src ^ dst) & m) && !((dst ^ res) & m));
			setcc(CC_C, src < dst);
			return;
		}
		case 3: { // BIT
			uint16_t dst = byte ? getb(d) : getw(d);
			setnz(src & dst, byte);
			PSW &= ~CC_V;
			return;
		}
		case 4: { // BIC
			uint16_t dst = byte ? getb(d) : getw(d);
			uint16_t res = dst & ~src;
			setnz(res, byte);
			PSW &= ~CC_V;
			if (byte)
				putb(d, res);
			else
				putw(d, res);
			return;
		}
		case 5: { // BIS
			uint16_t dst = byte ? getb(d) : getw(d);
			uint16_t res = dst | src;
			setnz(res, byte);
			PSW &= ~CC_V;
			if (byte)
				putb(d, res);
			else
				putw(d, res);
			return;
		}
		}
	}

	// EIS and friends: 070xxx..077xxx
	if ((op & 0170000) == 0070000) {
		int r = (op >> 6) & 7;
		switch (op & 0177000) {
		case 0070000: { // MUL
			opnd s = decode(op & 077, false);
			int32_t p = (int32_t) (int16_t) R[r] * (int16_t) getw(s);
			if (!(r & 1)) {
				R[r] = (p >> 16) & 0xffff;
				R[r | 1] = p & 0xffff;
			} else
				R[r] = p & 0xffff;
			PSW &= ~(CC_N | CC_Z | CC_V | CC_C);
			if (p < 0)
				PSW |= CC_N;
			if (p == 0)
				PSW |= CC_Z;
			if (p < -32768 || p > 32767)
				PSW |= CC_C;
			return;
		}
		case 0071000: { // DIV
			opnd s = decode(op & 077, false);
			int16_t dv = (int16_t) getw(s);
			int32_t dd = (int32_t) (((uint32_t) R[r] << 16) | R[r | 1]);
			PSW &= ~(CC_N | CC_Z | CC_V | CC_C);
			if (dv == 0) {
				PSW |= CC_V | CC_C;
				return;
			}
			int64_t q = (int64_t) dd / dv, rem = (int64_t) dd % dv;
			if (q < -32768 || q > 32767) {
				PSW |= CC_V;
				return;
			}
			R[r] = q & 0xffff;
			R[r | 1] = rem & 0xffff;
			if (q < 0)
				PSW |= CC_N;
			if (q == 0)
				PSW |= CC_Z;
			return;
		}
		case 0072000: { // ASH
			opnd s = decode(op & 077, false);
			int sh = getw(s) & 077;
			if (sh & 040)
				sh -= 64;
			int16_t v = (int16_t) R[r];
			int16_t res;
			bool c = PSW & CC_C, vv = false;
			if (sh > 0) {
				int32_t t = v;
				for (int k = 0; k < sh; k++) {
					c = (t >> 15) & 1;
					int32_t n = (t << 1) & 0xffff;
					if ((n ^ t) & 0x8000)
						vv = true;
					t = n;
				}
				res = (int16_t) t;
			} else if (sh < 0) {
				int32_t t = v;
				for (int k = 0; k < -sh; k++) {
					c = t & 1;
					t >>= 1;
				}
				res = (int16_t) t;
			} else
				res = v;
			R[r] = (uint16_t) res;
			setnz((uint16_t) res, false);
			setcc(CC_V, vv);
			setcc(CC_C, c);
			return;
		}
		case 0073000: { // ASHC
			opnd s = decode(op & 077, false);
			int sh = getw(s) & 077;
			if (sh & 040)
				sh -= 64;
			int64_t v = (int32_t) (((uint32_t) R[r] << 16) | R[r | 1]);
			bool c = PSW & CC_C, vv = false;
			if (sh > 0) {
				for (int k = 0; k < sh; k++) {
					c = (v >> 31) & 1;
					int64_t n = (int32_t) ((uint32_t) v << 1);
					if ((n ^ v) & 0x80000000LL)
						vv = true;
					v = n;
				}
			} else if (sh < 0) {
				for (int k = 0; k < -sh; k++) {
					c = v & 1;
					v >>= 1;
				}
			}
			uint32_t u = (uint32_t) v;
			R[r] = u >> 16;
			R[r | 1] = u & 0xffff;
			PSW &= ~(CC_N | CC_Z);
			if (u & 0x80000000u)
				PSW |= CC_N;
			if (!u)
				PSW |= CC_Z;
			setcc(CC_V, vv);
			setcc(CC_C, c);
			return;
		}
		case 0074000: { // XOR
			opnd d = decode(op & 077, false);
			uint16_t res = getw(d) ^ R[r];
			setnz(res, false);
			PSW &= ~CC_V;
			putw(d, res);
			return;
		}
		case 0077000: // SOB
			R[r]--;
			if (R[r])
				PC -= 2 * (op & 077);
			return;
		}
		illegal();
	}

	// branches
	if ((op & 0074000) == 0 && (op & 0103400) != 0) {
		if (cond(op))
			PC += 2 * (int8_t) (op & 0xff);
		return;
	}

	// EMT / TRAP
	if ((op & 0177400) == 0104000) {
		trap(030);
		return;
	}
	if ((op & 0177400) == 0104400) {
		trap(034);
		return;
	}

	// single operand group
	uint16_t g = op & 0177700;
	switch (g) {
	case 0000100: { // JMP
		if (((op >> 3) & 7) == 0)
			illegal();
		opnd d = decode(op & 077, false);
		PC = d.addr;
		return;
	}
	case 0000300: { // SWAB
		opnd d = decode(op & 077, false);
		uint16_t v = getw(d);
		v = (v >> 8) | (v << 8);
		putw(d, v);
		setnz(v & 0xff, true);
		PSW &= ~(CC_V | CC_C);
		return;
	}
	case 0006400: { // MARK
		SP = PC + 2 * (op & 077);
		PC = R[5];
		R[5] = pop();
		return;
	}
	case 0006500: // MFPI (unmapped: same as fetch)
	case 0106500: { // MFPD
		opnd d = decode(op & 077, false);
		uint16_t v = d.reg ? (d.r == 6 ? SP : R[d.r]) : rw(d.addr);
		push(v);
		setnz(v, false);
		PSW &= ~CC_V;
		return;
	}
	case 0006600: // MTPI
	case 0106600: { // MTPD
		uint16_t v = pop();
		opnd d = decode(op & 077, false);
		putw(d, v);
		setnz(v, false);
		PSW &= ~CC_V;
		return;
	}
	case 0006700: { // SXT
		opnd d = decode(op & 077, false);
		putw(d, (PSW & CC_N) ? 0177777 : 0);
		setcc(CC_Z, !(PSW & CC_N));
		PSW &= ~CC_V;
		return;
	}
	case 0106400: { // MTPS
		opnd s = decode(op & 077, true);
		uint8_t v = getb(s);
		PSW = (PSW & 0xff10) | (v & 0357);
		return;
	}
	case 0106700: { // MFPS
		opnd d = decode(op & 077, true);
		uint8_t v = PSW & 0xff;
		if (d.reg)
			R[d.r] = (v & 0x80) ? 0xff00 | v : v;
		else
			putb(d, v);
		setnz(v, true);
		PSW &= ~CC_V;
		return;
	}
	}
	if ((op & 0177000) == 0004000) { // JSR
		if (((op >> 3) & 7) == 0)
			illegal();
		int r = (op >> 6) & 7;
		opnd d = decode(op & 077, false);
		push(R[r]);
		R[r] = PC;
		PC = d.addr;
		return;
	}
	if ((op & 0077000) == 0005000 || (op & 0077000) == 0006000) {
		// 0..7: CLR COM INC DEC NEG ADC SBC TST (050..057), 8..11: ROR ROL ASR ASL (060..063)
		int sub = (op >> 6) & 7;
		if ((op & 0077000) == 0006000)
			sub += 8;
		if (sub > 11)
			illegal();
		opnd d = decode(op & 077, byte);
		uint16_t m = byte ? 0x80 : 0x8000, mask = byte ? 0xff : 0xffff;
		uint16_t v = byte ? getb(d) : getw(d);
		uint16_t res = v;
		bool C = PSW & CC_C;
		switch (sub) {
		case 0: // CLR
			res = 0;
			PSW = (PSW & ~017) | CC_Z;
			break;
		case 1: // COM
			res = ~v & mask;
			setnz(res, byte);
			PSW = (PSW & ~CC_V) | CC_C;
			break;
		case 2: // INC
			res = (v + 1) & mask;
			setnz(res, byte);
			setcc(CC_V, res == m);
			break;
		case 3: // DEC
			res = (v - 1) & mask;
			setnz(res, byte);
			setcc(CC_V, res == (uint16_t) (m - 1));
			break;
		case 4: // NEG
			res = (-v) & mask;
			setnz(res, byte);
			setcc(CC_V, res == m);
			setcc(CC_C, res != 0);
			break;
		case 5: // ADC
			res = (v + (C ? 1 : 0)) & mask;
			setnz(res, byte);
			setcc(CC_V, C && res == m);
			setcc(CC_C, C && res == 0);
			break;
		case 6: // SBC
			res = (v - (C ? 1 : 0)) & mask;
			setnz(res, byte);
			setcc(CC_V, C && res == (uint16_t) (m - 1));
			setcc(CC_C, C && res == mask);
			break;
		case 7: // TST
			setnz(v, byte);
			PSW &= ~(CC_V | CC_C);
			return;
		case 8: // ROR
			res = ((v >> 1) | (C ? m : 0)) & mask;
			setcc(CC_C, v & 1);
			setnz(res, byte);
			setcc(CC_V, ((PSW & CC_N) != 0) != ((PSW & CC_C) != 0));
			break;
		case 9: // ROL
			res = ((v << 1) | (C ? 1 : 0)) & mask;
			setcc(CC_C, v & m);
			setnz(res, byte);
			setcc(CC_V, ((PSW & CC_N) != 0) != ((PSW & CC_C) != 0));
			break;
		case 10: // ASR
			res = ((v >> 1) | (v & m)) & mask;
			setcc(CC_C, v & 1);
			setnz(res, byte);
			setcc(CC_V, ((PSW & CC_N) != 0) != ((PSW & CC_C) != 0));
			break;
		case 11: // ASL
			res = (v << 1) & mask;
			setcc(CC_C, v & m);
			setnz(res, byte);
			setcc(CC_V, ((PSW & CC_N) != 0) != ((PSW & CC_C) != 0));
			break;
		}
		if (byte)
			putb(d, res);
		else
			putw(d, res);
		return;
	}

	// operate group
	switch (op) {
	case 0: // HALT
		halted = true;
		return;
	case 1: // WAIT
		waiting = true;
		return;
	case 2: // RTI
	case 6: // RTT
		PC = pop();
		PSW = pop();
		return;
	case 3:
		trap(014);
		return;
	case 4:
		trap(020);
		return;
	case 5: // RESET
		io_reset();
		return;
	case 7: // MFPT: F11
		R[0] = 3;
		return;
	}
	if ((op & 0177770) == 0000200) { // RTS
		int r = op & 7;
		PC = R[r];
		R[r] = pop();
		return;
	}
	if ((op & 0177770) == 0000230) { // SPL
		PSW = (PSW & ~0340) | ((op & 7) << 5);
		return;
	}
	if ((op & 0177740) == 0000240) { // CCC/SCC/NOP
		if (op & 020)
			PSW |= op & 017;
		else
			PSW &= ~(op & 017);
		return;
	}
	illegal(); // 17xxxx floating point, 075xxx FIS, ...
}

static void step(void) {
	// interrupts between instructions
	if (!irqs.empty()) {
		int pri = (PSW >> 5) & 7;
		int best = -1;
		for (size_t k = 0; k < irqs.size(); k++)
			if (irqs[k].level > pri && (best < 0 || irqs[k].level > irqs[best].level))
				best = (int) k;
		if (best >= 0) {
			uint16_t vec = irqs[best].vector;
			// DL11 transmitter: level triggered by "ready && IE", keep it
			irqs.erase(irqs.begin() + best);
			if (vec == 064 && (xcsr & 0100)) {
				// re-raised by the next XBUF write
			}
			waiting = false;
			trap(vec);
			return;
		}
	}
	if (waiting)
		return;
	uint16_t pc0 = PC;
	if (trace_lo <= PC && PC < trace_hi)
		fprintf(stderr, "%06o: %06o  R0=%06o R1=%06o R2=%06o R3=%06o R4=%06o R5=%06o SP=%06o PSW=%03o\n",
				PC, rw(PC), R[0], R[1], R[2], R[3], R[4], R[5], SP, PSW & 0377);
	try {
		uint16_t op = fetch();
		icount++;
		exec(op);
	} catch (bus_error&) {
		if (trap_vector == 010)
			note("illegal instruction at %06o", pc0);
		trap(trap_vector);
	}
}

// ---------------------------------------------------------------- loader

// absolute loader (papertape) format
static int load_lda(const char *fn, uint16_t *start) {
	FILE *f = fopen(fn, "rb");
	if (!f)
		return -1;
	std::vector<uint8_t> d;
	int c;
	while ((c = fgetc(f)) != EOF)
		d.push_back((uint8_t) c);
	fclose(f);
	size_t i = 0;
	*start = 1;
	while (i < d.size()) {
		while (i < d.size() && d[i] == 0)
			i++;
		if (i + 6 > d.size())
			break;
		if (d[i] != 1 || d[i + 1] != 0) {
			fprintf(stderr, "%s: bad block header at %zu\n", fn, i);
			return -1;
		}
		unsigned count = d[i + 2] | (d[i + 3] << 8);
		unsigned addr = d[i + 4] | (d[i + 5] << 8);
		if (count < 6 || i + count + 1 > d.size()) {
			fprintf(stderr, "%s: bad block length at %zu\n", fn, i);
			return -1;
		}
		uint8_t sum = 0;
		for (unsigned k = 0; k <= count; k++)
			sum += d[i + k];
		if (sum != 0) {
			fprintf(stderr, "%s: checksum error in block at %zu\n", fn, i);
			return -1;
		}
		if (count == 6) {
			*start = addr;
			return 0;
		}
		for (unsigned k = 0; k < count - 6; k++)
			mem[(addr + k) & 0xffff] = d[i + 6 + k];
		i += count + 1;
	}
	return 0;
}

// ---------------------------------------------------------------- main

static struct termios tio_saved;
static bool tio_set = false;
static void tio_restore(void) {
	if (tio_set)
		tcsetattr(0, TCSANOW, &tio_saved);
}
static volatile sig_atomic_t got_signal = 0;
static void on_signal(int) {
	got_signal = 1;
}

int main(int argc, char **argv) {
	double run_s = 0;
	bool tcp_console = false, restricted = false, no_net = false;
	std::vector<std::pair<int, int>> fwd;
	int opt;
	while ((opt = getopt(argc, argv, "t:f:qsrnkT:L:DF:S:")) != -1) {
		switch (opt) {
		case 't':
			run_s = atof(optarg);
			break;
		case 'f': {
			int h, g;
			if (sscanf(optarg, "%d:%d", &h, &g) == 2)
				fwd.push_back( { h, g });
			break;
		}
		case 'q':
			quiet = true;
			break;
		case 's':
			tcp_console = true;
			break;
		case 'r':
			restricted = true;
			break;
		case 'n':
			no_net = true;
			break;
		case 'k':
			kw11_present = false;
			break;
		case 'S':
			speed_ips = atof(optarg);
			break;
		case 'F':
			drop_fin_n = atoi(optarg);
			break;
		case 'D':
			dump_frames = true;
			break;
		case 'L':
			loss_pct = atoi(optarg);
			srand(1123);
			break;
		case 'T':
			sscanf(optarg, "%o:%o", &trace_lo, &trace_hi);
			break;
		default:
			fprintf(stderr, "usage: emu11 [-t s] [-f host:guest]... [-q] [-s] [-r] [-n] [-k] [-L pct] [-D] [-T lo:hi] prog.lda\n"
					"  -t run for s seconds   -f forward a host TCP port to the PDP-11 (default 8080:80 2323:23)\n"
					"  -s console on TCP port 11123   -r no internet (restricted)   -n no DELQA\n"
					"  -k no KW11 line clock   -L drop pct %% of the frames   -D log frames\n"
					"  -T lo:hi trace instructions with PC in [lo,hi) (octal)\n"
					"  -S ips   run at most ips instructions per second (11/23: about 300000)\n"
					"  -F n     drop the n-th TCP FIN sent by the PDP-11\n");
			return 2;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "no program\n");
		return 2;
	}
	if (fwd.empty()) {
		fwd.push_back( { 8080, 80 });
		fwd.push_back( { 2323, 23 });
	}
	uint16_t start;
	if (load_lda(argv[optind], &start) < 0) {
		fprintf(stderr, "cannot load %s\n", argv[optind]);
		return 1;
	}
	if (start & 1) {
		fprintf(stderr, "%s has no start address\n", argv[optind]);
		return 1;
	}

	if (!no_net) {
		qe = new delqa_core_c(&qe_if);
		static const uint8_t mac[6] = { 0x08, 0x00, 0x2b, 0x11, 0x23, 0x01 };
		qe->set_mac(mac);
		qe->set_link(true);
		qe->bus_reset();
		net_init(fwd, restricted);
	}

	if (tcp_console) {
		con_listen = socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		setsockopt(con_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		struct sockaddr_in sa;
		memset(&sa, 0, sizeof sa);
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sa.sin_port = htons(11123);
		if (bind(con_listen, (struct sockaddr*) &sa, sizeof sa) < 0 || listen(con_listen, 1) < 0) {
			perror("console port");
			return 1;
		}
		fcntl(con_listen, F_SETFL, O_NONBLOCK);
		con_in = con_out = -1;
		note("console: telnet 127.0.0.1 11123");
	} else {
		fcntl(0, F_SETFL, O_NONBLOCK);
		if (isatty(0)) {
			tcgetattr(0, &tio_saved);
			struct termios t = tio_saved;
			cfmakeraw(&t);
			t.c_oflag |= OPOST | ONLCR;
			tcsetattr(0, TCSANOW, &t);
			tio_set = true;
			atexit(tio_restore);
			note("console on this terminal, ctrl-E quits");
		}
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	PC = start;
	PSW = 0340;
	SP = 0;
	next_tick_ns = now_ns();
	int64_t t_end = run_s > 0 ? now_ns() + (int64_t) (run_s * 1e9) : 0;
	int64_t t_start = now_ns();
	unsigned long long steps = 0; // instructions and idle WAIT steps, for -S
	note("start at %06o", start);

	while (!halted && !got_signal) {
		for (int k = 0; k < 500 && !halted && !waiting; k++) {
			step();
			steps++;
		}
		if (speed_ips > 0) {
			// do not run faster than a real PDP-11
			int64_t due = t_start + (int64_t) (steps / speed_ips * 1e9);
			int64_t t = now_ns();
			if (due > t + 100000) {
				struct timespec ts = { 0, (long) (due - t) };
				nanosleep(&ts, NULL);
			}
		}
		if (waiting)
			step(); // take a pending interrupt
		con_poll();
		if (!con_inq.empty() && con_inq[0] == 5 && !tcp_console)
			break; // ctrl-E
		clock_poll();
		if (qe) {
			net_poll();
			if (qe->wait_for_work(0))
				qe->service();
		}
		if (waiting && irqs.empty()) {
			struct timespec ts = { 0, 200000 };
			nanosleep(&ts, NULL);
		}
		if (t_end && now_ns() > t_end)
			break;
	}
	if (halted)
		note("HALT at %06o, R0=%06o R1=%06o SP=%06o PSW=%06o, %llu instructions", (uint16_t) (PC),
				R[0], R[1], SP, PSW, icount);
	else
		note("stopped at %06o after %llu instructions", PC, icount);
	if (loss_pct)
		note("%lu frames lost on purpose (-L %d)", lost_frames, loss_pct);
	if (qe)
		note("DELQA: tx %lu rx %lu dropped %lu filtered %lu, %lu DMA words", qe->stat_tx_frames,
				qe->stat_rx_frames, qe->stat_rx_dropped, qe->stat_rx_filtered, qe_dma_words);
	return halted ? 3 : 0;
}
