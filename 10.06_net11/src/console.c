/* console.c - net11: DL11 console at 177560, polled */
#include "hw.h"

static void con_put(sink_t *s, char c)
{
	(void)s;
	while (!(DL_XCSR & 0200))
		;
	DL_XBUF = (u8)c;
}

sink_t con = { con_put, 0, 0 };

void con_init(void)
{
	DL_RCSR = 0;            /* no interrupts */
	DL_XCSR = 0;
}

int con_getc(void)
{
	if (!(DL_RCSR & 0200))
		return -1;
	return DL_RBUF & 0177;
}
