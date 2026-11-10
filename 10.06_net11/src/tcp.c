/* tcp.c - net11: a small TCP, server side only.

   - a few connections, each with a send buffer that holds the data until
     the peer acknowledges it; retransmission goes back to the oldest
     unacknowledged byte, with an exponential backoff
   - receive in order only: a segment that does not start at rcv_nxt is
     dropped and answered with an ACK (the peer resends)
   - data is handed to the application at once, so the window is constant
   - sequence numbers are compared with SEQ_LT, never with signed 32 bit
     compares (see lib32.S)
*/
#include "net.h"

#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10

#define RCV_WND 2048
#define OUR_MSS 1460
#define MAX_LISTEN 4

tcb_t tcbs[TCP_MAXCONN];
int tcp_debug;          /* monitor "debug": log TCP events on the console */

#define DBG(t, what) do { if (tcp_debug) cprint("[tcp %u<-%u %s: %s]\n", (t)->lport, (t)->rport, \
	tcp_state_names[(t)->state], what); } while (0)
static const tcp_app_t *listeners[MAX_LISTEN];
u32 tcp_conns, tcp_retrans;
static u16 iss_counter;

const char *tcp_state_names[] = { "CLOSED", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT1",
	"FIN_WAIT2", "CLOSING", "TIME_WAIT", "CLOSE_WAIT", "LAST_ACK" };

void tcp_listen(const tcp_app_t *app)
{
	int i;
	for (i = 0; i < MAX_LISTEN; i++)
		if (!listeners[i]) {
			listeners[i] = app;
			return;
		}
}

/* send a segment of connection t; 0 if not sent (no ARP entry yet) */
static int send_seg(tcb_t *t, u8 flags, u32 seq, const u8 *data, u16 len, int mss_opt)
{
	u8 *h = ip_payload();
	u16 hl = mss_opt ? 24 : 20;
	put16(h, t->lport);
	put16(h + 2, t->rport);
	put32(h + 4, seq);
	put32(h + 8, (flags & ACK) ? t->rcv_nxt : 0);
	h[12] = (hl / 4) << 4;
	h[13] = flags;
	put16(h + 14, RCV_WND);
	put16(h + 16, 0);
	put16(h + 18, 0);
	if (mss_opt) {
		h[20] = 2;
		h[21] = 4;
		put16(h + 22, OUR_MSS);
	}
	if (len)
		memcpy(h + hl, data, len);
	put16(h + 16, ~ip_cksum(ip_pseudo(my_ip, t->rip, IP_TCP, hl + len), h, hl + len));
	return ip_send(t->rip, IP_TCP, hl + len) > 0;
}

/* reset for a segment that belongs to no connection */
static void send_reset(const u8 *src, u16 sport, u16 dport, u32 seq, u32 ack, int with_ack)
{
	tcb_t tmp;
	memcpy(tmp.rip, src, 4);
	tmp.rport = sport;
	tmp.lport = dport;
	tmp.rcv_nxt = ack;
	send_seg(&tmp, RST | (with_ack ? ACK : 0), seq, NULL, 0, 0);
}

static void set_closed(tcb_t *t)
{
	const tcp_app_t *app = t->app;
	if (t->state == TCP_CLOSED)
		return;
	DBG(t, "closed");
	t->state = TCP_CLOSED;
	if (app && app->closed && !t->close_after)
		app->closed(t);
	t->close_after = 1;     /* app told */
}

/* the connection is finished for the application (TIME_WAIT etc.) */
static void app_done(tcb_t *t)
{
	if (t->app && t->app->closed && !t->close_after) {
		t->close_after = 1;
		t->app->closed(t);
	}
}

void tcp_abort(tcb_t *t)
{
	if (t->state == TCP_CLOSED)
		return;
	if (t->state != TCP_TIME_WAIT)
		send_seg(t, RST | ACK, t->snd_nxt, NULL, 0, 0);
	set_closed(t);
}

u16 tcp_space(tcb_t *t)
{
	if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT)
		return 0;
	if (t->fin_pending)
		return 0;
	return TCP_TXBUF - t->txlen;
}

u16 tcp_write(tcb_t *t, const u8 *data, u16 len)
{
	u16 space = tcp_space(t);
	if (len > space)
		len = space;
	memcpy(t->tx + t->txlen, data, len);
	t->txlen += len;
	return len;
}

void tcp_close(tcb_t *t)
{
	if (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT)
		t->fin_pending = 1;
	else if (t->state == TCP_SYN_RCVD)
		tcp_abort(t);
}

static int fin_sent(tcb_t *t)
{
	return t->state == TCP_FIN_WAIT1 || t->state == TCP_CLOSING || t->state == TCP_LAST_ACK;
}

static void output(tcb_t *t)
{
	int segs = 0;
	u16 off, n, avail, wnd;
	if (t->state == TCP_CLOSED || t->state == TCP_SYN_RCVD)
		return;
	if (t->state == TCP_TIME_WAIT) {
		/* only acknowledge the peer's FIN (again, if it repeats it) */
		if (t->ack_now)
			send_seg(t, ACK, t->snd_nxt, NULL, 0, 0);
		t->ack_now = 0;
		return;
	}
	/* data */
	while (segs < 4) {
		off = (u16)(t->snd_nxt - t->snd_una);
		if (off >= t->txlen)
			break;
		avail = t->txlen - off;
		wnd = t->snd_wnd > off ? t->snd_wnd - off : 0;
		n = avail;
		if (n > t->mss)
			n = t->mss;
		if (n > wnd)
			n = wnd;
		if (!n)
			break;
		if (!send_seg(t, ACK | PSH, t->snd_nxt, t->tx + off, n, 0))
			return;
		if (t->snd_nxt == t->snd_una)
			t->rtimer = now();
		t->snd_nxt += n;
		t->ack_now = 0;
		segs++;
	}
	/* FIN when all data is out */
	if (t->fin_pending && (u16)(t->snd_nxt - t->snd_una) == t->txlen) {
		if (!send_seg(t, FIN | ACK, t->snd_nxt, NULL, 0, 0))
			return;
		if (t->snd_nxt == t->snd_una)
			t->rtimer = now();
		t->snd_nxt++;
		t->fin_pending = 0;
		t->ack_now = 0;
		if (t->state == TCP_ESTABLISHED)
			t->state = TCP_FIN_WAIT1;
		else if (t->state == TCP_CLOSE_WAIT)
			t->state = TCP_LAST_ACK;
	}
	if (t->ack_now) {
		send_seg(t, ACK, t->snd_nxt, NULL, 0, 0);
		t->ack_now = 0;
	}
}

static u16 parse_mss(const u8 *seg, u16 hl)
{
	u16 i = 20;
	while (i < hl) {
		u8 kind = seg[i];
		if (kind == 0)
			break;
		if (kind == 1) {
			i++;
			continue;
		}
		if (i + 1 >= hl || seg[i + 1] < 2)
			break;
		if (kind == 2 && seg[i + 1] == 4 && i + 3 < hl)
			return get16(seg + i + 2);
		i += seg[i + 1];
	}
	return 536;
}

void tcp_input(const u8 *src, u8 *seg, u16 len)
{
	u16 hl, sport, dport, wnd, dlen;
	u32 seq, ack;
	u8 flags, *data;
	tcb_t *t = NULL;
	int i;

	if (len < 20)
		return;
	hl = hinib(seg[12]) * 4;
	if (hl < 20 || hl > len)
		return;
	sport = get16(seg);
	dport = get16(seg + 2);
	seq = get32(seg + 4);
	ack = get32(seg + 8);
	flags = seg[13];
	wnd = get16(seg + 14);
	data = seg + hl;
	dlen = len - hl;

	for (i = 0; i < TCP_MAXCONN; i++)
		if (tcbs[i].state != TCP_CLOSED && tcbs[i].rport == sport && tcbs[i].lport == dport
		    && !memcmp(tcbs[i].rip, src, 4)) {
			t = &tcbs[i];
			break;
		}

	if (!t) {
		const tcp_app_t *app = NULL;
		if (flags & RST)
			return;
		if ((flags & (SYN | ACK)) == SYN) {
			for (i = 0; i < MAX_LISTEN; i++)
				if (listeners[i] && listeners[i]->port == dport)
					app = listeners[i];
			if (app)
				for (i = 0; i < TCP_MAXCONN; i++)
					if (tcbs[i].state == TCP_CLOSED) {
						t = &tcbs[i];
						break;
					}
			if (!app) {
				send_reset(src, sport, dport, 0, seq + 1 + dlen, 1);
				return;
			}
			if (!t)
				return;         /* all busy: the peer will retry */
			memset(t, 0, sizeof(tcb_t) - TCP_TXBUF);
			t->state = TCP_SYN_RCVD;
			memcpy(t->rip, src, 4);
			t->rport = sport;
			t->lport = dport;
			t->rcv_nxt = seq + 1;
			t->iss = ((u32)now() << 16) | (u16)(++iss_counter * 4093u);
			t->snd_una = t->iss;
			t->snd_nxt = t->iss + 1;
			t->snd_wnd = wnd;
			t->mss = parse_mss(seg, hl);
			if (t->mss > OUR_MSS)
				t->mss = OUR_MSS;
			if (t->mss < 64)
				t->mss = 64;
			t->app = app;
			t->rto = hz;
			t->rtimer = now();
			tcp_conns++;
			send_seg(t, SYN | ACK, t->iss, NULL, 0, 1);
			return;
		}
		if (flags & ACK)
			send_reset(src, sport, dport, ack, 0, 0);
		else
			send_reset(src, sport, dport, 0, seq + dlen + ((flags & (SYN | FIN)) ? 1 : 0), 1);
		return;
	}

	t->idle = 0;
	if (flags & RST) {
		DBG(t, "reset by peer");
		set_closed(t);
		return;
	}
	if (flags & SYN) {
		/* our SYN-ACK got lost: send it again */
		if (t->state == TCP_SYN_RCVD && seq + 1 == t->rcv_nxt)
			send_seg(t, SYN | ACK, t->iss, NULL, 0, 1);
		else
			send_seg(t, ACK, t->snd_nxt, NULL, 0, 0);
		return;
	}
	if (!(flags & ACK))
		return;

	if (t->state == TCP_SYN_RCVD) {
		if (ack != t->snd_nxt) {
			send_reset(src, sport, dport, ack, 0, 0);
			return;
		}
		t->snd_una = ack;
		t->state = TCP_ESTABLISHED;
		t->retries = 0;
		t->rto = hz;
		if (t->app->accept)
			t->app->accept(t);
	} else if (SEQ_LT(t->snd_una, ack) && SEQ_LE(ack, t->snd_nxt)) {
		u32 n = ack - t->snd_una;
		u16 d = n > t->txlen ? t->txlen : (u16)n;
		if (d) {
			memmove(t->tx, t->tx + d, t->txlen - d);
			t->txlen -= d;
		}
		t->snd_una = ack;
		t->retries = 0;
		t->rto = hz;
		t->rtimer = now();
		if (ack == t->snd_nxt) {
			/* everything acknowledged, our FIN too if it was sent */
			if (t->state == TCP_FIN_WAIT1)
				t->state = TCP_FIN_WAIT2;
			else if (t->state == TCP_CLOSING) {
				t->state = TCP_TIME_WAIT;
				t->rtimer = now();
				app_done(t);
			} else if (t->state == TCP_LAST_ACK) {
				set_closed(t);
				return;
			}
		}
		if (d && t->app->sent && (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT))
			t->app->sent(t);
	}
	t->snd_wnd = wnd;

	if (dlen) {
		if (seq == t->rcv_nxt && (t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT1
					  || t->state == TCP_FIN_WAIT2)) {
			t->rcv_nxt += dlen;
			if (t->app->recv)
				t->app->recv(t, data, dlen);
		}
		t->ack_now = 1;
	}
	if ((flags & FIN) && t->state == TCP_TIME_WAIT)
		t->ack_now = 1;
	if ((flags & FIN) && seq + dlen == t->rcv_nxt) {
		t->rcv_nxt++;
		t->ack_now = 1;
		switch (t->state) {
		case TCP_ESTABLISHED:
			t->state = TCP_CLOSE_WAIT;
			t->fin_pending = 1;     /* close our side as well */
			break;
		case TCP_FIN_WAIT1:
			if (t->snd_una == t->snd_nxt) {
				t->state = TCP_TIME_WAIT;
				t->rtimer = now();
				app_done(t);
			} else
				t->state = TCP_CLOSING;
			break;
		case TCP_FIN_WAIT2:
			t->state = TCP_TIME_WAIT;
			t->rtimer = now();
			app_done(t);
			break;
		}
	}
	output(t);
}

/* called often: retransmissions, and data the application wrote */
void tcp_poll(void)
{
	int i;
	for (i = 0; i < TCP_MAXCONN; i++) {
		tcb_t *t = &tcbs[i];
		if (t->state == TCP_CLOSED)
			continue;
		if (t->state == TCP_TIME_WAIT) {
			if (elapsed(t->rtimer, 2 * hz))
				set_closed(t);
			continue;
		}
		if ((t->snd_nxt != t->snd_una || t->state == TCP_SYN_RCVD) && elapsed(t->rtimer, t->rto)) {
			if (++t->retries > 8) {
				DBG(t, "too many retransmissions, abort");
				tcp_abort(t);
				continue;
			}
			tcp_retrans++;
			if (tcp_debug)
				cprint("[tcp %u<-%u %s: retransmit #%u, %u bytes in flight, rto %u]\n", t->lport,
					t->rport, tcp_state_names[t->state], t->retries,
					(u16)(t->snd_nxt - t->snd_una), t->rto);
			if (t->rto < 16 * hz)
				t->rto <<= 1;
			t->rtimer = now();
			if (t->state == TCP_SYN_RCVD) {
				send_seg(t, SYN | ACK, t->iss, NULL, 0, 1);
				continue;
			}
			/* go back: send again from the oldest unacknowledged byte */
			if (fin_sent(t) && (u16)(t->snd_nxt - t->snd_una) > t->txlen)
				t->fin_pending = 1;
			t->snd_nxt = t->snd_una;
			if (t->snd_wnd == 0)
				t->snd_wnd = 1;         /* window probe */
		}
		output(t);
	}
}

void tcp_timer(void)
{
	int i;
	for (i = 0; i < TCP_MAXCONN; i++) {
		tcb_t *t = &tcbs[i];
		if (t->state == TCP_CLOSED)
			continue;
		t->idle++;
		if (t->state == TCP_FIN_WAIT2 && t->idle > 60)
			set_closed(t);
		else if (t->idle > 1800)        /* 30 minutes without traffic */
			tcp_abort(t);
	}
}

void tcp_show(sink_t *s)
{
	int i, n = 0;
	for (i = 0; i < TCP_MAXCONN; i++) {
		tcb_t *t = &tcbs[i];
		if (t->state == TCP_CLOSED)
			continue;
		sprint(s, "  %u <- %I:%u  %s, %u bytes queued, idle %us\n", t->lport, t->rip, t->rport,
			tcp_state_names[t->state], t->txlen, t->idle);
		n++;
	}
	if (!n)
		sputs(s, "  no connections\n");
}

/* ---- a sink writing into a connection */
static void tcpsink_put(sink_t *s, char c)
{
	tcb_t *t = s->ctx;
	if (t && t->state != TCP_CLOSED)
		tcp_write(t, (const u8 *)&c, 1);
}

void tcp_sink(sink_t *s, tcb_t *t)
{
	s->put = tcpsink_put;
	s->ctx = t;
	s->col = 0;
}
