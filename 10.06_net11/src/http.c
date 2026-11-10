/* http.c - net11: a web server on port 80 with a status page.

   The answer is built into one page buffer and fed into the connection as
   its send buffer drains; one answer at a time (a second request waits for
   the buffer, or gets "busy" if it stays taken). */
#include "net.h"

#define PAGE_MAX 4000
#define NHTTP 4

u32 http_requests;

static char page[PAGE_MAX];
static u16 page_len;
static tcb_t *page_owner;

static struct hreq {
	tcb_t *t;
	char line[64];
	u8 llen, got_line, crlf;
	u16 pos;                /* bytes of page[] written */
	u8 sending;
} reqs[NHTTP];

/* sink into page[] */
static void page_put(sink_t *s, char c)
{
	(void)s;
	if (page_len < PAGE_MAX)
		page[page_len++] = c;
}

static sink_t ps = { page_put, 0, 0 };

static void html(const char *str)
{
	/* escape for HTML */
	for (; *str; str++) {
		if (*str == '<')
			sputs(&ps, "&lt;");
		else if (*str == '&')
			sputs(&ps, "&amp;");
		else
			sputc(&ps, *str);
	}
}

/* run a status function into a buffer, then escape it into the page */
static char tmp[1400];
static u16 tmp_len;
static void tmp_put(sink_t *s, char c)
{
	(void)s;
	if (c != '\r' && tmp_len < sizeof tmp - 1)
		tmp[tmp_len++] = c;
}

static void build_index(void)
{
	sink_t ts = { tmp_put, 0, 0 };
	sputs(&ps, "HTTP/1.0 200 OK\nContent-Type: text/html\nConnection: close\n\n");
	sputs(&ps, "<!DOCTYPE html><html><head><title>net11 on ");
	sputs(&ps, my_hostname);
	sputs(&ps, "</title><meta name=viewport content=\"width=device-width\">"
	      "<style>body{background:#1d1b1f;color:#f0d6a8;font-family:monospace;margin:2em}"
	      "h1{color:#ff9f40;font-size:1.4em;letter-spacing:.2em}"
	      "pre{background:#000;color:#ffb000;padding:1em;border-radius:6px;overflow:auto}"
	      "a{color:#ff9f40}</style></head><body>\n");
	sputs(&ps, "<h1>PDP-11 ONLINE</h1><p>This page was served by a PDP-11, running net11: "
	      "a TCP/IP stack and DEQNA/DELQA driver in about 20 KB.</p>\n<pre>");
	tmp_len = 0;
	sysinfo(&ts);
	tmp[tmp_len] = 0;
	html(tmp);
	sputs(&ps, "\nConnections:\n");
	tmp_len = 0;
	tcp_show(&ts);
	tmp[tmp_len] = 0;
	html(tmp);
	sputs(&ps, "</pre><p>telnet to this machine for the command monitor. "
	      "<a href=\"/stats\">/stats</a> has the same as plain text, <a href=\"/about\">/about</a> tells how this works.</p></body></html>\n");
}

static void build_about(void)
{
	sputs(&ps, "HTTP/1.0 200 OK\nContent-Type: text/html\nConnection: close\n\n");
	sputs(&ps, "<!DOCTYPE html><html><head><title>about net11</title>"
	      "<style>body{background:#1d1b1f;color:#f0d6a8;font-family:monospace;margin:2em;max-width:48em}"
	      "h1,h2{color:#ff9f40;letter-spacing:.15em}a{color:#ff9f40}</style></head><body>\n"
	      "<h1>ABOUT NET11</h1>\n"
	      "<p>net11 is a small stand-alone network system for the DEC PDP-11. It needs no "
	      "operating system: it is loaded into memory and started at address 1000, and it "
	      "runs on any PDP-11 with the EIS instructions (MUL, DIV, ASH), such as the "
	      "11/23, 11/23+, 11/53 or 11/73. It uses neither the floating point unit nor memory "
	      "management.</p>\n"
	      "<h2>WHAT IT DOES</h2>\n"
	      "<p>It drives a DEQNA or DELQA Ethernet controller at 174440 through its buffer "
	      "descriptor lists (DMA), and speaks ARP, IPv4, ICMP, UDP and TCP. It gets its "
	      "address with DHCP, asks pool.ntp.org for the time, resolves names with DNS, "
	      "serves these pages on port 80 and a command monitor on telnet port 23 and on "
	      "the console terminal.</p>\n"
	      "<h2>HOW IT IS BUILT</h2>\n"
	      "<p>About 2000 lines of C and a little assembler, compiled with GCC for the "
	      "pdp11-aout target. Two code generation bugs of GCC 13 for the PDP-11 needed "
	      "workarounds: signed 32 bit comparisons, and right shifts of bytes. The 32 bit "
	      "division routines are in assembler for that reason. Packets are always taken "
	      "apart byte by byte, because the PDP-11 keeps 32 bit numbers high word first "
	      "and the network wants big endian bytes.</p>\n"
	      "<p>Before it met real hardware it ran in emu11, a small PDP-11/23 simulator built "
	      "around the same DELQA logic that the QBone uses to emulate the controller, "
	      "connected to the network by libslirp.</p>\n"
	      "<h2>THE HARDWARE</h2>\n"
	      "<p>On a QBone the DELQA is emulated by a BeagleBone Black plugged into the "
	      "QBUS backplane: the PDP-11 sees a real controller, and the frames go out "
	      "through the BeagleBone's Ethernet port. The QBone also emulates the disks, the "
	      "memory and the line clock that this machine may be missing.</p>\n"
	      "<h2>LIMITS</h2>\n"
	      "<p>No IP fragments, no IPv6, a handful of TCP connections, one web page "
	      "answer at a time, and TCP only receives segments in order. That is plenty for "
	      "a computer from 1979 talking to the internet of today.</p>\n"
	      "<p><a href=\"/\">status page</a></p></body></html>\n");
}

static void build_stats(void)
{
	sputs(&ps, "HTTP/1.0 200 OK\nContent-Type: text/plain\nConnection: close\n\n");
	sysinfo(&ps);
}

static void build_404(const char *path)
{
	sputs(&ps, "HTTP/1.0 404 Not Found\nContent-Type: text/plain\nConnection: close\n\n");
	sprint(&ps, "%s: not here. This is a PDP-11, it only has /, /stats and /about.\n", path);
}

static void push(struct hreq *r)
{
	u16 n;
	if (!r->sending || !r->t)
		return;
	n = page_len - r->pos;
	if (n)
		r->pos += tcp_write(r->t, (const u8 *)page + r->pos, n);
	if (r->pos >= page_len) {
		r->sending = 0;
		page_owner = NULL;
		tcp_close(r->t);
	}
}

static void respond(struct hreq *r)
{
	char *path, *sp;
	if (page_owner) {
		static const char busy[] =
			"HTTP/1.0 503 Busy\r\nContent-Type: text/plain\r\n\r\nbusy, try again\n";
		tcp_write(r->t, (const u8 *)busy, sizeof busy - 1);
		tcp_close(r->t);
		return;
	}
	http_requests++;
	page_owner = r->t;
	page_len = 0;
	ps.col = 0;
	path = r->line;
	if (strncmp(path, "GET ", 4) && strncmp(path, "HEAD ", 5)) {
		sputs(&ps, "HTTP/1.0 501 Not Implemented\n\n");
	} else {
		path = strncmp(path, "GET ", 4) ? path + 5 : path + 4;
		for (sp = path; *sp && *sp != ' '; sp++)
			;
		*sp = 0;
		if (!strcmp(path, "/") || !strcmp(path, "/index.html"))
			build_index();
		else if (!strcmp(path, "/stats"))
			build_stats();
		else if (!strcmp(path, "/about"))
			build_about();
		else
			build_404(path);
	}
	if (!strncmp(r->line, "HEAD", 4)) {
		/* headers only */
		char *e = page;
		u16 i;
		for (i = 0; i + 3 < page_len; i++, e++)
			if (e[0] == '\r' && e[1] == '\n' && e[2] == '\r' && e[3] == '\n') {
				page_len = i + 4;
				break;
			}
	}
	r->pos = 0;
	r->sending = 1;
	push(r);
}

static void accept_(tcb_t *t)
{
	int i;
	struct hreq *v = NULL;
	for (i = 0; i < NHTTP; i++)
		if (!reqs[i].t) {
			v = &reqs[i];
			break;
		}
	if (!v) {
		/* all taken: drop the one idle longest that never sent a request */
		for (i = 0; i < NHTTP; i++)
			if (!reqs[i].got_line && !reqs[i].sending && (!v || reqs[i].t->idle > v->t->idle))
				v = &reqs[i];
		if (!v) {
			t->appstate = NULL;
			tcp_abort(t);
			return;
		}
		tcp_abort(v->t);        /* calls closed_(), which frees the slot */
	}
	memset(v, 0, sizeof *v);
	v->t = t;
	t->appstate = v;
}

static void recv_(tcb_t *t, u8 *d, u16 len)
{
	struct hreq *r = t->appstate;
	u16 i;
	if (!r || r->sending || r->got_line > 1)
		return;
	for (i = 0; i < len; i++) {
		char c = d[i];
		if (!r->got_line) {
			if (c == '\n') {
				r->line[r->llen] = 0;
				r->got_line = 1;
				r->crlf = 1;
			} else if (c != '\r' && r->llen < sizeof r->line - 1)
				r->line[r->llen++] = c;
			continue;
		}
		/* headers: an empty line ends the request */
		if (c == '\n') {
			if (r->crlf) {
				r->got_line = 2;
				respond(r);
				return;
			}
			r->crlf = 1;
		} else if (c != '\r')
			r->crlf = 0;
	}
}

static void sent_(tcb_t *t)
{
	struct hreq *r = t->appstate;
	if (r)
		push(r);
}

static void closed_(tcb_t *t)
{
	struct hreq *r = t->appstate;
	if (!r)
		return;
	if (page_owner == t)
		page_owner = NULL;
	r->t = NULL;
	t->appstate = NULL;
}

/* browsers open connections they may never use: do not let them sit */
static const tcp_app_t http_app = { 80, accept_, recv_, sent_, closed_, 20 };

void http_init(void)
{
	tcp_listen(&http_app);
}
