/* delqa.hpp: DEC DELQA (M7516) QBUS Ethernet controller for QBone.

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

 QUniBone glue for delqa_core_c: QBUS registers, DMA, interrupt, and the
 BeagleBone's Ethernet interface.

 demo device menu:
   sd delqa
   p interface eth0          Linux interface (before "en")
   p mac 08:00:2b:12:34:56   station address (before "en"; default from eth0)
   en delqa
 Defaults: CSR 17774440, vector programmed by the driver (usually 120), BR4.
 */
#ifndef _DELQA_HPP_
#define _DELQA_HPP_

#include <stdint.h>
#include "qunibusdevice.hpp"
#include "delqa_core.hpp"
#include "ethernet_raw.hpp"

class delqa_c: public qunibusdevice_c, public delqa_bus_if {
private:
	delqa_core_c core;
	ethernet_raw_c eth;

	dma_request_c dma_request = dma_request_c(this);
	intr_request_c intr_request = intr_request_c(this);
	uint16_t current_vector;

	qunibusdevice_register_t *reg[8];

	bool parse_mac(const std::string &text, uint8_t *mac);
	void update_stats(void);

public:
	parameter_string_c ifname = parameter_string_c(this, "interface", "if", false,
			"Linux network interface, set before enable");
	parameter_string_c macaddr = parameter_string_c(this, "mac", "mac", false,
			"station address, e.g. 08:00:2b:12:34:56. Empty: 08:00:2b + end of interface address");
	parameter_unsigned_c tx_frames = parameter_unsigned_c(this, "tx_frames", "txf", true, "",
			"%u", "frames sent", 32, 10);
	parameter_unsigned_c rx_frames = parameter_unsigned_c(this, "rx_frames", "rxf", true, "",
			"%u", "frames received", 32, 10);
	parameter_unsigned_c rx_dropped = parameter_unsigned_c(this, "rx_dropped", "rxd", true, "",
			"%u", "received frames dropped (no buffers, too long)", 32, 10);

	delqa_c();
	~delqa_c();

	bool on_param_changed(parameter_c *param) override;
	bool on_before_install(void) override;
	void on_after_uninstall(void) override;

	void on_after_register_access(qunibusdevice_register_t *device_reg, uint8_t unibus_control,
			DATO_ACCESS access) override;
	void on_power_changed(signal_edge_enum aclo_edge, signal_edge_enum dclo_edge) override;
	void on_init_changed(void) override;

	// 0: controller (descriptor lists, DMA), 1: network receiver
	void worker(unsigned instance) override;

	// delqa_bus_if
	bool dma_read(uint32_t addr, uint16_t *buffer, unsigned wordcount) override;
	bool dma_write(uint32_t addr, uint16_t *buffer, unsigned wordcount) override;
	void publish(const uint16_t *dati, uint16_t vector, bool intr_raise, bool intr_cancel)
			override;
	bool transmit(const uint8_t *frame, unsigned len) override;
	bool in_init(void) override;
	void log(int level, const char *msg) override;
};

#endif
