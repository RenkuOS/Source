/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


/*!	The logical efuse map of the RTL8723B has the layout of the RTL8192E up
	to 0xd0 (transmit power, crystal, thermal meter, RF options); the SDIO
	part keeps the MAC address at 0x11a (rtw88, struct rtw8723xs_efuse).
*/


#include "opt_wlan.h"

#include <sys/param.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>
#include <sys/bus.h>
#include <sys/endian.h>

#include <net/if.h>
#include <net/ethernet.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>

#include <dev/rtwn/if_rtwnreg.h>
#include <dev/rtwn/if_rtwnvar.h>
#include <dev/rtwn/if_rtwn_debug.h>

#include <dev/rtwn/rtl8192e/r92e.h>
#include <dev/rtwn/rtl8192e/r92e_var.h>

#include <dev/rtwn/rtl8723b/r8723b.h>


#define R8723B_ROM_ID			0x00
#define R8723B_ROM_ID_VALUE		0x8129
#define R8723B_ROM_SDIO_MACADDR		0x11a
#define R8723B_ROM_BT_SETTING		0xc3


void
r8723b_parse_rom_sdio(struct rtwn_softc *sc, uint8_t *buf)
{
	struct r92e_softc *rs = sc->sc_priv;
	uint16_t id = buf[R8723B_ROM_ID] | buf[R8723B_ROM_ID + 1] << 8;

	if (id != R8723B_ROM_ID_VALUE) {
		device_printf(sc->sc_dev, "efuse id %#x, expected %#x\n", id,
		    R8723B_ROM_ID_VALUE);
	}

	r92e_parse_rom(sc, buf);
	((struct r8723b_softc *)sc->sc_priv)->bt_setting =
	    buf[R8723B_ROM_BT_SETTING];

	IEEE80211_ADDR_COPY(sc->sc_ic.ic_macaddr,
	    &buf[R8723B_ROM_SDIO_MACADDR]);

	RTWN_DPRINTF(sc, RTWN_DEBUG_ROM, "efuse: crystal cap %#x, thermal meter "
	    "%#x, regulatory %d, BT setting %#x\n", rs->crystalcap,
	    sc->thermal_meter, rs->regulatory, buf[R8723B_ROM_BT_SETTING]);
}
