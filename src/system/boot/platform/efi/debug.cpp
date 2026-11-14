/*
 * Copyright 2016-2022 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "debug.h"

#include <string.h>

#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <kernel.h>
#include <util/ring_buffer.h>

#include "efi_platform.h"
#include "mmu.h"
#include "serial.h"


static const char* const kDebugSyslogSignature = "Haiku syslog";

// The same place the BIOS loader keeps it. A fixed address lets the next boot
// find what a session that never reached the syslog daemon had written.
static const phys_addr_t kDebugSyslogAddress = 63 * 1024 * 1024;
static const size_t kDebugSyslogSize = 1024 * 1024;

static char sBuffer[16384];
static uint32 sBufferPosition;

static void* sDebugSyslogMemory = NULL;
static ring_buffer* sDebugSyslogBuffer = NULL;
static bool sPostCleanup = false;


static void
syslog_write(const char* buffer, size_t length)
{
	if (sPostCleanup && sDebugSyslogBuffer != NULL) {
		ring_buffer_write(sDebugSyslogBuffer, (const uint8*)buffer, length);
	} else if (sBufferPosition + length < sizeof(sBuffer)) {
		memcpy(sBuffer + sBufferPosition, buffer, length);
		sBufferPosition += length;
	}
}


static void
dprintf_args(const char *format, va_list args)
{
	char buffer[512];
	int length = vsnprintf(buffer, sizeof(buffer), format, args);
	if (length == 0)
		return;

	if (length >= (int)sizeof(buffer))
		length = sizeof(buffer) - 1;

	syslog_write(buffer, length);
	serial_puts(buffer, length);
}


extern "C" void
dprintf(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	dprintf_args(format, args);
	va_end(args);
}


extern "C" void
panic(const char *format, ...)
{
	va_list args;

	platform_switch_to_text_mode();

	puts("*** PANIC ***");

	va_start(args, format);
	vprintf(format, args);
	va_end(args);

	while (true)
		kBootServices->Stall(1000000);
}


/*!	Must run before anything else allocates memory, or the previous
	session's buffer may already have been handed out and overwritten.
	The heap does not exist yet, so the pages are only claimed here.
*/
void
debug_claim_syslog_memory(void)
{
	efi_physical_addr address = kDebugSyslogAddress;
	if (kBootServices->AllocatePages(AllocateAddress, EfiLoaderData,
			kDebugSyslogSize / B_PAGE_SIZE, &address) == EFI_SUCCESS)
		sDebugSyslogMemory = (void*)(addr_t)address;
}


static void
allocate_ring_buffer(void)
{
	void* buffer = sDebugSyslogMemory;
	size_t size = kDebugSyslogSize;
	bool recover = false;

	if (buffer != NULL) {
		if (platform_add_allocated_region(buffer, size) != B_OK)
			return;

		// check whether there's a previous syslog we can recover
		recover = memcmp(buffer, kDebugSyslogSignature,
			strlen(kDebugSyslogSignature)) == 0;
	} else if (platform_allocate_region(&buffer, size, 0) != B_OK)
		return;

	size_t offset = ROUNDUP(strlen(kDebugSyslogSignature), sizeof(void*));
	sDebugSyslogBuffer = create_ring_buffer_etc((uint8*)buffer + offset,
		size - offset, recover ? RING_BUFFER_INIT_FROM_BUFFER : 0);

	// The kernel builds its ring buffer over the same memory with this size,
	// so it has to include the header, or the two would wrap at different
	// places and the next boot would read the end of the buffer wrong.
	gKernelArgs.debug_output = sDebugSyslogBuffer;
	gKernelArgs.debug_size = size - offset;
}


void
debug_cleanup(void)
{
	allocate_ring_buffer();

	if (sDebugSyslogBuffer != NULL) {
		// If desired, store the debug syslog data from the previous session for
		// the kernel.
		size_t bytesReadable = 0;
		if (gKernelArgs.previous_debug_size != 0) {
			bytesReadable = ring_buffer_readable(sDebugSyslogBuffer);
			gKernelArgs.previous_debug_size = bytesReadable;
		}

		if (bytesReadable != 0) {
			if (uint8* buffer = (uint8*)kernel_args_malloc(bytesReadable)) {
				ring_buffer_read(sDebugSyslogBuffer, buffer, bytesReadable);
				gKernelArgs.previous_debug_output = buffer;
			} else
				gKernelArgs.previous_debug_size = 0;
		}

		dprintf("EFI: debug syslog at %#" B_PRIxADDR "%s, previous session "
			"%" B_PRIu32 " bytes\n", (addr_t)sDebugSyslogBuffer,
			sDebugSyslogMemory != NULL ? "" : " (not the fixed address)",
			(uint32)gKernelArgs.previous_debug_size);

		// Prepare the debug syslog buffer for this session.
		size_t signatureLength = strlen(kDebugSyslogSignature);
		void* buffer
			= (void*)ROUNDDOWN((addr_t)sDebugSyslogBuffer, B_PAGE_SIZE);

		if (gKernelArgs.keep_debug_output_buffer) {
			// copy the output gathered so far into the ring buffer
			ring_buffer_clear(sDebugSyslogBuffer);
			ring_buffer_write(sDebugSyslogBuffer, (uint8*)sBuffer,
				sBufferPosition);

			memcpy(buffer, kDebugSyslogSignature, signatureLength);
		} else {
			// clear the signature
			memset(buffer, 0, signatureLength);
		}
	} else {
		gKernelArgs.keep_debug_output_buffer = false;
		gKernelArgs.previous_debug_size = 0;
	}

	if (!gKernelArgs.keep_debug_output_buffer) {
		gKernelArgs.debug_output = kernel_args_malloc(sBufferPosition);
		if (gKernelArgs.debug_output != NULL) {
			memcpy(gKernelArgs.debug_output, sBuffer, sBufferPosition);
			gKernelArgs.debug_size = sBufferPosition;
		}
	}

	sPostCleanup = true;
}


char*
platform_debug_get_log_buffer(size_t *_size)
{
	if (_size != NULL)
		*_size = sizeof(sBuffer);

	return sBuffer;
}
