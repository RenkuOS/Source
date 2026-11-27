/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef CHARGE_LIMIT_H
#define CHARGE_LIMIT_H


#include <SupportDefs.h>


struct charge_limit {
			uint32				start;
			uint32				stop;
};


// The choices of the charge limit menu, from a full charge down.
extern const charge_limit kChargeLimits[];
extern const int32 kChargeLimitCount;


int32	charge_limit_index(const uint32* stops, int32 count);


#endif	// CHARGE_LIMIT_H
