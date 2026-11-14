/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	What brcmsmac logs through.

	This stands in for the driver's own debug.h, which reaches into Linux's
	device model and its tracepoints for nothing this driver needs. The
	level is dropped: during a bring up everything is worth reading.
*/
#ifndef BRCMS_DEBUG_HAIKU_H
#define BRCMS_DEBUG_HAIKU_H


#include "linux/types.h"


struct device;

void __brcms_info(struct device* dev, const char* format, ...);
void __brcms_warn(struct device* dev, const char* format, ...);
void __brcms_err(struct device* dev, const char* format, ...);
void __brcms_crit(struct device* dev, const char* format, ...);

#define brcms_info(core, f, a...)	__brcms_info(NULL, f, ##a)
#define brcms_warn(core, f, a...)	__brcms_warn(NULL, f, ##a)
#define brcms_err(core, f, a...)	__brcms_err(NULL, f, ##a)
#define brcms_crit(core, f, a...)	__brcms_crit(NULL, f, ##a)

// The tracing is off, as it is upstream without CONFIG_BRCMDBG: these fire
// hundreds of times during one initialisation, and what matters has to fit
// on a screen so it can be read off the machine.
#define brcms_dbg(core, l, f, a...)			do {} while (0)
#define brcms_dbg_info(core, f, a...)		do {} while (0)
#define brcms_dbg_mac80211(core, f, a...)	do {} while (0)
#define brcms_dbg_rx(core, f, a...)			do {} while (0)
#define brcms_dbg_tx(core, f, a...)			do {} while (0)
#define brcms_dbg_int(core, f, a...)		do {} while (0)
#define brcms_dbg_dma(core, f, a...)		do {} while (0)
#define brcms_dbg_ht(core, f, a...)			do {} while (0)


#endif	// BRCMS_DEBUG_HAIKU_H
