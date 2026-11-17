/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */
#ifndef RTL8723B_H
#define RTL8723B_H

#include <dev/rtwn/rtl8192e/r92e_var.h>

/*
 * Global definitions. The page numbers are those of the Realtek driver for
 * the RTL8723B (and rtl8xxxu): 247 transmit pages of 128 bytes, 12 for the
 * high queue, 2 each for the normal and low queues, the rest public.
 */
#define R8723B_TX_PAGE_COUNT	247
#define R8723B_TX_PAGE_SIZE	128
#define R8723B_HPQ_NPAGES	12
#define R8723B_NPQ_NPAGES	2
#define R8723B_LPQ_NPAGES	2
#define R8723B_PUBQ_NPAGES	(R8723B_TX_PAGE_COUNT - R8723B_HPQ_NPAGES - \
				 R8723B_NPQ_NPAGES - R8723B_LPQ_NPAGES)
#define R8723B_RX_DMA_BUFFER_SIZE 0x3f80

#define R8723B_MAX_FW_SIZE	0x8000
#define R8723B_FW_SIGNATURE	0x530

#define R8723B_EFUSE_MAX_LEN	512
#define R8723B_EFUSE_MAP_LEN	512

/*
 * Private data: the RTL8192E data that its methods use, first, so that they
 * can take sc_priv as their own.
 */
struct r8723b_softc {
	struct r92e_softc	rs;
	uint8_t			bt_setting;	/* efuse 0xc3 */
#define R8723B_BT_SETTING_AUX_ANT	0x40	/* WLAN on S0 (aux) */

	/* Dynamic mechanisms (r8723b_dm.c). */
	uint8_t			dm_igi_history[4];
	uint16_t		dm_fa_history[4];
	uint8_t			dm_igi_bitmap;
	uint8_t			dm_min_rssi;
	uint8_t			dm_pre_min_rssi;
	uint8_t			dm_linked;
	uint8_t			dm_damping;
	uint8_t			dm_damping_cnt;
	uint8_t			dm_damping_rssi;
	uint8_t			dm_cck_pd_lv;
	uint32_t		dm_cck_fa;
	uint32_t		dm_cck_fa_avg;
	uint32_t		dm_total_fa;
	void			(*dm_scan_start)(struct ieee80211com *);
	void			(*dm_scan_end)(struct ieee80211com *);
};

/*
 * Function declarations.
 */
/* r8723b_init.c */
void	r8723b_init_bb(struct rtwn_softc *);
void	r8723b_init_rf(struct rtwn_softc *);
void	r8723b_rf_write(struct rtwn_softc *, int, uint8_t, uint32_t);
void	r8723b_lc_calib(struct rtwn_softc *);
void	r8723b_iq_calib(struct rtwn_softc *);
int	r8723b_set_page_size(struct rtwn_softc *);
void	r8723b_init_rx_agg(struct rtwn_softc *);
void	r8723b_post_init(struct rtwn_softc *);
#ifndef RTWN_WITHOUT_UCODE
void	r8723b_fw_download_enable(struct rtwn_softc *, int);
#endif

/* r8723b_dm.c */
void	r8723b_dm_attach(struct rtwn_softc *);
void	r8723b_dm_init(struct rtwn_softc *);
void	r8723b_dm_watchdog(struct rtwn_softc *);

/* r8723b_rom.c */
void	r8723b_parse_rom_sdio(struct rtwn_softc *, uint8_t *);

#endif	/* RTL8723B_H */
