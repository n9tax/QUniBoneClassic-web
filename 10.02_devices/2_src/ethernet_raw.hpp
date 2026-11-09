/* ethernet_raw.hpp: send and receive raw Ethernet frames on a Linux interface
   (AF_PACKET socket), for emulated network controllers.

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
 IMPLIED.

 The interface is put into promiscuous mode, so the PDP-11 can use its own
 station address (and DECnet's AA-00-04-.. address) on the same cable as Linux.
 Needs root (CAP_NET_RAW).

 Limitation: frames between Linux on the BeagleBone and the PDP-11 are not
 passed: a frame sent on an AF_PACKET socket is not delivered to the local
 IP stack, and the local stack's frames do not reach the wire for us to see.
 All other hosts on the LAN (and the internet, via the router) can be reached.
 */
#ifndef _ETHERNET_RAW_HPP_
#define _ETHERNET_RAW_HPP_

#include <stdint.h>
#include <string>

class ethernet_raw_c {
public:
	ethernet_raw_c();
	~ethernet_raw_c();

	// open interface, e.g. "eth0". false: error text in "error"
	bool open(const std::string &ifname, std::string &error);
	void close(void);
	bool is_open(void) {
		return fd >= 0;
	}
	// hardware address of the Linux interface
	bool get_host_mac(uint8_t *mac);

	// receive one frame addressed to anybody but sent by someone else.
	// Returns length, 0 on timeout or for our own transmissions, -1 on error.
	int receive(uint8_t *buffer, unsigned buffer_size, unsigned timeout_ms);
	bool send(const uint8_t *frame, unsigned len);

	std::string ifname;
	bool gro_disabled; // generic receive offload switched off by us

private:
	int fd;
	int ifindex;
	bool disable_gro(std::string &info);
};

#endif
