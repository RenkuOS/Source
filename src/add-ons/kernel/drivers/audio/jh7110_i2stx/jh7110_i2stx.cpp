/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


//!	First I2S transmitter of the StarFive JH7110 ("starfive,jh7110-i2stx0").
// A Synopsys DesignWare I2S block in master mode; inside the SoC its output
// goes to the HDMI transmitter, which sends the samples as HDMI audio. The
// JH7110 clock generator makes the bit and frame clocks (I2STX0_BCLK_MST and
// I2STX0_LRCK_MST, the latter by default BCLK / 64), the block only shifts
// the samples out.
//
// The block's interrupts are not used: a DMA channel of the AXI DMA
// controller fills the transmit FIFO on request, one 16 bit write per
// sample, left and right in turn, from a ring of two buffers. The DMA
// interrupt after each buffer gives the playback position.
//
// MCLK is 256 fs, BCLK 64 fs. MCLK comes from PLL2 (1188 MHz) through
// integer dividers, so 48 kHz is only approximated (48339 Hz); the
// played_real_time reported to the media kit lets it follow the real rate.
//
// The register layout and the start sequence (no DMACR write on the JH7110,
// 16 bit resolution, FIFO threshold at half the depth) were checked against
// Linux's sound/soc/dwc/dwc-i2s.c and local.h.


#include <ByteOrder.h>
#include <KernelExport.h>
#include <bus/FDT.h>
#include <device_manager.h>
#include <driver_settings.h>
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


#define DRIVER_NAME		"jh7110_i2stx"
#define I2STX_DRIVER_MODULE_NAME \
	"drivers/audio/hmulti/jh7110_i2stx/driver_v1"
#define I2STX_DEVICE_MODULE_NAME \
	"drivers/audio/hmulti/jh7110_i2stx/device_v1"
#define I2STX_DEVICE_PATH "audio/hmulti/jh7110_i2stx/1"

#define TRACE_ALWAYS(x...)	dprintf(DRIVER_NAME ": " x)



// DesignWare I2S registers
#define I2S_IER						0x000		// block enable
#define I2S_ITER					0x008		// transmitter block enable
#define I2S_CER						0x00c		// clock enable
#define I2S_CCR						0x010		// clock configuration
#define I2S_TXFFR					0x018		// transmit FIFO reset
#define I2S_TER0					0x02c		// channel 0 transmit enable
#define I2S_TER(n)					(0x02c + 0x40 * (n))
#define I2S_TX_CHANNELS				4			// stereo pairs of the block
#define I2S_TCR0					0x034		// channel 0 resolution
#define I2S_IMR0					0x03c		// channel 0 interrupt mask
#define		IMR_TX_MASK					0x30	// TX FIFO empty and overrun
#define I2S_TFCR0					0x04c		// channel 0 FIFO threshold
#define I2S_TXDMA					0x1c8		// transmit FIFO for the DMA
#define I2S_COMP_PARAM_2			0x1f0
#define I2S_COMP_PARAM_1			0x1f4
#define I2S_COMP_VERSION			0x1f8
#define I2S_COMP_TYPE				0x1fc

#define TCR_16_BIT					0x2
#define CCR_16_CLOCKS				0x0		// word select after 16 bit clocks

// HDMI transmitter (Innosilicon, no device tree node): 8 bit registers,
// register n at base + 4 * n. Its clocks, resets and power belong to the
// display, which the firmware brought up; they are only checked here.
#define HDMI_BASE					0x29590000
#define HDMI_SIZE					0x4000
#define HDMI_AV_MUTE				0x05
#define		AV_MUTE_AUDIO				(1u << 1)
#define HDMI_AUDIO_CTRL1			0x35
#define		AUDIO_CTRL1_MCLK_256FS		0x01
#define		AUDIO_CTRL1_SOURCE_MASK		(3u << 3)	// 0: I2S
#define		AUDIO_CTRL1_MCLK_MASK		(3u << 0)
#define		AUDIO_CTRL1_CTS_EXTERNAL	(1u << 7)	// CTS from 0x45 - 0x47
#define HDMI_AUDIO_SAMPLE_RATE		0x37
#define		SAMPLE_RATE_48K				0x2
#define HDMI_AUDIO_I2S_MODE			0x38
#define		I2S_MODE_MASK				0x3f		// lines [5:2], format [1:0]
#define		I2S_MODE_LINE0_STANDARD		(1u << 2)
#define HDMI_AUDIO_I2S_MAP			0x39
#define HDMI_AUDIO_N_HIGH			0x3f
#define HDMI_AUDIO_CTS_HIGH			0x45
#define HDMI_HDCP_CTRL				0x52
#define		HDCP_CTRL_HDMI_MODE			(1u << 1)
#define HDMI_PACKET_INDEX			0x9f
#define		PACKET_INDEX_AVI			0x06
#define HDMI_PACKET_DATA			0xa0		// AVI: VIC at + 7
#define HDMI_STATUS					0xc8
#define		STATUS_HOTPLUG				(1u << 7)
#define HDMI_AUDIO_N_48K			6144		// 128 fs / 1000

#define PMU_BASE					0x17030000
#define		PMU_CURR_POWER_MODE			0x80
#define		PMU_PD_VOUT					(1u << 4)
#define SYSCRG_BASE					0x13020000
#define SYSCRG_RESET_ASSERT			0x2f8		// + 4 * (id / 32)
#define VOUTCRG_BASE				0x295c0000
#define		VOUTCRG_HDMI_TX_SYS			0x44
#define		VOUTCRG_RESET_ASSERT		0x48
#define		VOUTRST_HDMI_TX				(1u << 9)
#define CLOCK_ENABLE				(1u << 31)

enum {
	HDMI_CTS_EXACT = 0,		// CTS for the real sample rate (coherent)
	HDMI_CTS_NOMINAL,		// CTS for 48 kHz, as the HDMI specification lists
	HDMI_CTS_INTERNAL,		// the transmitter measures CTS itself
};

#define I2STX_MCLK					12288000	// 256 fs at 48 kHz
#define I2STX_MCLK_PER_SAMPLE		256
#define I2STX_BCLK_PER_SAMPLE		64

// The playback ring: two buffers, one DMA period each.
#define PLAYBACK_CHANNELS			2
#define PLAYBACK_SAMPLE_SIZE		2
#define PLAYBACK_FRAME_SIZE			(PLAYBACK_CHANNELS * PLAYBACK_SAMPLE_SIZE)
#define PLAYBACK_BUFFERS			2
#define PLAYBACK_BUFFER_FRAMES		1024
#define PLAYBACK_BUFFER_SIZE		(PLAYBACK_BUFFER_FRAMES * PLAYBACK_FRAME_SIZE)
#define PLAYBACK_RING_SIZE			(PLAYBACK_BUFFERS * PLAYBACK_BUFFER_SIZE)
#define EXCHANGE_TIMEOUT			500000


struct i2stx_driver {
	device_node*			node;
	device_node*			fdtNode;
	area_id					registerArea;
	addr_t					registers;
	phys_addr_t				registersPhysical;
	area_id					bufferArea;
	uint8*					buffer;
	phys_addr_t				bufferPhysical;
	int64					mclk;
	uint32					fifoThreshold;

	dw_axi_dma_module_info*	dma;
	dw_axi_dma_channel*		dmaChannel;
	bool					hardwareReady;

	area_id					hdmiArea;
	addr_t					hdmi;			// 0: no HDMI audio
	int32					hdmiCtsMode;

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
	{ 2, B_MULTI_OUTPUT_BUS, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0 },
	{ 3, B_MULTI_OUTPUT_BUS, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0 },
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


//	#pragma mark - hardware


/*!	As Linux's jh7110_i2s_crg_master_init(): APB clock on, MCLK from
	MCLK_INNER (the external MCLK input is not used), bit clock on, both
	resets released. MCLK is set to 256 fs and the bit clock to 64 fs;
	LRCK_MST keeps its default divider of 64.
*/
static status_t
enable_clocks(i2stx_driver* driver)
{
	fdt_clock_reset_module_info* clocks;
	if (get_module(JH7110_CLOCK_RESET_MODULE_NAME, (module_info**)&clocks)
			!= B_OK) {
		TRACE_ALWAYS("no clock/reset driver\n");
		return B_NOT_SUPPORTED;
	}

	fdt_clock apb, mclk, mclkInner, bclk;
	fdt_reset apbReset, bclkReset;
	status_t status = clocks->get_clock(driver->fdtNode, "apb", 0, &apb);
	if (status == B_OK)
		status = clocks->get_clock(driver->fdtNode, "mclk", 0, &mclk);
	if (status == B_OK) {
		status = clocks->get_clock(driver->fdtNode, "mclk_inner", 0,
			&mclkInner);
	}
	if (status == B_OK)
		status = clocks->get_clock(driver->fdtNode, "i2sclk", 0, &bclk);
	if (status == B_OK)
		status = clocks->get_reset(driver->fdtNode, NULL, 0, &apbReset);
	if (status == B_OK)
		status = clocks->get_reset(driver->fdtNode, NULL, 1, &bclkReset);
	if (status != B_OK) {
		TRACE_ALWAYS("clocks: %s\n", strerror(status));
		put_module(JH7110_CLOCK_RESET_MODULE_NAME);
		return status;
	}

	// MCLK must be MCLK_INNER: the tree never changes a mux, so check it
	int64 inner = clocks->get_clock_rate(mclkInner);
	if (clocks->get_clock_rate(mclk) != inner || inner <= 0) {
		TRACE_ALWAYS("clocks: MCLK is not on MCLK_INNER\n");
		put_module(JH7110_CLOCK_RESET_MODULE_NAME);
		return B_NOT_SUPPORTED;
	}

	status = clocks->enable_clock(apb, true);
	if (status == B_OK) {
		driver->mclk = clocks->set_clock_rate(mclk, I2STX_MCLK);
		if (driver->mclk <= 0)
			status = driver->mclk < 0 ? (status_t)driver->mclk : B_ERROR;
	}
	if (status == B_OK) {
		int64 bclkRate = clocks->set_clock_rate(bclk, driver->mclk
			* I2STX_BCLK_PER_SAMPLE / I2STX_MCLK_PER_SAMPLE);
		if (bclkRate <= 0)
			status = bclkRate < 0 ? (status_t)bclkRate : B_ERROR;
		else
			TRACE_ALWAYS("MCLK %" B_PRId64 " Hz, BCLK %" B_PRId64 " Hz\n",
				driver->mclk, bclkRate);
	}
	if (status == B_OK)
		status = clocks->enable_clock(bclk, true);
	if (status == B_OK)
		status = clocks->set_reset(apbReset, false);
	if (status == B_OK)
		status = clocks->set_reset(bclkReset, false);
	if (status != B_OK)
		TRACE_ALWAYS("clocks: %s\n", strerror(status));

	put_module(JH7110_CLOCK_RESET_MODULE_NAME);
	return status;
}


/*!	Channel 0 for 16 bit samples, with the FIFO threshold at half its
	depth. The TX FIFO interrupts are unmasked, as Linux does in DMA mode:
	this block (1.06a, no DMACR) seems to raise its DMA request from the
	same condition. The node has no interrupt line, so nothing is taken.
*/
static void
i2stx_configure(i2stx_driver* driver)
{
	addr_t regs = driver->registers;
	// All the stereo channels are enabled after reset, and each enabled one
	// takes its samples from the FIFO: only channel 0 may stay on.
	for (uint32 i = 0; i < I2S_TX_CHANNELS; i++)
		write32(regs + I2S_TER(i), 0);
	write32(regs + I2S_TCR0, TCR_16_BIT);
	write32(regs + I2S_TFCR0, driver->fifoThreshold - 1);
	write32(regs + I2S_IMR0, read32(regs + I2S_IMR0) & ~IMR_TX_MASK);
	write32(regs + I2S_TER0, 1);
	write32(regs + I2S_CCR, CCR_16_CLOCKS);
	write32(regs + I2S_TXFFR, 1);
}


static void
i2stx_set_enabled(i2stx_driver* driver, bool enable)
{
	addr_t regs = driver->registers;
	if (enable) {
		write32(regs + I2S_IER, 1);
		write32(regs + I2S_ITER, 1);
		write32(regs + I2S_CER, 1);
	} else {
		write32(regs + I2S_ITER, 0);
		write32(regs + I2S_CER, 0);
		write32(regs + I2S_IER, 0);
	}
}


//	#pragma mark - HDMI


static inline uint8
hdmi_read(i2stx_driver* driver, uint32 reg)
{
	return read32(driver->hdmi + 4 * reg) & 0xff;
}


static inline void
hdmi_write(i2stx_driver* driver, uint32 reg, uint8 value)
{
	write32(driver->hdmi + 4 * reg, value);
}


static uint32
read_physical(phys_addr_t address)
{
	void* virt;
	area_id area = map_physical_memory("jh7110_i2stx probe",
		address & ~(phys_addr_t)(B_PAGE_SIZE - 1), B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, &virt);
	if (area < 0)
		return 0;
	uint32 value = *(volatile uint32*)((addr_t)virt
		+ (address & (B_PAGE_SIZE - 1)));
	delete_area(area);
	return value;
}


/*!	Whether the HDMI transmitter can be touched: the display power domain
	on, its bus clocks and resets as the firmware left them for the video
	(SYS gates 58, 60, 61, 62 on, SYS resets 26 and 43 released), the
	transmitter's system clock on and its reset released. Every register is
	read only after what it needs was checked: an unclocked block can stall
	the bus even on a read.
*/
static bool
hdmi_powered()
{
	if ((read_physical(PMU_BASE + PMU_CURR_POWER_MODE) & PMU_PD_VOUT) == 0)
		return false;
	static const uint32 kGates[] = { 58, 60, 61, 62 };
	for (size_t i = 0; i < B_COUNT_OF(kGates); i++) {
		if ((read_physical(SYSCRG_BASE + 4 * kGates[i]) & CLOCK_ENABLE) == 0)
			return false;
	}
	static const uint32 kResets[] = { 26, 43 };
	for (size_t i = 0; i < B_COUNT_OF(kResets); i++) {
		uint32 id = kResets[i];
		if ((read_physical(SYSCRG_BASE + SYSCRG_RESET_ASSERT + 4 * (id / 32))
				& (1u << (id % 32))) != 0) {
			return false;
		}
	}
	return (read_physical(VOUTCRG_BASE + VOUTCRG_HDMI_TX_SYS) & CLOCK_ENABLE)
			!= 0
		&& (read_physical(VOUTCRG_BASE + VOUTCRG_RESET_ASSERT)
			& VOUTRST_HDMI_TX) == 0;
}


/*!	The TMDS clock of the video mode the firmware set, from the VIC of the
	AVI InfoFrame it wrote; 0 for a mode not listed here.
*/
static int64
hdmi_tmds_rate(i2stx_driver* driver)
{
	if (hdmi_read(driver, HDMI_PACKET_INDEX) != PACKET_INDEX_AVI)
		hdmi_write(driver, HDMI_PACKET_INDEX, PACKET_INDEX_AVI);
	uint8 vic = hdmi_read(driver, HDMI_PACKET_DATA + 7) & 0x7f;
	switch (vic) {
		case 1:
			return 25175000;
		case 2: case 3: case 17: case 18:
			return 27000000;
		case 4: case 5: case 19: case 20:
			return 74250000;
		case 16: case 31:
			return 148500000;
	}
	TRACE_ALWAYS("HDMI: video mode VIC %u not known\n", vic);
	return 0;
}


/*!	Maps the HDMI transmitter and sets up its audio input for 2 channel I2S
	at MCLK = 256 fs, muted. The video side is not touched. Without a
	powered transmitter in HDMI mode with a sink, HDMI audio is left off.
*/
static void
hdmi_setup(i2stx_driver* driver)
{
	driver->hdmiCtsMode = HDMI_CTS_EXACT;
	void* settings = load_driver_settings(DRIVER_NAME);
	if (settings != NULL) {
		if (!get_driver_boolean_parameter(settings, "hdmi_audio", true,
				true)) {
			unload_driver_settings(settings);
			TRACE_ALWAYS("HDMI: audio off in the settings\n");
			return;
		}
		const char* cts = get_driver_parameter(settings, "hdmi_cts", NULL,
			NULL);
		if (cts != NULL && strcmp(cts, "nominal") == 0)
			driver->hdmiCtsMode = HDMI_CTS_NOMINAL;
		else if (cts != NULL && strcmp(cts, "internal") == 0)
			driver->hdmiCtsMode = HDMI_CTS_INTERNAL;
		unload_driver_settings(settings);
	}

	if (!hdmi_powered()) {
		TRACE_ALWAYS("HDMI: transmitter not powered, no HDMI audio\n");
		return;
	}
	void* address;
	driver->hdmiArea = map_physical_memory("jh7110_i2stx hdmi", HDMI_BASE,
		HDMI_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (driver->hdmiArea < 0)
		return;
	driver->hdmi = (addr_t)address;

	uint8 mode = hdmi_read(driver, HDMI_HDCP_CTRL);
	uint8 status = hdmi_read(driver, HDMI_STATUS);
	if ((mode & HDCP_CTRL_HDMI_MODE) == 0 || (status & STATUS_HOTPLUG) == 0) {
		TRACE_ALWAYS("HDMI: %s, no HDMI audio\n",
			(mode & HDCP_CTRL_HDMI_MODE) == 0 ? "DVI mode" : "no sink");
		delete_area(driver->hdmiArea);
		driver->hdmiArea = -1;
		driver->hdmi = 0;
		return;
	}

	// N and CTS: the sink rebuilds fs = f_TMDS * N / (128 * CTS)
	uint32 n = HDMI_AUDIO_N_48K;
	int64 tmds = hdmi_tmds_rate(driver);
	uint32 cts = 0;
	if (tmds == 0)
		driver->hdmiCtsMode = HDMI_CTS_INTERNAL;
	else if (driver->hdmiCtsMode == HDMI_CTS_NOMINAL)
		cts = tmds / 1000;
	else if (driver->hdmiCtsMode == HDMI_CTS_EXACT) {
		// fs = MCLK / 256, so CTS = f_TMDS * N * 2 / MCLK
		cts = (uint32)((tmds * n * 2 + driver->mclk / 2) / driver->mclk);
	}

	hdmi_write(driver, HDMI_AV_MUTE,
		hdmi_read(driver, HDMI_AV_MUTE) | AV_MUTE_AUDIO);

	uint8 control = hdmi_read(driver, HDMI_AUDIO_CTRL1);
	control &= ~(AUDIO_CTRL1_SOURCE_MASK | AUDIO_CTRL1_MCLK_MASK
		| AUDIO_CTRL1_CTS_EXTERNAL);
	control |= AUDIO_CTRL1_MCLK_256FS;
	if (driver->hdmiCtsMode != HDMI_CTS_INTERNAL)
		control |= AUDIO_CTRL1_CTS_EXTERNAL;
	hdmi_write(driver, HDMI_AUDIO_CTRL1, control);
	hdmi_write(driver, HDMI_AUDIO_SAMPLE_RATE, SAMPLE_RATE_48K);
	hdmi_write(driver, HDMI_AUDIO_I2S_MODE,
		(hdmi_read(driver, HDMI_AUDIO_I2S_MODE) & ~I2S_MODE_MASK)
			| I2S_MODE_LINE0_STANDARD);
	hdmi_write(driver, HDMI_AUDIO_I2S_MAP, 0);
	hdmi_write(driver, HDMI_AUDIO_N_HIGH, (n >> 16) & 0xf);
	hdmi_write(driver, HDMI_AUDIO_N_HIGH + 1, (n >> 8) & 0xff);
	hdmi_write(driver, HDMI_AUDIO_N_HIGH + 2, n & 0xff);
	hdmi_write(driver, HDMI_AUDIO_CTS_HIGH, (cts >> 16) & 0xf);
	hdmi_write(driver, HDMI_AUDIO_CTS_HIGH + 1, (cts >> 8) & 0xff);
	hdmi_write(driver, HDMI_AUDIO_CTS_HIGH + 2, cts & 0xff);

	static const char* const kCtsModes[] = { "exact", "nominal", "internal" };
	TRACE_ALWAYS("HDMI: audio set up, TMDS %" B_PRId64 " Hz, N %" B_PRIu32
		", CTS %s %" B_PRIu32 "\n", tmds, n, kCtsModes[driver->hdmiCtsMode],
		cts);
}


static void
hdmi_set_muted(i2stx_driver* driver, bool muted)
{
	if (driver->hdmi == 0)
		return;
	uint8 value = hdmi_read(driver, HDMI_AV_MUTE);
	if (muted)
		value |= AV_MUTE_AUDIO;
	else
		value &= ~AV_MUTE_AUDIO;
	hdmi_write(driver, HDMI_AV_MUTE, value);
}


static void
dma_period_done(void* cookie, uint32 currentPeriod)
{
	i2stx_driver* driver = (i2stx_driver*)cookie;

	acquire_spinlock(&driver->positionLock);
	driver->bufferCycle = currentPeriod % PLAYBACK_BUFFERS;
	driver->framesCount += PLAYBACK_BUFFER_FRAMES;
	driver->realTime = system_time();
	driver->interrupts++;
	release_spinlock(&driver->positionLock);

	release_sem_etc(driver->bufferReadySem, 1, B_DO_NOT_RESCHEDULE);
}


static status_t
playback_start(i2stx_driver* driver)
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
	transfer.deviceAddress = driver->registersPhysical + I2S_TXDMA;
	transfer.deviceWidth = PLAYBACK_SAMPLE_SIZE;
	transfer.callback = dma_period_done;
	transfer.cookie = driver;

	i2stx_configure(driver);
	status_t status = driver->dma->start_cyclic(driver->dmaChannel, &transfer);
	if (status != B_OK) {
		TRACE_ALWAYS("playback: DMA start failed: %s\n", strerror(status));
		return status;
	}
	i2stx_set_enabled(driver, true);
	hdmi_set_muted(driver, false);

	driver->running = true;
	TRACE_ALWAYS("playback: started\n");
	return B_OK;
}


static void
playback_stop(i2stx_driver* driver)
{
	if (!driver->running)
		return;

	// The DMA channel first, while the transmitter still asks for data: a
	// channel waiting for a request that never comes does not stop.
	hdmi_set_muted(driver, true);
	driver->dma->stop(driver->dmaChannel);
	i2stx_set_enabled(driver, false);
	driver->running = false;

	// Wake a buffer_exchange() that is still waiting.
	release_sem_etc(driver->bufferReadySem, 1, 0);

	TRACE_ALWAYS("playback: stopped after %" B_PRId64 " frames, %" B_PRIu32
		" interrupts, DMA errors %#" B_PRIx32 "\n", driver->framesCount,
		driver->interrupts, driver->dma->errors(driver->dmaChannel));
}


//	#pragma mark - multi_audio


static status_t
get_description(i2stx_driver* driver, multi_description* data)
{
	data->interface_version = B_CURRENT_INTERFACE_VERSION;
	data->interface_minimum = B_CURRENT_INTERFACE_VERSION;

	strlcpy(data->friendly_name, "HDMI (JH7110 I2S)",
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
get_enabled_channels(i2stx_driver* driver, multi_channel_enable* data)
{
	data->lock_source = B_MULTI_LOCK_INTERNAL;
	for (int32 i = 0; i < PLAYBACK_CHANNELS; i++)
		B_SET_CHANNEL(data->enable_bits, i, true);
	return B_OK;
}


static status_t
get_global_format(i2stx_driver* driver, multi_format_info* data)
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
set_global_format(i2stx_driver* driver, multi_format_info* data)
{
	// There is only one format; report it back.
	return get_global_format(driver, data);
}


static status_t
list_mix_controls(i2stx_driver* driver, multi_mix_control_info* data)
{
	// The DAC has no gain control; the system mixer sets the volume.
	data->control_count = 0;
	return B_OK;
}


static status_t
list_mix_connections(i2stx_driver* driver, multi_mix_connection_info* data)
{
	return B_ERROR;
}


static status_t
list_mix_channels(i2stx_driver* driver, multi_mix_channel_info* data)
{
	return B_ERROR;
}


static status_t
get_mix(i2stx_driver* driver, multi_mix_value_info* data)
{
	return B_OK;
}


static status_t
set_mix(i2stx_driver* driver, multi_mix_value_info* data)
{
	return B_OK;
}


/*!	The ring layout is fixed, so the requested buffer count and size are
	ignored.
*/
static status_t
get_buffers(i2stx_driver* driver, multi_buffer_list* data)
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
buffer_exchange(i2stx_driver* driver, multi_buffer_info* data)
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
buffer_force_stop(i2stx_driver* driver)
{
	MutexLocker locker(driver->lock);
	playback_stop(driver);
	return B_OK;
}


#define cookie_type i2stx_driver
#include "../generic/multi.c"


//	#pragma mark - setup


static void
unmap_hardware(i2stx_driver* driver)
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
	if (driver->hdmiArea >= 0)
		delete_area(driver->hdmiArea);
	driver->hdmiArea = -1;
	driver->hdmi = 0;
}


static status_t
map_hardware(i2stx_driver* driver)
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

	status_t status = enable_clocks(driver);
	if (status != B_OK)
		return status;
	TRACE_ALWAYS("%" B_PRId64 " Hz sample rate\n",
		driver->mclk / I2STX_MCLK_PER_SAMPLE);

	void* address;
	driver->registerArea = map_physical_memory("jh7110_i2stx registers",
		regs, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (driver->registerArea < 0)
		return driver->registerArea;
	driver->registers = (addr_t)address;
	i2stx_set_enabled(driver, false);

	// the FIFO depth is 2^(n + 1) words, n in COMP_PARAM_1 [3:2]
	uint32 param1 = read32(driver->registers + I2S_COMP_PARAM_1);
	uint32 fifoDepth = 1u << (1 + ((param1 >> 2) & 3));
	driver->fifoThreshold = fifoDepth / 2;
	TRACE_ALWAYS("DesignWare I2S version %#" B_PRIx32 ", type %#" B_PRIx32
		", parameters %#" B_PRIx32 "/%#" B_PRIx32 ", FIFO %" B_PRIu32
		" words\n", read32(driver->registers + I2S_COMP_VERSION),
		read32(driver->registers + I2S_COMP_TYPE), param1,
		read32(driver->registers + I2S_COMP_PARAM_2), fifoDepth);
	if ((param1 & (1u << 5)) == 0) {
		TRACE_ALWAYS("the block has no transmitter\n");
		return B_NOT_SUPPORTED;
	}

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
	driver->bufferArea = create_area("jh7110_i2stx playback", &address,
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

	hdmi_setup(driver);
	return B_OK;
}


/*!	The hardware is set up on the first open rather than at boot: the
	clocks and the DMA channel are only claimed once someone plays sound.
*/
static status_t
prepare_hardware(i2stx_driver* driver)
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
i2stx_init_device(void* driverCookie, void** _deviceCookie)
{
	*_deviceCookie = driverCookie;
	return B_OK;
}


static void
i2stx_uninit_device(void* deviceCookie)
{
}


static status_t
i2stx_open(void* deviceCookie, const char* path, int openMode,
	void** _cookie)
{
	i2stx_driver* driver = (i2stx_driver*)deviceCookie;
	MutexLocker locker(driver->lock);
	status_t status = prepare_hardware(driver);
	if (status != B_OK)
		return status;
	driver->openCount++;
	*_cookie = driver;
	return B_OK;
}


static status_t
i2stx_close(void* cookie)
{
	return B_OK;
}


static status_t
i2stx_free(void* cookie)
{
	i2stx_driver* driver = (i2stx_driver*)cookie;
	MutexLocker locker(driver->lock);
	if (--driver->openCount == 0)
		playback_stop(driver);
	return B_OK;
}


static status_t
i2stx_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
i2stx_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
i2stx_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	return multi_audio_control_generic((i2stx_driver*)cookie, op, buffer,
		length);
}


//	#pragma mark - driver module API


static float
i2stx_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}

	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK
		|| strcmp(compatible, "starfive,jh7110-i2stx0") != 0) {
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
i2stx_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "JH7110 I2S HDMI audio" }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, I2STX_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
i2stx_init_driver(device_node* node, void** _driverCookie)
{
	i2stx_driver* driver = new(std::nothrow) i2stx_driver;
	if (driver == NULL)
		return B_NO_MEMORY;
	memset(driver, 0, sizeof(*driver));
	driver->node = node;
	driver->registerArea = driver->bufferArea = driver->hdmiArea = -1;
	mutex_init(&driver->lock, "jh7110_i2stx");
	B_INITIALIZE_SPINLOCK(&driver->positionLock);

	driver->fdtNode = sDeviceManager->get_parent_node(node);
	status_t status = B_OK;
	driver->bufferReadySem = create_sem(0, "jh7110_i2stx buffer ready");
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
i2stx_uninit_driver(void* driverCookie)
{
	i2stx_driver* driver = (i2stx_driver*)driverCookie;
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
i2stx_register_child_devices(void* driverCookie)
{
	i2stx_driver* driver = (i2stx_driver*)driverCookie;
	return sDeviceManager->publish_device(driver->node, I2STX_DEVICE_PATH,
		I2STX_DEVICE_MODULE_NAME);
}


driver_module_info i2stx_driver_module = {
	{
		I2STX_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	i2stx_supports_device,
	i2stx_register_device,
	i2stx_init_driver,
	i2stx_uninit_driver,
	i2stx_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


struct device_module_info i2stx_device_module = {
	{
		I2STX_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	i2stx_init_device,
	i2stx_uninit_device,
	NULL,	// removed

	i2stx_open,
	i2stx_close,
	i2stx_free,
	i2stx_read,
	i2stx_write,
	NULL,	// io
	i2stx_control,

	NULL,	// select
	NULL	// deselect
};


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


module_info* modules[] = {
	(module_info*)&i2stx_driver_module,
	(module_info*)&i2stx_device_module,
	NULL
};
