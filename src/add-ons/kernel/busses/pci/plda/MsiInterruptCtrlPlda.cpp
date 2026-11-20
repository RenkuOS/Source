/*
 * Copyright 2023, Haiku, Inc.
 * Copyright 2026, The RenkuOS Authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "PldaPCIController.h"

#include <interrupts.h>
#include <util/AutoLock.h>


uint32
MsiInterruptCtrlPlda::Read(uint32 offset)
{
	return *(vuint32*)(fRegs + offset);
}


void
MsiInterruptCtrlPlda::Write(uint32 offset, uint32 value)
{
	*(vuint32*)(fRegs + offset) = value;
}


status_t
MsiInterruptCtrlPlda::Init(addr_t regs, int32 irq)
{
	fRegs = regs;

	TRACE("MSI on IRQ %" B_PRId32 ", IMSI_ADDR register %#" B_PRIx32
		"\n", irq, Read(kPldaIMsiAddr));

	// only MSIs: INTx and the error events are not used
	Write(kPldaIMaskLocal, 0);
	Write(kPldaIStatusMsi, 0xffffffff);
	Write(kPldaIStatusLocal, kPldaIntAll);

	status_t result = allocate_io_interrupt_vectors(PLDA_MSI_VECTORS,
		&fMsiStartIrq, INTERRUPT_TYPE_IRQ);
	if (result != B_OK) {
		dprintf("plda: unable to allocate MSI vectors\n");
		return result;
	}

	result = install_io_interrupt_handler(irq, InterruptReceived, this, 0);
	if (result != B_OK) {
		dprintf("plda: unable to attach the MSI interrupt handler\n");
		free_io_interrupt_vectors(PLDA_MSI_VECTORS, fMsiStartIrq);
		return result;
	}

	Write(kPldaIMaskLocal, kPldaIntMsi);

	msi_set_interface(static_cast<MSIInterface*>(this));

	TRACE("MSI vectors %" B_PRId32 "-%" B_PRId32 "\n", fMsiStartIrq,
		fMsiStartIrq + PLDA_MSI_VECTORS - 1);
	return B_OK;
}


status_t
MsiInterruptCtrlPlda::AllocateVectors(uint32 count, uint32& startVector,
	uint64& address, uint32& data)
{
	if (count != 1)
		return B_ERROR;

	InterruptsSpinLocker locker(fLock);

	for (uint32 i = 0; i < PLDA_MSI_VECTORS; i++) {
		if ((fAllocatedMsiIrqs & (1u << i)) == 0) {
			fAllocatedMsiIrqs |= 1u << i;

			startVector = fMsiStartIrq + i;
			address = PLDA_MSI_ADDRESS;
			data = i;
			return B_OK;
		}
	}

	return B_ERROR;
}


void
MsiInterruptCtrlPlda::FreeVectors(uint32 count, uint32 startVector)
{
	InterruptsSpinLocker locker(fLock);

	int32 irq = (int32)startVector - fMsiStartIrq;
	while (count > 0) {
		if (irq >= 0 && irq < PLDA_MSI_VECTORS)
			fAllocatedMsiIrqs &= ~(1u << irq);
		irq++;
		count--;
	}
}


int32
MsiInterruptCtrlPlda::InterruptReceived(void* arg)
{
	return static_cast<MsiInterruptCtrlPlda*>(arg)->InterruptReceivedInt();
}


int32
MsiInterruptCtrlPlda::InterruptReceivedInt()
{
	uint32 status = Read(kPldaIStatusLocal);
	if ((status & kPldaIntMsi) == 0)
		return B_UNHANDLED_INTERRUPT;

	// As in Linux: acknowledge the summary bit first, then each vector, so
	// that an MSI arriving meanwhile raises the summary bit again.
	Write(kPldaIStatusLocal, kPldaIntMsi);

	uint32 msiStatus = Read(kPldaIStatusMsi);
	for (uint32 i = 0; i < PLDA_MSI_VECTORS; i++) {
		if ((msiStatus & (1u << i)) != 0) {
			Write(kPldaIStatusMsi, 1u << i);
			io_interrupt_handler(fMsiStartIrq + i, B_EDGE_TRIGGERED);
		}
	}

	return B_HANDLED_INTERRUPT;
}
