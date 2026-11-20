/*
 * Copyright 2021 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_BOOT_UART_H
#define KERNEL_BOOT_UART_H


#include <boot/addr_range.h>
#include <SupportDefs.h>


typedef struct {
	char kind[32];
	addr_range regs;
	uint32 irq;
	int64 clock;
	uint8 reg_shift;
		// registers are (1 << reg_shift) bytes apart ("reg-shift")
	uint8 reg_io_width;
		// register access width in bytes ("reg-io-width"), 0 if unknown
} __attribute__((packed)) uart_info;


#endif /* KERNEL_BOOT_UART_H */
