/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


//!	PWM-DAC of the StarFive JH7110 ("starfive,jh7110-pwmdac").
// The block turns 16 bit samples into two PWM signals; on the Milk-V Mars
// and the VisionFive 2 they leave on GPIO 33 (left) and 34 (right) and an RC
// filter makes the line out of the 3.5 mm jack from them.
//
// The DAC has no interrupt and no status: a DMA channel of the AXI DMA
// controller fills its FIFO on request, one 32 bit write per stereo frame,
// from a ring of two buffers. The DMA interrupt after each buffer gives the
// playback position.
//
// The sample rate is the core clock divided by 256. The core clock comes
// from PLL2 (1188 MHz on the JH7110) through integer dividers, so 48 kHz is
// only approximated; the played_real_time reported to the media kit lets it
// follow the real rate.
//
// The control register fields and the configuration (8 bit shift, centered
// duty cycle, inverted sample MSB for signed data) were checked against
// Linux's sound/soc/starfive/jh7110_pwmdac.c, the pin multiplexing against
// drivers/pinctrl/starfive/pinctrl-starfive-jh7110.c.


#include <ByteOrder.h>
#include <KernelExport.h>
#include <bus/FDT.h>
#include <device_manager.h>
#include <dw_axi_dma.h>
#include <fdt_clock_reset.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include <hmulti_audio.h>

#include <new>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define DRIVER_NAME		"jh7110_pwmdac"
#define PWMDAC_DRIVER_MODULE_NAME \
	"drivers/audio/hmulti/jh7110_pwmdac/driver_v1"
#define PWMDAC_DEVICE_MODULE_NAME \
	"drivers/audio/hmulti/jh7110_pwmdac/device_v1"
#define PWMDAC_DEVICE_PATH "audio/hmulti/jh7110_pwmdac/1"

#define TRACE_ALWAYS(x...)	dprintf(DRIVER_NAME ": " x)


// registers
#define PWMDAC_WDATA				0x00
#define PWMDAC_CTRL					0x04
#define		CTRL_ENABLE					(1u << 0)
#define		CTRL_SHIFT_10				(1u << 1)	// clear: 8 bit
#define		CTRL_DUTY_CYCLE_SHIFT		2
#define		CTRL_DUTY_CYCLE_CENTER		(2u << CTRL_DUTY_CYCLE_SHIFT)
#define		CTRL_CNT_N_SHIFT			4			// count - 1, 9 bits
#define		CTRL_DATA_CHANGE			(1u << 13)
#define		CTRL_DATA_INVERT_MSB		(1u << 14)
#define		CTRL_DATA_SHIFT_SHIFT		15			// 3 bits

#define PWMDAC_CORE_CLOCK			12288000	// 48 kHz with an 8 bit shift
#define PWMDAC_CLOCKS_PER_SAMPLE	256

// GPIO multiplexer of the "starfive,jh7110-sys-pinctrl" block
#define SYS_GPIO_DOEN				0x000
#define SYS_GPIO_DOUT				0x040
#define SYS_GPIO_GPI				0x080
#define SYS_GPIO_PADCFG				0x120		// pins 0 - 63
#define		PADCFG_IE					(1u << 0)
#define		PADCFG_DS_SHIFT				1
#define		PADCFG_DS_MASK				(3u << PADCFG_DS_SHIFT)
#define		PADCFG_PU					(1u << 3)
#define		PADCFG_PD					(1u << 4)
#define		PADCFG_SLEW					(1u << 5)
#define		PADCFG_SMT					(1u << 6)
#define SYS_GPIO_COUNT				64
#define GPI_NONE					0xff

// The playback ring: two buffers, one DMA period each.
#define PLAYBACK_CHANNELS			2
#define PLAYBACK_SAMPLE_SIZE		2
#define PLAYBACK_FRAME_SIZE			(PLAYBACK_CHANNELS * PLAYBACK_SAMPLE_SIZE)
#define PLAYBACK_BUFFERS			2
#define PLAYBACK_BUFFER_FRAMES		1024
#define PLAYBACK_BUFFER_SIZE		(PLAYBACK_BUFFER_FRAMES * PLAYBACK_FRAME_SIZE)
#define PLAYBACK_RING_SIZE			(PLAYBACK_BUFFERS * PLAYBACK_BUFFER_SIZE)
#define EXCHANGE_TIMEOUT			500000


struct pwmdac_driver {
	device_node*			node;
	device_node*			fdtNode;
	area_id					registerArea;
	addr_t					registers;
	phys_addr_t				registersPhysical;
	area_id					bufferArea;
	uint8*					buffer;
	phys_addr_t				bufferPhysical;
	int64					coreClock;

	dw_axi_dma_module_info*	dma;
	dw_axi_dma_channel*		dmaChannel;
	bool					hardwareReady;

	// Protects the fields below and the start and stop of playback.
	mutex					lock;
	int32					openCount;
	bool					running;
	sem_id					bufferReadySem;

	// Playback position, written by the DMA interrupt.
	spinlock				positionLock;
	uint32					bufferCycle;
	int64					framesCount;
	bigtime_t				realTime;
	uint32					interrupts;
};


static device_manager_info* sDeviceManager;

static multi_channel_info sChannels[] = {
	{ 0, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0 },
	{ 1, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0 },
	{ 2, B_MULTI_OUTPUT_BUS, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO },
	{ 3, B_MULTI_OUTPUT_BUS, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO },
};


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
has_prop(fdt_device_module_info* module, fdt_device* dev, const char* name)
{
	int length;
	return module->get_prop(dev, name, &length) != NULL;
}


//	#pragma mark - pins


/*!	Applies one "pinmux" group of the GPIO multiplexer and the pad
	properties next to it. Each cell is din (31-24), dout (23-16),
	doen (15-10), function (9-8) and pin (7-0).
*/
static void
apply_pin_group(addr_t gpio, fdt_device_module_info* module, fdt_device* dev)
{
	int length;
	const uint32* cells = (const uint32*)module->get_prop(dev, "pinmux",
		&length);
	if (cells == NULL)
		return;

	uint32 padSet = 0;
	uint32 padClear = 0;
	if (has_prop(module, dev, "bias-disable"))
		padClear |= PADCFG_PU | PADCFG_PD;
	if (has_prop(module, dev, "input-disable"))
		padClear |= PADCFG_IE;
	if (has_prop(module, dev, "input-schmitt-disable"))
		padClear |= PADCFG_SMT;
	uint32 value;
	if (get_prop_u32(module, dev, "slew-rate", &value)) {
		padClear |= PADCFG_SLEW;
		if (value != 0)
			padSet |= PADCFG_SLEW;
	}
	if (get_prop_u32(module, dev, "drive-strength", &value)) {
		uint32 strength = value >= 12 ? 3 : value >= 8 ? 2 : value >= 4 ? 1 : 0;
		padClear |= PADCFG_DS_MASK;
		padSet |= strength << PADCFG_DS_SHIFT;
	}

	for (int i = 0; i < length / 4; i++) {
		uint32 cell = B_BENDIAN_TO_HOST_INT32(cells[i]);
		uint32 pin = cell & 0xff;
		uint32 function = (cell >> 8) & 0x3;
		uint32 doen = (cell >> 10) & 0x3f;
		uint32 dout = (cell >> 16) & 0xff;
		uint32 din = (cell >> 24) & 0xff;
		if (pin >= SYS_GPIO_COUNT || function != 0) {
			TRACE_ALWAYS("pins: cannot set up pin cell %#" B_PRIx32 "\n", cell);
			continue;
		}

		uint32 offset = 4 * (pin / 4);
		uint32 shift = 8 * (pin % 4);
		addr_t reg = gpio + SYS_GPIO_DOUT + offset;
		write32(reg, (read32(reg) & ~(0x7fu << shift)) | (dout << shift));
		reg = gpio + SYS_GPIO_DOEN + offset;
		write32(reg, (read32(reg) & ~(0x3fu << shift)) | (doen << shift));
		if (din != GPI_NONE) {
			uint32 inputShift = 8 * (din % 4);
			reg = gpio + SYS_GPIO_GPI + 4 * (din / 4);
			write32(reg, (read32(reg) & ~(0x7fu << inputShift))
				| ((pin + 2) << inputShift));
		}

		reg = gpio + SYS_GPIO_PADCFG + 4 * pin;
		write32(reg, (read32(reg) & ~padClear) | padSet);

		TRACE_ALWAYS("pins: GPIO %" B_PRIu32 " output %" B_PRIu32
			", enable %" B_PRIu32 ", pad %#" B_PRIx32 "\n", pin, dout, doen,
			read32(reg));
	}
}


/*!	Sets up the pins of "pinctrl-0", which the firmware leaves alone: the
	group node holds one or more child nodes with "pinmux" lists, and its
	parent is the GPIO block.
*/
static status_t
setup_pins(fdt_device_module_info* module, fdt_device* dev)
{
	uint32 phandle;
	if (!get_prop_u32(module, dev, "pinctrl-0", &phandle))
		return B_OK;

	fdt_bus_module_info* busModule;
	fdt_bus* bus;
	if (sDeviceManager->get_driver(module->get_bus(dev),
			(driver_module_info**)&busModule, (void**)&bus) != B_OK)
		return B_ERROR;
	device_node* group = busModule->node_by_phandle(bus, phandle);
	if (group == NULL)
		return B_NAME_NOT_FOUND;

	device_node* gpioNode = sDeviceManager->get_parent_node(group);
	fdt_device* gpioDev;
	fdt_device_module_info* gpioModule = get_fdt(gpioNode, &gpioDev);
	uint64 regs, regsLength;
	int length;
	const char* compatible = gpioModule != NULL
		? (const char*)gpioModule->get_prop(gpioDev, "compatible", &length)
		: NULL;
	if (compatible == NULL
		|| strcmp(compatible, "starfive,jh7110-sys-pinctrl") != 0
		|| !gpioModule->get_reg(gpioDev, 0, &regs, &regsLength)) {
		TRACE_ALWAYS("pins: not on the SYS GPIO block, left as they are\n");
		sDeviceManager->put_node(gpioNode);
		return B_NOT_SUPPORTED;
	}
	sDeviceManager->put_node(gpioNode);

	void* address;
	area_id area = map_physical_memory("jh7110_pwmdac pins", regs, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&address);
	if (area < 0)
		return area;

	// an empty list matches every child (NULL would match none)
	const device_attr anyChild[] = { { NULL } };
	device_node* child = NULL;
	while (sDeviceManager->get_next_child_node(group, anyChild, &child)
			== B_OK) {
		fdt_device* childDev;
		fdt_device_module_info* childModule = get_fdt(child, &childDev);
		if (childModule != NULL)
			apply_pin_group((addr_t)address, childModule, childDev);
	}

	delete_area(area);
	return B_OK;
}


//	#pragma mark - hardware


/*!	Clocks on, then the reset released, as Linux does. The core clock is
	set for 48 kHz.
*/
static status_t
enable_clocks(pwmdac_driver* driver)
{
	fdt_clock_reset_module_info* clocks;
	if (get_module(JH7110_CLOCK_RESET_MODULE_NAME, (module_info**)&clocks)
			!= B_OK) {
		TRACE_ALWAYS("no clock/reset driver\n");
		return B_NOT_SUPPORTED;
	}

	fdt_clock apb, core;
	fdt_reset reset;
	status_t status = clocks->get_clock(driver->fdtNode, "apb", 0, &apb);
	if (status == B_OK)
		status = clocks->get_clock(driver->fdtNode, "core", 0, &core);
	if (status == B_OK)
		status = clocks->get_reset(driver->fdtNode, NULL, 0, &reset);
	if (status == B_OK) {
		driver->coreClock = clocks->set_clock_rate(core, PWMDAC_CORE_CLOCK);
		status = clocks->enable_clock(apb, true);
	}
	if (status == B_OK)
		status = clocks->enable_clock(core, true);
	if (status == B_OK)
		status = clocks->set_reset(reset, false);
	if (status == B_OK)
		driver->coreClock = clocks->get_clock_rate(core);
	else
		TRACE_ALWAYS("clocks: %s\n", strerror(status));

	put_module(JH7110_CLOCK_RESET_MODULE_NAME);
	return status;
}


static void
pwmdac_set_enabled(pwmdac_driver* driver, bool enable)
{
	uint32 control = CTRL_DUTY_CYCLE_CENTER | CTRL_DATA_INVERT_MSB;
		// 8 bit shift, one sample per count, no data shift
	if (enable)
		control |= CTRL_ENABLE;
	write32(driver->registers + PWMDAC_CTRL, control);
}


static void
dma_period_done(void* cookie, uint32 currentPeriod)
{
	pwmdac_driver* driver = (pwmdac_driver*)cookie;

	acquire_spinlock(&driver->positionLock);
	driver->bufferCycle = currentPeriod % PLAYBACK_BUFFERS;
	driver->framesCount += PLAYBACK_BUFFER_FRAMES;
	driver->realTime = system_time();
	driver->interrupts++;
	release_spinlock(&driver->positionLock);

	release_sem_etc(driver->bufferReadySem, 1, B_DO_NOT_RESCHEDULE);
}


static status_t
playback_start(pwmdac_driver* driver)
{
	int32 count;
	if (get_sem_count(driver->bufferReadySem, &count) == B_OK && count > 0)
		acquire_sem_etc(driver->bufferReadySem, count, B_RELATIVE_TIMEOUT, 0);

	driver->bufferCycle = 0;
	driver->framesCount = 0;
	driver->realTime = system_time();
	driver->interrupts = 0;

	dw_axi_dma_cyclic transfer = {};
	transfer.direction = DW_AXI_DMA_MEM_TO_DEV;
	transfer.buffer = driver->bufferPhysical;
	transfer.periodSize = PLAYBACK_BUFFER_SIZE;
	transfer.periodCount = PLAYBACK_BUFFERS;
	transfer.deviceAddress = driver->registersPhysical + PWMDAC_WDATA;
	transfer.deviceWidth = PLAYBACK_FRAME_SIZE;
	transfer.callback = dma_period_done;
	transfer.cookie = driver;

	pwmdac_set_enabled(driver, true);
	status_t status = driver->dma->start_cyclic(driver->dmaChannel, &transfer);
	if (status != B_OK) {
		pwmdac_set_enabled(driver, false);
		TRACE_ALWAYS("playback: DMA start failed: %s\n", strerror(status));
		return status;
	}

	driver->running = true;
	TRACE_ALWAYS("playback: started\n");
	return B_OK;
}


static void
playback_stop(pwmdac_driver* driver)
{
	if (!driver->running)
		return;

	driver->dma->stop(driver->dmaChannel);
	pwmdac_set_enabled(driver, false);
	driver->running = false;

	// Wake a buffer_exchange() that is still waiting.
	release_sem_etc(driver->bufferReadySem, 1, 0);

	TRACE_ALWAYS("playback: stopped after %" B_PRId64 " frames, %" B_PRIu32
		" interrupts, DMA errors %#" B_PRIx32 "\n", driver->framesCount,
		driver->interrupts, driver->dma->errors(driver->dmaChannel));
}


//	#pragma mark - multi_audio


static status_t
get_description(pwmdac_driver* driver, multi_description* data)
{
	data->interface_version = B_CURRENT_INTERFACE_VERSION;
	data->interface_minimum = B_CURRENT_INTERFACE_VERSION;

	strlcpy(data->friendly_name, "Line out (JH7110 PWM-DAC)",
		sizeof(data->friendly_name));
	strlcpy(data->vendor_info, "Renku", sizeof(data->vendor_info));

	data->output_channel_count = PLAYBACK_CHANNELS;
	data->output_bus_channel_count = PLAYBACK_CHANNELS;
	data->input_channel_count = 0;
	data->input_bus_channel_count = 0;
	data->aux_bus_channel_count = 0;

	if (data->request_channel_count >= (int32)B_COUNT_OF(sChannels))
		memcpy(data->channels, sChannels, sizeof(sChannels));

	data->output_rates = B_SR_48000;
	data->input_rates = 0;
	data->max_cvsr_rate = 0;
	data->min_cvsr_rate = 0;
	data->output_formats = B_FMT_16BIT;
	data->input_formats = 0;
	data->lock_sources = B_MULTI_LOCK_INTERNAL;
	data->timecode_sources = 0;
	data->interface_flags = B_MULTI_INTERFACE_PLAYBACK;
	data->start_latency = 30000;
	data->control_panel[0] = '\0';
	return B_OK;
}


static status_t
get_enabled_channels(pwmdac_driver* driver, multi_channel_enable* data)
{
	data->lock_source = B_MULTI_LOCK_INTERNAL;
	for (int32 i = 0; i < PLAYBACK_CHANNELS; i++)
		B_SET_CHANNEL(data->enable_bits, i, true);
	return B_OK;
}


static status_t
get_global_format(pwmdac_driver* driver, multi_format_info* data)
{
	data->output_latency = 0;
	data->input_latency = 0;
	data->timecode_kind = 0;
	data->output.format = B_FMT_16BIT;
	data->output.rate = B_SR_48000;
	data->input.format = 0;
	data->input.rate = 0;
	return B_OK;
}


static status_t
set_global_format(pwmdac_driver* driver, multi_format_info* data)
{
	// There is only one format; report it back.
	return get_global_format(driver, data);
}


static status_t
list_mix_controls(pwmdac_driver* driver, multi_mix_control_info* data)
{
	// The DAC has no gain control; the system mixer sets the volume.
	data->control_count = 0;
	return B_OK;
}


static status_t
list_mix_connections(pwmdac_driver* driver, multi_mix_connection_info* data)
{
	return B_ERROR;
}


static status_t
list_mix_channels(pwmdac_driver* driver, multi_mix_channel_info* data)
{
	return B_ERROR;
}


static status_t
get_mix(pwmdac_driver* driver, multi_mix_value_info* data)
{
	return B_OK;
}


static status_t
set_mix(pwmdac_driver* driver, multi_mix_value_info* data)
{
	return B_OK;
}


/*!	The ring layout is fixed, so the requested buffer count and size are
	ignored.
*/
static status_t
get_buffers(pwmdac_driver* driver, multi_buffer_list* data)
{
	MutexLocker locker(driver->lock);

	if (!driver->running)
		vm_memset_physical(driver->bufferPhysical, 0, PLAYBACK_RING_SIZE);

	data->flags = B_MULTI_BUFFER_PLAYBACK;
	data->return_playback_buffers = PLAYBACK_BUFFERS;
	data->return_playback_channels = PLAYBACK_CHANNELS;
	data->return_playback_buffer_size = PLAYBACK_BUFFER_FRAMES;
	data->return_record_buffers = 0;
	data->return_record_channels = 0;
	data->return_record_buffer_size = 0;

	if (data->request_playback_buffers < PLAYBACK_BUFFERS
		|| data->request_playback_channels < PLAYBACK_CHANNELS) {
		return B_BAD_VALUE;
	}

	for (int32 i = 0; i < PLAYBACK_BUFFERS; i++) {
		buffer_desc descs[PLAYBACK_CHANNELS];
		for (int32 channel = 0; channel < PLAYBACK_CHANNELS; channel++) {
			descs[channel].base = (char*)driver->buffer
				+ i * PLAYBACK_BUFFER_SIZE + channel * PLAYBACK_SAMPLE_SIZE;
			descs[channel].stride = PLAYBACK_FRAME_SIZE;
		}
		if (!IS_USER_ADDRESS(data->playback_buffers[i])
			|| user_memcpy(data->playback_buffers[i], descs, sizeof(descs))
				!= B_OK) {
			return B_BAD_ADDRESS;
		}
	}
	return B_OK;
}


static status_t
buffer_exchange(pwmdac_driver* driver, multi_buffer_info* data)
{
	multi_buffer_info info;
	if (!IS_USER_ADDRESS(data)
		|| user_memcpy(&info, data, sizeof(info)) != B_OK) {
		return B_BAD_ADDRESS;
	}

	{
		MutexLocker locker(driver->lock);
		if (!driver->running) {
			status_t status = playback_start(driver);
			if (status != B_OK)
				return status;
		}
	}

	status_t status = acquire_sem_etc(driver->bufferReadySem, 1,
		B_CAN_INTERRUPT | B_RELATIVE_TIMEOUT, EXCHANGE_TIMEOUT);
	if (status != B_OK) {
		if (status == B_TIMED_OUT) {
			TRACE_ALWAYS("playback: no DMA interrupt, errors %#" B_PRIx32
				"\n", driver->dma->errors(driver->dmaChannel));
		}
		return status;
	}

	cpu_status state = disable_interrupts();
	acquire_spinlock(&driver->positionLock);
	info.playback_buffer_cycle = driver->bufferCycle;
	info.played_real_time = driver->realTime;
	info.played_frames_count = driver->framesCount;
	release_spinlock(&driver->positionLock);
	restore_interrupts(state);

	info.record_buffer_cycle = -1;
	info.recorded_real_time = 0;
	info.recorded_frames_count = 0;

	if (user_memcpy(data, &info, sizeof(info)) != B_OK)
		return B_BAD_ADDRESS;
	return B_OK;
}


static status_t
buffer_force_stop(pwmdac_driver* driver)
{
	MutexLocker locker(driver->lock);
	playback_stop(driver);
	return B_OK;
}


#define cookie_type pwmdac_driver
#include "../generic/multi.c"


//	#pragma mark - setup


static void
unmap_hardware(pwmdac_driver* driver)
{
	if (driver->dmaChannel != NULL)
		driver->dma->release_channel(driver->dmaChannel);
	driver->dmaChannel = NULL;
	if (driver->dma != NULL)
		put_module(DW_AXI_DMA_MODULE_NAME);
	driver->dma = NULL;
	if (driver->bufferArea >= 0)
		delete_area(driver->bufferArea);
	driver->bufferArea = -1;
	if (driver->registerArea >= 0)
		delete_area(driver->registerArea);
	driver->registerArea = -1;
}


static status_t
map_hardware(pwmdac_driver* driver)
{
	fdt_device* dev;
	fdt_device_module_info* module = get_fdt(driver->fdtNode, &dev);
	uint64 regs, regsLength;
	if (module == NULL || !module->get_reg(dev, 0, &regs, &regsLength))
		return B_BAD_DATA;
	driver->registersPhysical = regs;

	// before the first register access: an unclocked block can stall the
	// bus, and this line then shows which one
	TRACE_ALWAYS("at %#" B_PRIx64 "\n", regs);

	setup_pins(module, dev);

	status_t status = enable_clocks(driver);
	if (status != B_OK)
		return status;
	TRACE_ALWAYS("core clock %" B_PRId64 " Hz, %" B_PRId64 " Hz sample rate\n",
		driver->coreClock, driver->coreClock / PWMDAC_CLOCKS_PER_SAMPLE);

	void* address;
	driver->registerArea = map_physical_memory("jh7110_pwmdac registers",
		regs, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (driver->registerArea < 0)
		return driver->registerArea;
	driver->registers = (addr_t)address;
	pwmdac_set_enabled(driver, false);

	status = get_module(DW_AXI_DMA_MODULE_NAME, (module_info**)&driver->dma);
	if (status != B_OK) {
		driver->dma = NULL;
		TRACE_ALWAYS("no DMA module\n");
		return status;
	}
	status = driver->dma->acquire_channel(driver->fdtNode, "tx",
		&driver->dmaChannel);
	if (status != B_OK) {
		driver->dmaChannel = NULL;
		TRACE_ALWAYS("no DMA channel: %s\n", strerror(status));
		return status;
	}

	// The media add-on writes into the ring directly, like with hda. The
	// device tree does not mark the JH7110 DMA as non-coherent, so the ring
	// is ordinary cached memory.
	driver->bufferArea = create_area("jh7110_pwmdac playback", &address,
		B_ANY_KERNEL_ADDRESS, PLAYBACK_RING_SIZE, B_32_BIT_CONTIGUOUS,
		B_READ_AREA | B_WRITE_AREA);
	if (driver->bufferArea < 0)
		return driver->bufferArea;
	driver->buffer = (uint8*)address;

	physical_entry entry;
	status = get_memory_map(address, PLAYBACK_RING_SIZE, &entry, 1);
	if (status != B_OK)
		return status;
	driver->bufferPhysical = entry.address;
	// The ring is mapped for userland (the media add-on writes into it), and
	// the kernel may not touch user pages (riscv64: SUM is clear).
	vm_memset_physical(driver->bufferPhysical, 0, PLAYBACK_RING_SIZE);
	TRACE_ALWAYS("ready, playback ring at %#" B_PRIxPHYSADDR "\n",
		driver->bufferPhysical);
	return B_OK;
}


/*!	The hardware is set up on the first open rather than at boot: the pins,
	clocks and the DMA channel are only claimed once someone plays sound.
*/
static status_t
prepare_hardware(pwmdac_driver* driver)
{
	if (driver->hardwareReady)
		return B_OK;

	status_t status = map_hardware(driver);
	if (status != B_OK) {
		TRACE_ALWAYS("hardware setup failed: %s\n", strerror(status));
		unmap_hardware(driver);
		return status;
	}
	driver->hardwareReady = true;
	return B_OK;
}


static status_t
pwmdac_init_device(void* driverCookie, void** _deviceCookie)
{
	*_deviceCookie = driverCookie;
	return B_OK;
}


static void
pwmdac_uninit_device(void* deviceCookie)
{
}


static status_t
pwmdac_open(void* deviceCookie, const char* path, int openMode,
	void** _cookie)
{
	pwmdac_driver* driver = (pwmdac_driver*)deviceCookie;
	MutexLocker locker(driver->lock);
	status_t status = prepare_hardware(driver);
	if (status != B_OK)
		return status;
	driver->openCount++;
	*_cookie = driver;
	return B_OK;
}


static status_t
pwmdac_close(void* cookie)
{
	return B_OK;
}


static status_t
pwmdac_free(void* cookie)
{
	pwmdac_driver* driver = (pwmdac_driver*)cookie;
	MutexLocker locker(driver->lock);
	if (--driver->openCount == 0)
		playback_stop(driver);
	return B_OK;
}


static status_t
pwmdac_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
pwmdac_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
pwmdac_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	return multi_audio_control_generic((pwmdac_driver*)cookie, op, buffer,
		length);
}


//	#pragma mark - driver module API


static float
pwmdac_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}

	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK
		|| strcmp(compatible, "starfive,jh7110-pwmdac") != 0) {
		return 0.0f;
	}

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
pwmdac_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "JH7110 PWM-DAC audio" }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, PWMDAC_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
pwmdac_init_driver(device_node* node, void** _driverCookie)
{
	pwmdac_driver* driver = new(std::nothrow) pwmdac_driver;
	if (driver == NULL)
		return B_NO_MEMORY;
	memset(driver, 0, sizeof(*driver));
	driver->node = node;
	driver->registerArea = driver->bufferArea = -1;
	mutex_init(&driver->lock, "jh7110_pwmdac");
	B_INITIALIZE_SPINLOCK(&driver->positionLock);

	driver->fdtNode = sDeviceManager->get_parent_node(node);
	status_t status = B_OK;
	driver->bufferReadySem = create_sem(0, "jh7110_pwmdac buffer ready");
	if (driver->bufferReadySem < 0)
		status = driver->bufferReadySem;

	if (status != B_OK) {
		TRACE_ALWAYS("init failed: %s\n", strerror(status));
		sDeviceManager->put_node(driver->fdtNode);
		mutex_destroy(&driver->lock);
		delete driver;
		return status;
	}

	*_driverCookie = driver;
	return B_OK;
}


static void
pwmdac_uninit_driver(void* driverCookie)
{
	pwmdac_driver* driver = (pwmdac_driver*)driverCookie;
	mutex_lock(&driver->lock);
	playback_stop(driver);
	mutex_unlock(&driver->lock);

	delete_sem(driver->bufferReadySem);
	unmap_hardware(driver);
	sDeviceManager->put_node(driver->fdtNode);
	mutex_destroy(&driver->lock);
	delete driver;
}


static status_t
pwmdac_register_child_devices(void* driverCookie)
{
	pwmdac_driver* driver = (pwmdac_driver*)driverCookie;
	return sDeviceManager->publish_device(driver->node, PWMDAC_DEVICE_PATH,
		PWMDAC_DEVICE_MODULE_NAME);
}


driver_module_info pwmdac_driver_module = {
	{
		PWMDAC_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	pwmdac_supports_device,
	pwmdac_register_device,
	pwmdac_init_driver,
	pwmdac_uninit_driver,
	pwmdac_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


struct device_module_info pwmdac_device_module = {
	{
		PWMDAC_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	pwmdac_init_device,
	pwmdac_uninit_device,
	NULL,	// removed

	pwmdac_open,
	pwmdac_close,
	pwmdac_free,
	pwmdac_read,
	pwmdac_write,
	NULL,	// io
	pwmdac_control,

	NULL,	// select
	NULL	// deselect
};


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


module_info* modules[] = {
	(module_info*)&pwmdac_driver_module,
	(module_info*)&pwmdac_device_module,
	NULL
};
