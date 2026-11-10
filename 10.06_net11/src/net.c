/* net.c - net11: Ethernet, ARP, IPv4, ICMP (ping), UDP.

   Packet fields are always read and written byte by byte (get16/put16...):
   the PDP-11 is little endian within a word and stores 32 bit values high
   word first, the network is big endian. */
#include "net.h"

u8 my_ip[4], my_mask[4], my_gw[4], my_dns[4];
int ip_up;
char my_hostname[16] = "pdp11";
u32 ip_rx, ip_tx, icmp_rx, udp_rx, tcp_rx, arp_rx, eth_drop;

static u8 txf[ETH_MAX + 2];     /* the one transmit frame being built */
static u8 arpf[60];
static u16 ip_id = 1;
static const u8 bcast_mac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const u8 bcast_ip[4] = { 255, 255, 255, 255 };

/* ---------------------------------------------------------------- checksum */

u16 ip_cksum(u16 sum, const u8 *p, u16 n)
{
	u16 w;
	while (n > 1) {
		w = ((u16)p[0] << 8) | p[1];
		sum += w;
		if (sum < w)
			sum++;
		p += 2;
		n -= 2;
	}
	if (n) {
		w = (u16)p[0] << 8;
		sum += w;
		if (sum < w)
			sum++;
	}
	return sum;
}

static u16 sum_add(u16 sum, u16 w)
{
	sum += w;
	if (sum < w)
		sum++;
	return sum;
}

/* ---------------------------------------------------------------- ARP */

#define NARP 8
static struct arpent {
	u8 ip[4];
	u8 mac[6];
	u16 age;                /* seconds since last confirmed */
	u8 used;
} arp[NARP];

static int same_net(const u8 *ip)
{
	int i;
	for (i = 0; i < 4; i++)
		if ((ip[i] & my_mask[i]) != (my_ip[i] & my_mask[i]))
			return 0;
	return 1;
}

static int is_bcast(const u8 *ip)
{
	int i;
	if (!memcmp(ip, bcast_ip, 4))
		return 1;
	if (!ip_up)
		return 0;
	for (i = 0; i < 4; i++)
		if ((ip[i] | my_mask[i]) != 0xff || (ip[i] & my_mask[i]) != (my_ip[i] & my_mask[i]))
			return 0;
	return 1;
}

static void arp_update(const u8 *ip, const u8 *mac, int create)
{
	int i, victim = 0;
	u16 oldest = 0;
	if (!ip[0] && !ip[1] && !ip[2] && !ip[3])
		return;
	for (i = 0; i < NARP; i++)
		if (arp[i].used && !memcmp(arp[i].ip, ip, 4)) {
			memcpy(arp[i].mac, mac, 6);
			arp[i].age = 0;
			return;
		}
	if (!create)
		return;
	for (i = 0; i < NARP; i++) {
		if (!arp[i].used) {
			victim = i;
			break;
		}
		if (arp[i].age >= oldest) {
			oldest = arp[i].age;
			victim = i;
		}
	}
	memcpy(arp[victim].ip, ip, 4);
	memcpy(arp[victim].mac, mac, 6);
	arp[victim].age = 0;
	arp[victim].used = 1;
}

int arp_lookup(const u8 *ip, u8 *mac)
{
	int i;
	for (i = 0; i < NARP; i++)
		if (arp[i].used && !memcmp(arp[i].ip, ip, 4)) {
			memcpy(mac, arp[i].mac, 6);
			return 1;
		}
	return 0;
}

static void arp_send(u16 op, const u8 *tmac, const u8 *tip, const u8 *dst_mac)
{
	u8 *p = arpf + 14;
	memcpy(arpf, dst_mac, 6);
	memcpy(arpf + 6, my_mac, 6);
	put16(arpf + 12, 0x0806);
	put16(p, 1);
	put16(p + 2, 0x0800);
	p[4] = 6;
	p[5] = 4;
	put16(p + 6, op);
	memcpy(p + 8, my_mac, 6);
	memcpy(p + 14, my_ip, 4);
	memcpy(p + 18, tmac, 6);
	memcpy(p + 24, tip, 4);
	memset(arpf + 42, 0, 18);
	qe_send(arpf, 60);
}

static u16 last_arp_req;
static u8 last_arp_ip[4];

static void arp_request(const u8 *ip)
{
	static const u8 zero[6];
	/* not more than one request per second for the same address */
	if (!memcmp(ip, last_arp_ip, 4) && !elapsed(last_arp_req, hz))
		return;
	memcpy(last_arp_ip, ip, 4);
	last_arp_req = now();
	arp_send(1, zero, ip, bcast_mac);
}

static void arp_input(u8 *p, u16 len)
{
	u16 op;
	if (len < 28 || get16(p) != 1 || get16(p + 2) != 0x0800 || p[4] != 6 || p[5] != 4)
		return;
	arp_rx++;
	op = get16(p + 6);
	if (ip_up && !memcmp(p + 24, my_ip, 4)) {
		arp_update(p + 14, p + 8, 1);
		if (op == 1)
			arp_send(2, p + 8, p + 14, p + 8);
	} else
		arp_update(p + 14, p + 8, 0);
}

void arp_show(sink_t *s)
{
	int i, n = 0;
	for (i = 0; i < NARP; i++)
		if (arp[i].used) {
			sprint(s, "  %-16I %M  %us\n", arp[i].ip, arp[i].mac, arp[i].age);
			n++;
		}
	if (!n)
		sputs(s, "  (empty)\n");
}

/* ---------------------------------------------------------------- IP */

u8 *ip_payload(void)
{
	return txf + 34;
}

int ip_send(const u8 *dst, u8 proto, u16 len)
{
	u8 *h = txf + 14;
	const u8 *hop = dst;
	u8 mac[6];

	if (is_bcast(dst))
		memcpy(mac, bcast_mac, 6);
	else {
		if (ip_up && !same_net(dst) && (my_gw[0] | my_gw[1] | my_gw[2] | my_gw[3]))
			hop = my_gw;
		if (!arp_lookup(hop, mac)) {
			arp_request(hop);
			return 0;
		}
	}
	memcpy(txf, mac, 6);
	memcpy(txf + 6, my_mac, 6);
	put16(txf + 12, 0x0800);
	h[0] = 0x45;
	h[1] = 0;
	put16(h + 2, 20 + len);
	put16(h + 4, ip_id++);
	put16(h + 6, 0);
	h[8] = 64;
	h[9] = proto;
	put16(h + 10, 0);
	if (ip_up)
		memcpy(h + 12, my_ip, 4);
	else
		memset(h + 12, 0, 4);
	memcpy(h + 16, dst, 4);
	put16(h + 10, ~ip_cksum(0, h, 20));
	ip_tx++;
	return qe_send(txf, 34 + len) ? 1 : -1;
}

/* ---------------------------------------------------------------- ICMP / ping */

static struct {
	sink_t *s;
	u8 ip[4];
	u16 count, sent, recvd, seq, id;
	u16 t_next, t_sent[8];
	u16 t_last;
	u32 rtt_sum;
	u16 rtt_min, rtt_max;
	u8 active;
} ping;

int ping_active(void)
{
	return ping.active;
}

void ping_stop(void)
{
	if (!ping.active)
		return;
	ping.active = 0;
	sprint(ping.s, "--- %I: %u sent, %u received", ping.ip, ping.sent, ping.recvd);
	if (ping.recvd)
		sprint(ping.s, ", round trip min/avg/max %u/%lu/%u ms", ping.rtt_min,
			ping.rtt_sum / ping.recvd, ping.rtt_max);
	sputs(ping.s, "\n");
}

void ping_start(sink_t *s, const u8 *ip, u16 count)
{
	ping_stop();
	memset(&ping, 0, sizeof ping);
	ping.s = s;
	memcpy(ping.ip, ip, 4);
	ping.count = count ? count : 4;
	ping.id = now() | 1;
	ping.t_next = now();
	ping.rtt_min = 0xffff;
	ping.active = 1;
	sprint(s, "PING %I: 56 data bytes\n", ip);
}

void ping_poll(void)
{
	u8 *p;
	int i, r;
	if (!ping.active)
		return;
	if (ping.sent >= ping.count) {
		/* wait 2 s for the last reply */
		if (ping.recvd >= ping.sent || elapsed(ping.t_last, 2 * hz))
			ping_stop();
		return;
	}
	if ((s16)(now() - ping.t_next) < 0)
		return;
	p = ip_payload();
	p[0] = 8;
	p[1] = 0;
	put16(p + 2, 0);
	put16(p + 4, ping.id);
	put16(p + 6, ping.seq);
	for (i = 0; i < 56; i++)
		p[8 + i] = 'a' + i % 26;
	put16(p + 2, ~ip_cksum(0, p, 64));
	r = ip_send(ping.ip, IP_ICMP, 64);
	if (r == 0) {           /* resolving the hardware address: soon again */
		ping.t_next = now() + hz / 4;
		if (elapsed(ping.t_last, 3 * hz) && ping.t_last) {
			sprint(ping.s, "no answer to ARP for %I\n", ping.ip);
			ping.t_last = now();
		}
		if (!ping.t_last)
			ping.t_last = now();
		return;
	}
	ping.t_sent[ping.seq & 7] = now();
	ping.seq++;
	ping.sent++;
	ping.t_last = now();
	ping.t_next = now() + hz;
}

static void ping_reply(const u8 *src, u8 *p, u16 len)
{
	u16 seq, dt, ms;
	if (!ping.active || get16(p + 4) != ping.id)
		return;
	seq = get16(p + 6);
	if ((u16)(ping.seq - seq) > 8)
		return;
	dt = now() - ping.t_sent[seq & 7];
	ms = (u16)(((u32)dt * 1000) / hz);
	ping.recvd++;
	ping.rtt_sum += ms;
	if (ms < ping.rtt_min)
		ping.rtt_min = ms;
	if (ms > ping.rtt_max)
		ping.rtt_max = ms;
	if (dt)
		sprint(ping.s, "%u bytes from %I: icmp_seq=%u time=%u ms\n", len - 8, src, seq, ms);
	else            /* within one clock tick */
		sprint(ping.s, "%u bytes from %I: icmp_seq=%u time<%u ms\n", len - 8, src, seq,
			(u16)(1000 / hz + 1));
}

static void icmp_input(const u8 *src, int bcast, u8 *p, u16 len)
{
	u8 *q;
	if (len < 8 || ip_cksum(0, p, len) != 0xffff)
		return;
	icmp_rx++;
	switch (p[0]) {
	case 8:                 /* echo request */
		if (bcast || len > ETH_MAX - 34)
			return;
		q = ip_payload();
		memcpy(q, p, len);
		q[0] = 0;
		put16(q + 2, 0);
		put16(q + 2, ~ip_cksum(0, q, len));
		ip_send(src, IP_ICMP, len);
		break;
	case 0:
		ping_reply(src, p, len);
		break;
	}
}

/* ---------------------------------------------------------------- UDP */

#define NUDP 6
static struct {
	u16 port;
	udp_handler_t h;
} udp_ports[NUDP];

int udp_bind(u16 port, udp_handler_t h)
{
	int i;
	for (i = 0; i < NUDP; i++)
		if (!udp_ports[i].port || udp_ports[i].port == port) {
			udp_ports[i].port = h ? port : 0;
			udp_ports[i].h = h;
			return 1;
		}
	return 0;
}

u16 ip_pseudo(const u8 *src, const u8 *dst, u8 proto, u16 len)
{
	u16 sum = ip_cksum(0, src, 4);
	sum = ip_cksum(sum, dst, 4);
	sum = sum_add(sum, proto);
	return sum_add(sum, len);
}

static void udp_input(const u8 *src, const u8 *dst, u8 *p, u16 len)
{
	u16 ulen, dport;
	int i;
	if (len < 8)
		return;
	ulen = get16(p + 4);
	if (ulen < 8 || ulen > len)
		return;
	if (get16(p + 6) && ip_cksum(ip_pseudo(src, dst, IP_UDP, ulen), p, ulen) != 0xffff)
		return;
	udp_rx++;
	dport = get16(p + 2);
	for (i = 0; i < NUDP; i++)
		if (udp_ports[i].port == dport && udp_ports[i].h) {
			udp_ports[i].h(src, get16(p), dport, p + 8, ulen - 8);
			return;
		}
}

u8 *udp_payload(void)
{
	return ip_payload() + 8;
}

int udp_send(const u8 *dst, u16 sport, u16 dport, u16 len)
{
	u8 *u = ip_payload();
	static const u8 zero[4];
	u16 c;
	put16(u, sport);
	put16(u + 2, dport);
	put16(u + 4, len + 8);
	put16(u + 6, 0);
	c = ~ip_cksum(ip_pseudo(ip_up ? my_ip : zero, dst, IP_UDP, len + 8), u, len + 8);
	put16(u + 6, c ? c : 0xffff);
	return ip_send(dst, IP_UDP, len + 8);
}

/* ---------------------------------------------------------------- input */

static void ip_input(u8 *eth, u8 *p, u16 len)
{
	u16 hl, tot;
	u8 *src, *dst;
	int bcast;
	if (len < 20 || hinib(p[0]) != 4)
		return;
	hl = (p[0] & 15) * 4;
	tot = get16(p + 2);
	if (hl < 20 || tot < hl || tot > len)
		return;
	if (ip_cksum(0, p, hl) != 0xffff)
		return;
	if (get16(p + 6) & 0x3fff)      /* fragment: not supported */
		return;
	src = p + 12;
	dst = p + 16;
	bcast = is_bcast(dst);
	if (!(ip_up && !memcmp(dst, my_ip, 4)) && !bcast && !(!ip_up && p[9] == IP_UDP))
		return;
	ip_rx++;
	/* learn hardware addresses from traffic */
	if (ip_up && !bcast) {
		if (same_net(src))
			arp_update(src, eth + 6, 1);
		else if (my_gw[0] | my_gw[1] | my_gw[2] | my_gw[3])
			arp_update(my_gw, eth + 6, 1);
	}
	switch (p[9]) {
	case IP_ICMP:
		icmp_input(src, bcast, p + hl, tot - hl);
		break;
	case IP_UDP:
		udp_input(src, dst, p + hl, tot - hl);
		break;
	case IP_TCP:
		if (ip_up && !bcast) {
			tcp_rx++;
			/* TCP checksum over the pseudo header */
			if (ip_cksum(ip_pseudo(src, dst, IP_TCP, tot - hl), p + hl, tot - hl) == 0xffff)
				tcp_input(src, p + hl, tot - hl);
		}
		break;
	}
}

void eth_input(u8 *f, u16 len)
{
	u16 type;
	if (len < 14) {
		eth_drop++;
		return;
	}
	type = get16(f + 12);
	if (type == 0x0800)
		ip_input(f, f + 14, len - 14);
	else if (type == 0x0806)
		arp_input(f + 14, len - 14);
	else
		eth_drop++;
}

void net_timer(void)
{
	int i;
	for (i = 0; i < NARP; i++)
		if (arp[i].used && ++arp[i].age > 1200)
			arp[i].used = 0;
}

/* a telnet session closed: its ping must not print any more */
void ping_cancel(sink_t *s)
{
	if (ping.active && ping.s == s)
		ping.active = 0;
}
