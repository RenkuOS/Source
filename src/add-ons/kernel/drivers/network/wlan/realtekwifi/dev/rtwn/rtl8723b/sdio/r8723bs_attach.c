/*-
 * Copyright (c) 2017 Kevin Lo <kevlo@FreeBSD.org>
 * Copyright 2026 Akakor. All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Methods of the RTL8723BS, based on r92eu_attach.c: the MAC of the
 * RTL8723B is of the RTL8192E generation (40 byte Tx descriptor, 24 byte Rx
 * descriptor, the same efuse layout and firmware commands), with one RF
 * chain and its own power, BB and RF setup.
 */

#include <sys/cdefs.h>
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
#include <sys/linker.h>

#include <net/if.h>
#include <net/ethernet.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>

#include <dev/rtwn/if_rtwnreg.h>
#include <dev/rtwn/if_rtwnvar.h>

#include <dev/rtwn/if_rtwn_nop.h>

#include <dev/rtwn/sdio/rtwn_sdio_reg.h>
#include <dev/rtwn/sdio/rtwn_sdio_var.h>

#include <dev/rtwn/rtl8192c/r92c.h>
#include <dev/rtwn/rtl8192c/r92c_reg.h>

#include <dev/rtwn/rtl8188e/r88e.h>

#include <dev/rtwn/rtl8192e/r92e.h>
#include <dev/rtwn/rtl8192e/r92e_reg.h>
#include <dev/rtwn/rtl8192e/r92e_var.h>

#include <dev/rtwn/rtl8812a/r12a.h>
#include <dev/rtwn/rtl8812a/r12a_tx_desc.h>
#include <dev/rtwn/rtl8812a/usb/r12au.h>

#include <dev/rtwn/rtl8821a/r21a.h>
#include <dev/rtwn/rtl8821a/r21a_reg.h>

#include <dev/rtwn/rtl8723b/r8723b_priv.h>
#include <dev/rtwn/rtl8723b/sdio/r8723bs.h>


static int
r8723bs_attach_private(struct rtwn_softc *sc)
{
	struct r8723b_softc *rs;

	rs = malloc(sizeof(struct r8723b_softc), M_RTWN_PRIV,
	    M_WAITOK | M_ZERO);
	if (rs == NULL)
		return (ENOMEM);
	sc->sc_priv = rs;
	return (0);
}


static void
r8723bs_detach_private(struct rtwn_softc *sc)
{
	free(sc->sc_priv, M_RTWN_PRIV);
}


static void
r8723bs_postattach(struct rtwn_softc *sc)
{
	r8723b_dm_attach(sc);
}


static void
r8723bs_adj_devcaps(struct rtwn_softc *sc)
{
	/* No 5 GHz, no 40 MHz yet. */
}


/* A receive request (at most the receive FIFO) is read in one piece. */
_Static_assert(R8723B_RX_DMA_BUFFER_SIZE <= RTWN_SDIO_RXBUFSZ,
    "the receive buffer must hold the whole receive FIFO");


int
r8723bs_attach(struct rtwn_sdio_softc *uc)
{
	struct rtwn_softc *sc		= &uc->sc_sc;

	/* SDIO part. */
	uc->sc_himr			= RTWN_SDIO_INT_RX_REQUEST;

	/* Common part. */
	sc->sc_flags			= RTWN_FLAG_EXT_HDR;

	sc->sc_set_chan			= r92e_set_chan;
	sc->sc_fill_tx_desc		= r12a_fill_tx_desc;
	sc->sc_fill_tx_desc_raw		= r12a_fill_tx_desc_raw;
	sc->sc_fill_tx_desc_null	= r12a_fill_tx_desc_null;
	sc->sc_dump_tx_desc		= r12au_dump_tx_desc;
	sc->sc_tx_radiotap_flags	= r12a_tx_radiotap_flags;
	sc->sc_rx_radiotap_flags	= r12a_rx_radiotap_flags;
	sc->sc_get_rx_stats		= r12a_get_rx_stats;
	sc->sc_get_rssi_cck		= r92e_get_rssi_cck;
	sc->sc_get_rssi_ofdm		= r88e_get_rssi_ofdm;
	sc->sc_classify_intr		= r12au_classify_intr;
	sc->sc_handle_tx_report		= r12a_ratectl_tx_complete;
	sc->sc_handle_tx_report2	= rtwn_nop_softc_uint8_int;
	sc->sc_handle_c2h_report	= r92e_handle_c2h_report;
	sc->sc_check_frame		= rtwn_nop_int_softc_mbuf;
	sc->sc_rf_read			= r92e_rf_read;
	sc->sc_rf_write			= r8723b_rf_write;
	sc->sc_check_condition		= r92c_check_condition;
	sc->sc_efuse_postread		= rtwn_nop_softc;
	sc->sc_parse_rom		= r8723b_parse_rom_sdio;
	sc->sc_set_led			= r92e_set_led;
	sc->sc_power_on			= r8723bs_power_on;
	sc->sc_power_off		= r8723bs_power_off;
#ifndef RTWN_WITHOUT_UCODE
	sc->sc_fw_reset			= r92e_fw_reset;
	sc->sc_fw_download_enable	= r8723b_fw_download_enable;
#endif
	sc->sc_llt_init			= r92e_llt_init;
	sc->sc_set_page_size		= r8723b_set_page_size;
	sc->sc_lc_calib			= r8723b_lc_calib;
	sc->sc_iq_calib			= r8723b_iq_calib;
	sc->sc_read_chipid_vendor	= rtwn_nop_softc_uint32;
	sc->sc_adj_devcaps		= r8723bs_adj_devcaps;
	sc->sc_vap_preattach		= rtwn_nop_softc_vap;
	sc->sc_postattach		= r8723bs_postattach;
	sc->sc_detach_private		= r8723bs_detach_private;
#ifndef RTWN_WITHOUT_UCODE
	sc->sc_set_media_status		= r92e_set_media_status;
	sc->sc_set_rsvd_page		= r88e_set_rsvd_page;
	sc->sc_set_pwrmode		= r92e_set_pwrmode;
	sc->sc_set_rssi			= rtwn_nop_softc;	/* XXX TODO? */
#else
	sc->sc_set_media_status		= rtwn_nop_softc_int;
#endif
	sc->sc_beacon_init		= r12a_beacon_init;
	sc->sc_beacon_enable		= r92c_beacon_enable;
	sc->sc_sta_beacon_enable	= r92c_sta_beacon_enable;
	sc->sc_beacon_set_rate		= rtwn_nop_void_int;
	sc->sc_beacon_select		= r21a_beacon_select;
	sc->sc_temp_measure		= r88e_temp_measure;
	sc->sc_dm_watchdog		= r8723b_dm_watchdog;
	sc->sc_temp_read		= r88e_temp_read;
	sc->sc_init_tx_agg		= rtwn_nop_softc;
	sc->sc_init_rx_agg		= r8723b_init_rx_agg;
	sc->sc_init_ampdu		= rtwn_nop_softc;
	sc->sc_init_intr		= rtwn_nop_softc;
	sc->sc_init_edca		= r92c_init_edca;
	sc->sc_init_bb			= r8723b_init_bb;
	sc->sc_init_rf			= r8723b_init_rf;
	sc->sc_init_antsel		= rtwn_nop_softc;
	sc->sc_post_init		= r8723bs_post_init;
	sc->sc_init_bcnq1_boundary	= rtwn_nop_int_softc;
	sc->sc_set_tx_power		= r92e_set_tx_power;

	sc->mac_prog			= &rtl8723b_mac[0];
	sc->mac_size			= nitems(rtl8723b_mac);
	sc->bb_prog			= &rtl8723b_bb[0];
	sc->bb_size			= nitems(rtl8723b_bb);
	sc->agc_prog			= &rtl8723b_agc[0];
	sc->agc_size			= nitems(rtl8723b_agc);
	sc->rf_prog			= &rtl8723b_rf[0];

	sc->name			= "RTL8723BS";
	sc->fwname			= "rtwn-rtl8723bsfw";
	sc->fwsig			= R8723B_FW_SIGNATURE;

	sc->page_count			= R8723B_TX_PAGE_COUNT;
	sc->pktbuf_count		= 0;			/* Unused */
	sc->ackto			= 0x40;
	sc->npubqpages			= R8723B_PUBQ_NPAGES;
	sc->nhqpages			= R8723B_HPQ_NPAGES;
	sc->nnqpages			= R8723B_NPQ_NPAGES;
	sc->nlqpages			= R8723B_LPQ_NPAGES;
	sc->page_size			= R8723B_TX_PAGE_SIZE;
	sc->txdesc_len			= sizeof(struct r12a_tx_desc);
	sc->efuse_maxlen		= R8723B_EFUSE_MAX_LEN;
	sc->efuse_maplen		= R8723B_EFUSE_MAP_LEN;
	sc->rx_dma_size			= R8723B_RX_DMA_BUFFER_SIZE;

	sc->macid_limit			= R12A_MACID_MAX + 1;
	sc->cam_entry_limit		= R12A_CAM_ENTRY_COUNT;
	sc->fwsize_limit		= R8723B_MAX_FW_SIZE;
	sc->temp_delta			= R88E_CALIB_THRESHOLD;

	sc->bcn_status_reg[0]		= R92C_TDECTRL;
	sc->bcn_status_reg[1]		= R21A_DWBCN1_CTRL;

	sc->ntxchains			= 1;
	sc->nrxchains			= 1;

	/*
	 * After an error on the SDIO bus the chip can stop answering function 1
	 * (every CMD53 then ends with ILLEGAL_COMMAND), under load and also while
	 * idle; taking the interface down and up brings it back. A beacon comes
	 * every 100 ms, so 4 s without a frame on a link that is up means that.
	 */
	sc->rx_silence_limit		= 4;

	return (r8723bs_attach_private(sc));
}
