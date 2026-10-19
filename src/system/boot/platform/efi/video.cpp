/*
 * Copyright 2016, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "video.h"

#include <stdlib.h>
#include <string.h>

#include <boot/kernel_args.h>
#include <boot/menu.h>
#include <boot/platform.h>
#include <boot/platform/generic/video.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <drivers/driver_settings.h>
#include <edid.h>
#include <util/list.h>

#include "efi_platform.h"
#include <efi/protocol/edid.h>
#include <efi/protocol/graphics-output.h>


//#define TRACE_VIDEO
#ifdef TRACE_VIDEO
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


struct video_mode {
	list_link	link;
	size_t		mode;
	size_t		width, height, bits_per_pixel, bytes_per_row;
};


static efi_guid sGraphicsOutputGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static efi_guid sEdidActiveGuid = EFI_EDID_ACTIVE_PROTOCOL_GUID;
static efi_graphics_output_protocol *sGraphicsOutput;
static efi_edid_protocol *sEdidActiveProtocol;
static size_t sGraphicsMode;
static struct list sModeList;
static uint32 sModeCount;
static bool sModeChosen;
static bool sSettingsLoaded;
static uint8 sFirmwareBackground;

// The gray startup backdrop belongs to MacBooks: their firmware paints the
// screen gray before the loader runs. Anything else keeps the black one.
#define MACBOOK_STARTUP_GRAY	191


static bool
smbios_string(const uint8* strings, const uint8* end, uint8 index,
	const char** out, size_t* outLength)
{
	if (index == 0)
		return false;
	const uint8* at = strings;
	for (uint8 i = 1; at < end && *at != '\0'; i++) {
		const uint8* stop = at;
		while (stop < end && *stop != '\0')
			stop++;
		if (stop >= end)
			return false;
		if (i == index) {
			*out = (const char*)at;
			*outLength = stop - at;
			return true;
		}
		at = stop + 1;
	}
	return false;
}


static bool
smbios_product_is_macbook()
{
	const uint8* start = NULL;
	size_t length = 0;
	const efi_configuration_table* table = kSystemTable->ConfigurationTable;
	for (size_t i = 0; i < kSystemTable->NumberOfTableEntries; i++) {
		const uint8* entry = (const uint8*)table[i].VendorTable;
		if (entry == NULL)
			continue;
		if (table[i].VendorGuid.equals(SMBIOS3_TABLE_GUID)
			&& memcmp(entry, "_SM3_", 5) == 0) {
			uint64 address;
			uint32 size;
			memcpy(&address, entry + 0x10, sizeof(address));
			memcpy(&size, entry + 0x0c, sizeof(size));
			start = (const uint8*)(addr_t)address;
			length = size;
			break;
		}
		if (table[i].VendorGuid.equals(SMBIOS_TABLE_GUID)
			&& memcmp(entry, "_SM_", 4) == 0) {
			uint32 address;
			uint16 size;
			memcpy(&address, entry + 0x18, sizeof(address));
			memcpy(&size, entry + 0x16, sizeof(size));
			start = (const uint8*)(addr_t)address;
			length = size;
		}
	}
	if (start == NULL || length < 4 || length > 0x100000)
		return false;

	const uint8* end = start + length;
	const uint8* at = start;
	while (at + 4 <= end) {
		uint8 type = at[0];
		uint8 formatted = at[1];
		if (formatted < 4 || at + formatted > end)
			return false;
		const uint8* strings = at + formatted;
		const uint8* next = strings;
		while (next + 1 < end && (next[0] != '\0' || next[1] != '\0'))
			next++;
		if (next + 1 >= end)
			return false;
		if (type == 1 && formatted >= 6) {
			// System Information: product name is string 0x05.
			const char* product;
			size_t productLength;
			if (!smbios_string(strings, next + 1, at[5], &product,
					&productLength))
				return false;
			return productLength >= 7 && memcmp(product, "MacBook", 7) == 0;
		}
		if (type == 127)
			return false;
		at = next + 2;
	}
	return false;
}


static bool
video_system_is_macbook()
{
	static int8 sMacBook = -1;
	if (sMacBook < 0) {
		sMacBook = smbios_product_is_macbook() ? 1 : 0;
		dprintf("EFI: startup gray %s (SMBIOS product %s MacBook)\n",
			sMacBook ? "on" : "off", sMacBook ? "is a" : "is not a");
	}
	return sMacBook != 0;
}


void
video_capture_firmware_background()
{
	sFirmwareBackground = 0;
	video_set_splash_background(0);
	if (!video_system_is_macbook())
		return;

	// Apple exposes the startup background as a four-byte RGBA variable.
	// Read it only; changing NVRAM is unnecessary for matching the screen.
	efi_guid appleVendor = {0x4d1ede05, 0x38c7, 0x4a6a,
		{0x9c, 0xc6, 0x4b, 0xcc, 0xa8, 0xb3, 0x8c, 0x14}};
	uint32 color = 0;
	size_t size = sizeof(color);
	efi_status status = kRuntimeServices->GetVariable(
		(char16_t*)u"DefaultBackgroundColor", &appleVendor, NULL, &size, &color);
	uint8 shade = color & 0xff;
	if (status == EFI_SUCCESS && size == sizeof(color)
		&& ((color >> 8) & 0xff) == shade && ((color >> 16) & 0xff) == shade) {
		sFirmwareBackground = shade;
		video_set_splash_background(shade);
		return;
	}

	// Without a neutral firmware color, use the MacBook startup gray.
	sFirmwareBackground = MACBOOK_STARTUP_GRAY;
	video_set_splash_background(MACBOOK_STARTUP_GRAY);
}


uint8
video_firmware_background_gray()
{
	return sFirmwareBackground;
}


void
video_fill_firmware_background()
{
	// Available before VFS/video initialization, while the EFI console starts.
	if (sFirmwareBackground == 0)
		return;
	efi_graphics_output_protocol* output = NULL;
	efi_status status = kBootServices->LocateProtocol(&sGraphicsOutputGuid, NULL,
		(void**)&output);
	if (status != EFI_SUCCESS || output == NULL || output->Mode == NULL
		|| output->Mode->Info == NULL || output->Blt == NULL) {
		return;
	}
	efi_graphics_output_blt_pixel pixel = {};
	pixel.Red = pixel.Green = pixel.Blue = sFirmwareBackground;
	output->Blt(output, &pixel, EfiBltVideoFill, 0, 0, 0, 0,
		output->Mode->Info->HorizontalResolution,
		output->Mode->Info->VerticalResolution, 0);
}


static void
video_override_splash_background()
{
	// A boot manager can clear the firmware screen before invoking us.
	// Allow an explicit neutral shade without changing video-mode selection.
	// Only on a MacBook: elsewhere the backdrop stays black.
	if (!video_system_is_macbook())
		return;
	void* settings = load_driver_settings("vesa");
	if (settings == NULL)
		return;
	const char* value = get_driver_parameter(settings, "splash_background_gray",
		NULL, NULL);
	if (value != NULL && value[0] != '\0') {
		unsigned shade = 0;
		bool valid = true;
		for (const char* digit = value; *digit != '\0'; digit++) {
			if (*digit < '0' || *digit > '9' || shade > 25) {
				valid = false;
				break;
			}
			shade = shade * 10 + (*digit - '0');
		}
		if (valid && shade <= 255) {
			sFirmwareBackground = shade;
			video_set_splash_background(shade);
		}
	}
	unload_driver_settings(settings);
}


static int
compare_video_modes(video_mode *a, video_mode *b)
{
	int compare = a->width - b->width;
	if (compare != 0)
		return compare;

	compare = a->height - b->height;
	if (compare != 0)
		return compare;

	return a->bits_per_pixel - b->bits_per_pixel;
}


static void
add_video_mode(video_mode *videoMode)
{
	video_mode *mode = NULL;
	while ((mode = (video_mode*)list_get_next_item(&sModeList, mode))
			!= NULL) {
		int compare = compare_video_modes(videoMode, mode);
		if (compare == 0) {
			// mode already exists
			return;
		}

		if (compare > 0)
			break;
	}

	list_insert_item_before(&sModeList, mode, videoMode);
	sModeCount++;
}


static video_mode*
closest_video_mode(uint32 width, uint32 height, uint32 depth)
{
	video_mode *bestMode = NULL;
	int64 bestDiff = 0;

	video_mode *mode = NULL;
	while ((mode = (video_mode*)list_get_next_item(&sModeList, mode)) != NULL) {
		if (mode->width > width) {
			// Only choose modes with a width less or equal than the searched
			// one; or else it might well be that the monitor cannot keep up.
			continue;
		}

		int64 diff = 2 * abs((int64)mode->width - width)
			+ abs((int64)mode->height - height)
			+ abs((int64)mode->bits_per_pixel - depth);

		if (bestMode == NULL || bestDiff > diff) {
			bestMode = mode;
			bestDiff = diff;
		}
	}

	return bestMode;
}


static void
get_mode_from_settings(void)
{
	if (sSettingsLoaded)
		return;

	void *handle = load_driver_settings("vesa");
	if (handle == NULL)
		return;

	const driver_settings *settings = get_driver_settings(handle);
	if (settings == NULL)
		goto out;

	sSettingsLoaded = true;

	for (int32 i = 0; i < settings->parameter_count; i++) {
		driver_parameter &parameter = settings->parameters[i];

		if (parameter.value_count < 3 || strcmp(parameter.name, "mode") != 0) continue;
		uint32 width = strtoul(parameter.values[0], NULL, 0);
		uint32 height = strtoul(parameter.values[1], NULL, 0);
		uint32 depth = strtoul(parameter.values[2], NULL, 0);

		// search mode that fits
		video_mode *mode = closest_video_mode(width, height, depth);
		if (mode != NULL) {
			sGraphicsMode = mode->mode;
			break;
		}
	}

out:
	unload_driver_settings(handle);
}


static int
depth_from_mode_info(efi_graphics_output_mode_information* info)
{
	if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
		return 32;
	} else if (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
		// seen this in the wild, but acts like RGB, go figure...
		return 32;
	} else if (info->PixelFormat == PixelBitMask
		&& info->PixelInformation.RedMask == 0x3FF00000
		&& info->PixelInformation.GreenMask == 0x000FFC00
		&& info->PixelInformation.BlueMask == 0x000003FF
		&& info->PixelInformation.ReservedMask == 0xC0000000) {
		return 30;
	} else if (info->PixelFormat == PixelBitMask
		&& info->PixelInformation.RedMask == 0xFF0000
		&& info->PixelInformation.GreenMask == 0x00FF00
		&& info->PixelInformation.BlueMask == 0x0000FF
		&& info->PixelInformation.ReservedMask == 0) {
		return 24;
	} else if (info->PixelFormat == PixelBitMask
		&& info->PixelInformation.RedMask == 0xF800
		&& info->PixelInformation.GreenMask == 0x07E0
		&& info->PixelInformation.BlueMask == 0x001F
		&& info->PixelInformation.ReservedMask == 0) {
		return 16;
	}

	return -1;
}


extern "C" status_t
platform_init_video(void)
{
	list_init(&sModeList);

	// we don't support VESA modes
	gKernelArgs.vesa_modes = NULL;
	gKernelArgs.vesa_modes_size = 0;

	gKernelArgs.edid_info = NULL;

	// make a guess at the best video mode to use, and save the mode ID for switching to graphics
	// mode
	efi_status status = kBootServices->LocateProtocol(&sGraphicsOutputGuid, NULL,
		(void **)&sGraphicsOutput);
	if (sGraphicsOutput == NULL || status != EFI_SUCCESS) {
		dprintf("GOP protocol not found\n");
		gKernelArgs.frame_buffer.enabled = false;
		sGraphicsOutput = NULL;
		return B_ERROR;
	}

	size_t bestArea = 0;
	size_t bestDepth = 0;

	TRACE(("looking for best graphics mode...\n"));
	for (size_t mode = 0; mode < sGraphicsOutput->Mode->MaxMode; ++mode) {
		efi_graphics_output_mode_information *info = NULL;
		size_t size;
		sGraphicsOutput->QueryMode(sGraphicsOutput, mode, &size, &info);
		size_t area = info->HorizontalResolution * info->VerticalResolution;
		TRACE(("  mode: %lu\n", mode));
		TRACE(("  width: %u\n", info->HorizontalResolution));
		TRACE(("  height: %u\n", info->VerticalResolution));
		TRACE(("  area: %lu\n", area));

		int depth = depth_from_mode_info(info);
		if (depth == -1) {
			TRACE(("  pixel format: %x unsupported\n",
				info->PixelFormat));
			continue;
		}
		TRACE(("  depth: %lu\n", depth));

		video_mode *videoMode = (video_mode*)malloc(sizeof(struct video_mode));
		if (videoMode != NULL) {
			videoMode->mode = mode;
			videoMode->width = info->HorizontalResolution;
			videoMode->height = info->VerticalResolution;
			videoMode->bits_per_pixel = depth;
			videoMode->bytes_per_row = info->PixelsPerScanLine * ((depth + 7) / 8);
			add_video_mode(videoMode);
		}

		area *= depth;
		TRACE(("  area (w/depth): %lu\n", area));
		if (area >= bestArea) {
			TRACE(("selected new best mode: %lu\n", mode));
			bestArea = area;
			bestDepth = depth;
			sGraphicsMode = mode;
		}
	}

	if (bestArea == 0 || bestDepth == 0) {
		sGraphicsOutput = NULL;
		gKernelArgs.frame_buffer.enabled = false;
		return B_ERROR;
	}

	gKernelArgs.frame_buffer.enabled = true;
	sModeChosen = false;
	sSettingsLoaded = false;

	status = kBootServices->LocateProtocol(&sEdidActiveGuid, NULL, (void **)&sEdidActiveProtocol);
	if ((sEdidActiveProtocol != NULL) && (status == EFI_SUCCESS)
		&& (sEdidActiveProtocol->SizeOfEdid) != 0) {
		edid1_info* edid_info = (edid1_info*)kernel_args_malloc(sizeof(edid1_info));
		if (edid_info != NULL) {
			edid_decode(edid_info, (edid1_raw*)sEdidActiveProtocol->Edid);
			gKernelArgs.edid_info = edid_info;
		}
	}

	return B_OK;
}


extern "C" void
platform_switch_to_logo(void)
{
	if (sGraphicsOutput == NULL || !gKernelArgs.frame_buffer.enabled)
		return;

	if (!sModeChosen)
		get_mode_from_settings();

	video_override_splash_background();
	sGraphicsOutput->SetMode(sGraphicsOutput, sGraphicsMode);
	video_fill_firmware_background();
	gKernelArgs.frame_buffer.physical_buffer.start =
		sGraphicsOutput->Mode->FrameBufferBase;
	gKernelArgs.frame_buffer.physical_buffer.size =
		sGraphicsOutput->Mode->FrameBufferSize;
	gKernelArgs.frame_buffer.width = sGraphicsOutput->Mode->Info->HorizontalResolution;
	gKernelArgs.frame_buffer.height = sGraphicsOutput->Mode->Info->VerticalResolution;
	int depth = depth_from_mode_info(sGraphicsOutput->Mode->Info);
	gKernelArgs.frame_buffer.depth = depth;
	gKernelArgs.frame_buffer.bytes_per_row = sGraphicsOutput->Mode->Info->PixelsPerScanLine
			* ((depth + 7) / 8);

	// Clear the whole mode to the firmware backdrop before the splash goes up. Without this
	// whatever the firmware left on screen -- on a Mac, the boot picker --
	// stays visible around the splash for the rest of the boot.
	efi_graphics_output_blt_pixel black;
	black.Red = depth == 32 ? sFirmwareBackground : 0;
	black.Green = depth == 32 ? sFirmwareBackground : 0;
	black.Blue = depth == 32 ? sFirmwareBackground : 0;
	black.Reserved = 0;
	sGraphicsOutput->Blt(sGraphicsOutput, &black, EfiBltVideoFill, 0, 0, 0, 0,
		gKernelArgs.frame_buffer.width, gKernelArgs.frame_buffer.height, 0);

	dprintf("EFI: firmware splash background gray %u\n", sFirmwareBackground);
	video_display_splash(gKernelArgs.frame_buffer.physical_buffer.start);
}


bool
video_mode_hook(Menu *menu, MenuItem *item)
{
	Menu* submenu = item->Submenu();
	MenuItem* subitem = submenu->FindMarked();
	if (subitem != NULL) {
		sGraphicsMode = (size_t)subitem->Data();
		sModeChosen = true;
	}

	return true;
}


Menu*
video_mode_menu()
{
	Menu *menu = new(std::nothrow)Menu(CHOICE_MENU, "Select Video Mode");
	MenuItem *item;

	video_mode *mode = NULL;
	while ((mode = (video_mode*)list_get_next_item(&sModeList, mode)) != NULL) {
		char label[64];
		snprintf(label, sizeof(label), "%lux%lu %lu bit", mode->width,
			mode->height, mode->bits_per_pixel);

		menu->AddItem(item = new (std::nothrow)MenuItem(label));
		item->SetData((const void*)mode->mode);
		if (mode->mode == sGraphicsMode) {
			item->SetMarked(true);
			item->Select(true);
		}
	}

	menu->AddSeparatorItem();
	menu->AddItem(item = new(std::nothrow)MenuItem("Return to main menu"));
	item->SetType(MENU_ITEM_NO_CHOICE);

	return menu;
}


extern "C" void
platform_blit4(addr_t frameBuffer, const uint8 *data,
	uint16 width, uint16 height, uint16 imageWidth,
	uint16 left, uint16 top)
{
	panic("platform_blit4 unsupported");
	return;
}


extern "C" void
platform_set_palette(const uint8 *palette)
{
	panic("platform_set_palette unsupported");
	return;
}
