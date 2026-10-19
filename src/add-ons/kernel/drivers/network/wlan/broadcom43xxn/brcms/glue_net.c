/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	The hardware half of what the network stack asks for.

	if_brcms.c answers net80211 and knows nothing of the chip; everything
	else under brcms/ drives the chip and knows nothing of net80211. This is
	where one becomes the other, and it is deliberately the only file that
	does: it is built with the chip's headers, and what it hands outward is
	named in glue_net.h, which mentions nothing either side would have to
	define.
*/


#include "linux/types.h"

#include <KernelExport.h>

#include "net/mac80211.h"
#include "linux/ieee80211.h"
#include "linux/skbuff.h"
#include "brcmu_wifi.h"
#include "d11.h"
#include "debug_haiku.h"
#include "glue.h"
#include "phy/phy_hal.h"
#include "glue_net.h"
#include "main.h"
#include "pub.h"
#include "types.h"


#define TRACE(x...)	dprintf("broadcom43xxn: " x)


// Where received frames go once something is there to take them.
static brcms_frame_handler sHandler;
static void* sHandlerCookie;
static brcms_sent_handler sSentHandler;
static brcms_retry_handler sRetryHandler;
static void* sSentCookie;
static bool sAttached;


status_t
brcms_glue_attach(addr_t registers)
{
	if (sAttached)
		return B_OK;

	TRACE("attach: bringing the card up\n");

	status_t status = brcms_glue_hardware_up(registers);
	if (status != B_OK)
		return status;

	sAttached = true;

	// Nothing is heard until the stack above asks for it.
	brcms_glue_quiet();

	TRACE("attach: done, card quiet\n");

	return B_OK;
}


void
brcms_glue_detach(void)
{
	if (!sAttached)
		return;

	brcms_glue_phy_uninit();
	sAttached = false;
	sHandler = NULL;
	sSentHandler = NULL;
}


void
brcms_glue_address(uint8* address)
{
	memcpy(address, brcms_glue_hardware()->etheraddr, 6);
}


void
brcms_glue_set_frame_handler(brcms_frame_handler handler, void* cookie)
{
	sHandler = handler;
	sHandlerCookie = cookie;
}


void
brcms_glue_set_sent_handler(brcms_sent_handler handler,
	brcms_retry_handler retry, void* cookie)
{
	sSentHandler = handler;
	sRetryHandler = retry;
	sSentCookie = cookie;
}


/*!	Says one frame is done with, if anything is there to be told. */
void
brcms_glue_report_sent(uint32 token, bool acknowledged, uint retries)
{
	if (sSentHandler != NULL)
		sSentHandler(sSentCookie, token, acknowledged, retries);
}


bool
brcms_glue_report_retry(uint32 token, void* packet)
{
	return sRetryHandler != NULL && sRetryHandler(sSentCookie, token, packet);
}


/*!	Hands one frame up, if anything is there to take it. */
void
brcms_glue_deliver(const void* frame, size_t length, int rssi, uint32 channel, uint16 cryptoStatus)
{
	if (sHandler != NULL)
		sHandler(sHandlerCookie, frame, length, rssi, channel, cryptoStatus);
}


/*!	Stops the MAC for a register change, if it is running at all.

	brcms_c_suspend_mac_and_wait() keeps a count rather than looking at
	the chip, and waits for the microcode to answer with MI_MACSSPNDD.
	Asking an already-stopped MAC to stop waits 83 ms for an answer that
	cannot come: the microcode gives that answer when it stops, once. The
	phy reads MCTL_EN_MAC before each of its own suspends for this very
	reason, and so does this.

	The count is set from the chip rather than trusted. A running MAC
	means every suspend so far has been released, whatever the count has
	been left reading by a suspend that timed out or by a phy path whose
	release was not taken.

	Returns whether the MAC was stopped here, which is whether
	brcms_glue_resume() has anything to put back.
*/
static bool
brcms_glue_pause(void)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	if (wlc_hw->d11core == NULL || !wlc_hw->clk)
		return false;

	if ((bcma_read32(wlc_hw->d11core, D11REGOFFS(maccontrol)) & MCTL_EN_MAC)
			== 0) {
		// Stopped already, and whoever stopped it owns putting it
		// back. The count still has to say it is stopped, or the next
		// suspend -- the phy makes several of its own without checking
		// -- waits 83 ms for an answer the microcode already gave. By
		// more than one, because one of those unchecked pairs would
		// otherwise take the count to zero and start a MAC that is
		// meant to be quiet. Reaching here with zero is a bug in this
		// driver: every stop is supposed to be counted.
		if (wlc_hw->mac_suspend_depth == 0)
			wlc_hw->mac_suspend_depth = 2;
		return false;
	}

	wlc_hw->mac_suspend_depth = 0;
	brcms_c_suspend_mac_and_wait(brcms_glue_common());
	return true;
}


/*!	Lets the MAC run again, if brcms_glue_pause() stopped it. */
static void
brcms_glue_resume(bool paused)
{
	if (paused)
		brcms_c_enable_mac(brcms_glue_common());
}


/*!	Shuts the receiver without taking the radio down.

	The MAC stops, which stops frames arriving, and everything the phy
	worked out stays where it is.

	This runs while the card is being attached, so it stops the MAC with
	the one register write and does not ask the microcode to confirm it.
	The confirmation handshake belongs on paths that have a working card
	to talk to, not on the one that is still deciding whether it has one.

	The count is told what was done, though, and by more than one: a
	later suspend must not wait 83 ms for a handshake that this stop has
	already used up, and one of the phy's unchecked suspend/enable pairs
	must not take the count to zero and start a MAC that is meant to be
	quiet.
*/
void
brcms_glue_quiet(void)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	if (wlc_hw->d11core == NULL || !wlc_hw->clk)
		return;

	brcms_b_mctrl(wlc_hw, MCTL_EN_MAC, 0);

	if (wlc_hw->mac_suspend_depth < 2)
		wlc_hw->mac_suspend_depth = 2;
}


static int32 sRxPolling;
static int32 sRxPending;
static int32 sRxBudgetStops;

bool
brcms_glue_rx_pending(void)
{
	return atomic_get(&sRxPending) != 0;
}


void
brcms_glue_rx_poll_report(void)
{
	TRACE("RX IRQ continuation: %ld budget stops, pending %u\n",
		(long)atomic_get(&sRxBudgetStops), brcms_glue_rx_pending());
}


static void
brcms_poll_frames(uint maxRounds, bigtime_t maxTime)
{
	if (atomic_test_and_set(&sRxPolling, 1, 0) != 0) {
		atomic_set(&sRxPending, 1);
		return;
	}
	atomic_set(&sRxPending, 0);
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	brcms_glue_rx_polled();
	bigtime_t start = system_time();
	bool more = false;
	for (uint rounds = 0; rounds < maxRounds; rounds++) {
		more = brcms_b_recv(wlc_hw, RX_FIFO, true);
		brcms_glue_reclaim_sent();
		if (!more || (maxTime > 0 && system_time() - start >= maxTime))
			break;
	}
	if (more) {
		atomic_set(&sRxPending, 1);
		atomic_add(&sRxBudgetStops, 1);
	}
	atomic_set(&sRxPolling, 0);
}


void
brcms_glue_poll_frames(void)
{
	brcms_poll_frames(64, 0);
}


void
brcms_glue_poll_frames_bounded(void)
{
	brcms_poll_frames(8, 2000);
}


/*!	Tunes the radio, with the MAC held still while it happens.

	brcmsmac stops the MAC around a channel change and lets it go after,
	and a scan is a channel change several times a second. Retuning
	underneath a running MAC is not something anything has tested.
*/
/*!	How long a slot the network is using.

	A network where every station can manage it agrees to count the air in
	nine microseconds rather than twenty, and says so in every beacon. A
	station using the wrong one waits either longer than it needs to or
	not long enough, and the second of those is a station that talks over
	everyone else.

	The MAC is stopped while this is written, as brcmsmac stops it.
*/
void
brcms_glue_set_short_slot(bool shortSlot)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	if (wlc_hw->d11core == NULL || !wlc_hw->clk)
		return;

	if (wlc_hw->shortslot == shortSlot)
		return;

	wlc_hw->shortslot = shortSlot;

	bool paused = brcms_glue_pause();
	brcms_b_update_slot_timing(wlc_hw, shortSlot);
	brcms_glue_resume(paused);
}


/*!	How the four queues are to contend, as the network has asked.

	The defaults this driver starts with are what a station uses when
	nothing has said otherwise. An access point that speaks of quality of
	service says otherwise in every beacon, and what it says is what its
	other stations are using: a station contending on its own terms rather
	than the network's is one that either never gets a turn or takes more
	than its share.
*/
void
brcms_glue_set_queue(uint queue, uint8 aifsn, uint8 logCWmin,
	uint8 logCWmax, uint16 txopLimit)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	if (wlc_hw->d11core == NULL || !wlc_hw->clk || queue >= 4)
		return;

	struct ieee80211_tx_queue_params parameters;
	parameters.aifs = aifsn;
	parameters.cw_min = (1 << logCWmin) - 1;
	parameters.cw_max = (1 << logCWmax) - 1;
	parameters.txop = txopLimit;

	bool paused = brcms_glue_pause();
	// net80211 uses the on-air BE, BK, VI, VO order, whereas the
	// imported queue API takes mac80211's VO, VI, BE, BK indices.
	static const uint8 queueMap[] = {
		IEEE80211_AC_BE, IEEE80211_AC_BK, IEEE80211_AC_VI, IEEE80211_AC_VO};
	brcms_c_wme_setparams(
		brcms_glue_common(), queueMap[queue], &parameters, false);
	TRACE("queue: AC %u aifs %u cw %u/%u txop %u (32us)\n", queue,
		parameters.aifs, parameters.cw_min, parameters.cw_max, txopLimit);
	brcms_glue_resume(paused);
}


/*!	How much of the air to take in.

	Scanning means listening to everyone: a network announces itself to
	nobody in particular and the card has not chosen one yet. Once it has,
	that is no longer wanted. The chip can tell its own network's frames
	from the rest, and letting it is not only tidier -- every frame taken
	in costs a buffer out of a pool of sixty four and a walk through the
	stack above, and a busy room has thousands a second in it.
*/
void
brcms_glue_set_filter(bool everything)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	if (wlc_hw->d11core == NULL || !wlc_hw->clk)
		return;

	uint flags = FIF_BCN_PRBRESP_PROMISC;
	if (everything)
		flags |= FIF_OTHER_BSS | FIF_PROBE_REQ;

	brcms_c_mac_promisc(brcms_glue_common(), flags);
}


/*!	Whether the card may doze between the network's beacons.

	A station that dozes tells the access point so, and the access point
	then holds its frames until the station next wakes and asks for them.
	It saves power on a machine running from its battery and it costs
	latency, and getting the handshake wrong costs frames that look like
	they were simply lost.

	Off unless something asks: a connection that works is worth more than
	a connection that works and saves power, and this has not been tried.
*/
void
brcms_glue_set_power_save(bool doze)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	if (wlc_hw->d11core == NULL || !wlc_hw->clk)
		return;

	uint32 before = bcma_read32(wlc_hw->d11core, D11REGOFFS(maccontrol));

	brcms_b_mctrl(wlc_hw, MCTL_WAKE | MCTL_HPS,
		MCTL_WAKE | (doze ? MCTL_HPS : 0));

	// If it was dozing, it has to be seen to be awake before anything
	// else is asked of it.
	bool awakeBefore = (before & MCTL_WAKE) != 0
		|| (before & MCTL_HPS) == 0;
	if (!awakeBefore)
		brcms_b_wait_for_wake(wlc_hw);
}


/*!	How often the network this card has joined announces itself.

	The chip counts the air against the access point's beacons, and until
	it is told how far apart they are it counts against nothing. The
	interval arrives in the units a beacon carries it in -- 1024
	microseconds apiece -- and the register wants microseconds.
*/
void
brcms_glue_set_beacon_period(uint16 periodInUnits)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	if (wlc_hw->d11core == NULL || !wlc_hw->clk || periodInUnits == 0)
		return;

	bcma_write32(wlc_hw->d11core, D11REGOFFS(tsf_cfprep),
		((uint32)periodInUnits << 10) << CFPREP_CBI_SHIFT);
}


/*!	Says whose network this card is on, or that it is on none.

	The receive engine matches against this: once it is set, frames from
	that network reach the ring without the promiscuous bits having to let
	everything through.
*/
void
brcms_glue_set_bssid(const uint8* bssid)
{
	static const uint8 kNobody[6] = { 0, 0, 0, 0, 0, 0 };

	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	if (wlc_hw->d11core == NULL || !wlc_hw->clk)
		return;

	brcms_b_set_addrmatch(wlc_hw, RCM_BSSID_OFFSET,
		bssid != NULL ? bssid : kNobody);
}


/*!	The phy's own second.

	It measures the noise, keeps the receive gain where it should be and
	runs the calibrations that are due. brcmsmac calls this once a second
	from its watchdog and nothing here ever called it, so the receiver has
	been running on whatever the initial calibration left behind.

	A receiver that has not been kept in tune still hears the slow and
	sturdy modulations -- which is why everything this card was given
	arrived as CCK while the access point kept trying something quicker
	and giving up.
*/
void
brcms_glue_phy_watchdog(void)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	if (wlc_hw->band == NULL || wlc_hw->band->pi == NULL || !wlc_hw->clk)
		return;

	// This driver masks MI_BG_NOISE in its data interrupt handler. Consume
	// the completed sample here before the watchdog requests another one.
	// The caller holds sc_txLock, also used by channel changes.
	uint32 status = bcma_read32(wlc_hw->d11core, D11REGOFFS(macintstatus));
	if (status != 0xffffffff && (status & MI_BG_NOISE) != 0) {
		bcma_write32(wlc_hw->d11core, D11REGOFFS(macintstatus), MI_BG_NOISE);
		wlc_phy_noise_sample_intr(wlc_hw->band->pi);
		static uint32 completed;
		if (++completed <= 3)
			TRACE("phy: noise result collected %u\n", completed);
	}

	wlc_phy_watchdog(wlc_hw->band->pi);
}


/*!	Whether the phy is to leave its calibration alone for now.

	The phy calibrates itself from time to time and suspends the MAC to do
	it, which is not something to be happening while the channel is being
	swept. brcmsmac holds it off for the length of a scan and lets it run
	again afterwards, which is both halves of what this does.

	Only the length of a scan. Held off for good -- which is what this
	driver did, from the one place it had to stop a calibration landing in
	the middle of a ten second listen -- the phy never tunes itself at all,
	and a receiver that has not been tuned hears the slow and sturdy
	modulations and not the fast ones. Every frame this card was given
	arrived as CCK, an access point that tried anything quicker heard
	nothing back and settled for 802.11b, and sixty four megabytes took
	three and a half minutes.
*/
void
brcms_glue_scan_hold(bool holding)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	if (wlc_hw->band == NULL || wlc_hw->band->pi == NULL)
		return;

	wlc_phy_hold_upd(wlc_hw->band->pi, PHY_HOLD_FOR_SCAN, holding);
}


uint32
brcms_glue_channel(void)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	if (wlc_hw->band == NULL || wlc_hw->band->pi == NULL)
		return 0;

	return CHSPEC_CHANNEL(wlc_phy_chanspec_get(wlc_hw->band->pi));
}


static uint32 sChannelsSet;


/*!	Tunes the radio, twenty megahertz wide or forty.

	A forty megahertz channel is named by the channel in the middle of it
	and by which half of it carries everything that is not 802.11n. The
	stack above names it the other way round -- by the control channel,
	with the other half above it or below -- so the two have to be
	translated: the middle is two channels along from the control one, in
	whichever direction the other half lies.

	\a wide is 0 for twenty megahertz, 1 when the other half is above the
	control channel, and -1 when it is below.
*/
void
brcms_glue_set_channel(uint32 channel, int wide)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	if (wlc_hw->band == NULL || wlc_hw->band->pi == NULL)
		return;

	u16 chanspec;

	if (wide != 0 && brcms_glue_settings()->ht40) {
		uint32 middle = wide > 0 ? channel + 2 : channel - 2;

		chanspec = (u16)(middle | WL_CHANSPEC_BW_40
			| (wide > 0 ? WL_CHANSPEC_CTL_SB_LOWER
				: WL_CHANSPEC_CTL_SB_UPPER)
			| (channel > CH_MAX_2G_CHANNEL
				? WL_CHANSPEC_BAND_5G : WL_CHANSPEC_BAND_2G));
	} else
		chanspec = ch20mhz_chspec(channel);

	// Which band the channel is in. The two have a phy each, because what
	// the radio has to be told differs between them, so crossing from one
	// to the other means changing which phy is being spoken to and
	// starting it on this channel.
	uint bandunit = channel > CH_MAX_2G_CHANNEL ? 1 : 0;
	bool crossing = wlc_hw->band->bandunit != bandunit;
	bool refreshPower = crossing
		|| wlc_phy_chanspec_get(wlc_hw->band->pi) != chanspec;

	if (crossing && (!brcms_glue_settings()->band5 || bandunit >= 2
			|| wlc_hw->bandstate[bandunit]->pi == NULL)) {
		TRACE("ch: channel %u is in a band this driver has not brought "
			"up\n", (unsigned)channel);
		return;
	}

	// Retunes are logged only while a traced probe is still outstanding.
	// This ties the bound to the probes, instead of expiring an unrelated
	// global channel log before the interesting transmission completes.
	static uint32 tracedRetunes;
	bool traceRetune = brcms_glue_probe_trace_active() && tracedRetunes < 128;
	uint32 retune = 0;
	if (traceRetune) {
		retune = ++tracedRetunes;
		TRACE("probe retune begin: t %lld event %lu from %#x to %#x "
			"ucode %#x\n", (long long)system_time(), (unsigned long)retune,
			wlc_phy_chanspec_get(wlc_hw->band->pi), chanspec,
			brcms_b_read_shm(wlc_hw, M_CURCHANNEL));
	}
	bool paused = brcms_glue_pause();


	if (crossing) {
		// Everything the hardware and this driver hold for a band is
		// the other band's until it is said again, from the microcode's
		// shared memory down to what rate to answer a frame at.
		brcms_glue_cross_band(bandunit, chanspec);
		brcms_glue_init_rate_lookup();
		brcms_glue_write_rate_tables();

		TRACE("ch: crossed to band %u\n", bandunit);
	}

	wlc_phy_chanspec_set(wlc_hw->band->pi, chanspec);
	wlc_hw->chanspec = chanspec;
	brcms_glue_common()->chanspec = chanspec;

	// A scan can cross power subbands without changing the PHY instance.
	// Refresh after the new channel/width is installed, while MAC remains
	// paused, so targets are calculated for the channel actually in use.
	if (refreshPower)
		brcms_glue_set_power_limit();


	brcms_glue_resume(paused);
	if (traceRetune) {
		TRACE("probe retune end: t %lld event %lu phy %#x ucode %#x\n",
			(long long)system_time(), (unsigned long)retune,
			wlc_phy_chanspec_get(wlc_hw->band->pi),
			brcms_b_read_shm(wlc_hw, M_CURCHANNEL));
	}

	// The first few, so that a scan that hears everything on one channel
	// can be told from one whose retunes are not reaching the radio. Only
	// the first few: a scan asks for this several times a second.
	if (sChannelsSet < 8) {
		TRACE("ch: asked %u, radio now on %u\n", (unsigned)channel,
			(unsigned)brcms_glue_channel());
	}

	sChannelsSet++;
}


// Evaluated only while establishing a key; all online CPUs must support SSSE3.
bool
renku_ccmp_vector_supported(void)
{
#if defined(__x86_64__)
	system_info info;
	if (get_system_info(&info) != B_OK || info.cpu_count == 0)
		return false;
	for (uint32 cpu = 0; cpu < (uint32)info.cpu_count; cpu++) {
		cpuid_info features;
		if (get_cpuid(&features, 1, cpu) != B_OK
			|| (features.eax_1.extended_features & (1U << 9)) == 0)
			return false;
	}
	return true;
#else
	return false;
#endif
}

// Core23 / firmware610.812 RX-only CCMP experiment. TX remains software.
// No keys or plaintext are logged. MIC and replay checks remain in net80211.
static unsigned sNextRxKeySlot = 4;
static unsigned sActiveRxKeySlot;
static bool sRxKeyTableInitialized;
static int32 sTxKeyReferences[54];
static bool sTxKeyInstalled[54];
// When each slot's key bytes were erased; 0 means the slot was never used.
// A slot is reused only after a quarantine far longer than any receive
// path can hold a frame the old key decrypted: the DMA ring drains in
// milliseconds and net80211 ages block-ack reorder slots out in well under
// a second. Old-key plaintext therefore cannot reach the new key's replay
// counter, and no transmit lease can still name the slot.
static bigtime_t sKeyErasedAt[54];
#define BRCMS_KEY_QUARANTINE	10000000LL
static uint32 sKeySlotsReused;

static void
brcms_glue_key_erase(struct brcms_hardware* hw, unsigned slot)
{
    for (unsigned i = 0; i < 16; i += 2)
        brcms_b_write_shm(hw, 0x73e * 2 + slot * 16 + i, 0);
    sTxKeyInstalled[slot] = false;
    bigtime_t now = system_time();
    sKeyErasedAt[slot] = now > 0 ? now : 1;
}

// Caller holds the crypto mutex. Fresh slots first, then the slot erased
// longest ago, once no lease names it and its quarantine has passed.
static unsigned
brcms_glue_key_slot_choose(void)
{
    if (sNextRxKeySlot < WSEC_MAX_RCMTA_KEYS)
        return sNextRxKeySlot++;
    bigtime_t now = system_time();
    unsigned chosen = 0;
    for (unsigned slot = 4; slot < WSEC_MAX_RCMTA_KEYS; slot++) {
        if (slot == sActiveRxKeySlot || sTxKeyInstalled[slot]
            || atomic_get(&sTxKeyReferences[slot]) != 0
            || sKeyErasedAt[slot] == 0
            || now - sKeyErasedAt[slot] < BRCMS_KEY_QUARANTINE)
            continue;
        if (chosen == 0 || sKeyErasedAt[slot] < sKeyErasedAt[chosen])
            chosen = slot;
    }
    if (chosen != 0)
        sKeySlotsReused++;
    return chosen;
}

uint32
brcms_glue_key_slots_reused(void)
{
    return sKeySlotsReused;
}

bool
brcms_glue_tx_key_acquire(unsigned slot)
{
    // Serialized with installation/retirement by the caller's crypto mutex.
    if (slot < 4 || slot >= 54 || sActiveRxKeySlot != slot
        || !sTxKeyInstalled[slot] || atomic_get(&sTxKeyReferences[slot]) >= 32767)
        return false;
    atomic_add(&sTxKeyReferences[slot], 1);
    return true;
}

void
brcms_glue_tx_key_release(unsigned slot)
{
    if (slot < 4 || slot >= 54) return;
    int32 previous = atomic_add(&sTxKeyReferences[slot], -1);
    if (previous <= 0) panic("broadcom43xxn: TX key reference underflow");
}

bool
brcms_glue_tx_key_valid(unsigned slot)
{
    return slot >= 4 && slot < 54 && sTxKeyInstalled[slot]
        && atomic_get(&sTxKeyReferences[slot]) > 0;
}

void
brcms_glue_tx_key_sweep(void)
{
    // Caller holds the crypto mutex. Inactive lookup + zero owners means
    // firmware cannot start another TX/RX operation with this slot.
    struct brcms_hardware* hw = brcms_glue_hardware();
    if (hw->d11core == NULL || !hw->clk) return;
    for (unsigned slot = 4; slot < sNextRxKeySlot; slot++) {
        if (slot == sActiveRxKeySlot || !sTxKeyInstalled[slot]
            || atomic_get(&sTxKeyReferences[slot]) != 0) continue;
        brcms_glue_key_erase(hw, slot);
    }
}


status_t
brcms_glue_rx_key(const uint8* key, const uint8* peer)
{
	struct brcms_hardware* hw = brcms_glue_hardware();
	if ((key == NULL) != (peer == NULL))
		return B_BAD_VALUE;
	if (hw->d11core == NULL || !hw->clk || hw->corerev != 23)
		return B_NOT_SUPPORTED;
	if (brcms_b_read_shm(hw, M_BOM_REV_MAJOR) != 610
		|| brcms_b_read_shm(hw, M_BOM_REV_MINOR) != 812
		|| brcms_b_read_shm(hw, M_SECRXKEYS_PTR) != 0x73e)
		return B_NOT_SUPPORTED;
	bool paused = brcms_glue_pause();
	if ((bcma_read32(hw->d11core, D11REGOFFS(maccontrol)) & MCTL_EN_MAC) != 0) {
		brcms_glue_resume(paused);
		return B_ERROR;
	}
	if (!sRxKeyTableInitialized) {
		unsigned long held = brcms_glue_window_hold();
		for (unsigned index = 0; index < RCMTA_SIZE; index++) {
			bcma_write32(hw->d11core, D11REGOFFS(objaddr), OBJADDR_RCMTA_SEL | (index * 2));
			(void)bcma_read32(hw->d11core, D11REGOFFS(objaddr));
			bcma_write32(hw->d11core, D11REGOFFS(objdata), 0);
			bcma_write32(hw->d11core, D11REGOFFS(objaddr), OBJADDR_RCMTA_SEL | (index * 2 + 1));
			(void)bcma_read32(hw->d11core, D11REGOFFS(objaddr));
			bcma_write16(hw->d11core, D11REGOFFS(objdata), 0);
		}
		brcms_glue_window_release(held);
		sRxKeyTableInitialized = true;
	}
	// Slots are reused only after brcms_glue_key_slot_choose()'s quarantine.
	// Delayed RX and BA reorder packets retain their old slot, which the new
	// key rejects until that slot has been idle long past any such packet.
	unsigned slot = sActiveRxKeySlot;
	// Invalidate old lookup and erase its bytes before choosing a new slot.
	if (slot != 0)
		brcms_b_write_shm(hw, M_SECKINDXALGO_BLK + 2 * slot, 0);
	unsigned long held = brcms_glue_window_hold();
	bcma_write32(hw->d11core, D11REGOFFS(objaddr), OBJADDR_RCMTA_SEL | ((slot >= 4 ? slot - 4 : 0) * 2));
	(void)bcma_read32(hw->d11core, D11REGOFFS(objaddr));
	bcma_write32(hw->d11core, D11REGOFFS(objdata), 0);
	bcma_write32(hw->d11core, D11REGOFFS(objaddr), OBJADDR_RCMTA_SEL | ((slot >= 4 ? slot - 4 : 0) * 2 + 1));
	(void)bcma_read32(hw->d11core, D11REGOFFS(objaddr));
	bcma_write16(hw->d11core, D11REGOFFS(objdata), 0);
	brcms_glue_window_release(held);
	if (slot != 0 && atomic_get(&sTxKeyReferences[slot]) == 0)
		brcms_glue_key_erase(hw, slot);
	sActiveRxKeySlot = 0;
	if (key != NULL) {
		// Leases released since the last sweep make more slots reusable.
		brcms_glue_tx_key_sweep();
		slot = brcms_glue_key_slot_choose();
		if (slot == 0) {
			brcms_glue_resume(paused);
			TRACE("RX CCMP slots all in quarantine; software fallback\n");
			return B_NOT_SUPPORTED;
		}
        sTxKeyInstalled[slot] = true;
		for (uint i = 0; i < 16; i += 2)
			brcms_b_write_shm(hw, 0x73e * 2 + slot * 16 + i,
				key[i] | ((uint16)key[i + 1] << 8));
	}
	if (key != NULL && peer != NULL) {
		held = brcms_glue_window_hold();
		bcma_write16(hw->d11core, D11REGOFFS(rcmta_size), RCMTA_SIZE);
		bcma_write32(hw->d11core, D11REGOFFS(objaddr), OBJADDR_RCMTA_SEL | ((slot - 4) * 2));
		(void)bcma_read32(hw->d11core, D11REGOFFS(objaddr));
		bcma_write32(hw->d11core, D11REGOFFS(objdata), peer[0]
			| ((uint32)peer[1] << 8) | ((uint32)peer[2] << 16) | ((uint32)peer[3] << 24));
		bcma_write32(hw->d11core, D11REGOFFS(objaddr), OBJADDR_RCMTA_SEL | ((slot - 4) * 2 + 1));
		(void)bcma_read32(hw->d11core, D11REGOFFS(objaddr));
		bcma_write16(hw->d11core, D11REGOFFS(objdata), peer[4] | ((uint16)peer[5] << 8));
		brcms_glue_window_release(held);
		brcms_b_write_shm(hw, M_SECKINDXALGO_BLK + 2 * slot, (slot << 4) | WSEC_ALGO_AES);
	}
	sActiveRxKeySlot = key != NULL ? slot : 0;
	brcms_glue_resume(paused);
	TRACE("RX CCMP hardware key %s; hardware MIC, software PN retained\n", key != NULL ? "installed" : "cleared");
	return key != NULL ? (status_t)slot : B_OK;
}

// Group keys use default-key slots 1..3, named by the frame's KeyID. The
// firmware's lookup word for a default slot is (slot << 4) | algorithm, the
// same format as pairwise slots. A replacement is installed immediately:
// group frames are never block-ack reordered, and the firmware keeps
// attempting a default slot after it is erased, so leaving a slot empty
// would cost every broadcast until the next key.
static bool sGroupKeyInstalled[4];

static void
brcms_glue_group_key_erase(struct brcms_hardware* hw, unsigned keyid)
{
	brcms_b_write_shm(hw, M_SECKINDXALGO_BLK + 2 * keyid, 0);
	for (unsigned i = 0; i < 16; i += 2)
		brcms_b_write_shm(hw, 0x73e * 2 + keyid * 16 + i, 0);
	sGroupKeyInstalled[keyid] = false;
}

status_t
brcms_glue_group_key(unsigned keyid, const uint8* key)
{
	struct brcms_hardware* hw = brcms_glue_hardware();
	if (keyid < 1 || keyid > 3)
		return B_BAD_VALUE;
	if (hw->d11core == NULL || !hw->clk || hw->corerev != 23)
		return B_NOT_SUPPORTED;
	if (brcms_b_read_shm(hw, M_BOM_REV_MAJOR) != 610
		|| brcms_b_read_shm(hw, M_BOM_REV_MINOR) != 812
		|| brcms_b_read_shm(hw, M_SECRXKEYS_PTR) != 0x73e)
		return B_NOT_SUPPORTED;
	bool paused = brcms_glue_pause();
	if ((bcma_read32(hw->d11core, D11REGOFFS(maccontrol)) & MCTL_EN_MAC) != 0) {
		brcms_glue_resume(paused);
		return B_ERROR;
	}
	if (sGroupKeyInstalled[keyid])
		brcms_glue_group_key_erase(hw, keyid);
	status_t result = B_OK;
	if (key != NULL) {
		for (unsigned i = 0; i < 16; i += 2)
			brcms_b_write_shm(hw, 0x73e * 2 + keyid * 16 + i,
				key[i] | ((uint16)key[i + 1] << 8));
		brcms_b_write_shm(hw, M_SECKINDXALGO_BLK + 2 * keyid,
			(keyid << 4) | WSEC_ALGO_AES);
		sGroupKeyInstalled[keyid] = true;
		result = (status_t)keyid;
	}
	brcms_glue_resume(paused);
	TRACE("RX CCMP hardware group key %u %s\n", keyid,
		key != NULL ? "installed" : "cleared");
	return result;
}

bool
brcms_glue_group_key_active(unsigned keyid)
{
	return keyid >= 1 && keyid <= 3 && sGroupKeyInstalled[keyid];
}

status_t
brcms_glue_rx_key_retire(int slot)
{
    // Key update mutex serializes this check with key publication. A stale
    // node/key destructor must never erase the replacement connection key.
    if (slot < 4 || slot >= 54 || (unsigned)slot != sActiveRxKeySlot)
        return B_OK;
    return brcms_glue_rx_key(NULL, NULL);
}
