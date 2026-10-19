/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BRCMS_GLUE_H
#define BRCMS_GLUE_H


#include "linux/types.h"


struct bcma_bus;
struct bcma_device;
struct brcms_phy_pub;
struct ssb_sprom;


/*!	One core as the enumeration ROM described it. */
struct brcms_glue_core {
	uint16	manufacturer;
	uint16	id;
	uint8	revision;
	uint8	classId;
	uint32	address;
	uint32	wrapper;
};


/*!	Takes the register window for a run of accesses rather than one.

	Interrupts are off between the two, so what goes between them is a
	handful of register accesses and nothing that waits.
*/
u16 brcms_glue_ack_duration(u32 rate, u8 preamble);
u16 brcms_glue_ba_duration(u32 rate);
void brcms_glue_write_ba_template(void);
void brcms_glue_write_ampdu_shm(void);
void brcms_glue_cross_band(uint bandunit, u16 chanspec);
void brcms_glue_band_init(u16 chanspec);
void brcms_glue_set_power_limit(void);
void brcms_glue_init_rate_lookup(void);
void brcms_glue_write_rate_tables(void);
unsigned long brcms_glue_window_hold(void);
void brcms_glue_window_release(unsigned long state);


status_t brcms_glue_bus_init(addr_t registers,
	const struct brcms_glue_core* cores, int coreCount);
struct bcma_device* brcms_glue_d11core(void);
struct bcma_bus* brcms_glue_bus(void);

void brcms_glue_read_sprom(struct bcma_device* chipCommon,
	struct ssb_sprom* out);

/*!	Builds the state the imported phy layer needs and starts it on a
	channel. \a chanspec is a brcmsmac channel specification, so
	2412 MHz is ch_to_chanspec(1).
*/
status_t brcms_glue_hardware_up(addr_t registers);
status_t brcms_glue_phy_init(uint16 chanspec);
void brcms_glue_deliver(const void* frame, size_t length, int rssi,
	uint32 channel, uint16 cryptoStatus);
void brcms_glue_report_sent(uint32 token, bool acknowledged, uint retries);
bool brcms_glue_report_retry(uint32 token, void* packet);
const char* brcms_glue_frame_name(uint16 frameControl);

/*!	Stops everything the phy layer started and frees it.

	This has to run before the driver image is unloaded: the phy arms a
	timer for its multiphase calibration, and a timer that fires after the
	image has gone takes a page fault inside the interrupt handler.
*/
void brcms_glue_phy_uninit(void);

/*!	Locks down the memory the card's own DMA engine reads and writes. */
status_t brcms_glue_dma_init(void);
void brcms_glue_dma_uninit(void);
void brcms_glue_dma_report(void);

void brcms_glue_set_bus_master(bool enable);

/*!	What is done with a frame once the card has handed one back. */
void brcms_glue_rx_report(void);
void brcms_glue_rx_reset(void);

/*!	Opens the receiver and takes what it has heard. */
status_t brcms_glue_listen(void);
bool brcms_glue_poll(void);
void brcms_glue_mac_report(void);
struct brcms_hardware* brcms_glue_hardware(void);
struct brcms_c_info* brcms_glue_common(void);

void brcms_glue_tx_report(void);

// brcmsmac's own, file local in its main.c and stf.c, which the transmit
// header is worked out with.
u16 frametype(u32 ratespec, u8 mimoframe);
uint brcms_c_calc_frame_time(struct brcms_c_info* wlc, u32 ratespec,
	u8 preambleType, uint length);
u16 brcms_c_phytxctl1_calc(struct brcms_c_info* wlc, u32 ratespec);
u16 brcms_c_stf_d11hdrs_phyctl_txant(struct brcms_c_info* wlc, u32 ratespec);
void brcms_c_compute_plcp(struct brcms_c_info* wlc, u32 ratespec,
	uint length, u8* plcp);
int brcms_c_txfifo(struct brcms_c_info* wlc, uint fifo, struct sk_buff* p);
uint32 brcms_glue_mac_stops(void);
uint32 brcms_glue_channel(void);
void brcms_glue_chip_report(void);
void brcms_glue_rx_polled(void);
void brcms_glue_stop_timers(void);
void brcms_glue_quiesce_timers(void);
status_t brcms_glue_start_timer_thread(void);
void brcms_glue_stop_timer_thread(void);
void brcms_glue_quiesce_hardware(struct brcms_hardware* wlc_hw);



// brcmsmac's own hardware entry points, which are file local in its main.c
// and are what this driver starts the card through.
struct brcms_hardware;
void brcms_b_init(struct brcms_hardware* wlc_hw, u16 chanspec);
void brcms_b_info_init(struct brcms_hardware* wlc_hw);
bool brcms_b_validate_chip_access(struct brcms_hardware* wlc_hw);
void brcms_c_setxband(struct brcms_hardware* wlc_hw, uint bandunit);
void brcms_b_hw_up(struct brcms_hardware* wlc_hw);
struct brcms_c_info;
bool brcms_b_attach_dmapio(struct brcms_c_info* wlc, uint j, bool wme);
int brcms_b_up_prep(struct brcms_hardware* wlc_hw);
int brcms_b_up_finish(struct brcms_hardware* wlc_hw);
u16 brcms_b_read_shm(struct brcms_hardware* wlc_hw, uint offset);
void brcms_b_wait_for_wake(struct brcms_hardware* wlc_hw);
void brcms_b_set_addrmatch(struct brcms_hardware* wlc_hw, int matchRegister,
	const u8* address);
bool brcms_b_recv(struct brcms_hardware* wlc_hw, uint fifo, bool bound);
void brcms_c_mac_promisc(struct brcms_c_info* wlc, uint filter_flags);
void brcms_c_edcf_setparams(struct brcms_c_info* wlc, bool suspend);
void brcms_c_wme_setparams(struct brcms_c_info* wlc, u16 aci,
	const struct ieee80211_tx_queue_params* params, bool suspend);
void brcms_b_update_slot_timing(struct brcms_hardware* wlc_hw,
	bool shortslot);
void brcms_b_clkctl_clk(struct brcms_hardware* wlc_hw, enum bcma_clkmode mode);
const u16* brcms_glue_xmtfifo_sz(uint corerev);


#endif	// BRCMS_GLUE_H
