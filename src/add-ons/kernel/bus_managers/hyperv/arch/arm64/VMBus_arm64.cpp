/*
 * Copyright 2026, Dru Satori, dru@druware.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#include "hyperv_cpu.h"
#include "VMBusPrivate.h"

#include <arch/int.h>


status_t
vmbus_detect_hyperv()
{
	CALLED();

	// No CPUID on arm64: Hyper-V identifies itself in the FADT (ACPI 6.0+).
	// This check must pass before any hvc, which is undefined elsewhere: bare
	// metal and other hypervisors have no "MsHyperV" FADT id, so they return
	// here and never issue one.
	ACPI_TABLE_FADT* fadt;
	if (gACPI->get_table(ACPI_SIG_FADT, 0, (void**)&fadt) != B_OK) {
		TRACE("No FADT\n");
		return B_ERROR;
	}
	if (fadt->Header.Revision < 6 || fadt->Header.Length < ACPI_FADT_V6_SIZE
		|| memcmp(&fadt->HypervisorId, "MsHyperV", 8) != 0) {
		TRACE("Not running on Hyper-V\n");
		return B_ERROR;
	}

	// Set the guest OS ID before reading the features, as Linux does
	uint16 status = hv_set_vpreg(HV_REG_GUEST_OS_ID, HV_GUEST_OS_ID_FREEBSD);
	if (status != 0) {
		ERROR("Setting guest OS id failed, status 0x%x\n", status);
		return B_ERROR;
	}

	uint64 priv[2];
	status = hv_get_vpreg(HV_REG_PRIVILEGES_AND_FEATURES, &priv[0], &priv[1]);
	if (status != 0) {
		ERROR("GetVpRegisters failed, status 0x%x\n", status);
		return B_ERROR;
	}

	uint32 privLow = (uint32)priv[0];
	uint32 privHigh = (uint32)(priv[0] >> 32);
	TRACE("Hyper-V features: %08" B_PRIx32 ":%08" B_PRIx32 "\n", privLow, privHigh);
	if ((privLow & HV_CPUID_EAX_REQUIRED_FEATURES) != HV_CPUID_EAX_REQUIRED_FEATURES
		|| (privHigh & HV_CPUID_EBX_REQUIRED_FEATURES) != HV_CPUID_EBX_REQUIRED_FEATURES) {
		TRACE("Missing required Hyper-V features\n");
		return B_ERROR;
	}

#ifdef TRACE_VMBUS
	uint64 version[2];
	if (hv_get_vpreg(HV_REG_HYPERVISOR_VERSION, &version[0], &version[1]) == 0) {
		TRACE("Hyper-V version: %" B_PRIu32 ".%" B_PRIu32 ".%" B_PRIu32 " [SP%" B_PRIu32 "]\n",
			(uint32)(version[0] >> 48), (uint32)(version[0] >> 32) & 0xFFFF,
			(uint32)version[0], (uint32)version[1]);
	}
#endif

	return B_OK;
}


status_t
VMBus::_EnableHypercalls()
{
	// No hypercall page on arm64. The guest OS ID must be set (on the boot CPU)
	// before any hypercall other than Get/SetVpRegisters.
	uint16 status = hv_set_vpreg(HV_REG_GUEST_OS_ID, HV_GUEST_OS_ID_FREEBSD);
	if (status != 0) {
		ERROR("Setting guest OS id failed, status 0x%x\n", status);
		return B_ERROR;
	}

	uint64 guestID = hv_get_vpreg64(HV_REG_GUEST_OS_ID);
	if (guestID != HV_GUEST_OS_ID_FREEBSD) {
		ERROR("Guest OS id readback 0x%" B_PRIx64 " != 0x%" B_PRIx64 "\n", guestID,
			HV_GUEST_OS_ID_FREEBSD);
		return B_ERROR;
	}

	TRACE("Hypercalls enabled, guest OS id 0x%" B_PRIx64 "\n", guestID);
	return B_OK;
}


void
VMBus::_DisableHypercalls()
{
	// Clearing the guest OS ID disables hypercalls
	hv_set_vpreg(HV_REG_GUEST_OS_ID, 0);
	TRACE("Hypercalls disabled\n");
}


uint16
VMBus::_HypercallPostMessage(phys_addr_t physAddr)
{
	// Memory form: x2 = input GPA, x3 = output GPA (none)
	uint64 regs[6] = { physAddr, 0, 0, 0, 0, 0 };
	memory_full_barrier();
	return (uint16)(hv_hvcall(HYPERCALL_POST_MESSAGE, regs) & HV_HYPERCALL_STATUS_MASK);
}


uint16
VMBus::_HypercallSignalEvent(uint32 connId)
{
	// Fast form (HYPERCALL_SIGNAL_EVENT has the fast bit): x2 = connection id
	uint64 regs[6] = { connId, 0, 0, 0, 0, 0 };
	return (uint16)(hv_hvcall(HYPERCALL_SIGNAL_EVENT, regs) & HV_HYPERCALL_STATUS_MASK);
}


status_t
VMBus::_SetInterruptVector()
{
	// ACPI interrupt == GIC INTID == io interrupt vector. The SINT vector field
	// takes the INTID itself and is 8 bits wide.
	if (fIRQ > 255) {
		ERROR("VMBus INTID %" B_PRId32 " does not fit the SINT vector\n", fIRQ);
		return B_BAD_VALUE;
	}
	fInterruptVector = fIRQ;
	return B_OK;
}


// Read-modify-write of one synthetic register, ORing any failure into *_status
static void
hv_update_vpreg(uint32 name, uint64 keepMask, uint64 setBits, uint16* _status)
{
	uint64 value, high;
	uint16 status = hv_get_vpreg(name, &value, &high);
	if (status != 0) {
		*_status |= status;
		return;
	}
	value = (value & keepMask) | setBits;
	*_status |= hv_set_vpreg(name, value);

	uint64 readBack;
	status = hv_get_vpreg(name, &readBack, &high);
	if (status != 0)
		*_status |= status;
	else if (readBack != value)
		*_status |= 0x8000;
}


void
VMBus::_EnableInterruptCPU(int32 cpu)
{
	phys_addr_t messagesPhys = fCPUMessagesPhys + (sizeof(*fCPUMessages) * cpu);
	phys_addr_t eventFlagsPhys = fCPUEventFlagsPhys + (sizeof(*fCPUEventFlags) * cpu);
	uint16 status = 0;

	hv_update_vpreg(HV_REG_SIMP, HV_SIMP_RSVD_MASK,
		((messagesPhys >> HV_PAGE_SHIFT) << HV_SIMP_PAGE_SHIFT) | HV_SIMP_ENABLE, &status);
	hv_update_vpreg(HV_REG_SIEFP, HV_SIEFP_RSVD_MASK,
		((eventFlagsPhys >> HV_PAGE_SHIFT) << HV_SIEFP_PAGE_SHIFT) | HV_SIEFP_ENABLE, &status);

	// Messages arrive as the GIC INTID in the vector field. AutoEOI must stay
	// clear on arm64, the GIC EOI is done by the kernel. SINT4 (synthetic timer)
	// is not used: arm64 uses the generic timer.
	hv_update_vpreg(HV_REG_SINT0 + VMBUS_SINT_MESSAGE, HV_SINT_RSVD_MASK, fInterruptVector,
		&status);

	hv_update_vpreg(HV_REG_SCONTROL, HV_SCONTROL_RSVD_MASK, HV_SCONTROL_ENABLE, &status);

	// The interrupt is a PPI: install_io_interrupt_handler() only enabled it on
	// the CPU it ran on, each CPU's redistributor needs its own enable.
	arch_int_enable_io_interrupt(fInterruptVector);

	// Runs with interrupts disabled on each CPU, nothing to unwind: report only
	if (status != 0)
		ERROR("cpu%" B_PRId32 ": SynIC setup failed (0x%x)\n", cpu, status);
	TRACE("cpu%" B_PRId32 ": SynIC enabled, sint%d vector %u\n", cpu, VMBUS_SINT_MESSAGE,
		fInterruptVector);
}


void
VMBus::_DisableInterruptCPU(int32 cpu)
{
	uint16 status = 0;
	hv_update_vpreg(HV_REG_SCONTROL, HV_SCONTROL_RSVD_MASK, 0, &status);
	hv_update_vpreg(HV_REG_SIMP, HV_SIMP_RSVD_MASK, 0, &status);
	hv_update_vpreg(HV_REG_SIEFP, HV_SIEFP_RSVD_MASK, 0, &status);
	hv_update_vpreg(HV_REG_SINT0 + VMBUS_SINT_MESSAGE, HV_SINT_RSVD_MASK, HV_SINT_MASKED,
		&status);
	if (status != 0)
		ERROR("cpu%" B_PRId32 ": SynIC disable failed (0x%x)\n", cpu, status);
}


/*static*/ void
VMBus::_SignalEom(void*, int)
{
	hv_set_vpreg(HV_REG_EOM, 0);
}
