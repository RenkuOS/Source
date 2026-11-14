/*
 * The shim includes Linux's 802.11 interface for types the phy layer never
 * reaches. Only the names it mentions are here, shaped so the structures
 * around them parse.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BRCMS_NET_MAC80211_H
#define BRCMS_NET_MAC80211_H


#include "../linux/types.h"


#define IEEE80211_NUM_ACS		4
#define IEEE80211_MAX_SSID_LEN	32

// The four access categories, in the order 802.11e numbers them.
enum ieee80211_ac_numbers {
	IEEE80211_AC_VO		= 0,
	IEEE80211_AC_VI		= 1,
	IEEE80211_AC_BE		= 2,
	IEEE80211_AC_BK		= 3,
};

struct wiphy { int unused; };

// Which frames the stack above wants to be shown. brcmsmac turns these
// into the receive filter the microcode applies.
#define FIF_ALLMULTI				(1 << 1)
#define FIF_FCSFAIL					(1 << 2)
#define FIF_PLCPFAIL				(1 << 3)
#define FIF_BCN_PRBRESP_PROMISC		(1 << 4)
#define FIF_CONTROL					(1 << 5)
#define FIF_OTHER_BSS				(1 << 6)
#define FIF_PSPOLL					(1 << 7)
#define FIF_PROBE_REQ				(1 << 8)

// What mac80211 keeps alongside a frame being sent. Only the one flag is
// read here, by the transmit ring, and nothing sets it yet.
#define IEEE80211_TX_CTL_AMPDU		(1 << 6)

struct ieee80211_tx_info {
	u32		flags;
	u8		band;
	u8		hw_queue;
};

#define IEEE80211_SKB_CB(skb)	((struct ieee80211_tx_info*)((skb)->cb))
struct ieee80211_hw { int unused; };
struct ieee80211_vif { int unused; };
struct ieee80211_sta { int unused; };
struct ieee80211_channel { int unused; };
struct ieee80211_supported_band { int unused; };
/*!	What the stack above would ask of one of the four queues.

	brcmsmac reads its own defaults into one of these and then programs the
	chip from it, so the shape matters and nothing else here does.
*/
struct ieee80211_tx_queue_params {
	u16		txop;
	u16		cw_min;
	u16		cw_max;
	u8		aifs;
};
struct ieee80211_rate { int unused; };


#endif	// BRCMS_NET_MAC80211_H
