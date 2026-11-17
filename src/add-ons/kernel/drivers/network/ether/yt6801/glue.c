/*
 * Copyright 2026, The RenkuOS authors.
 * SPDX-License-Identifier: MIT
 */

#include <sys/bus.h>
#include <sys/haiku-module.h>


HAIKU_FBSD_DRIVER_GLUE(yt6801, eqos, pci);
HAIKU_DRIVER_REQUIREMENTS(FBSD_FAST_TASKQUEUE);
HAIKU_FBSD_MII_DRIVER(mcphy);
NO_HAIKU_REENABLE_INTERRUPTS();


int
HAIKU_CHECK_DISABLE_INTERRUPTS(device_t dev)
{
	/* One unshared MSI vector: every interrupt is ours, eqos_intr() acks */
	return 1;
}
