/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License for the native code.
 * Genode-modeled portions retain the license in COPYING.Genode.
 * Copyright (C) 2017-2024 Genode Labs GmbH (referenced portions).
 *
 * SPDX-License-Identifier: LicenseRef-Genode-AGPL-3.0-with-linking-exception AND MIT
 *
 * Gen9 (Skylake/KabyLake) GT register definitions and MI command encodings
 * for the intel_extreme render node. All registers are plain offsets into
 * the MMIO BAR (BAR0), i.e. they live in the REGS_FLAT block and are used
 * with the render_read32()/render_write32() helpers in render_node.cpp.
 *
 * Values cross-checked against the Intel SKL PRM and Genode's Intel GPU
 * driver (repos/os/src/driver/gpu/intel/{mmio,commands,context}.h).
 */
#ifndef GEN9_GT_REGS_H
#define GEN9_GT_REGS_H


//	------------------------------------------------------------------
//	PCI config space
//	------------------------------------------------------------------

// GGC (GMCH Graphics Control), 16 bit at 0x50
#define GEN9_PCI_GGC					0x50
#define GEN9_GGC_GGMS_MASK				(3 << 6)	// GGTT size, bits 7:6
	// size = (1 << GGMS) MB: 1 -> 2 MB, 2 -> 4 MB, 3 -> 8 MB
#define GEN9_GGC_GMS_MASK				(0xff << 8)	// stolen memory size
	// values 1..16 -> n * 32 MB, values 240..254 -> (n - 236) * 4 MB

//	------------------------------------------------------------------
//	Interrupt registers (GT block 0, IHD-OS-BDW-Vol 2c p. 658/953)
//	------------------------------------------------------------------

#define GEN9_MASTER_INT_CTL				0x44200
#	define GEN9_MASTER_INT_ENABLE		(1u << 31)
#	define GEN9_RENDER_INT_PENDING		(1u << 0)

#define GEN9_GT0_ISR					0x44300
#define GEN9_GT0_IMR					0x44304
#define GEN9_GT0_IIR					0x44308
#define GEN9_GT0_IER					0x4430C
#	define GEN9_GT_CS_MI_USER_INTERRUPT		(1u << 0)
#	define GEN9_GT_CS_ERROR_INTERRUPT		(1u << 3)
#	define GEN9_GT_CS_WATCHDOG_EXPIRED		(1u << 6)
#	define GEN9_GT_PAGE_FAULT_ERROR			(1u << 7)
#	define GEN9_GT_CS_CTX_SWITCH_INTERRUPT	(1u << 8)

//	------------------------------------------------------------------
//	RCS engine registers (base 0x2000)
//	------------------------------------------------------------------

#define GEN9_RCS_HWSTAM					0x2098	// hardware status mask
#define GEN9_RCS_HWS_PGA				0x2080	// HW status page GGTT address
#define GEN9_RCS_EMR					0x20B4	// error mask
#define GEN9_RCS_MI_MODE				0x209C
#	define GEN9_MI_MODE_STOP_RINGS			(1u << 8)
#	define GEN9_MI_MODE_RINGS_IDLE			(1u << 9)
#	define GEN9_MI_MODE_STOP_RINGS_MASK		(1u << (16 + 8))
#define GEN9_RCS_RESET_CTRL				0x20D0
#	define GEN9_RESET_CTRL_REQUEST			(1u << 0)
#	define GEN9_RESET_CTRL_READY			(1u << 1)
#	define GEN9_RESET_CTRL_CATASTROPHIC		(1u << 2)
#	define GEN9_RESET_CTRL_MASK_BIT			(1u << 16)
#define GEN9_R_PWR_CLK_STATE			0x20C8
#define GEN9_RCS_INSTPM					0x20C0

// legacy ring buffer registers
#define GEN9_RCS_RING_TAIL				0x2030
#define GEN9_RCS_RING_HEAD				0x2034
#define GEN9_RCS_RING_START				0x2038
#define GEN9_RCS_RING_CTL				0x203C
#	define GEN9_RING_CTL_ENABLE				(1u << 0)

// execlists
#define GEN9_RCS_EXECLIST_SUBMITPORT	0x2230
#define GEN9_RCS_EXECLIST_STATUS		0x2234	// 64 bit
#	define GEN9_EXECLIST_STATUS_CURRENT_PTR		(1u << 0)
#	define GEN9_EXECLIST_STATUS_WRITE_PTR			(1u << 1)
#	define GEN9_EXECLIST_STATUS_QUEUE_FULL		(1u << 2)
#	define GEN9_EXECLIST_STATUS_1_VALID			(1u << 3)
#	define GEN9_EXECLIST_STATUS_0_VALID			(1u << 4)
#define GEN9_RCS_RING_CONTEXT_STATUS_PTR 0x23A0
#define GEN9_RCS_CS_CTX_TIMESTAMP		0x23A8
#define GEN9_RCS_CTX_CONTROL			0x2244

// context image registers (LRI targets in the logical ring context)
#define GEN9_RCS_BB_STATE				0x2110
#define GEN9_RCS_SBB_ADDR				0x2114
#define GEN9_RCS_SBB_STATE				0x2118
#define GEN9_RCS_SBB_ADDR_UDW			0x211C
#define GEN9_RCS_BB_ADDR				0x2140
#define GEN9_RCS_BB_ADDR_UDW			0x2168
#define GEN9_RCS_BB_PER_CTX_PTR			0x21C0
#define GEN9_RCS_INDIRECT_CTX			0x21C4
#define GEN9_RCS_INDIRECT_CTX_OFFSET	0x21C8

// PPGTT page directory pointers (not used by the v1 GGTT addressing mode)
#define GEN9_RCS_PDP0_LDW				0x2270
#define GEN9_RCS_PDP0_UDW				0x2274
#define GEN9_RCS_PDP1_LDW				0x2278
#define GEN9_RCS_PDP1_UDW				0x227C
#define GEN9_RCS_PDP2_LDW				0x2280
#define GEN9_RCS_PDP2_UDW				0x2284
#define GEN9_RCS_PDP3_LDW				0x2288
#define GEN9_RCS_PDP3_UDW				0x228C

//	------------------------------------------------------------------
//	GT global registers
//	------------------------------------------------------------------

#define GEN9_GFX_MODE					0x229C
#	define GEN9_GFX_MODE_EXECLIST_ENABLE	(1u << 15)
#	define GEN9_GFX_MODE_EXECLIST_MASK		(1u << (16 + 15))

#define GEN9_GDRST						0x941C
#	define GEN9_GDRST_FULL					(1u << 0)
#	define GEN9_GDRST_RENDER				(1u << 1)

#define GEN9_FUSE2						0x9120
#	define GEN9_FUSE2_SLICE_ENABLE_MASK		(7u << 25)
#	define GEN9_FUSE2_SUBSLICE_DISABLE_MASK	(0xfu << 20)
#define GEN9_EU_DISABLE0				0x9134
#define GEN9_MIRROR_EU_DISABLE0			0x9134

#define GEN9_RP_FREQ_NORMAL				0xA008
#	define GEN9_RP_FREQ_TURBO_DISABLE		(1u << 31)
#define GEN9_RP_CTRL					0xA024

#define GEN9_TIMESTAMP_CTR				0x44070

//	------------------------------------------------------------------
//	Forcewake (Gen9)
//	------------------------------------------------------------------

#define GEN9_FORCEWAKE_GT				0xA188
#define GEN9_FORCEWAKE_MEDIA			0xA270
#define GEN9_FORCEWAKE_RENDER			0xA278
#	define GEN9_FORCEWAKE_KERNEL			(1u << 0)
#	define GEN9_FORCEWAKE_KERNEL_MASK		(1u << 16)
#	define GEN9_FORCEWAKE_RESET				0xffff0000u
#define GEN9_MISC_CTRL0					0xA180	// posting read target

#define GEN9_FORCEWAKE_RENDER_ACK		0x0D84
#define GEN9_FORCEWAKE_MEDIA_ACK		0x0D88
#define GEN9_FORCEWAKE_GT_ACK			0x130044
#	define GEN9_FORCEWAKE_ACK_KERNEL		(1u << 0)

//	------------------------------------------------------------------
//	Misc workarounds / unit registers
//	------------------------------------------------------------------

#define GEN9_NDE_RSTWRN_OPT				0x46408
#	define GEN9_RST_PCH_HANDSHAKE_EN		(1u << 4)

// page attribute table (Gen9+): Genode programs 0x40E4 = 0x000A0907,
// 0x40E0 = 0x3B2B1B0B (WB/WC/WT/UC over LLC/eLLC targets)
#define GEN9_PAT_INDEX_L				0x40E4
#define GEN9_PAT_INDEX_H				0x40E0
#	define GEN9_PAT_INDEX_L_VALUE			0x000A0907u
#	define GEN9_PAT_INDEX_H_VALUE			0x3B2B1B0Bu

// GAPS TSV erratum: without bit 7 the GPU hangs arbitrarily during
// batch execution — set at every init/reset
#define GEN9_GAPS_TSV					0xB004
#	define GEN9_GAPS_TSV_ENABLE				(1u << 7)

// 32 fence registers, 64 bit each
#define GEN9_FENCE_REG_BASE				0x100000
#define GEN9_FENCE_REG_COUNT			32

// GFX TLB flush: write 1, read back
#define GEN9_GFX_FLSH_CNTL				0x101008
#	define GEN9_GFX_FLSH_CNTL_FLUSH			(1u << 0)

// fault registers: per-engine ring fault (i915 RING_FAULT_REG), the GAM
// fault register array (Genode FAULT_REG, RCS = engine 0), and the fault
// TLB readback data (faulting GPU VA)
#define GEN9_RING_FAULT_REG				0x2494
#define GEN9_GAM_FAULT_REG				0x4000
#define GEN9_FAULT_TLB_RB_DATA0			0x4B10
#define GEN9_FAULT_TLB_RB_DATA1			0x4B14

//	------------------------------------------------------------------
//	GGTT
//	------------------------------------------------------------------

// the GGTT is mapped in the upper half of BAR0 (BAR0 + barSize / 2);
// entries are 64 bit, bit 0 = present, bits 47:12 = page frame
#define GEN9_GGTT_PTE_PRESENT			(1ULL << 0)

//	------------------------------------------------------------------
//	MI command encodings (IHD-OS-BDW-Vol 2a; Genode commands.h)
//	------------------------------------------------------------------

#define MI_NOOP							0x00000000u
#define MI_USER_INTERRUPT				0x01000000u
#define MI_ARB_CHECK					0x02800000u
#define MI_ARB_ON_OFF(enable)			(0x04000000u | (enable))
#define MI_BATCH_BUFFER_END				0x05000000u
#define MI_STORE_DWORD_IMM				0x10000000u	/* + length field */
	// Gen8+ 4-dword form (64-bit address), use-global-GTT bit:
#define MI_STORE_DWORD_IMM_GEN9			(0x10000000u | (1u << 22) | 2)
#define MI_SEMAPHORE_WAIT				0x0E40C002u	/* GGTT, poll, eq, len 2 */
#define MI_LOAD_REGISTER_IMM(n)			(0x11000000u | (2 * (n) - 1))
#define MI_LRI_FORCE_POSTED				(1u << 12)

// Gen8+: 3 dwords, address space indicator bit 8 (1 = PPGTT, 0 = GGTT)
#define MI_BATCH_BUFFER_START			0x18800000u
#	define MI_BATCH_BUFFER_START_PPGTT		(1u << 8)
	// Genode (PPGTT): 0x18800101, v1 GGTT addressing: 0x18800001

// PIPE_CONTROL: type MI_RCS | subtype GFX_PIPE | opcode PIPE_CONTROL
#define MI_PIPE_CONTROL					0x7A000000u
#define MI_PIPE_CONTROL_GEN9			0x7A000004u	// 6 dwords on Gen9
#	define PIPE_CONTROL_FLUSH_L3				(1u << 27)
#	define PIPE_CONTROL_GLOBAL_GTT_IVB			(1u << 24)
#	define PIPE_CONTROL_MMIO_WRITE				(1u << 23)
#	define PIPE_CONTROL_STORE_DATA_INDEX		(1u << 21)
#	define PIPE_CONTROL_CS_STALL				(1u << 20)
#	define PIPE_CONTROL_TLB_INVALIDATE			(1u << 18)
#	define PIPE_CONTROL_MEDIA_STATE_CLEAR		(1u << 16)
#	define PIPE_CONTROL_QW_WRITE				(1u << 14)
#	define PIPE_CONTROL_DEPTH_STALL				(1u << 13)
#	define PIPE_CONTROL_WRITE_FLUSH				(1u << 12)
#	define PIPE_CONTROL_RENDER_TARGET_FLUSH		(1u << 12)
#	define PIPE_CONTROL_INSTRUCTION_INVALIDATE	(1u << 11)
#	define PIPE_CONTROL_TEXTURE_INVALIDATE		(1u << 10)
#	define PIPE_CONTROL_NOTIFY					(1u << 8)
#	define PIPE_CONTROL_FLUSH_ENABLE			(1u << 7)
#	define PIPE_CONTROL_DC_FLUSH_ENABLE			(1u << 5)
#	define PIPE_CONTROL_VF_CACHE_INVALIDATE		(1u << 4)
#	define PIPE_CONTROL_CONST_CACHE_INVALIDATE	(1u << 3)
#	define PIPE_CONTROL_STATE_CACHE_INVALIDATE	(1u << 2)
#	define PIPE_CONTROL_DEPTH_CACHE_FLUSH		(1u << 0)

//	------------------------------------------------------------------
//	Execlist context descriptor (64 bit, IHD-OS-BDW-Vol 2d p. 107)
//	------------------------------------------------------------------

#	define GEN9_CTX_DESC_LRCA_MASK			0xFFFFF000u	// bits 31:12
#	define GEN9_CTX_DESC_PRIVILEGE			(1u << 8)
#	define GEN9_CTX_DESC_ADDRESSING_SHIFT	3
#	define GEN9_CTX_DESC_LEGACY_32			(0x01 << 3)	// GGTT addressing
#	define GEN9_CTX_DESC_LEGACY_64			(0x03 << 3)	// PPGTT 48 bit
#	define GEN9_CTX_DESC_FORCE_RESTORE		(1u << 2)
#	define GEN9_CTX_DESC_FORCE_PD_RESTORE	(1u << 1)
#	define GEN9_CTX_DESC_VALID				(1u << 0)
	// high dword: context id (bits 51:32, 20 bit wide)


#endif	/* GEN9_GT_REGS_H */
