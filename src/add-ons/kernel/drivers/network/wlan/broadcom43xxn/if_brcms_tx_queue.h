/* SPDX-License-Identifier: MIT */
/* Copyright 2026, rainygirl <rainygirl@gmail.com>. */
#ifndef IF_BRCMS_TX_QUEUE_H
#define IF_BRCMS_TX_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

struct mbuf;
struct ieee80211_node;

#define BRCMS_TX_DATA_PENDING 256
#define BRCMS_TX_PENDING (BRCMS_TX_DATA_PENDING + 8)

struct brcms_pending_tx {
	struct mbuf* packet;
	struct ieee80211_node* node;
	bool raw;
};

/* The caller serializes all accesses and owns entries until enqueue succeeds. */
struct brcms_tx_queue {
	struct brcms_pending_tx entries[BRCMS_TX_PENDING];
	uint16_t head;
	uint16_t count;
};

static inline bool
brcms_tx_enqueue(struct brcms_tx_queue* queue, struct brcms_pending_tx entry)
{
	if (queue->count == BRCMS_TX_PENDING)
		return false;
	uint16_t slot = (queue->head + queue->count) % BRCMS_TX_PENDING;
	queue->entries[slot] = entry;
	queue->count++;
	return true;
}

static inline bool
brcms_tx_dequeue(struct brcms_tx_queue* queue, struct brcms_pending_tx* entry)
{
	if (queue->count == 0)
		return false;
	*entry = queue->entries[queue->head];
	queue->head = (queue->head + 1) % BRCMS_TX_PENDING;
	queue->count--;
	return true;
}

#endif
