/* telnet.c - net11: telnet server on port 23, running the monitor */
#include "net.h"

#define NSESS 3

static struct sess {
	tcb_t *t;
	sink_t out;
	mon_t mon;
	u8 iac, opt, cr;
} sess[NSESS];

void mon_forget(sink_t *s);

static void accept_(tcb_t *t)
{
	static const u8 neg[] = { 255, 251, 1, 255, 251, 3, 255, 253, 3 };  /* WILL ECHO, WILL SGA, DO SGA */
	int i;
	struct sess *x = NULL;
	for (i = 0; i < NSESS; i++)
		if (!sess[i].t) {
			x = &sess[i];
			break;
		}
	if (!x) {
		tcp_write(t, (const u8 *)"too many sessions\r\n", 19);
		tcp_close(t);
		t->appstate = NULL;
		return;
	}
	memset(x, 0, sizeof *x);
	x->t = t;
	t->appstate = x;
	tcp_sink(&x->out, t);
	mon_init(&x->mon, &x->out, 0);
	tcp_write(t, neg, sizeof neg);
	banner(&x->out);
	sprint(&x->out, "Connected to %s (%I). Type help.\n", my_hostname, my_ip);
	mon_prompt(&x->mon);
}

static void recv_(tcb_t *t, u8 *d, u16 len)
{
	struct sess *x = t->appstate;
	u16 i;
	if (!x)
		return;
	for (i = 0; i < len; i++) {
		u8 c = d[i];
		if (x->iac == 1) {              /* after IAC */
			if (c == 255) {
				x->iac = 0;     /* escaped 255: ignore */
			} else if (c >= 251 && c <= 254)
				x->iac = 2;     /* WILL WONT DO DONT: option follows */
			else if (c == 250)
				x->iac = 3;     /* subnegotiation until IAC SE */
			else
				x->iac = 0;
			continue;
		}
		if (x->iac == 2) {
			x->iac = 0;
			continue;
		}
		if (x->iac == 3) {
			if (c == 255)
				x->iac = 4;
			continue;
		}
		if (x->iac == 4) {
			x->iac = c == 240 ? 0 : 3;
			continue;
		}
		if (c == 255) {
			x->iac = 1;
			continue;
		}
		if (x->cr && (c == '\n' || c == 0)) {   /* CR LF or CR NUL: one Return */
			x->cr = 0;
			continue;
		}
		x->cr = c == '\r';
		mon_input(&x->mon, c);
		if (x->mon.quit) {
			sputs(&x->out, "bye\n");
			tcp_close(t);
			return;
		}
	}
}

static void closed_(tcb_t *t)
{
	struct sess *x = t->appstate;
	if (!x)
		return;
	mon_forget(&x->out);
	x->out.ctx = NULL;
	x->t = NULL;
	t->appstate = NULL;
}

static const tcp_app_t telnet_app = { 23, accept_, recv_, NULL, closed_ };

void telnet_init(void)
{
	tcp_listen(&telnet_app);
}
