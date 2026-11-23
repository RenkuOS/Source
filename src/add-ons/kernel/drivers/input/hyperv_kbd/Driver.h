/*
 * Copyright 2026, Dru Satori, dru@druware.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef _HYPERV_KBD_DRIVER_H_
#define _HYPERV_KBD_DRIVER_H_

#include <Drivers.h>
#include <KernelExport.h>
#include <OS.h>
#include <util/kernel_cpp.h>

#define DRIVER_NAME			"hyperv_kbd"

//#define TRACE_HYPERV_KBD
#ifdef TRACE_HYPERV_KBD
#	define TRACE(x...) dprintf("\33[94mhyperv_kbd:\33[0m " x)
#else
#	define TRACE(x...) ;
#endif
#define TRACE_ALWAYS(x...)	dprintf("\33[94mhyperv_kbd:\33[0m " x)
#define ERROR(x...)			dprintf("\33[94mhyperv_kbd:\33[0m " x)
#define CALLED(x...)		TRACE("CALLED %s\n", __PRETTY_FUNCTION__)

#define HYPERV_KBD_DRIVER_MODULE_NAME		"drivers/input/hyperv_kbd/driver_v1"
#define HYPERV_KBD_DEVICE_MODULE_NAME		"drivers/input/hyperv_kbd/device_v1"
#define HYPERV_KBD_PUBLISH_PATH				"input/keyboard/hyperv_kbd/0"


#endif // _HYPERV_KBD_DRIVER_H_
