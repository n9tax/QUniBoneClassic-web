/* main.c - net11: start-up and the main loop.

   net11 is a stand-alone program: load it (QBone/UniBone: "m lp net11.lda"),
   start it at 1000 from ODT, and it brings up the Ethernet controller, gets
   an address with DHCP, and serves telnet (port 23) and HTTP (port 80).
   The console runs the same command monitor as telnet. */
#include "net.h"

static mon_t conmon;

/* unexpected traps and interrupts (crt0.S trap_stubs) */
void fault(u16 vec, u16 pc, u16 psw)
{
	static u16 count;
	if (vec == 0 || vec == 024) {
		/* stray interrupt or power fail: report a few, carry on */
		if (count++ < 5)
			cprint("\n[interrupt through an unused vector, PC %o]\n", pc);
		return;
	}
	cprint("\n*** trap through vector %o at PC %o, PSW %o: net11 stops\n", vec, pc, psw);
	halt_cpu();
}

static void vectors(void)
{
	u16 v;
	volatile u16 *p;
	/* each stub: mov $vec,-(sp) (2 words) + br (1 word) = 6 bytes */
	static const u16 known[] = { 04, 010, 014, 020, 024, 030, 034, 0244, 0250 };
	u16 other = (u16)trap_stubs + 9 * 6;
	int i;
	for (v = 4; v < 01000; v += 4) {
		p = (volatile u16 *)v;
		p[0] = other;
		p[1] = 0340;
	}
	for (i = 0; i < 9; i++) {
		p = (volatile u16 *)known[i];
		p[0] = (u16)trap_stubs + i * 6;
	}
}

int main(void)
{
	u16 last_s;
	vectors();
	con_init();
	clock_init();
	spl0();
	banner(&con);
	if (!have_clock)
		cprint("No KW11-L line clock at 177546: time runs approximately.\n"
		       "(QBone: switch on the KW11 in the panel, or \"en kw11\" in demo.)\n");
	if (!qe_init()) {
		cprint("No DEQNA/DELQA at 174440. Switch on the DELQA in the QBone panel (QBUS),\n"
		       "then start again at 1000. The monitor works without network.\n");
	} else
		cprint("DELQA/DEQNA at 174440, Ethernet address %M\n", my_mac);
	http_init();
	telnet_init();
	sntp_start();
	if (qe_present) {
		dhcp_start();
		cprint("Asking for an address with DHCP ...\n");
	}
	cprint("Type help for commands.\n");
	mon_init(&conmon, &con, 1);
	mon_prompt(&conmon);

	last_s = (u16)uptime_s;
	for (;;) {
		int c = con_getc();
		if (c >= 0)
			mon_input(&conmon, c);
		qe_poll();
		ping_poll();
		tcp_poll();
		clock_poll();
		if ((u16)uptime_s != last_s) {
			last_s = (u16)uptime_s;
			net_timer();
			tcp_timer();
			if (qe_present) {
				dhcp_timer();
				dns_timer();
				sntp_timer();
			}
		}
	}
}
