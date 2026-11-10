/* clock.c - net11: time keeping with the KW11-L line clock (interrupt at 100).

   Without a line clock (no register at 177546), ticks are counted by the
   main loop instead, calibrated very roughly: timers then run at an
   unknown speed, but the network still works. */
#include "hw.h"

volatile u16 ticks;
u16 hz = 60;
int have_clock;
u32 uptime_s;

static u16 last_second;
static u16 loops;

u16 now(void)
{
	return ticks;
}

int elapsed(u16 since, u16 n)
{
	return (u16)(ticks - since) >= n;
}

void clock_init(void)
{
	*(volatile u16 *)0100 = (u16)clk_isr;
	*(volatile u16 *)0102 = 0340;
	have_clock = probe((volatile u16 *)LKS_ADDR);
	if (have_clock)
		LKS = 0100;     /* interrupt enable */
	last_second = ticks;
}

void clock_poll(void)
{
	if (!have_clock) {
		/* about 1 ms per main loop pass on an 11/23, so a tick every 16 */
		if (++loops >= 16) {
			loops = 0;
			ticks++;
		}
	}
	while ((u16)(ticks - last_second) >= hz) {
		last_second += hz;
		uptime_s++;
	}
}
