/*
 * Copyright 2016 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <boot/menu.h>
#include <boot/platform/generic/text_menu.h>
#include <safemode.h>

#include "efi_platform.h"
#include "video.h"
#include "smp.h"


// Some Bay Trail tablets (Teclast X98 Air III and others with firmware by
// Wisky) carry a 32-bit and a 64-bit UEFI in one ROM. Byte 0 of this
// variable selects which one starts next time; Android's "Start Windows"
// tool clears bits 1, 2 and 5 and sets bit 0 for 32-bit mode (0x3B in
// 64-bit mode became 0x19). The firmware does the rest itself at the next
// boot.
static efi_guid sBootSelectGUID = { 0x944fb13b, 0x773f, 0x4cbb,
	{ 0x9c, 0x6f, 0x32, 0x6c, 0xeb, 0xde, 0x4c, 0x23 } };
static char16_t sBootSelectName[] = u"BootSelectVariable";

#define BOOT_SELECT_32_CLEAR	0x26
#define BOOT_SELECT_32_SET		0x01
#define BOOT_SELECT_64_CLEAR	0x04
#define BOOT_SELECT_64_SET		0x22


static bool
read_boot_select(uint32_t* attributes, size_t* size, uint8_t* data,
	size_t capacity)
{
	*size = capacity;
	efi_status status = kRuntimeServices->GetVariable(sBootSelectName,
		&sBootSelectGUID, attributes, size, data);
	return status == EFI_SUCCESS && *size >= 1;
}


static bool
firmware_mode_hook(Menu* menu, MenuItem* item)
{
	uint8_t data[64];
	uint32_t attributes;
	size_t size;
	if (!read_boot_select(&attributes, &size, data, sizeof(data))) {
		dprintf("firmware mode: BootSelectVariable not readable\n");
		return false;
	}

	uint8_t old = data[0];
#if defined(__x86_64__)
	// Running in 64-bit mode: switch to 32-bit
	data[0] = (data[0] & ~BOOT_SELECT_32_CLEAR) | BOOT_SELECT_32_SET;
#else
	data[0] = (data[0] & ~BOOT_SELECT_64_CLEAR) | BOOT_SELECT_64_SET;
#endif
	efi_status status = kRuntimeServices->SetVariable(sBootSelectName,
		&sBootSelectGUID, attributes, size, data);
	dprintf("firmware mode: BootSelectVariable %#x -> %#x: %s\n", old,
		data[0], status == EFI_SUCCESS ? "ok, restarting" : "failed");
	if (status != EFI_SUCCESS)
		return false;

	kRuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
	return true;
}


static void
add_firmware_mode_item(Menu* menu)
{
	uint8_t data[64];
	uint32_t attributes;
	size_t size;
	if (!read_boot_select(&attributes, &size, data, sizeof(data)))
		return;

#if defined(__x86_64__)
	const char* label = "Restart with the 32-bit firmware";
#else
	const char* label = "Restart with the 64-bit firmware";
#endif
	MenuItem* item = new(std::nothrow) MenuItem(label);
	if (item == NULL)
		return;
	menu->AddItem(item);
	item->SetTarget(firmware_mode_hook);
	item->SetHelpText("This firmware has a 32-bit and a 64-bit UEFI. "
		"Switches to the other one and restarts the computer.");
}


void
platform_add_menus(Menu *menu)
{
	MenuItem *item;

	switch (menu->Type()) {
		case MAIN_MENU:
			item = new(std::nothrow)MenuItem("Select video mode", video_mode_menu());
			if (item != NULL) {
				menu->AddItem(item);
				item->SetTarget(video_mode_hook);
				item->SetShortcut('v');
			}

			add_firmware_mode_item(menu);
			break;

		case SAFE_MODE_MENU:
			item = new(std::nothrow) MenuItem("Use fail-safe graphics driver");
			if (item != NULL) {
				menu->AddItem(item);
				item->SetType(MENU_ITEM_MARKABLE);
				item->SetData(B_SAFEMODE_FAIL_SAFE_VIDEO_MODE);
				item->SetHelpText("The system will use VESA mode "
					"and won't try to use any video graphics drivers.");
			}

			smp_add_safemode_menus(menu);

			item = new(std::nothrow) MenuItem("Disable ACPI");
			if (item != NULL) {
				menu->AddItem(item);
				item->SetType(MENU_ITEM_MARKABLE);
				item->SetData(B_SAFEMODE_DISABLE_ACPI);
				item->SetHelpText("Disables Advanced Configuration and Power "
					"Interface hardware support, overriding the ACPI setting "
					"in the kernel settings file.");
			}
			break;

		default:
			break;
	}
}


void
platform_update_menu_item(Menu *menu, MenuItem *item)
{
	platform_generic_update_text_menu_item(menu, item);
}


void
platform_run_menu(Menu *menu)
{
	platform_generic_run_text_menu(menu);
}


size_t
platform_get_user_input_text(Menu *menu, MenuItem *item, char *buffer,
	size_t bufferSize)
{
	return platform_generic_get_user_input_text(menu, item, buffer,
		bufferSize);
}
