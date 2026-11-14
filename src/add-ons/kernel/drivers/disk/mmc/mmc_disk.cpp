/*
 * Copyright 2018-2021 Haiku, Inc. All rights reserved.
 * Copyright 2020, Viveris Technologies.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		B Krishnan Iyer, krishnaniyer97@gmail.com
 *		Adrien Destugues, pulkomandy@pulkomandy.tk
 */

#include <new>

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "mmc_disk.h"
#include "mmc_icon.h"
#include "mmc.h"

#include <drivers/device_manager.h>
#include <drivers/KernelExport.h>
#include <drivers/Drivers.h>
#include <drivers/driver_settings.h>
#include <kernel/OS.h>
#include <util/fs_trim_support.h>

#include <AutoDeleter.h>


// Verbose tracing, including one line per read and write call. That also
// fills the syslog fast enough to push the boot messages out of it.
//#define TRACE_MMC_DISK
#ifdef TRACE_MMC_DISK
#	define TRACE(x...) dprintf("\33[33mmmc_disk:\33[0m " x)
#else
#	define TRACE(x...) do {} while (false)
		// Not just ";": "if (...) TRACE(...); else ..." must still compile
#endif
#define TRACE_ALWAYS(x...)	dprintf("\33[33mmmc_disk:\33[0m " x)
#define ERROR(x...)			dprintf("\33[33mmmc_disk:\33[0m " x)
#define CALLED() 			TRACE("CALLED %s\n", __PRETTY_FUNCTION__)

#define MMC_DISK_DRIVER_MODULE_NAME "drivers/disk/mmc/mmc_disk/driver_v1"
#define MMC_DISK_DEVICE_MODULE_NAME "drivers/disk/mmc/mmc_disk/device_v1"
#define MMC_DEVICE_ID_GENERATOR "mmc/device_id"


static const uint32 kBlockSize = 512; // FIXME get it from the CSD

static device_manager_info* sDeviceManager;


struct mmc_disk_csd {
	// The content of this register is described in Physical Layer Simplified
	// Specification Version 8.00, section 5.3
	uint64 bits[2];

	uint8 structure_version() { return bits[1] >> 54; }
	uint8 read_bl_len() { return (bits[1] >> 8) & 0xF; }
	uint32 c_size()
	{
		if (structure_version() == 0)
			return ((bits[0] >> 54) & 0x3FF) | ((bits[1] & 0x3) << 10);
		if (structure_version() == 1)
			return (bits[0] >> 40) & 0x3FFFFF;
		return ((bits[0] >> 40) & 0xFFFFFF) | ((bits[1] & 0xF) << 24);
	}

	uint8 c_size_mult()
	{
		if (structure_version() == 0)
			return (bits[0] >> 39) & 0x7;
		// In later versions this field is not present in the structure and a
		// fixed value is used.
		return 8;
	}

	uint64 mmc_capacity()
	{
		// MMC devices always use the CSD 1.0 layout for these fields,
		// whatever their CSD_STRUCTURE says (JEDEC 84-B51, 7.3). For devices
		// larger than 2GB, C_SIZE is 0xFFF and the real size is in EXT_CSD.
		uint64 size = ((bits[0] >> 54) & 0x3FF) | ((bits[1] & 0x3) << 10);
		uint32 mult = (bits[0] >> 39) & 0x7;
		return (size + 1) << (mult + 2 + read_bl_len());
	}
};


// EXT_CSD byte offsets, JEDEC 84-B51 section 7.4
enum {
	kExtCsdFlushCache = 32,
	kExtCsdCacheControl = 33,
	kExtCsdPartitionConfig = 179,
	kExtCsdBusWidth = 183,
	kExtCsdHsTiming = 185,
	kExtCsdRevision = 192,
	kExtCsdStructure = 194,
	kExtCsdDeviceType = 196,
	kExtCsdSecCount = 212,		// 4 bytes, little endian
	kExtCsdHcWpGroupSize = 221,
	kExtCsdHcEraseGroupSize = 224,
	kExtCsdBootSizeMult = 226,
	kExtCsdCacheSize = 249,		// 4 bytes, little endian, in KiB
	kExtCsdSize = 512
};


static uint32
ext_csd_sector_count(const uint8* extCsd)
{
	return (uint32)extCsd[kExtCsdSecCount]
		| ((uint32)extCsd[kExtCsdSecCount + 1] << 8)
		| ((uint32)extCsd[kExtCsdSecCount + 2] << 16)
		| ((uint32)extCsd[kExtCsdSecCount + 3] << 24);
}


/*!	Compares EXT_CSD fields that never change. Used to check that data
	transfers still work after changing the bus width, the same way Linux
	does when the device does not support the bus test commands.
*/
static bool
ext_csd_same_device(const uint8* a, const uint8* b)
{
	static const int kFields[] = { kExtCsdRevision, kExtCsdStructure,
		kExtCsdDeviceType, kExtCsdSecCount, kExtCsdSecCount + 1,
		kExtCsdSecCount + 2, kExtCsdSecCount + 3, kExtCsdHcWpGroupSize,
		kExtCsdHcEraseGroupSize, kExtCsdBootSizeMult };
	for (size_t i = 0; i < sizeof(kFields) / sizeof(kFields[0]); i++) {
		if (a[kFields[i]] != b[kFields[i]])
			return false;
	}
	return true;
}


static float
mmc_disk_supports_device(device_node* parent)
{
	// Filter all devices that are not on an MMC bus
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus,
			true) != B_OK)
		return -1;

	if (strcmp(bus, "mmc") != 0)
		return 0.0;

	CALLED();

	// Filter all devices that are not of the known types
	uint8_t deviceType;
	if (sDeviceManager->get_attr_uint8(parent, kMmcTypeAttribute,
			&deviceType, true) != B_OK)
	{
		ERROR("Could not get device type\n");
		return -1;
	}

	if (deviceType == CARD_TYPE_SD)
		TRACE("SD card found, parent: %p\n", parent);
	else if (deviceType == CARD_TYPE_SDHC)
		TRACE("SDHC card found, parent: %p\n", parent);
	else if (deviceType == CARD_TYPE_SDIO) {
		// Ignore silently, since it is not mass storage and should be handled by other drivers
		return 0.0;
	} else if (deviceType == CARD_TYPE_MMC)
		TRACE("MMC device found, parent: %p\n", parent);
	else if (deviceType == CARD_TYPE_MMC_EXTENDED_CAPACITY)
		TRACE("High capacity MMC device found, parent: %p\n", parent);
	else
		return 0.0;

	return 0.8;
}


static status_t
mmc_disk_register_device(device_node* node)
{
	CALLED();

	uint8_t deviceType = CARD_TYPE_UNKNOWN;
	sDeviceManager->get_attr_uint8(node, kMmcTypeAttribute, &deviceType, true);

	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = is_mmc_card_type((card_type)deviceType)
				? "eMMC" : "SD Card" }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, MMC_DISK_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
mmc_disk_execute_iorequest(void* data, IOOperation* operation)
{
	mmc_disk_driver_info* info = (mmc_disk_driver_info*)data;
	status_t error;

	if (operation->IsWrite() && info->readOnly) {
		info->scheduler->OperationCompleted(operation, B_READ_ONLY_DEVICE, 0);
		return B_READ_ONLY_DEVICE;
	}

	uint8_t command;
	if (operation->IsWrite()) {
		command = SD_WRITE_MULTIPLE_BLOCKS;
		// Set before the write: even a failed one may have left data in the
		// device's cache
		atomic_set(&info->cacheDirty, 1);
	} else
		command = SD_READ_MULTIPLE_BLOCKS;
	error = info->mmc->do_io(info->parent, info->parentCookie, info->rca,
		command, operation, (info->flags & kIoCommandOffsetAsSectors) != 0);

	// And again after it: a flush from another thread (a sync, say) may have
	// cleared the flag while this write waited for the bus, and then run
	// before it. The write is in the cache now, and the next flush, for
	// example the one its file system sends after a journal write, must not
	// skip it.
	if (operation->IsWrite())
		atomic_set(&info->cacheDirty, 1);

	if (error != B_OK) {
		info->scheduler->OperationCompleted(operation, error, 0);
		return error;
	}

	info->scheduler->OperationCompleted(operation, B_OK, operation->Length());
	return B_OK;
}


static status_t
mmc_disk_read_ext_csd(mmc_disk_driver_info* info, uint8* extCsd)
{
	if (info->mmc->read_data == NULL)
		return B_NOT_SUPPORTED;
	return info->mmc->read_data(info->parent, info->parentCookie, info->rca,
		MMC_SEND_EXT_CSD, 0, extCsd, kExtCsdSize);
}


/*!	Polls CMD13 (SEND_STATUS) until the device is back in the transfer state
	and ready for data, for example after the busy phase of CMD6.
*/
static status_t
mmc_disk_wait_ready(mmc_disk_driver_info* info, uint32* _status)
{
	bigtime_t deadline = system_time() + 1000000;
	while (true) {
		uint32 status = 0;
		status_t error = info->mmc->execute_command(info->parent,
			info->parentCookie, info->rca, SEND_STATUS,
			(uint32)info->rca << 16, &status);
		*_status = status;
		if (error != B_OK)
			return error;

		const uint32 kStateTransfer = 4;
		if ((status & (1 << 8)) != 0 && ((status >> 9) & 0xF) == kStateTransfer)
			return B_OK;
		if (system_time() > deadline)
			return B_TIMED_OUT;
		snooze(1000);
	}
}


/*!	Writes one byte of the EXT_CSD with CMD6 (SWITCH) and waits for the device
	to be done with it. The busy phase must be over before anything else is
	changed, in particular the bus width on the host side.
*/
static status_t
mmc_disk_switch(mmc_disk_driver_info* info, uint8 index, uint8 value)
{
	// Access mode 3 = write byte, command set 0
	uint32 argument = (3 << 24) | ((uint32)index << 16) | ((uint32)value << 8);
	uint32 response = 0;
	status_t error = info->mmc->execute_command(info->parent,
		info->parentCookie, info->rca, MMC_SWITCH, argument, &response);
	if (error != B_OK) {
		ERROR("CMD6 EXT_CSD[%u] = %u failed: %s\n", index, value,
			strerror(error));
		return error;
	}

	uint32 status = 0;
	error = mmc_disk_wait_ready(info, &status);
	if (error != B_OK) {
		ERROR("device not ready after CMD6 EXT_CSD[%u] = %u: %s "
			"(status %08" B_PRIx32 ")\n", index, value, strerror(error), status);
		return error;
	}
	if ((status & (1 << 7)) != 0) {
		ERROR("CMD6 EXT_CSD[%u] = %u rejected (SWITCH_ERROR, status %08"
			B_PRIx32 ")\n", index, value, status);
		return B_ERROR;
	}
	return B_OK;
}


/*!	Tries 8 then 4 bit data transfers, and keeps the first that
	works. Each width is checked by reading the EXT_CSD again and comparing
	fields that never change with the copy read in 1-bit mode.
*/
static void
mmc_disk_select_bus_width(mmc_disk_driver_info* info, const uint8* extCsd)
{
	static const struct {
		int		width;
		uint8	value;
	} kWidths[] = {
		{ 8, 2 },
		{ 4, 1 },
	};

	info->busWidth = 1;

	uint8* check = (uint8*)malloc(kExtCsdSize);
	if (check == NULL)
		return;

	for (size_t i = 0; i < sizeof(kWidths) / sizeof(kWidths[0]); i++) {
		const int width = kWidths[i].width;
		if (width > info->maxBusWidth) {
			TRACE("bus width %d skipped (limited to %d by the settings "
				"file)\n", width, info->maxBusWidth);
			continue;
		}

		if (mmc_disk_switch(info, kExtCsdBusWidth, kWidths[i].value) != B_OK) {
			TRACE("bus width %d: device refused the switch\n", width);
			continue;
		}

		info->mmc->set_bus_width(info->parent, info->parentCookie, width);

		memset(check, 0, kExtCsdSize);
		status_t error = mmc_disk_read_ext_csd(info, check);
		if (error == B_OK && ext_csd_same_device(extCsd, check)) {
			info->busWidth = width;
			TRACE("%d-bit bus verified (EXT_CSD[183] now %u)\n",
				width, check[kExtCsdBusWidth]);
			free(check);
			return;
		}

		ERROR("bus width %d does not work (%s%s), going back to 1 bit\n",
			width, strerror(error),
			error == B_OK ? ", EXT_CSD read back differs" : "");

		info->mmc->set_bus_width(info->parent, info->parentCookie, 1);
		mmc_disk_switch(info, kExtCsdBusWidth, 0);
		error = mmc_disk_read_ext_csd(info, check);
		ERROR("1-bit transfers after going back: %s\n", strerror(error));
	}

	TRACE("staying with a 1-bit bus\n");
	free(check);
}


/*!	Turns the device's volatile write cache on (or off, as the settings say).
	It exists since eMMC 4.5 (EXT_CSD revision 6). With it on, completed
	writes may still be in the cache: mmc_disk_flush_cache() writes them to
	flash when a file system asks (B_FLUSH_DRIVE_CACHE, which BFS does after
	each journal write and the kernel after syncing a volume, including at
	shutdown), when the device is closed, and when the driver goes away.
*/
static void
mmc_disk_setup_cache(mmc_disk_driver_info* info, const uint8* extCsd)
{
	const uint32 cacheSize = (uint32)extCsd[kExtCsdCacheSize]
		| ((uint32)extCsd[kExtCsdCacheSize + 1] << 8)
		| ((uint32)extCsd[kExtCsdCacheSize + 2] << 16)
		| ((uint32)extCsd[kExtCsdCacheSize + 3] << 24);
	const bool supported = extCsd[kExtCsdRevision] >= 6 && cacheSize > 0;
	bool enabled = (extCsd[kExtCsdCacheControl] & 1) != 0;
	const char* note = "";

	if (!supported) {
		note = " (not supported by the device)";
	} else if (info->readOnly) {
		note = " (read-only, left as the firmware set it)";
	} else if (info->wantCache != enabled) {
		// Turning the cache off also flushes it
		if (mmc_disk_switch(info, kExtCsdCacheControl,
				info->wantCache ? 1 : 0) == B_OK) {
			uint8* check = (uint8*)malloc(kExtCsdSize);
			if (check != NULL && mmc_disk_read_ext_csd(info, check) == B_OK)
				enabled = (check[kExtCsdCacheControl] & 1) != 0;
			else
				enabled = info->wantCache;
			free(check);
		}
		if (enabled != info->wantCache)
			note = " (COULD NOT BE CHANGED)";
		else if (!enabled)
			note = " (disabled by the settings file)";
	} else if (!enabled)
		note = " (disabled by the settings file)";

	info->cacheEnabled = enabled;
	TRACE_ALWAYS("write cache: %" B_PRIu32 " KiB, %s%s\n", cacheSize,
		enabled ? "on (flushed on request, on close and on shutdown)"
			: "off (writes are on flash when they complete)", note);
}


/*!	Writes what the device's volatile cache holds to flash, if anything was
	written since the last flush. The controller waits for the end of the
	busy signal (up to 30s) while it still owns the bus, so no other command
	can reach the device before the flush is over.
*/
static status_t
mmc_disk_flush_cache(mmc_disk_driver_info* info)
{
	if (!info->cacheEnabled || atomic_get_and_set(&info->cacheDirty, 0) == 0)
		return B_OK;

	status_t status = mmc_disk_switch(info, kExtCsdFlushCache, 1);
	if (status != B_OK) {
		atomic_set(&info->cacheDirty, 1);
		ERROR("write cache flush FAILED: %s\n", strerror(status));
	}
	return status;
}


static bool
mmc_disk_transfers_work(mmc_disk_driver_info* info, const uint8* extCsd,
	uint8* check)
{
	for (int i = 0; i < 3; i++) {
		memset(check, 0, kExtCsdSize);
		if (mmc_disk_read_ext_csd(info, check) != B_OK
			|| !ext_csd_same_device(extCsd, check)) {
			return false;
		}
	}
	return true;
}


/*!	Puts device and controller back to the legacy timing at the current bus
	width, after a faster timing did not work.
*/
static void
mmc_disk_back_to_legacy(mmc_disk_driver_info* info, const uint8* extCsd,
	uint8* check)
{
	const uint8 sdrWidth = info->busWidth == 8 ? 2
		: info->busWidth == 4 ? 1 : 0;

	// The controller first. Until the device is back too, only commands are
	// exchanged, and the command line works at 25MHz whatever timing the
	// device is in.
	info->mmc->set_timing(info->parent, info->parentCookie, info->rca,
		MMC_TIMING_LEGACY, MMC_TIMING_NO_VERIFY);
	mmc_disk_switch(info, kExtCsdBusWidth, sdrWidth);
	mmc_disk_switch(info, kExtCsdHsTiming, 0);

	bool ok = mmc_disk_transfers_work(info, extCsd, check);
	if (!ok)
		ERROR("back to the legacy timing, but transfers still fail\n");
}


/*!	HS200: 4 or 8 bit single data rate bus (already set), device switched with
	HS_TIMING = 2, then the controller at 200MHz, which runs the tuning.
*/
static status_t
mmc_disk_enter_hs200(mmc_disk_driver_info* info)
{
	status_t status = mmc_disk_switch(info, kExtCsdHsTiming, 2);
	if (status != B_OK) {
		TRACE("HS200: device refused the timing\n");
		return status;
	}
	return info->mmc->set_timing(info->parent, info->parentCookie, info->rca,
		MMC_TIMING_HS200, MMC_TIMING_SIGNALING_1V8);
}


/*!	HS400, JEDEC 84-B51 section 6.6.2.3: from a tuned HS200 bus, lower the
	clock to 52MHz, switch the device to high speed, then to the 8 bit dual
	data rate bus width, then to HS400, and finally the controller to HS400 at
	200MHz, keeping the HS200 tuning result.
*/
static status_t
mmc_disk_enter_hs400(mmc_disk_driver_info* info)
{
	status_t status = mmc_disk_enter_hs200(info);
	if (status != B_OK)
		return status;

	status = info->mmc->set_timing(info->parent, info->parentCookie, info->rca,
		MMC_TIMING_HS_SDR25, MMC_TIMING_SIGNALING_1V8 | MMC_TIMING_KEEP_TUNING
			| MMC_TIMING_NO_VERIFY);
	if (status != B_OK)
		return status;

	status = mmc_disk_switch(info, kExtCsdHsTiming, 1);
	if (status != B_OK) {
		TRACE("HS400: device refused high speed timing\n");
		return status;
	}
	status = mmc_disk_switch(info, kExtCsdBusWidth, 6);
	if (status != B_OK) {
		TRACE("HS400: device refused the 8 bit DDR bus width\n");
		return status;
	}
	status = mmc_disk_switch(info, kExtCsdHsTiming, 3);
	if (status != B_OK) {
		TRACE("HS400: device refused the timing\n");
		return status;
	}

	return info->mmc->set_timing(info->parent, info->parentCookie, info->rca,
		MMC_TIMING_HS400, MMC_TIMING_SIGNALING_1V8 | MMC_TIMING_KEEP_TUNING);
}


/*!	DDR52: high speed timing, then the dual data rate bus width (which needs
	high speed timing to be set already), then the controller.
*/
static status_t
mmc_disk_enter_ddr52(mmc_disk_driver_info* info, uint32 flags)
{
	const uint8 ddrWidth = info->busWidth == 8 ? 6 : 5;
	status_t status = mmc_disk_switch(info, kExtCsdHsTiming, 1);
	if (status != B_OK) {
		TRACE("DDR52: device refused high speed timing\n");
		return status;
	}
	status = mmc_disk_switch(info, kExtCsdBusWidth, ddrWidth);
	if (status != B_OK) {
		TRACE("DDR52: device refused the DDR bus width\n");
		return status;
	}
	return info->mmc->set_timing(info->parent, info->parentCookie, info->rca,
		MMC_TIMING_DDR52, flags);
}


static status_t
mmc_disk_enter_hs52(mmc_disk_driver_info* info, int timing, uint32 flags)
{
	status_t status = mmc_disk_switch(info, kExtCsdHsTiming, 1);
	if (status != B_OK) {
		TRACE("HS52: device refused high speed timing\n");
		return status;
	}
	return info->mmc->set_timing(info->parent, info->parentCookie, info->rca,
		timing, flags);
}


/*!	Tries the bus timings from the fastest down, and keeps the
	first one with which data transfers still work. Each attempt is checked
	twice: by the controller (the first blocks of the device compared with the
	self test reference, using the normal transfer method) and here (EXT_CSD
	read three times and compared with the copy read at the start). After a
	failed attempt, device and controller go back to the legacy timing before
	the next one.
*/
static void
mmc_disk_select_speed(mmc_disk_driver_info* info, const uint8* extCsd)
{
	static const struct {
		int		timing;
		uint32	flags;
	} kAttempts[] = {
		{ MMC_TIMING_HS400, MMC_TIMING_SIGNALING_1V8 },
		{ MMC_TIMING_HS200, MMC_TIMING_SIGNALING_1V8 },
		{ MMC_TIMING_DDR52, MMC_TIMING_SIGNALING_1V8 },
		{ MMC_TIMING_DDR52, 0 },
		{ MMC_TIMING_HS_SDR25, MMC_TIMING_SIGNALING_1V8 },
		{ MMC_TIMING_HS, 0 },
	};

	info->timing = MMC_TIMING_LEGACY;

	if (info->mmc->set_timing == NULL) {
		TRACE("the bus manager cannot change the timing\n");
		return;
	}

	// EXT_CSD DEVICE_TYPE: bit 1 HS 52MHz, bit 2 DDR 52MHz (1.8V/3V),
	// bit 4 HS200 (1.8V), bit 6 HS400 (1.8V)
	const uint8 deviceType = extCsd[kExtCsdDeviceType];
	const bool deviceHs = (deviceType & (1 << 1)) != 0;
	const bool deviceDdr = (deviceType & (1 << 2)) != 0;
	const bool deviceHs200 = (deviceType & (1 << 4)) != 0;
	const bool deviceHs400 = (deviceType & (1 << 6)) != 0;
	TRACE("device supports:%s%s%s%s, bus width %d\n",
		deviceHs ? " HS52" : "", deviceDdr ? " DDR52" : "",
		deviceHs200 ? " HS200" : "", deviceHs400 ? " HS400" : "",
		info->busWidth);

	uint8* check = (uint8*)malloc(kExtCsdSize);
	if (check == NULL)
		return;

	for (size_t i = 0; i < sizeof(kAttempts) / sizeof(kAttempts[0]); i++) {
		const int timing = kAttempts[i].timing;
		const uint32 flags = kAttempts[i].flags;
		const bool signaling18 = (flags & MMC_TIMING_SIGNALING_1V8) != 0;

		bool possible;
		switch (timing) {
			case MMC_TIMING_HS400:
				possible = deviceHs400 && info->busWidth == 8;
				break;
			case MMC_TIMING_HS200:
				possible = deviceHs200 && info->busWidth >= 4;
				break;
			case MMC_TIMING_DDR52:
				possible = deviceDdr && info->busWidth >= 4;
				break;
			default:
				possible = deviceHs;
				break;
		}
		if (!possible || timing > info->maxTiming)
			continue;

		// Skip what the controller cannot do before touching the device. HS400
		// also needs HS200 on the way.
		if (info->mmc->set_timing(info->parent, info->parentCookie, info->rca,
				timing, MMC_TIMING_QUERY) != B_OK
			|| (timing == MMC_TIMING_HS400
				&& info->mmc->set_timing(info->parent, info->parentCookie,
					info->rca, MMC_TIMING_HS200, MMC_TIMING_QUERY) != B_OK)) {
			continue;
		}

		const char* name = mmc_timing_name(timing);
		const char* variant = signaling18 && timing < MMC_TIMING_HS200
			? " with 1.8V signaling" : "";
		TRACE("trying %s%s\n", name, variant);

		status_t status;
		switch (timing) {
			case MMC_TIMING_HS400:
				status = mmc_disk_enter_hs400(info);
				break;
			case MMC_TIMING_HS200:
				status = mmc_disk_enter_hs200(info);
				break;
			case MMC_TIMING_DDR52:
				status = mmc_disk_enter_ddr52(info, flags);
				break;
			default:
				status = mmc_disk_enter_hs52(info, timing, flags);
				break;
		}

		if (status == B_OK && mmc_disk_transfers_work(info, extCsd, check)) {
			info->timing = timing;
			TRACE("%s%s works\n", name, variant);
			free(check);
			return;
		}

		ERROR("%s%s does not work (%s)\n", name, variant,
			status != B_OK ? strerror(status) : "EXT_CSD read back differs");
		mmc_disk_back_to_legacy(info, extCsd, check);
	}

	TRACE("staying with the legacy timing (25MHz)\n");
	free(check);
}


static status_t
mmc_disk_get_emmc_geometry(mmc_disk_driver_info* info, mmc_disk_csd& csd,
	device_geometry* geometry)
{
	uint64 sectorCount = 0;

	uint8* extCsd = (uint8*)malloc(kExtCsdSize);
	if (extCsd == NULL)
		return B_NO_MEMORY;

	memset(extCsd, 0, kExtCsdSize);
	status_t error = mmc_disk_read_ext_csd(info, extCsd);
	if (error == B_OK) {
		sectorCount = ext_csd_sector_count(extCsd);
		TRACE("EXT_CSD: revision %u, structure %u, device type %02x, "
			"sectors %" B_PRIu64 " (%" B_PRIu64 " MB), bus width %u, "
			"HS timing %u, boot partitions %u KB each, partition config "
			"%02x\n", extCsd[kExtCsdRevision], extCsd[kExtCsdStructure],
			extCsd[kExtCsdDeviceType], sectorCount,
			sectorCount * kBlockSize / (1024 * 1024),
			extCsd[kExtCsdBusWidth], extCsd[kExtCsdHsTiming],
			extCsd[kExtCsdBootSizeMult] * 128, extCsd[kExtCsdPartitionConfig]);
	} else
		ERROR("reading EXT_CSD failed: %s\n", strerror(error));

	if (sectorCount == 0) {
		// Devices up to 2GB have no SEC_COUNT, their size is in the CSD
		sectorCount = csd.mmc_capacity() / kBlockSize;
		TRACE("using the size from the CSD: %" B_PRIu64 " sectors%s\n",
			sectorCount,
			error != B_OK ? " (probably wrong, EXT_CSD could not be read)" : "");
	}

	if (error == B_OK) {
		mmc_disk_select_bus_width(info, extCsd);
		mmc_disk_select_speed(info, extCsd);
		mmc_disk_setup_cache(info, extCsd);
	} else
		info->busWidth = 1;

	free(extCsd);

	geometry->bytes_per_sector = kBlockSize;
	geometry->bytes_per_physical_sector = kBlockSize;
	if (sectorCount <= UINT32_MAX) {
		geometry->sectors_per_track = 1;
		geometry->cylinder_count = sectorCount;
	} else {
		geometry->sectors_per_track = 256;
		geometry->cylinder_count = sectorCount / 256;
	}
	geometry->head_count = 1;
	geometry->device_type = B_DISK;
	geometry->removable = false;
	geometry->read_only = info->readOnly;
	geometry->write_once = false;

	TRACE_ALWAYS("%" B_PRIu64 " sectors of %" B_PRIu32 " bytes, %d-bit bus, "
		"%s, %s\n", sectorCount, kBlockSize, info->busWidth,
		mmc_timing_name(info->timing),
		info->readOnly ? "read-only" : "writable");
	return B_OK;
}


static status_t
mmc_block_get_geometry(mmc_disk_driver_info* info, device_geometry* geometry)
{
	struct mmc_disk_csd csd;
	TRACE("Get geometry\n");
	status_t error = info->mmc->execute_command(info->parent, info->parentCookie, 0, SEND_CSD,
		info->rca << 16, (uint32_t*)&csd);
	if (error != B_OK) {
		TRACE("Could not get CSD! %s\n", strerror(error));
		return error;
	}

	TRACE("CSD: %" PRIx64 " %" PRIx64 "\n", csd.bits[0], csd.bits[1]);

	if (info->isMMC) {
		TRACE("CSD %016" B_PRIx64 " %016" B_PRIx64 ", structure %u\n",
			csd.bits[1], csd.bits[0], csd.structure_version());
		return mmc_disk_get_emmc_geometry(info, csd, geometry);
	}

	// SD cards: unchanged from before
	if (csd.structure_version() >= 3) {
		TRACE("unknown CSD version %d\n", csd.structure_version());
		return B_NOT_SUPPORTED;
	}

	geometry->bytes_per_sector = 1 << csd.read_bl_len();
	geometry->sectors_per_track = csd.c_size() + 1;
	geometry->cylinder_count = 1 << (csd.c_size_mult() + 2);
	geometry->head_count = 1;
	geometry->device_type = B_DISK;
	geometry->removable = true;
	geometry->read_only = false; // TODO check write protect switch?
	geometry->write_once = false;

	// This function will be called before all data transfers, so we use this
	// opportunity to switch the card to 4-bit data transfers (instead of the
	// default 1 bit mode)
	uint32_t cardStatus = 0;
	const uint32 k4BitMode = 2;
	info->mmc->execute_command(info->parent, info->parentCookie, info->rca,
		SD_APP_CMD, info->rca << 16, &cardStatus);
	status_t status = info->mmc->execute_command(info->parent,
		info->parentCookie, info->rca, SD_SET_BUS_WIDTH, k4BitMode,
		&cardStatus);

	TRACE("Card status after switching to 4 bit mode: %08x\n", cardStatus);

	if (status == B_OK) {
		// From now on we use 4 bit mode
		TRACE("Switch to 4 bit mode for data transfers\n");
		info->mmc->set_bus_width(info->parent, info->parentCookie, 4);
	} else {
		TRACE("Switching to 4 bit mode failed: %s\n", strerror(status));
	}

	return B_OK;
}


static status_t
mmc_disk_init_driver(device_node* node, void** cookie)
{
	CALLED();
	mmc_disk_driver_info* info = (mmc_disk_driver_info*)malloc(
		sizeof(mmc_disk_driver_info));

	if (info == NULL)
		return B_NO_MEMORY;

	memset(info, 0, sizeof(*info));

	void* unused2;
	info->node = node;
	info->parent = sDeviceManager->get_parent_node(info->node);
	sDeviceManager->get_driver(info->parent, (driver_module_info **)&info->mmc,
		&unused2);

	// We need to grab the bus cookie as well
	// FIXME it would be easier if that was available from the get_driver call
	// above directly, but currently it isn't.
	device_node* busNode = sDeviceManager->get_parent_node(info->parent);
	driver_module_info* unused;
	sDeviceManager->get_driver(busNode, &unused, &info->parentCookie);
	sDeviceManager->put_node(busNode);

	TRACE("MMC bus handle: %p %s\n", info->mmc, info->mmc->info.info.name);

	if (sDeviceManager->get_attr_uint16(node, kMmcRcaAttribute, &info->rca,
			true) != B_OK) {
		TRACE("MMC card node has no RCA attribute\n");
		free(info);
		return B_BAD_DATA;
	}

	uint8_t deviceType;
	if (sDeviceManager->get_attr_uint8(info->parent, kMmcTypeAttribute,
			&deviceType, true) != B_OK) {
		ERROR("Could not get device type\n");
		free(info);
		return B_BAD_DATA;
	}

	// SD and MMC cards use byte offsets for IO commands, later ones (SDHC,
	// SDXC, ...) use sectors.
	if (deviceType == CARD_TYPE_SD || deviceType == CARD_TYPE_MMC)
		info->flags = 0;
	else
		info->flags = kIoCommandOffsetAsSectors;

	info->deviceType = deviceType;
	info->isMMC = is_mmc_card_type((card_type)deviceType);
	info->readOnly = false;
	info->maxBusWidth = 8;
	info->busWidth = 1;
	info->maxTiming = MMC_TIMING_HS400;
	info->timing = MMC_TIMING_LEGACY;
	info->cacheEnabled = false;
	info->wantCache = true;
	info->cacheDirty = 0;

	if (info->isMMC) {
		// Options in /boot/home/config/settings/kernel/drivers/mmc_disk:
		//   emmc_write_enable false	read-only (writable by default)
		//   emmc_write_cache false		don't use the device's write cache
		//   emmc_bus_width 4			widest bus to try
		bool writeEnable = true;
		void* handle = load_driver_settings("mmc_disk");
		if (handle != NULL) {
			writeEnable = get_driver_boolean_parameter(handle,
				"emmc_write_enable", true, true);
			info->wantCache = get_driver_boolean_parameter(handle,
				"emmc_write_cache", true, true);
			const char* width = get_driver_parameter(handle, "emmc_bus_width",
				NULL, NULL);
			if (width != NULL) {
				if (strcmp(width, "1") == 0)
					info->maxBusWidth = 1;
				else if (strcmp(width, "4") == 0)
					info->maxBusWidth = 4;
				else if (strcmp(width, "8") == 0)
					info->maxBusWidth = 8;
			}
			// Fastest bus timing to try: legacy, hs, ddr, hs200 or hs400,
			// for example
			//   emmc_speed hs200
			const char* speed = get_driver_parameter(handle, "emmc_speed",
				NULL, NULL);
			if (speed != NULL) {
				if (strcmp(speed, "legacy") == 0)
					info->maxTiming = MMC_TIMING_LEGACY;
				else if (strcmp(speed, "hs") == 0)
					info->maxTiming = MMC_TIMING_HS_SDR25;
				else if (strcmp(speed, "ddr") == 0)
					info->maxTiming = MMC_TIMING_DDR52;
				else if (strcmp(speed, "hs200") == 0)
					info->maxTiming = MMC_TIMING_HS200;
				else if (strcmp(speed, "hs400") == 0)
					info->maxTiming = MMC_TIMING_HS400;
			}
			unload_driver_settings(handle);
		}
		info->readOnly = !writeEnable;

		TRACE("eMMC device, RCA %u, %s addressing, %s, bus width up to %d "
			"bits, speed up to %s, write cache %s%s\n", info->rca,
			(info->flags & kIoCommandOffsetAsSectors) != 0 ? "sector" : "byte",
			info->readOnly ? "READ-ONLY" : "WRITABLE", info->maxBusWidth,
			mmc_timing_name(info->maxTiming),
			info->wantCache ? "wanted" : "not wanted",
			handle != NULL ? "" : " (no settings file)");
	}

	status_t error;

	static const uint32 kDMAResourceBufferCount			= 16;
	static const uint32 kDMAResourceBounceBufferCount	= 16;

	info->dmaResource = new(std::nothrow) DMAResource;
	if (info->dmaResource == NULL) {
		TRACE("Failed to allocate DMA resource");
		free(info);
		return B_NO_MEMORY;
	}

	error = info->dmaResource->Init(info->node, kBlockSize,
		kDMAResourceBufferCount, kDMAResourceBounceBufferCount);
	if (error != B_OK) {
		TRACE("Failed to init DMA resource");
		delete info->dmaResource;
		free(info);
		return error;
	}

	info->scheduler = new(std::nothrow) IOSchedulerSimple(info->dmaResource);
	if (info->scheduler == NULL) {
		TRACE("Failed to allocate scheduler");
		delete info->dmaResource;
		free(info);
		return B_NO_MEMORY;
	}

	error = info->scheduler->Init("mmc storage");
	if (error != B_OK) {
		TRACE("Failed to init scheduler");
		delete info->scheduler;
		delete info->dmaResource;
		free(info);
		return error;
	}
	info->scheduler->SetCallback(&mmc_disk_execute_iorequest, info);

	memset(&info->geometry, 0, sizeof(info->geometry));

	TRACE("MMC card device initialized for RCA %x\n", info->rca);
	*cookie = info;
	return B_OK;
}


static void
mmc_disk_uninit_driver(void* _cookie)
{
	CALLED();
	mmc_disk_driver_info* info = (mmc_disk_driver_info*)_cookie;
	if (info->isMMC)
		mmc_disk_flush_cache(info);
	delete info->scheduler;
	delete info->dmaResource;
	sDeviceManager->put_node(info->parent);
	free(info);
}


static status_t
mmc_disk_register_child_devices(void* _cookie)
{
	CALLED();
	mmc_disk_driver_info* info = (mmc_disk_driver_info*)_cookie;
	status_t status;

	int32 id = sDeviceManager->create_id(MMC_DEVICE_ID_GENERATOR);
	if (id < 0)
		return id;

	char name[64];
	snprintf(name, sizeof(name), "disk/mmc/%" B_PRId32 "/raw", id);

	status = sDeviceManager->publish_device(info->node, name,
		MMC_DISK_DEVICE_MODULE_NAME);

	return status;
}


//	#pragma mark - device module API


static status_t
mmc_block_init_device(void* _info, void** _cookie)
{
	CALLED();

	// No additional context, so just reuse the same data as the disk device
	mmc_disk_driver_info* info = (mmc_disk_driver_info*)_info;
	*_cookie = info;

	// Note: it is not possible to execute commands here, because this is called
	// with the mmc_bus locked for enumeration (and still using slow clock).

	return B_OK;
}


static void
mmc_block_uninit_device(void* _cookie)
{
	CALLED();
	//mmc_disk_driver_info* info = (mmc_disk_driver_info*)_cookie;

	// TODO cleanup whatever is relevant
}


static status_t
mmc_block_open(void* _info, const char* path, int openMode, void** _cookie)
{
	CALLED();
	mmc_disk_driver_info* info = (mmc_disk_driver_info*)_info;

	// allocate cookie
	mmc_disk_handle* handle = new(std::nothrow) mmc_disk_handle;
	*_cookie = handle;
	if (handle == NULL)
		return B_NO_MEMORY;

	handle->info = info;

	if (handle->info->geometry.bytes_per_sector == 0) {
		status_t error = mmc_block_get_geometry(handle->info,
			&handle->info->geometry);
		if (error != B_OK) {
			TRACE("Failed to get disk capacity");
			delete handle;
			*_cookie = NULL;
			return error;
		}
	}

	return B_OK;
}


static status_t
mmc_block_close(void* cookie)
{
	mmc_disk_handle* handle = (mmc_disk_handle*)cookie;
	CALLED();

	// Programs writing to the raw device (dd, for example) never ask for a
	// flush: fsync() on a device does not reach the driver
	if (handle->info->isMMC)
		mmc_disk_flush_cache(handle->info);
	return B_OK;
}


static status_t
mmc_block_free(void* cookie)
{
	CALLED();
	mmc_disk_handle* handle = (mmc_disk_handle*)cookie;

	delete handle;
	return B_OK;
}


static status_t
mmc_block_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	CALLED();
	mmc_disk_handle* handle = (mmc_disk_handle*)cookie;

	size_t length = *_length;
	if (position >= handle->info->DeviceSize())
		return ERANGE;
	if ((position + (off_t)length) > handle->info->DeviceSize())
		length = (handle->info->DeviceSize() - position);

	IORequest request;
	status_t status = request.Init(position, (addr_t)buffer, length, false, 0);
	if (status != B_OK)
		return status;

	status = handle->info->scheduler->ScheduleRequest(&request);
	if (status != B_OK)
		return status;

	status = request.Wait(0, 0);
	*_length = request.TransferredBytes();
	return status;
}


static status_t
mmc_block_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	CALLED();
	mmc_disk_handle* handle = (mmc_disk_handle*)cookie;

	if (handle->info->readOnly) {
		*_length = 0;
		return B_READ_ONLY_DEVICE;
	}

	size_t length = *_length;
	if (position >= handle->info->DeviceSize())
		return ERANGE;
	if ((position + (off_t)length) > handle->info->DeviceSize())
		length = (handle->info->DeviceSize() - position);

	IORequest request;
	status_t status = request.Init(position, (addr_t)buffer, length, true, 0);
	if (status != B_OK)
		return status;

	status = handle->info->scheduler->ScheduleRequest(&request);
	if (status != B_OK)
		return status;

	status = request.Wait(0, 0);
	*_length = request.TransferredBytes();
	return status;
}


static status_t
mmc_block_io(void* cookie, io_request* request)
{
	CALLED();
	mmc_disk_handle* handle = (mmc_disk_handle*)cookie;

	if (request->IsWrite() && handle->info->readOnly)
		return B_READ_ONLY_DEVICE;

	if ((request->Offset() + (off_t)request->Length()) > handle->info->DeviceSize())
		return ERANGE;

	return handle->info->scheduler->ScheduleRequest(request);
}


static status_t
mmc_block_trim(mmc_disk_driver_info* info, fs_trim_data* trimData)
{
	enum {
		kEraseModeErase = 0, // force to actually erase the data
		kEraseModeDiscard = 1,
			// just mark the data as unused for internal wear leveling
			// algorithms
		kEraseModeFullErase = 2, // erase the whole card
	};
	TRACE("trim_device()\n");

	trimData->trimmed_size = 0;

	const off_t deviceSize = info->DeviceSize(); // in bytes
	if (deviceSize < 0)
		return B_BAD_VALUE;

	STATIC_ASSERT(sizeof(deviceSize) <= sizeof(uint64));
	ASSERT(deviceSize >= 0);

	// Do not trim past device end
	for (uint32 i = 0; i < trimData->range_count; i++) {
		uint64 offset = trimData->ranges[i].offset;
		uint64& size = trimData->ranges[i].size;

		if (offset >= (uint64)deviceSize)
			return B_BAD_VALUE;
		size = min_c(size, (uint64)deviceSize - offset);
	}

	uint64 trimmedSize = 0;
	status_t result = B_OK;
	for (uint32 i = 0; i < trimData->range_count; i++) {
		uint64 offset = trimData->ranges[i].offset;
		uint64 length = trimData->ranges[i].size;

		// Round up offset and length to multiple of the sector size
		// The offset is rounded up, so some space may be left
		// (not trimmed) at the start of the range.
		offset = ROUNDUP(offset, kBlockSize);
		// Adjust the length for the possibly skipped range
		length -= offset - trimData->ranges[i].offset;
		// The length is rounded down, so some space at the end may also
		// be left (not trimmed).
		length &= ~(kBlockSize - 1);

		if (length == 0)
			continue;

		TRACE("trim %" B_PRIu64 " bytes from %" B_PRIu64 "\n",
			length, offset);

		ASSERT(offset % kBlockSize == 0);
		ASSERT(length % kBlockSize == 0);

		if ((info->flags & kIoCommandOffsetAsSectors) != 0) {
			offset /= kBlockSize;
			length /= kBlockSize;
		}

		// Parameter of execute_command is uint32_t
		if (offset > UINT32_MAX
			|| length > UINT32_MAX - offset) {
			result = B_BAD_VALUE;
			break;
		}

		uint32_t response;
		result = info->mmc->execute_command(info->parent, info->parentCookie,
			info->rca, SD_ERASE_WR_BLK_START, offset, &response);
		if (result != B_OK)
			break;
		result = info->mmc->execute_command(info->parent, info->parentCookie,
			info->rca, SD_ERASE_WR_BLK_END, offset + length, &response);
		if (result != B_OK)
			break;
		result = info->mmc->execute_command(info->parent, info->parentCookie,
			info->rca, SD_ERASE, kEraseModeDiscard, &response);
		if (result != B_OK)
			break;

		trimmedSize += (info->flags & kIoCommandOffsetAsSectors) != 0
			? length * kBlockSize : length;
	}

	trimData->trimmed_size = trimmedSize;

	return result;
}


static status_t
mmc_block_ioctl(void* cookie, uint32 op, void* buffer, size_t length)
{
	mmc_disk_handle* handle = (mmc_disk_handle*)cookie;
	mmc_disk_driver_info* info = handle->info;

	switch (op) {
		case B_GET_MEDIA_STATUS:
		{
			if (buffer == NULL || length < sizeof(status_t))
				return B_BAD_VALUE;

			status_t status = B_OK;
			return user_memcpy(buffer, &status, sizeof(status_t));
		}

		case B_GET_DEVICE_SIZE:
		{
			// Legacy ioctl, use B_GET_GEOMETRY

			uint64_t size = info->DeviceSize();
			if (size > SIZE_MAX)
				return B_NOT_SUPPORTED;
			size_t size32 = size;
			return user_memcpy(buffer, &size32, sizeof(size_t));
		}

		case B_GET_GEOMETRY:
		{
			if (buffer == NULL || length > sizeof(device_geometry))
				return B_BAD_VALUE;

			return user_memcpy(buffer, &info->geometry, length);
		}

		case B_GET_ICON_NAME:
			return user_strlcpy((char*)buffer, "devices/drive-harddisk",
				B_FILE_NAME_LENGTH);

		case B_GET_VECTOR_ICON:
		{
			// TODO: take device type into account!
			device_icon iconData;
			if (length != sizeof(device_icon))
				return B_BAD_VALUE;
			if (user_memcpy(&iconData, buffer, sizeof(device_icon)) != B_OK)
				return B_BAD_ADDRESS;

			if (iconData.icon_size >= (int32)sizeof(kDriveIcon)) {
				if (user_memcpy(iconData.icon_data, kDriveIcon,
						sizeof(kDriveIcon)) != B_OK)
					return B_BAD_ADDRESS;
			}

			iconData.icon_size = sizeof(kDriveIcon);
			return user_memcpy(buffer, &iconData, sizeof(device_icon));
		}

		case B_TRIM_DEVICE:
		{
			// eMMC uses CMD35/CMD36 to set the erase range, not the SD
			// commands CMD32/CMD33 used by mmc_block_trim(): those are illegal
			// commands for an eMMC device. Not implemented yet.
			if (info->isMMC || info->readOnly)
				return B_NOT_SUPPORTED;

			// We know the buffer is kernel-side because it has been
			// preprocessed in devfs
			return mmc_block_trim(info, (fs_trim_data*)buffer);
		}

		case B_FLUSH_DRIVE_CACHE:
			// File systems ask for this to make sure their journal is on
			// the media, and the kernel after syncing a volume
			if (!info->isMMC)
				return B_DEV_INVALID_IOCTL;
			return mmc_disk_flush_cache(info);
	}

	return B_DEV_INVALID_IOCTL;
}


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{}
};


// The "block device" associated with the device file. It can be open()
// multiple times, each allocating an mmc_disk_handle. It does not interact
// with the hardware directly, instead it forwards all IO requests to the
// disk driver through the IO scheduler.
struct device_module_info sMMCBlockDevice = {
	{
		MMC_DISK_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	mmc_block_init_device,
	mmc_block_uninit_device,
	NULL, // remove,

	mmc_block_open,
	mmc_block_close,
	mmc_block_free,
	mmc_block_read,
	mmc_block_write,
	mmc_block_io,
	mmc_block_ioctl,

	NULL,	// select
	NULL,	// deselect
};


// Driver for the disk devices itself. This is paired with an
// mmc_disk_driver_info instanciated once per device. Handles the actual disk
// I/O operations
struct driver_module_info sMMCDiskDriver = {
	{
		MMC_DISK_DRIVER_MODULE_NAME,
		0,
		NULL
	},
	mmc_disk_supports_device,
	mmc_disk_register_device,
	mmc_disk_init_driver,
	mmc_disk_uninit_driver,
	mmc_disk_register_child_devices,
	NULL, // mmc_disk_rescan_child_devices,
	NULL,
};


module_info* modules[] = {
	(module_info*)&sMMCDiskDriver,
	(module_info*)&sMMCBlockDevice,
	NULL
};
