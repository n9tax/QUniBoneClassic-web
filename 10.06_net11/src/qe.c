/* qe.c - net11: DEQNA / DELQA Ethernet driver, polled.

   The controller works through lists of 6 word buffer descriptors in PDP-11
   memory (DMA). net11 uses it in DEQNA-lock mode, as all DEC drivers do:

   receive:  NRX descriptors, each with a buffer for a whole frame, then an
             invalid descriptor that ends the list. When the controller has
             filled all of them it stops at the end (CSR<RL>, "receive list
             invalid"); net11 then hands it the list again, re-armed.
   transmit: one descriptor plus an end marker per frame; net11 waits until
             the controller has sent it (status word 1 written).

   No interrupts: the main loop polls. Addresses below 160000 are physical
   addresses too (memory management off), so buffers can be given to the
   controller as they are.
*/
#include "net.h"

#define QE_BASE 0174440
#define QE_REG(n) REG(QE_BASE + 2 * (n))
#define QE_RBDL_LO QE_REG(2)
#define QE_RBDL_HI QE_REG(3)
#define QE_TBDL_LO QE_REG(4)
#define QE_TBDL_HI QE_REG(5)
#define QE_VAR QE_REG(6)
#define QE_CSR QE_REG(7)

#define CSR_RE 0000001
#define CSR_SR 0000002
#define CSR_NI 0000004
#define CSR_XL 0000020
#define CSR_RL 0000040
#define CSR_XI 0000200
#define CSR_IL 0000400
#define CSR_OK 0010000
#define CSR_RI 0100000

#define DSC_V 0100000
#define DSC_C 0040000
#define DSC_E 0020000
#define DSC_S 0010000
#define DSC_L 0000200

#define SW1_NOTYET 0100000
#define SW1_LASTNOT 0140000
#define SW1_ERROR 0040000
#define SW1_ESETUP 0020000

struct desc {
	u16 flag, addr_hi, addr_lo, wc, sw1, sw2;
};

#define NRX 6
#define BUFW 760                /* 1520 bytes */

static volatile struct desc rxd[NRX + 1];
static u16 rxbuf[NRX][BUFW];
static volatile struct desc txd[2];
static u16 txbuf[BUFW];
static u8 rx_next;
static u8 need_reload;

u8 my_mac[6];
int qe_present;
u16 qe_csr_addr = QE_BASE + 016;
u32 qe_rx_frames, qe_tx_frames, qe_tx_errors, qe_rx_errors, qe_reloads;

static void rx_arm(void)
{
	int i;
	for (i = 0; i < NRX; i++) {
		rxd[i].flag = SW1_NOTYET;
		rxd[i].addr_hi = DSC_V;
		rxd[i].addr_lo = (u16)rxbuf[i];
		rxd[i].wc = -BUFW;
		rxd[i].sw1 = SW1_NOTYET;
		rxd[i].sw2 = 0;
	}
	rxd[NRX].flag = SW1_NOTYET;
	rxd[NRX].addr_hi = 0;           /* end of list */
	rxd[NRX].sw1 = SW1_NOTYET;
	rx_next = 0;
}

static void rx_load(void)
{
	QE_RBDL_LO = (u16)rxd;
	QE_RBDL_HI = 0;
	qe_reloads++;
}

/* transmit one buffer; setup: a setup frame. 0 on error or timeout */
static int tx_buffer(u16 len, int setup)
{
	u16 t0;
	u16 words = (len + 1) >> 1;
	txd[0].flag = SW1_NOTYET;
	txd[0].addr_hi = DSC_V | DSC_E | (setup ? DSC_S : 0) | ((len & 1) ? DSC_L : 0);
	txd[0].addr_lo = (u16)txbuf;
	txd[0].wc = -words;
	txd[0].sw1 = SW1_NOTYET;
	txd[0].sw2 = 0;
	txd[1].flag = SW1_NOTYET;
	txd[1].addr_hi = 0;
	txd[1].sw1 = SW1_NOTYET;
	QE_TBDL_LO = (u16)txd;
	QE_TBDL_HI = 0;
	t0 = now();
	while (txd[0].sw1 == SW1_NOTYET) {
		if (QE_CSR & CSR_NI) {
			cprint("qe: nonexistent memory on transmit\n");
			return 0;
		}
		if (elapsed(t0, hz))    /* 1 second */
			return 0;
		if (!have_clock)
			clock_poll();
	}
	if ((txd[0].sw1 & SW1_LASTNOT) == SW1_ERROR)
		return 0;
	return 1;
}

static void setup_frame(void)
{
	u8 *p = (u8 *)txbuf;
	int n, j;
	static const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	memset(p, 0, 128);
	for (n = 0; n < 14; n++) {
		const u8 *a = n == 0 ? bcast : my_mac;
		int base = n < 7 ? n + 1 : 64 + n - 7 + 1;
		for (j = 0; j < 6; j++)
			p[base + j * 8] = a[j];
	}
	tx_buffer(128, 1);
}

int qe_init(void)
{
	int i;
	u16 t0;
	qe_present = probe((volatile u16 *)(QE_BASE + 016));
	if (!qe_present)
		return 0;
	QE_CSR = CSR_SR;                /* software reset on 1 -> 0 */
	QE_CSR = 0;
	t0 = now();
	while (!elapsed(t0, 2) && have_clock)
		;
	QE_VAR = 0;                     /* DEQNA-lock mode, vector 0 */
	for (i = 0; i < 6; i++)
		my_mac[i] = QE_REG(i) & 0xff;
	QE_CSR = CSR_IL;                /* normal operation, receiver off */
	rx_arm();
	rx_load();
	QE_CSR = CSR_IL | CSR_RE;
	setup_frame();
	return 1;
}

int qe_send(const u8 *frame, u16 len)
{
	if (!qe_present)
		return 0;
	if (len > ETH_MAX)
		return 0;
	memcpy(txbuf, frame, len);
	if (len < 60) {
		memset((u8 *)txbuf + len, 0, 60 - len);
		len = 60;
	}
	if (!tx_buffer(len, 0)) {
		qe_tx_errors++;
		return 0;
	}
	qe_tx_frames++;
	return 1;
}

void qe_poll(void)
{
	int n;
	if (!qe_present)
		return;
	for (n = 0; n < NRX && rx_next < NRX; n++) {
		volatile struct desc *d = &rxd[rx_next];
		u16 sw1 = d->sw1;
		if (sw1 == SW1_NOTYET)
			break;
		if ((sw1 & SW1_LASTNOT) == SW1_LASTNOT) {
			/* frame continues in the next buffer: cannot happen with
			   full size buffers; drop the pieces */
			qe_rx_errors++;
		} else if (sw1 & SW1_ERROR) {
			qe_rx_errors++;
		} else if (!(sw1 & SW1_ESETUP)) {
			u16 len = ((sw1 & 03400) | (d->sw2 & 0377)) + 60;
			if (len > ETH_MAX)
				len = ETH_MAX;
			qe_rx_frames++;
			eth_input((u8 *)rxbuf[rx_next], len);
		}
		rx_next++;
	}
	if (rx_next >= NRX) {
		rx_arm();
		need_reload = 1;
	}
	/* the controller stops at the end of the list: give it the list again */
	if (need_reload && (QE_CSR & CSR_RL)) {
		need_reload = 0;
		rx_load();
	}
}
