/*
 * Copyright 2026, Ian Dumych. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 *
 * Panel bring-up for the PINE64 PinePhone (Allwinner A64): Xingbangda XBD599
 * 720x1440 panel (Sitronix ST7703) on 4-lane MIPI-DSI, fed by TCON0 and
 * mixer 0 of the Display Engine 2.0, panel rails on the AXP803 PMIC (RSB),
 * PWM backlight.
 *
 * Tow-Boot initializes none of this and provides no GOP, so the loader does
 * it once and hands the frame buffer to the kernel like a GOP one. Register
 * values follow the Allwinner A64 user manual and the register-level account
 * of the same bring-up by Lee Lupyuen for Apache NuttX
 * (https://lupyuen.github.io/articles/de, .../dsi3, .../pio, .../lcd).
 *
 * Without a serial console the RGB LED tells how far it got: three white
 * blinks on start, then one colour per completed step (see Stage); a step
 * that fails blinks its colour quickly for three seconds.
 */


#include "pinephone_display.h"

#include <string.h>

#include <KernelExport.h>
#include <boot/kernel_args.h>
#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/stdio.h>

extern "C" {
#include <libfdt.h>
}

#include "dtb.h"
#include "efi_platform.h"


//#define TRACE_PINEPHONE_DISPLAY
#ifdef TRACE_PINEPHONE_DISPLAY
#	define TRACE(x...) dprintf("pinephone: " x)
#else
#	define TRACE(x...) ;
#endif
#define ERROR(x...) dprintf("pinephone: " x)


namespace {


static const uint32 kPanelWidth = 720;
static const uint32 kPanelHeight = 1440;
static const uint32 kBacklightPercent = 90;

// A64 memory map
static const addr_t kDisplayEngine = 0x01000000;
static const addr_t kSystemControl = 0x01c00000;
static const addr_t kTcon0 = 0x01c0c000;
static const addr_t kCcu = 0x01c20000;
static const addr_t kPio = 0x01c20800;
static const addr_t kDsi = 0x01ca0000;
static const addr_t kDphy = 0x01ca1000;
static const addr_t kRPrcm = 0x01f01400;
static const addr_t kRPio = 0x01f02c00;
static const addr_t kRsb = 0x01f03400;
static const addr_t kRPwm = 0x01f03800;

// CCU
static const addr_t kPllVideo0 = kCcu + 0x010;
static const addr_t kPllMipi = kCcu + 0x040;
static const addr_t kPllDe = kCcu + 0x048;
static const addr_t kBusGate0 = kCcu + 0x060;
static const addr_t kBusGate1 = kCcu + 0x064;
static const addr_t kDeClock = kCcu + 0x104;
static const addr_t kTcon0Clock = kCcu + 0x118;
static const addr_t kMipiDsiClock = kCcu + 0x168;
static const addr_t kBusReset0 = kCcu + 0x2c0;
static const addr_t kBusReset1 = kCcu + 0x2c4;
static const uint32 kBus0MipiDsi = 1 << 1;
static const uint32 kBus1Tcon0 = 1 << 3;
static const uint32 kBus1De = 1 << 12;
static const uint32 kPllEnable = 1u << 31;
static const uint32 kPllLock = 1 << 28;

// PIO ports
enum { kPortB = 1, kPortD = 3, kPortH = 7, kPortL = 0 };
enum { kPioInput = 0, kPioOutput = 1, kPioFunction2 = 2 };

// The LED: PD18 green, PD19 red, PD20 blue.
enum {
	kLedOff = 0,
	kLedGreen = 1,
	kLedRed = 2,
	kLedBlue = 4,
	kLedYellow = kLedRed | kLedGreen,
	kLedMagenta = kLedRed | kLedBlue,
	kLedCyan = kLedGreen | kLedBlue,
	kLedWhite = kLedRed | kLedGreen | kLedBlue
};

// LED colour once a step is done.
enum Stage {
	kStageTcon = kLedRed,			// backlight and TCON0 set up
	kStagePower = kLedYellow,		// panel rails on (AXP803 answered)
	kStageDsi = kLedMagenta,		// DSI controller and D-PHY on
	kStagePanel = kLedBlue,			// ST7703 took its init sequence
	kStageVideo = kLedCyan,			// DSI video mode, display engine on
	kStageScanout = kLedGreen,		// frame buffer layer scanned out
	kStageKernel = kLedWhite		// loader starting the kernel
};


static bool sActive = false;
static addr_t sFrameBuffer;
static size_t sFrameBufferSize;


// #pragma mark - register access


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


static inline void
modify32(addr_t address, uint32 mask, uint32 value)
{
	write32(address, (read32(address) & ~mask) | (value & mask));
}


static inline void
set32(addr_t address, uint32 bits)
{
	modify32(address, bits, bits);
}


static inline void
clear32(addr_t address, uint32 bits)
{
	modify32(address, bits, 0);
}


static void
delay_ms(uint32 milliseconds)
{
	spin(milliseconds * 1000);
}


static void
clean_dcache(addr_t start, size_t size)
{
	uint64 ctr;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	size_t line = 4 << ((ctr >> 16) & 0xf);
	for (addr_t address = start & ~(line - 1); address < start + size;
			address += line) {
		asm volatile("dc cvac, %0" : : "r"(address) : "memory");
	}
	asm volatile("dsb sy" : : : "memory");
}


// #pragma mark - GPIO, LED


static void
pio_configure(addr_t base, uint32 port, uint32 pin, uint32 function)
{
	addr_t config = base + port * 0x24 + (pin / 8) * 4;
	uint32 shift = (pin % 8) * 4;
	modify32(config, 0x7 << shift, function << shift);
}


static void
pio_set(addr_t base, uint32 port, uint32 pin, bool high)
{
	addr_t data = base + port * 0x24 + 0x10;
	if (high)
		set32(data, 1 << pin);
	else
		clear32(data, 1 << pin);
}


static void
led_set(uint32 colours)
{
	pio_set(kPio, kPortD, 18, (colours & kLedGreen) != 0);
	pio_set(kPio, kPortD, 19, (colours & kLedRed) != 0);
	pio_set(kPio, kPortD, 20, (colours & kLedBlue) != 0);
}


static void
led_init()
{
	led_set(kLedOff);
	for (uint32 pin = 18; pin <= 20; pin++)
		pio_configure(kPio, kPortD, pin, kPioOutput);
}


static void
led_blink(uint32 colours, uint32 count, uint32 periodMs)
{
	for (uint32 i = 0; i < count; i++) {
		led_set(colours);
		delay_ms(periodMs / 2);
		led_set(kLedOff);
		delay_ms(periodMs / 2);
	}
}


static void
stage_done(Stage stage)
{
	led_set(stage);
}


static void
stage_failed(Stage stage)
{
	led_blink(stage, 15, 200);
	led_set(stage);
}


// #pragma mark - AXP803 over RSB


static const addr_t kRsbControl = kRsb + 0x00;
static const addr_t kRsbClock = kRsb + 0x04;
static const addr_t kRsbStatus = kRsb + 0x0c;
static const addr_t kRsbAddress = kRsb + 0x10;
static const addr_t kRsbData = kRsb + 0x1c;
static const addr_t kRsbPmicControl = kRsb + 0x28;
static const addr_t kRsbCommand = kRsb + 0x2c;
static const addr_t kRsbDeviceAddress = kRsb + 0x30;
static const uint32 kRsbSoftReset = 1 << 0;
static const uint32 kRsbStart = 1 << 7;
static const uint32 kRsbTransferOver = 1 << 0;
static const uint32 kRsbCommandWrite8 = 0x4e;
static const uint32 kRsbCommandRead8 = 0x8b;
static const uint32 kRsbCommandSetRuntimeAddress = 0xe8;

static const uint32 kAxp803HardwareAddress = 0x3a3;
static const uint32 kAxp803RuntimeAddress = 0x2d;


static status_t
rsb_transfer()
{
	write32(kRsbStatus, read32(kRsbStatus));
		// write-1-to-clear what an earlier transfer left
	write32(kRsbControl, kRsbStart);
	for (int i = 0; i < 100; i++) {
		if ((read32(kRsbControl) & kRsbStart) == 0)
			return read32(kRsbStatus) == kRsbTransferOver ? B_OK : B_ERROR;
		spin(50);
	}
	return B_TIMED_OUT;
}


static status_t
rsb_read(uint8 reg, uint8& value)
{
	write32(kRsbCommand, kRsbCommandRead8);
	write32(kRsbDeviceAddress, kAxp803RuntimeAddress << 16);
	write32(kRsbAddress, reg);
	status_t status = rsb_transfer();
	if (status == B_OK)
		value = read32(kRsbData) & 0xff;
	return status;
}


static status_t
rsb_write(uint8 reg, uint8 value)
{
	write32(kRsbCommand, kRsbCommandWrite8);
	write32(kRsbDeviceAddress, kAxp803RuntimeAddress << 16);
	write32(kRsbAddress, reg);
	write32(kRsbData, value);
	return rsb_transfer();
}


// U-Boot leaves the RSB set up for the AXP803; redo what it does in case it
// did not: clocks, pins, 3 MHz bus clock, the PMIC switched from I2C to RSB
// mode and given its runtime address.
static status_t
rsb_init()
{
	set32(kRPrcm + 0x28, 1 << 3);		// APB0 gate: RSB
	set32(kRPrcm + 0xb0, 1 << 3);		// APB0 reset: RSB deasserted
	pio_configure(kRPio, kPortL, 0, kPioFunction2);	// S_RSB_SCK
	pio_configure(kRPio, kPortL, 1, kPioFunction2);	// S_RSB_SDA

	write32(kRsbControl, kRsbSoftReset);
	for (int i = 0; (read32(kRsbControl) & kRsbSoftReset) != 0; i++) {
		if (i > 100)
			return B_TIMED_OUT;
		spin(10);
	}
	write32(kRsbClock, (1 << 8) | 3);
		// SDA output delay 1, 24 MHz / (2 * (3 + 1)) = 3 MHz

	write32(kRsbPmicControl, (1u << 31) | (0x7c << 16) | (0x3e << 8));
		// "PMU init": register 0x3e = 0x7c switches the PMIC to RSB
	for (int i = 0; (read32(kRsbPmicControl) & (1u << 31)) != 0; i++) {
		if (i > 100)
			return B_TIMED_OUT;
		spin(10);
	}

	write32(kRsbCommand, kRsbCommandSetRuntimeAddress);
	write32(kRsbDeviceAddress,
		(kAxp803RuntimeAddress << 16) | kAxp803HardwareAddress);
	return rsb_transfer();
}


static status_t
pmic_set_bits(uint8 reg, uint8 bits)
{
	uint8 value;
	status_t status = rsb_read(reg, value);
	if (status != B_OK)
		return status;
	return rsb_write(reg, value | bits);
}


// Panel rails: DLDO1 3.3 V (DSI, touch), GPIO0 as LDO 3.3 V (panel VCC),
// DLDO2 1.8 V (panel IOVCC).
static status_t
pmic_init()
{
	uint8 chipId = 0;
	status_t status = rsb_read(0x03, chipId);
	if (status != B_OK) {
		ERROR("AXP803 does not answer (%" B_PRId32 "), setting up RSB\n",
			status);
		status = rsb_init();
		if (status == B_OK)
			status = rsb_read(0x03, chipId);
		if (status != B_OK) {
			ERROR("no AXP803 on the RSB: %" B_PRId32 "\n", status);
			return status;
		}
	}
	TRACE("AXP803 chip ID %#x\n", chipId);

	if ((status = rsb_write(0x15, 26)) != B_OK			// DLDO1 3.3 V
		|| (status = pmic_set_bits(0x12, 1 << 3)) != B_OK	// DLDO1 on
		|| (status = rsb_write(0x91, 26)) != B_OK			// GPIO0 LDO 3.3 V
		|| (status = rsb_write(0x90, 0x3)) != B_OK			// GPIO0: LDO on
		|| (status = rsb_write(0x16, 11)) != B_OK			// DLDO2 1.8 V
		|| (status = pmic_set_bits(0x12, 1 << 4)) != B_OK) {	// DLDO2 on
		ERROR("setting the panel rails failed: %" B_PRId32 "\n", status);
		return status;
	}
	return B_OK;
}


// #pragma mark - backlight, panel reset


// R_PWM channel 0 on PL10 at 24 MHz / 1200 = 20 kHz, enable on PH10.
static void
backlight_enable(uint32 percent)
{
	static const uint32 kPeriod = 1199;
	static const uint32 kPwmGate = 1 << 6;
	static const uint32 kPwmEnable = 1 << 4;
	static const uint32 kPwmPrescaleOne = 0xf;

	pio_configure(kRPio, kPortL, 10, kPioFunction2);	// S_PWM
	clear32(kRPwm + 0x00, kPwmGate);
	write32(kRPwm + 0x04, (kPeriod << 16) | (kPeriod * percent / 100));
	write32(kRPwm + 0x00, kPwmGate | kPwmEnable | kPwmPrescaleOne);

	pio_configure(kPio, kPortH, 10, kPioOutput);
	pio_set(kPio, kPortH, 10, true);
}


static void
panel_reset(bool release)
{
	pio_configure(kPio, kPortD, 23, kPioOutput);
	pio_set(kPio, kPortD, 23, release);
}


// #pragma mark - TCON0


// PLL_VIDEO0 297 MHz -> PLL_MIPI 297 * 8 * 2 / 11 = 432 MHz -> TCON0, whose
// dot clock is a sixth of it (72 MHz); 432 Mbit/s on each of the 4 DSI lanes
// at 24 bits per pixel. TCON0 runs in the CPU (command) interface mode the
// DSI controller is triggered from.
static void
tcon0_init()
{
	write32(kPllVideo0, kPllEnable | (1 << 24) | (0x62 << 8) | 7);
	write32(kPllMipi, (1 << 23) | (1 << 22));	// LDO1, LDO2
	delay_ms(1);
	write32(kPllMipi, kPllEnable | (1 << 23) | (1 << 22) | (7 << 8)
		| (1 << 4) | 10);
	delay_ms(1);

	write32(kTcon0Clock, 1u << 31);			// on, from PLL_MIPI
	set32(kBusGate1, kBus1Tcon0);
	set32(kBusReset1, kBus1Tcon0);

	write32(kTcon0 + 0x000, 0);				// TCON_GCTL: off
	write32(kTcon0 + 0x004, 0);				// TCON_GINT0
	write32(kTcon0 + 0x008, 0);				// TCON_GINT1
	write32(kTcon0 + 0x08c, 0xffffffff);	// TCON0_IO_TRI: all tri-state
	write32(kTcon0 + 0x0f4, 0xffffffff);	// TCON1_IO_TRI

	write32(kTcon0 + 0x044, (8 << 28) | 6);	// TCON0_DCLK: on, / 6
	write32(kTcon0 + 0x040, (1u << 31) | (1 << 24));
		// TCON0_CTL: on, CPU interface, from DE
	write32(kTcon0 + 0x048,
		((kPanelWidth - 1) << 16) | (kPanelHeight - 1));	// TCON0_BASIC0
	write32(kTcon0 + 0x0f8, 0x8);			// TCON0_ECC_FIFO
	write32(kTcon0 + 0x060,
		(1 << 28) | (1 << 16) | (1 << 2) | (1 << 0));
		// TCON0_CPU_IF: CPU mode, flush mode, trigger FIFO, trigger
	write32(kTcon0 + 0x160, (47 << 16) | (kPanelWidth - 1));	// CPU_TRI0
	write32(kTcon0 + 0x164, kPanelHeight - 1);					// CPU_TRI1
	write32(kTcon0 + 0x168, (7106 << 16) | 10);					// CPU_TRI2
	write32(kTcon0 + 0x1f0, (3000 << 16) | 3);	// TCON_SAFE_PERIOD
	write32(kTcon0 + 0x08c, 0x7u << 29);	// TCON0_IO_TRI: outputs on

	set32(kTcon0 + 0x000, 1u << 31);		// TCON_GCTL: on
}


// #pragma mark - MIPI DSI


static const addr_t kDsiBasicControl0 = kDsi + 0x010;
static const addr_t kDsiInstructionFunction = kDsi + 0x020;
static const addr_t kDsiInstructionJumpSelect = kDsi + 0x048;
static const addr_t kDsiCommandControl = kDsi + 0x200;
static const addr_t kDsiCommandTx = kDsi + 0x300;
static const uint32 kDsiInstructionEnable = 1 << 0;
static const uint32 kDsiLaneClockEnable = 1 << 4;

enum {
	kDsiInstructionLp11 = 0,
	kDsiInstructionTba = 1,
	kDsiInstructionHsc = 2,
	kDsiInstructionHsd = 3,
	kDsiInstructionLpdt = 4,
	kDsiInstructionHscExit = 5,
	kDsiInstructionNop = 6,
	kDsiInstructionDelay = 7,
	kDsiInstructionEnd = 15
};

enum {
	kDcsShortWrite = 0x05,
	kDcsShortWriteParameter = 0x15,
	kDcsLongWrite = 0x39
};


// MIPI DSI packet header ECC: a Hamming code over the 24 header bits.
static uint8
dsi_ecc(uint32 header)
{
	static const uint32 kParity[6] = {
		0xf12cb7, 0xf2555b, 0x749a6d, 0xb8e38e, 0xdf03f0, 0xeffc00
	};
	uint8 ecc = 0;
	for (int bit = 0; bit < 6; bit++)
		ecc |= (__builtin_popcount(header & kParity[bit]) & 1) << bit;
	return ecc;
}


// MIPI DSI payload checksum: CRC-16/CCITT, LSB first, seeded with 0xffff.
static uint16
dsi_crc(const uint8* data, size_t length)
{
	uint16 crc = 0xffff;
	for (size_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++)
			crc = (crc & 1) != 0 ? (crc >> 1) ^ 0x8408 : crc >> 1;
	}
	return crc;
}


static status_t
dsi_wait_instruction()
{
	for (int i = 0; i < 50; i++) {
		if ((read32(kDsiBasicControl0) & kDsiInstructionEnable) == 0)
			return B_OK;
		spin(100);
	}
	return B_TIMED_OUT;
}


// Sends a DCS command on virtual channel 0 in low power mode.
static status_t
dsi_write_dcs(const uint8* command, size_t length)
{
	uint8 packet[128];
	size_t packetLength;
	uint8 type = length == 1 ? kDcsShortWrite
		: length == 2 ? kDcsShortWriteParameter : kDcsLongWrite;

	if (type == kDcsLongWrite) {
		if (length + 6 > sizeof(packet))
			return B_BAD_VALUE;
		uint32 header = type | (length << 8);
		packet[0] = type;
		packet[1] = length & 0xff;
		packet[2] = length >> 8;
		packet[3] = dsi_ecc(header);
		memcpy(packet + 4, command, length);
		uint16 crc = dsi_crc(command, length);
		packet[4 + length] = crc & 0xff;
		packet[5 + length] = crc >> 8;
		packetLength = length + 6;
	} else {
		uint8 parameter = length == 2 ? command[1] : 0;
		packet[0] = type;
		packet[1] = command[0];
		packet[2] = parameter;
		packet[3] = dsi_ecc(type | (command[0] << 8) | (parameter << 16));
		packetLength = 4;
	}

	write32(kDsiCommandControl, (1 << 26) | (1 << 25) | (1 << 9));
		// clear RX overflow, RX and TX flags
	for (size_t i = 0; i < packetLength; i += 4) {
		uint32 word = 0;
		for (size_t j = 0; j < 4 && i + j < packetLength; j++)
			word |= (uint32)packet[i + j] << (8 * j);
		write32(kDsiCommandTx + i, word);
	}
	modify32(kDsiCommandControl, 0xff, packetLength - 1);

	write32(kDsiInstructionJumpSelect,
		(kDsiInstructionLpdt << (4 * kDsiInstructionLp11))
		| (kDsiInstructionEnd << (4 * kDsiInstructionLpdt)));
	clear32(kDsiBasicControl0, kDsiInstructionEnable);
	set32(kDsiBasicControl0, kDsiInstructionEnable);
	status_t status = dsi_wait_instruction();
	if (status != B_OK)
		clear32(kDsiBasicControl0, kDsiInstructionEnable);
	return status;
}


// The controller's instruction engine and the video timings of the panel:
// 720x1440, vertical sync 10, back porch 17, total 1485 lines.
static void
dsi_enable()
{
	set32(kBusGate0, kBus0MipiDsi);
	set32(kBusReset0, kBus0MipiDsi);

	write32(kDsi + 0x000, 1);					// DSI_CTL: on
	write32(kDsiBasicControl0, (1 << 17) | (1 << 16));	// CRC, ECC
	write32(kDsi + 0x060, 10);					// DSI_TRANS_START
	write32(kDsi + 0x078, 0);					// DSI_TRANS_ZERO

	addr_t function = kDsiInstructionFunction;
	write32(function + 4 * kDsiInstructionLp11, 0x1f);
	write32(function + 4 * kDsiInstructionTba, 0x10000001);
	write32(function + 4 * kDsiInstructionHsc, 0x20000010);
	write32(function + 4 * kDsiInstructionHsd, 0x2000000f);
	write32(function + 4 * kDsiInstructionLpdt, 0x30100001);
	write32(function + 4 * kDsiInstructionHscExit, 0x40000010);
	write32(function + 4 * kDsiInstructionNop, 0xf);
	write32(function + 4 * kDsiInstructionDelay, 0x5000001f);
	write32(kDsi + 0x04c, 0x560001);			// DSI_INST_JUMP_CFG0
	write32(kDsi + 0x2f8, 0xff);				// DSI_DEBUG_DATA

	write32(kDsi + 0x014, (1468 << 4) | (1 << 2) | (1 << 1) | (1 << 0));
		// DSI_BASIC_CTL1: video start delay, precision align, frame start,
		// video mode
	write32(kDsi + 0x07c, 0x10000007);			// DSI_TCON_DRQ
	write32(kDsi + 0x040, 0x30000002);			// DSI_INST_LOOP_SEL
	write32(kDsi + 0x044, 0x310031);			// DSI_INST_LOOP_NUM0
	write32(kDsi + 0x054, 0x310031);			// DSI_INST_LOOP_NUM1

	write32(kDsi + 0x090, (19 << 24) | ((kPanelWidth * 3) << 8) | 0x3e);
		// DSI_PIXEL_PH: ECC, word count, VC 0, packed RGB888
	write32(kDsi + 0x098, 0xffff);				// DSI_PIXEL_PF0: CRC force
	write32(kDsi + 0x09c, 0xffffffff);			// DSI_PIXEL_PF1: CRC init
	write32(kDsi + 0x080, (1 << 16) | 8);		// DSI_PIXEL_CTL0: RGB888
	write32(kDsi + 0x00c, 0);					// DSI_BASIC_CTL

	write32(kDsi + 0x0b0, (0x12 << 24) | 0x21);	// DSI_SYNC_HSS
	write32(kDsi + 0x0b4, (0x01 << 24) | 0x31);	// DSI_SYNC_HSE
	write32(kDsi + 0x0b8, (0x07 << 24) | 0x01);	// DSI_SYNC_VSS
	write32(kDsi + 0x0bc, (0x14 << 24) | 0x11);	// DSI_SYNC_VSE

	write32(kDsi + 0x018, (17 << 16) | 10);		// DSI_BASIC_SIZE0: VBP, VSA
	write32(kDsi + 0x01c, (1485 << 16) | kPanelHeight);	// SIZE1: VT, VACT

	write32(kDsi + 0x0c0, 0x9004a19);			// DSI_BLK_HSA0
	write32(kDsi + 0x0c4, 0x50b4 << 16);		// DSI_BLK_HSA1
	write32(kDsi + 0x0c8, 0x35005419);			// DSI_BLK_HBP0
	write32(kDsi + 0x0cc, 0x757a << 16);		// DSI_BLK_HBP1
	write32(kDsi + 0x0d0, 0x9004a19);			// DSI_BLK_HFP0
	write32(kDsi + 0x0d4, 0x50b4 << 16);		// DSI_BLK_HFP1
	write32(kDsi + 0x0e0, 0xc091a19);			// DSI_BLK_HBLK0
	write32(kDsi + 0x0e4, 0x72bd << 16);		// DSI_BLK_HBLK1
	write32(kDsi + 0x0e8, 0x1a000019);			// DSI_BLK_VBLK0
	write32(kDsi + 0x0ec, 0xffffu << 16);		// DSI_BLK_VBLK1
}


// Switches the lanes to high speed and starts sending video.
static void
dsi_start()
{
	write32(kDsiInstructionJumpSelect, 0xf02);
	set32(kDsiBasicControl0, kDsiInstructionEnable);
	clear32(kDsiInstructionFunction + 4 * kDsiInstructionLp11,
		kDsiLaneClockEnable);
	delay_ms(1);
	write32(kDsiInstructionJumpSelect, 0x63f07006);
	set32(kDsiBasicControl0, kDsiInstructionEnable);
}


// #pragma mark - MIPI D-PHY


static void
dphy_enable()
{
	write32(kMipiDsiClock, (1 << 15) | (2 << 8) | 3);
		// on, from PLL_PERIPH0 (600 MHz) / 4

	write32(kDphy + 0x04, 0x10000000);			// DPHY_TX_CTL
	write32(kDphy + 0x10, 0xa06000e);			// DPHY_TX_TIME0
	write32(kDphy + 0x14, 0xa033207);			// DPHY_TX_TIME1
	write32(kDphy + 0x18, 0x1e);				// DPHY_TX_TIME2
	write32(kDphy + 0x1c, 0x0);					// DPHY_TX_TIME3
	write32(kDphy + 0x20, 0x303);				// DPHY_TX_TIME4
	write32(kDphy + 0x00, 0x31);				// DPHY_GCTL: on, 4 lanes

	write32(kDphy + 0x4c, 0x9f007f00);			// DPHY_ANA0
	write32(kDphy + 0x50, 0x17000000);			// DPHY_ANA1
	write32(kDphy + 0x5c, 0x1f01555);			// DPHY_ANA4
	write32(kDphy + 0x54, 0x2);					// DPHY_ANA2
	delay_ms(1);
	write32(kDphy + 0x58, 0x3040000);			// DPHY_ANA3
	delay_ms(1);
	set32(kDphy + 0x58, 0xf8000000);			// ANA3: enable VTTC
	delay_ms(1);
	set32(kDphy + 0x58, 0x4000000);				// ANA3: enable DIV
	delay_ms(1);
	set32(kDphy + 0x54, 0x10);					// ANA2: enable CK CPU
	delay_ms(1);
	set32(kDphy + 0x50, 0x80000000);			// ANA1: VTT mode
	set32(kDphy + 0x54, 0xf000000);				// ANA2: enable P2S CPU
}


// #pragma mark - ST7703 panel controller


// The XBD599 init sequence (same as Linux' panel-sitronix-st7703 for the
// "xingbangda,xbd599"): manufacturer commands, then sleep out, 120 ms,
// display on.
static const uint8 kSetExtc[] = { 0xb9, 0xf1, 0x12, 0x83 };
static const uint8 kSetMipi[] = {
	0xba, 0x33, 0x81, 0x05, 0xf9, 0x0e, 0x0e, 0x20, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x44, 0x25, 0x00, 0x91, 0x0a, 0x00, 0x00, 0x02, 0x4f,
	0x11, 0x00, 0x00, 0x37
};
static const uint8 kSetPowerExt[] = { 0xb8, 0x25, 0x22, 0x20, 0x03 };
static const uint8 kSetRgbIf[] = {
	0xb3, 0x10, 0x10, 0x05, 0x05, 0x03, 0xff, 0x00, 0x00, 0x00, 0x00
};
static const uint8 kSetScr[] = {
	0xc0, 0x73, 0x73, 0x50, 0x50, 0x00, 0xc0, 0x08, 0x70, 0x00
};
static const uint8 kSetVdc[] = { 0xbc, 0x4e };
static const uint8 kSetPanel[] = { 0xcc, 0x0b };
static const uint8 kSetCyc[] = { 0xb4, 0x80 };
static const uint8 kSetDisp[] = { 0xb2, 0xf0, 0x12, 0xf0 };
static const uint8 kSetEq[] = {
	0xe3, 0x00, 0x00, 0x0b, 0x0b, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0xff,
	0x00, 0xc0, 0x10
};
static const uint8 kSetC6[] = { 0xc6, 0x01, 0x00, 0xff, 0xff, 0x00 };
static const uint8 kSetPower[] = {
	0xc1, 0x74, 0x00, 0x32, 0x32, 0x77, 0xf1, 0xff, 0xff, 0xcc, 0xcc, 0x77,
	0x77
};
static const uint8 kSetBgp[] = { 0xb5, 0x07, 0x07 };
static const uint8 kSetVcom[] = { 0xb6, 0x2c, 0x2c };
static const uint8 kSetBf[] = { 0xbf, 0x02, 0x11, 0x00 };
static const uint8 kSetGip1[] = {
	0xe9, 0x82, 0x10, 0x06, 0x05, 0xa2, 0x0a, 0xa5, 0x12, 0x31, 0x23, 0x37,
	0x83, 0x04, 0xbc, 0x27, 0x38, 0x0c, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0c,
	0x00, 0x03, 0x00, 0x00, 0x00, 0x75, 0x75, 0x31, 0x88, 0x88, 0x88, 0x88,
	0x88, 0x88, 0x13, 0x88, 0x64, 0x64, 0x20, 0x88, 0x88, 0x88, 0x88, 0x88,
	0x88, 0x02, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00
};
static const uint8 kSetGip2[] = {
	0xea, 0x02, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x02, 0x46, 0x02, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x64, 0x88,
	0x13, 0x57, 0x13, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x75, 0x88, 0x23,
	0x14, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x0a, 0xa5, 0x00, 0x00,
	0x00, 0x00
};
static const uint8 kSetGamma[] = {
	0xe0, 0x00, 0x09, 0x0d, 0x23, 0x27, 0x3c, 0x41, 0x35, 0x07, 0x0d, 0x0e,
	0x12, 0x13, 0x10, 0x12, 0x12, 0x18, 0x00, 0x09, 0x0d, 0x23, 0x27, 0x3c,
	0x41, 0x35, 0x07, 0x0d, 0x0e, 0x12, 0x13, 0x10, 0x12, 0x12, 0x18
};
static const uint8 kSleepOut[] = { 0x11 };
static const uint8 kDisplayOn[] = { 0x29 };

struct panel_command {
	const uint8*	data;
	size_t			length;
	uint32			delayAfterMs;
};

#define PANEL_COMMAND(command, delay) { command, sizeof(command), delay }

static const panel_command kPanelInit[] = {
	PANEL_COMMAND(kSetExtc, 0),
	PANEL_COMMAND(kSetMipi, 0),
	PANEL_COMMAND(kSetPowerExt, 0),
	PANEL_COMMAND(kSetRgbIf, 0),
	PANEL_COMMAND(kSetScr, 0),
	PANEL_COMMAND(kSetVdc, 0),
	PANEL_COMMAND(kSetPanel, 0),
	PANEL_COMMAND(kSetCyc, 0),
	PANEL_COMMAND(kSetDisp, 0),
	PANEL_COMMAND(kSetEq, 0),
	PANEL_COMMAND(kSetC6, 0),
	PANEL_COMMAND(kSetPower, 0),
	PANEL_COMMAND(kSetBgp, 0),
	PANEL_COMMAND(kSetVcom, 0),
	PANEL_COMMAND(kSetBf, 0),
	PANEL_COMMAND(kSetGip1, 0),
	PANEL_COMMAND(kSetGip2, 0),
	PANEL_COMMAND(kSetGamma, 0),
	PANEL_COMMAND(kSleepOut, 120),
	PANEL_COMMAND(kDisplayOn, 0),
};


static status_t
panel_init()
{
	status_t result = B_OK;
	for (size_t i = 0; i < B_COUNT_OF(kPanelInit); i++) {
		status_t status = dsi_write_dcs(kPanelInit[i].data,
			kPanelInit[i].length);
		if (status != B_OK) {
			ERROR("panel command %#x failed: %" B_PRId32 "\n",
				kPanelInit[i].data[0], status);
			result = status;
		}
		if (kPanelInit[i].delayAfterMs != 0)
			delay_ms(kPanelInit[i].delayAfterMs);
	}
	return result;
}


// #pragma mark - Display Engine 2.0


static const addr_t kMixer0 = kDisplayEngine + 0x100000;
static const size_t kMixer0Length = 0x6000;
static const addr_t kGlobal = kMixer0 + 0x0000;
static const addr_t kBlender = kMixer0 + 0x1000;
static const addr_t kUiChannel1 = kMixer0 + 0x3000;


static inline addr_t
ui_channel(uint32 channel)
{
	return kUiChannel1 + (channel - 1) * 0x1000;
}


static inline addr_t
ui_scaler(uint32 channel)
{
	return kMixer0 + 0x40000 + (channel - 1) * 0x10000;
}


static status_t
de_init()
{
	clear32(kSystemControl + 0x4, 1 << 24);	// SRAM C to the DE

	write32(kPllDe, kPllEnable | (1 << 24) | (23 << 8) | 1);
		// 24 MHz * 24 / 2 = 288 MHz
	bool locked = false;
	for (int i = 0; i < 100 && !locked; i++) {
		locked = (read32(kPllDe) & kPllLock) != 0;
		if (!locked)
			delay_ms(1);
	}
	if (!locked)
		ERROR("PLL_DE does not lock\n");

	modify32(kDeClock, (1u << 31) | (0x7 << 24), (1u << 31) | (1 << 24));
		// on, from PLL_DE
	set32(kBusReset1, kBus1De);
	set32(kBusGate1, kBus1De);

	set32(kDisplayEngine + 0x000, 1 << 0);	// core 0 clock
	set32(kDisplayEngine + 0x008, 1 << 0);	// core 0 out of reset
	set32(kDisplayEngine + 0x004, 1 << 0);	// core 0 bus clock
	clear32(kDisplayEngine + 0x010, 1 << 0);	// mixer 0 to TCON0

	for (size_t offset = 0; offset < kMixer0Length; offset += 4)
		write32(kMixer0 + offset, 0);

	// Disable the post processing blocks.
	write32(kMixer0 + 0x020000, 0);			// video scaler
	write32(kDisplayEngine + 0x130000, 0);	// undocumented, as Linux does
	write32(ui_scaler(1), 0);
	write32(ui_scaler(2), 0);
	write32(kMixer0 + 0x0a0000, 0);			// FCE
	write32(kMixer0 + 0x0a2000, 0);			// BWS
	write32(kMixer0 + 0x0a4000, 0);			// LTI
	write32(kMixer0 + 0x0a6000, 0);			// peaking
	write32(kMixer0 + 0x0a8000, 0);			// ASE
	write32(kMixer0 + 0x0aa000, 0);			// FCC
	write32(kDisplayEngine + 0x1b0000, 0);	// DRC

	write32(kGlobal + 0x000, 1);			// GLB_CTL: mixer on
	return locked ? B_OK : B_ERROR;
}


// One XRGB8888 UI layer covering the panel, blended over opaque black.
static void
de_show(phys_addr_t frameBuffer)
{
	uint32 size = ((kPanelHeight - 1) << 16) | (kPanelWidth - 1);

	write32(kBlender + 0x088, 0xff000000);	// BLD_BK_COLOR: black
	write32(kBlender + 0x084, 0);			// BLD_PREMUL_CTL

	write32(ui_channel(1) + 0x00,
		(0xff << 24) | (4 << 8) | (2 << 1) | 1);
		// OVL_UI_ATTR_CTL: global alpha ff, XRGB8888, alpha mode 2, on
	write32(ui_channel(1) + 0x10, (uint32)frameBuffer);	// TOP_LADD
	write32(ui_channel(1) + 0x0c, kPanelWidth * 4);		// PITCH
	write32(ui_channel(1) + 0x04, size);				// MBSIZE
	write32(ui_channel(1) + 0x88, size);				// SIZE
	write32(ui_channel(1) + 0x08, 0);					// COOR
	write32(kBlender + 0x08c, size);					// BLD_SIZE
	write32(kGlobal + 0x00c, size);						// GLB_SIZE

	write32(kBlender + 0x008, size);			// BLD_CH_ISIZE(0)
	write32(kBlender + 0x004, 0xff000000);		// BLD_FILL_COLOR(0)
	write32(kBlender + 0x00c, 0);				// BLD_CH_OFFSET(0)
	write32(kBlender + 0x090, 0x03010301);		// BLD_CTL(0): source over
	write32(ui_scaler(1), 0);

	for (uint32 channel = 2; channel <= 3; channel++) {
		write32(ui_channel(channel) + 0x00, 0);
		write32(ui_scaler(channel), 0);
	}

	write32(kBlender + 0x080, 1);				// BLD_CH_RTCTL: pipe 0 = UI 1
	write32(kBlender + 0x000, (1 << 8) | 1);	// BLD_FILL_COLOR_CTL: pipe 0
	write32(kGlobal + 0x008, 1);				// GLB_DBUFFER: apply
}


// #pragma mark -


static bool
is_pinephone()
{
	const void* fdt = dtb_get_fdt();
	return fdt != NULL
		&& fdt_node_check_compatible(fdt, 0, "pine64,pinephone") == 0;
}


static status_t
allocate_frame_buffer()
{
	sFrameBufferSize = kPanelWidth * kPanelHeight * 4;
	size_t pages = (sFrameBufferSize + B_PAGE_SIZE - 1) / B_PAGE_SIZE;

	// Reserved memory stays out of the kernel's RAM, and the loader maps it
	// uncached for the kernel. The DE only takes 32 bit addresses.
	efi_physical_addr address = 0xffffffff;
	efi_status status = kBootServices->AllocatePages(AllocateMaxAddress,
		EfiReservedMemoryType, pages, &address);
	if (status != EFI_SUCCESS) {
		address = 0xffffffff;
		status = kBootServices->AllocatePages(AllocateMaxAddress,
			EfiRuntimeServicesData, pages, &address);
	}
	if (status != EFI_SUCCESS) {
		ERROR("no memory for the frame buffer\n");
		return B_NO_MEMORY;
	}

	sFrameBuffer = address;
	memset((void*)sFrameBuffer, 0, sFrameBufferSize);
	clean_dcache(sFrameBuffer, sFrameBufferSize);
	return B_OK;
}


}	// namespace


status_t
pinephone_display_init()
{
	if (!is_pinephone())
		return B_NOT_SUPPORTED;

	dprintf("pinephone: bringing up the panel\n");
	led_init();
	led_blink(kLedWhite, 3, 500);

	status_t status = allocate_frame_buffer();
	if (status != B_OK) {
		stage_failed(kStageTcon);
		return status;
	}

	backlight_enable(kBacklightPercent);
	tcon0_init();
	panel_reset(false);
	stage_done(kStageTcon);

	status = pmic_init();
	if (status != B_OK) {
		stage_failed(kStagePower);
		return status;
	}
	stage_done(kStagePower);
	delay_ms(15);

	dsi_enable();
	dphy_enable();
	stage_done(kStageDsi);

	panel_reset(true);
	delay_ms(15);
	if (panel_init() != B_OK)
		stage_failed(kStagePanel);
	else
		stage_done(kStagePanel);

	dsi_start();
	if (de_init() != B_OK)
		stage_failed(kStageVideo);
	else
		stage_done(kStageVideo);
	delay_ms(160);

	de_show(sFrameBuffer);
	stage_done(kStageScanout);
	sActive = true;

	gKernelArgs.frame_buffer.enabled = true;
	gKernelArgs.frame_buffer.physical_buffer.start = sFrameBuffer;
	gKernelArgs.frame_buffer.physical_buffer.size = sFrameBufferSize;
	gKernelArgs.frame_buffer.width = kPanelWidth;
	gKernelArgs.frame_buffer.height = kPanelHeight;
	gKernelArgs.frame_buffer.depth = 32;
	gKernelArgs.frame_buffer.bytes_per_row = kPanelWidth * 4;

	dprintf("pinephone: panel on, frame buffer at %#" B_PRIxADDR "\n",
		sFrameBuffer);
	return B_OK;
}


bool
pinephone_display_active()
{
	return sActive;
}


void
pinephone_display_flush()
{
	if (sActive)
		clean_dcache(sFrameBuffer, sFrameBufferSize);
}


void
pinephone_display_starting_kernel()
{
	// After a failed step the LED keeps showing which one it was.
	if (!sActive)
		return;

	pinephone_display_flush();
	led_set(kStageKernel);
}
