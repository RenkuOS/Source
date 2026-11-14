/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


//!	Audio for AMD Stoney Ridge Chromebooks (ACP 2.2, DA7219, MAX98357A).
// The driver attaches to the DA7219 codec on I2C and publishes the speakers
// as a multi_audio device: 48 kHz, 16 bit stereo, two buffers of 1024
// frames.
//
// Speaker path: the FCH oscout1 clock (48 MHz) feeds the DA7219 PLL; the
// DA7219 is the I2S clock master for the bus. ACP DMA moves samples from
// system memory to ACP SRAM and from there to the "BT" I2S transmitter,
// which the MAX98357A amplifier listens to. GPIO 119 enables the amplifier.
//
// The ACP reports DMA completion through the GPU interrupt handler ring,
// which nothing in Renku services, so a kernel thread polls the DMA
// position instead.


#include <ACPI.h>
#include <KernelExport.h>
#include <PCI.h>
#include <device_manager.h>
#include <i2c.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include <hmulti_audio.h>

#include <new>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define DRIVER_NAME		"amd_acp"
#define AMD_ACP_DRIVER_MODULE_NAME "drivers/audio/amd_acp/driver_v1"
#define AMD_ACP_DEVICE_MODULE_NAME "drivers/audio/amd_acp/device_v1"
#define AMD_ACP_DEVICE_PATH "audio/hmulti/amd_acp/1"

#define TRACE_ALWAYS(x...)	dprintf(DRIVER_NAME ": " x)


// The ACP 2.2 registers sit in the GPU register BAR (BAR5 on Sea Islands
// and newer); register numbers are dword indices into that BAR.
#define PCI_VENDOR_ATI				0x1002
#define PCI_DEVICE_STONEY			0x98e4
#define GPU_MMIO_BAR				5
#define ACP_REGISTER_FIRST			0x5000
#define ACP_REGISTER_WINDOW			0x1000

// Register numbers and fields, from AMD's ACP 2.2 register headers (MIT).
#define mmACP_DMA_CNTL_0			0x5000
#define mmACP_DMA_DSCR_STRT_IDX_0	0x5010
#define mmACP_DMA_DSCR_CNT_0		0x5020
#define mmACP_DMA_PRIO_0			0x5030
#define mmACP_DMA_CUR_DSCR_0		0x5040
#define mmACP_DMA_ERR_STS_0			0x5060
#define mmACP_DMA_DESC_BASE_ADDR	0x5070
#define mmACP_DMA_DESC_MAX_NUM_DSCR	0x5071
#define mmACP_DMA_CH_STS			0x5072
#define mmACP_AXI2DAGB_ONION_CNTL	0x50e7
#define mmACP_AXI2DAGB_GARLIC_CNTL	0x50f3
#define mmACP_DAGB_PAGE_SIZE_GRP_1	0x50ff
#define mmACP_DAGB_BASE_ADDR_GRP_1	0x5100
#define mmACP_DAGB_ATU_CTRL			0x510f
#define mmACP_CONTROL				0x5131
#define mmACP_STATUS				0x5133
#define mmACP_SOFT_RESET			0x5134
#define mmACP_EXTERNAL_INTR_ENB		0x5157
#define mmACP_SRBM_TARG_IDX_ADDR	0x519f
#define mmACP_SRBM_TARG_IDX_DATA	0x51a0
#define mmACP_I2S_BT_TX_BYTE_CNT_HIGH	0x51b3
#define mmACP_I2S_BT_TX_BYTE_CNT_LOW	0x51b4
#define mmACP_I2S_PIN_CONFIG		0x51d3
#define mmACP_BT_UART_PAD_SEL		0x51d7
#define mmACP_I2S_16BIT_RESOLUTION_EN	0x5209

#define ACP_DMA_CNTL_RESET			(1 << 0)
#define ACP_DMA_CNTL_RUN			(1 << 1)
#define ACP_DMA_CNTL_IOC_ENABLE		(1 << 2)
#define ACP_DMA_CNTL_CIRCULAR		(1 << 3)
#define ACP_CONTROL_CLOCK_ENABLE	(1 << 0)
#define ACP_SOFT_RESET_AUDIO		(1 << 8)
#define ACP_SOFT_RESET_AUDIO_DONE	(1 << 24)
#define ACP_DAGB_SNOOP_SELECT		(1 << 29)
#define ACP_DAGB_TARGET_MEMORY		(1 << 30)
#define ACP_DAGB_GROUP_ENABLE		(1u << 31)
#define ACP_BT_UART_PAD_SELECT		(1 << 0)
#define ACP_I2S_BT_16BIT_RESOLUTION	(1 << 2)

// Layout of the ACP internal address space, as used for Stoney.
#define ACP_DAGB_DEFAULT_CNTL		0x00000fb4
#define ACP_SRAM_BASE_ADDRESS		0x04000000
#define ACP_DAGB_PTE_SRAM_ADDRESS	0x04001000
#define ACP_DAGB_PTE_SRBM_OFFSET	0x1000
#define ACP_SRAM_BANK_3_ADDRESS		0x04006000
#define ACP_SRAM_BANK_SIZE			0x2000
#define ACP_PAGE_SIZE_4K			0x02
#define ACP_DESCRIPTOR_SIZE			16
#define ACP_DESCRIPTOR_COUNT_256	0x4

// Speaker playback uses the BT instance: channel 8 copies system memory to
// SRAM bank 3, channel 9 feeds the BT I2S transmitter from it.
#define ACP_BT_PLAYBACK_PTE_OFFSET	8
#define ACP_BT_SYSMEM_CHANNEL		8
#define ACP_BT_I2S_CHANNEL			9
#define ACP_BT_SYSMEM_DESCRIPTOR	8
#define ACP_BT_I2S_DESCRIPTOR		10
#define ACP_DMA_ATTR_GARLIC_TO_SRAM	0x9
#define ACP_DMA_TO_BLUETOOTH		0x3
#define ACP_DMA_TO_I2S				(1 << 22)
#define ACP_DMA_STOP_TIMEOUT		1000000

// The BT I2S transmitter is a Synopsys DesignWare I2S block at this offset
// from the ACP register base, with its parameter registers moved.
#define ACP_BT_I2S_OFFSET			0x970
#define DW_I2S_IER					0x000
#define DW_I2S_ITER					0x008
#define DW_I2S_CER					0x00c
#define DW_I2S_CCR					0x010
#define DW_I2S_TXFFR				0x018
#define DW_I2S_TER0					0x02c
#define DW_I2S_TCR0					0x034
#define DW_I2S_IMR0					0x03c
#define DW_I2S_TFCR0				0x04c
#define DW_I2S_BT_COMP_PARAM_1		0x0ac
#define DW_I2S_RESOLUTION_16BIT		0x02
#define DW_I2S_IMR_TX				((1 << 4) | (1 << 5))

// FCH "ACPI MMIO" blocks: the misc block drives the oscout1 clock that is
// the DA7219 master clock; the GPIO block has one dword per pin.
#define FCH_ACPI_MMIO_BASE			0xfed80000
#define FCH_ACPI_MMIO_SIZE			0x2000
#define FCH_MISC_OFFSET				0x0e00
#define FCH_MISC_CLKDRVSTR2			0x28
#define FCH_MISC_MISCCLKCNTL1		0x40
#define FCH_OSCOUT1_DISABLE			(1 << 2)
#define FCH_OSCOUT1_25MHZ			(1 << 16)
#define FCH_GPIO_OFFSET				0x1500
#define FCH_GPIO_OUTPUT_VALUE		(1 << 22)
#define FCH_GPIO_OUTPUT_ENABLE		(1 << 23)
#define FCH_GPIO_STATUS_BITS		((1 << 28) | (1 << 29))

#define GPIO_SPEAKER_ENABLE			119

// DA7219 registers, from its datasheet and the Linux register header.
#define DA7219_SR					0x17
#define DA7219_PLL_CTRL				0x20
#define DA7219_PLL_FRAC_TOP			0x22
#define DA7219_PLL_FRAC_BOT			0x23
#define DA7219_PLL_INTEGER			0x24
#define DA7219_PLL_SRM_STS			0x25
#define DA7219_DAI_CLK_MODE			0x2b
#define DA7219_DAI_CTRL				0x2c
#define DA7219_REFERENCES			0x32
#define DA7219_CHIP_ID1				0x81
#define DA7219_CHIP_ID2				0x82
#define DA7219_CHIP_REVISION		0x83
#define DA7219_SYSTEM_ACTIVE		0xfd

#define DA7219_EXPECTED_ID1			0x23
#define DA7219_EXPECTED_ID2			0x93
#define DA7219_SR_48000				0x0b
#define DA7219_PLL_INDIV_MASK		(0x7 << 2)
#define DA7219_PLL_INDIV_36_TO_54_MHZ	(0x4 << 2)
#define DA7219_PLL_MCLK_SQR_EN		(1 << 5)
#define DA7219_PLL_MODE_MASK		(0x3 << 6)
#define DA7219_PLL_MODE_NORMAL		(0x1 << 6)
#define DA7219_PLL_SRM_STS_MCLK		(1 << 4)
#define DA7219_DAI_BCLKS_PER_WCLK_MASK	0x3
#define DA7219_DAI_BCLKS_PER_WCLK_64	0x1
#define DA7219_DAI_CLK_EN			(1 << 7)
#define DA7219_DAI_FORMAT_MASK		0x3
#define DA7219_DAI_WORD_LENGTH_MASK	(0x3 << 2)
#define DA7219_DAI_CH_NUM_MASK		(0x3 << 4)
#define DA7219_DAI_CH_NUM_2			(0x2 << 4)
#define DA7219_DAI_EN				(1 << 7)
#define DA7219_BIAS_EN				(1 << 3)
#define DA7219_SYSTEM_ACTIVE_BIT	(1 << 0)

// 48 MHz MCLK / 16 = 3 MHz reference; 98.304 MHz = 3 MHz * (32 + 6291/8192)
#define DA7219_PLL_INTEGER_98304	32
#define DA7219_PLL_FRACTION_98304	6291

// The playback ring is one SRAM bank: two buffers, one per DMA descriptor.
#define PLAYBACK_RATE				48000
#define PLAYBACK_CHANNELS			2
#define PLAYBACK_SAMPLE_SIZE		2
#define PLAYBACK_FRAME_SIZE			(PLAYBACK_CHANNELS * PLAYBACK_SAMPLE_SIZE)
#define PLAYBACK_BUFFERS			2
#define PLAYBACK_RING_SIZE			ACP_SRAM_BANK_SIZE
#define PLAYBACK_BUFFER_SIZE		(PLAYBACK_RING_SIZE / PLAYBACK_BUFFERS)
#define PLAYBACK_BUFFER_FRAMES		(PLAYBACK_BUFFER_SIZE / PLAYBACK_FRAME_SIZE)
#define PLAYBACK_BUFFER_TIME \
	((bigtime_t)PLAYBACK_BUFFER_FRAMES * 1000000 / PLAYBACK_RATE)

// How the poll thread waits for the next buffer boundary: it sleeps most of
// a buffer after a boundary, then checks every POLL_INTERVAL.
#define POLL_INTERVAL				500
#define POLL_EARLY_WAKEUP			3000
#define EXCHANGE_TIMEOUT			500000


struct amd_acp_driver {
	device_node*			node;
	i2c_device_interface*	i2c;
	i2c_device				i2cCookie;
	pci_info				gpu;
	area_id					acpArea;
	addr_t					acp;
	area_id					fchArea;
	addr_t					fch;
	area_id					bufferArea;
	uint8*					buffer;
	phys_addr_t				bufferPhysical;

	// Protects the fields below and the start and stop of playback.
	mutex					lock;
	int32					openCount;
	int32					running;
	thread_id				pollThread;
	sem_id					bufferReadySem;

	// Playback position, written by the poll thread.
	spinlock				positionLock;
	uint32					bufferCycle;
	int64					framesCount;
	bigtime_t				realTime;
};


static device_manager_info* sDeviceManager;
static pci_module_info* sPCI;

static multi_channel_info sChannels[] = {
	{ 0, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0 },
	{ 1, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0 },
	{ 2, B_MULTI_OUTPUT_BUS, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_ANALOG_HEADER },
	{ 3, B_MULTI_OUTPUT_BUS, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_ANALOG_HEADER },
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


static inline uint32
acp_read(amd_acp_driver* driver, uint32 reg)
{
	return read32(driver->acp + (reg - ACP_REGISTER_FIRST) * 4);
}


static inline void
acp_write(amd_acp_driver* driver, uint32 reg, uint32 value)
{
	write32(driver->acp + (reg - ACP_REGISTER_FIRST) * 4, value);
}


static inline uint32
i2s_read(amd_acp_driver* driver, uint32 offset)
{
	return read32(driver->acp + ACP_BT_I2S_OFFSET + offset);
}


static inline void
i2s_write(amd_acp_driver* driver, uint32 offset, uint32 value)
{
	write32(driver->acp + ACP_BT_I2S_OFFSET + offset, value);
}


//	#pragma mark - DA7219


static status_t
da7219_read(amd_acp_driver* driver, uint8 reg, uint8* value)
{
	status_t status = driver->i2c->acquire_bus(driver->i2cCookie);
	if (status != B_OK)
		return status;
	status = driver->i2c->exec_command(driver->i2cCookie, I2C_OP_READ_STOP,
		&reg, 1, value, 1);
	driver->i2c->release_bus(driver->i2cCookie);
	return status;
}


static status_t
da7219_write(amd_acp_driver* driver, uint8 reg, uint8 value)
{
	uint8 command[2] = { reg, value };
	status_t status = driver->i2c->acquire_bus(driver->i2cCookie);
	if (status != B_OK)
		return status;
	status = driver->i2c->exec_command(driver->i2cCookie, I2C_OP_WRITE_STOP,
		command, sizeof(command), NULL, 0);
	driver->i2c->release_bus(driver->i2cCookie);
	return status;
}


static status_t
da7219_update(amd_acp_driver* driver, uint8 reg, uint8 mask, uint8 value)
{
	uint8 old;
	status_t status = da7219_read(driver, reg, &old);
	if (status != B_OK)
		return status;
	return da7219_write(driver, reg, (old & ~mask) | (value & mask));
}


static status_t
da7219_probe(amd_acp_driver* driver)
{
	uint8 id1 = 0, id2 = 0, revision = 0;
	status_t status = da7219_read(driver, DA7219_CHIP_ID1, &id1);
	if (status == B_OK)
		status = da7219_read(driver, DA7219_CHIP_ID2, &id2);
	if (status == B_OK)
		status = da7219_read(driver, DA7219_CHIP_REVISION, &revision);
	if (status != B_OK) {
		TRACE_ALWAYS("codec: I2C read failed: %s\n", strerror(status));
		return status;
	}

	bool isDA7219 = id1 == DA7219_EXPECTED_ID1 && id2 == DA7219_EXPECTED_ID2;
	TRACE_ALWAYS("codec: chip id %02x %02x revision %02x (%s)\n", id1, id2,
		revision, isDA7219 ? "DA7219" : "unexpected");
	return isDA7219 ? B_OK : ENODEV;
}


/*!	Runs the PLL from the 48 MHz MCLK and makes the codec drive 48 kHz
	word and 64 fs bit clocks onto the I2S bus.
*/
static status_t
da7219_start_clocks(amd_acp_driver* driver)
{
	status_t status = da7219_update(driver, DA7219_SYSTEM_ACTIVE,
		DA7219_SYSTEM_ACTIVE_BIT, DA7219_SYSTEM_ACTIVE_BIT);
	if (status == B_OK) {
		status = da7219_update(driver, DA7219_REFERENCES, DA7219_BIAS_EN,
			DA7219_BIAS_EN);
	}
	if (status == B_OK) {
		status = da7219_write(driver, DA7219_PLL_FRAC_TOP,
			DA7219_PLL_FRACTION_98304 >> 8);
	}
	if (status == B_OK) {
		status = da7219_write(driver, DA7219_PLL_FRAC_BOT,
			DA7219_PLL_FRACTION_98304 & 0xff);
	}
	if (status == B_OK) {
		status = da7219_write(driver, DA7219_PLL_INTEGER,
			DA7219_PLL_INTEGER_98304);
	}
	if (status == B_OK) {
		status = da7219_update(driver, DA7219_PLL_CTRL,
			DA7219_PLL_INDIV_MASK | DA7219_PLL_MODE_MASK
				| DA7219_PLL_MCLK_SQR_EN,
			DA7219_PLL_INDIV_36_TO_54_MHZ | DA7219_PLL_MODE_NORMAL);
	}
	if (status == B_OK)
		status = da7219_write(driver, DA7219_SR, DA7219_SR_48000);
	if (status == B_OK) {
		status = da7219_update(driver, DA7219_DAI_CLK_MODE,
			DA7219_DAI_BCLKS_PER_WCLK_MASK, DA7219_DAI_BCLKS_PER_WCLK_64);
	}
	if (status == B_OK) {
		// I2S, 16 bit words, 2 channels.
		status = da7219_update(driver, DA7219_DAI_CTRL,
			DA7219_DAI_EN | DA7219_DAI_CH_NUM_MASK
				| DA7219_DAI_WORD_LENGTH_MASK | DA7219_DAI_FORMAT_MASK,
			DA7219_DAI_EN | DA7219_DAI_CH_NUM_2);
	}
	if (status != B_OK) {
		TRACE_ALWAYS("codec: setup failed: %s\n", strerror(status));
		return status;
	}

	snooze(10000);

	uint8 pllStatus = 0;
	da7219_read(driver, DA7219_PLL_SRM_STS, &pllStatus);
	if ((pllStatus & DA7219_PLL_SRM_STS_MCLK) == 0)
		TRACE_ALWAYS("codec: no master clock (PLL_SRM_STS %#04x)\n", pllStatus);

	status = da7219_update(driver, DA7219_DAI_CLK_MODE, DA7219_DAI_CLK_EN,
		DA7219_DAI_CLK_EN);
	if (status != B_OK)
		TRACE_ALWAYS("codec: enabling clocks failed: %s\n", strerror(status));
	return status;
}


static void
da7219_stop_clocks(amd_acp_driver* driver)
{
	da7219_update(driver, DA7219_DAI_CLK_MODE, DA7219_DAI_CLK_EN, 0);
	da7219_update(driver, DA7219_DAI_CTRL, DA7219_DAI_EN, 0);
}


//	#pragma mark - FCH clock and GPIO


static void
fch_set_oscout1(amd_acp_driver* driver, bool enable)
{
	addr_t misc = driver->fch + FCH_MISC_OFFSET;
	write32(misc + FCH_MISC_CLKDRVSTR2,
		read32(misc + FCH_MISC_CLKDRVSTR2) & ~FCH_OSCOUT1_25MHZ);
	uint32 control = read32(misc + FCH_MISC_MISCCLKCNTL1);
	if (enable)
		control &= ~FCH_OSCOUT1_DISABLE;
	else
		control |= FCH_OSCOUT1_DISABLE;
	write32(misc + FCH_MISC_MISCCLKCNTL1, control);
}


static void
fch_set_gpio(amd_acp_driver* driver, uint32 pin, bool high)
{
	addr_t address = driver->fch + FCH_GPIO_OFFSET + pin * 4;
	uint32 value = read32(address) & ~FCH_GPIO_STATUS_BITS;
	value |= FCH_GPIO_OUTPUT_ENABLE;
	if (high)
		value |= FCH_GPIO_OUTPUT_VALUE;
	else
		value &= ~FCH_GPIO_OUTPUT_VALUE;
	write32(address, value);
}


//	#pragma mark - ACP


static void
acp_log_state(amd_acp_driver* driver)
{
	TRACE_ALWAYS("acp: CONTROL %#" B_PRIx32 ", STATUS %#" B_PRIx32
		", SOFT_RESET %#010" B_PRIx32 ", I2S_PIN_CONFIG %#" B_PRIx32
		", BT_UART_PAD_SEL %#" B_PRIx32 "\n",
		acp_read(driver, mmACP_CONTROL), acp_read(driver, mmACP_STATUS),
		acp_read(driver, mmACP_SOFT_RESET),
		acp_read(driver, mmACP_I2S_PIN_CONFIG),
		acp_read(driver, mmACP_BT_UART_PAD_SEL));
}


static status_t
acp_wait(amd_acp_driver* driver, uint32 reg, uint32 mask, uint32 value,
	bigtime_t timeout)
{
	bigtime_t end = system_time() + timeout;
	do {
		if ((acp_read(driver, reg) & mask) == value)
			return B_OK;
		spin(100);
	} while (system_time() < end);
	return B_TIMED_OUT;
}


static status_t
acp_init(amd_acp_driver* driver)
{
	acp_write(driver, mmACP_SOFT_RESET,
		acp_read(driver, mmACP_SOFT_RESET) | ACP_SOFT_RESET_AUDIO);
	if (acp_wait(driver, mmACP_SOFT_RESET, ACP_SOFT_RESET_AUDIO_DONE,
			ACP_SOFT_RESET_AUDIO_DONE, 25000) != B_OK) {
		TRACE_ALWAYS("acp: soft reset did not complete\n");
		return B_TIMED_OUT;
	}

	acp_write(driver, mmACP_CONTROL,
		acp_read(driver, mmACP_CONTROL) | ACP_CONTROL_CLOCK_ENABLE);
	if (acp_wait(driver, mmACP_STATUS, 1, 1, 25000) != B_OK) {
		TRACE_ALWAYS("acp: clock did not come up\n");
		return B_TIMED_OUT;
	}

	acp_write(driver, mmACP_SOFT_RESET,
		acp_read(driver, mmACP_SOFT_RESET) & ~ACP_SOFT_RESET_AUDIO);

	// Route the BT I2S pads to the ACP instead of the UART.
	acp_write(driver, mmACP_BT_UART_PAD_SEL,
		acp_read(driver, mmACP_BT_UART_PAD_SEL) | ACP_BT_UART_PAD_SELECT);

	acp_write(driver, mmACP_AXI2DAGB_ONION_CNTL, ACP_DAGB_DEFAULT_CNTL);
	acp_write(driver, mmACP_AXI2DAGB_GARLIC_CNTL, ACP_DAGB_DEFAULT_CNTL);
	acp_write(driver, mmACP_DAGB_BASE_ADDR_GRP_1, ACP_DAGB_PTE_SRAM_ADDRESS
		| ACP_DAGB_SNOOP_SELECT | ACP_DAGB_TARGET_MEMORY
		| ACP_DAGB_GROUP_ENABLE);
	acp_write(driver, mmACP_DAGB_PAGE_SIZE_GRP_1, ACP_PAGE_SIZE_4K);
	acp_write(driver, mmACP_DMA_DESC_BASE_ADDR, ACP_SRAM_BASE_ADDRESS);
	acp_write(driver, mmACP_DMA_DESC_MAX_NUM_DSCR, ACP_DESCRIPTOR_COUNT_256);

	// Nothing services the GPU interrupt handler ring the ACP reports to;
	// the poll thread follows the DMA position instead.
	acp_write(driver, mmACP_EXTERNAL_INTR_ENB, 0);
	return B_OK;
}


static void
acp_sram_write(amd_acp_driver* driver, uint32 offset, uint32 value)
{
	acp_write(driver, mmACP_SRBM_TARG_IDX_ADDR, offset);
	acp_write(driver, mmACP_SRBM_TARG_IDX_DATA, value);
}


static void
acp_write_descriptor(amd_acp_driver* driver, uint32 index, uint32 source,
	uint32 destination, uint32 transfer)
{
	uint32 offset = index * ACP_DESCRIPTOR_SIZE;
	acp_sram_write(driver, offset, source);
	acp_sram_write(driver, offset + 4, destination);
	acp_sram_write(driver, offset + 8, transfer);
}


static void
acp_setup_channel(amd_acp_driver* driver, uint32 channel,
	uint32 firstDescriptor)
{
	uint32 control = mmACP_DMA_CNTL_0 + channel;

	acp_write(driver, control,
		acp_read(driver, control) & ~ACP_DMA_CNTL_RESET);
	if (acp_wait(driver, control, ACP_DMA_CNTL_RESET, 0, 25000) != B_OK)
		TRACE_ALWAYS("acp: channel %" B_PRIu32 " stuck in reset\n", channel);

	acp_write(driver, control, acp_read(driver, control) & ~ACP_DMA_CNTL_RUN);
	acp_write(driver, mmACP_DMA_DSCR_STRT_IDX_0 + channel, firstDescriptor);
	acp_write(driver, mmACP_DMA_DSCR_CNT_0 + channel, PLAYBACK_BUFFERS);
	acp_write(driver, mmACP_DMA_PRIO_0 + channel, 0);
}


/*!	Like Linux, sets the completion flag only on the channel that feeds the
	I2S block; the ACP interrupt itself stays disabled.
*/
static void
acp_start_channel(amd_acp_driver* driver, uint32 channel, bool completion)
{
	uint32 control = mmACP_DMA_CNTL_0 + channel;
	uint32 value = acp_read(driver, control) | ACP_DMA_CNTL_RUN
		| ACP_DMA_CNTL_CIRCULAR;
	if (completion)
		value |= ACP_DMA_CNTL_IOC_ENABLE;
	else
		value &= ~ACP_DMA_CNTL_IOC_ENABLE;
	acp_write(driver, mmACP_DAGB_ATU_CTRL, 1);
	acp_write(driver, control, value);
}


static void
acp_stop_channel(amd_acp_driver* driver, uint32 channel)
{
	uint32 control = mmACP_DMA_CNTL_0 + channel;
	uint32 value = acp_read(driver, control)
		& ~(ACP_DMA_CNTL_RUN | ACP_DMA_CNTL_IOC_ENABLE);
	acp_write(driver, control, value);

	if ((acp_read(driver, mmACP_DMA_CH_STS) & (1 << channel)) != 0)
		acp_write(driver, control, value | ACP_DMA_CNTL_RESET);
	if (acp_wait(driver, mmACP_DMA_CH_STS, 1 << channel, 0,
			ACP_DMA_STOP_TIMEOUT) != B_OK) {
		TRACE_ALWAYS("acp: channel %" B_PRIu32 " did not stop\n", channel);
	}
	acp_write(driver, control, value & ~ACP_DMA_CNTL_RESET);
}


/*!	Points ACP DMA at the playback ring: the page table maps it at the BT
	playback PTE offset, channel 8 copies both buffers into SRAM bank 3 and
	channel 9 sends them to the BT I2S transmitter, both circularly.
*/
static void
acp_setup_playback(amd_acp_driver* driver)
{
	acp_write(driver, mmACP_I2S_16BIT_RESOLUTION_EN,
		acp_read(driver, mmACP_I2S_16BIT_RESOLUTION_EN)
			| ACP_I2S_BT_16BIT_RESOLUTION);

	for (uint32 i = 0; i < PLAYBACK_RING_SIZE / B_PAGE_SIZE; i++) {
		uint64 address = driver->bufferPhysical + i * B_PAGE_SIZE;
		uint32 offset = ACP_DAGB_PTE_SRBM_OFFSET
			+ (ACP_BT_PLAYBACK_PTE_OFFSET + i) * 8;
		acp_sram_write(driver, offset, (uint32)address);
		acp_sram_write(driver, offset + 4,
			(uint32)(address >> 32) | (1u << 31));
	}

	for (uint32 i = 0; i < PLAYBACK_BUFFERS; i++) {
		acp_write_descriptor(driver, ACP_BT_SYSMEM_DESCRIPTOR + i,
			ACP_BT_PLAYBACK_PTE_OFFSET * B_PAGE_SIZE
				+ i * PLAYBACK_BUFFER_SIZE,
			ACP_SRAM_BANK_3_ADDRESS + i * PLAYBACK_BUFFER_SIZE,
			(ACP_DMA_ATTR_GARLIC_TO_SRAM << 16) | PLAYBACK_BUFFER_SIZE);
		acp_write_descriptor(driver, ACP_BT_I2S_DESCRIPTOR + i,
			ACP_SRAM_BANK_3_ADDRESS + i * PLAYBACK_BUFFER_SIZE, 0,
			ACP_DMA_TO_I2S | (ACP_DMA_TO_BLUETOOTH << 16)
				| PLAYBACK_BUFFER_SIZE);
	}

	acp_setup_channel(driver, ACP_BT_SYSMEM_CHANNEL,
		ACP_BT_SYSMEM_DESCRIPTOR);
	acp_setup_channel(driver, ACP_BT_I2S_CHANNEL, ACP_BT_I2S_DESCRIPTOR);
}


static uint64
acp_transmitted_bytes(amd_acp_driver* driver)
{
	uint32 high, low;
	do {
		high = acp_read(driver, mmACP_I2S_BT_TX_BYTE_CNT_HIGH);
		low = acp_read(driver, mmACP_I2S_BT_TX_BYTE_CNT_LOW);
	} while (high != acp_read(driver, mmACP_I2S_BT_TX_BYTE_CNT_HIGH));
	return ((uint64)high << 32) | low;
}


/*!	Unmasks the transmit FIFO empty and overrun interrupts as Linux does at
	playback start: inside the ACP, the FIFO empty interrupt of the BT I2S
	block is what asks DMA channel 9 for data.
*/
static void
i2s_start(amd_acp_driver* driver)
{
	uint32 comp1 = i2s_read(driver, DW_I2S_BT_COMP_PARAM_1);
	uint32 fifoDepth = 1 << (1 + ((comp1 >> 2) & 0x3));

	i2s_write(driver, DW_I2S_TER0, 0);
	i2s_write(driver, DW_I2S_TCR0, DW_I2S_RESOLUTION_16BIT);
	i2s_write(driver, DW_I2S_TFCR0, fifoDepth / 2 - 1);
	i2s_write(driver, DW_I2S_TER0, 1);
	i2s_write(driver, DW_I2S_CCR, 0);
	i2s_write(driver, DW_I2S_TXFFR, 1);
	i2s_write(driver, DW_I2S_IMR0,
		i2s_read(driver, DW_I2S_IMR0) & ~DW_I2S_IMR_TX);

	i2s_write(driver, DW_I2S_IER, 1);
	i2s_write(driver, DW_I2S_ITER, 1);
	i2s_write(driver, DW_I2S_CER, 1);
}


static void
i2s_stop(amd_acp_driver* driver)
{
	i2s_write(driver, DW_I2S_IMR0,
		i2s_read(driver, DW_I2S_IMR0) | DW_I2S_IMR_TX);
	i2s_write(driver, DW_I2S_ITER, 0);
	i2s_write(driver, DW_I2S_TER0, 0);
	i2s_write(driver, DW_I2S_CER, 0);
	i2s_write(driver, DW_I2S_IER, 0);
}


//	#pragma mark - playback


/*!	Follows DMA channel 9 from one descriptor to the next. Each switch means
	a buffer finished playing; buffer_exchange() then hands that buffer to
	the media add-on to fill.
*/
static int32
playback_poll_thread(void* data)
{
	amd_acp_driver* driver = (amd_acp_driver*)data;
	uint32 lastDescriptor = ACP_BT_I2S_DESCRIPTOR;

	while (atomic_get(&driver->running) != 0) {
		uint32 descriptor = acp_read(driver,
			mmACP_DMA_CUR_DSCR_0 + ACP_BT_I2S_CHANNEL);
		if (descriptor == lastDescriptor
			|| descriptor < ACP_BT_I2S_DESCRIPTOR
			|| descriptor >= ACP_BT_I2S_DESCRIPTOR + PLAYBACK_BUFFERS) {
			snooze(POLL_INTERVAL);
			continue;
		}
		lastDescriptor = descriptor;

		uint64 bytes = acp_transmitted_bytes(driver);
		bigtime_t now = system_time();

		cpu_status state = disable_interrupts();
		acquire_spinlock(&driver->positionLock);
		driver->bufferCycle = descriptor - ACP_BT_I2S_DESCRIPTOR;
		driver->framesCount = bytes / PLAYBACK_FRAME_SIZE;
		driver->realTime = now;
		release_spinlock(&driver->positionLock);
		restore_interrupts(state);

		release_sem_etc(driver->bufferReadySem, 1, B_DO_NOT_RESCHEDULE);
		snooze(PLAYBACK_BUFFER_TIME - POLL_EARLY_WAKEUP);
	}
	return 0;
}


static status_t
playback_start(amd_acp_driver* driver)
{
	int32 count;
	if (get_sem_count(driver->bufferReadySem, &count) == B_OK && count > 0)
		acquire_sem_etc(driver->bufferReadySem, count, B_RELATIVE_TIMEOUT, 0);

	fch_set_oscout1(driver, true);
	snooze(1000);
	status_t status = da7219_start_clocks(driver);
	if (status == B_OK)
		status = acp_init(driver);
	if (status != B_OK) {
		da7219_stop_clocks(driver);
		fch_set_oscout1(driver, false);
		return status;
	}

	driver->bufferCycle = 0;
	driver->framesCount = 0;
	driver->realTime = system_time();

	acp_setup_playback(driver);
	acp_start_channel(driver, ACP_BT_SYSMEM_CHANNEL, false);
	acp_start_channel(driver, ACP_BT_I2S_CHANNEL, true);
	i2s_start(driver);
	fch_set_gpio(driver, GPIO_SPEAKER_ENABLE, true);

	driver->running = 1;
	driver->pollThread = spawn_kernel_thread(playback_poll_thread,
		"amd_acp playback", B_REAL_TIME_DISPLAY_PRIORITY, driver);
	if (driver->pollThread < 0) {
		status = driver->pollThread;
		TRACE_ALWAYS("playback: no poll thread: %s\n", strerror(status));
		driver->running = 0;
		fch_set_gpio(driver, GPIO_SPEAKER_ENABLE, false);
		acp_stop_channel(driver, ACP_BT_I2S_CHANNEL);
		i2s_stop(driver);
		acp_stop_channel(driver, ACP_BT_SYSMEM_CHANNEL);
		da7219_stop_clocks(driver);
		fch_set_oscout1(driver, false);
		return status;
	}
	resume_thread(driver->pollThread);

	TRACE_ALWAYS("playback: started\n");
	return B_OK;
}


static void
playback_stop(amd_acp_driver* driver)
{
	if (!driver->running)
		return;

	atomic_set(&driver->running, 0);
	status_t result;
	wait_for_thread(driver->pollThread, &result);
	driver->pollThread = -1;

	uint64 bytes = acp_transmitted_bytes(driver);

	// Stop channel 9 while the transmitter still asks for data, then the
	// transmitter, then the copy into SRAM.
	fch_set_gpio(driver, GPIO_SPEAKER_ENABLE, false);
	acp_stop_channel(driver, ACP_BT_I2S_CHANNEL);
	i2s_stop(driver);
	acp_stop_channel(driver, ACP_BT_SYSMEM_CHANNEL);
	da7219_stop_clocks(driver);
	fch_set_oscout1(driver, false);

	// Wake a buffer_exchange() that is still waiting.
	release_sem_etc(driver->bufferReadySem, 1, 0);

	TRACE_ALWAYS("playback: stopped after %" B_PRIu64 " frames, DMA errors %#"
		B_PRIx32 "/%#" B_PRIx32 "\n", bytes / PLAYBACK_FRAME_SIZE,
		acp_read(driver, mmACP_DMA_ERR_STS_0 + ACP_BT_SYSMEM_CHANNEL),
		acp_read(driver, mmACP_DMA_ERR_STS_0 + ACP_BT_I2S_CHANNEL));
}


//	#pragma mark - multi_audio


static status_t
get_description(amd_acp_driver* driver, multi_description* data)
{
	data->interface_version = B_CURRENT_INTERFACE_VERSION;
	data->interface_minimum = B_CURRENT_INTERFACE_VERSION;

	strlcpy(data->friendly_name, "Speakers (AMD ACP)",
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
get_enabled_channels(amd_acp_driver* driver, multi_channel_enable* data)
{
	data->lock_source = B_MULTI_LOCK_INTERNAL;
	for (int32 i = 0; i < PLAYBACK_CHANNELS; i++)
		B_SET_CHANNEL(data->enable_bits, i, true);
	return B_OK;
}


static status_t
get_global_format(amd_acp_driver* driver, multi_format_info* data)
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
set_global_format(amd_acp_driver* driver, multi_format_info* data)
{
	// There is only one format; report it back.
	return get_global_format(driver, data);
}


static status_t
list_mix_controls(amd_acp_driver* driver, multi_mix_control_info* data)
{
	// The amplifier has no gain control; the system mixer sets the volume.
	data->control_count = 0;
	return B_OK;
}


static status_t
list_mix_connections(amd_acp_driver* driver, multi_mix_connection_info* data)
{
	return B_ERROR;
}


static status_t
list_mix_channels(amd_acp_driver* driver, multi_mix_channel_info* data)
{
	return B_ERROR;
}


static status_t
get_mix(amd_acp_driver* driver, multi_mix_value_info* data)
{
	return B_OK;
}


static status_t
set_mix(amd_acp_driver* driver, multi_mix_value_info* data)
{
	return B_OK;
}


/*!	The ring layout is fixed by the hardware setup, so the requested buffer
	count and size are ignored.
*/
static status_t
get_buffers(amd_acp_driver* driver, multi_buffer_list* data)
{
	MutexLocker locker(driver->lock);

	if (!driver->running)
		memset(driver->buffer, 0, PLAYBACK_RING_SIZE);

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
buffer_exchange(amd_acp_driver* driver, multi_buffer_info* data)
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
	if (status != B_OK)
		return status;

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
buffer_force_stop(amd_acp_driver* driver)
{
	MutexLocker locker(driver->lock);
	playback_stop(driver);
	return B_OK;
}


#define cookie_type amd_acp_driver
#include "../generic/multi.c"


//	#pragma mark - setup


static status_t
map_hardware(amd_acp_driver* driver)
{
	bool found = false;
	for (int32 i = 0; sPCI->get_nth_pci_info(i, &driver->gpu) == B_OK; i++) {
		if (driver->gpu.vendor_id == PCI_VENDOR_ATI
			&& driver->gpu.device_id == PCI_DEVICE_STONEY) {
			found = true;
			break;
		}
	}
	if (!found) {
		TRACE_ALWAYS("acp: no Stoney GPU (%04x:%04x) found\n", PCI_VENDOR_ATI,
			PCI_DEVICE_STONEY);
		return ENODEV;
	}

	phys_addr_t barAddress = driver->gpu.u.h0.base_registers[GPU_MMIO_BAR];
	uint32 barSize = driver->gpu.u.h0.base_register_sizes[GPU_MMIO_BAR];
	uint32 windowOffset = ACP_REGISTER_FIRST * 4;
	if (barAddress == 0 || barSize < windowOffset + ACP_REGISTER_WINDOW) {
		TRACE_ALWAYS("acp: register BAR too small for the ACP block\n");
		return ENODEV;
	}

	void* address;
	driver->acpArea = map_physical_memory("amd_acp registers",
		barAddress + windowOffset, ACP_REGISTER_WINDOW, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (driver->acpArea < 0)
		return driver->acpArea;
	driver->acp = (addr_t)address;

	driver->fchArea = map_physical_memory("amd_acp fch", FCH_ACPI_MMIO_BASE,
		FCH_ACPI_MMIO_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (driver->fchArea < 0)
		return driver->fchArea;
	driver->fch = (addr_t)address;

	// The media add-on writes into the ring directly, like with hda. The
	// ACP reads it without snooping the CPU caches, so it is uncached.
	driver->bufferArea = create_area("amd_acp playback", &address,
		B_ANY_KERNEL_ADDRESS, PLAYBACK_RING_SIZE, B_32_BIT_CONTIGUOUS,
		B_READ_AREA | B_WRITE_AREA);
	if (driver->bufferArea < 0)
		return driver->bufferArea;
	driver->buffer = (uint8*)address;

	physical_entry entry;
	status_t status = get_memory_map(address, PLAYBACK_RING_SIZE, &entry, 1);
	if (status != B_OK)
		return status;
	driver->bufferPhysical = entry.address;

	status = vm_set_area_memory_type(driver->bufferArea,
		driver->bufferPhysical, B_UNCACHED_MEMORY);
	if (status != B_OK) {
		TRACE_ALWAYS("acp: cannot make the ring uncached: %s\n",
			strerror(status));
		return status;
	}
	memset(driver->buffer, 0, PLAYBACK_RING_SIZE);
	return B_OK;
}


static void
unmap_hardware(amd_acp_driver* driver)
{
	if (driver->bufferArea >= 0)
		delete_area(driver->bufferArea);
	if (driver->fchArea >= 0)
		delete_area(driver->fchArea);
	if (driver->acpArea >= 0)
		delete_area(driver->acpArea);
}


//	#pragma mark - device module API


static status_t
amd_acp_init_device(void* driverCookie, void** _deviceCookie)
{
	*_deviceCookie = driverCookie;
	return B_OK;
}


static void
amd_acp_uninit_device(void* deviceCookie)
{
}


static status_t
amd_acp_open(void* deviceCookie, const char* path, int openMode,
	void** _cookie)
{
	amd_acp_driver* driver = (amd_acp_driver*)deviceCookie;
	MutexLocker locker(driver->lock);
	driver->openCount++;
	*_cookie = driver;
	return B_OK;
}


static status_t
amd_acp_close(void* cookie)
{
	return B_OK;
}


static status_t
amd_acp_free(void* cookie)
{
	amd_acp_driver* driver = (amd_acp_driver*)cookie;
	MutexLocker locker(driver->lock);
	if (--driver->openCount == 0)
		playback_stop(driver);
	return B_OK;
}


static status_t
amd_acp_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
amd_acp_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
amd_acp_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	return multi_audio_control_generic((amd_acp_driver*)cookie, op, buffer,
		length);
}


//	#pragma mark - driver module API


static float
amd_acp_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK
		|| strcmp(bus, "i2c") != 0) {
		return 0.0;
	}

	const char* hid;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid,
			false) != B_OK
		|| strcmp(hid, "DLGS7219") != 0) {
		return 0.0;
	}

	return 0.6;
}


static status_t
amd_acp_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "AMD ACP audio (DA7219)" }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, AMD_ACP_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
amd_acp_init_driver(device_node* node, void** _driverCookie)
{
	amd_acp_driver* driver = new(std::nothrow) amd_acp_driver;
	if (driver == NULL)
		return B_NO_MEMORY;
	memset(driver, 0, sizeof(*driver));
	driver->node = node;
	driver->acpArea = driver->fchArea = driver->bufferArea = -1;
	driver->pollThread = -1;
	mutex_init(&driver->lock, "amd_acp");
	B_INITIALIZE_SPINLOCK(&driver->positionLock);

	device_node* parent = sDeviceManager->get_parent_node(node);
	status_t status = sDeviceManager->get_driver(parent,
		(driver_module_info**)&driver->i2c, (void**)&driver->i2cCookie);
	sDeviceManager->put_node(parent);

	if (status == B_OK)
		status = da7219_probe(driver);
	if (status == B_OK)
		status = map_hardware(driver);
	if (status == B_OK) {
		driver->bufferReadySem = create_sem(0, "amd_acp buffer ready");
		if (driver->bufferReadySem < 0)
			status = driver->bufferReadySem;
	}

	if (status != B_OK) {
		TRACE_ALWAYS("probe failed: %s\n", strerror(status));
		unmap_hardware(driver);
		mutex_destroy(&driver->lock);
		delete driver;
		return status;
	}

	acp_log_state(driver);
	TRACE_ALWAYS("ready, playback ring at %#" B_PRIxPHYSADDR "\n",
		driver->bufferPhysical);
	*_driverCookie = driver;
	return B_OK;
}


static void
amd_acp_uninit_driver(void* driverCookie)
{
	amd_acp_driver* driver = (amd_acp_driver*)driverCookie;
	mutex_lock(&driver->lock);
	playback_stop(driver);
	mutex_unlock(&driver->lock);

	delete_sem(driver->bufferReadySem);
	unmap_hardware(driver);
	mutex_destroy(&driver->lock);
	delete driver;
}


static status_t
amd_acp_register_child_devices(void* driverCookie)
{
	amd_acp_driver* driver = (amd_acp_driver*)driverCookie;
	return sDeviceManager->publish_device(driver->node, AMD_ACP_DEVICE_PATH,
		AMD_ACP_DEVICE_MODULE_NAME);
}


driver_module_info amd_acp_driver_module = {
	{
		AMD_ACP_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	amd_acp_supports_device,
	amd_acp_register_device,
	amd_acp_init_driver,
	amd_acp_uninit_driver,
	amd_acp_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


struct device_module_info amd_acp_device_module = {
	{
		AMD_ACP_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	amd_acp_init_device,
	amd_acp_uninit_device,
	NULL,	// removed

	amd_acp_open,
	amd_acp_close,
	amd_acp_free,
	amd_acp_read,
	amd_acp_write,
	NULL,	// io
	amd_acp_control,

	NULL,	// select
	NULL	// deselect
};


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ B_PCI_MODULE_NAME, (module_info**)&sPCI },
	{}
};


module_info* modules[] = {
	(module_info*)&amd_acp_driver_module,
	(module_info*)&amd_acp_device_module,
	NULL
};
