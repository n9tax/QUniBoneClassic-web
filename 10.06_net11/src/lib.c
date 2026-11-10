/* lib.c - net11: formatted output, memory and string functions, byte order.
   No C library: everything net11 needs is here. */
#include "hw.h"

void *memcpy(void *d, const void *s, unsigned n)
{
	u8 *dp = d;
	const u8 *sp = s;
	if (!(((unsigned)dp | (unsigned)sp | n) & 1)) {
		u16 *dw = d;
		const u16 *sw = s;
		for (n >>= 1; n; n--)
			*dw++ = *sw++;
		return d;
	}
	while (n--)
		*dp++ = *sp++;
	return d;
}

void *memmove(void *d, const void *s, unsigned n)
{
	u8 *dp = d;
	const u8 *sp = s;
	if (dp <= sp || dp >= sp + n)
		return memcpy(d, s, n);
	dp += n;
	sp += n;
	while (n--)
		*--dp = *--sp;
	return d;
}

void *memset(void *d, int c, unsigned n)
{
	u8 *dp = d;
	while (n--)
		*dp++ = c;
	return d;
}

int memcmp(const void *a, const void *b, unsigned n)
{
	const u8 *p = a, *q = b;
	for (; n; n--, p++, q++)
		if (*p != *q)
			return *p - *q;
	return 0;
}

unsigned strlen(const char *s)
{
	const char *p = s;
	while (*p)
		p++;
	return p - s;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, unsigned n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b)
			return (u8)*a - (u8)*b;
		if (!*a)
			return 0;
	}
	return 0;
}

char *strcpy(char *d, const char *s)
{
	char *r = d;
	while ((*d++ = *s++))
		;
	return r;
}

/* network byte order helpers: never cast packet bytes to u16/u32 */
u16 get16(const u8 *p)
{
	return ((u16)p[0] << 8) | p[1];
}

void put16(u8 *p, u16 v)
{
	p[0] = v >> 8;
	p[1] = v;
}

u32 get32(const u8 *p)
{
	return ((u32)get16(p) << 16) | get16(p + 2);
}

void put32(u8 *p, u32 v)
{
	put16(p, (u16)(v >> 16));
	put16(p + 2, (u16)v);
}

/* ---- formatted output */

void sputc(sink_t *s, char c)
{
	if (c == '\n') {
		s->put(s, '\r');
		s->col = 0;
	} else if (c == '\r')
		s->col = 0;
	else
		s->col++;
	s->put(s, c);
}

void sputs(sink_t *s, const char *str)
{
	while (*str)
		sputc(s, *str++);
}

/* unsigned 32 bit to digits, any base; returns length */
static int utoa32(u32 v, int base, char *buf)
{
	char tmp[12];
	int n = 0, i;
	if (base == 10 && v <= 0xffff) {      /* fast path: 16 bit DIV */
		u16 w = (u16)v;
		do {
			tmp[n++] = '0' + w % 10;
			w /= 10;
		} while (w);
	} else {
		do {
			u32 r;
			int d;
			v = udivmod32(v, (u32)base, &r);
			d = (int)r;
			tmp[n++] = d < 10 ? '0' + d : 'a' + d - 10;
		} while (v);
	}
	for (i = 0; i < n; i++)
		buf[i] = tmp[n - 1 - i];
	buf[n] = 0;
	return n;
}

/* %d %u %x %o %c %s, with l for 32 bit, field width, '-' and '0' flags.
   Also %I (IP address, u8 *) and %M (MAC address, u8 *). */
void sprintf_v(sink_t *s, const char *fmt, va_list ap)
{
	char buf[24];
	const char *str;
	int width, left, zero, lng, len;
	u32 v;

	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			sputc(s, *fmt);
			continue;
		}
		fmt++;
		left = zero = lng = 0;
		width = 0;
		if (*fmt == '-')
			left = 1, fmt++;
		if (*fmt == '0')
			zero = 1, fmt++;
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + *fmt++ - '0';
		if (*fmt == 'l')
			lng = 1, fmt++;
		str = buf;
		switch (*fmt) {
		case 'd':
			if (lng) {
				s32 sv = va_arg(ap, s32);
				if (sv < 0) {
					buf[0] = '-';
					utoa32((u32)-sv, 10, buf + 1);
				} else
					utoa32((u32)sv, 10, buf);
			} else {
				int iv = va_arg(ap, int);
				if (iv < 0) {
					buf[0] = '-';
					utoa32((u32)(u16)-iv, 10, buf + 1);
				} else
					utoa32((u32)iv, 10, buf);
			}
			break;
		case 'u':
		case 'x':
		case 'o':
			v = lng ? va_arg(ap, u32) : (u32)va_arg(ap, unsigned);
			utoa32(v, *fmt == 'u' ? 10 : *fmt == 'x' ? 16 : 8, buf);
			break;
		case 'c':
			buf[0] = (char)va_arg(ap, int);
			buf[1] = 0;
			break;
		case 's':
			str = va_arg(ap, const char *);
			if (!str)
				str = "(null)";
			break;
		case 'I':
			str = ip_str(va_arg(ap, const u8 *));
			break;
		case 'M':
			str = mac_str(va_arg(ap, const u8 *));
			break;
		case '%':
			buf[0] = '%';
			buf[1] = 0;
			break;
		default:
			buf[0] = '?';
			buf[1] = 0;
			break;
		}
		len = strlen(str);
		if (!left)
			for (; len < width; width--)
				sputc(s, zero ? '0' : ' ');
		sputs(s, str);
		if (left)
			for (; len < width; width--)
				sputc(s, ' ');
	}
}

void sprint(sink_t *s, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	sprintf_v(s, fmt, ap);
	va_end(ap);
}

/* print into a buffer */
struct bufctx {
	char *p;
	int left;
};

static void buf_put(sink_t *s, char c)
{
	struct bufctx *b = s->ctx;
	if (c == '\r')
		return;
	if (b->left > 1) {
		*b->p++ = c;
		b->left--;
	}
}

int snprint(char *buf, int size, const char *fmt, ...)
{
	struct bufctx b;
	sink_t s;
	va_list ap;
	b.p = buf;
	b.left = size;
	s.put = buf_put;
	s.ctx = &b;
	s.col = 0;
	va_start(ap, fmt);
	sprintf_v(&s, fmt, ap);
	va_end(ap);
	*b.p = 0;
	return b.p - buf;
}

/* ---- parsing */

int parse_num(const char *s, u16 *v, int base)
{
	u16 r = 0;
	int any = 0;
	for (; *s; s++) {
		int d;
		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (*s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else
			return 0;
		if (d >= base)
			return 0;
		r = r * base + d;
		any = 1;
	}
	*v = r;
	return any;
}

/* "a.b.c.d" -> ip; 1 if ok */
int parse_ip(const char *s, u8 *ip)
{
	int i;
	for (i = 0; i < 4; i++) {
		u16 v = 0;
		int digits = 0;
		while (*s >= '0' && *s <= '9') {
			v = v * 10 + *s++ - '0';
			if (++digits > 3)
				return 0;
		}
		if (!digits || v > 255)
			return 0;
		ip[i] = v;
		if (i < 3 && *s++ != '.')
			return 0;
	}
	return *s == 0;
}

static char strbufs[4][18];
static int strbuf_next;

char *ip_str(const u8 *ip)
{
	char *b = strbufs[strbuf_next++ & 3];
	snprint(b, 18, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
	return b;
}

char *mac_str(const u8 *m)
{
	static const char hex[] = "0123456789abcdef";
	char *b = strbufs[strbuf_next++ & 3], *p = b;
	int i;
	for (i = 0; i < 6; i++) {
		*p++ = hex[hinib(m[i])];
		*p++ = hex[m[i] & 15];
		if (i < 5)
			*p++ = ':';
	}
	*p = 0;
	return b;
}

/* gcc calls __main() at the start of main() on a.out targets (constructors):
   net11 has none */
void __main(void)
{
}
