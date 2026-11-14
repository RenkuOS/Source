/*
 * Copyright 2006-2022, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <arch/generic/debug_uart.h>


void
DebugUART::SetRegisterLayout(uint8 regShift, uint8 regIOWidth)
{
	fRegShift = regShift;
	if (regIOWidth == 1 || regIOWidth == 2 || regIOWidth == 4)
		fRegIOWidth = regIOWidth;
}


void
DebugUART::Out8(int reg, uint8 value)
{
#if defined(__i386__) || defined(__x86_64__)
	// outb for access to IO space.
	if ((Base() + reg) <= 0xFFFF) {
		__asm__ volatile ("outb %%al,%%dx" : : "a" (value), "d" (Base() + reg));
		return;
	}
#endif
	addr_t address = Base() + ((addr_t)reg << fRegShift);
	switch (fRegIOWidth) {
		case 4:
			*(volatile uint32*)address = value;
			break;
		case 2:
			*(volatile uint16*)address = value;
			break;
		default:
			*(volatile uint8*)address = value;
			break;
	}
}


uint8
DebugUART::In8(int reg)
{
#if defined(__i386__) || defined(__x86_64__)
	// inb for access to IO space.
	if ((Base() + reg) <= 0xFFFF) {
		uint8 _v;
		__asm__ volatile ("inb %%dx,%%al" : "=a" (_v) : "d" (Base() + reg));
		return _v;
	}
#endif
	addr_t address = Base() + ((addr_t)reg << fRegShift);
	switch (fRegIOWidth) {
		case 4:
			return *(volatile uint32*)address;
		case 2:
			return *(volatile uint16*)address;
		default:
			return *(volatile uint8*)address;
	}
}


void
DebugUART::Barrier()
{
	// Simple memory barriers
#if defined(__POWERPC__)
	asm volatile("eieio; sync");
#elif defined(__ARM__) || defined(__aarch64__)
	asm volatile ("" : : : "memory");
#endif
}
