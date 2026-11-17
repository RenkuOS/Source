/*
 * Copyright 2023, Haiku, Inc.
 * Copyright 2026, The RenkuOS Authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 *
 * Based on the PLDA driver of X547's device_manager2 branch.
 */
#ifndef _PLDA_PCI_CONTROLLER_H_
#define _PLDA_PCI_CONTROLLER_H_


#include <bus/FDT.h>
#include <bus/PCI.h>
#include <arch/generic/msi.h>

#include <AutoDeleterOS.h>
#include <lock.h>
#include <util/Vector.h>


#define CHECK_RET(err) {status_t _err = (err); if (_err < B_OK) return _err;}

//#define TRACE_PLDA
#ifdef TRACE_PLDA
#	define TRACE(x...) dprintf("plda: " x)
#else
#	define TRACE(x...) do { if (false) dprintf("plda: " x); } while (false)
#endif

#define PLDA_PCI_DRIVER_MODULE_NAME "busses/pci/plda/driver_v1"


enum {
	fdtPciRangeConfig      = 0x00000000,
	fdtPciRangeIoPort      = 0x01000000,
	fdtPciRangeMmio32Bit   = 0x02000000,
	fdtPciRangeMmio64Bit   = 0x03000000,
	fdtPciRangeTypeMask    = 0x03000000,
	fdtPciRangePrefechable = 0x40000000,
};


// PLDA XpressRich bridge registers ("apb" region), from the Linux and U-Boot
// PLDA drivers
enum {
	kPldaGenSettings		= 0x080,
	kPldaPciIds				= 0x09c,
	kPldaPciMisc			= 0x0b4,
	kPldaWinRom				= 0x0fc,
	kPldaIMaskLocal			= 0x180,
	kPldaIStatusLocal		= 0x184,
	kPldaIMsiAddr			= 0x190,
	kPldaIStatusMsi			= 0x194,
	kPldaPmsgSupportRx		= 0x3f0,
	kPldaAtrAxi4Slv0		= 0x800,
	kPldaAtrTableSize		= 0x20,
	kPldaAtrTableCount		= 8,
};

enum {
	// ATR table entry
	kPldaAtrSrcAddrLow		= 0x00,
	kPldaAtrSrcAddrHigh		= 0x04,
	kPldaAtrTrslAddrLow		= 0x08,
	kPldaAtrTrslAddrHigh	= 0x0c,
	kPldaAtrTrslParam		= 0x10,

	kPldaAtrTrslIdMemory	= 0,
	kPldaAtrTrslIdConfig	= 1,
};

enum {
	// IMASK_LOCAL, ISTATUS_LOCAL
	kPldaIntIntxMask		= 0x0f000000,
	kPldaIntMsi				= 0x10000000,
	kPldaIntAerEvent		= 0x20000000,
	kPldaIntEvents			= 0x40000000,
	kPldaIntSysError		= 0x80000000,
	kPldaIntAll				= 0xffffffff,
};

// The device sends its MSIs to this PCI address, which the bridge captures
// (IMSI_ADDR in the Linux PLDA driver).
#define PLDA_MSI_ADDRESS		0x190
#define PLDA_MSI_VECTORS		32


class MsiInterruptCtrlPlda: public MSIInterface {
public:
			virtual				~MsiInterruptCtrlPlda() = default;

			status_t			Init(addr_t regs, int32 irq);

			status_t			AllocateVectors(uint32 count, uint32& startVector,
									uint64& address, uint32& data) final;
			void				FreeVectors(uint32 count, uint32 startVector) final;

private:
	static	int32				InterruptReceived(void* arg);
	inline	int32				InterruptReceivedInt();

	inline	uint32				Read(uint32 offset);
	inline	void				Write(uint32 offset, uint32 value);

private:
			spinlock			fLock = B_SPINLOCK_INITIALIZER;
			addr_t				fRegs {};
			uint32				fAllocatedMsiIrqs {};
			int32				fMsiStartIrq {};
};


class PldaPCIController {
public:
	static float SupportsDevice(device_node* parent);
	static status_t RegisterDevice(device_node* parent);
	static status_t InitDriver(device_node* node, PldaPCIController*& outDriver);
	void UninitDriver();

	status_t ReadConfig(uint8 bus, uint8 device, uint8 function,
				uint16 offset, uint8 size, uint32& value);
	status_t WriteConfig(uint8 bus, uint8 device, uint8 function,
				uint16 offset, uint8 size, uint32 value);

	status_t GetMaxBusDevices(int32& count);

	status_t ReadIrq(uint8 bus, uint8 device, uint8 function,
				uint8 pin, uint8& irq);
	status_t WriteIrq(uint8 bus, uint8 device, uint8 function,
				uint8 pin, uint8 irq);

	status_t GetRange(uint32 index, pci_resource_range* range);

private:
	inline status_t InitDriverInt(device_node* node);
	status_t ReadResourceInfo();
	status_t CheckJH7110Port();
	status_t EnableJH7110Port(fdt_device_module_info* fdtModule,
		fdt_device* fdtDev);
	void SetupJH7110RootPort();
	status_t WaitForJH7110Link(bigtime_t timeout);
	void ConfigureJH7110Bridge();
#ifdef TRACE_PLDA
	void DumpJH7110Bus();
#endif
	void SetPerst(bool assert);
	uint32 StgSysconReg(uint32 offset);
	void SetAtrEntry(uint32 index, phys_addr_t srcAddr, phys_addr_t trslAddr,
		uint64 windowSize, uint32 trslParam);
	void DumpAtr();

	inline addr_t ConfigAddress(uint8 bus, uint8 device, uint8 function,
		uint16 offset);
	inline bool IsHiddenRootPortBar(uint8 bus, uint8 device, uint8 function,
		uint16 offset);

	inline uint32 ReadReg(uint32 offset)
		{ return *(vuint32*)(fRegsBase + offset); }
	inline void WriteReg(uint32 offset, uint32 value)
		{ *(vuint32*)(fRegsBase + offset) = value; }

private:
	spinlock fLock = B_SPINLOCK_INITIALIZER;

	device_node* fNode {};
	uint32 fDomain {};
	uint8 fSecondaryBus = 1;
	uint32 fPerstGpio {};

	AreaDeleter fConfigArea;
	phys_addr_t fConfigPhysBase {};
	addr_t fConfigBase {};
	size_t fConfigSize {};

	AreaDeleter fRegsArea;
	phys_addr_t fRegsPhysBase {};
	addr_t fRegsBase {};
	size_t fRegsSize {};

	Vector<pci_resource_range> fResourceRanges;

	MsiInterruptCtrlPlda fIrqCtrl;
};


extern device_manager_info* gDeviceManager;

#endif	// _PLDA_PCI_CONTROLLER_H_
