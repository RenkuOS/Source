/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


//!	Audio for Intel Bay Trail tablets with an RT5640 codec.
// On Bay Trail the audio does not go through HD Audio: the low power engine
// (LPE, ACPI 80860F28), a DSP with its own SSP (I2S) ports and DMA, feeds a
// codec on I2C. This driver attaches to that codec (Realtek RT5640, ACPI
// 10EC5640) and will drive the LPE as well.
//
// It brings up the codec (master clock through ACPI, reset, jack detection),
// boots Sound Open Firmware on the DSP (sof_dsp.cpp) and publishes the
// speakers as a multi_audio device: 48 kHz, 16 bit stereo, two buffers of
// 1024 frames, played through a fixed pipeline from memory to SSP0 and the
// codec. Headphones, volume controls and recording are not done yet.


#include <ACPI.h>
#include <KernelExport.h>
#include <device_manager.h>
#include <i2c.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>

#include <hmulti_audio.h>

#include <FindDirectory.h>

#include <new>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sof_dsp.h"


#define DRIVER_NAME		"intel_lpe"
#define INTEL_LPE_DRIVER_MODULE_NAME "drivers/audio/hmulti/intel_lpe/driver_v1"
#define INTEL_LPE_DEVICE_MODULE_NAME "drivers/audio/hmulti/intel_lpe/device_v1"
#define INTEL_LPE_DEVICE_PATH "audio/hmulti/intel_lpe/1"

#define TRACE_ALWAYS(x...)	dprintf(DRIVER_NAME ": " x)


// RT5640 registers (ALC5640 datasheet, chapter 8); 8 bit register numbers,
// 16 bit big endian values.
#define RT5640_RESET				0x00
#define RT5640_IN1_IN2				0x0d
#define RT5640_PWR_DIG1				0x61
#define RT5640_PWR_ANLG1			0x63
#define RT5640_PWR_ANLG2			0x64
#define RT5640_JD_CTRL				0xbb
#define RT5640_IRQ_CTRL1			0xbd
#define RT5640_IRQ_CTRL2			0xbe
#define RT5640_INT_IRQ_ST			0xbf
#define RT5640_GPIO_CTRL1			0xc0
#define RT5640_GEN_CTRL1			0xfa
#define RT5640_GEN_CTRL2			0xfb
#define RT5640_VENDOR_ID			0xfe
#define RT5640_DEVICE_ID			0xff

#define RT5640_VENDOR_REALTEK		0x10ec

// MX-BF: status of the selected jack detect source
#define RT5640_STATUS_JD			(1 << 4)

#define JACK_POLL_INTERVAL			250000

// The firmware is read from the boot volume, which may not be mounted yet
// when the driver starts.
#define FIRMWARE_NAME				"sof-byt.ri"
#define FIRMWARE_WAIT				(120 * 1000000LL)
#define FIRMWARE_MAX_SIZE			(1024 * 1024)

// how long the first playback waits for the DSP to boot
#define DSP_READY_WAIT				(15 * 1000000LL)

// The playback ring: two buffers of 1024 frames, one page each.
#define PLAYBACK_RATE				48000
#define PLAYBACK_CHANNELS			2
#define PLAYBACK_SAMPLE_SIZE		2
#define PLAYBACK_FRAME_SIZE			(PLAYBACK_CHANNELS * PLAYBACK_SAMPLE_SIZE)
#define PLAYBACK_BUFFERS			2
#define PLAYBACK_BUFFER_FRAMES		1024
#define PLAYBACK_BUFFER_SIZE		(PLAYBACK_BUFFER_FRAMES * PLAYBACK_FRAME_SIZE)
#define PLAYBACK_RING_SIZE			(PLAYBACK_BUFFERS * PLAYBACK_BUFFER_SIZE)
#define PLAYBACK_BUFFER_TIME \
	((bigtime_t)PLAYBACK_BUFFER_FRAMES * 1000000 / PLAYBACK_RATE)

// How the playback thread waits for the next buffer: it sleeps most of a
// buffer after a change, then checks every POLL_INTERVAL.
#define POLL_INTERVAL				500
#define POLL_EARLY_WAKEUP			3000
#define EXCHANGE_TIMEOUT			500000


struct intel_lpe_driver {
	device_node*			node;
	acpi_handle				codecHandle;

	i2c_device_interface*	i2c;
	i2c_device				i2cCookie;

	thread_id				pollThread;
	volatile bool			stopPolling;

	// Protects the DSP mailboxes and doorbells.
	mutex					dspLock;
	lpe_dsp					dsp;
	bool					dspInitialized;

	area_id					ringArea;
	uint8*					ring;
	area_id					tableArea;
	phys_addr_t				tablePhysical;

	// Protects the fields below and the start and stop of playback.
	mutex					lock;
	int32					openCount;
	int32					running;
	thread_id				playbackThread;
	sem_id					bufferReadySem;

	// Playback position, written by the playback thread.
	spinlock				positionLock;
	uint32					bufferCycle;
	int64					framesCount;
	bigtime_t				realTime;
};


static device_manager_info* sDeviceManager;


//	#pragma mark - RT5640


static status_t
rt5640_read(intel_lpe_driver* driver, uint8 reg, uint16* _value)
{
	uint8 data[2];
	status_t status = driver->i2c->acquire_bus(driver->i2cCookie);
	if (status != B_OK)
		return status;
	status = driver->i2c->exec_command(driver->i2cCookie, I2C_OP_READ_STOP,
		&reg, 1, data, sizeof(data));
	driver->i2c->release_bus(driver->i2cCookie);
	if (status == B_OK)
		*_value = (data[0] << 8) | data[1];
	return status;
}


static status_t
rt5640_write(intel_lpe_driver* driver, uint8 reg, uint16 value)
{
	uint8 command[3] = { reg, (uint8)(value >> 8), (uint8)value };
	status_t status = driver->i2c->acquire_bus(driver->i2cCookie);
	if (status != B_OK)
		return status;
	status = driver->i2c->exec_command(driver->i2cCookie, I2C_OP_WRITE_STOP,
		command, sizeof(command), NULL, 0);
	driver->i2c->release_bus(driver->i2cCookie);
	return status;
}


/*!	Switches on the master clock of the codec. Its ACPI device does that in
	_PS0: it forces platform clock 3 of the PMC on, which feeds the MCLK pin.
*/
static void
rt5640_power_on(intel_lpe_driver* driver)
{
	if (driver->codecHandle == NULL)
		return;

	acpi_module_info* acpi;
	if (get_module(B_ACPI_MODULE_NAME, (module_info**)&acpi) != B_OK)
		return;

	status_t status = acpi->evaluate_method(driver->codecHandle, "_PS0", NULL,
		NULL);
	if (status != B_OK)
		TRACE_ALWAYS("_PS0 of the codec failed: %s\n", strerror(status));
	put_module(B_ACPI_MODULE_NAME);
	snooze(10000);
}


static status_t
rt5640_probe(intel_lpe_driver* driver)
{
	rt5640_power_on(driver);

	uint16 vendor, device;
	status_t status = rt5640_read(driver, RT5640_VENDOR_ID, &vendor);
	if (status == B_OK)
		status = rt5640_read(driver, RT5640_DEVICE_ID, &device);
	if (status != B_OK) {
		TRACE_ALWAYS("codec does not answer: %s\n", strerror(status));
		return status;
	}
	if (vendor != RT5640_VENDOR_REALTEK) {
		TRACE_ALWAYS("unexpected codec vendor %#04x\n", vendor);
		return B_DEVICE_NOT_FOUND;
	}
	TRACE_ALWAYS("RT5640 codec, device id %#04x\n", device);

	// any write to MX-00 resets the codec to its defaults
	status = rt5640_write(driver, RT5640_RESET, 0);
	if (status != B_OK)
		return status;
	snooze(10000);

	// Jack detection as the Android driver of the tablet left the codec
	// (register dump taken while playing to the speakers): JD1 (the IN1N
	// pin, IN1 single ended) is the source, and the analog references and
	// biases that the jack detect comparator needs are on.
	// Android also routes the jack state to the IRQ output (MX-BD, MX-C0);
	// that line ends at a GPIO interrupt of the SoC that nothing handles yet,
	// so it stays off and the driver polls MX-BF instead.
	static const struct {
		uint8	reg;
		uint16	value;
	} kJackDetectSetup[] = {
		{ RT5640_PWR_ANLG1,		0xe818 },
		{ RT5640_GEN_CTRL1,		0x3e71 },
		{ RT5640_GEN_CTRL2,		0x4040 },
		{ RT5640_JD_CTRL,		0x4003 },
	};
	for (size_t i = 0; i < B_COUNT_OF(kJackDetectSetup); i++) {
		rt5640_write(driver, kJackDetectSetup[i].reg,
			kJackDetectSetup[i].value);
	}

	static const uint8 kRegisters[] = {
		RT5640_IN1_IN2, RT5640_PWR_DIG1, RT5640_PWR_ANLG1, RT5640_PWR_ANLG2,
		RT5640_JD_CTRL, RT5640_IRQ_CTRL1, RT5640_IRQ_CTRL2, RT5640_INT_IRQ_ST,
		RT5640_GPIO_CTRL1, RT5640_GEN_CTRL1, RT5640_GEN_CTRL2
	};
	for (size_t i = 0; i < B_COUNT_OF(kRegisters); i++) {
		uint16 value = 0;
		rt5640_read(driver, kRegisters[i], &value);
		TRACE_ALWAYS("MX-%02X = %04x\n", kRegisters[i], value);
	}
	return B_OK;
}


static void start_dsp(intel_lpe_driver* driver);


static status_t
jack_poll_thread(void* data)
{
	intel_lpe_driver* driver = (intel_lpe_driver*)data;
	uint16 last = 0xffff;
	uint16 lastJD2 = 0xffff;

	start_dsp(driver);

	while (!driver->stopPolling) {
		if (driver->dspInitialized) {
			MutexLocker dspLocker(driver->dspLock);
			lpe_dsp_poll_messages(&driver->dsp);
		}

		uint16 status, gen2;
		if (rt5640_read(driver, RT5640_INT_IRQ_ST, &status) == B_OK
			&& rt5640_read(driver, RT5640_GEN_CTRL2, &gen2) == B_OK) {
			if (status != last || (gen2 & (1 << 11)) != lastJD2) {
				TRACE_ALWAYS("jack detect: JD1 %s (MX-BF %04x, MX-FB %04x)\n",
					(status & RT5640_STATUS_JD) != 0 ? "high" : "low",
					status, gen2);
				last = status;
				lastJD2 = gen2 & (1 << 11);
			}
		}
		snooze(JACK_POLL_INTERVAL);
	}
	return B_OK;
}


//	#pragma mark - DSP


/*!	Reads the SOF firmware from data/firmware/intel/sof, where Linux keeps it
	as well.
*/
static status_t
read_firmware(uint8** _data, size_t* _size)
{
	static const directory_which kDirectories[] = {
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY,
	};

	int fd = -1;
	char path[B_PATH_NAME_LENGTH];
	for (size_t i = 0; i < B_COUNT_OF(kDirectories) && fd < 0; i++) {
		char base[B_PATH_NAME_LENGTH];
		if (find_directory(kDirectories[i], -1, false, base, sizeof(base))
				!= B_OK) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/firmware/intel/sof/%s", base,
			FIRMWARE_NAME);
		fd = open(path, O_RDONLY);
	}
	if (fd < 0)
		return B_ENTRY_NOT_FOUND;

	off_t size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	if (size <= 0 || size > FIRMWARE_MAX_SIZE) {
		close(fd);
		return B_BAD_DATA;
	}

	uint8* data = (uint8*)malloc(size);
	if (data == NULL) {
		close(fd);
		return B_NO_MEMORY;
	}
	ssize_t bytesRead = read(fd, data, size);
	close(fd);
	if (bytesRead != size) {
		free(data);
		return B_IO_ERROR;
	}

	TRACE_ALWAYS("loaded %s (%" B_PRIdOFF " bytes)\n", path, size);
	*_data = data;
	*_size = size;
	return B_OK;
}


static void
start_dsp(intel_lpe_driver* driver)
{
	uint8* image = NULL;
	size_t size = 0;
	status_t status;
	bigtime_t timeout = system_time() + FIRMWARE_WAIT;
	while ((status = read_firmware(&image, &size)) == B_ENTRY_NOT_FOUND
		&& system_time() < timeout && !driver->stopPolling) {
		snooze(1000000);
	}
	if (status != B_OK) {
		TRACE_ALWAYS("no SOF firmware (%s): %s\n", FIRMWARE_NAME,
			strerror(status));
		return;
	}

	MutexLocker dspLocker(driver->dspLock);
	status = lpe_dsp_init(&driver->dsp);
	if (status == B_OK) {
		driver->dspInitialized = true;
		status = lpe_dsp_boot(&driver->dsp, image, size);
	}
	dspLocker.Unlock();
	free(image);

	if (status != B_OK)
		TRACE_ALWAYS("DSP did not boot: %s\n", strerror(status));
}


//	#pragma mark - codec playback setup


struct rt5640_register_value {
	uint8	reg;
	uint16	value;
};

// The codec as Android set it up while it played through the speakers
// (register dump of the stock Android on this tablet), in a safe order:
// PLL, then clocks and interfaces, mixers, power, and the speaker last.
// AIF1 is an I2S consumer with 24 bit data; the system clock comes from
// the PLL (MCLK 25 MHz * 59 / 60 = 24.576 MHz = 512 * 48 kHz).
static const rt5640_register_value kSpeakerPlayback[] = {
	{ 0x81, 0x1c82 },	// PLL: N = 57, K = 2
	{ 0x82, 0xd000 },	// PLL: M = 13
	{ 0x64, 0x0200 },	// power: PLL
	{ 0x80, 0x4000 },	// system clock from the PLL, PLL from MCLK
	{ 0x83, 0x9a00 },	// ASRC
	{ 0x84, 0xf820 },	// ASRC for the filters, the DAC stereo filter too:
						// SSP0 runs from 19.2 MHz, MCLK is 25 MHz
	{ 0x85, 0x0008 },
	{ 0x89, 0x0000 },
	{ 0x8a, 0x0000 },
	{ 0x70, 0x8000 },	// I2S1: consumer, 16 bit, I2S (the 16 most
						// significant bits of the 25 bit slots)
	{ 0x71, 0x8000 },
	{ 0x72, 0x8000 },
	{ 0x73, 0x0014 },
	{ 0x74, 0x0c00 },
	{ 0x75, 0x1d00 },
	{ 0x02, 0xa7a7 },	// headphone volume, muted
	{ 0x03, 0xc8c8 },
	{ 0x04, 0x8000 },
	{ 0x05, 0x0000 },
	{ 0x0d, 0x0540 },
	{ 0x0e, 0x0500 },
	{ 0x0f, 0x0808 },
	{ 0x19, 0xafaf },	// DAC1 digital volume, 0 dB
	{ 0x1a, 0xaaaa },
	{ 0x1b, 0x0000 },
	{ 0x1c, 0x8080 },
	{ 0x1d, 0x2f2f },
	{ 0x1e, 0x0000 },
	{ 0x27, 0x7060 },
	{ 0x28, 0x7070 },
	{ 0x29, 0x8080 },	// DAC mixer: interface 1 to DAC1
	{ 0x2a, 0x1414 },	// stereo DAC mixer: DAC L1/R1
	{ 0x2b, 0x5454 },
	{ 0x2c, 0xaa00 },
	{ 0x2d, 0x0000 },
	{ 0x2e, 0x0c00 },
	{ 0x2f, 0x0100 },
	{ 0x3b, 0x0000 },
	{ 0x3c, 0x007b },
	{ 0x3d, 0x0000 },
	{ 0x3e, 0x007b },
	{ 0x45, 0xc000 },
	{ 0x46, 0x0036 },	// speaker mixer: DAC L1
	{ 0x47, 0x0036 },	// speaker mixer: DAC R1
	{ 0x48, 0xe800 },
	{ 0x49, 0x2800 },
	{ 0x4a, 0x0004 },
	{ 0x4c, 0xfc00 },
	{ 0x4d, 0x0000 },
	{ 0x4e, 0x0000 },
	{ 0x4f, 0x01fe },
	{ 0x50, 0x0000 },
	{ 0x51, 0x0000 },
	{ 0x52, 0x01fe },
	{ 0x53, 0xf000 },
	{ 0x8b, 0x0600 },
	{ 0x8c, 0x0334 },
	{ 0x8d, 0x3000 },	// class D: ratio 2x
	{ 0x8e, 0x0009 },
	{ 0x8f, 0x3100 },
	{ 0x90, 0x0646 },
	{ 0x91, 0x0f00 },
	{ 0x92, 0x0000 },
	{ 0x93, 0x3410 },
	{ 0xb0, 0x2080 },
	{ 0xb1, 0x0000 },
	{ 0xb2, 0x0000 },
	{ 0xb4, 0x2206 },
	{ 0xb5, 0x1f00 },
	{ 0xb6, 0x0000 },
	{ 0xb7, 0x0000 },
	{ 0xb8, 0x034b },
	{ 0xb9, 0x0066 },
	{ 0xba, 0x000b },
	{ 0xd0, 0x0680 },
	{ 0xd1, 0x1c17 },
	{ 0xd2, 0x8c00 },
	{ 0xd3, 0xaa20 },
	{ 0xd6, 0x0400 },
	{ 0xd7, 0x0000 },
	{ 0xd9, 0x0809 },
	{ 0xda, 0x0000 },
	{ 0x61, 0x9801 },	// power: I2S1, DAC L1/R1
	{ 0x62, 0x0000 },
	{ 0x65, 0x3000 },	// power: speaker mixers
	{ 0x66, 0xc000 },	// power: speaker volumes
	{ 0x01, 0x0808 },	// speaker volume, unmuted
};

// private registers (index MX-6A, data MX-6C) that the Realtek drivers set
// at initialization
static const rt5640_register_value kPrivateInit[] = {
	{ 0x3d, 0x3600 },
	{ 0x12, 0x0aa8 },
	{ 0x14, 0x0aaa },
	{ 0x21, 0xe0e0 },
	{ 0x23, 0x1804 },
};

#define RT5640_PRIVATE_INDEX		0x6a
#define RT5640_PRIVATE_DATA			0x6c

#define RT5640_SPEAKER_VOLUME		0x01
#define RT5640_SPEAKER_MUTED		0x8888

// SUS GPIO 21 (pad 2): Android drives it low as
// "byt_ffrd8_tristate_buffer_gpio"; the firmware leaves it high.
#define SUS_BASE					0xfed0e000
#define SUS_PAD_21					2


static void
set_tristate_buffer_gpio()
{
	uint8* sus;
	area_id area = map_physical_memory("intel_lpe SUS pads", SUS_BASE,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&sus);
	if (area < 0)
		return;
	volatile uint32* registers = (volatile uint32*)(sus + SUS_PAD_21 * 16);
	// GPIO function, output enabled (VAL bit 1 clear), level low (bit 0)
	registers[0] = (registers[0] & ~7) | 1;
	registers[2] = (registers[2] & ~3) | 4;
	delete_area(area);
}


static status_t
setup_speaker_playback(intel_lpe_driver* driver)
{
	for (size_t i = 0; i < B_COUNT_OF(kPrivateInit); i++) {
		rt5640_write(driver, RT5640_PRIVATE_INDEX, kPrivateInit[i].reg);
		rt5640_write(driver, RT5640_PRIVATE_DATA, kPrivateInit[i].value);
	}

	for (size_t i = 0; i < B_COUNT_OF(kSpeakerPlayback); i++) {
		status_t status = rt5640_write(driver, kSpeakerPlayback[i].reg,
			kSpeakerPlayback[i].value);
		if (status != B_OK) {
			TRACE_ALWAYS("writing MX-%02X failed: %s\n",
				kSpeakerPlayback[i].reg, strerror(status));
			return status;
		}
		if (kSpeakerPlayback[i].reg == 0x64)
			snooze(1000);
	}
	return B_OK;
}


//	#pragma mark - playback


/*!	Follows the host position, which the firmware writes into the stream
	mailbox every millisecond. Each change of buffer means a buffer finished
	playing; buffer_exchange() then hands that buffer to the media add-on.
*/
static int32
playback_thread(void* data)
{
	intel_lpe_driver* driver = (intel_lpe_driver*)data;
	uint32 last = 0;

	while (atomic_get(&driver->running) != 0) {
		sof_ipc_stream_posn position;
		if (lpe_dsp_read_position(&driver->dsp, &position) != B_OK) {
			snooze(POLL_INTERVAL);
			continue;
		}
		uint32 current = (uint32)(position.host_posn / PLAYBACK_BUFFER_SIZE)
			% PLAYBACK_BUFFERS;
		if (current == last) {
			snooze(POLL_INTERVAL);
			continue;
		}
		last = current;
		bigtime_t now = system_time();

		cpu_status state = disable_interrupts();
		acquire_spinlock(&driver->positionLock);
		driver->bufferCycle = current;
		driver->framesCount += PLAYBACK_BUFFER_FRAMES;
		driver->realTime = now;
		release_spinlock(&driver->positionLock);
		restore_interrupts(state);

		release_sem_etc(driver->bufferReadySem, 1, B_DO_NOT_RESCHEDULE);
		snooze(PLAYBACK_BUFFER_TIME - POLL_EARLY_WAKEUP);
	}
	return 0;
}


static status_t
playback_start(intel_lpe_driver* driver)
{
	// the DSP boots once the firmware can be read from the boot volume
	bigtime_t timeout = system_time() + DSP_READY_WAIT;
	while (!driver->dsp.ready) {
		if (system_time() > timeout)
			return B_DEV_NOT_READY;
		snooze(100000);
	}

	int32 count;
	if (get_sem_count(driver->bufferReadySem, &count) == B_OK && count > 0)
		acquire_sem_etc(driver->bufferReadySem, count, B_RELATIVE_TIMEOUT, 0);

	status_t status = setup_speaker_playback(driver);
	if (status != B_OK)
		return status;

	driver->bufferCycle = 0;
	driver->framesCount = 0;
	driver->realTime = system_time();

	lpe_stream_buffer buffer;
	buffer.pageTable = driver->tablePhysical;
	buffer.pageCount = PLAYBACK_RING_SIZE / B_PAGE_SIZE;
	buffer.size = PLAYBACK_RING_SIZE;
	buffer.periodBytes = PLAYBACK_BUFFER_SIZE;
	{
		MutexLocker dspLocker(driver->dspLock);
		status = lpe_dsp_start_playback(&driver->dsp, &buffer);
	}
	if (status != B_OK) {
		rt5640_write(driver, RT5640_SPEAKER_VOLUME, RT5640_SPEAKER_MUTED);
		return status;
	}

	driver->running = 1;
	driver->playbackThread = spawn_kernel_thread(playback_thread,
		"intel_lpe playback", B_REAL_TIME_DISPLAY_PRIORITY, driver);
	if (driver->playbackThread < 0) {
		status = driver->playbackThread;
		driver->running = 0;
		MutexLocker dspLocker(driver->dspLock);
		lpe_dsp_stop_playback(&driver->dsp);
		rt5640_write(driver, RT5640_SPEAKER_VOLUME, RT5640_SPEAKER_MUTED);
		return status;
	}
	resume_thread(driver->playbackThread);
	return B_OK;
}


static void
playback_stop(intel_lpe_driver* driver)
{
	if (!driver->running)
		return;

	atomic_set(&driver->running, 0);
	status_t result;
	wait_for_thread(driver->playbackThread, &result);
	driver->playbackThread = -1;

	rt5640_write(driver, RT5640_SPEAKER_VOLUME, RT5640_SPEAKER_MUTED);
	{
		MutexLocker dspLocker(driver->dspLock);
		lpe_dsp_stop_playback(&driver->dsp);
	}

	// wake a buffer_exchange() that is still waiting
	release_sem_etc(driver->bufferReadySem, 1, 0);
	TRACE_ALWAYS("playback stopped after %" B_PRId64 " frames\n",
		driver->framesCount);
}


//	#pragma mark - multi_audio


static multi_channel_info sChannels[] = {
	{ 0, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0 },
	{ 1, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0 },
	{ 2, B_MULTI_OUTPUT_BUS, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_ANALOG_HEADER },
	{ 3, B_MULTI_OUTPUT_BUS, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_ANALOG_HEADER },
};


static status_t
get_description(intel_lpe_driver* driver, multi_description* data)
{
	data->interface_version = B_CURRENT_INTERFACE_VERSION;
	data->interface_minimum = B_CURRENT_INTERFACE_VERSION;

	strlcpy(data->friendly_name, "Speakers (Intel LPE)",
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
get_enabled_channels(intel_lpe_driver* driver, multi_channel_enable* data)
{
	data->lock_source = B_MULTI_LOCK_INTERNAL;
	for (int32 i = 0; i < PLAYBACK_CHANNELS; i++)
		B_SET_CHANNEL(data->enable_bits, i, true);
	return B_OK;
}


static status_t
get_global_format(intel_lpe_driver* driver, multi_format_info* data)
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
set_global_format(intel_lpe_driver* driver, multi_format_info* data)
{
	// there is only one format; report it back
	return get_global_format(driver, data);
}


static status_t
list_mix_controls(intel_lpe_driver* driver, multi_mix_control_info* data)
{
	// no gain controls yet; the system mixer sets the volume
	data->control_count = 0;
	return B_OK;
}


static status_t
list_mix_connections(intel_lpe_driver* driver,
	multi_mix_connection_info* data)
{
	return B_ERROR;
}


static status_t
list_mix_channels(intel_lpe_driver* driver, multi_mix_channel_info* data)
{
	return B_ERROR;
}


static status_t
get_mix(intel_lpe_driver* driver, multi_mix_value_info* data)
{
	return B_OK;
}


static status_t
set_mix(intel_lpe_driver* driver, multi_mix_value_info* data)
{
	return B_OK;
}


/*!	The ring layout is fixed, so the requested buffer count and size are
	ignored.
*/
static status_t
get_buffers(intel_lpe_driver* driver, multi_buffer_list* data)
{
	MutexLocker locker(driver->lock);

	if (!driver->running)
		memset(driver->ring, 0, PLAYBACK_RING_SIZE);

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
			descs[channel].base = (char*)driver->ring
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
buffer_exchange(intel_lpe_driver* driver, multi_buffer_info* data)
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
			if (status != B_OK) {
				TRACE_ALWAYS("playback did not start: %s\n",
					strerror(status));
				return status;
			}
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
buffer_force_stop(intel_lpe_driver* driver)
{
	MutexLocker locker(driver->lock);
	playback_stop(driver);
	return B_OK;
}


#define cookie_type intel_lpe_driver
#include "../generic/multi.c"


//	#pragma mark - setup


/*!	The ring the media add-on writes into, and the page table through
	which the DSP reads it.
*/
static status_t
create_ring(intel_lpe_driver* driver)
{
	void* address;
	driver->ringArea = create_area("intel_lpe playback", &address,
		B_ANY_KERNEL_ADDRESS, PLAYBACK_RING_SIZE, B_32_BIT_CONTIGUOUS,
		B_READ_AREA | B_WRITE_AREA);
	if (driver->ringArea < 0)
		return driver->ringArea;
	driver->ring = (uint8*)address;
	memset(driver->ring, 0, PLAYBACK_RING_SIZE);

	driver->tableArea = create_area("intel_lpe page table", &address,
		B_ANY_KERNEL_ADDRESS, B_PAGE_SIZE, B_32_BIT_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (driver->tableArea < 0)
		return driver->tableArea;
	memset(address, 0, B_PAGE_SIZE);

	physical_entry entry;
	status_t status = get_memory_map(address, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK)
		return status;
	driver->tablePhysical = entry.address;

	phys_addr_t pages[PLAYBACK_RING_SIZE / B_PAGE_SIZE];
	status = get_memory_map(driver->ring, PLAYBACK_RING_SIZE, &entry, 1);
	if (status != B_OK)
		return status;
	for (size_t i = 0; i < B_COUNT_OF(pages); i++)
		pages[i] = entry.address + i * B_PAGE_SIZE;
	lpe_fill_page_table((uint8*)address, pages, B_COUNT_OF(pages));
	return B_OK;
}


static void
delete_ring(intel_lpe_driver* driver)
{
	if (driver->tableArea >= 0)
		delete_area(driver->tableArea);
	if (driver->ringArea >= 0)
		delete_area(driver->ringArea);
}


//	#pragma mark - device module API


static status_t
intel_lpe_init_device(void* driverCookie, void** _deviceCookie)
{
	*_deviceCookie = driverCookie;
	return B_OK;
}


static void
intel_lpe_uninit_device(void* deviceCookie)
{
}


static status_t
intel_lpe_open(void* deviceCookie, const char* path, int openMode,
	void** _cookie)
{
	intel_lpe_driver* driver = (intel_lpe_driver*)deviceCookie;
	MutexLocker locker(driver->lock);
	driver->openCount++;
	*_cookie = driver;
	return B_OK;
}


static status_t
intel_lpe_close(void* cookie)
{
	return B_OK;
}


static status_t
intel_lpe_free(void* cookie)
{
	intel_lpe_driver* driver = (intel_lpe_driver*)cookie;
	MutexLocker locker(driver->lock);
	if (--driver->openCount == 0)
		playback_stop(driver);
	return B_OK;
}


static status_t
intel_lpe_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
intel_lpe_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
intel_lpe_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	return multi_audio_control_generic((intel_lpe_driver*)cookie, op, buffer,
		length);
}


//	#pragma mark - driver module API


static float
intel_lpe_supports_device(device_node* parent)
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
		|| strcmp(hid, "10EC5640") != 0) {
		return 0.0;
	}

	return 0.6;
}


static status_t
intel_lpe_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Intel LPE audio (RT5640)" }},
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, INTEL_LPE_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
intel_lpe_init_driver(device_node* node, void** _driverCookie)
{
	intel_lpe_driver* driver = new(std::nothrow) intel_lpe_driver;
	if (driver == NULL)
		return B_NO_MEMORY;
	memset(driver, 0, sizeof(*driver));
	driver->node = node;
	driver->pollThread = -1;
	driver->playbackThread = -1;
	driver->ringArea = driver->tableArea = -1;
	mutex_init(&driver->lock, "intel_lpe");
	mutex_init(&driver->dspLock, "intel_lpe dsp");
	B_INITIALIZE_SPINLOCK(&driver->positionLock);

	device_node* parent = sDeviceManager->get_parent_node(node);
	uint64 handle = 0;
	if (sDeviceManager->get_attr_uint64(parent, ACPI_DEVICE_HANDLE_ITEM,
			&handle, false) == B_OK) {
		driver->codecHandle = (acpi_handle)(addr_t)handle;
	}
	status_t status = sDeviceManager->get_driver(parent,
		(driver_module_info**)&driver->i2c, (void**)&driver->i2cCookie);
	sDeviceManager->put_node(parent);

	if (status == B_OK)
		status = rt5640_probe(driver);
	if (status == B_OK)
		status = create_ring(driver);
	if (status == B_OK) {
		driver->bufferReadySem = create_sem(0, "intel_lpe buffer ready");
		if (driver->bufferReadySem < 0)
			status = driver->bufferReadySem;
	}
	if (status != B_OK) {
		TRACE_ALWAYS("probe failed: %s\n", strerror(status));
		delete_ring(driver);
		mutex_destroy(&driver->dspLock);
		mutex_destroy(&driver->lock);
		delete driver;
		return status;
	}

	set_tristate_buffer_gpio();

	driver->pollThread = spawn_kernel_thread(jack_poll_thread,
		"intel_lpe poll", B_LOW_PRIORITY, driver);
	if (driver->pollThread >= 0)
		resume_thread(driver->pollThread);

	*_driverCookie = driver;
	return B_OK;
}


static void
intel_lpe_uninit_driver(void* driverCookie)
{
	intel_lpe_driver* driver = (intel_lpe_driver*)driverCookie;
	mutex_lock(&driver->lock);
	playback_stop(driver);
	mutex_unlock(&driver->lock);

	if (driver->pollThread >= 0) {
		driver->stopPolling = true;
		status_t result;
		wait_for_thread(driver->pollThread, &result);
	}
	if (driver->dspInitialized)
		lpe_dsp_uninit(&driver->dsp);
	delete_sem(driver->bufferReadySem);
	delete_ring(driver);
	mutex_destroy(&driver->dspLock);
	mutex_destroy(&driver->lock);
	delete driver;
}


static status_t
intel_lpe_register_child_devices(void* driverCookie)
{
	intel_lpe_driver* driver = (intel_lpe_driver*)driverCookie;
	return sDeviceManager->publish_device(driver->node, INTEL_LPE_DEVICE_PATH,
		INTEL_LPE_DEVICE_MODULE_NAME);
}


driver_module_info intel_lpe_driver_module = {
	{
		INTEL_LPE_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	intel_lpe_supports_device,
	intel_lpe_register_device,
	intel_lpe_init_driver,
	intel_lpe_uninit_driver,
	intel_lpe_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


struct device_module_info intel_lpe_device_module = {
	{
		INTEL_LPE_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	intel_lpe_init_device,
	intel_lpe_uninit_device,
	NULL,	// removed

	intel_lpe_open,
	intel_lpe_close,
	intel_lpe_free,
	intel_lpe_read,
	intel_lpe_write,
	NULL,	// io
	intel_lpe_control,

	NULL,	// select
	NULL	// deselect
};


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


module_info* modules[] = {
	(module_info*)&intel_lpe_driver_module,
	(module_info*)&intel_lpe_device_module,
	NULL
};
