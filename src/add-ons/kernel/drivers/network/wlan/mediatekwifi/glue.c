/*
 * Copyright 2026, Kevin Adams, kevinadams05@gmail.com.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */

#include <sys/haiku-module.h>


HAIKU_FBSD_WLAN_DRIVER_GLUE(mediatekwifi, mwx, pci)
NO_HAIKU_FBSD_MII_DRIVER();
NO_HAIKU_REENABLE_INTERRUPTS();
HAIKU_DRIVER_REQUIREMENTS(OBSD_WLAN);
HAIKU_FIRMWARE_VERSION(0);
NO_HAIKU_FIRMWARE_NAME_MAP();


int
HAIKU_CHECK_DISABLE_INTERRUPTS(device_t dev)
{
	// Only MSI and MSI-X are used for now (see mwx_attach), so every
	// interrupt on our vector is ours. mwx_intr() masks and unmasks the
	// device itself.
	return 1;
}
