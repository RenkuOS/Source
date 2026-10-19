/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	Memory the chip can reach on its own.

	The descriptor rings and every frame buffer are read and written by the
	card's own DMA engine, which is given physical addresses and knows
	nothing of this kernel's page tables. Linux hands brcmsmac a mapping
	layer for that; here there is none, so both come out of areas locked
	into physically contiguous memory, and each buffer carries the address
	the engine should be given.

	The frame buffers are one allocation carved into equal slices rather
	than a heap: a slice's physical address is then its offset from a base
	that was worked out once, and nothing has to be looked up per frame.
*/


#include "linux/types.h"

#include <KernelExport.h>
#include <OS.h>

#include "glue.h"
#include "linux/dma-mapping.h"
#include "linux/skbuff.h"


#define TRACE(x...)	dprintf("broadcom43xxn: " x)

// What one frame buffer holds. The ring asks for RXBUFSZ plus the room the
// chip wants in front for its own receive header, which is 2048 + 38, so a
// page a slice both covers it and keeps every buffer page aligned.
#ifndef ROUNDUP
#	define ROUNDUP(a, b)	(((a) + ((b) - 1)) & ~((b) - 1))
#endif

#define BUFFER_SIZE			4096

// Enough that a burst has somewhere to land. Of these, NRXBUFPOST sit in
// the receive ring and the rest are in flight through this driver and the
// stack above; the counters said sixty two of sixty four at once while a
// download was running, which is a pool about to run out.
#define BUFFER_COUNT		768


struct dma_area {
	area_id			area;
	void*			address;
	phys_addr_t		physical;
	size_t			size;

	// Where inside the area the caller was pointed, which is what it hands
	// back when it is done with it.
	void*			handedOut;
};


// The descriptor rings. There are at most a handful and they last as long
// as the driver, so they are tracked in a fixed table rather than a list.
#define MAX_RINGS			16

static struct dma_area* sRings[MAX_RINGS];
static uint32 sRingCount;
static uint32 sRingsRetried;

static struct dma_area sBufferPool;
static struct sk_buff sBuffers[BUFFER_COUNT];
static struct sk_buff_head sFreeBuffers;
// Disabling interrupts only excludes this CPU. RX and TX use this
// shared pool from different CPUs.
static spinlock sBufferLock = B_SPINLOCK_INITIALIZER;
static uint32 sBuffersInUse;
static uint32 sBufferHighWater;
static uint32 sBufferFailures;


/*!	Locks down \a size bytes the card can reach and says where they are. */
static status_t
allocate_dma_area(struct dma_area* out, size_t size, const char* name)
{
	size = ROUNDUP(size, B_PAGE_SIZE);

	out->area = create_area(name, &out->address, B_ANY_KERNEL_ADDRESS, size,
		B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (out->area < B_OK)
		return out->area;

	physical_entry entry;
	status_t status = get_memory_map(out->address, size, &entry, 1);
	if (status != B_OK) {
		delete_area(out->area);
		out->area = -1;
		return status;
	}

	out->physical = entry.address;
	out->size = size;
	memset(out->address, 0, size);

	return B_OK;
}


static void
free_dma_area(struct dma_area* which)
{
	if (which->area >= B_OK)
		delete_area(which->area);

	which->area = -1;
	which->address = NULL;
	which->physical = 0;
	which->size = 0;
}


status_t
brcms_glue_dma_init(void)
{
	if (sBufferPool.area > B_OK)
		return B_OK;

	status_t status = allocate_dma_area(&sBufferPool,
		(size_t)BUFFER_SIZE * BUFFER_COUNT, "broadcom43xxn frames");
	if (status != B_OK) {
		TRACE("dma: cannot lock down %d frame buffers: %d\n", BUFFER_COUNT,
			(int)status);
		return status;
	}

	skb_queue_head_init(&sFreeBuffers);
	for (int i = 0; i < BUFFER_COUNT; i++) {
		struct sk_buff* skb = &sBuffers[i];
		skb->head = (unsigned char*)sBufferPool.address + i * BUFFER_SIZE;
		skb->physical = sBufferPool.physical + (phys_addr_t)i * BUFFER_SIZE;
		skb->end = skb->head + BUFFER_SIZE;
		skb->data = skb->head;
		skb->tail = skb->head;
		skb->len = 0;
		skb_queue_tail(&sFreeBuffers, skb);
	}

	sBuffersInUse = 0;
	sBufferHighWater = 0;
	sBufferFailures = 0;

	TRACE("dma: %d frame buffers of %d bytes at %#" B_PRIxPHYSADDR "\n",
		BUFFER_COUNT, BUFFER_SIZE, sBufferPool.physical);

	return B_OK;
}


void
brcms_glue_dma_uninit(void)
{
	for (uint32 i = 0; i < MAX_RINGS; i++) {
		if (sRings[i] != NULL) {
			free_dma_area(sRings[i]);
			free(sRings[i]);
			sRings[i] = NULL;
		}
	}

	sRingCount = 0;
	free_dma_area(&sBufferPool);
	skb_queue_head_init(&sFreeBuffers);
}


void
brcms_glue_dma_report(void)
{
	TRACE("dma: %" B_PRIu32 " rings (%" B_PRIu32 " realigned), buffers %"
		B_PRIu32 " in use, %" B_PRIu32 " at most of %d, %" B_PRIu32
		" refused\n", sRingCount, sRingsRetried, sBuffersInUse,
		sBufferHighWater, BUFFER_COUNT, sBufferFailures);
}


struct sk_buff*
dev_alloc_skb(unsigned int length)
{
	if (length > BUFFER_SIZE)
		return NULL;

	cpu_status state = disable_interrupts();
	acquire_spinlock(&sBufferLock);
	struct sk_buff* skb = skb_dequeue(&sFreeBuffers);
	if (skb != NULL) {
		sBuffersInUse++;
		if (sBuffersInUse > sBufferHighWater)
			sBufferHighWater = sBuffersInUse;
	} else
		sBufferFailures++;
	release_spinlock(&sBufferLock);
	restore_interrupts(state);

	if (skb == NULL)
		return NULL;

	skb->data = skb->head;
	skb->tail = skb->head;
	skb->len = 0;

	// The driver's own scratch space. Buffers come back round, and the
	// transmit ring reads its aggregation flag out of here, so a buffer
	// that still holds what the last frame put there is a frame sent down
	// a path nothing meant to take.
	memset(skb->cb, 0, sizeof(skb->cb));

	return skb;
}


void
dev_kfree_skb(struct sk_buff* skb)
{
	if (skb == NULL)
		return;

	cpu_status state = disable_interrupts();
	acquire_spinlock(&sBufferLock);
	skb_queue_tail(&sFreeBuffers, skb);
	sBuffersInUse--;
	release_spinlock(&sBufferLock);
	restore_interrupts(state);
}


void
dev_kfree_skb_any(struct sk_buff* skb)
{
	dev_kfree_skb(skb);
}


/*!	What brcmsmac itself calls to get and give back a frame. */
struct sk_buff*
brcmu_pkt_buf_get_skb(uint length)
{
	struct sk_buff* skb = dev_alloc_skb(length);
	if (skb == NULL)
		return NULL;

	skb_put(skb, length);

	return skb;
}


void
brcmu_pkt_buf_free_skb(struct sk_buff* skb)
{
	dev_kfree_skb(skb);
}


/*!	Where a buffer already is, in the only terms the engine understands.

	Linux would set up a mapping here. Nothing needs one: every buffer the
	imported sources hand down came out of the pool above, so its physical
	address is its offset from a base worked out once. Anything that did
	not come from there is refused rather than guessed at, which the caller
	sees through dma_mapping_error().
*/
dma_addr_t
dma_map_single(struct device* dev, void* address, size_t size,
	enum dma_data_direction direction)
{
	addr_t offset = (addr_t)address - (addr_t)sBufferPool.address;
	if (sBufferPool.address == NULL || offset >= sBufferPool.size
		|| offset + size > sBufferPool.size) {
		TRACE("dma: %p is not a frame buffer, refusing to map it\n",
			address);
		return 0;
	}

	return sBufferPool.physical + offset;
}


void
dma_unmap_single(struct device* dev, dma_addr_t physical, size_t size,
	enum dma_data_direction direction)
{
	// The memory was locked down and coherent before the engine saw it and
	// stays that way after, so there is nothing to undo.
}


/*!	A descriptor ring, which the engine walks on its own.

	The imported dma.c asks for a ring, then rounds the address it got up to
	the alignment the engine needs and adds that same offset to the physical
	address. That only comes out right if both addresses are already
	aligned, so that is what this returns: a pointer whose virtual and
	physical forms agree.

	Contiguous memory is page aligned in both, but the engine wants eight
	kilobytes, and whether the two agree one bit further up is not something
	create_area() can be asked for. So it is asked repeatedly, holding on to
	what does not fit until something does, and letting the rest go after.
*/
#define RING_ALIGNMENT		8192

void*
dma_alloc_coherent(struct device* dev, size_t size, dma_addr_t* _physical,
	gfp_t flags)
{
	// One area, with room at the front for whatever the caller has to skip
	// to line the ring up. It used to take up to eight areas and keep
	// whichever one happened to have its virtual and physical addresses
	// agreeing modulo eight kilobytes, because dma64_alloc() took the
	// alignment offset from the virtual address and gave it to the chip.
	// When none of the eight agreed -- which depends on nothing but where
	// the memory landed that boot -- the rings did not come up, the phy
	// init failed after them, and the machine had no wireless at all until
	// it was booted again. dma64_alloc() now takes the offset from the
	// physical address, so any area will do.
	struct dma_area* ring = (struct dma_area*)malloc(sizeof(*ring));
	if (ring == NULL)
		return NULL;

	if (allocate_dma_area(ring, size + RING_ALIGNMENT,
			"broadcom43xxn ring") != B_OK) {
		free(ring);
		return NULL;
	}

	uint32 slot;
	for (slot = 0; slot < MAX_RINGS; slot++) {
		if (sRings[slot] == NULL)
			break;
	}

	if (slot == MAX_RINGS) {
		TRACE("dma: more rings than there is room for\n");
		free_dma_area(ring);
		free(ring);
		return NULL;
	}

	ring->handedOut = ring->address;
	sRings[slot] = ring;
	sRingCount++;

	*_physical = ring->physical;

	return ring->handedOut;
}


void
dma_free_coherent(struct device* dev, size_t size, void* address,
	dma_addr_t physical)
{
	for (uint32 i = 0; i < MAX_RINGS; i++) {
		if (sRings[i] != NULL && sRings[i]->handedOut == address) {
			free_dma_area(sRings[i]);
			free(sRings[i]);
			sRings[i] = NULL;
			sRingCount--;
			return;
		}
	}
}
