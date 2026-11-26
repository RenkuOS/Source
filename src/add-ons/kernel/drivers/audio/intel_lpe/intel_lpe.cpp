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
// codec as a multi_audio device: 48 kHz, 16 bit stereo, two buffers of
// 1024 frames, played through a fixed pipeline from memory to SSP0 and the
// codec. Playback goes to the headphones while they are plugged in, else to
// the speakers, and records from the internal microphone. Volume controls
// are not done yet.


#include <ACPI.h>
#include <KernelExport.h>
#include <device_manager.h>
#include <i2c.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>

#include <hmulti_audio.h>

#include <FindDirectory.h>
#include <condition_variable.h>

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

#define DSP_BOOTING					0
#define DSP_READY					1
#define DSP_FAILED					2

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

// How the playback thread waits for the next buffer: woken by the position
// message of the DSP, or, without its interrupt, by polling: sleeping most
// of a buffer after a change, then checking every POLL_INTERVAL.
#define POLL_INTERVAL				500
#define POLL_EARLY_WAKEUP			3000
// short enough that a lost position message does not skip a buffer
#define POSITION_TIMEOUT			(PLAYBACK_BUFFER_TIME / 4)
#define EXCHANGE_TIMEOUT			500000
// After about 5 seconds of silence the output amplifiers are powered down.
#define IDLE_BUFFERS				(5 * PLAYBACK_RATE / PLAYBACK_BUFFER_FRAMES)


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

	// DSP_BOOTING until start_dsp() is done; dspBooted is notified then.
	int32					dspState;
	ConditionVariable		dspBooted;

	// one ring per direction (LPE_PLAYBACK, LPE_CAPTURE)
	area_id					ringArea[LPE_STREAM_COUNT];
	uint8*					ring[LPE_STREAM_COUNT];
	area_id					tableArea[LPE_STREAM_COUNT];
	phys_addr_t				tablePhysical[LPE_STREAM_COUNT];

	// Protects the fields below and the start and stop of playback.
	mutex					lock;
	int32					openCount;
	int32					running;
	thread_id				playbackThread;
	sem_id					bufferReadySem;
	sem_id					positionSem;
	bool					capturing;
	// guarded by outputLock
	bool					microphoneActive;
	bool					headsetMicrophone;

	// mixer settings, in codec steps; guarded by outputLock
	uint8					dacVolume[2];
	bool					dacMuted;
	uint8					adcLevel[2];
	bool					adcMuted;

	// Protects the output routing: speakers or headphones.
	mutex					outputLock;
	bool					headphonesPlugged;
	bool					outputActive;
	bool					headphoneAmpOn;
	bool					speakerPowered;
	bool					outputIdle;
	// silent buffers in a row, counted by the playback thread; the jack
	// thread powers the output down and up, woken through idleSem
	int32					silentBuffers;
	sem_id					idleSem;

	// Playback position, written by the playback thread.
	spinlock				positionLock;
	uint32					bufferCycle;
	int64					framesCount;
	bigtime_t				realTime;
	uint32					recordCycle;
	int64					recordedFrames;
	bigtime_t				recordTime;
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
static void update_output(intel_lpe_driver* driver);
static void select_microphone(intel_lpe_driver* driver);
static bool detect_headset(intel_lpe_driver* driver);


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

				// JD1 is low while headphones are plugged in; the output
				// follows at once, the microphone once the plug is known
				bool plugged = (status & RT5640_STATUS_JD) == 0;
				{
					MutexLocker outputLocker(driver->outputLock);
					driver->headphonesPlugged = plugged;
					driver->headsetMicrophone = false;
					update_output(driver);
					select_microphone(driver);
				}
				if (plugged && detect_headset(driver)) {
					MutexLocker outputLocker(driver->outputLock);
					driver->headsetMicrophone = true;
					select_microphone(driver);
				}
			}
		}
		// power the output down after a while of silence, and up again as
		// soon as the playback thread sees sound
		bool idle = atomic_get(&driver->silentBuffers) >= IDLE_BUFFERS;
		if (idle != driver->outputIdle) {
			MutexLocker outputLocker(driver->outputLock);
			driver->outputIdle = idle;
			update_output(driver);
			TRACE_ALWAYS("output %s\n", idle ? "idle, amplifiers off"
				: "in use, amplifiers on");
		}

		acquire_sem_etc(driver->idleSem, 1, B_RELATIVE_TIMEOUT,
			JACK_POLL_INTERVAL);
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


/*!	Called from the DSP interrupt for each period a stream completed. */
static void
position_reported(void* cookie, uint32 direction)
{
	intel_lpe_driver* driver = (intel_lpe_driver*)cookie;
	if (direction == LPE_PLAYBACK)
		release_sem_etc(driver->positionSem, 1, B_DO_NOT_RESCHEDULE);

	static int32 sFirst = 0;
	if (atomic_add(&sFirst, 1) == 0)
		TRACE_ALWAYS("position messages from the DSP arrive\n");
}


static status_t
boot_dsp(intel_lpe_driver* driver)
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
		return status;
	}

	MutexLocker dspLocker(driver->dspLock);
	status = lpe_dsp_init(&driver->dsp);
	driver->dsp.positionHook = position_reported;
	driver->dsp.hookCookie = driver;
	if (status == B_OK) {
		driver->dspInitialized = true;
		status = lpe_dsp_boot(&driver->dsp, image, size);
	}
	dspLocker.Unlock();
	free(image);

	if (status != B_OK)
		TRACE_ALWAYS("DSP did not boot: %s\n", strerror(status));
	return status;
}


/*!	Boots the DSP once the firmware can be read from the boot volume, and
	records the outcome for wait_for_dsp().
*/
static void
start_dsp(intel_lpe_driver* driver)
{
	status_t status = boot_dsp(driver);
	atomic_set(&driver->dspState, status == B_OK ? DSP_READY : DSP_FAILED);
	driver->dspBooted.NotifyAll();
}


/*!	Waits until start_dsp() is done. Called without driver->lock held, so
	that open and close are not blocked meanwhile.
*/
static status_t
wait_for_dsp(intel_lpe_driver* driver)
{
	if (atomic_get(&driver->dspState) == DSP_BOOTING) {
		ConditionVariableEntry entry;
		driver->dspBooted.Add(&entry);
		if (atomic_get(&driver->dspState) == DSP_BOOTING) {
			status_t status = entry.Wait(B_CAN_INTERRUPT | B_RELATIVE_TIMEOUT,
				DSP_READY_WAIT);
			if (status == B_INTERRUPTED)
				return status;
		}
	}
	return atomic_get(&driver->dspState) == DSP_READY ? B_OK : B_DEV_NOT_READY;
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
	{ 0x01, 0x8888 },	// speaker volume, muted until the output is chosen
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
#define RT5640_SPEAKER_ON			0x0808	// 0 dB
#define RT5640_SPEAKER_MUTED		0x8888
#define RT5640_HEADPHONE_VOLUME		0x02
#define RT5640_HEADPHONE_ON			0x0c0c	// -6 dB
#define RT5640_HEADPHONE_MUTE		0x8080
#define RT5640_POWER_ANALOG1		0x63
#define RT5640_POWER_MIXER			0x65
#define RT5640_POWER_VOLUME			0x66
#define RT5640_DEPOP_1				0x8e
#define RT5640_DEPOP_2				0x8f
#define RT5640_DEPOP_3				0x90
#define RT5640_CHARGE_PUMP			0x91

// MX-63
#define RT5640_POWER_FV1			(1 << 14)
#define RT5640_POWER_HP_L			(1 << 7)
#define RT5640_POWER_HP_R			(1 << 6)
#define RT5640_POWER_HP_AMP			(1 << 5)
#define RT5640_POWER_FV2			(1 << 3)
// MX-65, MX-66
#define RT5640_POWER_OUT_MIX		(3 << 14)
#define RT5640_POWER_HP_VOLUME		(3 << 10)
// private registers
#define RT5640_PR_CHARGE_PUMP_1		0x24
#define RT5640_PR_MAIN_AMP_2		0x37
#define RT5640_PR_HP_DCC_1			0x77

static void
rt5640_update(intel_lpe_driver* driver, uint8 reg, uint16 mask, uint16 value)
{
	uint16 old;
	if (rt5640_read(driver, reg, &old) == B_OK)
		rt5640_write(driver, reg, (old & ~mask) | (value & mask));
}


static void
rt5640_write_private(intel_lpe_driver* driver, uint8 reg, uint16 value)
{
	rt5640_write(driver, RT5640_PRIVATE_INDEX, reg);
	rt5640_write(driver, RT5640_PRIVATE_DATA, value);
}


static void
rt5640_update_private(intel_lpe_driver* driver, uint8 reg, uint16 mask,
	uint16 value)
{
	uint16 old;
	rt5640_write(driver, RT5640_PRIVATE_INDEX, reg);
	if (rt5640_read(driver, RT5640_PRIVATE_DATA, &old) == B_OK)
		rt5640_write(driver, RT5640_PRIVATE_DATA,
			(old & ~mask) | (value & mask));
}


/*!	Powers the headphone path up: output mixers and headphone volumes,
	then the amplifier with its charge pump in the order the codec needs to
	avoid a pop, then the volume. The order and the depop values follow
	what Linux does for this codec (rt5640.c, used as a description).
*/
static void
headphone_on(intel_lpe_driver* driver)
{
	rt5640_update(driver, RT5640_POWER_MIXER, RT5640_POWER_OUT_MIX,
		RT5640_POWER_OUT_MIX);
	rt5640_update(driver, RT5640_POWER_VOLUME, RT5640_POWER_HP_VOLUME,
		RT5640_POWER_HP_VOLUME);
	rt5640_update(driver, RT5640_POWER_ANALOG1,
		RT5640_POWER_HP_L | RT5640_POWER_HP_R,
		RT5640_POWER_HP_L | RT5640_POWER_HP_R);

	// amplifier: manual depop, charge pump up, soft generator off
	rt5640_update_private(driver, RT5640_PR_CHARGE_PUMP_1, 0x0700, 0x0200);
	rt5640_update(driver, RT5640_DEPOP_2, 1 << 13, 1 << 13);
	rt5640_update(driver, RT5640_DEPOP_1, (1 << 3) | (1 << 2) | 1,
		(1 << 3) | 1);
	rt5640_write_private(driver, RT5640_PR_HP_DCC_1, 0x9f00);
	rt5640_update(driver, RT5640_POWER_ANALOG1,
		RT5640_POWER_FV1 | RT5640_POWER_FV2, 0);
	rt5640_update(driver, RT5640_POWER_ANALOG1, RT5640_POWER_HP_AMP,
		RT5640_POWER_HP_AMP);
	snooze(12000);
	rt5640_update(driver, RT5640_POWER_ANALOG1,
		RT5640_POWER_FV1 | RT5640_POWER_FV2,
		RT5640_POWER_FV1 | RT5640_POWER_FV2);

	// then automatic depop: digital depop on, charge pump in high voltage
	// mode with its clocks (192 kHz, 12 kHz, 192 kHz), soft generator on
	rt5640_update(driver, RT5640_DEPOP_2, (1 << 13) | (1 << 6), 1 << 6);
	rt5640_update(driver, RT5640_CHARGE_PUMP, 3 << 8, 2 << 8);
	rt5640_update(driver, RT5640_DEPOP_3, 0x0777, 0x0737);
	rt5640_write_private(driver, RT5640_PR_MAIN_AMP_2, 0x1c00);
	rt5640_update(driver, RT5640_DEPOP_1, (1 << 3) | (1 << 2), 1 << 2);
	rt5640_update_private(driver, RT5640_PR_CHARGE_PUMP_1, 0x0700, 0x0400);

	rt5640_write(driver, RT5640_HEADPHONE_VOLUME, RT5640_HEADPHONE_ON);
	snooze(80000);
	driver->headphoneAmpOn = true;
}


static void
headphone_off(intel_lpe_driver* driver)
{
	rt5640_update(driver, RT5640_HEADPHONE_VOLUME, RT5640_HEADPHONE_MUTE,
		RT5640_HEADPHONE_MUTE);
	snooze(70000);
	rt5640_update(driver, RT5640_POWER_ANALOG1,
		RT5640_POWER_HP_L | RT5640_POWER_HP_R | RT5640_POWER_HP_AMP, 0);
	rt5640_update(driver, RT5640_POWER_VOLUME, RT5640_POWER_HP_VOLUME, 0);
	rt5640_update(driver, RT5640_POWER_MIXER, RT5640_POWER_OUT_MIX, 0);
	driver->headphoneAmpOn = false;
}


// the speaker path: class D amplifier, speaker mixers and volumes
#define RT5640_POWER_CLASS_D		(1 << 0)	// MX-61
#define RT5640_POWER_SPEAKER_MIX	(3 << 12)	// MX-65
#define RT5640_POWER_SPEAKER_VOLUME	(3 << 14)	// MX-66


/*!	Called with outputLock held, the speaker volume muted. */
static void
speaker_power(intel_lpe_driver* driver, bool on)
{
	rt5640_update(driver, RT5640_PWR_DIG1, RT5640_POWER_CLASS_D,
		on ? RT5640_POWER_CLASS_D : 0);
	rt5640_update(driver, RT5640_POWER_MIXER, RT5640_POWER_SPEAKER_MIX,
		on ? RT5640_POWER_SPEAKER_MIX : 0);
	rt5640_update(driver, RT5640_POWER_VOLUME, RT5640_POWER_SPEAKER_VOLUME,
		on ? RT5640_POWER_SPEAKER_VOLUME : 0);
	driver->speakerPowered = on;
}


/*!	Routes playback to the headphones while they are plugged in, else to
	the speakers; both are off when nothing plays or after IDLE_BUFFERS of
	silence, to save power. Called with outputLock held.
*/
static void
update_output(intel_lpe_driver* driver)
{
	bool active = driver->outputActive && !driver->outputIdle;
	bool speaker = active && !driver->headphonesPlugged;
	bool headphones = active && driver->headphonesPlugged;

	if (!speaker) {
		rt5640_write(driver, RT5640_SPEAKER_VOLUME, RT5640_SPEAKER_MUTED);
		if (driver->speakerPowered)
			speaker_power(driver, false);
	}
	if (!headphones && driver->headphoneAmpOn)
		headphone_off(driver);

	if (headphones && !driver->headphoneAmpOn)
		headphone_on(driver);
	if (speaker) {
		if (!driver->speakerPowered)
			speaker_power(driver, true);
		rt5640_write(driver, RT5640_SPEAKER_VOLUME, RT5640_SPEAKER_ON);
	}
}


// Recording from the internal microphone, a differential microphone on IN3
// (Android: "IN3 Mode Control Differential", "Int Mic Switch On"), or from
// the microphone of a headset on IN2 (single ended, as Android and the Linux
// bytcr_rt5640 machine driver set it up). The input modes and the IN2 boost
// come with the playback setup; this powers the path and opens the ADC.
#define RT5640_PWR_DIG2				0x62
#define RT5640_STEREO_ADC_MIXER		0x27
#define RT5640_ADC_VOLUME			0x1c
#define RT5640_RECORD_MIXER_L		0x3c
#define RT5640_RECORD_MIXER_R		0x3e
#define RT5640_MICBIAS				0x93
#define RT5640_PR_BIAS_CURRENT4		0x15

#define RT5640_POWER_ADC			(3 << 1)	// MX-61: ADC L, R
#define RT5640_POWER_ADC_FILTER		(1 << 15)	// MX-62
#define RT5640_POWER_BOOST_IN3		(1 << 14)	// MX-64
#define RT5640_POWER_BOOST_IN2		(1 << 12)	// MX-64
#define RT5640_POWER_MICBIAS1		(1 << 11)	// MX-64
#define RT5640_POWER_LDO2			(1 << 2)	// MX-63, supplies IN1..IN3
#define RT5640_POWER_RECORD_MIX		(3 << 10)	// MX-65: RECMIX L, R
#define RT5640_MUTE_ADC1			((1 << 14) | (1 << 6))
// record mixer mutes (MX-3C, MX-3E): bit 2 is the IN3 boost, bit 4 IN2
#define RT5640_RECORD_MUTE_IN3		(1 << 2)
#define RT5640_RECORD_MUTE_IN2		(1 << 4)
// The gains of the Linux UCM profile for this codec: IN3 boost at its
// maximum (52 dB) and the ADC at +9 dB (0x2f is 0 dB, 0.375 dB a step).
#define RT5640_BOOST_IN3_MASK		(0xf << 8)	// MX-0D
#define RT5640_BOOST_IN3_52DB		(8 << 8)
#define RT5640_ADC_ON				0x4747		// +9 dB, unmuted
#define RT5640_ADC_MUTED			0xafaf
#define RT5640_ADC_MUTE_BITS		0x8080		// MX-1C: L, R

// Over-current detection on MICBIAS1 tells a headset from headphones: the
// microphone ring of plain headphones shorts the bias to ground. Values of
// the Linux driver for Bay Trail-CR boards: 2000 uA, scale factor 0.75.
#define RT5640_MIC1_OVCD_ENABLE		(1 << 11)	// MX-93
#define RT5640_MIC1_OVTH_MASK		(3 << 9)
#define RT5640_MIC1_OVTH_2000UA		(2 << 9)
#define RT5640_OVCD_STICKY			(1 << 11)	// MX-BE
#define RT5640_OVCD_STATUS			(1 << 3)
#define RT5640_OVCD_SCALE_0P75		0xa900		// PR-15
#define HEADSET_SETTLE_TIME			100000
#define HEADSET_DETECT_COUNT		5
#define HEADSET_DETECT_TRIES		20


static void
microphone_bias(intel_lpe_driver* driver, bool on)
{
	rt5640_update(driver, RT5640_POWER_ANALOG1, RT5640_POWER_LDO2,
		on ? RT5640_POWER_LDO2 : 0);
	rt5640_update(driver, RT5640_PWR_ANLG2, RT5640_POWER_MICBIAS1,
		on ? RT5640_POWER_MICBIAS1 : 0);
}


static uint16
record_volume(intel_lpe_driver* driver)
{
	return (driver->adcLevel[0] << 8) | driver->adcLevel[1]
		| (driver->adcMuted ? RT5640_ADC_MUTE_BITS : 0);
}


/*!	Routes the headset microphone while a headset is plugged in, the
	internal microphone otherwise. Called with outputLock held.
*/
static void
select_microphone(intel_lpe_driver* driver)
{
	if (!driver->microphoneActive)
		return;

	bool headset = driver->headphonesPlugged && driver->headsetMicrophone;
	uint16 power = headset ? RT5640_POWER_BOOST_IN2 : RT5640_POWER_BOOST_IN3;
	uint16 mutes = headset ? RT5640_RECORD_MUTE_IN3 : RT5640_RECORD_MUTE_IN2;

	rt5640_update(driver, RT5640_PWR_ANLG2,
		RT5640_POWER_BOOST_IN2 | RT5640_POWER_BOOST_IN3, power);
	rt5640_update(driver, RT5640_RECORD_MIXER_L,
		RT5640_RECORD_MUTE_IN2 | RT5640_RECORD_MUTE_IN3, mutes);
	rt5640_update(driver, RT5640_RECORD_MIXER_R,
		RT5640_RECORD_MUTE_IN2 | RT5640_RECORD_MUTE_IN3, mutes);
	TRACE_ALWAYS("recording from the %s microphone\n",
		headset ? "headset" : "internal");
}


/*!	Takes outputLock: the headphone path updates the same power registers
	from the jack thread.
*/
static void
microphone_on(intel_lpe_driver* driver)
{
	MutexLocker outputLocker(driver->outputLock);
	microphone_bias(driver, true);
	rt5640_update(driver, RT5640_IN1_IN2, RT5640_BOOST_IN3_MASK,
		RT5640_BOOST_IN3_52DB);
	rt5640_update(driver, RT5640_POWER_MIXER, RT5640_POWER_RECORD_MIX,
		RT5640_POWER_RECORD_MIX);
	rt5640_update(driver, RT5640_PWR_DIG1, RT5640_POWER_ADC,
		RT5640_POWER_ADC);
	rt5640_update(driver, RT5640_PWR_DIG2, RT5640_POWER_ADC_FILTER,
		RT5640_POWER_ADC_FILTER);
	rt5640_update(driver, RT5640_STEREO_ADC_MIXER, RT5640_MUTE_ADC1, 0);
	driver->microphoneActive = true;
	select_microphone(driver);
	rt5640_write(driver, RT5640_ADC_VOLUME, record_volume(driver));
}


static void
microphone_off(intel_lpe_driver* driver)
{
	MutexLocker outputLocker(driver->outputLock);
	driver->microphoneActive = false;
	rt5640_write(driver, RT5640_ADC_VOLUME, RT5640_ADC_MUTED);
	rt5640_update(driver, RT5640_STEREO_ADC_MIXER, RT5640_MUTE_ADC1,
		RT5640_MUTE_ADC1);
	rt5640_update(driver, RT5640_RECORD_MIXER_L,
		RT5640_RECORD_MUTE_IN2 | RT5640_RECORD_MUTE_IN3,
		RT5640_RECORD_MUTE_IN2 | RT5640_RECORD_MUTE_IN3);
	rt5640_update(driver, RT5640_RECORD_MIXER_R,
		RT5640_RECORD_MUTE_IN2 | RT5640_RECORD_MUTE_IN3,
		RT5640_RECORD_MUTE_IN2 | RT5640_RECORD_MUTE_IN3);
	rt5640_update(driver, RT5640_PWR_DIG2, RT5640_POWER_ADC_FILTER, 0);
	rt5640_update(driver, RT5640_PWR_DIG1, RT5640_POWER_ADC, 0);
	rt5640_update(driver, RT5640_POWER_MIXER, RT5640_POWER_RECORD_MIX, 0);
	rt5640_update(driver, RT5640_PWR_ANLG2,
		RT5640_POWER_BOOST_IN2 | RT5640_POWER_BOOST_IN3, 0);
	microphone_bias(driver, false);
}


/*!	Tells a headset from headphones after a plug was inserted, the way the
	Linux rt5640 driver describes it: with MICBIAS1 on, over-current means
	the microphone ring is shorted to ground. The plug may still be moving,
	so the same result must come HEADSET_DETECT_COUNT times in a row.
	Called from the jack thread without outputLock held.
*/
static bool
detect_headset(intel_lpe_driver* driver)
{
	{
		MutexLocker outputLocker(driver->outputLock);
		microphone_bias(driver, true);
		rt5640_write_private(driver, RT5640_PR_BIAS_CURRENT4,
			RT5640_OVCD_SCALE_0P75);
		rt5640_update(driver, RT5640_MICBIAS,
			RT5640_MIC1_OVCD_ENABLE | RT5640_MIC1_OVTH_MASK,
			RT5640_MIC1_OVCD_ENABLE | RT5640_MIC1_OVTH_2000UA);
		rt5640_update(driver, RT5640_IRQ_CTRL2, RT5640_OVCD_STICKY,
			RT5640_OVCD_STICKY);
	}

	bool headset = false;
	int32 headsetCount = 0;
	int32 headphoneCount = 0;
	for (int32 i = 0; i < HEADSET_DETECT_TRIES; i++) {
		rt5640_update(driver, RT5640_IRQ_CTRL2, RT5640_OVCD_STATUS, 0);
		snooze(HEADSET_SETTLE_TIME);

		uint16 status, control;
		if (rt5640_read(driver, RT5640_INT_IRQ_ST, &status) != B_OK
			|| (status & RT5640_STATUS_JD) != 0
			|| rt5640_read(driver, RT5640_IRQ_CTRL2, &control) != B_OK) {
			// unplugged again
			break;
		}
		if ((control & RT5640_OVCD_STATUS) != 0) {
			headsetCount = 0;
			if (++headphoneCount == HEADSET_DETECT_COUNT)
				break;
		} else {
			headphoneCount = 0;
			if (++headsetCount == HEADSET_DETECT_COUNT) {
				headset = true;
				break;
			}
		}
	}

	MutexLocker outputLocker(driver->outputLock);
	if (!driver->microphoneActive)
		microphone_bias(driver, false);
	TRACE_ALWAYS("jack: %s\n", headset ? "headset with microphone"
		: "headphones without microphone");
	return headset;
}


// Volume controls: the DAC1 digital volume (MX-19, 0.375 dB steps, 0xaf is
// 0 dB) for both outputs, and the ADC digital volume (MX-1C, 0.375 dB steps,
// 0x2f is 0 dB, bits 15 and 7 mute) for recording.
#define RT5640_DAC1_VOLUME			0x19
#define DAC_VOLUME_MAX				0xaf
#define ADC_LEVEL_ZERO_DB			0x2f
#define ADC_LEVEL_MAX				0x7f
#define ADC_LEVEL_DEFAULT			0x47		// +9 dB, see RT5640_ADC_ON
#define VOLUME_STEP					0.375f
#define RT5640_STEREO_DAC_MIXER		0x2a
#define RT5640_MUTE_DAC1			((1 << 14) | (1 << 6))	// MX-2A


/*!	Called with outputLock held. */
static void
apply_playback_volume(intel_lpe_driver* driver)
{
	rt5640_write(driver, RT5640_DAC1_VOLUME,
		(driver->dacVolume[0] << 8) | driver->dacVolume[1]);
	rt5640_update(driver, RT5640_STEREO_DAC_MIXER, RT5640_MUTE_DAC1,
		driver->dacMuted ? RT5640_MUTE_DAC1 : 0);
}


/*!	Called with outputLock held. */
static void
apply_record_volume(intel_lpe_driver* driver)
{
	if (driver->microphoneActive)
		rt5640_write(driver, RT5640_ADC_VOLUME, record_volume(driver));
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
	apply_playback_volume(driver);
	return B_OK;
}


//	#pragma mark - playback


/*!	Counts the silent buffers in a row, and wakes the jack thread when the
	output should be powered down or up.
*/
static void
count_silence(intel_lpe_driver* driver, uint32 buffer)
{
	const uint32* words = (const uint32*)(driver->ring[LPE_PLAYBACK]
		+ buffer * PLAYBACK_BUFFER_SIZE);
	bool silent = true;
	for (size_t i = 0; i < PLAYBACK_BUFFER_SIZE / sizeof(uint32); i++) {
		if (words[i] != 0) {
			silent = false;
			break;
		}
	}

	if (silent) {
		// stops counting at IDLE_BUFFERS
		if (atomic_get(&driver->silentBuffers) < IDLE_BUFFERS
			&& atomic_add(&driver->silentBuffers, 1) == IDLE_BUFFERS - 1)
			release_sem_etc(driver->idleSem, 1, B_DO_NOT_RESCHEDULE);
	} else if (atomic_get_and_set(&driver->silentBuffers, 0) >= IDLE_BUFFERS)
		release_sem_etc(driver->idleSem, 1, B_DO_NOT_RESCHEDULE);
}


static uint32
current_buffer(intel_lpe_driver* driver, uint32 direction)
{
	sof_ipc_stream_posn position;
	if (lpe_dsp_read_position(&driver->dsp, direction, &position) != B_OK)
		return PLAYBACK_BUFFERS;
	return (uint32)(position.host_posn / PLAYBACK_BUFFER_SIZE)
		% PLAYBACK_BUFFERS;
}


/*!	Follows the host positions, which the firmware writes into the stream
	mailbox every millisecond. Each change of the playback buffer means a
	buffer finished playing; buffer_exchange() then hands that buffer to the
	media add-on, together with the record buffer that was last filled.
*/
static int32
playback_thread(void* data)
{
	intel_lpe_driver* driver = (intel_lpe_driver*)data;
	uint32 last = 0;

	while (atomic_get(&driver->running) != 0) {
		sof_ipc_stream_posn position;
		if (lpe_dsp_read_position(&driver->dsp, LPE_PLAYBACK, &position)
				!= B_OK) {
			snooze(POLL_INTERVAL);
			continue;
		}
		uint32 current = (uint32)(position.host_posn / PLAYBACK_BUFFER_SIZE)
			% PLAYBACK_BUFFERS;
		if (current == last) {
			if (driver->dsp.interruptsEnabled) {
				acquire_sem_etc(driver->positionSem, 1, B_RELATIVE_TIMEOUT,
					POSITION_TIMEOUT);
			} else
				snooze(POLL_INTERVAL);
			continue;
		}
		last = current;
		bigtime_t now = system_time();
		count_silence(driver, current);

		// Both streams run off the same SSP clock, so each playback buffer
		// change comes with one complete record buffer: the one the DSP is
		// not writing into.
		uint32 record = PLAYBACK_BUFFERS;
		if (driver->capturing)
			record = current_buffer(driver, LPE_CAPTURE);

		cpu_status state = disable_interrupts();
		acquire_spinlock(&driver->positionLock);
		driver->bufferCycle = current;
		driver->framesCount += PLAYBACK_BUFFER_FRAMES;
		driver->realTime = now;
		if (record < PLAYBACK_BUFFERS) {
			driver->recordCycle = (record + 1) % PLAYBACK_BUFFERS;
			driver->recordedFrames += PLAYBACK_BUFFER_FRAMES;
			driver->recordTime = now;
		}
		release_spinlock(&driver->positionLock);
		restore_interrupts(state);

		release_sem_etc(driver->bufferReadySem, 1, B_DO_NOT_RESCHEDULE);
		if (!driver->dsp.interruptsEnabled)
			snooze(PLAYBACK_BUFFER_TIME - POLL_EARLY_WAKEUP);
	}
	return 0;
}


static status_t
playback_start(intel_lpe_driver* driver)
{
	if (atomic_get(&driver->dspState) != DSP_READY)
		return B_DEV_NOT_READY;

	int32 count;
	if (get_sem_count(driver->bufferReadySem, &count) == B_OK && count > 0)
		acquire_sem_etc(driver->bufferReadySem, count, B_RELATIVE_TIMEOUT, 0);
	if (get_sem_count(driver->positionSem, &count) == B_OK && count > 0)
		acquire_sem_etc(driver->positionSem, count, B_RELATIVE_TIMEOUT, 0);

	status_t status;
	{
		MutexLocker outputLocker(driver->outputLock);
		status = setup_speaker_playback(driver);
		// the setup leaves the headphone amplifier off and powers the
		// speaker path
		driver->headphoneAmpOn = false;
		driver->speakerPowered = true;
		driver->outputIdle = false;
		atomic_set(&driver->silentBuffers, 0);
	}
	if (status != B_OK)
		return status;

	driver->bufferCycle = 0;
	driver->framesCount = 0;
	driver->realTime = system_time();
	driver->recordCycle = 0;
	driver->recordedFrames = 0;
	driver->recordTime = driver->realTime;

	lpe_stream_buffer buffer;
	buffer.pageTable = driver->tablePhysical[LPE_PLAYBACK];
	buffer.pageCount = PLAYBACK_RING_SIZE / B_PAGE_SIZE;
	buffer.size = PLAYBACK_RING_SIZE;
	buffer.periodBytes = PLAYBACK_BUFFER_SIZE;
	{
		MutexLocker dspLocker(driver->dspLock);
		status = lpe_dsp_start_stream(&driver->dsp, LPE_PLAYBACK, &buffer);
	}
	if (status != B_OK)
		return status;

	// recording runs along with playback; if it does not start, playback
	// still does
	microphone_on(driver);
	buffer.pageTable = driver->tablePhysical[LPE_CAPTURE];
	{
		MutexLocker dspLocker(driver->dspLock);
		driver->capturing = lpe_dsp_start_stream(&driver->dsp, LPE_CAPTURE,
			&buffer) == B_OK;
	}
	if (!driver->capturing) {
		TRACE_ALWAYS("recording did not start\n");
		microphone_off(driver);
	}

	{
		MutexLocker outputLocker(driver->outputLock);
		driver->outputActive = true;
		update_output(driver);
	}

	driver->running = 1;
	driver->playbackThread = spawn_kernel_thread(playback_thread,
		"intel_lpe playback", B_REAL_TIME_DISPLAY_PRIORITY, driver);
	if (driver->playbackThread < 0) {
		status = driver->playbackThread;
		driver->running = 0;
		{
			MutexLocker outputLocker(driver->outputLock);
			driver->outputActive = false;
			update_output(driver);
		}
		MutexLocker dspLocker(driver->dspLock);
		lpe_dsp_stop_stream(&driver->dsp, LPE_PLAYBACK);
		if (driver->capturing) {
			lpe_dsp_stop_stream(&driver->dsp, LPE_CAPTURE);
			driver->capturing = false;
			dspLocker.Unlock();
			microphone_off(driver);
		}
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

	{
		MutexLocker outputLocker(driver->outputLock);
		driver->outputActive = false;
		update_output(driver);
	}
	{
		MutexLocker dspLocker(driver->dspLock);
		lpe_dsp_stop_stream(&driver->dsp, LPE_PLAYBACK);
		if (driver->capturing)
			lpe_dsp_stop_stream(&driver->dsp, LPE_CAPTURE);
	}
	if (driver->capturing) {
		driver->capturing = false;
		microphone_off(driver);
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
	{ 4, B_MULTI_INPUT_CHANNEL, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0 },
	{ 5, B_MULTI_INPUT_CHANNEL, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0 },
	{ 6, B_MULTI_INPUT_BUS, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO },
	{ 7, B_MULTI_INPUT_BUS, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO },
};


static status_t
get_description(intel_lpe_driver* driver, multi_description* data)
{
	data->interface_version = B_CURRENT_INTERFACE_VERSION;
	data->interface_minimum = B_CURRENT_INTERFACE_VERSION;

	strlcpy(data->friendly_name, "Intel LPE audio (RT5640)",
		sizeof(data->friendly_name));
	strlcpy(data->vendor_info, "Renku", sizeof(data->vendor_info));

	data->output_channel_count = PLAYBACK_CHANNELS;
	data->output_bus_channel_count = PLAYBACK_CHANNELS;
	data->input_channel_count = PLAYBACK_CHANNELS;
	data->input_bus_channel_count = PLAYBACK_CHANNELS;
	data->aux_bus_channel_count = 0;

	if (data->request_channel_count >= (int32)B_COUNT_OF(sChannels))
		memcpy(data->channels, sChannels, sizeof(sChannels));

	data->output_rates = B_SR_48000;
	data->input_rates = B_SR_48000;
	data->max_cvsr_rate = 0;
	data->min_cvsr_rate = 0;
	data->output_formats = B_FMT_16BIT;
	data->input_formats = B_FMT_16BIT;
	data->lock_sources = B_MULTI_LOCK_INTERNAL;
	data->timecode_sources = 0;
	data->interface_flags
		= B_MULTI_INTERFACE_PLAYBACK | B_MULTI_INTERFACE_RECORD;
	data->start_latency = 30000;
	data->control_panel[0] = '\0';
	return B_OK;
}


static status_t
get_enabled_channels(intel_lpe_driver* driver, multi_channel_enable* data)
{
	data->lock_source = B_MULTI_LOCK_INTERNAL;
	// the playback and the record channels
	for (int32 i = 0; i < 2 * PLAYBACK_CHANNELS; i++)
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
	data->input.format = B_FMT_16BIT;
	data->input.rate = B_SR_48000;
	return B_OK;
}


static status_t
set_global_format(intel_lpe_driver* driver, multi_format_info* data)
{
	// there is only one format; report it back
	return get_global_format(driver, data);
}


// The multi_audio node expects a top level group holding a group per
// channel strip, and in it the mute before the gain of the left and the
// right channel.
enum {
	MIX_OUTPUT_GROUP = 100,
	MIX_OUTPUT_STRIP,
	MIX_OUTPUT_MUTE,
	MIX_OUTPUT_LEFT,
	MIX_OUTPUT_RIGHT,
	MIX_INPUT_GROUP,
	MIX_INPUT_STRIP,
	MIX_INPUT_MUTE,
	MIX_INPUT_LEFT,
	MIX_INPUT_RIGHT,
	MIX_CONTROL_END
};


static void
init_mix_control(multi_mix_control* control, int32 id, uint32 flags,
	int32 parent, int32 master, enum strind_id string, const char* name)
{
	memset(control, 0, sizeof(*control));
	control->id = id;
	control->flags = flags;
	control->parent = parent;
	control->master = master;
	control->string = string;
	if (name != NULL)
		strlcpy(control->name, name, sizeof(control->name));
}


static status_t
list_mix_controls(intel_lpe_driver* driver, multi_mix_control_info* data)
{
	const int32 count = MIX_CONTROL_END - MIX_OUTPUT_GROUP;
	if (data->control_count < count)
		return B_BAD_VALUE;

	multi_mix_control* controls = data->controls;
	init_mix_control(&controls[0], MIX_OUTPUT_GROUP, B_MULTI_MIX_GROUP, 0, 0,
		S_OUTPUT, NULL);
	init_mix_control(&controls[1], MIX_OUTPUT_STRIP, B_MULTI_MIX_GROUP,
		MIX_OUTPUT_GROUP, 0, S_MASTER, NULL);
	init_mix_control(&controls[2], MIX_OUTPUT_MUTE, B_MULTI_MIX_ENABLE,
		MIX_OUTPUT_STRIP, 0, S_MUTE, NULL);
	for (int32 i = 0; i < 2; i++) {
		multi_mix_control* control = &controls[3 + i];
		init_mix_control(control, MIX_OUTPUT_LEFT + i, B_MULTI_MIX_GAIN,
			MIX_OUTPUT_STRIP, i == 0 ? 0 : MIX_OUTPUT_LEFT, S_GAIN, NULL);
		control->gain.min_gain = -DAC_VOLUME_MAX * VOLUME_STEP;
		control->gain.max_gain = 0;
		control->gain.granularity = VOLUME_STEP;
	}

	init_mix_control(&controls[5], MIX_INPUT_GROUP, B_MULTI_MIX_GROUP, 0, 0,
		S_INPUT, NULL);
	init_mix_control(&controls[6], MIX_INPUT_STRIP, B_MULTI_MIX_GROUP,
		MIX_INPUT_GROUP, 0, S_MIC, NULL);
	init_mix_control(&controls[7], MIX_INPUT_MUTE, B_MULTI_MIX_ENABLE,
		MIX_INPUT_STRIP, 0, S_MUTE, NULL);
	for (int32 i = 0; i < 2; i++) {
		multi_mix_control* control = &controls[8 + i];
		init_mix_control(control, MIX_INPUT_LEFT + i, B_MULTI_MIX_GAIN,
			MIX_INPUT_STRIP, i == 0 ? 0 : MIX_INPUT_LEFT, S_GAIN, NULL);
		control->gain.min_gain = -ADC_LEVEL_ZERO_DB * VOLUME_STEP;
		control->gain.max_gain
			= (ADC_LEVEL_MAX - ADC_LEVEL_ZERO_DB) * VOLUME_STEP;
		control->gain.granularity = VOLUME_STEP;
	}

	data->control_count = count;
	return B_OK;
}


static status_t
list_mix_connections(intel_lpe_driver* driver,
	multi_mix_connection_info* data)
{
	data->actual_count = 0;
	return B_OK;
}


static status_t
list_mix_channels(intel_lpe_driver* driver, multi_mix_channel_info* data)
{
	data->actual_count = 0;
	return B_OK;
}


static uint8
gain_to_steps(float gain, float zero, uint8 max)
{
	float steps = zero + gain / VOLUME_STEP + 0.5f;
	// the value comes from user space: NaN fails this test as well
	if (!(steps >= 0))
		return 0;
	if (steps > max)
		return max;
	return (uint8)steps;
}


static status_t
get_mix(intel_lpe_driver* driver, multi_mix_value_info* data)
{
	MutexLocker outputLocker(driver->outputLock);
	for (int32 i = 0; i < data->item_count; i++) {
		multi_mix_value& value = data->values[i];
		switch (value.id) {
			case MIX_OUTPUT_MUTE:
				value.enable = driver->dacMuted;
				break;
			case MIX_OUTPUT_LEFT:
			case MIX_OUTPUT_RIGHT:
				value.gain = ((float)driver->dacVolume[value.id
					- MIX_OUTPUT_LEFT] - DAC_VOLUME_MAX) * VOLUME_STEP;
				break;
			case MIX_INPUT_MUTE:
				value.enable = driver->adcMuted;
				break;
			case MIX_INPUT_LEFT:
			case MIX_INPUT_RIGHT:
				value.gain = ((float)driver->adcLevel[value.id
					- MIX_INPUT_LEFT] - ADC_LEVEL_ZERO_DB) * VOLUME_STEP;
				break;
		}
	}
	return B_OK;
}


static status_t
set_mix(intel_lpe_driver* driver, multi_mix_value_info* data)
{
	MutexLocker outputLocker(driver->outputLock);
	bool playback = false;
	bool record = false;
	for (int32 i = 0; i < data->item_count; i++) {
		const multi_mix_value& value = data->values[i];
		switch (value.id) {
			case MIX_OUTPUT_MUTE:
				driver->dacMuted = value.enable;
				playback = true;
				break;
			case MIX_OUTPUT_LEFT:
			case MIX_OUTPUT_RIGHT:
				driver->dacVolume[value.id - MIX_OUTPUT_LEFT]
					= gain_to_steps(value.gain, DAC_VOLUME_MAX,
						DAC_VOLUME_MAX);
				playback = true;
				break;
			case MIX_INPUT_MUTE:
				driver->adcMuted = value.enable;
				record = true;
				break;
			case MIX_INPUT_LEFT:
			case MIX_INPUT_RIGHT:
				driver->adcLevel[value.id - MIX_INPUT_LEFT]
					= gain_to_steps(value.gain, ADC_LEVEL_ZERO_DB,
						ADC_LEVEL_MAX);
				record = true;
				break;
		}
	}
	if (playback)
		apply_playback_volume(driver);
	if (record)
		apply_record_volume(driver);
	return B_OK;
}


static status_t
describe_ring(uint8* ring, buffer_desc** buffers)
{
	for (int32 i = 0; i < PLAYBACK_BUFFERS; i++) {
		buffer_desc descs[PLAYBACK_CHANNELS];
		memset(descs, 0, sizeof(descs));
		for (int32 channel = 0; channel < PLAYBACK_CHANNELS; channel++) {
			descs[channel].base = (char*)ring
				+ i * PLAYBACK_BUFFER_SIZE + channel * PLAYBACK_SAMPLE_SIZE;
			descs[channel].stride = PLAYBACK_FRAME_SIZE;
		}
		if (!IS_USER_ADDRESS(buffers[i])
			|| user_memcpy(buffers[i], descs, sizeof(descs)) != B_OK) {
			return B_BAD_ADDRESS;
		}
	}
	return B_OK;
}


/*!	The ring layout is fixed, so the requested buffer count and size are
	ignored. Playback and recording use the same layout.
*/
static status_t
get_buffers(intel_lpe_driver* driver, multi_buffer_list* data)
{
	MutexLocker locker(driver->lock);

	if (!driver->running) {
		for (int32 i = 0; i < LPE_STREAM_COUNT; i++)
			memset(driver->ring[i], 0, PLAYBACK_RING_SIZE);
	}

	data->flags = B_MULTI_BUFFER_PLAYBACK | B_MULTI_BUFFER_RECORD;
	data->return_playback_buffers = PLAYBACK_BUFFERS;
	data->return_playback_channels = PLAYBACK_CHANNELS;
	data->return_playback_buffer_size = PLAYBACK_BUFFER_FRAMES;
	data->return_record_buffers = PLAYBACK_BUFFERS;
	data->return_record_channels = PLAYBACK_CHANNELS;
	data->return_record_buffer_size = PLAYBACK_BUFFER_FRAMES;

	if (data->request_playback_buffers < PLAYBACK_BUFFERS
		|| data->request_playback_channels < PLAYBACK_CHANNELS) {
		return B_BAD_VALUE;
	}

	status_t status = describe_ring(driver->ring[LPE_PLAYBACK],
		data->playback_buffers);
	if (status != B_OK)
		return status;

	// the record buffers are optional
	if (data->request_record_buffers < PLAYBACK_BUFFERS
		|| data->request_record_channels < PLAYBACK_CHANNELS) {
		data->flags = B_MULTI_BUFFER_PLAYBACK;
		data->return_record_buffers = 0;
		data->return_record_channels = 0;
		data->return_record_buffer_size = 0;
		return B_OK;
	}
	return describe_ring(driver->ring[LPE_CAPTURE], data->record_buffers);
}


static status_t
buffer_exchange(intel_lpe_driver* driver, multi_buffer_info* data)
{
	multi_buffer_info info;
	if (!IS_USER_ADDRESS(data)
		|| user_memcpy(&info, data, sizeof(info)) != B_OK) {
		return B_BAD_ADDRESS;
	}

	if (!driver->running) {
		status_t status = wait_for_dsp(driver);
		if (status != B_OK)
			return status;
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
	if (driver->capturing) {
		info.record_buffer_cycle = driver->recordCycle;
		info.recorded_real_time = driver->recordTime;
		info.recorded_frames_count = driver->recordedFrames;
	} else {
		info.record_buffer_cycle = -1;
		info.recorded_real_time = 0;
		info.recorded_frames_count = 0;
	}
	release_spinlock(&driver->positionLock);
	restore_interrupts(state);

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


/*!	The ring the media add-on writes into (playback) or reads from
	(recording), and the page table through which the DSP reaches it.
*/
static status_t
create_ring(intel_lpe_driver* driver, uint32 direction)
{
	void* address;
	driver->ringArea[direction] = create_area(direction == LPE_PLAYBACK
			? "intel_lpe playback" : "intel_lpe record", &address,
		B_ANY_KERNEL_ADDRESS, PLAYBACK_RING_SIZE, B_32_BIT_CONTIGUOUS,
		B_READ_AREA | B_WRITE_AREA);
	if (driver->ringArea[direction] < 0)
		return driver->ringArea[direction];
	driver->ring[direction] = (uint8*)address;
	memset(address, 0, PLAYBACK_RING_SIZE);

	driver->tableArea[direction] = create_area("intel_lpe page table",
		&address, B_ANY_KERNEL_ADDRESS, B_PAGE_SIZE, B_32_BIT_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (driver->tableArea[direction] < 0)
		return driver->tableArea[direction];
	memset(address, 0, B_PAGE_SIZE);

	physical_entry entry;
	status_t status = get_memory_map(address, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK)
		return status;
	driver->tablePhysical[direction] = entry.address;

	phys_addr_t pages[PLAYBACK_RING_SIZE / B_PAGE_SIZE];
	status = get_memory_map(driver->ring[direction], PLAYBACK_RING_SIZE,
		&entry, 1);
	if (status != B_OK)
		return status;
	for (size_t i = 0; i < B_COUNT_OF(pages); i++)
		pages[i] = entry.address + i * B_PAGE_SIZE;
	lpe_fill_page_table((uint8*)address, pages, B_COUNT_OF(pages));
	return B_OK;
}


static void
delete_rings(intel_lpe_driver* driver)
{
	for (int32 i = 0; i < LPE_STREAM_COUNT; i++) {
		if (driver->tableArea[i] >= 0)
			delete_area(driver->tableArea[i]);
		if (driver->ringArea[i] >= 0)
			delete_area(driver->ringArea[i]);
	}
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
	// value-initialized: every field starts at zero
	intel_lpe_driver* driver = new(std::nothrow) intel_lpe_driver();
	if (driver == NULL)
		return B_NO_MEMORY;
	driver->node = node;
	driver->pollThread = -1;
	driver->playbackThread = -1;
	for (int32 i = 0; i < LPE_STREAM_COUNT; i++)
		driver->ringArea[i] = driver->tableArea[i] = -1;
	driver->bufferReadySem = driver->positionSem = driver->idleSem = -1;
	driver->dacVolume[0] = driver->dacVolume[1] = DAC_VOLUME_MAX;
	driver->adcLevel[0] = driver->adcLevel[1] = ADC_LEVEL_DEFAULT;
	mutex_init(&driver->lock, "intel_lpe");
	mutex_init(&driver->dspLock, "intel_lpe dsp");
	mutex_init(&driver->outputLock, "intel_lpe output");
	B_INITIALIZE_SPINLOCK(&driver->positionLock);
	driver->dspBooted.Init(driver, "intel_lpe dsp boot");

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
		status = create_ring(driver, LPE_PLAYBACK);
	if (status == B_OK)
		status = create_ring(driver, LPE_CAPTURE);
	if (status == B_OK) {
		driver->bufferReadySem = create_sem(0, "intel_lpe buffer ready");
		if (driver->bufferReadySem < 0)
			status = driver->bufferReadySem;
	}
	if (status == B_OK) {
		driver->positionSem = create_sem(0, "intel_lpe position");
		if (driver->positionSem < 0)
			status = driver->positionSem;
	}
	if (status == B_OK) {
		driver->idleSem = create_sem(0, "intel_lpe idle");
		if (driver->idleSem < 0)
			status = driver->idleSem;
	}
	if (status != B_OK) {
		TRACE_ALWAYS("probe failed: %s\n", strerror(status));
		if (driver->bufferReadySem >= 0)
			delete_sem(driver->bufferReadySem);
		if (driver->positionSem >= 0)
			delete_sem(driver->positionSem);
		delete_rings(driver);
		mutex_destroy(&driver->outputLock);
		mutex_destroy(&driver->dspLock);
		mutex_destroy(&driver->lock);
		delete driver;
		return status;
	}

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
	delete_sem(driver->positionSem);
	delete_sem(driver->idleSem);
	delete_rings(driver);
	mutex_destroy(&driver->outputLock);
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
