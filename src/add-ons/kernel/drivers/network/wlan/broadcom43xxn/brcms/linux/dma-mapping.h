/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	Handing a buffer to the card's own DMA engine.

	Linux maps a buffer into whatever address space the device sees. There
	is no such space here: every buffer the imported sources pass down came
	out of one contiguous allocation, so mapping one is working out where in
	physical memory it already is, and unmapping one is nothing at all.
*/
#ifndef BRCMS_LINUX_DMA_MAPPING_H
#define BRCMS_LINUX_DMA_MAPPING_H


#include "linux/types.h"


// Which way the frame is about to travel. Nothing here has to act on it:
// the memory is coherent, so there are no caches to push or invalidate.
enum dma_data_direction {
	DMA_BIDIRECTIONAL	= 0,
	DMA_TO_DEVICE		= 1,
	DMA_FROM_DEVICE		= 2,
	DMA_NONE			= 3,
};


void* dma_alloc_coherent(struct device* dev, size_t size,
	dma_addr_t* _physical, gfp_t flags);
void dma_free_coherent(struct device* dev, size_t size, void* address,
	dma_addr_t physical);

dma_addr_t dma_map_single(struct device* dev, void* address, size_t size,
	enum dma_data_direction direction);
void dma_unmap_single(struct device* dev, dma_addr_t physical, size_t size,
	enum dma_data_direction direction);

static inline int
dma_mapping_error(struct device* dev, dma_addr_t physical)
{
	return physical == 0;
}


#endif	// BRCMS_LINUX_DMA_MAPPING_H
