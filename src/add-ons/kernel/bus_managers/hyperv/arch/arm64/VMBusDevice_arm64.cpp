/*
 * Copyright 2026, Dru Satori, dru@druware.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#include "hyperv_cpu.h"
#include "VMBusDevicePrivate.h"


status_t
VMBusDevice::GetReferenceCounter(uint64* _counter)
{
	if (!fReferenceCounterSupported)
		return B_NOT_SUPPORTED;

	uint64 low, high;
	if (hv_get_vpreg(HV_REG_TIME_REF_COUNT, &low, &high) != 0)
		return B_ERROR;

	*_counter = low;
	return B_OK;
}


bool
VMBusDevice::_IsReferenceCounterSupported()
{
	return (hv_get_vpreg64(HV_REG_PRIVILEGES_AND_FEATURES)
		& HV_CPUID_FEATURE_TIME_REF_COUNTER) != 0;
}
