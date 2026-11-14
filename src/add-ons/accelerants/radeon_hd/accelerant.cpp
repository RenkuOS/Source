/*
 * Copyright 2006-2011, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 *		Alexander von Gluck, kallisti5@unixzen.com
 */


#include "accelerant.h"

#include <AGP.h>
#include <Debug.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <AutoDeleterOS.h>

#include "accelerant_protos.h"

#include "bios.h"
#include "connector.h"
#include "display.h"
#include "displayport.h"
#include "gpu.h"
#include "pll.h"
#include "utility.h"


#undef TRACE

#define TRACE_ACCELERANT
#ifdef TRACE_ACCELERANT
#	define TRACE(x...) _sPrintf("radeon_hd: " x)
#else
#	define TRACE(x...) ;
#endif

#define ERROR(x...) _sPrintf("radeon_hd: " x)


struct accelerant_info* gInfo;
display_info* gDisplay[MAX_DISPLAY];
connector_info* gConnector[ATOM_MAX_SUPPORTED_DEVICE];
gpio_info* gGPIOInfo[MAX_GPIO_PINS];


//	#pragma mark -


/*! This is the common accelerant_info initializer. It is called by
	both, the first accelerant and all clones.
*/
static status_t
init_common(int device, bool isClone)
{
	// initialize global accelerant info structure

	gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
	MemoryDeleter infoDeleter(gInfo);

	if (gInfo == NULL)
		return B_NO_MEMORY;

	memset(gInfo, 0, sizeof(accelerant_info));

	// malloc memory for active display information
	for (uint32 id = 0; id < MAX_DISPLAY; id++) {
		gDisplay[id] = (display_info*)malloc(sizeof(display_info));
		if (gDisplay[id] == NULL)
			return B_NO_MEMORY;
		memset(gDisplay[id], 0, sizeof(display_info));

		gDisplay[id]->regs = (register_info*)malloc(sizeof(register_info));
		if (gDisplay[id]->regs == NULL)
			return B_NO_MEMORY;
		memset(gDisplay[id]->regs, 0, sizeof(register_info));
	}

	// malloc for possible physical card connectors
	for (uint32 id = 0; id < ATOM_MAX_SUPPORTED_DEVICE; id++) {
		gConnector[id] = (connector_info*)malloc(sizeof(connector_info));

		if (gConnector[id] == NULL)
			return B_NO_MEMORY;
		memset(gConnector[id], 0, sizeof(connector_info));

		// Init a few things
		gConnector[id]->router.ddcValid = false;
		gConnector[id]->router.cdValid = false;
		gConnector[id]->encoder.pll.id = ATOM_PPLL_INVALID;
	}

	// malloc for card gpio pin information
	for (uint32 id = 0; id < MAX_GPIO_PINS; id++) {
		gGPIOInfo[id] = (gpio_info*)malloc(sizeof(gpio_info));

		if (gGPIOInfo[id] == NULL)
			return B_NO_MEMORY;
		memset(gGPIOInfo[id], 0, sizeof(gpio_info));
	}

	gInfo->is_clone = isClone;
	gInfo->device = device;

	gInfo->dpms_mode = B_DPMS_ON;
		// initial state

	// get basic info from driver

	radeon_get_private_data data;
	data.magic = RADEON_PRIVATE_DATA_MAGIC;

	if (ioctl(device, RADEON_GET_PRIVATE_DATA, &data,
			sizeof(radeon_get_private_data)) != 0) {
		return B_ERROR;
	}

	AreaDeleter sharedDeleter(clone_area("radeon hd shared info",
		(void**)&gInfo->shared_info, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA,
		data.shared_info_area));
	status_t status = gInfo->shared_info_area = sharedDeleter.Get();
	if (status < B_OK) {
		TRACE("%s, failed to create shared area\n", __func__);
		return status;
	}

	AreaDeleter regsDeleter(clone_area("radeon hd regs",
		(void**)&gInfo->regs, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA,
		gInfo->shared_info->registers_area));
	status = gInfo->regs_area = regsDeleter.Get();
	if (status < B_OK) {
		TRACE("%s, failed to create mmio area\n", __func__);
		return status;
	}

	// XXX: We *should* only need B_READ_AREA here, but AtomBIOS calls fail with it RO #19348
	gInfo->rom_area = clone_area("radeon hd AtomBIOS",
		(void**)&gInfo->rom, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA,
		gInfo->shared_info->rom_area);

	if (gInfo->rom_area < 0) {
		TRACE("%s: Clone of AtomBIOS failed!\n", __func__);
		gInfo->shared_info->has_rom = false;
	}

	if (gInfo->rom[0] != 0x55 || gInfo->rom[1] != 0xAA)
		TRACE("%s: didn't find a VGA bios in cloned region!\n", __func__);

	infoDeleter.Detach();
	sharedDeleter.Detach();
	regsDeleter.Detach();

	return B_OK;
}


/*! Clean up data common to both primary and cloned accelerant */
static void
uninit_common(void)
{
	if (gInfo != NULL) {
		delete_area(gInfo->regs_area);
		delete_area(gInfo->shared_info_area);
		delete_area(gInfo->rom_area);

		gInfo->regs_area = gInfo->shared_info_area = -1;

		// close the file handle ONLY if we're the clone
		if (gInfo->is_clone)
			close(gInfo->device);

		free(gInfo);
	}

	for (uint32 id = 0; id < MAX_DISPLAY; id++) {
		if (gDisplay[id] != NULL) {
			free(gDisplay[id]->regs);
			free(gDisplay[id]);
		}
	}

	for (uint32 id = 0; id < ATOM_MAX_SUPPORTED_DEVICE; id++)
		free(gConnector[id]);

	for (uint32 id = 0; id < MAX_GPIO_PINS; id++)
		free(gGPIOInfo[id]);
}


//	#pragma mark - public accelerant functions


/*! Init primary accelerant */
status_t
radeon_init_accelerant(int device)
{
	TRACE("%s enter\n", __func__);

	status_t status = init_common(device, false);
	if (status != B_OK)
		return status;

	radeon_shared_info &info = *gInfo->shared_info;

	init_lock(&info.accelerant_lock, "radeon hd accelerant");
	init_lock(&info.engine_lock, "radeon hd engine");

	radeon_init_bios(gInfo->rom);

	// probe firmware information
	radeon_gpu_probe();

	// apply GPU quirks
	radeon_gpu_quirks();

	// find GPIO pins from AtomBIOS
	gpio_populate();

	// find physical card connectors from AtomBIOS
	status = connector_probe();

	if (status != B_OK) {
		TRACE("%s: falling back to legacy connector probe.\n", __func__);
		status = connector_probe_legacy();
	}

	if (status != B_OK) {
		ERROR("%s: couldn't detect supported connectors!\n", __func__);
		radeon_uninit_accelerant();
		return status;
	}

	// print found connectors
	debug_connectors();

	// setup encoders on each connector if needed
	encoder_init();

	// program external pll clock
	pll_external_init();

	// setup link on any DisplayPort connectors
	dp_setup_connectors();

	// detect attached displays
	status = detect_displays();
	if (status != B_OK) {
		radeon_uninit_accelerant();
		return status;
	}

	// print found displays
	debug_displays();

	// create initial list of video modes
	status = create_mode_list();
	if (status != B_OK) {
		radeon_uninit_accelerant();
		return status;
	}

	radeon_gpu_mc_setup();

	// Set up data crunching + irq rings
	radeon_gpu_ring_setup();

	radeon_gpu_ring_boot(RADEON_QUEUE_TYPE_GFX_INDEX);

	radeon_retrace_init();

	TRACE("%s done\n", __func__);
	return B_OK;
}


/*! This function is called for both, the primary accelerant and all of
	its clones.
*/
void
radeon_uninit_accelerant(void)
{
	TRACE("%s enter\n", __func__);

	gInfo->mode_list = NULL;

	radeon_retrace_uninit();

	radeon_shared_info &info = *gInfo->shared_info;

	uninit_lock(&info.accelerant_lock);
	uninit_lock(&info.engine_lock);

	uninit_common();
	TRACE("%s done\n", __func__);
}


status_t
radeon_get_accelerant_device_info(accelerant_device_info* di)
{
	radeon_shared_info &info = *gInfo->shared_info;

	di->version = B_ACCELERANT_VERSION;
	strcpy(di->name, info.deviceName);

	char chipset[32];
	sprintf(chipset, "%s", gInfo->shared_info->chipsetName);
	strcpy(di->chipset, chipset);

	// add flags onto chipset name
	if ((info.chipsetFlags & CHIP_IGP) != 0)
		strcat(di->chipset, " IGP");
	if ((info.chipsetFlags & CHIP_MOBILE) != 0)
		strcat(di->chipset, " Mobile");
	if ((info.chipsetFlags & CHIP_APU) != 0)
		strcat(di->chipset, " APU");

	strcpy(di->serial_no, "None" );

	di->memory = gInfo->shared_info->graphics_memory_size;
	return B_OK;
}


//	#pragma mark - retrace


/*!	Wakes those waiting for the vertical retrace. */
static void
release_retrace_waiters()
{
	int32 count;
	if (get_sem_count(gInfo->retraceSemaphore, &count) == B_OK && count < 0)
		release_sem_etc(gInfo->retraceSemaphore, -count, B_DO_NOT_RESCHEDULE);
}


/*!	The driver gets no vertical blank interrupt, so this follows the CRTC
	instead: it reads where the scanout is, sleeps until just before the
	vertical blank, waits for the blank to begin, and then wakes those
	waiting for the retrace, such as app_server's compositor.
*/
static status_t
retrace_thread(void*)
{
	while (!gInfo->retraceQuit) {
		display_info* display = gDisplay[0];
		if (display == NULL || display->regs == NULL) {
			snooze(16667);
			release_retrace_waiters();
			continue;
		}
		const display_timing& timing = display->currentMode.timing;
		if (!display->attached || !display->powered
			|| timing.pixel_clock == 0 || timing.h_total == 0) {
			// nothing is shown: keep waiters going at about 60 Hz
			snooze(16667);
			release_retrace_waiters();
			continue;
		}

		uint32 offset = display->regs->crtcOffset;
		uint32 vTotal
			= (Read32(CRT, EVERGREEN_CRTC_V_TOTAL + offset) & 0x3fff) + 1;
		uint32 vBlankStart
			= Read32(CRT, EVERGREEN_CRTC_V_BLANK_START_END + offset) & 0x3fff;
		uint32 position
			= Read32(CRT, EVERGREEN_CRTC_STATUS_POSITION + offset) & 0x3fff;

		// pixel_clock is in kHz
		bigtime_t lineTime = (bigtime_t)timing.h_total * 1000
			/ timing.pixel_clock;
		if (lineTime < 1)
			lineTime = 1;
		bigtime_t frameTime = lineTime * vTotal;

		uint32 lines = (vBlankStart + vTotal - position) % vTotal;
		if (lines > 2)
			snooze((lines - 2) * lineTime);

		// Wait for the blank to begin; give up after a frame, should the
		// registers not say what is expected.
		bigtime_t deadline = system_time() + frameTime;
		while ((Read32(CRT, EVERGREEN_CRTC_STATUS + offset) & 1) == 0
			&& system_time() < deadline && !gInfo->retraceQuit) {
			snooze(max_c(lineTime / 2, 20));
		}

		release_retrace_waiters();

		// and for it to end, so that one blank wakes the waiters once
		deadline = system_time() + frameTime;
		while ((Read32(CRT, EVERGREEN_CRTC_STATUS + offset) & 1) != 0
			&& system_time() < deadline && !gInfo->retraceQuit) {
			snooze(max_c(lineTime, 20));
		}
	}
	return B_OK;
}


void
radeon_retrace_init()
{
	gInfo->retraceSemaphore = -1;
	gInfo->retraceThread = -1;
	gInfo->retraceQuit = false;

	// The CRTC registers used are where they are from Evergreen (DCE 4) on.
	if (gInfo->shared_info->dceMajor < 4)
		return;

	gInfo->retraceSemaphore = create_sem(0, "radeon hd retrace");
	if (gInfo->retraceSemaphore < 0)
		return;

	gInfo->retraceThread = spawn_thread(&retrace_thread, "radeon hd retrace",
		B_REAL_TIME_DISPLAY_PRIORITY, NULL);
	if (gInfo->retraceThread < 0) {
		delete_sem(gInfo->retraceSemaphore);
		gInfo->retraceSemaphore = -1;
		return;
	}
	resume_thread(gInfo->retraceThread);
}


void
radeon_retrace_uninit()
{
	if (gInfo->retraceThread >= 0) {
		gInfo->retraceQuit = true;
		status_t result;
		wait_for_thread(gInfo->retraceThread, &result);
		gInfo->retraceThread = -1;
	}
	if (gInfo->retraceSemaphore >= 0) {
		delete_sem(gInfo->retraceSemaphore);
		gInfo->retraceSemaphore = -1;
	}
}


sem_id
radeon_accelerant_retrace_semaphore()
{
	return gInfo->retraceSemaphore;
}
