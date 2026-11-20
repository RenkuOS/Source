/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */
#ifndef _BAYTRAIL_GPIO_H
#define _BAYTRAIL_GPIO_H


#include <module.h>


/*!	Access to single pins of the Intel Bay Trail GPIO controllers, for drivers
	that have to drive a pin that the firmware does not describe (or describes
	wrongly). A community is named "SCORE", "NCORE" or "SUS", a pin is its
	GPIO number within the community, as in ACPI.
*/
#define BAYTRAIL_GPIO_PINS_MODULE_NAME	"drivers/gpio/baytrail_gpio/pins/v1"

typedef struct baytrail_gpio_pins_module_info {
	module_info	info;

	status_t	(*set_output)(const char* community, uint32 pin, bool high);
	status_t	(*set_input)(const char* community, uint32 pin);
} baytrail_gpio_pins_module_info;


#endif	// _BAYTRAIL_GPIO_H
