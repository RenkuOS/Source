/*
 * Copyright 2024-2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 *
 * Driver for the Synopsys DesignWare Mobile Storage Host Controller
 * ("snps,dw-mshc"), as found in the StarFive JH7110 (VisionFive 2, Milk-V
 * Mars). Based on X547's driver for the device_manager2 branch, adapted to the
 * mmc_bus_interface of the current MMC stack.
 *
 * Data is moved with the internal DMA controller (IDMAC) in chained
 * descriptor mode, with 32 or 64 bit descriptors as the controller was built.
 * Controllers without the IDMAC are not supported.
 */


#include <new>
#include <stdio.h>
#include <string.h>

#include <bus/FDT.h>
#include <ByteOrder.h>
#include <KernelExport.h>
#include <condition_variable.h>
#include <lock.h>
#include <util/AutoLock.h>

#include "IORequest.h"
#include "mmc.h"

#include "designware_mmc.h"


//#define TRACE_DW_MMC
#ifdef TRACE_DW_MMC
#	define TRACE(x...) dprintf("dw_mmc: " x)
#else
#	define TRACE(x...) ;
#endif
#define ERROR(x...)	dprintf("dw_mmc: " x)


#define DW_MMC_DEVICE_MODULE_NAME	"busses/mmc/designware_mmc/driver_v1"
#define DW_MMC_BUS_MODULE_NAME		"busses/mmc/designware_mmc/device/v1"

// Card interface unit clock when the device tree doesn't tell
#define DW_MMC_DEFAULT_CIU_HZ		50000000

static const uint32 kBlockSize = 512;
static const uint32 kDescriptorCount = 128;
static const bigtime_t kCommandTimeout = 1000000;
static const bigtime_t kDataTimeout = 5000000;


static device_manager_info* sDeviceManager;


static const char* const kCompatible[] = {
	"starfive,jh7110-mmc",
	"snps,dw-mshc",
};


// #pragma mark - device tree helpers


static fdt_device_module_info*
get_fdt(device_node* node, fdt_device** _dev)
{
	fdt_device_module_info* module;
	if (sDeviceManager->get_driver(node, (driver_module_info**)&module,
			(void**)_dev) != B_OK) {
		return NULL;
	}
	return module;
}


static bool
is_compatible(device_node* node)
{
	const char* compatible;
	if (sDeviceManager->get_attr_string(node, "fdt/compatible", &compatible,
			false) != B_OK) {
		return false;
	}
	for (size_t i = 0; i < B_COUNT_OF(kCompatible); i++) {
		if (strcmp(compatible, kCompatible[i]) == 0)
			return true;
	}
	return false;
}


static bool
get_prop_u32(fdt_device_module_info* module, fdt_device* dev,
	const char* name, uint32* value)
{
	int length;
	const uint32* prop = (const uint32*)module->get_prop(dev, name, &length);
	if (prop == NULL || length < 4)
		return false;
	*value = B_BENDIAN_TO_HOST_INT32(*prop);
	return true;
}


/*!	Rate of the "ciu" clock: from a fixed clock provider, else from
	"assigned-clock-rates" (firmware sets the clock up as the device tree asks,
	e.g. 50 MHz on the JH7110), else a default.
*/
static uint32
get_ciu_rate(fdt_device_module_info* module, fdt_device* dev)
{
	uint32 rate;
	if (get_prop_u32(module, dev, "clock-frequency", &rate) && rate != 0)
		return rate;

	int namesLength;
	const char* names = (const char*)module->get_prop(dev, "clock-names",
		&namesLength);
	int clocksLength;
	const uint32* clocks = (const uint32*)module->get_prop(dev, "clocks",
		&clocksLength);
	if (names != NULL && clocks != NULL) {
		// find the "ciu" entry, walking "clocks" with each provider's
		// #clock-cells
		device_node* busNode = module->get_bus(dev);
		fdt_bus_module_info* busModule;
		fdt_bus* bus;
		if (sDeviceManager->get_driver(busNode,
				(driver_module_info**)&busModule, (void**)&bus) == B_OK) {
			const char* name = names;
			int cell = 0;
			int cellCount = clocksLength / 4;
			while (name < names + namesLength && cell < cellCount) {
				uint32 phandle = B_BENDIAN_TO_HOST_INT32(clocks[cell]);
				device_node* provider = busModule->node_by_phandle(bus,
					phandle);
				fdt_device* providerDev = NULL;
				fdt_device_module_info* providerModule = provider != NULL
					? get_fdt(provider, &providerDev) : NULL;
				uint32 clockCells = 0;
				if (providerModule != NULL) {
					get_prop_u32(providerModule, providerDev, "#clock-cells",
						&clockCells);
				}
				if (strcmp(name, "ciu") == 0) {
					if (providerModule != NULL && get_prop_u32(providerModule,
							providerDev, "clock-frequency", &rate)
						&& rate != 0) {
						return rate;
					}
					break;
				}
				cell += 1 + clockCells;
				name += strlen(name) + 1;
			}
		}
	}

	// TODO: take the matching entry when "assigned-clocks" lists more than
	// one clock
	if (get_prop_u32(module, dev, "assigned-clock-rates", &rate) && rate != 0)
		return rate;

	ERROR("no rate for the ciu clock, assuming %u Hz\n",
		DW_MMC_DEFAULT_CIU_HZ);
	return DW_MMC_DEFAULT_CIU_HZ;
}


// #pragma mark - DwMmc


class DwMmc {
public:
								DwMmc();
								~DwMmc();

			status_t			Init(device_node* fdtNode);

			status_t			SetClock(uint32 kilohertz);
			status_t			ExecuteCommand(uint8 command, uint32 argument,
									uint32* response);
			status_t			DoIO(uint8 command, IOOperation* operation,
									bool offsetAsSectors);
			void				SetScanSemaphore(sem_id sem);
			void				SetBusWidth(int width);
			void				SetCardType(card_type type);

private:
			uint32				_Read(uint32 offset)
									{ return fRegs[offset / 4]; }
			void				_Write(uint32 offset, uint32 value)
									{ fRegs[offset / 4] = value; }

			status_t			_ResetBits(uint32 bits);
			void				_ResetData();
			status_t			_WaitNotBusy(bigtime_t timeout);
			status_t			_SendClockUpdate();
			status_t			_CommandFlags(uint8 command, uint32* flags,
									bool* busy, bool* longResponse);
			status_t			_Execute(uint8 command, uint32 argument,
									uint32 extraFlags, uint32* response,
									bool waitData);
			status_t			_WaitInterrupt(uint32 mask,
									bigtime_t timeout, uint32* status);
			status_t			_SetupDescriptors(IOOperation* operation,
									uint32* _length);

	static	int32				_HandleInterrupt(void* data);
			int32				_HandleInterrupt();

private:
			area_id				fRegsArea;
			volatile uint32*	fRegs;
			long				fIrq;
			bool				fInterruptInstalled;

			uint32				fCiuRate;
			uint32				fClockKHz;
			uint32				fFifoDepth;
			bool				f64BitDma;
			bool				fNeedInit;
			bool				fUseCardDetect;
			card_type			fCardType;
			sem_id				fScanSemaphore;

			area_id				fDescArea;
			void*				fDescriptors;
			phys_addr_t			fDescPhys;

			mutex				fLock;
			spinlock			fStatusLock;
			uint32				fIntStatus;
			uint32				fIdStatus;
			ConditionVariable	fInterruptCondition;
};


DwMmc::DwMmc()
	:
	fRegsArea(-1),
	fRegs(NULL),
	fIrq(-1),
	fInterruptInstalled(false),
	fCiuRate(0),
	fClockKHz(0),
	fFifoDepth(0),
	f64BitDma(false),
	fNeedInit(true),
	fUseCardDetect(false),
	fCardType(CARD_TYPE_UNKNOWN),
	fScanSemaphore(-1),
	fDescArea(-1),
	fDescriptors(NULL),
	fDescPhys(0),
	fIntStatus(0),
	fIdStatus(0)
{
	mutex_init(&fLock, "dw_mmc");
	B_INITIALIZE_SPINLOCK(&fStatusLock);
	fInterruptCondition.Init(this, "dw_mmc interrupt");
}


DwMmc::~DwMmc()
{
	if (fRegs != NULL) {
		_Write(DW_MMC_INTMASK, 0);
		_Write(DW_MMC_CTRL, _Read(DW_MMC_CTRL) & ~DW_MMC_CTRL_INT_ENABLE);
	}
	if (fInterruptInstalled)
		remove_io_interrupt_handler(fIrq, _HandleInterrupt, this);
	if (fDescArea >= 0)
		delete_area(fDescArea);
	if (fRegsArea >= 0)
		delete_area(fRegsArea);
	mutex_destroy(&fLock);
}


status_t
DwMmc::Init(device_node* fdtNode)
{
	fdt_device* dev;
	fdt_device_module_info* module = get_fdt(fdtNode, &dev);
	if (module == NULL)
		return B_ERROR;

	uint64 regs, regsLength;
	if (!module->get_reg(dev, 0, &regs, &regsLength)) {
		ERROR("no registers\n");
		return B_BAD_DATA;
	}
	uint64 irq;
	if (!module->get_interrupt(dev, 0, NULL, &irq)) {
		ERROR("no interrupt\n");
		return B_BAD_DATA;
	}
	fIrq = irq;

	// before the first register access: an unclocked controller can stall
	// the bus, and this line then shows which one
	dprintf("dw_mmc: init %#" B_PRIx64 "\n", regs);

	fRegsArea = map_physical_memory("dw_mmc registers", regs,
		max_c(regsLength, (uint64)DW_MMC_REGS_SIZE), B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fRegs);
	if (fRegsArea < B_OK)
		return fRegsArea;

	uint32 hcon = _Read(DW_MMC_HCON);
	if (DW_MMC_HCON_TRANS_MODE(hcon) != DW_MMC_HCON_TRANS_IDMAC) {
		ERROR("controller without internal DMA, not supported\n");
		return B_NOT_SUPPORTED;
	}
	f64BitDma = (hcon & DW_MMC_HCON_ADDR_CONFIG_64) != 0;

	fCiuRate = get_ciu_rate(module, dev);

	// As Linux: the controller's card detect line is only meaningful when
	// the card is not detected through a GPIO, not soldered and not marked
	// as having broken card detection.
	int length;
	fUseCardDetect = module->get_prop(dev, "cd-gpios", &length) == NULL
		&& module->get_prop(dev, "broken-cd", &length) == NULL
		&& module->get_prop(dev, "non-removable", &length) == NULL;

	if (!get_prop_u32(module, dev, "fifo-depth", &fFifoDepth)) {
		// the reset value of the RX watermark is the FIFO depth - 1
		fFifoDepth = DW_MMC_FIFOTH_RX_WMARK(_Read(DW_MMC_FIFOTH)) + 1;
	}
	if (fFifoDepth < 8 || fFifoDepth > 4096) {
		ERROR("bad FIFO depth %" B_PRIu32 "\n", fFifoDepth);
		return B_BAD_DATA;
	}

	dprintf("dw_mmc: %#" B_PRIx64 ", IRQ %ld, version %#" B_PRIx32
		", %d bit IDMAC, FIFO %" B_PRIu32 ", ciu %" B_PRIu32 " Hz\n", regs,
		fIrq, _Read(DW_MMC_VERID), f64BitDma ? 64 : 32, fFifoDepth, fCiuRate);

	// descriptor ring, in memory the IDMAC can reach
	size_t descSize = f64BitDma ? sizeof(dw_mmc_desc64) : sizeof(dw_mmc_desc32);
	fDescArea = create_area("dw_mmc descriptors", &fDescriptors,
		B_ANY_KERNEL_ADDRESS,
		(kDescriptorCount * descSize + B_PAGE_SIZE - 1) & ~(B_PAGE_SIZE - 1),
		f64BitDma ? B_CONTIGUOUS : B_32_BIT_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (fDescArea < B_OK)
		return fDescArea;
	physical_entry entry;
	status_t status = get_memory_map(fDescriptors, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK)
		return status;
	fDescPhys = entry.address;

	// power the slot and reset the controller, FIFO and DMA
	_Write(DW_MMC_PWREN, 1);
	status = _ResetBits(DW_MMC_CTRL_ALL_RESETS);
	if (status != B_OK) {
		ERROR("controller reset failed\n");
		return status;
	}

	_Write(DW_MMC_RINTSTS, DW_MMC_INT_ALL);
	_Write(DW_MMC_INTMASK, 0);
	_Write(DW_MMC_TMOUT, 0xffffffff);

	// FIFO thresholds as Linux sets them: burst of 8, TX at half, RX just
	// below half
	_Write(DW_MMC_FIFOTH, (2u << 28) | ((fFifoDepth / 2 - 1) << 16)
		| (fFifoDepth / 2));

	// internal DMA: software reset, no IDMAC interrupts (completion is
	// signalled by "data transfer over")
	_Write(DW_MMC_BMOD, DW_MMC_BMOD_SWR);
	if (f64BitDma) {
		_Write(DW_MMC_IDINTEN64, 0);
		_Write(DW_MMC_IDSTS64, DW_MMC_IDSTS_ALL);
	} else {
		_Write(DW_MMC_IDINTEN32, 0);
		_Write(DW_MMC_IDSTS32, DW_MMC_IDSTS_ALL);
	}

	status = install_io_interrupt_handler(fIrq, _HandleInterrupt, this, 0);
	if (status != B_OK) {
		ERROR("could not install interrupt handler\n");
		return status;
	}
	fInterruptInstalled = true;

	_Write(DW_MMC_INTMASK, DW_MMC_INT_CD | DW_MMC_INT_CMD_DONE
		| DW_MMC_INT_DTO | DW_MMC_INT_CMD_ERROR | DW_MMC_INT_DATA_ERROR);
	_Write(DW_MMC_CTRL, _Read(DW_MMC_CTRL) | DW_MMC_CTRL_INT_ENABLE);

	_Write(DW_MMC_CTYPE, DW_MMC_CTYPE_1BIT);
	return SetClock(400);
}


status_t
DwMmc::_ResetBits(uint32 bits)
{
	_Write(DW_MMC_CTRL, _Read(DW_MMC_CTRL) | bits);
	bigtime_t timeout = system_time() + 500000;
	while ((_Read(DW_MMC_CTRL) & bits) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		spin(10);
	}
	return B_OK;
}


void
DwMmc::_ResetData()
{
	// stop the internal DMA and flush the FIFO after a failed transfer
	_Write(DW_MMC_BMOD, DW_MMC_BMOD_SWR);
	_Write(DW_MMC_CTRL, _Read(DW_MMC_CTRL)
		& ~(DW_MMC_CTRL_DMA_ENABLE | DW_MMC_CTRL_USE_IDMAC));
	if (_ResetBits(DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET) != B_OK)
		ERROR("FIFO/DMA reset failed\n");
}


status_t
DwMmc::_WaitNotBusy(bigtime_t timeout)
{
	bigtime_t end = system_time() + timeout;
	while ((_Read(DW_MMC_STATUS) & DW_MMC_STATUS_DATA_BUSY) != 0) {
		if (system_time() > end)
			return B_TIMED_OUT;
		snooze(10);
	}
	return B_OK;
}


status_t
DwMmc::_SendClockUpdate()
{
	_Write(DW_MMC_CMD, DW_MMC_CMD_START | DW_MMC_CMD_UPD_CLK
		| DW_MMC_CMD_PRV_DAT_WAIT);
	bigtime_t timeout = system_time() + 500000;
	while ((_Read(DW_MMC_CMD) & DW_MMC_CMD_START) != 0) {
		if (system_time() > timeout) {
			ERROR("clock update not accepted\n");
			return B_TIMED_OUT;
		}
		spin(10);
	}
	return B_OK;
}


status_t
DwMmc::SetClock(uint32 kilohertz)
{
	if (kilohertz == 0 || kilohertz == fClockKHz)
		return B_OK;

	uint32 hertz = kilohertz * 1000;
	// card clock = ciu / (2 * divider), divider 0 bypasses
	uint32 divider = fCiuRate <= hertz
		? 0 : (fCiuRate + 2 * hertz - 1) / (2 * hertz);

	_WaitNotBusy(500000);

	_Write(DW_MMC_CLKENA, 0);
	_Write(DW_MMC_CLKSRC, 0);
	status_t status = _SendClockUpdate();
	if (status != B_OK)
		return status;

	_Write(DW_MMC_CLKDIV, divider);
	status = _SendClockUpdate();
	if (status != B_OK)
		return status;

	_Write(DW_MMC_CLKENA, DW_MMC_CLKENA_ENABLE);
	status = _SendClockUpdate();
	if (status != B_OK)
		return status;

	fClockKHz = kilohertz;
	TRACE("card clock %" B_PRIu32 " kHz (divider %" B_PRIu32 ")\n",
		divider == 0 ? fCiuRate / 1000 : fCiuRate / (2000 * divider),
		divider);
	return B_OK;
}


/*!	Response type of each command the MMC stack sends, as in the SDHCI
	driver (some numbers mean different commands on SD and MMC cards).
*/
status_t
DwMmc::_CommandFlags(uint8 command, uint32* flags, bool* busy,
	bool* longResponse)
{
	static const uint32 kShort = DW_MMC_CMD_RESP_EXP | DW_MMC_CMD_RESP_CRC;
	*busy = false;
	*longResponse = false;

	switch (command) {
		case GO_IDLE_STATE:
			*flags = 0;
			break;
		case SD_APP_CMD:
		case SD_ERASE_WR_BLK_START:
		case SD_ERASE_WR_BLK_END:
		case SD_READ_SINGLE_BLOCK:
		case SD_READ_MULTIPLE_BLOCKS:
		case SD_WRITE_SINGLE_BLOCK:
		case SD_WRITE_MULTIPLE_BLOCKS:
			*flags = kShort;
			break;
		case SELECT_DESELECT_CARD:
		case SD_ERASE:
			*flags = kShort;
			*busy = true;
			break;
		case ALL_SEND_CID:
		case SEND_CSD:
			*flags = kShort | DW_MMC_CMD_RESP_LONG;
			*longResponse = true;
			break;
		case MMC_SEND_OP_COND:
		case SD_SEND_OP_COND:
			// R3: no CRC
			*flags = DW_MMC_CMD_RESP_EXP;
			break;
		case SD_SET_BUS_WIDTH:		// also MMC_SWITCH (R1b)
			*flags = kShort;
			*busy = fCardType == CARD_TYPE_MMC;
			break;
		case SD_SEND_RELATIVE_ADDR:	// also MMC_SET_RELATIVE_ADDR
		case SD_SEND_IF_COND:		// also MMC_SEND_EXT_CSD
			*flags = kShort;
			break;
		case SDIO_IO_SEND_OP_COND:
			// the bus manager probes every card for SDIO; not supported
			return B_UNSUPPORTED;
		default:
			ERROR("unknown command %u\n", command);
			return B_BAD_DATA;
	}
	return B_OK;
}


status_t
DwMmc::_WaitInterrupt(uint32 mask, bigtime_t timeout, uint32* _status)
{
	bigtime_t end = system_time() + timeout;
	while (true) {
		ConditionVariableEntry entry;
		fInterruptCondition.Add(&entry);

		cpu_status state = disable_interrupts();
		acquire_spinlock(&fStatusLock);
		uint32 status = fIntStatus;
		if ((status & mask) != 0)
			fIntStatus &= ~mask;
		release_spinlock(&fStatusLock);
		restore_interrupts(state);

		if ((status & mask) != 0) {
			*_status = status & mask;
			return B_OK;
		}

		bigtime_t left = end - system_time();
		if (left <= 0)
			return B_TIMED_OUT;
		entry.Wait(B_RELATIVE_TIMEOUT, left);
	}
}


status_t
DwMmc::_Execute(uint8 command, uint32 argument, uint32 extraFlags,
	uint32* response, bool waitData)
{
	uint32 flags;
	bool busy, longResponse;
	status_t status = _CommandFlags(command, &flags, &busy, &longResponse);
	if (status != B_OK)
		return status;

	if (_WaitNotBusy(500000) != B_OK) {
		ERROR("card busy before command %u\n", command);
		return B_BUSY;
	}

	flags |= extraFlags | command | DW_MMC_CMD_START | DW_MMC_CMD_USE_HOLD_REG
		| DW_MMC_CMD_PRV_DAT_WAIT;
	if (command == GO_IDLE_STATE || fNeedInit) {
		// 80 clock cycles before the first command
		flags |= DW_MMC_CMD_INIT;
		fNeedInit = false;
	}

	cpu_status state = disable_interrupts();
	acquire_spinlock(&fStatusLock);
	fIntStatus = 0;
	release_spinlock(&fStatusLock);
	restore_interrupts(state);

	_Write(DW_MMC_CMDARG, argument);
	_Write(DW_MMC_CMD, flags);

	uint32 result;
	status = _WaitInterrupt(DW_MMC_INT_CMD_DONE | DW_MMC_INT_CMD_ERROR,
		kCommandTimeout, &result);
	if (status != B_OK) {
		ERROR("command %u: no completion\n", command);
		return B_TIMED_OUT;
	}
	if ((result & DW_MMC_INT_RTO) != 0) {
		// expected while probing (SD or MMC, empty slot): not an error
		TRACE("command %u: response timeout\n", command);
		return B_TIMED_OUT;
	}
	if ((result & DW_MMC_INT_CMD_ERROR) != 0) {
		ERROR("command %u failed, status %#" B_PRIx32 "\n", command, result);
		return (result & DW_MMC_INT_RCRC) != 0 ? B_BAD_VALUE : B_IO_ERROR;
	}

	if (response != NULL && (flags & DW_MMC_CMD_RESP_EXP) != 0) {
		if (longResponse) {
			// The controller returns all 128 bits; the MMC stack expects the
			// SDHCI layout, without the CRC byte (bits 127-8 shifted down).
			uint32 r0 = _Read(DW_MMC_RESP0);
			uint32 r1 = _Read(DW_MMC_RESP1);
			uint32 r2 = _Read(DW_MMC_RESP2);
			uint32 r3 = _Read(DW_MMC_RESP3);
			response[0] = (r0 >> 8) | (r1 << 24);
			response[1] = (r1 >> 8) | (r2 << 24);
			response[2] = (r2 >> 8) | (r3 << 24);
			response[3] = r3 >> 8;
		} else
			response[0] = _Read(DW_MMC_RESP0);
	}

	if (waitData) {
		status = _WaitInterrupt(DW_MMC_INT_DTO | DW_MMC_INT_DATA_ERROR,
			kDataTimeout, &result);
		uint32 idStatus = _Read(f64BitDma ? DW_MMC_IDSTS64 : DW_MMC_IDSTS32);
		_Write(f64BitDma ? DW_MMC_IDSTS64 : DW_MMC_IDSTS32, idStatus);
		if (status != B_OK || (result & DW_MMC_INT_DATA_ERROR) != 0
			|| (idStatus & DW_MMC_IDSTS_ERROR) != 0) {
			ERROR("command %u: data transfer failed (%s, status %#" B_PRIx32
				", IDMAC %#" B_PRIx32 ")\n", command, strerror(status), result,
				idStatus);
			_ResetData();
			return status != B_OK ? status : B_IO_ERROR;
		}
	}

	if (busy && _WaitNotBusy(kDataTimeout) != B_OK) {
		ERROR("command %u: card stays busy\n", command);
		return B_TIMED_OUT;
	}

	return B_OK;
}


status_t
DwMmc::ExecuteCommand(uint8 command, uint32 argument, uint32* response)
{
	MutexLocker locker(fLock);
	return _Execute(command, argument, 0, response, false);
}


/*!	Fills the descriptor chain from the physical vecs of \a operation. */
status_t
DwMmc::_SetupDescriptors(IOOperation* operation, uint32* _length)
{
	const generic_io_vec* vecs = operation->Vecs();
	uint32 vecCount = operation->VecCount();
	generic_size_t length = operation->Length();

	uint32 index = 0;
	uint32 total = 0;
	for (uint32 i = 0; i < vecCount && total < length; i++) {
		generic_addr_t base = vecs[i].base;
		generic_size_t left = min_c(vecs[i].length, length - total);
		while (left > 0) {
			if (index >= kDescriptorCount) {
				ERROR("transfer needs more than %" B_PRIu32 " descriptors\n",
					kDescriptorCount);
				return B_BAD_VALUE;
			}
			uint32 size = min_c(left, (generic_size_t)DW_MMC_DESC_MAX_BYTES);
			uint32 flags = DW_MMC_DES0_OWN | DW_MMC_DES0_CH
				| DW_MMC_DES0_DIC;
			if (index == 0)
				flags |= DW_MMC_DES0_FS;

			if (f64BitDma) {
				dw_mmc_desc64* desc = (dw_mmc_desc64*)fDescriptors + index;
				phys_addr_t next = fDescPhys + (index + 1) * sizeof(*desc);
				desc->des1 = 0;
				desc->des2 = size;
				desc->des3 = 0;
				desc->des4 = (uint32)base;
				desc->des5 = (uint32)((uint64)base >> 32);
				desc->des6 = (uint32)next;
				desc->des7 = (uint32)((uint64)next >> 32);
				desc->des0 = flags;
			} else {
				if (base + size > 0x100000000ULL) {
					ERROR("buffer above 4 GB for a 32 bit IDMAC\n");
					return B_BAD_VALUE;
				}
				dw_mmc_desc32* desc = (dw_mmc_desc32*)fDescriptors + index;
				desc->des1 = size;
				desc->des2 = (uint32)base;
				desc->des3 = (uint32)(fDescPhys + (index + 1) * sizeof(*desc));
				desc->des0 = flags;
			}

			base += size;
			left -= size;
			total += size;
			index++;
		}
	}
	if (index == 0 || total != length)
		return B_BAD_VALUE;

	// last descriptor: interrupt allowed, end of chain
	uint32* des0 = f64BitDma
		? &((dw_mmc_desc64*)fDescriptors)[index - 1].des0
		: &((dw_mmc_desc32*)fDescriptors)[index - 1].des0;
	*des0 = (*des0 & ~(DW_MMC_DES0_DIC | DW_MMC_DES0_CH)) | DW_MMC_DES0_LD;

	__sync_synchronize();
	*_length = total;
	return B_OK;
}


status_t
DwMmc::DoIO(uint8 command, IOOperation* operation, bool offsetAsSectors)
{
	off_t offset = operation->Offset();
	if (offset % kBlockSize != 0 || operation->Length() % kBlockSize != 0)
		return B_BAD_VALUE;

	MutexLocker locker(fLock);

	uint32 length;
	status_t status = _SetupDescriptors(operation, &length);
	if (status != B_OK)
		return status;

	bool isWrite = operation->IsWrite();
	bool multiple = command == SD_READ_MULTIPLE_BLOCKS
		|| command == SD_WRITE_MULTIPLE_BLOCKS;

	if (_WaitNotBusy(500000) != B_OK)
		return B_BUSY;

	// fresh FIFO and DMA state for each transfer
	if (_ResetBits(DW_MMC_CTRL_FIFO_RESET) != B_OK)
		return B_IO_ERROR;
	_Write(DW_MMC_BMOD, DW_MMC_BMOD_SWR);

	_Write(DW_MMC_BLKSIZ, kBlockSize);
	_Write(DW_MMC_BYTCNT, length);

	if (f64BitDma) {
		_Write(DW_MMC_DBADDRL, (uint32)fDescPhys);
		_Write(DW_MMC_DBADDRU, (uint32)((uint64)fDescPhys >> 32));
		_Write(DW_MMC_IDSTS64, DW_MMC_IDSTS_ALL);
	} else {
		_Write(DW_MMC_DBADDR, (uint32)fDescPhys);
		_Write(DW_MMC_IDSTS32, DW_MMC_IDSTS_ALL);
	}
	_Write(DW_MMC_CTRL, _Read(DW_MMC_CTRL) | DW_MMC_CTRL_DMA_ENABLE
		| DW_MMC_CTRL_USE_IDMAC);
	_Write(DW_MMC_BMOD, DW_MMC_BMOD_FB | DW_MMC_BMOD_DE);
	_Write(DW_MMC_PLDMND, 1);

	uint32 flags = DW_MMC_CMD_DAT_EXP;
	if (isWrite)
		flags |= DW_MMC_CMD_DAT_WR;
	if (multiple)
		flags |= DW_MMC_CMD_SEND_STOP;	// CMD12 sent by the controller

	uint32 response;
	status = _Execute(command, offset / (offsetAsSectors ? kBlockSize : 1),
		flags, &response, true);

	_Write(DW_MMC_CTRL, _Read(DW_MMC_CTRL)
		& ~(DW_MMC_CTRL_DMA_ENABLE | DW_MMC_CTRL_USE_IDMAC));
	_Write(DW_MMC_BMOD, 0);

	if (status != B_OK)
		return status;

	// a write is only done when the card has programmed the blocks
	if (isWrite && _WaitNotBusy(kDataTimeout) != B_OK)
		return B_TIMED_OUT;

	return B_OK;
}


void
DwMmc::SetScanSemaphore(sem_id sem)
{
	fScanSemaphore = sem;

	// Without a usable card detect line (GPIO, soldered eMMC) scan once now;
	// otherwise only when a card is there (CDETECT is active low). Later
	// insertions come as card detect interrupts.
	bool present = !fUseCardDetect || (_Read(DW_MMC_CDETECT) & 1) == 0;
	dprintf("dw_mmc: card detect %s, %s\n",
		fUseCardDetect ? "from the controller" : "not used",
		present ? "scanning" : "slot empty");
	if (present)
		release_sem_etc(fScanSemaphore, 1, B_DO_NOT_RESCHEDULE);
}


void
DwMmc::SetBusWidth(int width)
{
	MutexLocker locker(fLock);
	switch (width) {
		case 8:
			_Write(DW_MMC_CTYPE, DW_MMC_CTYPE_8BIT);
			break;
		case 4:
			_Write(DW_MMC_CTYPE, DW_MMC_CTYPE_4BIT);
			break;
		default:
			_Write(DW_MMC_CTYPE, DW_MMC_CTYPE_1BIT);
			break;
	}
}


void
DwMmc::SetCardType(card_type type)
{
	fCardType = type;
}


int32
DwMmc::_HandleInterrupt(void* data)
{
	return ((DwMmc*)data)->_HandleInterrupt();
}


int32
DwMmc::_HandleInterrupt()
{
	uint32 status = _Read(DW_MMC_MINTSTS);
	if (status == 0)
		return B_UNHANDLED_INTERRUPT;

	_Write(DW_MMC_RINTSTS, status);

	if ((status & DW_MMC_INT_CD) != 0 && fScanSemaphore >= 0)
		release_sem_etc(fScanSemaphore, 1, B_DO_NOT_RESCHEDULE);

	acquire_spinlock(&fStatusLock);
	fIntStatus |= status;
	release_spinlock(&fStatusLock);

	fInterruptCondition.NotifyAll();
	return B_HANDLED_INTERRUPT;
}


// #pragma mark - mmc_bus_interface (one slot)


static status_t
init_bus(device_node* node, void** _cookie)
{
	// node: our slot; parent: the controller device; its parent: the FDT node
	device_node* parent = sDeviceManager->get_parent_node(node);
	device_node* fdtNode = sDeviceManager->get_parent_node(parent);
	sDeviceManager->put_node(parent);

	DwMmc* bus = new(std::nothrow) DwMmc;
	status_t status = bus != NULL ? bus->Init(fdtNode) : B_NO_MEMORY;
	sDeviceManager->put_node(fdtNode);

	if (status != B_OK) {
		ERROR("init failed: %s\n", strerror(status));
		delete bus;
		return status;
	}
	*_cookie = bus;
	return B_OK;
}


static void
uninit_bus(void* cookie)
{
	delete (DwMmc*)cookie;
}


static void
bus_removed(void* cookie)
{
}


static status_t
set_clock(void* controller, uint32_t kilohertz)
{
	return ((DwMmc*)controller)->SetClock(kilohertz);
}


static status_t
execute_command(void* controller, uint8_t command, uint32_t argument,
	uint32_t* response)
{
	return ((DwMmc*)controller)->ExecuteCommand(command, argument, response);
}


static status_t
do_io(void* controller, uint8_t command, IOOperation* operation,
	bool offsetAsSectors)
{
	return ((DwMmc*)controller)->DoIO(command, operation, offsetAsSectors);
}


static void
set_scan_semaphore(void* controller, sem_id sem)
{
	((DwMmc*)controller)->SetScanSemaphore(sem);
}


static void
set_bus_width(void* controller, int width)
{
	((DwMmc*)controller)->SetBusWidth(width);
}


static void
set_card_type(void* controller, card_type type)
{
	((DwMmc*)controller)->SetCardType(type);
}


static void
terminate_bus(void* controller)
{
}


// #pragma mark - controller device (binds to the FDT node)


static float
supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}
	if (!is_compatible(parent))
		return 0.0f;

	fdt_device* dev;
	fdt_device_module_info* module = get_fdt(parent, &dev);
	if (module == NULL)
		return 0.0f;
	int length;
	const char* status = (const char*)module->get_prop(dev, "status", &length);
	if (status != NULL && strcmp(status, "okay") != 0
		&& strcmp(status, "ok") != 0) {
		return 0.0f;
	}
	return 0.8f;
}


static status_t
register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "DesignWare MMC controller" } },
		{}
	};
	return sDeviceManager->register_node(parent, DW_MMC_DEVICE_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
init_device(device_node* node, void** _cookie)
{
	*_cookie = node;
	return B_OK;
}


static status_t
register_child_devices(void* cookie)
{
	device_node* node = (device_node*)cookie;

	// The DMA limits depend on how the controller was built: map it briefly
	// to read HCON.
	device_node* fdtNode = sDeviceManager->get_parent_node(node);
	fdt_device* dev;
	fdt_device_module_info* module = get_fdt(fdtNode, &dev);
	uint64 regs, regsLength;
	bool haveRegs = module != NULL
		&& module->get_reg(dev, 0, &regs, &regsLength);
	sDeviceManager->put_node(fdtNode);
	if (!haveRegs)
		return B_BAD_DATA;

	dprintf("dw_mmc: probe %#" B_PRIx64 "\n", regs);
	volatile uint32* registers;
	area_id area = map_physical_memory("dw_mmc probe", regs, DW_MMC_REGS_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, (void**)&registers);
	if (area < B_OK)
		return area;
	bool dma64 = (registers[DW_MMC_HCON / 4] & DW_MMC_HCON_ADDR_CONFIG_64)
		!= 0;
	delete_area(area);

	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "SD/MMC slot" } },
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{ .string = MMC_BUS_MODULE_NAME } },
		{ B_DEVICE_BUS, B_STRING_TYPE, { .string = "mmc" } },

		// whole sectors, each descriptor moving at most 4 KB
		{ B_DMA_ALIGNMENT, B_UINT32_TYPE, { .ui32 = kBlockSize - 1 } },
		{ B_DMA_HIGH_ADDRESS, B_UINT64_TYPE,
			{ .ui64 = dma64 ? ~(uint64)0 : 0x100000000ULL } },
		{ B_DMA_MAX_SEGMENT_COUNT, B_UINT32_TYPE,
			{ .ui32 = kDescriptorCount } },
		{ B_DMA_MAX_SEGMENT_BLOCKS, B_UINT32_TYPE,
			{ .ui32 = DW_MMC_DESC_MAX_BYTES / kBlockSize } },
		{ B_DMA_MAX_TRANSFER_BLOCKS, B_UINT32_TYPE,
			{ .ui32 = kDescriptorCount * DW_MMC_DESC_MAX_BYTES
				/ kBlockSize } },
		{}
	};

	return sDeviceManager->register_node(node, DW_MMC_BUS_MODULE_NAME, attrs,
		NULL, NULL);
}


static void
uninit_device(void* cookie)
{
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


static driver_module_info sControllerModule = {
	{
		DW_MMC_DEVICE_MODULE_NAME,
		0,
		NULL
	},
	supports_device,
	register_device,
	init_device,
	uninit_device,
	register_child_devices,
	NULL,	// rescan
	NULL,	// device removed
};


static mmc_bus_interface sSlotModule = {
	.info = {
		.info = {
			.name = DW_MMC_BUS_MODULE_NAME,
		},
		.init_driver = init_bus,
		.uninit_driver = uninit_bus,
		.device_removed = bus_removed,
	},

	.set_clock = set_clock,
	.execute_command = execute_command,
	.do_io = do_io,
	.set_scan_semaphore = set_scan_semaphore,
	.set_bus_width = set_bus_width,
	.terminate_bus = terminate_bus,
	.set_card_type = set_card_type,
};


module_info* modules[] = {
	(module_info*)&sControllerModule,
	(module_info*)&sSlotModule,
	NULL
};
