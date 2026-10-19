/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	The packet buffer the imported sources pass about.

	brcmsmac moves frames in Linux socket buffers, so the shape of one is
	part of the interface and the names below are Linux's. What is behind
	them is not: a socket buffer here is a fixed slice of one contiguous
	allocation, handed out from a free list, and it knows its own physical
	address because the chip's DMA engine is given that address directly
	rather than any mapping.

	A buffer is laid out head .. data .. tail .. end. Reserving moves data
	forward to leave room for a header, putting appends at the tail, and
	pulling steps data past a header that has been dealt with.
*/
#ifndef BRCMS_LINUX_SKBUFF_H
#define BRCMS_LINUX_SKBUFF_H


#include "linux/types.h"


struct sk_buff {
	struct sk_buff*	next;
	struct sk_buff*	prev;

	unsigned char*	head;
	unsigned char*	data;
	unsigned char*	tail;
	unsigned char*	end;
	unsigned int	len;

	// Where head is in physical memory. The receive and transmit rings are
	// filled with this, so it has to be right for every buffer handed out.
	phys_addr_t		physical;

	// The driver's own scratch space on a packet. brcmsmac keeps its
	// transmit bookkeeping here.
	char			cb[48] __attribute__((aligned(8)));
};


struct sk_buff_head {
	struct sk_buff*	next;
	struct sk_buff*	prev;
	u32				qlen;
};


static inline void
skb_queue_head_init(struct sk_buff_head* list)
{
	list->next = (struct sk_buff*)list;
	list->prev = (struct sk_buff*)list;
	list->qlen = 0;
}


static inline bool
skb_queue_empty(const struct sk_buff_head* list)
{
	return list->next == (const struct sk_buff*)list;
}


static inline u32
skb_queue_len(const struct sk_buff_head* list)
{
	return list->qlen;
}


static inline struct sk_buff*
skb_peek(struct sk_buff_head* list)
{
	return skb_queue_empty(list) ? NULL : list->next;
}


static inline struct sk_buff*
skb_peek_tail(struct sk_buff_head* list)
{
	return skb_queue_empty(list) ? NULL : list->prev;
}


static inline void
skb_queue_tail(struct sk_buff_head* list, struct sk_buff* skb)
{
	skb->next = (struct sk_buff*)list;
	skb->prev = list->prev;
	list->prev->next = skb;
	list->prev = skb;
	list->qlen++;
}


static inline void
skb_queue_head(struct sk_buff_head* list, struct sk_buff* skb)
{
	skb->prev = (struct sk_buff*)list;
	skb->next = list->next;
	list->next->prev = skb;
	list->next = skb;
	list->qlen++;
}


static inline struct sk_buff*
skb_dequeue(struct sk_buff_head* list)
{
	if (skb_queue_empty(list))
		return NULL;

	struct sk_buff* skb = list->next;
	list->next = skb->next;
	skb->next->prev = (struct sk_buff*)list;
	list->qlen--;
	skb->next = NULL;
	skb->prev = NULL;

	return skb;
}


static inline void
skb_unlink(struct sk_buff* skb, struct sk_buff_head* list)
{
	skb->prev->next = skb->next;
	skb->next->prev = skb->prev;
	list->qlen--;
	skb->next = NULL;
	skb->prev = NULL;
}


/*!	Moves one list onto the end of another, leaving the first empty. */
static inline void
skb_queue_splice_tail(struct sk_buff_head* from, struct sk_buff_head* to)
{
	if (skb_queue_empty(from))
		return;

	from->next->prev = to->prev;
	to->prev->next = from->next;
	from->prev->next = (struct sk_buff*)to;
	to->prev = from->prev;
	to->qlen += from->qlen;

	skb_queue_head_init(from);
}


#define skb_queue_walk(list, skb) \
	for (skb = (list)->next; skb != (struct sk_buff*)(list); \
		skb = skb->next)

#define skb_queue_walk_safe(list, skb, tmp) \
	for (skb = (list)->next, tmp = skb->next; \
		skb != (struct sk_buff*)(list); skb = tmp, tmp = skb->next)


/*!	Leaves \a length bytes free in front of the data, for a header. */
static inline void
skb_reserve(struct sk_buff* skb, int length)
{
	skb->data += length;
	skb->tail += length;
}


/*!	Claims \a length bytes at the end and returns where they start. */
static inline unsigned char*
skb_put(struct sk_buff* skb, unsigned int length)
{
	unsigned char* at = skb->tail;
	skb->tail += length;
	skb->len += length;
	return at;
}


/*!	Steps past \a length bytes of header that have been dealt with. */
static inline unsigned char*
skb_pull(struct sk_buff* skb, unsigned int length)
{
	skb->len -= length;
	skb->data += length;
	return skb->data;
}


static inline void
__skb_trim(struct sk_buff* skb, unsigned int length)
{
	skb->len = length;
	skb->tail = skb->data + length;
}


static inline unsigned int
skb_tailroom(const struct sk_buff* skb)
{
	return skb->end - skb->tail;
}


struct sk_buff* dev_alloc_skb(unsigned int length);
void dev_kfree_skb(struct sk_buff* skb);
void dev_kfree_skb_any(struct sk_buff* skb);


#endif	// BRCMS_LINUX_SKBUFF_H
