/* monitor.c - net11: command interpreter, for the console and telnet */
#include "net.h"

#define VERSION "0.1"

void banner(sink_t *s)
{
	sputs(s, "\nnet11 " VERSION " - TCP/IP for the PDP-11 with a DEQNA/DELQA\n");
}

extern char prog_end[];

static void show_mem_cpu(sink_t *s)
{
	u16 here;
	sprint(s, "  Memory   program 001000..%06o, stack at %06o, %u bytes free between\n",
		(u16)prog_end, (u16)&here, (u16)&here - (u16)prog_end);
}

void sysinfo(sink_t *s)
{
	char tbuf[32];
	u32 up = uptime_s;
	u32 r;
	u16 days = (u16)udivmod32(up, 86400UL, &r);
	u16 hh = (u16)(r / 3600), mm = (u16)((r % 3600) / 60), ss = (u16)(r % 60);
	sprint(s, "  Uptime   %u days %02u:%02u:%02u%s\n", days, hh, mm, ss,
		have_clock ? "" : " (no line clock: approximate)");
	if (time_valid) {
		fmt_time(tbuf, unix_time());
		sprint(s, "  Time     %s\n", tbuf);
	}
	if (qe_present)
		sprint(s, "  Ethernet DELQA/DEQNA at %o, address %M\n", qe_csr_addr - 016, my_mac);
	else
		sputs(s, "  Ethernet no DEQNA/DELQA found at 174440\n");
	if (ip_up)
		sprint(s, "  IP       %I mask %I gateway %I DNS %I\n", my_ip, my_mask, my_gw, my_dns);
	else
		sputs(s, "  IP       no address yet\n");
	sprint(s, "  Frames   %lu received, %lu sent, %lu errors\n", qe_rx_frames, qe_tx_frames,
		qe_rx_errors + qe_tx_errors);
	sprint(s, "  Packets  IP %lu in %lu out, ICMP %lu, UDP %lu, TCP %lu, ARP %lu\n", ip_rx, ip_tx,
		icmp_rx, udp_rx, tcp_rx, arp_rx);
	sprint(s, "  TCP      %lu connections, %lu retransmissions, %lu web requests\n", tcp_conns,
		tcp_retrans, http_requests);
}

static void help(sink_t *s, int console)
{
	sputs(s,
	      "  help                 this list\n"
	      "  sys                  system and network status\n"
	      "  ip                   address configuration\n"
	      "  ip A.B.C.D MASK GW [DNS]   set a static address\n"
	      "  dhcp                 get an address with DHCP (again)\n"
	      "  ping HOST [COUNT]    ICMP echo (ctrl-C stops)\n"
	      "  host NAME            look up a name in the DNS\n"
	      "  date                 date and time (from pool.ntp.org)\n"
	      "  arp                  hardware address cache\n"
	      "  tcp                  TCP connections\n"
	      "  debug                log TCP events on the console (on/off)\n"
	      "  hz [50|60]           line clock frequency (QBone KW11: p freq 60)\n"
	      "  mem ADDR [N]         show N words of memory (octal)\n");
	if (console)
		sputs(s, "  halt                 halt the PDP-11 (back to ODT)\n");
	else
		sputs(s, "  quit                 close this telnet session\n");
}

static int split(char *line, char **argv, int max)
{
	int n = 0;
	char *p = line;
	while (*p && n < max) {
		while (*p == ' ')
			*p++ = 0;
		if (!*p)
			break;
		argv[n++] = p;
		while (*p && *p != ' ')
			p++;
	}
	while (*p == ' ')
		*p++ = 0;
	return n;
}

/* ping and host need DNS: remember who asked */
static struct {
	sink_t *s;
	u16 count;
	u8 for_ping;
} pending;

static void dns_done(void *ctx, const char *name, const u8 *ip)
{
	(void)ctx;
	if (!pending.s)
		return;
	if (!ip)
		sprint(pending.s, "%s: not found\n", name);
	else if (pending.for_ping)
		ping_start(pending.s, ip, pending.count);
	else
		sprint(pending.s, "%s is %I\n", name, ip);
	pending.s = NULL;
}

static void lookup(mon_t *m, const char *name, int for_ping, u16 count)
{
	pending.s = m->out;
	pending.count = count;
	pending.for_ping = for_ping;
	if (!dns_lookup(name, dns_done, NULL)) {
		pending.s = NULL;
		sputs(m->out, ip_up ? "DNS busy or no DNS server\n" : "no IP address yet\n");
	}
}

/* forget a sink that goes away (telnet session closed) */
void mon_forget(sink_t *s)
{
	if (pending.s == s)
		pending.s = NULL;
	ping_cancel(s);
}

static void exec(mon_t *m, char *line)
{
	char *argv[6];
	int argc = split(line, argv, 6);
	sink_t *s = m->out;
	char tbuf[32];

	if (!argc)
		return;
	if (!strcmp(argv[0], "help") || !strcmp(argv[0], "?"))
		help(s, m->is_console);
	else if (!strcmp(argv[0], "sys") || !strcmp(argv[0], "stat")) {
		sysinfo(s);
		show_mem_cpu(s);
		dhcp_show(s);
	} else if (!strcmp(argv[0], "ip")) {
		if (argc >= 4) {
			u8 ip[4], mask[4], gw[4], dns[4];
			memset(dns, 0, 4);
			if (!parse_ip(argv[1], ip) || !parse_ip(argv[2], mask) || !parse_ip(argv[3], gw)
			    || (argc > 4 && !parse_ip(argv[4], dns))) {
				sputs(s, "usage: ip A.B.C.D MASK GATEWAY [DNS]\n");
				return;
			}
			ip_static(ip, mask, gw, dns);
		}
		if (ip_up)
			sprint(s, "  address %I mask %I gateway %I DNS %I\n", my_ip, my_mask, my_gw, my_dns);
		else
			sputs(s, "  no address yet\n");
		dhcp_show(s);
	} else if (!strcmp(argv[0], "dhcp")) {
		ip_up = 0;
		dhcp_state = 0;
		dhcp_start();
		sputs(s, "asking for an address ...\n");
	} else if (!strcmp(argv[0], "ping")) {
		u16 count = 4;
		if (argc < 2) {
			sputs(s, "usage: ping HOST [COUNT]\n");
			return;
		}
		if (argc > 2 && !parse_num(argv[2], &count, 10)) {
			sputs(s, "bad count\n");
			return;
		}
		lookup(m, argv[1], 1, count);
	} else if (!strcmp(argv[0], "host")) {
		if (argc < 2)
			sputs(s, "usage: host NAME\n");
		else
			lookup(m, argv[1], 0, 0);
	} else if (!strcmp(argv[0], "date")) {
		if (time_valid) {
			fmt_time(tbuf, unix_time());
			sprint(s, "%s\n", tbuf);
		} else
			sputs(s, "time not known yet (no answer from pool.ntp.org)\n");
	} else if (!strcmp(argv[0], "arp"))
		arp_show(s);
	else if (!strcmp(argv[0], "tcp"))
		tcp_show(s);
	else if (!strcmp(argv[0], "hz")) {
		u16 v;
		if (argc > 1 && parse_num(argv[1], &v, 10) && (v == 50 || v == 60))
			hz = v;
		else if (argc > 1)
			sputs(s, "usage: hz 50|60\n");
		sprint(s, "line clock %u Hz%s\n", hz, have_clock ? "" : " (no KW11: counted by the main loop)");
	} else if (!strcmp(argv[0], "debug")) {
		tcp_debug = !tcp_debug;
		sprint(s, "TCP event log on the console %s\n", tcp_debug ? "on" : "off");
	}
	else if (!strcmp(argv[0], "mem")) {
		u16 a, n = 8, i;
		if (argc < 2 || !parse_num(argv[1], &a, 8) || (argc > 2 && !parse_num(argv[2], &n, 8))) {
			sputs(s, "usage: mem ADDR [N], octal\n");
			return;
		}
		a &= ~1;
		if (n > 0100)
			n = 0100;
		for (i = 0; i < n; i++) {
			volatile u16 *p = (volatile u16 *)(a + 2 * i);
			if (!(i & 7))
				sprint(s, "%s%06o:", i ? "\n" : "", a + 2 * i);
			if (a + 2 * i >= 0160000 && !probe(p))
				sputs(s, " ------");
			else
				sprint(s, " %06o", *p);
		}
		sputs(s, "\n");
	} else if (!strcmp(argv[0], "halt") && m->is_console) {
		sputs(s, "halting; start again at 1000\n");
		halt_cpu();
	} else if ((!strcmp(argv[0], "quit") || !strcmp(argv[0], "exit")) && !m->is_console)
		m->quit = 1;
	else
		sprint(s, "%s: unknown command, try help\n", argv[0]);
}

void mon_init(mon_t *m, sink_t *out, int is_console)
{
	memset(m, 0, sizeof *m);
	m->out = out;
	m->is_console = is_console;
}

void mon_prompt(mon_t *m)
{
	sprint(m->out, "%s> ", my_hostname);
}

void mon_input(mon_t *m, int c)
{
	sink_t *s = m->out;
	if (c == 3) {                   /* ctrl-C */
		if (ping_active())
			ping_stop();
		m->len = 0;
		sputs(s, "^C\n");
		mon_prompt(m);
		return;
	}
	if (c == '\r' || c == '\n') {
		sputs(s, "\n");
		m->line[m->len] = 0;
		m->len = 0;
		exec(m, m->line);
		if (!m->quit)
			mon_prompt(m);
		return;
	}
	if (c == 8 || c == 127) {
		if (m->len) {
			m->len--;
			sputs(s, "\b \b");
		}
		return;
	}
	if (c == 025) {                 /* ctrl-U */
		while (m->len) {
			m->len--;
			sputs(s, "\b \b");
		}
		return;
	}
	if (c < ' ' || c > '~')
		return;
	if (c >= 'A' && c <= 'Z')
		c += 'a' - 'A';         /* commands are lower case; uppercase terminals welcome */
	if (m->len < sizeof m->line - 1) {
		m->line[m->len++] = c;
		sputc(s, c);
	}
}
