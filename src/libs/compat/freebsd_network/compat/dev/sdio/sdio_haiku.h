/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */
#ifndef _FBSD_COMPAT_DEV_SDIO_SDIO_HAIKU_H_
#define _FBSD_COMPAT_DEV_SDIO_SDIO_HAIKU_H_


/*!	SDIO functions for FreeBSD style drivers on Haiku. A driver for the
	"sdio" bus gets a struct sdio_attach_args as the ivars of its device.
	All calls sleep and must not be made from interrupt context; the
	interrupt handler of sdio_setup_intr() runs in a thread.
*/


#include <sys/types.h>


struct sdio_func;

struct sdio_attach_args {
	uint16_t			vendor;
	uint16_t			device;
	uint8_t				function;
	uint8_t				class_code;
	struct sdio_func*	func;
};


#ifdef __cplusplus
extern "C" {
#endif

struct sdio_func* sdio_get_function(device_t dev);

/* CMD52 on the register space of the function, and of function 0 (CCCR) */
uint8_t	sdio_read_1(struct sdio_func* func, uint32_t addr, int* err);
void	sdio_write_1(struct sdio_func* func, uint32_t addr, uint8_t value,
			int* err);
uint8_t	sdio_f0_read_1(struct sdio_func* func, uint32_t addr, int* err);
void	sdio_f0_write_1(struct sdio_func* func, uint32_t addr, uint8_t value,
			int* err);

/* CMD53, split into block and byte mode transfers as needed */
uint16_t sdio_read_2(struct sdio_func* func, uint32_t addr, int* err);
void	sdio_write_2(struct sdio_func* func, uint32_t addr, uint16_t value,
			int* err);
uint32_t sdio_read_4(struct sdio_func* func, uint32_t addr, int* err);
void	sdio_write_4(struct sdio_func* func, uint32_t addr, uint32_t value,
			int* err);
int		sdio_read_bytes(struct sdio_func* func, uint32_t addr, void* buffer,
			size_t length, bool incrementAddress);
int		sdio_write_bytes(struct sdio_func* func, uint32_t addr,
			const void* buffer, size_t length, bool incrementAddress);

int		sdio_enable_func(struct sdio_func* func);
int		sdio_disable_func(struct sdio_func* func);
int		sdio_set_block_size(struct sdio_func* func, uint16_t size);
uint16_t sdio_get_block_size(struct sdio_func* func);

/* The handler runs in a thread; the card interrupt is enabled again when it
 * returns, so it must clear the interrupt source in the function. */
int		sdio_setup_intr(struct sdio_func* func, void (*handler)(void*),
			void* arg);
void	sdio_teardown_intr(struct sdio_func* func);

#ifdef __cplusplus
}
#endif


#endif	/* _FBSD_COMPAT_DEV_SDIO_SDIO_HAIKU_H_ */
