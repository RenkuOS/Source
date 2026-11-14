/* SPDX-License-Identifier: MIT */
#include "linux/types.h"

#ifndef BRCMS_LINUX_LEDS_H
#define BRCMS_LINUX_LEDS_H

// The radio LED, which this driver does not drive.
struct led_classdev {
	int unused;
};

#endif
