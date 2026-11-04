/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef INTEL_GTT_H
#define INTEL_GTT_H


#include <stddef.h>
#include <stdint.h>


// Gen8+ places the GGTT at 8 MiB in the 16 MiB GTTMMADR BAR.
// Keep range validation independent of PCI and MMIO so malformed resource
// descriptions can be tested without accessing hardware.
static inline bool
intel_gen8_gtt_base(uint64_t mmioBase, uint64_t mmioSize, uint64_t tableSize,
	uint64_t* tableBase)
{
	const uint64_t offset = 8ULL << 20;
	if (tableBase == NULL || tableSize == 0 || tableSize > offset
		|| mmioSize < offset || tableSize > mmioSize - offset) {
		return false;
	}

	// Check the entire physical range, including its last byte.
	if (mmioBase > UINT64_MAX - (offset + tableSize - 1))
		return false;

	*tableBase = mmioBase + offset;
	return true;
}


#endif // INTEL_GTT_H
