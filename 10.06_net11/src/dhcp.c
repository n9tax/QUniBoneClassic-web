/* dhcp.c - net11: DHCP client (RFC 2131), or a static address */
#include "net.h"

enum { D_OFF, D_DISCOVER, D_REQUEST, D_BOUND, D_STATIC };
int dhcp_state;
u32 dhcp_lease;
u8 dhcp_server[4];

static u32 xid;
static u8 offered[4];
static u16 timer, tries;
static u32 renew_in, expire_in;
static const u8 bcast[4] = { 255, 255, 255, 255 };

static void send_msg(u8 type)
{
	u8 *p = udp_payload(), *o;
	unsigned n;
	memset(p, 0, 240);
	p[0] = 1;               /* BOOTREQUEST */
	p[1] = 1;               /* Ethernet */
	p[2] = 6;
	put32(p + 4, xid);
	put16(p + 10, 0x8000);  /* broadcast replies: we have no address yet */
	memcpy(p + 28, my_mac, 6);
	p[236] = 99;
	p[237] = 130;
	p[238] = 83;
	p[239] = 99;
	o = p + 240;
	*o++ = 53;
	*o++ = 1;
	*o++ = type;
	*o++ = 61;              /* client identifier */
	*o++ = 7;
	*o++ = 1;
	memcpy(o, my_mac, 6);
	o += 6;
	n = strlen(my_hostname);
	*o++ = 12;              /* host name */
	*o++ = n;
	memcpy(o, my_hostname, n);
	o += n;
	if (type == 3) {        /* REQUEST */
		*o++ = 50;
		*o++ = 4;
		memcpy(o, offered, 4);
		o += 4;
		if (dhcp_server[0] | dhcp_server[1] | dhcp_server[2] | dhcp_server[3]) {
			*o++ = 54;
			*o++ = 4;
			memcpy(o, dhcp_server, 4);
			o += 4;
		}
	}
	*o++ = 55;              /* parameters: mask, router, DNS, domain */
	*o++ = 4;
	*o++ = 1;
	*o++ = 3;
	*o++ = 6;
	*o++ = 15;
	*o++ = 57;              /* maximum message size */
	*o++ = 2;
	put16(o, 1500);
	o += 2;
	*o++ = 255;
	while ((o - p) < 300)   /* some servers want a BOOTP sized packet */
		*o++ = 0;
	udp_send(bcast, 68, 67, o - p);
}

static void start_discover(void)
{
	xid = ((u32)now() << 16) ^ ((u32)my_mac[4] << 8) ^ my_mac[5] ^ 0x5ad11u;
	dhcp_state = D_DISCOVER;
	memset(dhcp_server, 0, 4);
	tries = 0;
	timer = 1;
}

static void input(const u8 *src, u16 sport, u16 dport, u8 *d, u16 len)
{
	u8 type = 0, *o, *end;
	u8 mask[4], gw[4], dns[4], server[4];
	u32 lease = 86400;
	int changed;
	(void)src;
	(void)sport;
	(void)dport;
	if (len < 240 || d[0] != 2 || get32(d + 4) != xid || memcmp(d + 28, my_mac, 6)
	    || d[236] != 99 || d[237] != 130 || d[238] != 83 || d[239] != 99)
		return;
	memset(mask, 0, 4);
	memset(gw, 0, 4);
	memset(dns, 0, 4);
	memset(server, 0, 4);
	o = d + 240;
	end = d + len;
	while (o < end && *o != 255) {
		u8 code = *o++, l;
		if (code == 0)
			continue;
		if (o >= end)
			break;
		l = *o++;
		if (o + l > end)
			break;
		switch (code) {
		case 53:
			type = o[0];
			break;
		case 1:
			if (l >= 4)
				memcpy(mask, o, 4);
			break;
		case 3:
			if (l >= 4)
				memcpy(gw, o, 4);
			break;
		case 6:
			if (l >= 4)
				memcpy(dns, o, 4);
			break;
		case 51:
			if (l >= 4)
				lease = get32(o);
			break;
		case 54:
			if (l >= 4)
				memcpy(server, o, 4);
			break;
		}
		o += l;
	}
	if (type == 2 && dhcp_state == D_DISCOVER) {            /* OFFER */
		memcpy(offered, d + 16, 4);
		memcpy(dhcp_server, server, 4);
		dhcp_state = D_REQUEST;
		tries = 0;
		timer = 4;
		send_msg(3);
	} else if (type == 5 && (dhcp_state == D_REQUEST || dhcp_state == D_BOUND)) {   /* ACK */
		changed = memcmp(my_ip, d + 16, 4) || !ip_up;
		memcpy(my_ip, d + 16, 4);
		if (mask[0])
			memcpy(my_mask, mask, 4);
		else {
			my_mask[0] = my_mask[1] = my_mask[2] = 255;
			my_mask[3] = 0;
		}
		memcpy(my_gw, gw, 4);
		memcpy(my_dns, dns, 4);
		if (server[0])
			memcpy(dhcp_server, server, 4);
		dhcp_lease = lease;
		renew_in = lease / 2;
		if (renew_in < 30)
			renew_in = 30;
		expire_in = lease;
		ip_up = 1;
		dhcp_state = D_BOUND;
		if (changed)
			cprint("\nDHCP: address %I, gateway %I, DNS %I, lease %lu s\n", my_ip, my_gw, my_dns,
				lease);
	} else if (type == 6 && dhcp_state != D_STATIC) {      /* NAK */
		cprint("\nDHCP: server refused the address, starting over\n");
		ip_up = 0;
		start_discover();
	}
}

void dhcp_start(void)
{
	udp_bind(68, input);
	if (dhcp_state != D_STATIC)
		start_discover();
}

void ip_static(const u8 *ip, const u8 *mask, const u8 *gw, const u8 *dns)
{
	memcpy(my_ip, ip, 4);
	memcpy(my_mask, mask, 4);
	memcpy(my_gw, gw, 4);
	memcpy(my_dns, dns, 4);
	ip_up = 1;
	dhcp_state = D_STATIC;
}

void dhcp_timer(void)
{
	switch (dhcp_state) {
	case D_DISCOVER:
	case D_REQUEST:
		if (timer && --timer)
			break;
		if (dhcp_state == D_REQUEST && ++tries > 4) {
			if (ip_up && expire_in)
				break;          /* renewing: keep trying until the lease ends */
			start_discover();
			break;
		}
		if (dhcp_state == D_DISCOVER)
			tries++;
		timer = tries < 3 ? 4 : tries < 6 ? 8 : 16;
		send_msg(dhcp_state == D_DISCOVER ? 1 : 3);
		break;
	case D_BOUND:
		if (renew_in && --renew_in == 0) {
			memcpy(offered, my_ip, 4);
			dhcp_state = D_REQUEST;
			tries = 0;
			timer = 1;
		}
		break;
	}
	if (dhcp_state == D_REQUEST && ip_up && expire_in && --expire_in == 0) {
		cprint("\nDHCP: lease expired\n");
		ip_up = 0;
		start_discover();
	}
}

void dhcp_show(sink_t *s)
{
	static const char *names[] = { "off", "discovering", "requesting", "bound", "static" };
	sprint(s, "  DHCP %s", names[dhcp_state]);
	if (dhcp_state == D_BOUND || dhcp_state == D_REQUEST)
		sprint(s, ", server %I, lease %lu s, renew in %lu s", dhcp_server, dhcp_lease, renew_in);
	sputs(s, "\n");
}
