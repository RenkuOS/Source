/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	Where the two halves of this driver meet.

	Under brcms/ is the chip, which knows nothing of any network stack and
	is built against a small set of Linux compatibility headers. Above it is
	if_brcms.c, which is a FreeBSD driver because that is what Haiku's
	wireless stack expects, and is built against FreeBSD's.

	The two sets of headers do not agree -- both have a struct mutex, for
	one -- so neither side includes the other's. This is what passes between
	them, and it names nothing that either side would have to define: a
	frame crosses as bytes, because a socket buffer is one side's idea of
	one and an mbuf is the other's.
*/
#ifndef BRCMS_GLUE_NET_H
#define BRCMS_GLUE_NET_H


#include <SupportDefs.h>


/*!	What a received frame is handed to.

	The signal is a height above this driver's idea of a quiet room, which
	is what the stack above expects and what it ranks networks by.
*/
typedef void (*brcms_frame_handler)(void* cookie, const void* frame,
	size_t length, int rssi, uint32 channel, uint16 cryptoStatus);


/*!	What a finished transmission is reported to.

	The chip says whether anything answered for the frame and how many
	times it had to try, which is what the stack above works out how fast
	to send by. A software token identifies the frame even when retries complete
	out of submission order.
*/
typedef void (*brcms_sent_handler)(void* cookie, uint32 token, bool acknowledged,
	uint retries);
typedef bool (*brcms_retry_handler)(void* cookie, uint32 token, void* packet);


/*!	The chip's configuration space, which the FreeBSD side owns.

	The window into the chip's registers is moved by writing to it, so this
	is not only for setting up: it is on the path of every register access.
*/
uint32 brcms_glue_pci_read(uint16 offset, uint8 size);
void brcms_glue_pci_write(uint16 offset, uint32 value, uint8 size);
void brcms_glue_pci_set_master(bool enable);


/*!	Brings the chip up, as far as a tuned radio with its receiver shut. */
status_t brcms_glue_attach(addr_t registers);
void brcms_glue_detach(void);

/*!	The address the card was built with. */
void brcms_glue_address(uint8* address);

/*!	Where received frames are to be handed. */
void brcms_glue_set_frame_handler(brcms_frame_handler handler, void* cookie);
void brcms_glue_set_sent_handler(brcms_sent_handler handler,
	brcms_retry_handler retry, void* cookie);
status_t brcms_glue_retry_frame(void* packet, uint32 negotiatedMcs);

/*!	Opens the receiver, and shuts it again. */
status_t brcms_glue_listen(void);
/*!	What this driver has been told to try.

	Read once, from /boot/home/config/settings/kernel/drivers/broadcom43xxn.
	Each is a line of "name true" or "name false"; a name that is not
	there keeps the value below.

	This exists because every piece of this driver that was missing was
	found by its symptom, one machine lockup at a time: a card that cannot
	carry a network is a card that cannot be reached to be fixed. A switch
	that can be turned off in a text file costs one reboot instead of a
	rescue.

	The defaults are the state this card is known to work in. Anything not
	yet shown to work on the hardware starts off.
*/
struct brcms_glue_options {
	bool	rate_table;		/* write the ucode ACK/CTS rate table */
	bool	mcs;			/* choose a modulation scheme to send at */
	bool	phy_watchdog;	/* let the phy keep itself in tune */
	bool	full_chanspec;	/* set the channel the way brcmsmac does */
	bool	stf_chain;		/* tell the phy which aerials to use */
	bool	ampdu;			/* send several frames as one */
	bool	one_stream;		/* claim one spatial stream, not two */
	bool	basic_rates;	/* work out what to answer a frame at */
	bool	ba_template;	/* put this card's address in the BA template */
	bool	listen_in;		/* keep hearing what is said to others */
	bool	ht40;			/* use the wide channel when offered */
	bool	band5;			/* bring up the five gigahertz band too */
	bool	band2;			/* offer the crowded band at all */
	bool	edcf;			/* tell the microcode it is on an 802.11e medium */
	uint	tx_inflight;	/* hardware frames waiting while ACKs are prepared, 1..8 */
};

const struct brcms_glue_options* brcms_glue_settings(void);
void brcms_glue_read_settings(void);


/*!	One frame of an aggregate, as the stack above handed it over. */
struct brcms_glue_frame {
    uint8 keySlot;
	const void*	bytes;
	size_t		length;
	uint32		token;
};

#define BRCMS_TX_RATE_SGI 0x80

status_t brcms_glue_send_aggregate(const struct brcms_glue_frame* frames,
	uint count, uint8 rate, bool wide);

void brcms_glue_quiet(void);
void brcms_glue_scan_hold(bool holding);
void brcms_glue_phy_watchdog(void);

/*!	Takes whatever has arrived off the ring, handing each frame up. */
void brcms_glue_poll_frames(void);
void brcms_glue_poll_frames_bounded(void);
bool brcms_glue_rx_pending(void);
void brcms_glue_rx_poll_report(void);

/*!	The card's own interrupt.

	Claiming runs in the interrupt itself: it says whether the line is this
	card's and quiets it if so. Handling runs after, with the interrupt no
	longer held. Enabling lets the card ask again.
*/
bool brcms_glue_interrupt_claim(void);
void brcms_glue_interrupt_handle(void);
void brcms_glue_interrupt_enable(void);
void brcms_glue_interrupts_on(void);
void brcms_glue_interrupts_off(void);
void brcms_glue_interrupt_report(void);

/*!	What the card has been doing, for when the log is all there is. */
void brcms_glue_rx_report(void);
void brcms_glue_tx_report(void);
bool brcms_glue_probe_trace_active(void);
void brcms_glue_dma_report(void);
void brcms_glue_mac_report(void);
uint32 brcms_glue_interrupt_count(void);

/*!	Whether the network counts the air in nine microseconds or twenty. */
void brcms_glue_set_short_slot(bool shortSlot);

/*!	How one of the four queues is to contend, as the network has asked.

	The window sizes are given as the powers of two the air uses, and the
	transmit opportunity in the units a beacon carries it in.
*/
void brcms_glue_set_queue(uint queue, uint8 aifsn, uint8 logCWmin,
	uint8 logCWmax, uint16 txopLimit);

/*!	Whether to take in everything the card hears, or only its own. */
void brcms_glue_set_filter(bool everything);

/*!	Whether the card may doze between beacons. Off until asked. */
void brcms_glue_set_power_save(bool doze);

/*!	How often the network announces itself, in units of 1024 microseconds. */
void brcms_glue_set_beacon_period(uint16 periodInUnits);

/*!	Says whose network this card is on, or nobody's if given nothing. */
void brcms_glue_set_bssid(const uint8* bssid);

/*!	Empties the transmit status queue and takes the buffers back. */
void brcms_glue_reclaim_sent(void);
bool brcms_glue_tx_status_pending(void);

/*!	Tunes to a channel in the 2.4 GHz band. */
void brcms_glue_set_channel(uint32 channel, int wide);

/*!	Puts one frame on the air, at a rate in half megabit units.

	A rate of zero means the slowest there is, which is what a frame nobody
	has chosen a rate for should go at.
*/
status_t brcms_glue_send_frame(const void* frame, size_t length, uint8 rate,
	bool mcs, bool protect, bool wide, uint32 token);


#endif	// BRCMS_GLUE_NET_H

status_t brcms_glue_rx_key(const uint8* key, const uint8* peer);

bool brcms_glue_tx_key_acquire(unsigned slot);
void brcms_glue_tx_key_release(unsigned slot);
bool brcms_glue_tx_key_valid(unsigned slot);
uint32 brcms_glue_key_slots_reused(void);
status_t brcms_glue_group_key(unsigned keyid, const uint8* key);
bool brcms_glue_group_key_active(unsigned keyid);
void brcms_glue_tx_key_sweep(void);
status_t brcms_glue_send_frame_hw(const void*, size_t, uint8, bool, bool, bool, uint32, uint8);

status_t brcms_glue_rx_key_retire(int slot);
