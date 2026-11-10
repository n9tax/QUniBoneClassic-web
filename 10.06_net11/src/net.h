/* net.h - net11: network interface, IP stack, applications */
#ifndef NET_H
#define NET_H

#include "hw.h"

/* ---- qe.c: DEQNA / DELQA Ethernet controller */
#define ETH_MAX 1514
extern u8 my_mac[6];
extern int qe_present;
extern u16 qe_csr_addr;
int qe_init(void);              /* 0 if no controller */
void qe_poll(void);             /* received frames -> eth_input() */
int qe_send(const u8 *frame, u16 len);   /* 0 on error */
extern u32 qe_rx_frames, qe_tx_frames, qe_tx_errors, qe_rx_errors, qe_reloads;

/* ---- net.c: Ethernet, ARP, IPv4, ICMP, UDP */
extern u8 my_ip[4], my_mask[4], my_gw[4], my_dns[4];
extern int ip_up;               /* address configured */
extern char my_hostname[16];
extern u32 ip_rx, ip_tx, icmp_rx, udp_rx, tcp_rx, arp_rx, eth_drop;

void eth_input(u8 *frame, u16 len);
u16 ip_cksum(u16 sum, const u8 *p, u16 n);   /* ones complement sum, not inverted */
u16 ip_pseudo(const u8 *src, const u8 *dst, u8 proto, u16 len);  /* pseudo header sum */

/* Build an outgoing IP packet: ip_payload() points to where the transport
   header goes (in the shared transmit frame); ip_send() adds the IP and
   Ethernet headers and sends. ip_send returns 0 if the hardware address of
   the next hop is not known yet (an ARP request went out: try again). */
u8 *ip_payload(void);
int ip_send(const u8 *dst, u8 proto, u16 len);
#define IP_ICMP 1
#define IP_TCP 6
#define IP_UDP 17

/* UDP */
typedef void (*udp_handler_t)(const u8 *src_ip, u16 src_port, u16 dst_port, u8 *data, u16 len);
int udp_bind(u16 port, udp_handler_t h);
u8 *udp_payload(void);
int udp_send(const u8 *dst, u16 sport, u16 dport, u16 len);

/* ARP */
void arp_show(sink_t *s);
int arp_lookup(const u8 *ip, u8 *mac);
void net_timer(void);           /* once a second */

/* ping */
void ping_start(sink_t *s, const u8 *ip, u16 count);
void ping_stop(void);
int ping_active(void);
void ping_poll(void);
void ping_cancel(sink_t *s);

/* ---- tcp.c */
#define TCP_MAXCONN 6
#define TCP_TXBUF 1536
struct tcb;
typedef struct tcp_app {
	u16 port;
	void (*accept)(struct tcb *t);
	void (*recv)(struct tcb *t, u8 *data, u16 len);
	void (*sent)(struct tcb *t);        /* buffer space became free */
	void (*closed)(struct tcb *t);      /* connection gone */
	u16 idle_limit;                     /* seconds without traffic before reset */
} tcp_app_t;

typedef struct tcb {
	u8 state;
	u8 rip[4];
	u16 rport, lport;
	u32 snd_una, snd_nxt, rcv_nxt, iss;
	u16 snd_wnd, mss;
	u16 txlen;                  /* bytes in tx[], first one is snd_una */
	u8 fin_pending, ack_now, retries, close_after;
	u8 got_data;                /* the peer has sent something */
	u16 rto, rtimer;            /* ticks */
	u16 idle;                   /* seconds without traffic */
	const tcp_app_t *app;
	void *appstate;
	u8 tx[TCP_TXBUF];
} tcb_t;

enum { TCP_CLOSED, TCP_SYN_RCVD, TCP_ESTABLISHED, TCP_FIN_WAIT1, TCP_FIN_WAIT2,
	TCP_CLOSING, TCP_TIME_WAIT, TCP_CLOSE_WAIT, TCP_LAST_ACK };

extern tcb_t tcbs[TCP_MAXCONN];
void tcp_listen(const tcp_app_t *app);
void tcp_input(const u8 *src, u8 *seg, u16 len);
u16 tcp_space(tcb_t *t);
u16 tcp_write(tcb_t *t, const u8 *data, u16 len);   /* bytes taken */
void tcp_close(tcb_t *t);
void tcp_abort(tcb_t *t);
void tcp_poll(void);            /* every tick: timers and output */
void tcp_timer(void);           /* once a second */
void tcp_show(sink_t *s);
extern const char *tcp_state_names[];
extern u32 tcp_conns, tcp_retrans;
extern int tcp_debug;

/* a sink that writes into a TCP connection */
void tcp_sink(sink_t *s, tcb_t *t);

/* ---- dhcp.c */
void dhcp_start(void);
void dhcp_timer(void);          /* once a second */
extern int dhcp_state;
extern u32 dhcp_lease;
extern u8 dhcp_server[4];
void dhcp_show(sink_t *s);
void ip_static(const u8 *ip, const u8 *mask, const u8 *gw, const u8 *dns);

/* ---- dns.c */
typedef void (*dns_cb_t)(void *ctx, const char *name, const u8 *ip);   /* ip NULL: failed */
int dns_lookup(const char *name, dns_cb_t cb, void *ctx);
void dns_timer(void);

/* ---- sntp.c */
extern int time_valid;
extern u32 time_base;           /* Unix time at uptime time_base_up */
extern u32 time_base_up;
void sntp_start(void);
void sntp_timer(void);
u32 unix_time(void);
void fmt_time(char *buf, u32 t);        /* "2026-11-09 22:15:03 UTC" */

/* ---- http.c / telnet.c / monitor.c */
void http_init(void);
void telnet_init(void);
extern u32 http_requests;

typedef struct mon {
	sink_t *out;
	char line[80];
	u8 len;
	u8 is_console;
	u8 quit;
	u8 esc;
} mon_t;
void mon_init(mon_t *m, sink_t *out, int is_console);
void mon_input(mon_t *m, int c);
void mon_prompt(mon_t *m);
void banner(sink_t *s);
void sysinfo(sink_t *s);
void mon_forget(sink_t *s);

/* sequence number compare: a < b in TCP sequence space (no signed 32 bit compare!) */
#define SEQ_LT(a, b) (((u16)(((a) - (b)) >> 16)) & 0x8000)
#define SEQ_LE(a, b) (SEQ_LT(a, b) || (a) == (b))

#endif
