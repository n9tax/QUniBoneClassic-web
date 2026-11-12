/* delqa.cpp: DEC DELQA (M7516) QBUS Ethernet controller for QBone.

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
 IMPLIED. See delqa.hpp.

 Threads:
 - on_after_register_access(): PRU event, QBUS stalled until it returns.
   Only hands the written value to the core (short lock, maybe INTR).
 - worker 0: descriptor lists and DMA (core.service()).
 - worker 1: frames from the Linux interface into the core's receive queue.
 */

#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "logger.hpp"
#include "qunibus.h"
#include "qunibusadapter.hpp"
#include "qunibusdevice.hpp"
#include "delqa.hpp"

delqa_c::delqa_c() :
		qunibusdevice_c(), core(this) {
	name.value = "delqa";
	type_name.value = "DELQA";
	log_label = "delqa";
	current_vector = 0;

	// CSR 17774440, slot, vector (programmed by the driver via VAR), BR4
	set_default_bus_params(017774440, 18, 0120, 4);

	register_count = 8;
	static const char *names[8] =
			{ "SA0", "SA1", "SA2_RBDL", "SA3_RBDH", "SA4_TBDL", "SA5_TBDH", "VAR", "CSR" };
	for (unsigned i = 0; i < 8; i++) {
		reg[i] = &(this->registers[i]);
		strcpy(reg[i]->name, names[i]);
		reg[i]->active_on_dati = false; // reads need no logic
		// station address 0,1 are ROM; all others keep separate read/write values
		reg[i]->active_on_dato = (i >= 2);
		reg[i]->writable_bits = (i >= 2) ? 0xffff : 0;
		reg[i]->reset_value = 0;
	}
	reg[7]->reset_value = DELQA_CSR_XL | DELQA_CSR_RL;

	ifname.value = "eth0";
	macaddr.value = "";
	set_workers_count(2);
}

delqa_c::~delqa_c() {
	eth.close();
}

bool delqa_c::parse_mac(const std::string &text, uint8_t *mac) {
	unsigned v[6];
	char sep[5];
	if (sscanf(text.c_str(), "%2x%c%2x%c%2x%c%2x%c%2x%c%2x", &v[0], &sep[0], &v[1], &sep[1],
			&v[2], &sep[2], &v[3], &sep[3], &v[4], &sep[4], &v[5]) != 11)
		return false;
	for (unsigned i = 0; i < 5; i++)
		if (sep[i] != ':' && sep[i] != '-')
			return false;
	for (unsigned i = 0; i < 6; i++)
		mac[i] = (uint8_t) v[i];
	return !(mac[0] & 1); // no multicast station address
}

bool delqa_c::on_param_changed(parameter_c *param) {
	if (param == &priority_slot) {
		dma_request.set_priority_slot(priority_slot.new_value);
		intr_request.set_priority_slot(priority_slot.new_value);
	} else if (param == &intr_level) {
		intr_request.set_level(intr_level.new_value);
	} else if (param == &intr_vector) {
		intr_request.set_vector(intr_vector.new_value);
		current_vector = intr_vector.new_value;
	} else if (param == &ifname || param == &macaddr) {
		if (enabled.value) {
			ERROR("Disable delqa before changing %s", param->name.c_str());
			return false;
		}
		if (param == &macaddr && !macaddr.new_value.empty()) {
			uint8_t m[6];
			if (!parse_mac(macaddr.new_value, m)) {
				ERROR("MAC address must look like 08:00:2b:12:34:56 (unicast)");
				return false;
			}
		}
	}
	return qunibusdevice_c::on_param_changed(param); // enable: install, workers
}

// "en delqa": open the network interface, set the station address
bool delqa_c::on_before_install(void) {
	std::string error;
	if (!eth.open(ifname.value, error)) {
		ERROR("delqa: %s", error.c_str());
		return false;
	}
	uint8_t mac[6];
	if (macaddr.value.empty()) {
		// DEC prefix 08-00-2B, rest from the BeagleBone's address: unique on the LAN
		uint8_t host[6] = { 0, 0, 0, 0x11, 0x23, 0x01 };
		eth.get_host_mac(host);
		mac[0] = 0x08;
		mac[1] = 0x00;
		mac[2] = 0x2b;
		mac[3] = host[3];
		mac[4] = host[4];
		mac[5] = host[5];
		char buf[20];
		snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
				mac[3], mac[4], mac[5]);
		macaddr.value = buf;
	} else if (!parse_mac(macaddr.value, mac)) {
		ERROR("delqa: bad MAC address %s", macaddr.value.c_str());
		eth.close();
		return false;
	}
	core.set_mac(mac); // not installed yet: publish() ignores registers
	for (unsigned i = 0; i < 6; i++)
		reg[i]->reset_value = 0xff00 | mac[i];
	reg[7]->reset_value = DELQA_CSR_XL | DELQA_CSR_RL | DELQA_CSR_OK;
	core.set_link(true);
	INFO("delqa: interface %s, station address %s%s", ifname.value.c_str(),
			macaddr.value.c_str(), eth.gro_disabled ? ", GRO disabled" : "");
	printf("DELQA on %s, station address %s. The BeagleBone itself can not talk to the PDP-11"
			" over this interface; all other hosts can.\n", ifname.value.c_str(),
			macaddr.value.c_str());
	return true;
}

void delqa_c::on_after_uninstall(void) {
	core.set_link(false);
	eth.close();
	INFO("delqa: interface %s closed", ifname.value.c_str());
}

void delqa_c::on_after_register_access(qunibusdevice_register_t *device_reg,
		uint8_t unibus_control, DATO_ACCESS access) {
	if (unibus_control != QUNIBUS_CYCLE_DATO)
		return;
	if (qunibusadapter->line_INIT)
		return;
	delqa_core_c::access_e a = delqa_core_c::ACCESS_WORD;
	if (access == DATO_BYTEL)
		a = delqa_core_c::ACCESS_LOW_BYTE;
	else if (access == DATO_BYTEH)
		a = delqa_core_c::ACCESS_HIGH_BYTE;
	core.bus_write(device_reg->index, get_register_dato_value(device_reg), a);
}

// after QBUS install, device is reset by DCLO/DCOK cycle
void delqa_c::on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) {
	UNUSED(aclo_edge);
	UNUSED(dclo_edge);
	core.bus_reset();
}

void delqa_c::on_init_changed(void) {
	if (init_asserted)
		core.bus_reset();
}

void delqa_c::update_stats(void) {
	tx_frames.value = (unsigned) core.stat_tx_frames;
	rx_frames.value = (unsigned) core.stat_rx_frames;
	rx_dropped.value = (unsigned) core.stat_rx_dropped;
}

// A worker must never loop without sleeping: it runs at realtime priority on
// a single core BeagleBone, and a busy loop there starves Linux (ssh, the web
// panel). If a pass did nothing and took less than a millisecond, sleep a
// millisecond, and report it when it happens often.
void delqa_c::idle_guard(idle_guard_t &g, bool progress, uint64_t pass_ns, const char *what) {
	if (progress || pass_ns >= 1000000)
		return;
	timeout_c::wait_ms(1);
	g.count++;
	if (g.window.elapsed_ms() >= 1000) {
		if (g.count > 100)
			WARNING("delqa: %s woke %u times in a second with nothing to do (%s)", what,
					g.count, core.state_text().c_str());
		g.count = 0;
		g.window.start_ms(0);
	}
}

void delqa_c::worker(unsigned instance) {
	idle_guard_t guard;
	guard.count = 0;
	guard.window.start_ms(0);
	timeout_c pass;
	if (instance == 0) {
		// controller: DMA and interrupts, realtime
		worker_init_realtime_priority(rt_device);
		while (!workers_terminate) {
			pass.start_ms(0);
			bool did = false;
			if (core.wait_for_work(100) && !qunibusadapter->line_INIT)
				did = core.service();
			update_stats();
			idle_guard(guard, did, pass.elapsed_ns(), "controller");
		}
	} else {
		// network receive: frames wait in the socket buffer and the queue, so
		// normal priority is enough, and it keeps clear of ssh and the panel
		worker_init_realtime_priority(none_rt);
		static uint8_t buffer[2048 + 64];
		while (!workers_terminate) {
			pass.start_ms(0);
			int n = eth.receive(buffer, sizeof(buffer), 100);
			if (n < 0) {
				timeout_c::wait_ms(100); // interface closed or down
				continue;
			}
			if (n > (int) sizeof(buffer)) // truncated: count as oversize
				n = sizeof(buffer);
			if (n > 0)
				core.receive_frame(buffer, (unsigned) n);
			idle_guard(guard, n > 0, pass.elapsed_ns(), "receiver");
		}
	}
}

// ---------------------------------------------------------------------------
// delqa_bus_if

bool delqa_c::dma_read(uint32_t addr, uint16_t *buffer, unsigned wordcount) {
	qunibusadapter->DMA(dma_request, true, QUNIBUS_CYCLE_DATI, addr, buffer, wordcount);
	return dma_request.success;
}

bool delqa_c::dma_write(uint32_t addr, uint16_t *buffer, unsigned wordcount) {
	qunibusadapter->DMA(dma_request, true, QUNIBUS_CYCLE_DATO, addr, buffer, wordcount);
	return dma_request.success;
}

// called with the core's lock held, from a worker or the register callback
void delqa_c::publish(const uint16_t *dati, uint16_t vector, bool intr_raise, bool intr_cancel) {
	if (handle == 0)
		return; // not installed: no PRU registers yet
	for (unsigned i = 0; i < 7; i++)
		set_register_dati_value(reg[i], dati[i], __func__);
	if (vector != current_vector && vector != 0) {
		// The driver programs the vector via VAR. A request with the old vector
		// must not stay pending (the adapter matches requests by vector).
		qunibusadapter->cancel_INTR(intr_request);
		intr_request.set_vector(vector);
		current_vector = vector;
	}
	if (intr_raise)
		qunibusadapter->INTR(intr_request, reg[7], dati[7]); // CSR atomic with INTR
	else {
		if (intr_cancel)
			qunibusadapter->cancel_INTR(intr_request);
		set_register_dati_value(reg[7], dati[7], __func__);
	}
}

bool delqa_c::transmit(const uint8_t *frame, unsigned len) {
	return eth.send(frame, len);
}

bool delqa_c::in_init(void) {
	return qunibusadapter->line_INIT;
}

void delqa_c::log(int level, const char *msg) {
	switch (level) {
	case 1:
		ERROR("%s", msg);
		break;
	case 2:
		WARNING("%s", msg);
		break;
	case 3:
		INFO("%s", msg);
		break;
	default:
		DEBUG("%s", msg);
	}
}
