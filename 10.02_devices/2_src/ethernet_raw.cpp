/* ethernet_raw.cpp: raw Ethernet frames on a Linux interface (AF_PACKET).

 Copyright (c) 2026, the QUniBone contributors
 Permission is hereby granted, free of charge, to any person obtaining a
 copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:
 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED. See ethernet_raw.hpp.
 */

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>

#include "ethernet_raw.hpp"

ethernet_raw_c::ethernet_raw_c() {
	fd = -1;
	ifindex = 0;
	gro_disabled = false;
}

ethernet_raw_c::~ethernet_raw_c() {
	close();
}

bool ethernet_raw_c::open(const std::string &_ifname, std::string &error) {
	close();
	ifname = _ifname;
	if (ifname.empty() || ifname.size() >= IFNAMSIZ) {
		error = "bad interface name \"" + ifname + "\"";
		return false;
	}
	fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd < 0) {
		error = std::string("socket(AF_PACKET): ") + strerror(errno) + " (needs root)";
		return false;
	}
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
		error = "interface " + ifname + ": " + strerror(errno);
		close();
		return false;
	}
	ifindex = ifr.ifr_ifindex;

	struct sockaddr_ll sll;
	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_ALL);
	sll.sll_ifindex = ifindex;
	if (bind(fd, (struct sockaddr *) &sll, sizeof(sll)) < 0) {
		error = "bind " + ifname + ": " + strerror(errno);
		close();
		return false;
	}

	// promiscuous: frames for the PDP-11's own addresses
	struct packet_mreq mr;
	memset(&mr, 0, sizeof(mr));
	mr.mr_ifindex = ifindex;
	mr.mr_type = PACKET_MR_PROMISC;
	if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr)) < 0) {
		error = "promiscuous mode on " + ifname + ": " + strerror(errno);
		close();
		return false;
	}

	// bigger receive buffer: the PDP-11 may be slow to fetch frames
	int rcvbuf = 512 * 1024;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

	std::string info;
	gro_disabled = disable_gro(info);
	return true;
}

void ethernet_raw_c::close(void) {
	if (fd >= 0)
		::close(fd); // also ends promiscuous membership
	fd = -1;
}

// GRO merges consecutive TCP segments into one big frame before packet
// sockets see them. The PDP-11 can only take real Ethernet frames.
bool ethernet_raw_c::disable_gro(std::string &info) {
	struct ifreq ifr;
	struct ethtool_value ev;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
	ev.cmd = ETHTOOL_SGRO;
	ev.data = 0;
	ifr.ifr_data = (char *) &ev;
	if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) {
		info = std::string("could not disable GRO: ") + strerror(errno);
		return false;
	}
	return true;
}

bool ethernet_raw_c::get_host_mac(uint8_t *mac) {
	if (fd < 0)
		return false;
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0)
		return false;
	memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
	return true;
}

int ethernet_raw_c::receive(uint8_t *buffer, unsigned buffer_size, unsigned timeout_ms) {
	if (fd < 0)
		return -1;
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	int r = poll(&pfd, 1, (int) timeout_ms);
	if (r < 0)
		return errno == EINTR ? 0 : -1;
	if (r == 0)
		return 0;
	struct sockaddr_ll from;
	socklen_t fromlen = sizeof(from);
	ssize_t n = recvfrom(fd, buffer, buffer_size, MSG_TRUNC, (struct sockaddr *) &from,
			&fromlen);
	if (n < 0)
		return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
	// a packet socket also sees what this host (and the PDP-11) sends
	if (from.sll_pkttype == PACKET_OUTGOING)
		return 0;
	return (int) n; // may be > buffer_size if truncated: caller drops it
}

bool ethernet_raw_c::send(const uint8_t *frame, unsigned len) {
	if (fd < 0)
		return false;
	ssize_t n = ::send(fd, frame, len, 0);
	return n == (ssize_t) len;
}
