/* hw.h - net11: PDP-11 hardware, basic types, shared declarations */
#ifndef HW_H
#define HW_H

#include <stdarg.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;
typedef short s16;
typedef long s32;

#define NULL ((void *)0)

#define REG(a) (*(volatile u16 *)(a))
#define REGB(a) (*(volatile u8 *)(a))

/* console DL11 */
#define DL_RCSR REG(0177560)
#define DL_RBUF REG(0177562)
#define DL_XCSR REG(0177564)
#define DL_XBUF REG(0177566)
/* line clock KW11-L */
#define LKS_ADDR 0177546
#define LKS REG(LKS_ADDR)

/* crt0.S */
extern volatile u16 ticks;
void clk_isr(void);
int probe(volatile u16 *addr);
void spl7(void);
void spl0(void);
void halt_cpu(void);
void wait_intr(void);
extern char trap_stubs[];

/* clock.c */
extern u16 hz;                  /* ticks per second, 60 or 50 */
extern int have_clock;
extern u32 uptime_s;            /* seconds since start */
u16 now(void);                  /* ticks, wraps */
void clock_init(void);
void clock_poll(void);          /* keeps uptime_s */
int elapsed(u16 since, u16 ticks_);  /* 1 if ticks_ have passed since "since" */

/* output sinks: console or a telnet connection */
typedef struct sink {
	void (*put)(struct sink *s, char c);
	void *ctx;
	int col;
} sink_t;
extern sink_t con;              /* the console */
void sputc(sink_t *s, char c);
void sputs(sink_t *s, const char *str);
void sprintf_v(sink_t *s, const char *fmt, va_list ap);
void sprint(sink_t *s, const char *fmt, ...);
#define cprint(...) sprint(&con, __VA_ARGS__)
int snprint(char *buf, int size, const char *fmt, ...);

/* high nibble of a byte. gcc 13 compiles "unsigned char >> n" as a LEFT
   shift (ash $n instead of ash $-n); the empty asm keeps the value a word. */
static inline u16 hinib(u8 c)
{
	u16 v = c;
	__asm__("" : "+r"(v));
	return v >> 4;
}

/* lib32.S: 32 bit division (libgcc's is miscompiled, see lib32.S).
   Never compare signed 32 bit values with < > <= >= : gcc gets it wrong. */
u32 udivmod32(u32 a, u32 b, u32 *rem);

/* lib.c */
void *memcpy(void *d, const void *s, unsigned n);
void *memmove(void *d, const void *s, unsigned n);
void *memset(void *d, int c, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);
unsigned strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, unsigned n);
char *strcpy(char *d, const char *s);
int parse_ip(const char *s, u8 *ip);
int parse_num(const char *s, u16 *v, int base);
char *ip_str(const u8 *ip);     /* static buffer, 4 in rotation */
char *mac_str(const u8 *mac);
u16 get16(const u8 *p);
void put16(u8 *p, u16 v);
u32 get32(const u8 *p);
void put32(u8 *p, u32 v);

/* console.c */
void con_init(void);
int con_getc(void);             /* -1 if none */

#endif
