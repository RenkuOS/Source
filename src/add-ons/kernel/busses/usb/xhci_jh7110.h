/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef XHCI_JH7110_H
#define XHCI_JH7110_H


#include <device_manager.h>


/*!	Brings up the USB controller of the StarFive JH7110 described by the
	"starfive,jh7110-usb" device tree node \a glue and returns where its xHCI
	registers are and its interrupt.
*/
status_t jh7110_usb_init(device_node* glue, phys_addr_t* _address,
	uint64* _length, uint32* _irq);


#endif	// XHCI_JH7110_H
