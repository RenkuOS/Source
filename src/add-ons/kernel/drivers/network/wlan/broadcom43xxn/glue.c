/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	What tells Haiku this is one of its wireless drivers.

	The compatibility layer under src/libs/compat expects a FreeBSD driver
	and a few declarations saying how to treat it. Two of them are about the
	interrupt: one runs in the interrupt itself and says whether the line is
	this card's, quieting it if so, and the other lets the card ask again
	once the frames have been taken off the ring.
*/


#include <sys/bus.h>
#include <sys/kernel.h>

#include <machine/bus.h>

#include <net/if.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>

#include <driver_settings.h>

#include "brcms/glue_net.h"


HAIKU_FBSD_WLAN_DRIVER_GLUE(broadcom43xxn, brcms, pci)
NO_HAIKU_FBSD_MII_DRIVER();
HAIKU_DRIVER_REQUIREMENTS(FBSD_WLAN | FBSD_THREAD_TASKQUEUE);


// What the settings file said, with the defaults for anything it did not.
static struct brcms_glue_options sOptions = {
	.rate_table = true,
	.mcs = true,
	.phy_watchdog = false,
	.stf_chain = false,
	.ampdu = false,
	.one_stream = false,
	.basic_rates = true,
	.ba_template = true,
	.listen_in = false,
	.ht40 = false,
	.band5 = false,
	.band2 = true,
	.edcf = false,
	.full_chanspec = false,
	.tx_inflight = 1,
};


const struct brcms_glue_options*
brcms_glue_settings(void)
{
	return &sOptions;
}


void
brcms_glue_read_settings(void)
{
	void* handle = load_driver_settings("broadcom43xxn");
	if (handle == NULL)
		return;

	sOptions.rate_table = get_driver_boolean_parameter(handle,
		"rate_table", sOptions.rate_table, sOptions.rate_table);
	sOptions.mcs = get_driver_boolean_parameter(handle,
		"mcs", sOptions.mcs, sOptions.mcs);
	sOptions.phy_watchdog = get_driver_boolean_parameter(handle,
		"phy_watchdog", sOptions.phy_watchdog, sOptions.phy_watchdog);
	sOptions.full_chanspec = get_driver_boolean_parameter(handle,
		"full_chanspec", sOptions.full_chanspec, sOptions.full_chanspec);
	sOptions.stf_chain = get_driver_boolean_parameter(handle,
		"stf_chain", sOptions.stf_chain, sOptions.stf_chain);
	sOptions.ampdu = get_driver_boolean_parameter(handle,
		"ampdu", sOptions.ampdu, sOptions.ampdu);
	sOptions.one_stream = get_driver_boolean_parameter(handle,
		"one_stream", sOptions.one_stream, sOptions.one_stream);
	sOptions.basic_rates = get_driver_boolean_parameter(handle,
		"basic_rates", sOptions.basic_rates, sOptions.basic_rates);
	sOptions.ba_template = get_driver_boolean_parameter(handle,
		"ba_template", sOptions.ba_template, sOptions.ba_template);
	sOptions.listen_in = get_driver_boolean_parameter(handle,
		"listen_in", sOptions.listen_in, sOptions.listen_in);
	sOptions.ht40 = get_driver_boolean_parameter(handle,
		"ht40", sOptions.ht40, sOptions.ht40);
	sOptions.band5 = get_driver_boolean_parameter(handle,
		"band5", sOptions.band5, sOptions.band5);
	sOptions.band2 = get_driver_boolean_parameter(handle,
		"band2", sOptions.band2, sOptions.band2);
	sOptions.edcf = get_driver_boolean_parameter(handle,
		"edcf", sOptions.edcf, sOptions.edcf);

	// How many hardware frames may wait in the chip while the next
	// acknowledgement batch is prepared. Fewer keeps every TCP ACK that
	// reaches the air fresher; more keeps the chip from going idle.
	const char* inflight = get_driver_parameter(handle, "tx_inflight",
		NULL, NULL);
	if (inflight != NULL && inflight[0] >= '1' && inflight[0] <= '8'
		&& inflight[1] == '\0')
		sOptions.tx_inflight = inflight[0] - '0';

	unload_driver_settings(handle);

	dprintf("broadcom43xxn: settings: rate_table %d, mcs %d, phy_watchdog %d, "
		"full_chanspec %d, stf_chain %d, ampdu %d\n", sOptions.rate_table,
		sOptions.mcs, sOptions.phy_watchdog, sOptions.full_chanspec,
		sOptions.stf_chain, sOptions.ampdu);
	dprintf("broadcom43xxn: settings: one_stream %d, basic_rates %d, "
		"ba_template %d, listen_in %d, ht40 %d, band5 %d, band2 %d, "
		"edcf %d, tx_inflight %u\n",
		sOptions.one_stream, sOptions.basic_rates, sOptions.ba_template,
		sOptions.listen_in, sOptions.ht40, sOptions.band5, sOptions.band2,
		sOptions.edcf, sOptions.tx_inflight);
}
HAIKU_FIRMWARE_VERSION(0);
NO_HAIKU_FIRMWARE_NAME_MAP();


int
HAIKU_CHECK_DISABLE_INTERRUPTS(device_t dev)
{
	return brcms_glue_interrupt_claim() ? 1 : 0;
}


void
HAIKU_REENABLE_INTERRUPTS(device_t dev)
{
	brcms_glue_interrupt_enable();
}
