/*
 * Copyright 2005-2015, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2016, Jessica Hamilton, jessica.l.hamilton@gmail.com.
 * Distributed under the terms of the MIT License.
 */


#include <stdlib.h>
#include <string.h>

#include <compute_display_timing.h>
#include <create_display_modes.h>

#include "accelerant_protos.h"
#include "accelerant.h"


#define TRACE_MODE
#ifdef TRACE_MODE
extern "C" void _sPrintf(const char* format, ...);
#	define TRACE(x) _sPrintf x
#else
#	define TRACE(x) ;
#endif


bool
operator==(const display_mode &lhs, const display_mode &rhs)
{
	return lhs.space == rhs.space
		&& lhs.virtual_width == rhs.virtual_width
		&& lhs.virtual_height == rhs.virtual_height
		&& lhs.h_display_start == rhs.h_display_start
		&& lhs.v_display_start == rhs.v_display_start;
}


/*!	Creates the initial mode list of the primary accelerant.
	It's called from vesa_init_accelerant().
*/
status_t
create_mode_list(void)
{
	display_mode& currentMode = gInfo->shared_info->current_mode;

	if (currentMode.timing.h_total == 0 || currentMode.timing.v_total == 0) {
		compute_display_timing(currentMode.virtual_width, currentMode.virtual_height, 60, false,
			&currentMode.timing);
	}

	const color_space colorspace[] = {(color_space)currentMode.space};

	// A virtual display can show any size up to the driver's 3840x2160
	// framebuffer, whatever its EDID says (Parallels Desktop only lists
	// 1024x768 there), so always offer the host's current size and a set of
	// common sizes besides the EDID modes.
	static const struct { uint16 width, height; } kSizes[] = {
		{1280, 720}, {1280, 800}, {1280, 1024}, {1366, 768}, {1440, 900},
		{1600, 900}, {1600, 1200}, {1680, 1050}, {1920, 1080}, {1920, 1200},
		{2560, 1440}, {2560, 1600}, {3840, 2160}
	};
	const uint32 kSizeCount = sizeof(kSizes) / sizeof(kSizes[0]);
	display_mode modes[kSizeCount + 1];
	modes[0] = currentMode;
	fill_display_mode(currentMode.virtual_width, currentMode.virtual_height, &modes[0]);
	for (uint32 i = 0; i < kSizeCount; i++) {
		compute_display_timing(kSizes[i].width, kSizes[i].height, 60, false,
			&modes[i + 1].timing);
		fill_display_mode(kSizes[i].width, kSizes[i].height, &modes[i + 1]);
	}

	edid1_info edidInfo;
	if (gInfo->shared_info->has_edid)
		edid_decode(&edidInfo, &gInfo->shared_info->edid_raw);
	gInfo->mode_list_area = create_display_modes("virtio_gpu modes",
		gInfo->shared_info->has_edid ? &edidInfo : NULL, modes, kSizeCount + 1,
		colorspace, 1, NULL, &gInfo->mode_list, &gInfo->shared_info->mode_count);
	if (gInfo->mode_list_area < 0)
		return gInfo->mode_list_area;

	gInfo->shared_info->mode_list_area = gInfo->mode_list_area;
	return B_OK;
}


//	#pragma mark -


uint32
virtio_gpu_accelerant_mode_count(void)
{
	TRACE(("virtio_gpu_accelerant_mode_count() = %d\n",
		gInfo->shared_info->mode_count));
	return gInfo->shared_info->mode_count;
}


status_t
virtio_gpu_get_mode_list(display_mode* modeList)
{
	TRACE(("virtio_gpu_get_mode_info()\n"));
	memcpy(modeList, gInfo->mode_list,
		gInfo->shared_info->mode_count * sizeof(display_mode));
	return B_OK;
}


status_t
virtio_gpu_get_preferred_mode(display_mode* _mode)
{
	TRACE(("virtio_gpu_get_preferred_mode()\n"));
	// the host's display (window) size, which may not be in the mode list
	uint32 width = gInfo->shared_info->preferred_width;
	uint32 height = gInfo->shared_info->preferred_height;
	compute_display_timing(width, height, 60, false, &_mode->timing);
	fill_display_mode(width, height, _mode);
	_mode->space = gInfo->shared_info->current_mode.space;

	return B_OK;
}


status_t
virtio_gpu_set_display_mode(display_mode* _mode)
{
	TRACE(("virtio_gpu_set_display_mode()\n"));
	if (_mode != NULL && *_mode == gInfo->shared_info->current_mode)
		return B_OK;

	return ioctl(gInfo->device, VIRTIO_GPU_SET_DISPLAY_MODE,
		_mode, sizeof(display_mode));
}


status_t
virtio_gpu_get_display_mode(display_mode* _currentMode)
{
	TRACE(("virtio_gpu_get_display_mode()\n"));
	*_currentMode = gInfo->shared_info->current_mode;
	return B_OK;
}


status_t
virtio_gpu_get_edid_info(void* info, size_t size, uint32* _version)
{
	TRACE(("virtio_gpu_get_edid_info()\n"));

	if (!gInfo->shared_info->has_edid)
		return B_ERROR;
	if (size < sizeof(struct edid1_info))
		return B_BUFFER_OVERFLOW;

	edid_decode((edid1_info*)info, &gInfo->shared_info->edid_raw);
	*_version = EDID_VERSION_1;
	edid_dump((edid1_info*)info);
	return B_OK;
}


status_t
virtio_gpu_get_frame_buffer_config(frame_buffer_config* config)
{
	TRACE(("virtio_gpu_get_frame_buffer_config()\n"));

	config->frame_buffer = gInfo->shared_info->frame_buffer;
	TRACE(("virtio_gpu_get_frame_buffer_config() = %" B_PRIxADDR "\n",
		config->frame_buffer));
	//config->frame_buffer_dma = gInfo->shared_info->physical_frame_buffer;
	config->bytes_per_row = gInfo->shared_info->bytes_per_row;
	TRACE(("virtio_gpu_get_frame_buffer_config() %p\n", config->frame_buffer));
	return B_OK;
}


status_t
virtio_gpu_get_pixel_clock_limits(display_mode* mode, uint32* _low, uint32* _high)
{
	TRACE(("virtio_gpu_get_pixel_clock_limits()\n"));

	// TODO: do some real stuff here (taken from radeon driver)
	uint32 totalPixel = (uint32)mode->timing.h_total
		* (uint32)mode->timing.v_total;
	uint32 clockLimit = 2000000;

	// lower limit of about 48Hz vertical refresh
	*_low = totalPixel * 48L / 1000L;
	if (*_low > clockLimit)
		return B_ERROR;

	*_high = clockLimit;
	return B_OK;
}

