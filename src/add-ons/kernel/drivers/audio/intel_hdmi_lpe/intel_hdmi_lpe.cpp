/*
 * Copyright 2026, Akakor, 197073812+12akakor@users.noreply.github.com.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


//!	HDMI audio of Intel Bay Trail (Valleyview) graphics.
// Bay Trail sends HDMI audio from the display engine itself: an "HDMI LPE
// audio" block in the graphics registers fetches 16 bit stereo samples from
// four buffer descriptors in system memory and packs them into the HDMI
// signal of the pipe the port runs on. This driver publishes that as a
// playback-only multi_audio device.
//
// intel_extreme cannot set up HDMI on Valleyview yet (no DPLL and PHY
// programming), so the driver uses the output the firmware set up: it looks
// for an enabled HDMI port in HDMI (not DVI) mode, reads its pixel clock
// from the DPLL, and only turns the audio parts on. Like amd_acp it maps the
// registers of the graphics device itself and polls the buffer descriptors,
// as the audio interrupt arrives through the display interrupt, which
// intel_extreme does not forward.
//
// Register layout from the Intel Valleyview display documentation as Linux
// uses it (i915 intel_lpe_audio.c and the VLV register headers, MIT); the
// audio block sequence follows the description of the Linux
// sound/x86/intel_hdmi_audio.c driver, without its code.


#include <KernelExport.h>
#include <Drivers.h>
#include <PCI.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include <hmulti_audio.h>

#include <new>

#include <stdio.h>
#include <string.h>


#define DRIVER_NAME			"intel_hdmi_lpe"
#define DEVICE_PATH			"audio/hmulti/intel_hdmi/1"

#define TRACE_ALWAYS(x...)	dprintf(DRIVER_NAME ": " x)


#define PCI_VENDOR_INTEL			0x8086
#define PCI_DEVICE_VALLEYVIEW_GPU	0x0f31

// The display registers of Valleyview start at 0x180000 in BAR 0; the
// window ends after the HDMI LPE audio block.
#define DISPLAY_BASE				0x180000
#define DISPLAY_WINDOW_SIZE			0x66000

// IOSF sideband, to read the DPLL dividers from the DPIO PHY
#define IOSF_DOORBELL				0x2100
#define IOSF_DATA					0x2104
#define IOSF_ADDRESS				0x2108
#define IOSF_BUSY					(1 << 0)
#define IOSF_BYTE_ENABLES			(0xf << 4)
#define IOSF_PORT_DPIO				(0x12 << 8)
#define IOSF_OPCODE_MEMORY_READ		(0x00 << 16)
#define DPIO_PLL_DW3(pipe)			(0x8000 + (pipe) * 0x20 + 3 * 4)
#define DPIO_REFERENCE_KHZ			100000

#define DPLL(pipe)					(0x6014 + (pipe) * 4)
#define DPLL_VCO_ENABLE				(1u << 31)

#define HDMI_PORT_B					0x61140
#define HDMI_PORT_C					0x61160
#define HDMI_PORT_ENABLE			(1u << 31)
#define HDMI_PORT_PIPE_SHIFT		30
#define HDMI_MODE_SELECT_HDMI		(1 << 9)
#define HDMI_AUDIO_ENABLE			(1 << 6)
#define VIDEO_DIP_CTL(pipe)			((pipe) == 0 ? 0x60200 : 0x61170)

#define AUD_CHICKEN_BIT				0x62f38
#define AUD_CHICKEN_BIT_DBG_ENABLE	(1 << 0)
#define AUD_PORT_EN_B_DBG			0x62f20
#define AUD_PORT_EN_C_DBG			0x62f30
#define AUD_AMP_MUTE				(1 << 1)

// HDMI LPE audio block, one set of registers per pipe
#define LPE_AUDIO(pipe)				(0x65000 + (pipe) * 0x800)
#define AUD_CONFIG					0x00
#define AUD_CONFIG_ENABLE			(1 << 0)
#define AUD_CONFIG_VALID_BIT		(1 << 9)
#define AUD_CONFIG_16BIT_PACKETS	(1 << 12)
#define AUD_CH_STATUS_0				0x08
#define AUD_CH_STATUS_RATE_SHIFT	24
#define AUD_CH_STATUS_1				0x0c
#define AUD_CH_STATUS_16BIT_WORDS	(1 << 1)
#define AUD_HDMI_CTS				0x10
#define AUD_N_ENABLE				0x14
#define AUD_PROGRAM_VALUE			(1 << 24)
#define AUD_BUF_CONFIG				0x20
#define AUD_BUF_CONFIG_WATERMARKS	((7 << 8) | 0xfe)
#define AUD_BUF_CH_SWAP				0x24
#define AUD_BUF_CH_SWAP_IDENTITY	0x00fac4c8
#define AUD_BUF_ADDR(index)			(0x40 + (index) * 8)
#define AUD_BUF_VALID				(1 << 0)
#define AUD_BUF_LENGTH(index)		(0x44 + (index) * 8)
#define AUD_BUF_COUNT				4
#define AUD_CNTL_ST					0x60
#define AUD_CNTL_ST_DIP_EVERY_FRAME	(1 << 16)
#define AUD_CNTL_ST_DIP_ENABLE		(1 << 21)
#define AUD_HDMI_STATUS				0x64
#define AUD_HDMI_STATUS_RESET		(1 << 0)
#define AUD_HDMI_STATUS_EVENTS		(7u << 29)
#define AUD_HDMIW_INFOFR			0x68
#define AUD_INFOFRAME_WORDS			16
// HDMI audio infoframe header: type 0x84, version 1, length 10
#define AUD_INFOFRAME_HEADER		0x000a0184

#define PLAYBACK_CHANNELS			2
#define PLAYBACK_SAMPLE_SIZE		2
#define PLAYBACK_FRAME_SIZE			(PLAYBACK_CHANNELS * PLAYBACK_SAMPLE_SIZE)
#define PLAYBACK_BUFFERS			2
#define PLAYBACK_BUFFER_FRAMES		1024
#define PLAYBACK_BUFFER_SIZE		(PLAYBACK_BUFFER_FRAMES * PLAYBACK_FRAME_SIZE)
#define PLAYBACK_RING_SIZE			(PLAYBACK_BUFFERS * PLAYBACK_BUFFER_SIZE)
#define PLAYBACK_BUFFER_TIME(rate) \
	((bigtime_t)PLAYBACK_BUFFER_FRAMES * 1000000 / (rate))

// The poll thread sleeps most of a buffer after a boundary, then checks
// every POLL_INTERVAL.
#define POLL_INTERVAL				500
#define POLL_EARLY_WAKEUP			3000
#define EXCHANGE_TIMEOUT			500000


// N from HDMI 1.4 table 7-1 for TMDS clocks without a special case, and the
// IEC 60958 sample frequency code.
static const struct {
	uint32	multiRate;
	uint32	rate;
	uint32	n;
	uint32	statusCode;
} kRates[] = {
	{ B_SR_32000, 32000, 4096, 0x3 },
	{ B_SR_44100, 44100, 6272, 0x0 },
	{ B_SR_48000, 48000, 6144, 0x2 },
	{ B_SR_96000, 96000, 12288, 0xa },
};


struct hdmi_lpe_driver {
	pci_info		gpu;
	area_id			displayArea;
	addr_t			display;
	area_id			bufferArea;
	uint8*			buffer;
	phys_addr_t		bufferPhysical;

	// Protects the fields below and the start and stop of playback.
	mutex			lock;
	int32			openCount;
	int32			running;
	thread_id		pollThread;
	sem_id			bufferReadySem;
	uint32			rate;
	uint32			pipe;
	uint32			portRegister;
	uint32			tmdsClock;

	// Playback position, written by the poll thread.
	spinlock		positionLock;
	uint32			bufferCycle;
	int64			framesCount;
	bigtime_t		realTime;
};


int32 api_version = B_CUR_DRIVER_API_VERSION;

static pci_module_info* sPCI;
static hdmi_lpe_driver* sDriver;

static multi_channel_info sChannels[] = {
	{ 0, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0 },
	{ 1, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0 },
	{ 2, B_MULTI_OUTPUT_BUS, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0 },
	{ 3, B_MULTI_OUTPUT_BUS, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0 },
};


static inline uint32
display_read(hdmi_lpe_driver* driver, uint32 reg)
{
	return *(volatile uint32*)(driver->display + reg);
}


static inline void
display_write(hdmi_lpe_driver* driver, uint32 reg, uint32 value)
{
	*(volatile uint32*)(driver->display + reg) = value;
}


static inline void
audio_write(hdmi_lpe_driver* driver, uint32 reg, uint32 value)
{
	display_write(driver, LPE_AUDIO(driver->pipe) + reg, value);
}


static inline uint32
audio_read(hdmi_lpe_driver* driver, uint32 reg)
{
	return display_read(driver, LPE_AUDIO(driver->pipe) + reg);
}


//	#pragma mark - HDMI output


static status_t
iosf_wait(hdmi_lpe_driver* driver)
{
	for (int32 i = 0; i < 1000; i++) {
		if ((display_read(driver, IOSF_DOORBELL) & IOSF_BUSY) == 0)
			return B_OK;
		spin(10);
	}
	return B_TIMED_OUT;
}


static status_t
dpio_read(hdmi_lpe_driver* driver, uint32 address, uint32* _value)
{
	if (iosf_wait(driver) != B_OK)
		return B_BUSY;
	display_write(driver, IOSF_ADDRESS, address);
	display_write(driver, IOSF_DATA, 0);
	display_write(driver, IOSF_DOORBELL, IOSF_OPCODE_MEMORY_READ
		| IOSF_PORT_DPIO | IOSF_BYTE_ENABLES | IOSF_BUSY);
	if (iosf_wait(driver) != B_OK)
		return B_TIMED_OUT;
	*_value = display_read(driver, IOSF_DATA);
	return B_OK;
}


/*!	The pixel clock of \a pipe in kHz, from the DPLL dividers in the DPIO
	PHY, as Linux's vlv_crtc_clock_get() computes it. With 8 bits per
	color, it is the TMDS clock.
*/
static uint32
pipe_clock(hdmi_lpe_driver* driver, uint32 pipe)
{
	if ((display_read(driver, DPLL(pipe)) & DPLL_VCO_ENABLE) == 0)
		return 0;

	uint32 dividers;
	if (dpio_read(driver, DPIO_PLL_DW3(pipe), &dividers) != B_OK)
		return 0;

	uint32 m = ((dividers >> 8) & 0x7) * (dividers & 0xff);
	uint32 n = (dividers >> 12) & 0xf;
	uint32 p = ((dividers >> 21) & 0x7) * ((dividers >> 16) & 0x1f) * 5;
	if (n == 0 || p == 0)
		return 0;

	uint32 vco = ((uint64)DPIO_REFERENCE_KHZ * m + n / 2) / n;
	return (vco + p / 2) / p;
}


/*!	Finds the HDMI port the firmware enabled, its pipe and TMDS clock.
	Audio needs the port in HDMI mode: a DVI signal has no room for it.
*/
static status_t
find_output(hdmi_lpe_driver* driver, bool log)
{
	static const uint32 kPorts[] = { HDMI_PORT_B, HDMI_PORT_C };

	for (size_t i = 0; i < B_COUNT_OF(kPorts); i++) {
		uint32 port = display_read(driver, kPorts[i]);
		uint32 pipe = (port >> HDMI_PORT_PIPE_SHIFT) & 1;
		if (log) {
			TRACE_ALWAYS("HDMI port %c: %#" B_PRIx32 ", DPLL %c %#" B_PRIx32
				", DIP %#" B_PRIx32 "\n", 'B' + (char)i, port, 'A' + (char)pipe,
				display_read(driver, DPLL(pipe)),
				display_read(driver, VIDEO_DIP_CTL(pipe)));
		}
		if ((port & HDMI_PORT_ENABLE) == 0)
			continue;
		if ((port & HDMI_MODE_SELECT_HDMI) == 0) {
			TRACE_ALWAYS("HDMI port %c runs in DVI mode, no audio\n",
				'B' + (char)i);
			continue;
		}

		uint32 clock = pipe_clock(driver, pipe);
		if (clock < 25000 || clock > 300000) {
			TRACE_ALWAYS("HDMI port %c: no usable pixel clock (%" B_PRIu32
				" kHz)\n", 'B' + (char)i, clock);
			continue;
		}

		driver->portRegister = kPorts[i];
		driver->pipe = pipe;
		driver->tmdsClock = clock;
		if (log) {
			TRACE_ALWAYS("HDMI port %c on pipe %c, TMDS clock %" B_PRIu32
				" kHz\n", 'B' + (char)i, 'A' + (char)pipe, clock);
		}
		return B_OK;
	}
	return ENODEV;
}


static void
enable_output_audio(hdmi_lpe_driver* driver)
{
	uint32 port = display_read(driver, driver->portRegister);
	if ((port & HDMI_AUDIO_ENABLE) == 0)
		display_write(driver, driver->portRegister, port | HDMI_AUDIO_ENABLE);

	display_write(driver, AUD_CHICKEN_BIT,
		display_read(driver, AUD_CHICKEN_BIT) | AUD_CHICKEN_BIT_DBG_ENABLE);

	uint32 amp = driver->portRegister == HDMI_PORT_B
		? AUD_PORT_EN_B_DBG : AUD_PORT_EN_C_DBG;
	display_write(driver, amp, display_read(driver, amp) & ~AUD_AMP_MUTE);
}


//	#pragma mark - playback


static void
reset_audio(hdmi_lpe_driver* driver)
{
	audio_write(driver, AUD_CONFIG, 0);
	audio_write(driver, AUD_HDMI_STATUS, AUD_HDMI_STATUS_RESET);
	audio_write(driver, AUD_HDMI_STATUS, 0);
	// the event bits may need more than one write to clear
	for (int32 i = 0; i < 100; i++) {
		uint32 status = audio_read(driver, AUD_HDMI_STATUS);
		if ((status & AUD_HDMI_STATUS_EVENTS) == 0)
			break;
		audio_write(driver, AUD_HDMI_STATUS, status);
		spin(100);
	}
}


static void
arm_buffer(hdmi_lpe_driver* driver, uint32 index)
{
	audio_write(driver, AUD_BUF_ADDR(index), (uint32)(driver->bufferPhysical
		+ index * PLAYBACK_BUFFER_SIZE) | AUD_BUF_VALID);
	audio_write(driver, AUD_BUF_LENGTH(index), PLAYBACK_BUFFER_SIZE);
}


static void
setup_audio(hdmi_lpe_driver* driver)
{
	size_t rateIndex = 0;
	for (size_t i = 0; i < B_COUNT_OF(kRates); i++) {
		if (kRates[i].rate == driver->rate)
			rateIndex = i;
	}

	// Audio clock regeneration: CTS = f_TMDS * N / (128 * f_s)
	uint32 n = kRates[rateIndex].n;
	uint32 cts = (uint64)driver->tmdsClock * 1000 * n / (128 * driver->rate);
	audio_write(driver, AUD_N_ENABLE, AUD_PROGRAM_VALUE | n);
	audio_write(driver, AUD_HDMI_CTS, AUD_PROGRAM_VALUE | cts);

	// Audio infoframe: 2 channels, everything else "refer to stream header",
	// channel allocation 0 (front left and right).
	uint32 infoFrame[3] = { AUD_INFOFRAME_HEADER, (PLAYBACK_CHANNELS - 1) << 8,
		0 };
	uint8 sum = 0;
	for (int32 i = 0; i < 3; i++) {
		for (int32 byte = 0; byte < 4; byte++)
			sum += infoFrame[i] >> (byte * 8);
	}
	infoFrame[1] |= (uint8)-sum;

	audio_write(driver, AUD_CNTL_ST, 0);
	for (int32 i = 0; i < AUD_INFOFRAME_WORDS; i++)
		audio_write(driver, AUD_HDMIW_INFOFR, i < 3 ? infoFrame[i] : 0);
	audio_write(driver, AUD_CNTL_ST,
		AUD_CNTL_ST_DIP_EVERY_FRAME | AUD_CNTL_ST_DIP_ENABLE);

	audio_write(driver, AUD_CH_STATUS_0,
		kRates[rateIndex].statusCode << AUD_CH_STATUS_RATE_SHIFT);
	audio_write(driver, AUD_CH_STATUS_1, AUD_CH_STATUS_16BIT_WORDS);
	audio_write(driver, AUD_BUF_CONFIG, AUD_BUF_CONFIG_WATERMARKS);
	audio_write(driver, AUD_BUF_CH_SWAP, AUD_BUF_CH_SWAP_IDENTITY);

	for (uint32 i = 0; i < AUD_BUF_COUNT; i++) {
		if (i < PLAYBACK_BUFFERS)
			arm_buffer(driver, i);
		else {
			audio_write(driver, AUD_BUF_ADDR(i), 0);
			audio_write(driver, AUD_BUF_LENGTH(i), 0);
		}
	}

	TRACE_ALWAYS("playback: %" B_PRIu32 " Hz, N %" B_PRIu32 ", CTS %" B_PRIu32
		"\n", driver->rate, n, cts);
}


/*!	The engine counts down the length of the buffer it plays; at 0 it moves
	on to the next valid one. A finished buffer is the one buffer_exchange()
	hands to the media add-on to fill, and it is armed again at once.
*/
static int32
playback_poll_thread(void* data)
{
	hdmi_lpe_driver* driver = (hdmi_lpe_driver*)data;
	uint32 playing = 0;

	while (atomic_get(&driver->running) != 0) {
		if (audio_read(driver, AUD_BUF_LENGTH(playing)) != 0) {
			snooze(POLL_INTERVAL);
			continue;
		}

		arm_buffer(driver, playing);
		playing = (playing + 1) % PLAYBACK_BUFFERS;
		bigtime_t now = system_time();

		cpu_status state = disable_interrupts();
		acquire_spinlock(&driver->positionLock);
		driver->bufferCycle = playing;
		driver->framesCount += PLAYBACK_BUFFER_FRAMES;
		driver->realTime = now;
		release_spinlock(&driver->positionLock);
		restore_interrupts(state);

		release_sem_etc(driver->bufferReadySem, 1, B_DO_NOT_RESCHEDULE);
		snooze(PLAYBACK_BUFFER_TIME(driver->rate) - POLL_EARLY_WAKEUP);
	}
	return 0;
}


static status_t
playback_start(hdmi_lpe_driver* driver)
{
	// the monitor or its mode may have changed since the last start
	status_t status = find_output(driver, false);
	if (status != B_OK) {
		TRACE_ALWAYS("playback: no HDMI output in HDMI mode is active\n");
		return status;
	}

	int32 count;
	if (get_sem_count(driver->bufferReadySem, &count) == B_OK && count > 0)
		acquire_sem_etc(driver->bufferReadySem, count, B_RELATIVE_TIMEOUT, 0);

	enable_output_audio(driver);
	reset_audio(driver);
	setup_audio(driver);

	driver->bufferCycle = 0;
	driver->framesCount = 0;
	driver->realTime = system_time();

	// the silicon bug of AUD_CONFIG (bit 6 reads back as 0) does not matter
	// here: it is only ever written as a whole
	audio_write(driver, AUD_HDMI_STATUS, AUD_HDMI_STATUS_EVENTS);
	audio_write(driver, AUD_CONFIG, AUD_CONFIG_16BIT_PACKETS
		| AUD_CONFIG_VALID_BIT | AUD_CONFIG_ENABLE);

	driver->running = 1;
	driver->pollThread = spawn_kernel_thread(playback_poll_thread,
		"intel_hdmi_lpe playback", B_REAL_TIME_DISPLAY_PRIORITY, driver);
	if (driver->pollThread < 0) {
		status = driver->pollThread;
		driver->running = 0;
		reset_audio(driver);
		return status;
	}
	resume_thread(driver->pollThread);
	return B_OK;
}


static void
playback_stop(hdmi_lpe_driver* driver)
{
	if (!driver->running)
		return;

	atomic_set(&driver->running, 0);
	status_t result;
	wait_for_thread(driver->pollThread, &result);
	driver->pollThread = -1;

	uint32 status = audio_read(driver, AUD_HDMI_STATUS);
	reset_audio(driver);

	// Wake a buffer_exchange() that is still waiting.
	release_sem_etc(driver->bufferReadySem, 1, 0);

	TRACE_ALWAYS("playback: stopped after %" B_PRId64 " frames, status %#"
		B_PRIx32 "\n", driver->framesCount, status);
}


//	#pragma mark - multi_audio


static status_t
get_description(hdmi_lpe_driver* driver, multi_description* data)
{
	data->interface_version = B_CURRENT_INTERFACE_VERSION;
	data->interface_minimum = B_CURRENT_INTERFACE_VERSION;

	strlcpy(data->friendly_name, "HDMI (Intel LPE)",
		sizeof(data->friendly_name));
	strlcpy(data->vendor_info, "Renku", sizeof(data->vendor_info));

	data->output_channel_count = PLAYBACK_CHANNELS;
	data->output_bus_channel_count = PLAYBACK_CHANNELS;
	data->input_channel_count = 0;
	data->input_bus_channel_count = 0;
	data->aux_bus_channel_count = 0;

	if (data->request_channel_count >= (int32)B_COUNT_OF(sChannels))
		memcpy(data->channels, sChannels, sizeof(sChannels));

	data->output_rates = 0;
	for (size_t i = 0; i < B_COUNT_OF(kRates); i++)
		data->output_rates |= kRates[i].multiRate;
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
get_enabled_channels(hdmi_lpe_driver* driver, multi_channel_enable* data)
{
	data->lock_source = B_MULTI_LOCK_INTERNAL;
	for (int32 i = 0; i < PLAYBACK_CHANNELS; i++)
		B_SET_CHANNEL(data->enable_bits, i, true);
	return B_OK;
}


static status_t
get_global_format(hdmi_lpe_driver* driver, multi_format_info* data)
{
	data->output_latency = 0;
	data->input_latency = 0;
	data->timecode_kind = 0;
	data->output.format = B_FMT_16BIT;
	for (size_t i = 0; i < B_COUNT_OF(kRates); i++) {
		if (kRates[i].rate == driver->rate)
			data->output.rate = kRates[i].multiRate;
	}
	data->input.format = 0;
	data->input.rate = 0;
	return B_OK;
}


/*!	A running stream stops; the next buffer exchange starts it again at the
	new rate.
*/
static status_t
set_global_format(hdmi_lpe_driver* driver, multi_format_info* data)
{
	uint32 rate = 0;
	for (size_t i = 0; i < B_COUNT_OF(kRates); i++) {
		if (kRates[i].multiRate == data->output.rate)
			rate = kRates[i].rate;
	}
	if (rate == 0)
		return B_BAD_VALUE;

	MutexLocker locker(driver->lock);
	if (rate != driver->rate) {
		playback_stop(driver);
		driver->rate = rate;
	}
	locker.Unlock();
	return get_global_format(driver, data);
}


static status_t
list_mix_controls(hdmi_lpe_driver* driver, multi_mix_control_info* data)
{
	// HDMI has no gain control; the system mixer and the monitor set the
	// volume.
	data->control_count = 0;
	return B_OK;
}


static status_t
list_mix_connections(hdmi_lpe_driver* driver,
	multi_mix_connection_info* data)
{
	return B_ERROR;
}


static status_t
list_mix_channels(hdmi_lpe_driver* driver, multi_mix_channel_info* data)
{
	return B_ERROR;
}


static status_t
get_mix(hdmi_lpe_driver* driver, multi_mix_value_info* data)
{
	return B_OK;
}


static status_t
set_mix(hdmi_lpe_driver* driver, multi_mix_value_info* data)
{
	return B_OK;
}


/*!	The ring layout is fixed, so the requested buffer count and size are
	ignored.
*/
static status_t
get_buffers(hdmi_lpe_driver* driver, multi_buffer_list* data)
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
		memset(descs, 0, sizeof(descs));
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
buffer_exchange(hdmi_lpe_driver* driver, multi_buffer_info* data)
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
buffer_force_stop(hdmi_lpe_driver* driver)
{
	MutexLocker locker(driver->lock);
	playback_stop(driver);
	return B_OK;
}


#define cookie_type hdmi_lpe_driver
#include "../generic/multi.c"


//	#pragma mark - device hooks


static status_t
hdmi_lpe_open(const char* name, uint32 flags, void** _cookie)
{
	MutexLocker locker(sDriver->lock);
	sDriver->openCount++;
	*_cookie = sDriver;
	return B_OK;
}


static status_t
hdmi_lpe_close(void* cookie)
{
	return B_OK;
}


static status_t
hdmi_lpe_free(void* cookie)
{
	hdmi_lpe_driver* driver = (hdmi_lpe_driver*)cookie;
	MutexLocker locker(driver->lock);
	if (--driver->openCount == 0)
		playback_stop(driver);
	return B_OK;
}


static status_t
hdmi_lpe_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	return multi_audio_control_generic((hdmi_lpe_driver*)cookie, op, buffer,
		length);
}


static status_t
hdmi_lpe_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
hdmi_lpe_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static device_hooks sDeviceHooks = {
	hdmi_lpe_open,
	hdmi_lpe_close,
	hdmi_lpe_free,
	hdmi_lpe_control,
	hdmi_lpe_read,
	hdmi_lpe_write
};


//	#pragma mark - driver hooks


static bool
find_gpu(pci_module_info* pci, pci_info* info)
{
	for (int32 i = 0; pci->get_nth_pci_info(i, info) == B_OK; i++) {
		if (info->vendor_id == PCI_VENDOR_INTEL
			&& info->device_id == PCI_DEVICE_VALLEYVIEW_GPU) {
			return true;
		}
	}
	return false;
}


static void
free_driver(hdmi_lpe_driver* driver)
{
	if (driver->bufferReadySem >= 0)
		delete_sem(driver->bufferReadySem);
	if (driver->bufferArea >= 0)
		delete_area(driver->bufferArea);
	if (driver->displayArea >= 0)
		delete_area(driver->displayArea);
	mutex_destroy(&driver->lock);
	delete driver;
}


static status_t
setup_driver(hdmi_lpe_driver* driver)
{
	if (!find_gpu(sPCI, &driver->gpu))
		return ENODEV;

	phys_addr_t bar = driver->gpu.u.h0.base_registers[0];
	if (bar == 0 || driver->gpu.u.h0.base_register_sizes[0]
			< DISPLAY_BASE + DISPLAY_WINDOW_SIZE) {
		TRACE_ALWAYS("graphics register BAR too small\n");
		return ENODEV;
	}

	void* address;
	driver->displayArea = map_physical_memory("intel_hdmi_lpe registers",
		bar + DISPLAY_BASE, DISPLAY_WINDOW_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (driver->displayArea < 0)
		return driver->displayArea;
	driver->display = (addr_t)address;

	status_t status = find_output(driver, true);
	if (status != B_OK) {
		TRACE_ALWAYS("no HDMI output in HDMI mode is active\n");
		return status;
	}

	// The buffer addresses have 32 bits; the engine reads them without
	// snooping the CPU caches.
	driver->bufferArea = create_area("intel_hdmi_lpe playback", &address,
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

	status = vm_set_area_memory_type(driver->bufferArea,
		driver->bufferPhysical, B_UNCACHED_MEMORY);
	if (status != B_OK)
		return status;
	memset(driver->buffer, 0, PLAYBACK_RING_SIZE);

	driver->bufferReadySem = create_sem(0, "intel_hdmi_lpe buffer ready");
	if (driver->bufferReadySem < 0)
		return driver->bufferReadySem;
	return B_OK;
}


extern "C" status_t
init_hardware(void)
{
	pci_module_info* pci;
	if (get_module(B_PCI_MODULE_NAME, (module_info**)&pci) != B_OK)
		return ENODEV;

	pci_info info;
	bool found = find_gpu(pci, &info);
	put_module(B_PCI_MODULE_NAME);
	return found ? B_OK : ENODEV;
}


extern "C" status_t
init_driver(void)
{
	if (get_module(B_PCI_MODULE_NAME, (module_info**)&sPCI) != B_OK)
		return ENODEV;

	hdmi_lpe_driver* driver = new(std::nothrow) hdmi_lpe_driver;
	if (driver == NULL) {
		put_module(B_PCI_MODULE_NAME);
		return B_NO_MEMORY;
	}
	memset(driver, 0, sizeof(*driver));
	driver->displayArea = driver->bufferArea = -1;
	driver->bufferReadySem = -1;
	driver->pollThread = -1;
	driver->rate = 48000;
	mutex_init(&driver->lock, DRIVER_NAME);
	B_INITIALIZE_SPINLOCK(&driver->positionLock);

	status_t status = setup_driver(driver);
	if (status != B_OK) {
		free_driver(driver);
		put_module(B_PCI_MODULE_NAME);
		return status;
	}

	sDriver = driver;
	return B_OK;
}


extern "C" void
uninit_driver(void)
{
	mutex_lock(&sDriver->lock);
	playback_stop(sDriver);
	mutex_unlock(&sDriver->lock);

	free_driver(sDriver);
	sDriver = NULL;
	put_module(B_PCI_MODULE_NAME);
}


extern "C" const char**
publish_devices(void)
{
	static const char* devices[] = { DEVICE_PATH, NULL };
	return devices;
}


extern "C" device_hooks*
find_device(const char* name)
{
	return &sDeviceHooks;
}
