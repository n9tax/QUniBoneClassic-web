/* dns.c - net11: DNS resolver for A records, one query at a time */
#include "net.h"

#define DNS_PORT 1053

static struct {
	u8 active;
	u16 id;
	char name[64];
	dns_cb_t cb;
	void *ctx;
	u16 timer, tries;
} q;

static u16 next_id = 0x1123;

static void send_query(void)
{
	u8 *p = udp_payload(), *o;
	const char *s = q.name;
	put16(p, q.id);
	put16(p + 2, 0x0100);           /* recursion desired */
	put16(p + 4, 1);
	put16(p + 6, 0);
	put16(p + 8, 0);
	put16(p + 10, 0);
	o = p + 12;
	while (*s) {
		u8 *lenp = o++;
		u8 n = 0;
		while (*s && *s != '.') {
			*o++ = *s++;
			n++;
		}
		*lenp = n;
		if (*s == '.')
			s++;
	}
	*o++ = 0;
	put16(o, 1);                    /* A */
	put16(o + 2, 1);                /* IN */
	o += 4;
	udp_send(my_dns, DNS_PORT, 53, o - p);
}

static void finish(const u8 *ip)
{
	dns_cb_t cb = q.cb;
	q.active = 0;
	if (cb)
		cb(q.ctx, q.name, ip);
}

/* skip a (possibly compressed) name; NULL if malformed */
static u8 *skip_name(u8 *p, u8 *end)
{
	while (p < end) {
		if (*p == 0)
			return p + 1;
		if ((*p & 0xc0) == 0xc0)
			return p + 2;
		p += *p + 1;
	}
	return NULL;
}

static void input(const u8 *src, u16 sport, u16 dport, u8 *d, u16 len)
{
	u16 qd, an, i;
	u8 *p, *end = d + len;
	(void)src;
	(void)sport;
	(void)dport;
	if (!q.active || len < 12 || get16(d) != q.id || !(d[2] & 0x80))
		return;
	if (d[3] & 15) {
		finish(NULL);
		return;
	}
	qd = get16(d + 4);
	an = get16(d + 6);
	p = d + 12;
	for (i = 0; i < qd && p; i++) {
		p = skip_name(p, end);
		if (p)
			p += 4;
	}
	for (i = 0; i < an && p && p < end; i++) {
		u16 type, rdlen;
		p = skip_name(p, end);
		if (!p || p + 10 > end)
			break;
		type = get16(p);
		rdlen = get16(p + 8);
		p += 10;
		if (p + rdlen > end)
			break;
		if (type == 1 && rdlen == 4) {
			finish(p);
			return;
		}
		p += rdlen;
	}
	finish(NULL);
}

int dns_lookup(const char *name, dns_cb_t cb, void *ctx)
{
	u8 ip[4];
	static int bound;
	if (parse_ip(name, ip)) {
		cb(ctx, name, ip);
		return 1;
	}
	if (!ip_up || !(my_dns[0] | my_dns[1] | my_dns[2] | my_dns[3]) || q.active
	    || strlen(name) >= sizeof q.name)
		return 0;
	if (!bound) {
		udp_bind(DNS_PORT, input);
		bound = 1;
	}
	strcpy(q.name, name);
	q.cb = cb;
	q.ctx = ctx;
	q.id = next_id++;
	q.tries = 0;
	q.timer = 2;
	q.active = 1;
	send_query();
	return 1;
}

void dns_timer(void)
{
	if (!q.active || --q.timer)
		return;
	if (++q.tries >= 3) {
		finish(NULL);
		return;
	}
	q.timer = 2;
	send_query();
}
