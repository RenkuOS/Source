/*
 * Copyright 2026, DruSatori, dru@druware.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef _ARM64_ISA_LEVEL_H
#define _ARM64_ISA_LEVEL_H


#include <SupportDefs.h>


/*!	The arm64 ID registers arm64_isa_level() looks at, as read with MRS. */
typedef struct arm64_id_regs {
	uint64	isar0;	// ID_AA64ISAR0_EL1
	uint64	isar1;	// ID_AA64ISAR1_EL1
	uint64	isar2;	// ID_AA64ISAR2_EL1
	uint64	mmfr2;	// ID_AA64MMFR2_EL1
	uint64	pfr0;	// ID_AA64PFR0_EL1
	uint64	pfr1;	// ID_AA64PFR1_EL1
	uint64	zfr0;	// ID_AA64ZFR0_EL1
} arm64_id_regs;


static inline uint32
arm64_id_field(uint64 reg, unsigned shift)
{
	return (reg >> shift) & 0xf;
}


/*!	Returns a conservative lower bound of the Armv8.x-A / Armv9.x-A level,
	as (major << 8) | minor, at least 0x0800: the highest level for which
	every feature checked here for it and the levels below is present. A
	CPU that lacks one of them, e.g. one that is optional at its level,
	reads as a lower level.

	Only instruction set features (FEAT_* names from the Arm ARM) are
	tested: system features such as VHE or NV are hidden from a guest
	kernel at EL1 and would understate the level in a VM.
	Armv9.0 is Armv8.5 plus FEAT_SVE2; Armv9.x then tracks Armv8.(x+5).
*/
static inline uint32
arm64_isa_level(const arm64_id_regs* r)
{
	const int levels[] = {
		// 8.1: FEAT_LSE, FEAT_RDM
		arm64_id_field(r->isar0, 20) >= 2 && arm64_id_field(r->isar0, 28) >= 1,
		// 8.2: FEAT_DPB
		arm64_id_field(r->isar1, 0) >= 1,
		// 8.3: FEAT_PAuth (APA, API or APA3), FEAT_JSCVT, FEAT_FCMA, FEAT_LRCPC
		(arm64_id_field(r->isar1, 4) >= 1 || arm64_id_field(r->isar1, 8) >= 1
			|| arm64_id_field(r->isar2, 12) >= 1)
			&& arm64_id_field(r->isar1, 12) >= 1
			&& arm64_id_field(r->isar1, 16) >= 1
			&& arm64_id_field(r->isar1, 20) >= 1,
		// 8.4: FEAT_DIT, FEAT_LRCPC2, FEAT_FlagM, FEAT_LSE2, FEAT_DotProd
		arm64_id_field(r->pfr0, 48) >= 1 && arm64_id_field(r->isar1, 20) >= 2
			&& arm64_id_field(r->isar0, 52) >= 1
			&& arm64_id_field(r->mmfr2, 32) >= 1
			&& arm64_id_field(r->isar0, 44) >= 1,
		// 8.5: FEAT_SB, FEAT_FRINTTS, FEAT_FlagM2, FEAT_BTI, FEAT_DPB2
		arm64_id_field(r->isar1, 36) >= 1 && arm64_id_field(r->isar1, 32) >= 1
			&& arm64_id_field(r->isar0, 52) >= 2
			&& arm64_id_field(r->pfr1, 0) >= 1
			&& arm64_id_field(r->isar1, 0) >= 2,
		// 8.6: FEAT_BF16, FEAT_I8MM
		arm64_id_field(r->isar1, 44) >= 1 && arm64_id_field(r->isar1, 52) >= 1,
		// 8.7: FEAT_WFxT, FEAT_XS
		arm64_id_field(r->isar2, 0) >= 2 && arm64_id_field(r->isar1, 56) >= 1,
		// 8.8: FEAT_MOPS, FEAT_HBC
		arm64_id_field(r->isar2, 16) >= 1 && arm64_id_field(r->isar2, 20) >= 1,
	};

	uint32 minor = 0;
	while (minor < sizeof(levels) / sizeof(levels[0]) && levels[minor])
		minor++;

	// FEAT_SVE2: PFR0.SVE says ZFR0 is valid, ZFR0.SVEver >= 1
	if (minor >= 5 && arm64_id_field(r->pfr0, 32) >= 1
		&& arm64_id_field(r->zfr0, 0) >= 1)
		return 0x0900 | (minor - 5);
	return 0x0800 | minor;
}


#endif	// _ARM64_ISA_LEVEL_H
