/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _DW_AXI_DMA_H
#define _DW_AXI_DMA_H


#include <device_manager.h>


/*!	Synopsys DesignWare AXI DMA controller, for device tree consumers
	("dmas"/"dma-names" with one cell, the hardware handshake number).

	A consumer passes its own FDT device node and the name of the "dmas"
	entry and gets a channel. A cyclic transfer moves a ring of
	\c periodCount periods to or from one device register; the callback runs
	in interrupt context after each period, with the index of the period
	the controller works on now. It must not block.
*/

typedef struct dw_axi_dma_channel dw_axi_dma_channel;

typedef void (*dw_axi_dma_callback)(void* cookie, uint32 currentPeriod);

enum {
	DW_AXI_DMA_MEM_TO_DEV	= 0,
	DW_AXI_DMA_DEV_TO_MEM	= 1,
};

typedef struct dw_axi_dma_cyclic {
	uint32				direction;
	phys_addr_t			buffer;			// 4 byte aligned
	size_t				periodSize;		// bytes, multiple of deviceWidth
	uint32				periodCount;
	phys_addr_t			deviceAddress;
	uint32				deviceWidth;	// bytes per device access: 1, 2 or 4
	dw_axi_dma_callback	callback;
	void*				cookie;
} dw_axi_dma_cyclic;


typedef struct dw_axi_dma_module_info {
	module_info info;

	status_t	(*acquire_channel)(device_node* consumerFdtNode,
					const char* name, dw_axi_dma_channel** _channel);
	void		(*release_channel)(dw_axi_dma_channel* channel);

	status_t	(*start_cyclic)(dw_axi_dma_channel* channel,
					const dw_axi_dma_cyclic* transfer);
	void		(*stop)(dw_axi_dma_channel* channel);

	// error interrupt bits seen since the start (0: none)
	uint32		(*errors)(dw_axi_dma_channel* channel);
} dw_axi_dma_module_info;


#define DW_AXI_DMA_MODULE_NAME "generic/dw_axi_dma/v1"


#endif	// _DW_AXI_DMA_H
