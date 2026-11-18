/*
 * Copyright 2026, Dru Satori, dru@druware.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef _HYPERV_ARM64_CPU_H_
#define _HYPERV_ARM64_CPU_H_


#include <SupportDefs.h>


// arm64 has no MSRs or CPUID: hypercalls use SMCCC (hvc #0), synthetic
// registers are read with HvCallGetVpRegisters.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface

// SMCCC function id: fast-call bit clear (yielding), 64-bit, vendor hypervisor
// service range (6), function 1
#define HV_SMCCC_FUNC_ID						0x46000001ULL

// Hypercall input value
#define HV_HYPERCALL_GET_VP_REGISTERS			0x0050ULL
#define HV_HYPERCALL_SET_VP_REGISTERS			0x0051ULL
#define HV_HYPERCALL_FAST						(1ULL << 16)
#define HV_HYPERCALL_REP_COUNT_1				(1ULL << 32)
#define HV_HYPERCALL_STATUS_MASK				0xFFFFULL

#define HV_PARTITION_ID_SELF					(~0ULL)
#define HV_VP_INDEX_SELF						0xFFFFFFFEULL

// Synthetic registers (x86 MSR in brackets); bit layouts are the same as x86
#define HV_REG_GUEST_OS_ID						0x00090002	// 0x40000000
#define HV_REG_VP_INDEX							0x00090003	// 0x40000002
#define HV_REG_TIME_REF_COUNT					0x00090004	// 0x40000020
#define HV_REG_SINT0							0x000A0000	// 0x40000090
#define HV_REG_SCONTROL							0x000A0010	// 0x40000080
#define HV_REG_SIEFP							0x000A0012	// 0x40000082
#define HV_REG_SIMP								0x000A0013	// 0x40000083
#define HV_REG_EOM								0x000A0014	// 0x40000084

#define HV_SCONTROL_ENABLE						(1ULL << 0)
#define HV_SCONTROL_RSVD_MASK					0xFFFFFFFFFFFFFFFEULL
#define HV_SIEFP_ENABLE							(1ULL << 0)
#define HV_SIEFP_RSVD_MASK						0x0FFEULL
#define HV_SIEFP_PAGE_SHIFT						12
#define HV_SIMP_ENABLE							(1ULL << 0)
#define HV_SIMP_RSVD_MASK						0x0FFEULL
#define HV_SIMP_PAGE_SHIFT						12
#define HV_SINT_VECTOR_MASK						0x00000000000000FFULL
#define HV_SINT_MASKED							(1ULL << 16)
#define HV_SINT_AUTOEOI							(1ULL << 17)
#define HV_SINT_POLLING							(1ULL << 18)
#define HV_SINT_RSVD_MASK						0xFFFFFFFFFFF8FF00ULL

// Guest OS ID, the same value the x86 backend writes: FreeBSD 16.0, open source
// (x86/hyperv_cpu.h, HV_FREEBSD_*; a Haiku OS type would need Microsoft's approval)
#define HV_GUEST_OS_ID_FREEBSD \
	((uint64)((1ULL << 63) | (0x2ULL << 56) | (0ULL << 48) | (1600000ULL << 16) | 0ULL))

// Pseudo registers with the layout of x86 CPUID leaves 0x40000002-4
#define HV_REG_HYPERVISOR_VERSION				0x00000100
#define HV_REG_PRIVILEGES_AND_FEATURES			0x00000200
#define HV_REG_FEATURES							0x00000201

// Low dword of HV_REG_PRIVILEGES_AND_FEATURES (x86: CPUID 0x40000003 eax)
#define HV_CPUID_FEATURE_TIME_REF_COUNTER		(1 << 1)
#define HV_CPUID_FEATURE_SYNIC					(1 << 2)

// High dword of HV_REG_PRIVILEGES_AND_FEATURES (x86: CPUID 0x40000003 ebx)
#define HV_CPUID_FEATURE_HYPERCALL_POST_MESSAGE	(1 << 4)
#define HV_CPUID_FEATURE_HYPERCALL_SIGNAL_EVENT	(1 << 5)

// Required feature bits for VMBus operation. Unlike x86 the synthetic timer is
// not needed, the generic timer is used, nor the hypercall MSRs, which arm64
// does not have.
#define HV_CPUID_EAX_REQUIRED_FEATURES	HV_CPUID_FEATURE_SYNIC
#define HV_CPUID_EBX_REQUIRED_FEATURES	(HV_CPUID_FEATURE_HYPERCALL_POST_MESSAGE \
	| HV_CPUID_FEATURE_HYPERCALL_SIGNAL_EVENT)


// Issues a hypercall. regs holds x2..x7: fast-call input on entry, fast-call
// output on return. Returns x0 (status in the low 16 bits).
// Must only be called once Hyper-V has been detected: hvc is undefined elsewhere.
static inline uint64
hv_hvcall(uint64 control, uint64 regs[6])
{
	register uint64 x0 asm("x0") = HV_SMCCC_FUNC_ID;
	register uint64 x1 asm("x1") = control;
	register uint64 x2 asm("x2") = regs[0];
	register uint64 x3 asm("x3") = regs[1];
	register uint64 x4 asm("x4") = regs[2];
	register uint64 x5 asm("x5") = regs[3];
	register uint64 x6 asm("x6") = regs[4];
	register uint64 x7 asm("x7") = regs[5];

	// x8-x17 are not used, but SMCCC allows the hypervisor to change them
	asm volatile("hvc #0"
		: "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5),
			"+r" (x6), "+r" (x7)
		:
		: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
			"memory");

	regs[0] = x2;
	regs[1] = x3;
	regs[2] = x4;
	regs[3] = x5;
	regs[4] = x6;
	regs[5] = x7;
	return x0;
}


// Reads one 128-bit synthetic register of the calling VP.
// Returns the hypercall status (0 on success).
static inline uint16
hv_get_vpreg(uint32 name, uint64* _low, uint64* _high)
{
	uint64 regs[6] = { HV_PARTITION_ID_SELF, HV_VP_INDEX_SELF, name, 0, 0, 0 };
	uint64 status = hv_hvcall(HV_HYPERCALL_GET_VP_REGISTERS | HV_HYPERCALL_FAST
		| HV_HYPERCALL_REP_COUNT_1, regs);

	*_low = regs[4];
	*_high = regs[5];
	return (uint16)(status & HV_HYPERCALL_STATUS_MASK);
}


// Reads the low 64 bits of a synthetic register, 0 if the hypercall fails.
static inline uint64
hv_get_vpreg64(uint32 name)
{
	uint64 low, high;
	return hv_get_vpreg(name, &low, &high) == 0 ? low : 0;
}


// Writes one synthetic register of the calling VP (high 64 bits zero).
// Returns the hypercall status (0 on success).
static inline uint16
hv_set_vpreg(uint32 name, uint64 value)
{
	uint64 regs[6] = { HV_PARTITION_ID_SELF, HV_VP_INDEX_SELF, name, 0, value, 0 };
	uint64 status = hv_hvcall(HV_HYPERCALL_SET_VP_REGISTERS | HV_HYPERCALL_FAST
		| HV_HYPERCALL_REP_COUNT_1, regs);
	return (uint16)(status & HV_HYPERCALL_STATUS_MASK);
}


#endif // _HYPERV_ARM64_CPU_H_
