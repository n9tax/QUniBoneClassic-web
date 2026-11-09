/* delqa_core.hpp: DEC DELQA (M7516) / DEQNA (M7504) QBUS Ethernet controller,
   device logic without QUniBone dependencies.

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
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 The controller logic is kept apart from the QUniBone bus adapter (delqa.cpp),
 so it can be tested on a PC against simulated PDP-11 memory
 (10.02_devices/3_test/delqa).

 Register layout (offsets from base, default 17774440, vector programmed via VAR):
   +00, +02   station address bytes 0,1 (read; checksum when CSR<EL> set)
   +04..+12   station address bytes 2..5 (read)
   +04 / +06  receive buffer descriptor list address low / high (write; high starts)
   +10 / +12  transmit buffer descriptor list address low / high (write; high starts)
   +14        vector address register VAR
   +16        control and status register CSR

 Buffer descriptor (6 words, host memory):
   0  flag word: host writes 100000, controller writes 177777 when it uses it
   1  address descriptor bits: V C E S ... L H, buffer address <21:16> in bits 5:0
   2  buffer address <15:0>
   3  buffer length, two's complement word count
   4  status word 1, host writes 100000 ("not yet used")
   5  status word 2
 */

#ifndef _DELQA_CORE_HPP_
#define _DELQA_CORE_HPP_

#include <stdint.h>
#include <stddef.h>
#include <deque>
#include <vector>
#include <mutex>
#include <condition_variable>

// CSR bits
#define DELQA_CSR_RE	0x0001	// receiver enable
#define DELQA_CSR_SR	0x0002	// software reset (on 1->0)
#define DELQA_CSR_NI	0x0004	// nonexistent memory timeout
#define DELQA_CSR_BD	0x0008	// boot/diagnostic ROM load
#define DELQA_CSR_XL	0x0010	// transmit list invalid
#define DELQA_CSR_RL	0x0020	// receive list invalid
#define DELQA_CSR_IE	0x0040	// interrupt enable
#define DELQA_CSR_XI	0x0080	// transmit interrupt request (write 1 to clear)
#define DELQA_CSR_IL	0x0100	// internal loopback DISABLE (0 = internal loopback)
#define DELQA_CSR_EL	0x0200	// external loopback
#define DELQA_CSR_SE	0x0400	// sanity timer enable
#define DELQA_CSR_RR	0x0800	// reserved
#define DELQA_CSR_OK	0x1000	// transceiver power OK
#define DELQA_CSR_CA	0x2000	// carrier
#define DELQA_CSR_PE	0x4000	// parity error
#define DELQA_CSR_RI	0x8000	// receive interrupt request (write 1 to clear)
#define DELQA_CSR_RW	0x074B	// bits the host can write
#define DELQA_CSR_W1C	0x8080	// write one to clear

// VAR bits (DELQA; a DEQNA only has the vector bits)
#define DELQA_VAR_MS	0x8000	// mode select: 1 = DELQA normal mode, 0 = DEQNA-lock mode
#define DELQA_VAR_OS	0x4000	// option switch (read only)
#define DELQA_VAR_RS	0x2000	// request self test
#define DELQA_VAR_ST	0x1C00	// self test status S3..S1 (000 = passed)
#define DELQA_VAR_IV	0x03FC	// interrupt vector
#define DELQA_VAR_ID	0x0001	// identity test bit
#define DELQA_VAR_RW	(DELQA_VAR_MS | DELQA_VAR_RS | DELQA_VAR_IV | DELQA_VAR_ID)

// address descriptor bits (descriptor word 1)
#define DELQA_DSC_V	0x8000	// valid
#define DELQA_DSC_C	0x4000	// chain: address points to next descriptor
#define DELQA_DSC_E	0x2000	// end of message (transmit)
#define DELQA_DSC_S	0x1000	// setup packet (transmit)
#define DELQA_DSC_L	0x0080	// buffer ends on low byte (transmit)
#define DELQA_DSC_H	0x0040	// buffer starts on high byte (transmit)
#define DELQA_DSC_HIADDR 0x003F	// buffer address <21:16>

// status word 1
#define DELQA_SW1_NOTYET	0x8000	// written by host: descriptor not used yet
#define DELQA_SW1_LASTNOT	0xC000	// used, not the last descriptor of the frame
#define DELQA_SW1_ERROR	0x4000	// last descriptor, error summary
#define DELQA_RSW1_ESETUP	0x2000	// receive: setup or loopback frame
#define DELQA_RSW1_DISCARD	0x1000
#define DELQA_RSW1_RUNT	0x0800
#define DELQA_RSW1_RBLHI	0x0700	// receive: received byte length - 60, bits 10:8
#define DELQA_TSW1_LOSS	0x1000	// transmit: loss of carrier
#define DELQA_TSW1_NOCAR	0x0800	// transmit: no carrier

#define DELQA_FRAME_MIN	60	// without CRC
#define DELQA_FRAME_MAX	1514	// without CRC
#define DELQA_SETUP_ADDRS	14

// Services the controller needs from its surroundings: PDP-11 memory (DMA),
// QBUS register visibility and interrupts, the Ethernet, logging.
class delqa_bus_if {
public:
	virtual ~delqa_bus_if() {
	}
	// DMA: false on bus timeout (nonexistent memory)
	virtual bool dma_read(uint32_t addr, uint16_t *buffer, unsigned wordcount) = 0;
	virtual bool dma_write(uint32_t addr, uint16_t *buffer, unsigned wordcount) = 0;
	// New values the PDP-11 reads from the 8 registers (DATI).
	// intr_raise: interrupt condition became active, request an interrupt
	// with "vector" (set the CSR atomically with the interrupt).
	// intr_cancel: condition went away, withdraw a pending request.
	virtual void publish(const uint16_t *dati, uint16_t vector, bool intr_raise,
			bool intr_cancel) = 0;
	// send a frame (without CRC) to the wire. false: not sent
	virtual bool transmit(const uint8_t *frame, unsigned len) = 0;
	// true while bus INIT is asserted: no DMA
	virtual bool in_init(void) {
		return false;
	}
	// level: 1 error, 2 warning, 3 info, 4 debug
	virtual void log(int level, const char *msg) = 0;
};

class delqa_core_c {
public:
	enum frame_type_e {
		FRAME_NORMAL = 0, FRAME_LOOPBACK, FRAME_SETUP
	};
	struct frame_t {
		std::vector<uint8_t> data;
		frame_type_e type;
	};
	// register write access, same order as QUniBone DATO_ACCESS
	enum access_e {
		ACCESS_WORD = 0, ACCESS_HIGH_BYTE, ACCESS_LOW_BYTE
	};

	static const unsigned RXQ_MAX = 64; // frames held while the host has no buffers

	delqa_core_c(delqa_bus_if *bus);

	void set_mac(const uint8_t *mac);
	void get_mac(uint8_t *mac);
	void set_link(bool ok); // transceiver OK (network interface open)

	// power up and QBUS INIT: like software reset, also clears VAR
	void bus_reset(void);
	// PDP-11 wrote a register. "value" is the full 16 bit register content after
	// the write (for byte access: the other byte as last written).
	void bus_write(unsigned index, uint16_t value, access_e access);
	// a frame from the wire. Returns true if accepted by the address filter.
	bool receive_frame(const uint8_t *frame, unsigned len);

	// worker thread: wait for something to do, then do it (DMA without locks held)
	bool wait_for_work(unsigned timeout_ms);
	void service(void);

	// current register values as seen by the PDP-11
	uint16_t dati(unsigned index);
	uint16_t get_csr(void);
	uint16_t get_vector(void);
	bool get_promiscuous(void);

	// statistics
	volatile unsigned long stat_tx_frames;
	volatile unsigned long stat_rx_frames;
	volatile unsigned long stat_rx_dropped; // queue full, oversize
	volatile unsigned long stat_rx_filtered; // not for us, or receiver off

private:
	delqa_bus_if *bus;
	std::mutex mtx;
	std::condition_variable cv;

	uint8_t mac[6];
	bool link_ok;

	uint16_t csr;
	uint16_t var;
	uint16_t rbdl_lo, rbdl_hi, tbdl_lo, tbdl_hi;
	uint32_t rbdl_ba, tbdl_ba; // next descriptor to use
	unsigned rx_gen, tx_gen; // change when the host reloads a list, or on reset
	bool tx_kick;
	bool intr_level; // interrupt condition as last published

	// address filter, from the last setup packet
	bool setup_valid;
	bool promiscuous;
	bool all_multicast;
	uint8_t filter[DELQA_SETUP_ADDRS][6];

	std::deque<frame_t> rxq;
	size_t rx_used; // bytes of rxq.front() already placed in host buffers

	bool warned_bootrom;
	unsigned warned_oversize;

	void logf(int level, const char *fmt, ...);
	void software_reset_locked(void);
	void write_csr_locked(uint16_t value, access_e access);
	void write_var_locked(uint16_t value);
	void compute_regs_locked(uint16_t *regs);
	void publish_locked(void);
	bool rx_pending_locked(void);
	void enqueue_locked(const std::vector<uint8_t> &data, frame_type_e type);
	void process_setup_locked(const std::vector<uint8_t> &pkt);
	bool address_match_locked(const uint8_t *dst);
	void nxm(bool transmit, unsigned gen);

	bool process_tx(void);
	bool process_rx(void);
};

#endif
