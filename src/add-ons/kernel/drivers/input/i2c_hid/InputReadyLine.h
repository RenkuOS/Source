/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */
#ifndef I2C_HID_INPUT_READY_LINE_H
#define I2C_HID_INPUT_READY_LINE_H


#include <ACPI.h>
#include <OS.h>


// The interrupt line of an I2C HID device, read as a level. The device
// asserts the line while an input report is ready. Some devices return
// their last report again when the host reads the input register while
// no report is ready.
class InputReadyLine {
public:
								InputReadyLine();
								~InputReadyLine();

			status_t			Init(acpi_module_info* acpi,
									acpi_handle device);

			bool				IsAsserted() const;

private:
			area_id				fArea;
			volatile uint32*	fPadConfig;
			uint32				fStateMask;
			bool				fActiveLow;
};


#endif // I2C_HID_INPUT_READY_LINE_H
