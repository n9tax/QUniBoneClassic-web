/* sntp.c - net11: date and time from an NTP server (SNTP, RFC 4330) */
#include "net.h"

#define SNTP_PORT 1123
#define NTP_UNIX_OFFSET 2208988800UL    /* 1900 -> 1970 */

int time_valid;
u32 time_base, time_base_up;

static const char *server = "pool.ntp.org";
static u8 server_ip[4];
static u16 wait_s = 5;          /* seconds until the next attempt */
static u8 pending;

u32 unix_time(void)
{
	return time_base + (uptime_s - time_base_up);
}

static void input(const u8 *src, u16 sport, u16 dport, u8 *d, u16 len)
{
	u32 t;
	char buf[32];
	(void)src;
	(void)sport;
	(void)dport;
	if (len < 48 || (d[0] & 7) != 4)       /* mode 4: server */
		return;
	t = get32(d + 40);                      /* transmit time, seconds */
	if (t < NTP_UNIX_OFFSET)
		return;
	time_base = t - NTP_UNIX_OFFSET;
	time_base_up = uptime_s;
	pending = 0;
	if (!time_valid) {
		fmt_time(buf, time_base);
		cprint("\nTime: %s\n", buf);
	}
	time_valid = 1;
	wait_s = 6 * 3600;      /* again in 6 hours */
}

static void send_req(void)
{
	u8 *p = udp_payload();
	memset(p, 0, 48);
	p[0] = 0x1b;            /* version 3, client */
	udp_send(server_ip, SNTP_PORT, 123, 48);
}

static void resolved(void *ctx, const char *name, const u8 *ip)
{
	(void)ctx;
	(void)name;
	if (!ip)
		return;
	memcpy(server_ip, ip, 4);
	send_req();
}

void sntp_start(void)
{
	udp_bind(SNTP_PORT, input);
}

void sntp_timer(void)
{
	if (!ip_up)
		return;
	if (--wait_s)
		return;
	wait_s = time_valid ? 6 * 3600 : 30;
	pending = 1;
	if (!dns_lookup(server, resolved, NULL))
		wait_s = 5;     /* resolver busy or no DNS server: soon again */
}

/* "2026-11-09 22:15:03 UTC" (buf: 24 bytes) */
void fmt_time(char *buf, u32 t)
{
	static const u8 mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	u32 r;
	u16 days, y, m, d, ylen, secs_lo;
	u16 hh, mm, ss;
	days = (u16)udivmod32(t, 86400UL, &r);
	secs_lo = (u16)(r / 60);        /* minutes of the day, < 1440 */
	ss = (u16)(r % 60);
	hh = secs_lo / 60;
	mm = secs_lo % 60;
	for (y = 1970;; y++) {
		ylen = ((y & 3) == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
		if (days < ylen)
			break;
		days -= ylen;
	}
	for (m = 0; m < 12; m++) {
		d = mdays[m] + (m == 1 && ylen == 366);
		if (days < d)
			break;
		days -= d;
	}
	snprint(buf, 24, "%u-%02u-%02u %02u:%02u:%02u UTC", y, m + 1, days + 1, hh, mm, ss);
}
