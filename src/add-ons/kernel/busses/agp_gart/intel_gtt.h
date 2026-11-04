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


// GMS encodings from the Alder Lake PCI graphics-control register definition.
// Reserved encodings must not turn into an assumed one-MiB stolen region.
static inline bool
intel_alderlake_stolen_size(uint16_t memoryConfig, uint64_t* stolenSize)
{
	if (stolenSize == NULL)
		return false;

	uint8_t gms = memoryConfig >> 8;
	uint64_t mib;
	if (gms <= 0x10)
		mib = (uint64_t)gms * 32;
	else if (gms == 0x20 || gms == 0x30 || gms == 0x40)
		mib = (uint64_t)gms * 32;
	else if (gms >= 0xf0 && gms <= 0xfe)
		mib = (uint64_t)(gms - 0xf0 + 1) * 4;
	else
		return false;

	*stolenSize = mib << 20;
	return true;
}


// This initial system-memory path uses the 39-bit DMA limit of ADL-P/N.
// Other platforms must not silently truncate addresses to fit this limit.
static inline bool
intel_gen8_gtt_pte(uint64_t physicalAddress, uint64_t* pte)
{
	if (pte == NULL || (physicalAddress & 4095) != 0
		|| physicalAddress >= (1ULL << 39)) {
		return false;
	}

	// GGTT entries use the physical address directly, unlike legacy GTTs.
	// Bit 0 is present; no local-memory or PPGTT access flags are requested.
	*pte = physicalAddress | 1;
	return true;
}


static inline bool
intel_gen8_gtt_layout(uint64_t tableEntries, uint64_t apertureSize,
	uint64_t reservedSize)
{
	return tableEntries > 0 && tableEntries <= (1ULL << 20)
		&& apertureSize > 0 && (apertureSize & 4095) == 0
		&& apertureSize / 4096 <= tableEntries
		&& reservedSize <= apertureSize && (reservedSize & 4095) == 0;
}


// Caller owns an uncached mapping of tableEntries 64-bit entries and serializes
// updates. This performs one PTE store only; GPU TLB invalidation is separate.
static inline bool
intel_gen8_gtt_write(volatile uint64_t* table, uint64_t tableEntries,
	uint64_t apertureSize, uint64_t reservedSize, uint64_t offset,
	uint64_t physicalAddress)
{
	if (sizeof(void*) != 8 || table == NULL || ((uintptr_t)table & 7) != 0
		|| !intel_gen8_gtt_layout(tableEntries, apertureSize, reservedSize)
		|| (offset & 4095) != 0 || offset < reservedSize
		|| offset >= apertureSize) {
		return false;
	}

	uint64_t pte;
	if (!intel_gen8_gtt_pte(physicalAddress, &pte))
		return false;

	table[offset / 4096] = pte;
	return true;
}


#endif // INTEL_GTT_H
