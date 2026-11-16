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
	return B_NOT_SUPPORTED;
}


bool
VMBusDevice::_IsReferenceCounterSupported()
{
	return false;
}
