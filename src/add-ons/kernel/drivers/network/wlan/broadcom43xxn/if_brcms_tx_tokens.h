/* SPDX-License-Identifier: MIT
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef IF_BRCMS_TX_TOKENS_H
#define IF_BRCMS_TX_TOKENS_H

#define BRCMS_TX_TOKEN_SLOTS 64

struct brcms_tx_tokens {
	uint64_t occupied;
	uint32_t next;
	uint32_t tokens[BRCMS_TX_TOKEN_SLOTS];
};

/* All access is serialized by the driver's TX mutex. */
static inline bool
brcms_tx_token_reserve(struct brcms_tx_tokens* state, uint32_t* token)
{
	for (unsigned i = 0; i < BRCMS_TX_TOKEN_SLOTS; i++) {
		uint32_t candidate = state->next++;
		unsigned slot = candidate % BRCMS_TX_TOKEN_SLOTS;
		uint64_t bit = (uint64_t)1 << slot;
		if ((state->occupied & bit) == 0) {
			state->occupied |= bit;
			state->tokens[slot] = candidate;
			*token = candidate;
			return true;
		}
	}
	return false;
}

static inline bool
brcms_tx_token_valid(const struct brcms_tx_tokens* state, uint32_t token)
{
	unsigned slot = token % BRCMS_TX_TOKEN_SLOTS;
	return (state->occupied & ((uint64_t)1 << slot)) != 0
		&& state->tokens[slot] == token;
}

static inline bool
brcms_tx_token_release(struct brcms_tx_tokens* state, uint32_t token)
{
	if (!brcms_tx_token_valid(state, token))
		return false;
	state->occupied &= ~((uint64_t)1 << (token % BRCMS_TX_TOKEN_SLOTS));
	return true;
}

#endif
