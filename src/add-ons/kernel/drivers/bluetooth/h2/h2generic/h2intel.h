/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef _H2INTEL_H_
#define _H2INTEL_H_


#include <USB3.h>

#include "h2generic.h"


bool intel_needs_firmware(const usb_device_descriptor* descriptor);
status_t intel_load_firmware(bt_usb_dev* bdev);


#endif
