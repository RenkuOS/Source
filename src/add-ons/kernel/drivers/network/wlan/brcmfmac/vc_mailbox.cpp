/*
 * Copyright 2026, Bryan Irvine
 * All rights reserved. Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 *
 * Minimal access to the VideoCore firmware property mailbox and to the GPIO
 * block of the BCM2711, as needed to power up the WLAN chip of the
 * Raspberry Pi 4 / 400.
 */


#include "brcmfmac.h"

#include <string.h>

#include <ACPI.h>
#include "acpi.h"

#include <vm/vm.h>


static const phys_addr_t kMailboxBase = 0xfe00b880;
static const uint32 kMailboxRead = 0x00;
static const uint32 kMailboxStatus = 0x18;
static const uint32 kMailboxWrite = 0x20;
static const uint32 kMailboxFull = 1u << 31;
static const uint32 kMailboxEmpty = 1u << 30;
static const uint32 kMailboxPropertyChannel = 8;
static const uint32 kVideoCoreBusAlias = 0xc0000000;

static const uint32 kTagGetClockRate = 0x00030002;
static const uint32 kTagGetGpioState = 0x00030041;
static const uint32 kTagSetGpioState = 0x00038041;

static const phys_addr_t kGpioBase = 0xfe200000;


static mutex sMailboxLock = MUTEX_INITIALIZER("vc mailbox");


static inline void
clean_invalidate_dcache(void* address, size_t size)
{
#ifdef __aarch64__
	uint64 ctr;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	size_t line = 4 << ((ctr >> 16) & 0xf);
	addr_t start = (addr_t)address & ~(line - 1);
	addr_t end = (addr_t)address + size;
	for (addr_t p = start; p < end; p += line)
		asm volatile("dc civac, %0" : : "r"(p) : "memory");
	asm volatile("dsb sy" : : : "memory");
#endif
}


/*!	Only talk to the VideoCore if the ACPI tables were really made by the
	Raspberry Pi firmware.
*/
bool
is_raspberry_pi_firmware()
{
	acpi_module_info* acpi;
	if (get_module(B_ACPI_MODULE_NAME, (module_info**)&acpi) != B_OK)
		return false;

	bool result = false;
	ACPI_TABLE_HEADER* header;
	if (acpi->get_table(ACPI_SIG_DSDT, 0, (void**)&header) == B_OK)
		result = strncmp(header->OemId, "RPIFDN", 6) == 0;

	put_module(B_ACPI_MODULE_NAME);
	return result;
}


/*!	Performs a single tag property call. \a values holds \a valueWords words
	that are sent to the firmware, and are replaced by its answer.
*/
status_t
vc_mailbox_property(uint32 tag, uint32* values, uint32 valueWords)
{
	if (valueWords > 16)
		return B_BAD_VALUE;

	MutexLocker locker(sMailboxLock);

	// The firmware can only access the first GB of RAM.
	void* bufferAddress;
	virtual_address_restrictions virtualRestrictions = {};
	virtualRestrictions.address_specification = B_ANY_KERNEL_ADDRESS;
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = 0x40000000;
	area_id bufferArea = create_area_etc(B_SYSTEM_TEAM, "vc mailbox buffer",
		B_PAGE_SIZE, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		0, 0, &virtualRestrictions, &physicalRestrictions, &bufferAddress);
	if (bufferArea < 0)
		return bufferArea;

	physical_entry entry;
	if (get_memory_map(bufferAddress, B_PAGE_SIZE, &entry, 1) != B_OK) {
		delete_area(bufferArea);
		return B_ERROR;
	}

	void* mailboxPage;
	area_id mailboxArea = map_physical_memory("vc mailbox",
		kMailboxBase & ~(B_PAGE_SIZE - 1), B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&mailboxPage);
	if (mailboxArea < 0) {
		delete_area(bufferArea);
		return mailboxArea;
	}
	volatile uint32* mailbox = (volatile uint32*)((addr_t)mailboxPage
		+ (kMailboxBase & (B_PAGE_SIZE - 1)));

	volatile uint32* message = (volatile uint32*)bufferAddress;
	uint32 words = 0;
	message[words++] = 0;				// total size, filled in below
	message[words++] = 0;				// request
	message[words++] = tag;
	message[words++] = valueWords * 4;	// value buffer size
	message[words++] = 0;				// request code
	for (uint32 i = 0; i < valueWords; i++)
		message[words++] = values[i];
	message[words++] = 0;				// end tag
	message[0] = words * 4;
	clean_invalidate_dcache(bufferAddress, B_PAGE_SIZE);

	status_t status = B_TIMED_OUT;
	uint32 busAddress = (uint32)entry.address | kVideoCoreBusAlias;
	bigtime_t timeout = system_time() + 200000;

	// drain stale replies
	while ((mailbox[kMailboxStatus / 4] & kMailboxEmpty) == 0
		&& system_time() < timeout) {
		(void)mailbox[kMailboxRead / 4];
	}
	while ((mailbox[kMailboxStatus / 4] & kMailboxFull) != 0
		&& system_time() < timeout) {
		spin(1);
	}

	if (system_time() < timeout) {
		mailbox[kMailboxWrite / 4] = busAddress | kMailboxPropertyChannel;

		while (system_time() < timeout) {
			if ((mailbox[kMailboxStatus / 4] & kMailboxEmpty) != 0) {
				spin(10);
				continue;
			}
			uint32 reply = mailbox[kMailboxRead / 4];
			if ((reply & 0xf) != kMailboxPropertyChannel)
				continue;

			clean_invalidate_dcache(bufferAddress, B_PAGE_SIZE);
			if (message[1] == 0x80000000 && (message[4] & 0x80000000) != 0) {
				for (uint32 i = 0; i < valueWords; i++)
					values[i] = message[5 + i];
				status = B_OK;
			} else
				status = B_ERROR;
			break;
		}
	}

	delete_area(mailboxArea);
	delete_area(bufferArea);
	return status;
}


uint32
vc_get_clock_rate(uint32 clockId)
{
	uint32 values[2] = { clockId, 0 };
	if (vc_mailbox_property(kTagGetClockRate, values, 2) != B_OK
		|| values[0] != clockId) {
		return 0;
	}
	return values[1];
}


status_t
vc_set_gpio(uint32 gpio, uint32 state)
{
	uint32 values[2] = { gpio, state };
	return vc_mailbox_property(kTagSetGpioState, values, 2);
}


status_t
vc_get_gpio(uint32 gpio, uint32* state)
{
	uint32 values[2] = { gpio, 0 };
	status_t status = vc_mailbox_property(kTagGetGpioState, values, 2);
	if (status == B_OK)
		*state = values[1];
	return status;
}


/*!	Routes GPIO 34-39 to the Arasan controller (ALT3) and sets up the pulls
	the SDIO bus needs: none on CLK, pull-up on CMD and DAT0-3.
	The UEFI firmware normally already did the routing, but it leaves the
	(partly pull-down) reset defaults for the pulls.
*/
void
bcm2711_wifi_pin_setup()
{
	void* page;
	area_id area = map_physical_memory("bcm2711 gpio", kGpioBase, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &page);
	if (area < 0) {
		ERROR("cannot map GPIO registers\n");
		return;
	}
	volatile uint32* gpio = (volatile uint32*)page;

	// GPFSEL3 covers GPIO 30-39, 3 bits each
	uint32 fsel = gpio[0x0c / 4];
	uint32 newFsel = fsel;
	for (int pin = 34; pin <= 39; pin++) {
		int shift = (pin - 30) * 3;
		newFsel = (newFsel & ~(7u << shift)) | (7u << shift);	// ALT3
	}
	if (newFsel != fsel) {
		INFO("routing GPIO 34-39 to the SDIO controller (fsel3 %#" B_PRIx32
			" -> %#" B_PRIx32 ")\n", fsel, newFsel);
		gpio[0x0c / 4] = newFsel;
	}

	// GPIO_PUP_PDN_CNTRL_REG2 covers GPIO 32-47, 2 bits each
	uint32 pull = gpio[0xec / 4];
	uint32 newPull = pull;
	for (int pin = 34; pin <= 39; pin++) {
		int shift = (pin - 32) * 2;
		uint32 value = pin == 34 ? 0 : 1;	// none on CLK, up elsewhere
		newPull = (newPull & ~(3u << shift)) | (value << shift);
	}
	if (newPull != pull) {
		TRACE("GPIO pulls %#" B_PRIx32 " -> %#" B_PRIx32 "\n", pull, newPull);
		gpio[0xec / 4] = newPull;
	}

	delete_area(area);
}
