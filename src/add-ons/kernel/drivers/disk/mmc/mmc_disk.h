/*
 * Copyright 2018-2020 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		B Krishnan Iyer, krishnaniyer97@gmail.com
 *		Adrien Destugues, pulkomandy@pulkomandy.tk
 */
#ifndef _MMC_DISK_H
#define _MMC_DISK_H


#include <device_manager.h>
#include <KernelExport.h>

#include <stdint.h>

#include <mmc.h>

#include "IOSchedulerSimple.h"


enum MMCDiskFlags {
	kIoCommandOffsetAsSectors = 1,
		// IO commands use sector offsets instead of byte offsets
};


// This is the device info structure, allocated once per device
typedef struct {
	device_node* node;
	device_node* parent;
	void* parentCookie;
	mmc_device_interface* mmc;
	uint16_t rca;
	uint32_t flags;

	uint8_t deviceType;
	bool isMMC;
	bool readOnly;
		// eMMC devices are read-only unless enabled in the settings file
	int maxBusWidth;
		// Widest data bus to try on eMMC (1, 4 or 8), from the settings file
	int busWidth;
	int maxTiming;
		// Fastest mmc_bus_timing to try on eMMC, from the settings file
	int timing;
	bool wantCache;
		// Use the device's volatile write cache (settings file, default yes)
	bool cacheEnabled;
		// The volatile write cache is on: writes must be flushed
	int32 cacheDirty;
		// Something was written since the last flush

	device_geometry geometry;

	DMAResource* dmaResource;
	IOScheduler* scheduler;

	off_t DeviceSize() const {
		return (off_t)geometry.bytes_per_sector * geometry.sectors_per_track
			* geometry.cylinder_count * geometry.head_count;
	}
} mmc_disk_driver_info;


// This is allocated once per open() call on the device (there can be multiple
// open file descriptors for the same device)
typedef struct {
	mmc_disk_driver_info* info;
} mmc_disk_handle;


#endif /*_MMC_DISK_H*/
