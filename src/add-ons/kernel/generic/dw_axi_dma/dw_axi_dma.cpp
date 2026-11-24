/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 *
 * Synopsys DesignWare AXI DMA controller (DW_axi_dmac), as found in the
 * StarFive JH7110 ("starfive,jh7110-axi-dma"). See <dw_axi_dma.h>.
 *
 * Only cyclic transfers between a memory ring and one device register are
 * supported, which is what audio needs. Every period is one linked list
 * item marked as the last one: the channel stops after each period, and the
 * interrupt handler marks the items valid again and restarts the channel.
 * The controller fetches the next item from CH_LLP, which then names the
 * period it works on.
 *
 * The register layout, the JH7110 specifics (CFG2 layout of the channel
 * configuration with the 8 channel enable layout, handshake number from the
 * "dmas" cell, no APB registers) and the cyclic scheme were checked against
 * Linux's drivers/dma/dw-axi-dmac.
 */


#include <new>
#include <stdlib.h>
#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <bus/FDT.h>
#include <device_manager.h>
#include <dw_axi_dma.h>
#include <fdt_clock_reset.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>


#define INFO(x...)	dprintf("dw_axi_dma: " x)
#define ERROR(x...)	dprintf("dw_axi_dma: " x)


// common registers
#define DMAC_ID					0x000
#define DMAC_COMPVER			0x008
#define DMAC_CFG				0x010
#define		DMAC_CFG_EN				(1u << 0)
#define		DMAC_CFG_INT_EN			(1u << 1)
#define DMAC_CHEN				0x018
#define		CHEN_EN_SHIFT			0
#define		CHEN_WE_SHIFT			8	// up to 8 channels
#define COMMON_REGS_SIZE		0x100

// channel registers, at COMMON_REGS_SIZE + channel * CHANNEL_REGS_SIZE
#define CHANNEL_REGS_SIZE		0x100
#define CH_CFG_L				0x020
#define CH_CFG_H				0x024
#define CH_LLP					0x028
#define CH_INTSTATUS_ENA		0x080
#define CH_INTSTATUS			0x088
#define CH_INTSIGNAL_ENA		0x090
#define CH_INTCLEAR				0x098

// CH_CFG
#define CFG_L_SRC_MULTBLK_SHIFT	0
#define CFG_L_DST_MULTBLK_SHIFT	2
#define		MULTBLK_LINKED_LIST		3
#define CFG2_L_SRC_PER_SHIFT	4	// 6 bits
#define CFG2_L_DST_PER_SHIFT	11	// 6 bits
#define CFG_H_TT_FC_SHIFT		0
#define		TT_FC_MEM_TO_PER		1
#define		TT_FC_PER_TO_MEM		2
#define CFG_H_HS_SEL_SRC		(1u << 3)	// set: software handshake
#define CFG_H_HS_SEL_DST		(1u << 4)
#define CFG_H_SRC_PER_SHIFT		7	// 4 bits
#define CFG_H_DST_PER_SHIFT		12	// 4 bits
#define CFG_H_PRIORITY_SHIFT	17
#define CFG2_H_PRIORITY_SHIFT	15

// CH_CTL, in the linked list items
#define CTL_L_SRC_INC_FIXED		(1u << 4)
#define CTL_L_DST_INC_FIXED		(1u << 6)
#define CTL_L_SRC_WIDTH_SHIFT	8
#define CTL_L_DST_WIDTH_SHIFT	11
#define CTL_L_SRC_MSIZE_SHIFT	14
#define CTL_L_DST_MSIZE_SHIFT	18
#define		MSIZE_4					1
#define CTL_H_ARLEN_EN			(1u << 6)
#define CTL_H_ARLEN_SHIFT		7
#define CTL_H_AWLEN_EN			(1u << 15)
#define CTL_H_AWLEN_SHIFT		16
#define CTL_H_LLI_LAST			(1u << 30)
#define CTL_H_LLI_VALID			(1u << 31)

// channel interrupts
#define IRQ_DMA_TRF				(1u << 1)
#define IRQ_ALL_ERRORS			(0x003f0000u | 0x00007fe0u)

#define MAX_CHANNELS			8
#define MAX_BLOCK_TS			65536


struct Lli {
	uint64	sar;
	uint64	dar;
	uint32	blockTsLow;
	uint32	blockTsHigh;
	uint64	llp;
	uint32	ctlLow;
	uint32	ctlHigh;
	uint32	sstat;
	uint32	dstat;
	uint32	statusLow;
	uint32	statusHigh;
	uint32	reserved[2];
} _PACKED;

#define LLI_COUNT				(B_PAGE_SIZE / sizeof(Lli))


struct Controller;

struct dw_axi_dma_channel {
	Controller*			controller;
	uint32				index;
	bool				used;
	bool				running;
	uint32				handshake;
	addr_t				regs;

	area_id				lliArea;
	volatile Lli*		lli;
	phys_addr_t			lliPhysical;
	uint32				lliCount;

	dw_axi_dma_callback	callback;
	void*				cookie;
	uint32				errors;
};

struct Controller {
	Controller*			next;
	device_node*		fdtNode;
	area_id				area;
	addr_t				regs;
	uint32				irq;
	bool				irqInstalled;
	bool				useCfg2;
	uint32				channelCount;
	uint32				burstLength;	// 0: not restricted
	uint32				blockSize[MAX_CHANNELS];
	uint32				priority[MAX_CHANNELS];
	spinlock			lock;
	dw_axi_dma_channel	channels[MAX_CHANNELS];
};


static device_manager_info* sDeviceManager;
static mutex sLock = MUTEX_INITIALIZER("dw_axi_dma");
static Controller* sControllers;


static inline uint32
read32(addr_t address)
{
	return *(volatile uint32*)address;
}


static inline void
write32(addr_t address, uint32 value)
{
	*(volatile uint32*)address = value;
}


/*!	Orders the linked list items in memory before the register write that
	lets the controller fetch them.
*/
static inline void
memory_to_device_barrier()
{
#if defined(__riscv)
	asm volatile("fence iorw, iorw" ::: "memory");
#else
	__sync_synchronize();
#endif
}


static fdt_device_module_info*
get_fdt(device_node* node, fdt_device** _dev)
{
	fdt_device_module_info* module;
	if (node == NULL || sDeviceManager->get_driver(node,
			(driver_module_info**)&module, (void**)_dev) != B_OK) {
		return NULL;
	}
	return module;
}


static bool
get_prop_u32(fdt_device_module_info* module, fdt_device* dev,
	const char* name, uint32* value, uint32 index = 0)
{
	int length;
	const uint32* prop = (const uint32*)module->get_prop(dev, name, &length);
	if (prop == NULL || length < (int)(4 * (index + 1)))
		return false;
	*value = B_BENDIAN_TO_HOST_INT32(prop[index]);
	return true;
}


static bool
has_string(const char* list, int length, const char* string)
{
	for (int i = 0; i < length; i += strlen(list + i) + 1) {
		if (strcmp(list + i, string) == 0)
			return true;
	}
	return false;
}


//	#pragma mark - channel registers


static void
channel_enable(dw_axi_dma_channel* channel, bool enable)
{
	Controller* controller = channel->controller;
	uint32 value = (1u << (CHEN_WE_SHIFT + channel->index));
	if (enable)
		value |= 1u << (CHEN_EN_SHIFT + channel->index);
	write32(controller->regs + DMAC_CHEN, value);
}


static bool
channel_is_enabled(dw_axi_dma_channel* channel)
{
	return (read32(channel->controller->regs + DMAC_CHEN)
		& (1u << (CHEN_EN_SHIFT + channel->index))) != 0;
}


static phys_addr_t
channel_llp(dw_axi_dma_channel* channel)
{
	uint64 low = read32(channel->regs + CH_LLP);
	uint64 high = read32(channel->regs + CH_LLP + 4);
	return (phys_addr_t)((high << 32) | low) & ~(phys_addr_t)0x3f;
}


/*!	Called with the controller lock held. */
static void
channel_handle_interrupt(dw_axi_dma_channel* channel, uint32 status)
{
	if (!channel->running)
		return;

	if ((status & IRQ_ALL_ERRORS) != 0) {
		// a stuck error would interrupt again at once: stop the channel
		channel->errors |= status;
		channel->running = false;
		channel_enable(channel, false);
		write32(channel->regs + CH_INTSIGNAL_ENA, 0);
		return;
	}

	if ((status & IRQ_DMA_TRF) == 0)
		return;

	phys_addr_t llp = channel_llp(channel);
	uint32 current = 0;
	if (llp >= channel->lliPhysical
		&& llp < channel->lliPhysical + channel->lliCount * sizeof(Lli)) {
		current = (llp - channel->lliPhysical) / sizeof(Lli);
	}

	// The channel is idle: mark every item valid again, then let it fetch
	// the next one.
	for (uint32 i = 0; i < channel->lliCount; i++)
		channel->lli[i].ctlHigh |= CTL_H_LLI_VALID;
	memory_to_device_barrier();
	channel_enable(channel, true);

	if (channel->callback != NULL)
		channel->callback(channel->cookie, current);
}


static int32
controller_interrupt(void* data)
{
	Controller* controller = (Controller*)data;
	int32 result = B_UNHANDLED_INTERRUPT;

	acquire_spinlock(&controller->lock);
	for (uint32 i = 0; i < controller->channelCount; i++) {
		dw_axi_dma_channel* channel = &controller->channels[i];
		uint32 status = read32(channel->regs + CH_INTSTATUS);
		if (status == 0)
			continue;
		write32(channel->regs + CH_INTCLEAR, status);
		result = B_HANDLED_INTERRUPT;
		channel_handle_interrupt(channel, status);
	}
	release_spinlock(&controller->lock);

	return result;
}


//	#pragma mark - controller


/*!	Clocks on, then resets released, as Linux does. */
static status_t
controller_enable_clocks(device_node* fdtNode)
{
	fdt_clock_reset_module_info* clocks;
	if (get_module(JH7110_CLOCK_RESET_MODULE_NAME, (module_info**)&clocks)
			!= B_OK) {
		ERROR("no clock/reset driver\n");
		return B_NOT_SUPPORTED;
	}

	static const char* const kClockNames[] = { "core-clk", "cfgr-clk" };
	status_t status = B_OK;
	for (int i = 0; i < 2 && status == B_OK; i++) {
		fdt_clock clock;
		status = clocks->get_clock(fdtNode, kClockNames[i], 0, &clock);
		if (status == B_OK)
			status = clocks->enable_clock(clock, true);
		if (status != B_OK)
			ERROR("cannot enable the %s clock\n", kClockNames[i]);
	}
	for (uint32 i = 0; status == B_OK; i++) {
		fdt_reset reset;
		if (clocks->get_reset(fdtNode, NULL, i, &reset) != B_OK)
			break;
		status = clocks->set_reset(reset, false);
		if (status != B_OK)
			ERROR("cannot release reset %" B_PRIu32 "\n", i);
	}

	put_module(JH7110_CLOCK_RESET_MODULE_NAME);
	return status;
}


static void
controller_delete(Controller* controller)
{
	if (controller->irqInstalled) {
		remove_io_interrupt_handler(controller->irq, controller_interrupt,
			controller);
	}
	if (controller->area >= 0)
		delete_area(controller->area);
	delete controller;
}


static status_t
controller_create(device_node* fdtNode, Controller** _controller)
{
	fdt_device* dev;
	fdt_device_module_info* module = get_fdt(fdtNode, &dev);
	if (module == NULL)
		return B_BAD_VALUE;

	int length;
	const char* compatible = (const char*)module->get_prop(dev, "compatible",
		&length);
	if (compatible == NULL
		|| (!has_string(compatible, length, "starfive,jh7110-axi-dma")
			&& !has_string(compatible, length, "snps,axi-dma-1.01a"))) {
		ERROR("not a DesignWare AXI DMA controller\n");
		return B_BAD_VALUE;
	}

	Controller* controller = new(std::nothrow) Controller;
	if (controller == NULL)
		return B_NO_MEMORY;
	memset(controller, 0, sizeof(*controller));
	controller->fdtNode = fdtNode;
	controller->area = -1;
	B_INITIALIZE_SPINLOCK(&controller->lock);
	controller->useCfg2 = has_string(compatible, length,
		"starfive,jh7110-axi-dma");

	uint64 regs, regsLength, irq;
	if (!module->get_reg(dev, 0, &regs, &regsLength)
		|| !module->get_interrupt(dev, 0, NULL, &irq)
		|| !get_prop_u32(module, dev, "dma-channels",
			&controller->channelCount)
		|| controller->channelCount == 0
		|| controller->channelCount > MAX_CHANNELS) {
		ERROR("incomplete device tree node\n");
		controller_delete(controller);
		return B_BAD_DATA;
	}
	controller->irq = irq;

	for (uint32 i = 0; i < controller->channelCount; i++) {
		if (!get_prop_u32(module, dev, "snps,block-size",
				&controller->blockSize[i], i)
			|| controller->blockSize[i] > MAX_BLOCK_TS) {
			controller->blockSize[i] = 4096;
		}
		if (!get_prop_u32(module, dev, "snps,priority",
				&controller->priority[i], i)
			|| controller->priority[i] >= controller->channelCount) {
			controller->priority[i] = i;
		}
	}
	uint32 burst;
	if (get_prop_u32(module, dev, "snps,axi-max-burst-len", &burst)
		&& burst >= 1 && burst <= 256) {
		controller->burstLength = burst;
	}

	// before the first register access: an unclocked block can stall the
	// bus, and this line then shows which one
	INFO("controller at %#" B_PRIx64 ", irq %" B_PRIu64 ", %" B_PRIu32
		" channels\n", regs, irq, controller->channelCount);

	status_t status = controller_enable_clocks(fdtNode);
	if (status != B_OK) {
		controller_delete(controller);
		return status;
	}

	void* address;
	controller->area = map_physical_memory("dw_axi_dma registers", regs,
		ROUNDUP(regsLength, B_PAGE_SIZE), B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (controller->area < 0) {
		status = controller->area;
		controller_delete(controller);
		return status;
	}
	controller->regs = (addr_t)address;

	for (uint32 i = 0; i < controller->channelCount; i++) {
		dw_axi_dma_channel* channel = &controller->channels[i];
		channel->controller = controller;
		channel->index = i;
		channel->lliArea = -1;
		channel->regs = controller->regs + COMMON_REGS_SIZE
			+ i * CHANNEL_REGS_SIZE;
		write32(channel->regs + CH_INTSTATUS_ENA, 0);
		write32(channel->regs + CH_INTSIGNAL_ENA, 0);
		channel_enable(channel, false);
		write32(channel->regs + CH_INTCLEAR, 0xffffffff);
	}

	INFO("id %#" B_PRIx32 ", version %#" B_PRIx32 "\n",
		read32(controller->regs + DMAC_ID),
		read32(controller->regs + DMAC_COMPVER));

	status = install_io_interrupt_handler(controller->irq,
		controller_interrupt, controller, 0);
	if (status != B_OK) {
		ERROR("cannot install the interrupt handler: %s\n", strerror(status));
		controller_delete(controller);
		return status;
	}
	controller->irqInstalled = true;

	write32(controller->regs + DMAC_CFG, read32(controller->regs + DMAC_CFG)
		| DMAC_CFG_EN | DMAC_CFG_INT_EN);

	*_controller = controller;
	return B_OK;
}


/*!	Called with sLock held. */
static Controller*
controller_for_node(device_node* fdtNode)
{
	for (Controller* controller = sControllers; controller != NULL;
			controller = controller->next) {
		if (controller->fdtNode == fdtNode)
			return controller;
	}

	Controller* controller;
	if (controller_create(fdtNode, &controller) != B_OK)
		return NULL;
	controller->next = sControllers;
	sControllers = controller;
	return controller;
}


//	#pragma mark - module API


static status_t
acquire_channel(device_node* consumerFdtNode, const char* name,
	dw_axi_dma_channel** _channel)
{
	fdt_device* dev;
	fdt_device_module_info* module = get_fdt(consumerFdtNode, &dev);
	if (module == NULL)
		return B_BAD_VALUE;

	// the entry of the "dmas" list that "dma-names" calls name; every
	// entry is a phandle and one cell
	uint32 entry = 0;
	if (name != NULL) {
		int length;
		const char* names = (const char*)module->get_prop(dev, "dma-names",
			&length);
		if (names == NULL)
			return B_NAME_NOT_FOUND;
		int offset = 0;
		while (offset < length && strcmp(names + offset, name) != 0) {
			offset += strlen(names + offset) + 1;
			entry++;
		}
		if (offset >= length)
			return B_NAME_NOT_FOUND;
	}

	uint32 phandle, handshake;
	if (!get_prop_u32(module, dev, "dmas", &phandle, entry * 2)
		|| !get_prop_u32(module, dev, "dmas", &handshake, entry * 2 + 1))
		return B_NAME_NOT_FOUND;

	fdt_bus_module_info* busModule;
	fdt_bus* bus;
	if (sDeviceManager->get_driver(module->get_bus(dev),
			(driver_module_info**)&busModule, (void**)&bus) != B_OK)
		return B_ERROR;
	device_node* dmaNode = busModule->node_by_phandle(bus, phandle);
	if (dmaNode == NULL)
		return B_NAME_NOT_FOUND;

	fdt_device* dmaDev;
	fdt_device_module_info* dmaModule = get_fdt(dmaNode, &dmaDev);
	uint32 cells;
	if (dmaModule == NULL
		|| !get_prop_u32(dmaModule, dmaDev, "#dma-cells", &cells)
		|| cells != 1) {
		ERROR("unsupported #dma-cells\n");
		return B_NOT_SUPPORTED;
	}

	MutexLocker locker(sLock);
	Controller* controller = controller_for_node(dmaNode);
	if (controller == NULL)
		return B_ERROR;

	for (uint32 i = 0; i < controller->channelCount; i++) {
		dw_axi_dma_channel* channel = &controller->channels[i];
		if (channel->used)
			continue;

		void* address;
		channel->lliArea = create_area("dw_axi_dma lli", &address,
			B_ANY_KERNEL_ADDRESS, B_PAGE_SIZE, B_32_BIT_CONTIGUOUS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
		if (channel->lliArea < 0)
			return channel->lliArea;
		physical_entry physical;
		if (get_memory_map(address, B_PAGE_SIZE, &physical, 1) != B_OK) {
			delete_area(channel->lliArea);
			channel->lliArea = -1;
			return B_ERROR;
		}
		channel->lli = (volatile Lli*)address;
		channel->lliPhysical = physical.address;
		channel->used = true;
		channel->running = false;
		channel->handshake = handshake;
		channel->errors = 0;

		INFO("channel %" B_PRIu32 " for handshake %" B_PRIu32 "\n", i,
			handshake);
		*_channel = channel;
		return B_OK;
	}

	return B_BUSY;
}


static void stop(dw_axi_dma_channel* channel);


static void
release_channel(dw_axi_dma_channel* channel)
{
	stop(channel);

	MutexLocker locker(sLock);
	delete_area(channel->lliArea);
	channel->lliArea = -1;
	channel->lli = NULL;
	channel->used = false;
}


static status_t
start_cyclic(dw_axi_dma_channel* channel, const dw_axi_dma_cyclic* transfer)
{
	Controller* controller = channel->controller;

	uint32 deviceWidthLog;
	switch (transfer->deviceWidth) {
		case 1: deviceWidthLog = 0; break;
		case 2: deviceWidthLog = 1; break;
		case 4: deviceWidthLog = 2; break;
		default: return B_BAD_VALUE;
	}

	if (transfer->periodCount < 2 || transfer->periodCount > LLI_COUNT
		|| transfer->periodSize == 0
		|| (transfer->buffer & 3) != 0
		|| (transfer->periodSize & 3) != 0
		|| (transfer->periodSize % transfer->deviceWidth) != 0)
		return B_BAD_VALUE;

	// memory side in 32 bit units, the device side in its register width
	const uint32 memoryWidthLog = 2;
	bool toDevice = transfer->direction == DW_AXI_DMA_MEM_TO_DEV;
	uint32 blockTs = transfer->periodSize
		>> (toDevice ? memoryWidthLog : deviceWidthLog);
	if (blockTs > controller->blockSize[channel->index])
		return B_BAD_VALUE;

	stop(channel);

	uint32 ctlLow = (MSIZE_4 << CTL_L_SRC_MSIZE_SHIFT)
		| (MSIZE_4 << CTL_L_DST_MSIZE_SHIFT);
	if (toDevice) {
		ctlLow |= (memoryWidthLog << CTL_L_SRC_WIDTH_SHIFT)
			| (deviceWidthLog << CTL_L_DST_WIDTH_SHIFT)
			| CTL_L_DST_INC_FIXED;
	} else {
		ctlLow |= (deviceWidthLog << CTL_L_SRC_WIDTH_SHIFT)
			| (memoryWidthLog << CTL_L_DST_WIDTH_SHIFT)
			| CTL_L_SRC_INC_FIXED;
	}
	uint32 ctlHigh = CTL_H_LLI_VALID | CTL_H_LLI_LAST;
	if (controller->burstLength != 0) {
		uint32 length = controller->burstLength - 1;
		ctlHigh |= CTL_H_ARLEN_EN | CTL_H_AWLEN_EN
			| (length << CTL_H_ARLEN_SHIFT) | (length << CTL_H_AWLEN_SHIFT);
	}

	for (uint32 i = 0; i < transfer->periodCount; i++) {
		volatile Lli* lli = &channel->lli[i];
		phys_addr_t memory = transfer->buffer + i * transfer->periodSize;
		lli->sar = toDevice ? memory : transfer->deviceAddress;
		lli->dar = toDevice ? transfer->deviceAddress : memory;
		lli->blockTsLow = blockTs - 1;
		lli->blockTsHigh = 0;
		lli->llp = channel->lliPhysical
			+ ((i + 1) % transfer->periodCount) * sizeof(Lli);
		lli->ctlLow = ctlLow;
		lli->ctlHigh = ctlHigh;
		lli->sstat = lli->dstat = 0;
		lli->statusLow = lli->statusHigh = 0;
	}

	uint32 cfgLow = (MULTBLK_LINKED_LIST << CFG_L_SRC_MULTBLK_SHIFT)
		| (MULTBLK_LINKED_LIST << CFG_L_DST_MULTBLK_SHIFT);
	uint32 cfgHigh = (toDevice ? TT_FC_MEM_TO_PER : TT_FC_PER_TO_MEM)
		<< CFG_H_TT_FC_SHIFT;
	uint32 priority = controller->priority[channel->index];
	if (controller->useCfg2) {
		cfgLow |= toDevice
			? (channel->handshake & 0x3f) << CFG2_L_DST_PER_SHIFT
			: (channel->handshake & 0x3f) << CFG2_L_SRC_PER_SHIFT;
		cfgHigh |= priority << CFG2_H_PRIORITY_SHIFT;
	} else {
		cfgHigh |= toDevice
			? (channel->handshake & 0xf) << CFG_H_DST_PER_SHIFT
			: (channel->handshake & 0xf) << CFG_H_SRC_PER_SHIFT;
		cfgHigh |= priority << CFG_H_PRIORITY_SHIFT;
	}

	cpu_status state = disable_interrupts();
	acquire_spinlock(&controller->lock);

	channel->lliCount = transfer->periodCount;
	channel->callback = transfer->callback;
	channel->cookie = transfer->cookie;
	channel->errors = 0;

	write32(channel->regs + CH_CFG_L, cfgLow);
	write32(channel->regs + CH_CFG_H, cfgHigh);
	write32(channel->regs + CH_LLP, (uint32)channel->lliPhysical);
	write32(channel->regs + CH_LLP + 4,
		(uint32)((uint64)channel->lliPhysical >> 32));
	write32(channel->regs + CH_INTCLEAR, 0xffffffff);
	write32(channel->regs + CH_INTSTATUS_ENA, IRQ_DMA_TRF | IRQ_ALL_ERRORS);
	write32(channel->regs + CH_INTSIGNAL_ENA, IRQ_DMA_TRF | IRQ_ALL_ERRORS);

	channel->running = true;
	memory_to_device_barrier();
	channel_enable(channel, true);

	release_spinlock(&controller->lock);
	restore_interrupts(state);
	return B_OK;
}


static void
stop(dw_axi_dma_channel* channel)
{
	Controller* controller = channel->controller;

	cpu_status state = disable_interrupts();
	acquire_spinlock(&controller->lock);
	bool wasRunning = channel->running;
	channel->running = false;
	write32(channel->regs + CH_INTSIGNAL_ENA, 0);
	channel_enable(channel, false);
	release_spinlock(&controller->lock);
	restore_interrupts(state);

	for (int i = 0; i < 50 && channel_is_enabled(channel); i++)
		snooze(1000);
	if (channel_is_enabled(channel))
		ERROR("channel %" B_PRIu32 " did not stop\n", channel->index);

	write32(channel->regs + CH_INTSTATUS_ENA, 0);
	write32(channel->regs + CH_INTCLEAR, 0xffffffff);

	if (wasRunning && channel->errors != 0) {
		ERROR("channel %" B_PRIu32 " had errors %#" B_PRIx32 "\n",
			channel->index, channel->errors);
	}
}


static uint32
errors(dw_axi_dma_channel* channel)
{
	return channel->errors;
}


static status_t
std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
			return B_OK;

		case B_MODULE_UNINIT:
		{
			MutexLocker locker(sLock);
			while (sControllers != NULL) {
				Controller* controller = sControllers;
				sControllers = controller->next;
				write32(controller->regs + DMAC_CFG, 0);
				controller_delete(controller);
			}
			return B_OK;
		}
	}
	return B_ERROR;
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


static dw_axi_dma_module_info sModuleInfo = {
	{
		DW_AXI_DMA_MODULE_NAME,
		0,
		std_ops
	},

	acquire_channel,
	release_channel,
	start_cyclic,
	stop,
	errors,
};


module_info* modules[] = {
	(module_info*)&sModuleInfo,
	NULL
};
