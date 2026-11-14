/* SPDX-License-Identifier: MIT */
#include "amsdu_fields.h"
static unsigned sAmsduFrames, sAmsduSubframes;
/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

/*!	The BCM43224 as a wireless interface.

	Everything under brcms/ is the chip: its own phy layer, imported from
	Linux's brcmsmac, and what this driver put beneath it. That half
	enumerates the chip, loads its microcode, tunes the radio and moves
	frames both ways, and it was brought up and tested on its own before any
	of this existed.

	This is the other half. Haiku's wireless stack is FreeBSD's net80211,
	reached through the compatibility layer under src/libs/compat, so a
	driver that wants to appear as an interface has to look like a FreeBSD
	driver and answer net80211's calls. That is all this file does: it owns
	the ieee80211com, answers what net80211 asks, and turns each question
	into the call underneath that already does it.

	The two halves do not share headers -- both sides have a struct mutex,
	among others -- so a frame crosses between them as bytes. This file is
	the only one here that may say mbuf.
*/


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/socket.h>
#include <sys/taskqueue.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <sys/rman.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>
#include <net/ethernet.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_ratectl.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include "brcms/glue_net.h"
#include "if_brcms_tx_queue.h"
#include "if_brcms_ack_filter.h"
#include "if_brcms_tx_tokens.h"


// The chips this binds to. All are of the same family; the MacBook7,1 has
// the second.
#define VENDOR_BROADCOM		0x14e4

static const struct {
	uint16_t	device;
	const char*	name;
} kSupported[] = {
	{ 0x0576, "Broadcom BCM43224" },
	{ 0x4353, "Broadcom BCM43224" },
	{ 0x4357, "Broadcom BCM43225" },
};

// Only 2.4 GHz: that is the band the radio has been brought up and tested
// on, and saying more than is known is how a scan ends up listening to
// nothing.
// Eleven, not thirteen. The chip refuses to transmit on twelve and
// thirteen -- it answers every frame given to it for them with
// TX_STATUS_SUPR_BADCH, which is not the mismatch its name suggests but
// "not on that channel", the two addresses agreeing perfectly each time:
//
//   tx: the chip would not send it, reason 4; phy on 13, microcode told 0xd
//
// Offering them only spends scan time on channels this card can listen to
// and not answer on.
static const uint8_t kChannels[]
	= { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

// The 5 GHz channels this card may use. The lower band and the upper one,
// leaving out the stretch in the middle that wants radar detection -- which
// is a thing to get wrong quietly and be fined for.
static const uint8_t kChannels5[]
	= { 36, 40, 44, 48, 149, 153, 157, 161, 165 };

// What this driver calls a quiet room. The chip does not measure the
// noise, and a signal has to be reported as a height above something.
#define BRCMS_NOISE_FLOOR	-96


/*!	What a captured frame is wrapped in.

	Monitor mode hands frames to whatever is watching with a header saying
	how each arrived. It is what makes a capture worth taking: a frame on
	its own says what was sent, and this says on which channel, at what
	rate and how loudly.
*/
#define BRCMS_RX_RADIOTAP_PRESENT			\
	((1 << IEEE80211_RADIOTAP_FLAGS)		\
	| (1 << IEEE80211_RADIOTAP_RATE)		\
	| (1 << IEEE80211_RADIOTAP_CHANNEL)		\
	| (1 << IEEE80211_RADIOTAP_DBM_ANTSIGNAL)	\
	| (1 << IEEE80211_RADIOTAP_DBM_ANTNOISE))

struct brcms_rx_radiotap {
	struct ieee80211_radiotap_header	wr_ihdr;
	uint8_t		wr_flags;
	uint8_t		wr_rate;
	uint16_t	wr_chan_freq;
	uint16_t	wr_chan_flags;
	int8_t		wr_antsignal;
	int8_t		wr_antnoise;
} __packed __aligned(8);

#define BRCMS_TX_RADIOTAP_PRESENT			\
	((1 << IEEE80211_RADIOTAP_FLAGS)		\
	| (1 << IEEE80211_RADIOTAP_RATE)		\
	| (1 << IEEE80211_RADIOTAP_CHANNEL))

struct brcms_tx_radiotap {
	struct ieee80211_radiotap_header	wt_ihdr;
	uint8_t		wt_flags;
	uint8_t		wt_rate;
	uint16_t	wt_chan_freq;
	uint16_t	wt_chan_flags;
} __packed __aligned(8);

// How many frames may be with the chip at once before this stops keeping
// track of whose they were. The ring is deeper than this; what is lost is
// the rate control's view of a frame, not the frame.
#define BRCMS_IN_FLIGHT		64


struct brcms_softc {
	device_t				sc_dev;
	struct ieee80211com		sc_ic;
	struct mtx				sc_mtx;
	struct mtx				sc_txLock;
	struct mtx sc_cryptoLock;
	struct task				sc_txTask;
	struct brcms_tx_queue	sc_txPending;
	uint32_t				sc_txQueueFull;
	uint32_t				sc_txControlReserveUsed;
	uint32_t				sc_resetCoalesced;
	uint32_t				sc_ackCoalesced;

	struct resource*		sc_registers;
	int						sc_registersId;

	struct resource*		sc_interrupt;
	int						sc_interruptId;
	void*					sc_interruptCookie;

	// A card that never raises its line would otherwise be a card that
	// hears nothing and says nothing about it.
	struct callout			sc_watchdog;
	struct callout			sc_flush;
	struct callout			sc_statusRetry;
	uint32_t				sc_ticks;

	struct brcms_rx_radiotap	sc_rxTap;
	struct brcms_tx_radiotap	sc_txTap;
	bool					sc_everInterrupted;

	// Node references indexed by a software completion token. Retries
	// may complete after later frames, so submission order is insufficient.
	struct ieee80211_node*	sc_inFlight[BRCMS_IN_FLIGHT];
	struct mbuf*				sc_txCallback[BRCMS_IN_FLIGHT];
	bool					sc_rateFeedback[BRCMS_IN_FLIGHT];
	bool					sc_sentMcs[BRCMS_IN_FLIGHT];
	uint8_t				sc_sentRate[BRCMS_IN_FLIGHT];
	struct brcms_tx_tokens sc_txTokens;
	uint16_t sc_baSequence[BRCMS_IN_FLIGHT];
	uint8_t sc_baTID[BRCMS_IN_FLIGHT];
	struct ieee80211_node* sc_barNode;
	uint16_t sc_barSequence;
	uint8_t sc_barTID;
	uint32_t sc_barsSent;
	uint32_t				sc_queued;
	uint32_t				sc_completed;

	bool					sc_running;
	uint8_t					sc_address[IEEE80211_ADDR_LEN];
};


// The chip half reaches configuration space through this file, so it needs
// one device to work from. There is one card.
static device_t sDevice;


/* --- what the chip half needs of PCI --------------------------------- */

uint32
brcms_glue_pci_read(uint16 offset, uint8 size)
{
	return pci_read_config(sDevice, offset, size);
}


void
brcms_glue_pci_write(uint16 offset, uint32 value, uint8 size)
{
	pci_write_config(sDevice, offset, value, size);
}


void
brcms_glue_pci_set_master(bool enable)
{
	// The compatibility layer has the one and not the other, so the bit is
	// cleared by hand. Without it the chip's DMA engine cannot fetch so
	// much as a descriptor.
	if (enable) {
		pci_enable_busmaster(sDevice);
		return;
	}

	uint16_t command = pci_read_config(sDevice, PCIR_COMMAND, 2);
	if ((command & PCIM_CMD_BUSMASTEREN) != 0) {
		pci_write_config(sDevice, PCIR_COMMAND,
			command & ~PCIM_CMD_BUSMASTEREN, 2);
	}
}


/* --- what net80211 asks of a driver ---------------------------------- */

static int
brcms_probe(device_t dev)
{
	if (pci_get_vendor(dev) != VENDOR_BROADCOM)
		return ENXIO;

	uint16_t device = pci_get_device(dev);
	for (size_t i = 0; i < nitems(kSupported); i++) {
		if (kSupported[i].device == device) {
			device_set_desc(dev, kSupported[i].name);
			return BUS_PROBE_DEFAULT;
		}
	}

	return ENXIO;
}


/*!	Says which channels the radio has.

	net80211 asks rather than being told, so that it knows what to scan and
	what a frame's channel means.
*/
static void
brcms_getradiocaps(struct ieee80211com* ic, int maxChannels, int* _count,
	struct ieee80211_channel channels[])
{
	uint8_t bands[IEEE80211_MODE_BYTES];

	memset(bands, 0, sizeof(bands));
	setbit(bands, IEEE80211_MODE_11B);
	setbit(bands, IEEE80211_MODE_11G);
	setbit(bands, IEEE80211_MODE_11NG);

	// Forty megahertz channels as well as twenty. One of them covers most
	// of the 2.4 GHz band at once, so most access points do not offer
	// them; whether one is used is the network's choice, not this card's.
	// The crowded band can be left out, which is how a network that is on
	// both is joined on the quiet one: the two share a name, and what
	// decides between them is the strength the scan saw, which the
	// crowded one usually wins by being lower in frequency.
	*_count = 0;
	if (brcms_glue_settings()->band2) {
		ieee80211_add_channel_list_2ghz(channels, maxChannels, _count,
			kChannels, nitems(kChannels), bands,
			NET80211_CBW_FLAG_HT40);
	}

	// And the quiet band, where this part also works. Nothing above has
	// to change for it: the chanspec a channel number turns into carries
	// which band it is in.
	if (brcms_glue_settings()->band5) {
		uint8_t bands5[IEEE80211_MODE_BYTES];

		memset(bands5, 0, sizeof(bands5));
		setbit(bands5, IEEE80211_MODE_11A);
		setbit(bands5, IEEE80211_MODE_11NA);

		ieee80211_add_channel_list_5ghz(channels, maxChannels, _count,
			kChannels5, nitems(kChannels5), bands5,
			NET80211_CBW_FLAG_HT40);
	}
}


static void
brcms_set_channel(struct ieee80211com* ic)
{
	struct brcms_softc* sc = ic->ic_softc;

	// Which way the other half lies, if there is one.
	int wide = 0;
	if (IEEE80211_IS_CHAN_HT40U(ic->ic_curchan))
		wide = 1;
	else if (IEEE80211_IS_CHAN_HT40D(ic->ic_curchan))
		wide = -1;

	mtx_lock(&sc->sc_txLock);
	brcms_glue_set_channel(ieee80211_chan2ieee(ic, ic->ic_curchan), wide);
	mtx_unlock(&sc->sc_txLock);
}


/*!	Stays on a 5 GHz channel until the scan's maximum dwell.

	net80211 leaves a channel as soon as any frame arrives after the
	minimum dwell (20 ms). The microcode refuses probe requests on 5 GHz
	channels (TX_STATUS_SUPR_BADCH), so an access point there is found only
	by its beacon, sent every 102.4 ms. Leaving at the first beacon of a
	neighbouring network missed the strongest 5 GHz access point in about
	a third of rejoins, which then fell back to 2.4 GHz. The maximum dwell
	(200 ms) always spans a beacon interval; 2.4 GHz keeps early advance.
*/
static void (*sScanMindwell)(struct ieee80211_scan_state*);

static void
brcms_scan_mindwell(struct ieee80211_scan_state* ss)
{
	struct ieee80211com* ic = ss->ss_ic;
	if (ic->ic_curchan != NULL && IEEE80211_IS_CHAN_5GHZ(ic->ic_curchan))
		return;
	sScanMindwell(ss);
}


static void
brcms_scan_start(struct ieee80211com* ic)
{
	struct brcms_softc* sc = ic->ic_softc;
	mtx_lock(&sc->sc_txLock);

	// The receiver already takes everything it hears and net80211 moves
	// the channel itself, so the only thing to say is that the phy should
	// not pick this moment to calibrate: it stops the MAC to do that, and
	// the channel is being swept.
	brcms_glue_scan_hold(true);
	mtx_unlock(&sc->sc_txLock);
}


static void
brcms_scan_end(struct ieee80211com* ic)
{
	struct brcms_softc* sc = ic->ic_softc;
	mtx_lock(&sc->sc_txLock);

	// And it may tune itself again now.
	brcms_glue_scan_hold(false);
	mtx_unlock(&sc->sc_txLock);
}


/*!	The network has said how long a slot it counts the air in. */
static void
brcms_updateslot(struct ieee80211com* ic)
{
	struct brcms_softc* sc = ic->ic_softc;
	mtx_lock(&sc->sc_txLock);

	brcms_glue_set_short_slot((ic->ic_flags & IEEE80211_F_SHSLOT) != 0);
	mtx_unlock(&sc->sc_txLock);
}


/*!	The network has said how its stations are to contend.

	net80211 has the parameters out of the beacon by the time it asks, one
	set per queue. Returning zero says they were taken.
*/
static int
brcms_wme_update(struct ieee80211vap* vap, const struct wmeParams* params)
{
	struct brcms_softc* sc = vap->iv_ic->ic_softc;
	mtx_lock(&sc->sc_txLock);

	for (uint queue = 0; queue < WME_NUM_AC; queue++) {
		const struct wmeParams* p = &params[queue];

		brcms_glue_set_queue(queue, p->wmep_aifsn, p->wmep_logcwmin,
			p->wmep_logcwmax, p->wmep_txopLimit);
	}

	mtx_unlock(&sc->sc_txLock);
	return 0;
}


/*!	net80211 asking for everything, or for only what is addressed here. */
static void
brcms_update_promisc(struct ieee80211com* ic)
{
	brcms_glue_set_filter(ic->ic_promisc > 0);
}


static void
brcms_update_mcast(struct ieee80211com* ic)
{
	// The chip takes every group address it hears and the stack above
	// sorts them out, so there is nothing to narrow.
}


/*!	Remembers whose a frame is until the chip has finished with it. */
// What this card is choosing to send at. net80211 on Haiku has only the
// "none" rate control, so whatever it leaves in the node is what goes out,
// and a station that only ever speaks slowly is one an access point answers
// slowly.
/*!	Which modulation and coding scheme to send a data frame at.

	net80211 on Haiku has only the "none" rate control, which leaves the
	node at the slowest rate its network offers and never moves. Every
	frame this card sent went out at one megabit, and an access point
	answering a station that crawls will crawl back: nearly everything
	arriving here was 802.11b, and sixty four megabytes took three and a
	half minutes.

	The driver climbs after successful transmissions and falls back on
	failure. Rates are ordered by throughput, with two spatial streams
	only when they were negotiated with the access point.

	Management frames do not come through here. They go out slow and wide
	on purpose: they are what an association is made of, and there is
	nothing to fall back to if they are not heard.
*/
#define BRCMS_MCS_TOP		15
#define BRCMS_MCS_CLIMB		12

/*!	How many frames have to have worked before 802.11n is tried at all.

	Everything that gets this card onto a network -- the association, and
	the four frames that agree the key, which Haiku's supplicant sends as
	ordinary data -- has nothing to fall back to if it is not heard. None
	of it may be an experiment. By the time this many data frames have
	been answered, the card is on the network and carrying traffic, and a
	scheme that turns out not to work costs a few frames rather than the
	connection.

	This driver has taken that connection away four times while this was
	being worked out, and each time the machine had to be rescued by hand.
	That is what this number is for.
*/
#define BRCMS_MCS_SETTLE	200

static uint8 sMcs;
static uint32 sMcsGood;
static uint32 sMcsBad;
static uint32 sSettled;
static bool sMcsUsable = true;

static uint16_t sMcsMask = 0xff;


static uint8
brcms_adjacent_mcs(uint8 current, bool faster)
{
	// Long-GI rate in units of 6.5 Mbps (HT20) or 13.5 Mbps (HT40).
	// MCS 8 is slower than MCS 7: numeric MCS order is not speed order.
	static const uint8 units[] = {1, 2, 3, 4, 6, 8, 9, 10,
		2, 4, 6, 8, 12, 16, 18, 20};
	uint8 best = current;
	for (uint8 candidate = 0; candidate <= BRCMS_MCS_TOP; candidate++) {
		if ((sMcsMask & (1U << candidate)) == 0)
			continue;
		if (faster ? units[candidate] <= units[current]
			: units[candidate] >= units[current])
			continue;
		if (best == current || (faster ? units[candidate] < units[best]
			: units[candidate] > units[best]))
			best = candidate;
	}
	return best;
}


static void
brcms_rate_answered(bool acknowledged, uint attempts, bool mcs, uint8 rate)
{
	// Only successful unicast data establishes that the link is ready.
	// A fallback is temporary: probe HT again after a fresh settling run.
	if (sSettled < BRCMS_MCS_SETTLE) {
		if (!acknowledged)
			sSettled = 0;
		else if (++sSettled == BRCMS_MCS_SETTLE) {
			sMcsUsable = true;
			sMcs = 0;
			sMcsGood = sMcsBad = 0;
		}
		return;
	}

	// Outstanding frames retain the rate selected before this completion.
	// Do not punish the new rate for failures at an older, faster rate.
	if (!mcs || rate != sMcs)
		return;

	if (!acknowledged) {
		sMcsGood = 0;
		if (sMcs > 0) {
			sMcs = brcms_adjacent_mcs(sMcs, false);
			sMcsBad = 0;
		} else if (++sMcsBad >= 3) {
			sMcsUsable = false;
			sSettled = 0;
			sMcsBad = 0;
		}
		return;
	}

	sMcsBad = 0;
	if (attempts >= 2) {
		sMcsGood = 0;
		return;
	}
	if (++sMcsGood >= BRCMS_MCS_CLIMB) {
		sMcs = brcms_adjacent_mcs(sMcs, true);
		sMcsGood = 0;
	}
}


/*!	Frames held back so that several can go out as one.

	A frame of an aggregate is held only as long as it takes the next one
	to arrive, and never longer than the watchdog's quarter second. What
	is being traded is a little latency on the last frame of a burst for
	the fixed cost of a transmission on all the others.
*/

#define BRCMS_CRYPTO_COOKIE 0x524b4354U
#define BRCMS_CRYPTO_TAG 1
extern int renku_ccmp_key_slot(const struct ieee80211_key*);
static uint8_t sTxKeySlot[BRCMS_IN_FLIGHT];
static void
brcms_crypto_tag_free(struct m_tag* tag)
{
    brcms_glue_tx_key_release(*(uint8_t*)(tag + 1));
    m_tag_free_default(tag);
}
static struct m_tag*
brcms_crypto_tag(struct mbuf* m)
{
    return m_tag_locate(m, BRCMS_CRYPTO_COOKIE, BRCMS_CRYPTO_TAG, NULL);
}
static uint8_t
brcms_crypto_slot(struct mbuf* m)
{
    struct m_tag* tag = brcms_crypto_tag(m);
    return tag != NULL ? *(uint8_t*)(tag + 1) : 0;
}
static uint8_t
brcms_crypto_take(struct mbuf* m)
{
    struct m_tag* tag = brcms_crypto_tag(m);
    if (tag == NULL) return 0;
    uint8_t slot = *(uint8_t*)(tag + 1);
    *(uint8_t*)(tag + 1) = 0;
    return slot;
}
int
renku_ccmp_tx_prepare(const struct ieee80211_key* key, struct mbuf* m)
{
    int slot = renku_ccmp_key_slot(key);
    if (slot < 4 || slot >= 54) return 0;
    struct ieee80211_frame* wh = mtod(m, struct ieee80211_frame*);
    if ((wh->i_fc[0] & 15) != 8 || (wh->i_fc[1] & 3) != 1
        || IEEE80211_IS_MULTICAST(wh->i_addr1)) return 0;
    if (brcms_crypto_tag(m) != NULL) return -1;
    struct m_tag* tag = m_tag_alloc(BRCMS_CRYPTO_COOKIE, BRCMS_CRYPTO_TAG, 1, M_NOWAIT);
    if (tag == NULL) return -1;
    if (!brcms_glue_tx_key_acquire(slot)) { m_tag_free(tag); return -1; }
    *(uint8_t*)(tag + 1) = slot;
    tag->m_tag_free = brcms_crypto_tag_free;
    m_tag_prepend(m, tag);
    return 1;
}

#define BRCMS_STAGE_MOST	16
// One A-MSDU of small frames: fits a 2 KiB cluster after CCMP headroom.
#define BRCMS_AMSDU_MOST	24
#define BRCMS_AMSDU_BYTES	2000
#define BRCMS_PIPELINE_CREDIT	32

static struct {
	uint8_t					bytes[2048];
	size_t					length;
} sStage[BRCMS_STAGE_MOST];

static uint8_t sStageKeySlot[BRCMS_STAGE_MOST];
static uint					sStaged;
static uint8_t				sStageRate;
static bool					sStageWide;
static uint8_t sStageTID;
static uint sStageBytes;
static struct ieee80211_node* sStageNode[BRCMS_STAGE_MOST];
static uint32 sTxByRate[64];
static uint32 sTxMcs;
static uint32 sTxLegacy;
static uint32 sTxProtected;
static uint32 sTxRetries;
static uint32 sTxSucceeded;

// Diagnostic only: timestamps are owned by sc_txLock, like TX tokens.
static bigtime_t sPublishedAt[BRCMS_IN_FLIGHT];
static uint32 sCompletionLatency[2][6];
static uint32 sAggregateSizes[4];
static bigtime_t sQueueEntered[BRCMS_TX_PENDING];
static bigtime_t sQueueWaitMax, sFullHeadMax, sStageBorn, sStageWaitMax;
static uint32 sPreparedOverlap, sPipelineBatches, sPipelinePeak;
static uint32 sFullBusy[4], sFullKinds[7], sQueueWait[6];
static uint32 sStageBlocked;
static uint64 sEchoStage[BRCMS_STAGE_MOST], sEchoToken[BRCMS_IN_FLIGHT];
static uint32 sEchoCompleted[2], sEchoFailed[2], sEchoFailPrinted, sEchoSlowPrinted;
static bigtime_t sEchoChipMax[2];
static struct {
    uint64 key;
    uint32 total, failed;
    uint64 lowSequences[8];
} sEchoFlows[8];
static uint sEchoFlowNext;





static void brcms_queued(struct brcms_softc* sc, struct ieee80211_node* ni,
	bool feedback, bool mcs, uint8 rate, uint32_t token);
static void brcms_tx_task(void* argument, int pending);
static void brcms_flush_timer(void* argument);
static int brcms_send_mbuf(struct ieee80211_node* ni, struct mbuf* m,
	uint8_t rate, bool mcs, bool protect, bool wide);



// Read the echo identity before software encryption; never alter the packet.
static uint64
brcms_echo_key(struct brcms_softc* sc, struct mbuf* m)
{
    uint8_t bytes[128];
    int length = m_length(m, NULL);
    if (length < 26) return 0;
    int copied = length < (int)sizeof(bytes) ? length : sizeof(bytes);
    m_copydata(m, 0, copied, (caddr_t)bytes);
    uint header = ieee80211_hdrspace(&sc->sc_ic,
        (const struct ieee80211_frame*)bytes);
    static const uint8_t snap[] = {0xaa, 0xaa, 3, 0, 0, 0, 8, 0};
    if ((bytes[0] & 0x0c) != 8 || header + 8 + 20 > (uint)copied
        || memcmp(bytes + header, snap, sizeof(snap)) != 0) return 0;
    const uint8_t* ip = bytes + header + 8;
    uint ih = (ip[0] & 15) * 4;
    if ((ip[0] >> 4) != 4 || ih < 20 || ip[9] != 1
        || (ip[6] & 0xbf) != 0 || ip[7] != 0
        || header + 8 + ih + 8 > (uint)copied) return 0;
    const uint8_t* icmp = ip + ih;
    if ((icmp[0] != 0 && icmp[0] != 8) || icmp[1] != 0) return 0;
    return ((uint64)1 << 32) | ((uint64)icmp[0] << 33)
        | ((uint32)icmp[4] << 24) | ((uint32)icmp[5] << 16)
        | ((uint32)icmp[6] << 8) | icmp[7];
}

// Replace only the newest queued packet in this flow, and only when
// both are advancing pure ACKs with compatible control state. The
// original packet has not reached encryption or DMA. Reuse its 802.11
// sequence number so the block-ACK space has neither a hole nor reordering.
static struct mbuf*
brcms_coalesce_ack(struct brcms_softc* sc, struct ieee80211_node* ni,
	struct mbuf* m)
{
	if (sc->sc_txPending.count < 32
		|| (m->m_flags & (M_TXCB | M_EAPOL)) != 0)
		return NULL;
	uint8_t bytes[128];
	int length = m_length(m, NULL);
	if (length < 26)
		return NULL;
	int copied = length < (int)sizeof(bytes) ? length : sizeof(bytes);
	m_copydata(m, 0, copied, (caddr_t)bytes);
	uint headerLength = ieee80211_hdrspace(&sc->sc_ic,
		(const struct ieee80211_frame*)bytes);
	struct brcms_ack_key newer;
	if (!brcms_ack_parse(bytes, copied, length, headerLength, &newer)
		|| !newer.eligible)
		return NULL;
	for (uint n = sc->sc_txPending.count; n != 0; n--) {
		uint slot = (sc->sc_txPending.head + n - 1) % BRCMS_TX_PENDING;
		struct brcms_pending_tx* entry = &sc->sc_txPending.entries[slot];
		if (entry->raw)
			return NULL;
		if (entry->node != ni)
			continue;
		struct mbuf* previous = entry->packet;
		length = m_length(previous, NULL);
		copied = length < (int)sizeof(bytes) ? length : sizeof(bytes);
		m_copydata(previous, 0, copied, (caddr_t)bytes);
		struct brcms_ack_key older;
		if (!brcms_ack_parse(bytes, copied, length, headerLength, &older)
			|| !brcms_ack_same_flow(&newer, &older))
			continue;
		// A later data/SACK/ECN/window update is a barrier in this flow.
		if ((previous->m_flags & (M_TXCB | M_EAPOL)) != 0
            || ((previous->m_flags ^ m->m_flags) & M_AMPDU_MPDU) != 0 || !brcms_ack_supersedes(&newer, &older))
			return NULL;
		m_copyback(m, 22, 2, (caddr_t)(bytes + 22));
		M_SEQNO_SET(m, M_SEQNO_GET(previous));
		entry->packet = m;
		sc->sc_ackCoalesced++;
		return previous;
	}
	return NULL;
}



// Only identical, payload-free IPv4 resets without IP/TCP options qualify.
// IPv4 ID/checksum and the not-yet-published 802.11 sequence are immaterial;
// every other byte, including the TCP checksum, must be unchanged.
static bool
brcms_reset_same(const uint8_t* newer, uint newLength,
	const uint8_t* older, uint oldLength, uint header)
{
	if ((header != 26 && header != 28) || newLength != header + 8 + 40
		|| oldLength != newLength)
		return false;
	const uint8_t* ip = newer + header + 8;
	const uint8_t* previous = older + header + 8;
	if (ip[0] != 0x45 || previous[0] != 0x45 || ip[9] != 6
		|| (ip[6] & 0xbf) != 0 || ip[7] != 0
		|| ip[2] != 0 || ip[3] != 40 || ip[32] != 0x50
		|| (ip[33] != 0x04 && ip[33] != 0x14)
		|| (newer[1] & 0x18) != 0 || (newer[24] & 0xf0) != 0
		|| newer[25] != 0 || (newer[22] & 15) != 0
		|| (older[22] & 15) != 0)
		return false;
	return memcmp(newer, older, 22) == 0
		&& memcmp(newer + 24, older + 24, header + 8 + 4 - 24) == 0
		&& memcmp(ip + 6, previous + 6, 4) == 0
		&& memcmp(ip + 12, previous + 12, 28) == 0;
}


static struct mbuf*
brcms_coalesce_reset(struct brcms_softc* sc, struct ieee80211_node* ni,
	struct mbuf* m)
{
	if (sc->sc_txPending.count < 32
		|| (m->m_flags & (M_AMPDU_MPDU | M_TXCB | M_EAPOL)) != M_AMPDU_MPDU)
		return NULL;
	uint8_t bytes[128];
	int length = m_length(m, NULL);
	if (length < 26 || length > (int)sizeof(bytes))
		return NULL;
	m_copydata(m, 0, length, (caddr_t)bytes);
	uint header = ieee80211_hdrspace(&sc->sc_ic,
		(const struct ieee80211_frame*)bytes);
	struct brcms_ack_key key;
	if (!brcms_ack_parse(bytes, length, length, header, &key)
		|| !brcms_reset_same(bytes, length, bytes, length, header))
		return NULL;
	for (uint n = sc->sc_txPending.count; n != 0; n--) {
		uint slot = (sc->sc_txPending.head + n - 1) % BRCMS_TX_PENDING;
		struct brcms_pending_tx* entry = &sc->sc_txPending.entries[slot];
		if (entry->raw)
			return NULL;
		if (entry->node != ni)
			continue;
		struct mbuf* previous = entry->packet;
		int oldLength = m_length(previous, NULL);
		uint8_t oldBytes[128];
		int copied = oldLength < (int)sizeof(oldBytes)
			? oldLength : sizeof(oldBytes);
		m_copydata(previous, 0, copied, (caddr_t)oldBytes);
		struct brcms_ack_key older;
		if (!brcms_ack_parse(oldBytes, copied, oldLength, header, &older))
			return NULL;
		if (!brcms_ack_same_flow(&key, &older))
			continue;
		// A different later packet in this flow remains a barrier.
		if ((previous->m_flags & (M_AMPDU_MPDU | M_TXCB | M_EAPOL))
				!= M_AMPDU_MPDU
			|| !brcms_reset_same(bytes, length, oldBytes, oldLength, header))
			return NULL;
		m_copyback(m, 22, 2, (caddr_t)(oldBytes + 22));
		M_SEQNO_SET(m, M_SEQNO_GET(previous));
		entry->packet = m;
		sc->sc_resetCoalesced++;
		return previous;
	}
	return NULL;
}


// TCP connection control has no payload to defer. Preserve its opportunity
// to stop an old flow or establish a new one when ordinary data fills the queue.
static bool
brcms_tcp_control(const uint8_t* tcp, uint available, uint length)
{
	if (available < 20 || length < 20)
		return false;
	uint headerLength = (tcp[12] >> 4) * 4;
	return headerLength >= 20 && headerLength == length
		&& (tcp[13] & 0x07) != 0;
}


// Keep the original data capacity and allow a small number of extra
// control packets. SACK/zero-window ACKs still have all 256 data entries;
// payload-free SYN/FIN/RST share the ICMP/ARP/DHCP/EAPOL reserve.
static bool
brcms_control_frame(struct brcms_softc* sc, struct mbuf* m)
{
	if ((m->m_flags & (M_TXCB | M_EAPOL)) != 0)
		return true;
	uint8_t bytes[128];
	int length = m_length(m, NULL);
	if (length < 32)
		return false;
	int copied = length < (int)sizeof(bytes) ? length : sizeof(bytes);
	m_copydata(m, 0, copied, (caddr_t)bytes);
	uint headerLength = ieee80211_hdrspace(&sc->sc_ic,
		(const struct ieee80211_frame*)bytes);
	if ((bytes[0] & 0x0c) != 0x08 || headerLength + 8 > (uint)copied)
		return false;
	static const uint8_t snap[] = {0xaa, 0xaa, 3, 0, 0, 0};
	const uint8_t* llc = bytes + headerLength;
	if (memcmp(llc, snap, sizeof(snap)) != 0)
		return false;
	uint type = (llc[6] << 8) | llc[7];
	if (type == 0x0806 || type == 0x888e)
		return true;
	const uint8_t* ip = llc + 8;
	uint available = copied - headerLength - 8;
	uint ipLength;
	uint sourcePort, destinationPort;
	if (type == 0x0800 && available >= 20 && (ip[0] >> 4) == 4) {
		ipLength = (ip[0] & 15) * 4;
		if (ipLength < 20 || ipLength > available)
			return false;
		if (ip[9] == 1)
			return true;
		if (ip[9] == 6) {
			uint total = (ip[2] << 8) | ip[3];
			return (ip[6] & 0x3f) == 0 && ip[7] == 0
				&& total >= ipLength
				&& headerLength + 8 + total <= (uint)length
				&& brcms_tcp_control(ip + ipLength, available - ipLength,
					total - ipLength);
		}
		if (ip[9] != 17 || available < ipLength + 8)
			return false;
		sourcePort = (ip[ipLength] << 8) | ip[ipLength + 1];
		destinationPort = (ip[ipLength + 2] << 8) | ip[ipLength + 3];
		return (sourcePort == 67 && destinationPort == 68)
			|| (sourcePort == 68 && destinationPort == 67);
	}
	if (type == 0x86dd && available >= 40 && (ip[0] >> 4) == 6) {
		if (ip[6] == 58)
			return true;
		if (ip[6] == 6) {
			uint payload = (ip[4] << 8) | ip[5];
			return headerLength + 8 + 40 + payload <= (uint)length
				&& brcms_tcp_control(ip + 40, available - 40, payload);
		}
		if (ip[6] != 17 || available < 48)
			return false;
		sourcePort = (ip[40] << 8) | ip[41];
		destinationPort = (ip[42] << 8) | ip[43];
		return (sourcePort == 546 && destinationPort == 547)
			|| (sourcePort == 547 && destinationPort == 546);
	}
	return false;
}



// Classify rejected TCP ACKs independently of coalescing eligibility.
// In particular, SACK-bearing ACKs are still visible to this diagnostic.
static uint
brcms_full_kind(struct brcms_softc* sc, struct mbuf* m)
{
    uint8_t bytes[128];
    int length = m_length(m, NULL);
    if (length < 26) return 0;
    int copied = length < (int)sizeof(bytes) ? length : sizeof(bytes);
    m_copydata(m, 0, copied, (caddr_t)bytes);
    uint header = ieee80211_hdrspace(&sc->sc_ic,
        (const struct ieee80211_frame*)bytes);
    struct brcms_ack_key newer;
    if (!brcms_ack_parse(bytes, copied, length, header, &newer)) return 0;
    const uint8_t* ip = bytes + header + 8;
    uint ih = (ip[0] & 15) * 4;
    const uint8_t* tcp = ip + ih;
    uint th = (tcp[12] >> 4) * 4;
    uint total = ((uint)ip[2] << 8) | ip[3];
    if (total > ih + th) return 5;
    if (tcp[13] != 0x10) return 6;
    if (newer.window == 0) return 3;
    for (uint n = sc->sc_txPending.count; n != 0; n--) {
        uint slot = (sc->sc_txPending.head + n - 1) % BRCMS_TX_PENDING;
        struct brcms_pending_tx* entry = &sc->sc_txPending.entries[slot];
        if (entry->raw) break;
        length = m_length(entry->packet, NULL);
        copied = length < (int)sizeof(bytes) ? length : sizeof(bytes);
        m_copydata(entry->packet, 0, copied, (caddr_t)bytes);
        struct brcms_ack_key older;
        if (brcms_ack_parse(bytes, copied, length, header, &older)
            && brcms_ack_same_flow(&newer, &older)) {
            if (newer.acknowledgment == older.acknowledgment) return 4;
            return 2;
        }
    }
    return 1;
}

static void
brcms_queue_full_sample(struct brcms_softc* sc, struct mbuf* m)
{
    uint busy = sc->sc_queued - sc->sc_completed;
    sFullBusy[busy == 0 ? 0 : busy == 1 ? 1 : busy < 32 ? 2 : 3]++;
    sFullKinds[brcms_full_kind(sc, m)]++;
    if (sc->sc_txPending.count != 0) {
        bigtime_t age = system_time() - sQueueEntered[sc->sc_txPending.head];
        if (age > sFullHeadMax) sFullHeadMax = age;
    }
}

static int
brcms_queue_frame(struct brcms_softc* sc, struct ieee80211_node* ni,
	struct mbuf* m, bool raw)
{
	struct brcms_pending_tx entry = { m, ni, raw };
	struct mbuf* obsolete = NULL;
	mtx_lock(&sc->sc_txLock);
	int result = 0;
	if (!sc->sc_running)
		result = ENETDOWN;
	else if (!raw && ((obsolete = brcms_coalesce_reset(sc, ni, m)) != NULL
		|| (obsolete = brcms_coalesce_ack(sc, ni, m)) != NULL)) {
		// Admission reuses the existing queue entry and its sequence slot.
	} else if (sc->sc_txPending.count >= BRCMS_TX_DATA_PENDING
		&& !raw && !brcms_control_frame(sc, m)) {
		sc->sc_txQueueFull++;
		brcms_queue_full_sample(sc, m);
		result = ENOBUFS;
	} else if (!brcms_tx_enqueue(&sc->sc_txPending, entry)) {
		sc->sc_txQueueFull++;
		brcms_queue_full_sample(sc, m);
		result = ENOBUFS;
	} else if (sc->sc_txPending.count > BRCMS_TX_DATA_PENDING)
		sc->sc_txControlReserveUsed++;
	if (result == 0) {
        if (obsolete == NULL) {
            uint slot = (sc->sc_txPending.head + sc->sc_txPending.count - 1)
                % BRCMS_TX_PENDING;
            sQueueEntered[slot] = system_time();
        }
		// Assign sequence numbers only after admission, while both the
		// net80211 TX lock and the queue lock still protect this frame.
		if (obsolete == NULL && !raw && (m->m_flags & M_AMPDU_MPDU) != 0)
			ieee80211_output_seqno_assign(ni, -1, m);
		taskqueue_enqueue(taskqueue_thread, &sc->sc_txTask);
	}
	mtx_unlock(&sc->sc_txLock);
	if (obsolete != NULL) {
		m_freem(obsolete);
		ieee80211_free_node(ni);
	}
	return result;
}


static void
brcms_discard_pending(struct brcms_softc* sc)
{
	struct brcms_pending_tx entry;
	for (;;) {
		mtx_lock(&sc->sc_txLock);
		bool found = brcms_tx_dequeue(&sc->sc_txPending, &entry);
		mtx_unlock(&sc->sc_txLock);
		if (!found)
			break;
		m_freem(entry.packet);
		ieee80211_free_node(entry.node);
	}
}


/*!	Sends whatever has been held back.

	What is staged is taken under the lock and sent outside it: sending
	ends in net80211, and net80211's locks are above this driver's.
*/

static uint8_t
brcms_tx_wire_rate(struct ieee80211_node* ni, uint8_t rate, bool mcs,
	bool wide)
{
	uint32_t capability = wide ? IEEE80211_NODE_SGI40 : IEEE80211_NODE_SGI20;
	return rate | (mcs && (ni->ni_flags & capability) != 0
		? BRCMS_TX_RATE_SGI : 0);
}


// DMA credit alone is insufficient: retain every older sequence inside
// the peer's BA window until its retransmission completes. Management,
// other nodes/TIDs and pending BARs keep the original serialization.
static bool
brcms_batch_fits(struct brcms_softc* sc, uint count)
{
    uint busy = sc->sc_queued - sc->sc_completed;
    if (busy == 0)
        return true;
    if (sc->sc_barNode != NULL || busy + count > BRCMS_PIPELINE_CREDIT)
        return false;
    struct ieee80211_node* ni = sStageNode[0];
    uint window = ni->ni_tx_ampdu[sStageTID].txa_wnd;
    if (window > 64)
        window = 64;
    if (count == 0 || count > window || window < 2)
        return false;
    uint last = (sStage[count - 1].bytes[22]
        | (sStage[count - 1].bytes[23] << 8)) >> 4;
    uint found = 0;
    for (uint slot = 0; slot < BRCMS_IN_FLIGHT; slot++) {
        if (sc->sc_inFlight[slot] == NULL)
            continue;
        found++;
        if (sc->sc_inFlight[slot] != ni || sc->sc_baTID[slot] != sStageTID
            || ((last - sc->sc_baSequence[slot]) & 0xfff) >= window)
            return false;
    }
    return found == busy;
}

static bool
brcms_stage_flush(struct brcms_softc* sc)
{
	struct brcms_glue_frame frames[BRCMS_STAGE_MOST];
	struct ieee80211_node* nodes[BRCMS_STAGE_MOST];
	mtx_lock(&sc->sc_txLock);
	uint count = sStaged;
	if (count == 0) {
		mtx_unlock(&sc->sc_txLock);
		return true;
	}
	uint window = count == 1 ? BRCMS_IN_FLIGHT - 1
		: sStageNode[0]->ni_tx_ampdu[sStageTID].txa_wnd;
	if (window >= BRCMS_IN_FLIGHT)
		window = BRCMS_IN_FLIGHT - 1;
	bool fitsWindow = count <= window;
	if (sc->sc_running && fitsWindow
		&& !brcms_batch_fits(sc, count)) {
		sStageBlocked++;
		mtx_unlock(&sc->sc_txLock);
		callout_reset(&sc->sc_flush, 1, brcms_flush_timer, sc);
		return false;
	}
	for (uint i = 0; i < count; i++) {
		frames[i].bytes = sStage[i].bytes;
		frames[i].length = sStage[i].length;
        frames[i].keySlot = sStageKeySlot[i];
		nodes[i] = sStageNode[i];
	}
	status_t status = B_ERROR;
	uint reserved = 0;
	if (sc->sc_running && fitsWindow) {
		while (reserved < count && brcms_tx_token_reserve(&sc->sc_txTokens,
			&frames[reserved].token))
			reserved++;
		if (reserved == count)
			status = brcms_glue_send_aggregate(frames, count,
				brcms_tx_wire_rate(nodes[0], sStageRate, true, sStageWide),
				sStageWide);
	}
	if (status == B_OK) {
        uint busy = sc->sc_queued - sc->sc_completed;
        if (busy != 0) sPipelineBatches++;
        if (busy + count > sPipelinePeak) sPipelinePeak = busy + count;
        bigtime_t age = system_time() - sStageBorn;
        if (age > sStageWaitMax) sStageWaitMax = age;
		sAggregateSizes[count < 4 ? 0 : count < 8 ? 1 : count < 16 ? 2 : 3]++;
		for (uint i = 0; i < count; i++) {
			brcms_queued(sc, nodes[i], true, true, sStageRate, frames[i].token);
			uint slot = frames[i].token % BRCMS_IN_FLIGHT;
            sTxKeySlot[slot] = sStageKeySlot[i];
            sStageKeySlot[i] = 0;
			sEchoToken[slot] = sEchoStage[i];
			sc->sc_baTID[slot] = sStageTID;
			sc->sc_baSequence[slot] =
				(sStage[i].bytes[22] | (sStage[i].bytes[23] << 8)) >> 4;
		}
	} else {
		for (uint i = 0; i < reserved; i++)
			brcms_tx_token_release(&sc->sc_txTokens, frames[i].token);
	}
	for (uint i = 0; i < count; i++) {
        brcms_glue_tx_key_release(sStageKeySlot[i]);
        sStageKeySlot[i] = 0;
        sStageNode[i] = NULL;
    }
	sStaged = sStageBytes = 0;
	mtx_unlock(&sc->sc_txLock);
	if (status != B_OK) {
		for (uint i = 0; i < count; i++)
			ieee80211_free_node(nodes[i]);
	}
	return true;
}


static void
brcms_flush_timer(void* argument)
{
	brcms_stage_flush((struct brcms_softc*)argument);
}


/*!	Holds one frame back, in the hope of another arriving behind it. */

static uint
brcms_aggregate_limit(struct ieee80211_node* ni, uint8_t rate, bool wide)
{
	static const uint8_t units[] = {1, 2, 3, 4, 6, 8, 9, 10,
		2, 4, 6, 8, 12, 16, 18, 20};
	uint peer = (1U << (13 + (ni->ni_htparam & 3))) - 1;
	// At most 5 ms of payload, leaving room for preamble and rounding
	// below the 5.484 ms PPDU limit. Rates are conservative long GI.
	uint airtime = units[rate & 15] * (wide ? 13500 : 6500) * 5 / 8;
	return peer < airtime ? peer : airtime;
}


static int
brcms_stage_add(struct brcms_softc* sc, struct ieee80211_node* ni,
	struct mbuf* m, uint8_t rate, bool wide)
{
	uint8_t tid;
	m_copydata(m, 24, 1, (caddr_t)&tid);
	tid &= 0x0f;
	if (ni->ni_tx_ampdu[tid].txa_wnd < 2) {
		if (!brcms_stage_flush(sc))
			return ENOBUFS;
		return brcms_send_mbuf(ni, m, rate, true, false, wide);
	}
    uint64 echo = brcms_echo_key(ni->ni_ic->ic_softc, m);
	struct ieee80211_frame* wh = mtod(m, struct ieee80211_frame*);
	if ((wh->i_fc[1] & IEEE80211_FC1_PROTECTED) != 0
		&& ieee80211_crypto_encap(ni, m) == NULL)
		return ENOBUFS;
	int length = m_length(m, NULL);
	if (length < 26 || (size_t)length > sizeof(sStage[0].bytes))
		return EMSGSIZE;
	uint bytes = roundup(length + 4 + (brcms_crypto_slot(m) != 0 ? 8 : 0), 4) + 4;
	uint limit = brcms_aggregate_limit(ni, rate, wide);
	uint most = ni->ni_tx_ampdu[tid].txa_wnd;
	if (most > BRCMS_STAGE_MOST)
		most = BRCMS_STAGE_MOST;
	mtx_lock(&sc->sc_txLock);
	if (sStaged != 0 && (sStageNode[0] != ni || sStageTID != tid
		|| sStageRate != rate || sStageWide != wide
		|| sStaged == most || sStageBytes + bytes > limit)) {
		mtx_unlock(&sc->sc_txLock);
		if (!brcms_stage_flush(sc))
			return ENOBUFS;
		mtx_lock(&sc->sc_txLock);
	}
	if (!sc->sc_running) {
		mtx_unlock(&sc->sc_txLock);
		return ENETDOWN;
	}
	bool first = sStaged == 0;
	if (first)
		sStageBorn = system_time();
	uint slot = sStaged++;
	m_copydata(m, 0, length, (caddr_t)sStage[slot].bytes);
	sStage[slot].length = length;
    sStageKeySlot[slot] = brcms_crypto_take(m);
	sStageNode[slot] = ni;
	sEchoStage[slot] = echo;
	sStageRate = rate;
	sStageWide = wide;
	sStageTID = tid;
	sStageBytes += bytes;
	bool full = sStaged == most;
	mtx_unlock(&sc->sc_txLock);
	m_freem(m);
	if (full)
		brcms_stage_flush(sc);
	else if (first)
		callout_reset(&sc->sc_flush, 1, brcms_flush_timer, sc);
	return 0;
}


static void
brcms_rate_report(void)
{
	uint32 best[3] = { 0, 0, 0 };
	int which[3] = { -1, -1, -1 };

	for (int i = 0; i < 64; i++) {
		for (int slot = 0; slot < 3; slot++) {
			if (sTxByRate[i] > best[slot]) {
				for (int move = 2; move > slot; move--) {
					best[move] = best[move - 1];
					which[move] = which[move - 1];
				}
				best[slot] = sTxByRate[i];
				which[slot] = i;
				break;
			}
		}
	}

	printf("broadcom43xxn: tx: %" B_PRIu32 " as mcs, %" B_PRIu32
		" as legacy; most used %d (%" B_PRIu32 "), %d (%" B_PRIu32
		"), %d (%" B_PRIu32 "); now at mcs %u%s, %" B_PRIu32 " protected\n",
		sTxMcs, sTxLegacy,
		which[0], best[0], which[1], best[1], which[2], best[2],
		(unsigned)sMcs, !sMcsUsable ? " (given up on 802.11n)"
			: sSettled < BRCMS_MCS_SETTLE ? " (still settling)" : "",
		sTxProtected);

	printf("broadcom43xxn: tx: %" B_PRIu32 " successful, %" B_PRIu32
		" sends per hundred of them\n", sTxSucceeded,
		sTxSucceeded != 0 ? sTxRetries * 100 / sTxSucceeded : 0);
}


static void
brcms_queued(struct brcms_softc* sc, struct ieee80211_node* ni,
	bool feedback, bool mcs, uint8 rate, uint32_t token)
{
	uint32_t slot = token % BRCMS_IN_FLIGHT;

	mtx_assert(&sc->sc_txLock, MA_OWNED);
	KASSERT(sc->sc_inFlight[slot] == NULL, ("brcms TX completion slot busy"));

	KASSERT(sc->sc_txCallback[slot] == NULL, ("brcms TX callback slot busy"));
	sc->sc_inFlight[slot] = ni;
	sc->sc_baTID[slot] = 0xff;
	sc->sc_rateFeedback[slot] = feedback;
	sc->sc_sentMcs[slot] = mcs;
	sc->sc_sentRate[slot] = rate;
	sEchoToken[slot] = 0;
	sPublishedAt[slot] = system_time();
	sc->sc_queued++;
}


// All retries in the current aggregate must finish before advancing the
// peer's reorder window past a lost MPDU. Keep a node reference until then.
// Returns a reference to release outside the TX mutex on submission failure.
static struct ieee80211_node*
brcms_send_pending_bar(struct brcms_softc* sc)
{
	if (sc->sc_barNode == NULL || sc->sc_queued != sc->sc_completed)
		return NULL;
	struct ieee80211_node* ni = sc->sc_barNode;
	sc->sc_barNode = NULL;
	if (!sc->sc_running || ni->ni_vap == NULL)
		return ni;

	uint8_t frame[20] = { 0x84, 0 };
	// Reserve enough NAV for a compressed BA even at a 1 Mbps basic rate.
	uint16_t duration = IEEE80211_IS_CHAN_5GHZ(sc->sc_ic.ic_curchan)
		? 84 : 458;
	frame[2] = duration & 0xff;
	frame[3] = duration >> 8;
	memcpy(frame + 4, ni->ni_macaddr, 6);
	memcpy(frame + 10, ni->ni_vap->iv_myaddr, 6);
	uint16_t control = (sc->sc_barTID << 12) | 4; // compressed, immediate BA
	uint16_t sequence = sc->sc_barSequence << 4;
	frame[16] = control & 0xff;
	frame[17] = control >> 8;
	frame[18] = sequence & 0xff;
	frame[19] = sequence >> 8;
	uint8_t rate = IEEE80211_IS_CHAN_5GHZ(sc->sc_ic.ic_curchan) ? 12 : 2;
	uint32_t token;
	if (brcms_tx_token_reserve(&sc->sc_txTokens, &token)) {
		if (brcms_glue_send_frame(frame, sizeof(frame), rate, false, false,
				false, token) == B_OK) {
			brcms_queued(sc, ni, false, false, rate, token);
			sc->sc_barsSent++;
			return NULL;
		}
		brcms_tx_token_release(&sc->sc_txTokens, token);
	}
	return ni;
}


/*!	Tells the stack how a frame went, so that it can choose a rate.

	Without this the rate control never learns anything and stays wherever
	it started, which is either slower than the air allows or faster than
	it bears.
*/
static void
brcms_sent(void* cookie, uint32 token, bool acknowledged, uint retries)
{
	struct brcms_softc* sc = cookie;

	mtx_lock(&sc->sc_txLock);
	if (!brcms_tx_token_release(&sc->sc_txTokens, token)) {
		mtx_unlock(&sc->sc_txLock);
		return;
	}

	uint32_t slot = token % BRCMS_IN_FLIGHT;
    brcms_glue_tx_key_release(sTxKeySlot[slot]);
    sTxKeySlot[slot] = 0;
	struct ieee80211_node* ni = sc->sc_inFlight[slot];
	sc->sc_inFlight[slot] = NULL;
	struct mbuf* callback = sc->sc_txCallback[slot];
	sc->sc_txCallback[slot] = NULL;
	bigtime_t elapsed = system_time() - sPublishedAt[slot];
	uint bucket = elapsed < 250 ? 0 : elapsed < 1000 ? 1
		: elapsed < 4000 ? 2 : elapsed < 16000 ? 3
		: elapsed < 64000 ? 4 : 5;
	sCompletionLatency[sc->sc_baTID[slot] < 16][bucket]++;
    uint64 echo = sEchoToken[slot];
    if (echo != 0) {
        uint kind = (echo >> 33) == 8;
        sEchoCompleted[kind]++;
        uint64 flow = echo & ~((uint64)0xffff);
        uint index;
        for (index = 0; index < 8; index++)
            if (sEchoFlows[index].key == flow) break;
        if (index == 8) {
            index = sEchoFlowNext++ % 8;
            memset(&sEchoFlows[index], 0, sizeof(sEchoFlows[index]));
            sEchoFlows[index].key = flow;
        }
        sEchoFlows[index].total++;
        if (!acknowledged) sEchoFlows[index].failed++;
        uint seq = echo & 0xffff;
        if (seq < 512) sEchoFlows[index].lowSequences[seq / 64] |= (uint64)1 << (seq % 64);
        if (elapsed > sEchoChipMax[kind]) sEchoChipMax[kind] = elapsed;
        if (!acknowledged) {
            sEchoFailed[kind]++;
            if (sEchoFailPrinted++ < 16)
                device_printf(sc->sc_dev, "echo TX failed: type %u id %u seq %u chip %lld us token %u\n",
                    (uint)(echo >> 33), (uint)((echo >> 16) & 0xffff), (uint)(echo & 0xffff), (long long)elapsed, token);
        }
    }
	if (sc->sc_rateFeedback[slot]) {
		brcms_rate_answered(acknowledged, retries,
			sc->sc_sentMcs[slot], sc->sc_sentRate[slot]);
	}
	if (!acknowledged && ni != NULL && sc->sc_baTID[slot] < 16) {
		uint16_t next = (sc->sc_baSequence[slot] + 1) & 0xfff;
		if (sc->sc_barNode == NULL) {
			sc->sc_barNode = ieee80211_ref_node(ni);
			sc->sc_barTID = sc->sc_baTID[slot];
			sc->sc_barSequence = next;
		} else if (sc->sc_barNode == ni
			&& sc->sc_barTID == sc->sc_baTID[slot]
			&& ((next - sc->sc_barSequence) & 0xfff) < 2048) {
			sc->sc_barSequence = next;
		}
	}
	sc->sc_completed++;
	struct ieee80211_node* unusedBarNode = brcms_send_pending_bar(sc);
	if (sc->sc_running)
		taskqueue_enqueue(taskqueue_thread, &sc->sc_txTask);
	mtx_unlock(&sc->sc_txLock);
	if (unusedBarNode != NULL)
		ieee80211_free_node(unusedBarNode);

	if (ni == NULL)
		return;

	struct ieee80211_ratectl_tx_status status;
	memset(&status, 0, sizeof(status));
	status.flags = IEEE80211_RATECTL_STATUS_LONG_RETRY;
	status.long_retries = retries;
	status.status = acknowledged ? IEEE80211_RATECTL_TX_SUCCESS
		: IEEE80211_RATECTL_TX_FAIL_LONG;

	ieee80211_ratectl_tx_complete(ni, &status);

	// How many times the chip had to send it before anything came back.
	// Air spent on a frame nobody heard is air not spent on the next one.
	sTxRetries += retries;
	if (acknowledged)
		sTxSucceeded++;

	if (ni->ni_vap != NULL && ni->ni_vap->iv_ifp != NULL) {
		if_inc_counter(ni->ni_vap->iv_ifp,
			acknowledged ? IFCOUNTER_OPACKETS : IFCOUNTER_OERRORS, 1);
	}

	if (callback != NULL) {
		ieee80211_process_callback(ni, callback, acknowledged ? 0 : EIO);
		m_freem(callback);
	}
	ieee80211_free_node(ni);
}


// The original node reference stays in its token slot until this retry
// completes. DMA publication and token lookup share the normal TX mutex.
static bool
brcms_retry(void* cookie, uint32 token, void* packet)
{
	struct brcms_softc* sc = cookie;
	mtx_lock(&sc->sc_txLock);
	bool sent = sc->sc_running && brcms_tx_token_valid(&sc->sc_txTokens, token)
		&& brcms_glue_retry_frame(packet,
            sc->sc_sentMcs[token % BRCMS_IN_FLIGHT] ? sMcsMask : 0) == B_OK;
	mtx_unlock(&sc->sc_txLock);
	return sent;
}


/*!	Takes a frame the chip heard and gives it to the stack. */
static void
brcms_received(void* cookie, const void* frame, size_t length, int rssi,
	uint32 channel, uint16 cryptoStatus)
{
	struct brcms_softc* sc = cookie;

	if (length < sizeof(struct ieee80211_frame_min) || length > MCLBYTES)
		return;

	struct mbuf* m = m_get2(length, M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL)
		return;

	memcpy(mtod(m, void*), frame, length);
	m->m_pkthdr.len = m->m_len = length;

	// Nothing is associated while scanning, so frames go in without a node
	// of their own and net80211 works out whose each one is. The signal
	// arrives as a height above the quiet this driver assumes, and the two
	// together are the dBm the stack shows.
	struct ieee80211vap* vap = TAILQ_FIRST(&sc->sc_ic.ic_vaps);
	if (vap != NULL && vap->iv_ifp != NULL)
		if_inc_counter(vap->iv_ifp, IFCOUNTER_IPACKETS, 1);

	// Anything watching gets it with a note of how it arrived.
	if (ieee80211_radiotap_active(&sc->sc_ic)) {
		struct ieee80211_channel* channel = sc->sc_ic.ic_curchan;

		sc->sc_rxTap.wr_flags = 0;
		sc->sc_rxTap.wr_rate = 0;
		sc->sc_rxTap.wr_chan_freq = B_HOST_TO_LENDIAN_INT16(channel->ic_freq);
		sc->sc_rxTap.wr_chan_flags = B_HOST_TO_LENDIAN_INT16(channel->ic_flags);
		sc->sc_rxTap.wr_antsignal = rssi >= 0 ? rssi + BRCMS_NOISE_FLOOR : BRCMS_NOISE_FLOOR;
		sc->sc_rxTap.wr_antnoise = BRCMS_NOISE_FLOOR;
	}

	// How the frame arrived, including which channel it was heard on.
	//
	// That last part matters more than it looks. A station throws away a
	// beacon whose own idea of its channel differs from the channel the
	// stack believes it was listening on -- signal leaking in from a
	// neighbouring channel is not something to plan an association around.
	// Without a channel of its own a frame is judged against whatever the
	// scan has retuned to by the time it is handed over, and a scan retunes
	// several times a second while the ring holds frames heard before the
	// last retune. Nearly every beacon is then thrown away: what survives
	// is whatever happens to have been heard on a channel the scan has not
	// left yet.
	struct ieee80211_rx_stats rx;
	memset(&rx, 0, sizeof(rx));
	rx.r_flags = IEEE80211_R_NF | IEEE80211_R_RSSI;
	rx.c_nf = BRCMS_NOISE_FLOOR;
	rx.c_rssi = rssi;
	// Private plaintext hint: software still checks MIC and PN.
	if ((cryptoStatus & 8) != 0)
		rx.c_pktflags |= 0x80000000U | (((uint32)cryptoStatus >> 5 & 63) << 24);

	if (channel >= 1 && channel <= 196) {
		rx.r_flags |= IEEE80211_R_IEEE | IEEE80211_R_FREQ | IEEE80211_R_BAND;
		rx.c_ieee = channel;
		rx.c_band = channel <= 14 ? IEEE80211_CHAN_2GHZ : IEEE80211_CHAN_5GHZ;
		rx.c_freq = ieee80211_ieee2mhz(channel, rx.c_band);
	}

	if (!ieee80211_add_rx_params(m, &rx)) {
		m_freem(m);
		return;
	}

	struct ieee80211_node* ni = ieee80211_find_rxnode(&sc->sc_ic,
		(const struct ieee80211_frame_min*)frame);
	if (ni != NULL) {
		// Reorder HT MPDUs before software decryption checks their packet
		// numbers. Retransmitted holes in a block-ACK window otherwise
		// arrive after newer packets and are rejected as replays.
		if ((ni->ni_flags & IEEE80211_NODE_HT) != 0)
			m->m_flags |= M_AMPDU;
		ieee80211_input_mimo(ni, m);
		ieee80211_free_node(ni);
	} else
		ieee80211_input_mimo_all(&sc->sc_ic, m);
}


/*!	Reads the ring in case the card is not asking.

	If the interrupt works this finds nothing and costs a read a quarter of
	a second. If it does not, the interface still works, slowly, and says
	which of the two happened rather than appearing to hear nothing.
*/
static bigtime_t sPhyWatchdogMax;
static bigtime_t sPhyWatchdogWaitMax;
static uint32 sPhyWatchdogCalls;
static uint32 sPhyWatchdogSlow;
static uint32 sPhyWatchdogPrinted;


static void
brcms_watchdog(void* argument)
{
	struct brcms_softc* sc = argument;

	if (!sc->sc_running)
		return;

	if (brcms_glue_interrupt_count() > 0) {
		if (!sc->sc_everInterrupted) {
			sc->sc_everInterrupted = true;
			device_printf(sc->sc_dev, "the card raises its interrupt\n");
		}
	} else {
		brcms_glue_poll_frames();
	}
	// Also finish a delayed second block-ACK status if its interrupt was
	// coalesced with the first one. Normal IRQ-driven completions stay fast.
	brcms_glue_reclaim_sent();

	// The phy's own second: the noise it is hearing, the gain it should be
	// receiving at, and whichever calibration is due. This runs four times
	// a second, so every fourth tick.
	if ((sc->sc_ticks % 4) == 0 && brcms_glue_settings()->phy_watchdog) {
		// Channel changes and WME updates also suspend/resume this MAC.
		// Serialize the whole operation, not only individual MMIO accesses.
		bigtime_t waiting = system_time();
		mtx_lock(&sc->sc_txLock);
		bigtime_t started = system_time();
		if (sc->sc_running)
			brcms_glue_phy_watchdog();
		bigtime_t elapsed = system_time() - started;
		mtx_unlock(&sc->sc_txLock);
		bigtime_t waited = started - waiting;
		sPhyWatchdogCalls++;
		if (elapsed > sPhyWatchdogMax)
			sPhyWatchdogMax = elapsed;
		if (waited > sPhyWatchdogWaitMax)
			sPhyWatchdogWaitMax = waited;
		if (elapsed > 5000 || waited > 5000) {
			sPhyWatchdogSlow++;
			if (sPhyWatchdogPrinted++ < 8)
				device_printf(sc->sc_dev,
					"PHY watchdog slow: held %lld us, waited %lld us\n",
					(long long)elapsed, (long long)waited);
		}
	}

    mtx_lock(&sc->sc_cryptoLock);
    brcms_glue_tx_key_sweep();
    mtx_unlock(&sc->sc_cryptoLock);

	// Every half minute, what the card has been doing. There is nothing
	// else to go on when a network will not be joined and the only thing
	// to hand is the log.
	if (++sc->sc_ticks % (4 * 30) == 0) {
		brcms_glue_interrupt_report();
		brcms_glue_rx_report();
		brcms_glue_rx_poll_report();
		brcms_glue_tx_report();
		brcms_glue_dma_report();
		brcms_glue_mac_report();
		brcms_glue_chip_report();
		brcms_rate_report();
		device_printf(sc->sc_dev,
			"PHY watchdog timing: %lu calls, %lu slow, max held %lld us, max wait %lld us\n",
			(unsigned long)sPhyWatchdogCalls, (unsigned long)sPhyWatchdogSlow,
			(long long)sPhyWatchdogMax, (long long)sPhyWatchdogWaitMax);
		sPhyWatchdogMax = sPhyWatchdogWaitMax = 0;
		sPhyWatchdogCalls = sPhyWatchdogSlow = 0;
		mtx_lock(&sc->sc_txLock);
        device_printf(sc->sc_dev, "queue dwell: <250us %u <1ms %u <4ms %u <16ms %u <64ms %u rest %u max %lld us full head %lld us\n",
            sQueueWait[0], sQueueWait[1], sQueueWait[2], sQueueWait[3],
            sQueueWait[4], sQueueWait[5], (long long)sQueueWaitMax, (long long)sFullHeadMax);
        device_printf(sc->sc_dev, "queue full busy: zero %u one %u below32 %u rest %u; kinds other %u unmatched %u different %u zero %u same %u\n",
            sFullBusy[0], sFullBusy[1], sFullBusy[2], sFullBusy[3],
            sFullKinds[0], sFullKinds[1], sFullKinds[2], sFullKinds[3], sFullKinds[4]);
        device_printf(sc->sc_dev, "stage dwell: max %lld us blocked %u\n",
            (long long)sStageWaitMax, sStageBlocked);
        device_printf(sc->sc_dev, "echo TX: reply %u failed %u max %lld us; request %u failed %u max %lld us\n",
            sEchoCompleted[0], sEchoFailed[0], (long long)sEchoChipMax[0],
            sEchoCompleted[1], sEchoFailed[1], (long long)sEchoChipMax[1]);
        for (uint index = 0; index < 8; index++) {
            if (sEchoFlows[index].key == 0) continue;
            device_printf(sc->sc_dev, "echo flow: type %u id %u total %u failed %u seq0-511 %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx\n",
                (uint)(sEchoFlows[index].key >> 33), (uint)((sEchoFlows[index].key >> 16) & 0xffff),
                sEchoFlows[index].total, sEchoFlows[index].failed,
                (unsigned long long)sEchoFlows[index].lowSequences[0], (unsigned long long)sEchoFlows[index].lowSequences[1],
                (unsigned long long)sEchoFlows[index].lowSequences[2], (unsigned long long)sEchoFlows[index].lowSequences[3],
                (unsigned long long)sEchoFlows[index].lowSequences[4], (unsigned long long)sEchoFlows[index].lowSequences[5],
                (unsigned long long)sEchoFlows[index].lowSequences[6], (unsigned long long)sEchoFlows[index].lowSequences[7]);
        }
        device_printf(sc->sc_dev, "BA pipeline: batches %u peak %u\n", sPipelineBatches, sPipelinePeak);
        device_printf(sc->sc_dev, "TX prepared during prior batch: %u\n", sPreparedOverlap);
        device_printf(sc->sc_dev, "queue full non-ACK: payload %u control %u\n", sFullKinds[5], sFullKinds[6]);
		for (uint kind = 0; kind < 2; kind++) {
			device_printf(sc->sc_dev, "TX latency %u: <250us %u <1ms %u "
				"<4ms %u <16ms %u <64ms %u rest %u\n", kind,
				sCompletionLatency[kind][0], sCompletionLatency[kind][1],
				sCompletionLatency[kind][2], sCompletionLatency[kind][3],
				sCompletionLatency[kind][4], sCompletionLatency[kind][5]);
		}
		device_printf(sc->sc_dev, "TX aggregate sizes: <4 %u <8 %u <16 %u rest %u\n",
			sAggregateSizes[0], sAggregateSizes[1], sAggregateSizes[2],
			sAggregateSizes[3]);
		mtx_unlock(&sc->sc_txLock);
		struct ieee80211vap* vap = TAILQ_FIRST(&sc->sc_ic.ic_vaps);
		if (vap != NULL) {
			struct ieee80211_stats* st = &vap->iv_stats;
			device_printf(sc->sc_dev,
				"rx stack: dup %u replay %u mic %u "
				"decap %u nobuf %u reorder %u drop %u age %u bar %u\n",
				st->is_rx_dup, st->is_rx_ccmpreplay, st->is_rx_ccmpmic,
				st->is_rx_decap, st->is_rx_nobuf, st->is_ampdu_rx_reorder,
				st->is_ampdu_rx_drop, st->is_ampdu_rx_age, st->is_ampdu_bar_rx);
		}
		mtx_lock(&sc->sc_txLock);
		device_printf(sc->sc_dev, "tx recovery: %u BARs sent\n", sc->sc_barsSent);
		device_printf(sc->sc_dev, "ACK coalesced: %u\n", sc->sc_ackCoalesced);
		device_printf(sc->sc_dev, "identical queued resets coalesced: %u\n",
			sc->sc_resetCoalesced);
		device_printf(sc->sc_dev, "control reserve admissions: %u\n",
			sc->sc_txControlReserveUsed);
		device_printf(sc->sc_dev, "tx queue: %u pending, %u full, %u in flight\n",
			sc->sc_txPending.count, sc->sc_txQueueFull,
			sc->sc_queued - sc->sc_completed);
        device_printf(sc->sc_dev, "A-MSDU hardware-input batches %u subframes %u\n", sAmsduFrames, sAmsduSubframes);
        device_printf(sc->sc_dev, "CCMP key slots reused %u\n", brcms_glue_key_slots_reused());
		mtx_unlock(&sc->sc_txLock);
	}

	callout_reset(&sc->sc_watchdog, hz / 4, brcms_watchdog, sc);
}


// A second block-ACK status need not raise another interrupt. Retry it
// promptly instead of leaving the transmit queue stopped until watchdog.
static void brcms_status_retry(void* argument);

static void
brcms_schedule_status_retry(struct brcms_softc* sc)
{
	mtx_lock(&sc->sc_txLock);
	if (sc->sc_running && brcms_glue_tx_status_pending()
		&& !callout_pending(&sc->sc_statusRetry))
		callout_reset(&sc->sc_statusRetry, 1, brcms_status_retry, sc);
	mtx_unlock(&sc->sc_txLock);
}


static void
brcms_status_retry(void* argument)
{
	struct brcms_softc* sc = argument;
	mtx_lock(&sc->sc_txLock);
	bool running = sc->sc_running;
	mtx_unlock(&sc->sc_txLock);
	if (!running)
		return;
	brcms_glue_reclaim_sent();
	brcms_schedule_status_retry(sc);
}


/*!	Deals with what the card said, the interrupt no longer being held.

	The compatibility layer has already decided the line was this card's
	and quieted it; this runs in a thread of its own, and the layer lets
	the card ask again when it returns.
*/
extern void bus_continue_intr(void* cookie);


static void
brcms_interrupt(void* argument)
{
	struct brcms_softc* sc = argument;

	if (!sc->sc_running)
		return;

	brcms_glue_interrupt_handle();
	brcms_schedule_status_retry(sc);
	if (brcms_glue_rx_pending())
		bus_continue_intr(sc->sc_interruptCookie);
}


/*!	Brings the interface up or takes it down. */
static void
brcms_parent(struct ieee80211com* ic)
{
	struct brcms_softc* sc = ic->ic_softc;
	bool wanted = ic->ic_nrunning > 0;

	if (wanted == sc->sc_running)
		return;

	if (wanted) {
		if (brcms_glue_listen() != B_OK)
			return;

		sc->sc_running = true;
		brcms_glue_interrupts_on();
		callout_reset(&sc->sc_watchdog, hz / 4, brcms_watchdog, sc);
		ieee80211_start_all(ic);
	} else {
		mtx_lock(&sc->sc_txLock);
		sc->sc_running = false;
		callout_stop(&sc->sc_watchdog);
		brcms_glue_interrupts_off();
		brcms_glue_quiet();
		mtx_unlock(&sc->sc_txLock);
		brcms_discard_pending(sc);
	}
}


/*!	Encrypts the frame if it is to be, and hands the bytes to the chip.

	net80211 does the ciphering in software and leaves it to the driver to
	ask for it: a frame marked protected that is sent without this goes out
	in the clear, which an access point answers by saying nothing.

	The mbuf is freed either way.
*/
static int
brcms_send_mbuf(struct ieee80211_node* ni, struct mbuf* m, uint8_t rate,
	bool mcs, bool protect, bool wide)
{
    uint64 echo = brcms_echo_key(ni->ni_ic->ic_softc, m);
	struct ieee80211_frame* wh = mtod(m, struct ieee80211_frame*);

	if ((wh->i_fc[1] & IEEE80211_FC1_PROTECTED) != 0
		&& ieee80211_crypto_encap(ni, m) == NULL)
		return ENOBUFS;

	bool hasCallback = (m->m_flags & M_TXCB) != 0;
	uint8_t frame[2048];
	int length = m_length(m, NULL);

	if (length <= 0 || (size_t)length > sizeof(frame))
		return EMSGSIZE;

	m_copydata(m, 0, length, (caddr_t)frame);

	struct brcms_softc* sc = ni->ni_ic->ic_softc;
	mtx_lock(&sc->sc_txLock);
	int result = sc->sc_running ? ENOBUFS : ENETDOWN;
	uint32_t token;
	if (sc->sc_running
		&& sc->sc_queued - sc->sc_completed < BRCMS_IN_FLIGHT - 1
		&& brcms_tx_token_reserve(&sc->sc_txTokens, &token)) {
		result = brcms_glue_send_frame_hw(frame, length,
			brcms_tx_wire_rate(ni, rate, mcs, wide), mcs, protect, wide,
			token, brcms_crypto_slot(m)) == B_OK ? 0 : EIO;
		if (result != 0)
			brcms_tx_token_release(&sc->sc_txTokens, token);
		// Publish the node before a completion can consume its slot.
		if (result == 0) {
			brcms_queued(sc, ni,
				(frame[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_DATA
					&& !IEEE80211_IS_MULTICAST(frame + 4)
					&& (ni->ni_flags & IEEE80211_NODE_HT) != 0,
				mcs, rate, token);
			sEchoToken[token % BRCMS_IN_FLIGHT] = echo;
            sTxKeySlot[token % BRCMS_IN_FLIGHT] = brcms_crypto_take(m);
			if (hasCallback)
				sc->sc_txCallback[token % BRCMS_IN_FLIGHT] = m;
		}
	}
	mtx_unlock(&sc->sc_txLock);
	if (result != 0)
		return result;

	// A callback's tags must survive until the hardware reports its result.
	// Publish that ownership under the TX lock before completion can run.
	if (!hasCallback)
		m_freem(m);
	return 0;
}


/*!	Sends a frame net80211 built itself, which is how it talks.

	Everything an association is made of comes through here. It goes at the
	slowest rate: these matter more than they are frequent, and the slowest
	rate is the one most likely to be heard.

	On success this driver owns the station reference and lets it go; on
	failure the caller does.
*/
static int
brcms_raw_xmit(struct ieee80211_node* ni, struct mbuf* m,
	const struct ieee80211_bpf_params* params)
{
	struct brcms_softc* sc = ni->ni_ic->ic_softc;

	uint8_t frameType;
	m_copydata(m, 0, 1, (caddr_t)&frameType);
	static uint32 authSamples;
	if (frameType == 0xb0 && authSamples++ < 64) {
		device_printf(sc->sc_dev, "auth context: scanning %u state %u "
			"icflags %#x chan %u chanflags %#x\n",
			(ni->ni_ic->ic_flags & IEEE80211_F_SCAN) != 0,
			ni->ni_vap->iv_state, ni->ni_ic->ic_flags,
			ieee80211_chan2ieee(ni->ni_ic, ni->ni_ic->ic_curchan),
			ni->ni_ic->ic_curchan->ic_flags);
	}

	// The other contract: ieee80211_raw_output() releases the node when
	// this returns an error, and leaves the frame to this driver.
	if (!sc->sc_running) {
		m_freem(m);
		return ENETDOWN;
	}

	// A scan may retune as soon as this callback returns. Submit its
	// management frame on the current channel, not later on the worker.
	int result = brcms_send_mbuf(ni, m, 0, false, false, false);
	if (result != 0)
		m_freem(m);

	return result;
}


struct brcms_tx_choice {
    uint8_t rate;
    bool mcs, protect, wide, aggregate;
};

// Called with sc_txLock held. Keep the selected rate through preparation;
// completion feedback may change the next packet's rate in the meantime.
static struct brcms_tx_choice
brcms_tx_choose(struct brcms_softc* sc, struct ieee80211_node* ni, struct mbuf* m)
{
    struct ieee80211com* ic = &sc->sc_ic;
	// What to send it at. For an HT network the stack names a modulation
	// and coding scheme rather than a speed, and the chip takes that in
	// the same field with a bit set to say which it is.
	// A data frame to a station that speaks 802.11n goes at whatever has
	// been working. Anything else -- and anything to a station that does
	// not -- is left to net80211, which is right about the frames that
	// have to be heard rather than fast.
	struct ieee80211_frame* wh = mtod(m, struct ieee80211_frame*);
	bool data = (wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK)
		== IEEE80211_FC0_TYPE_DATA;

	// And only once the station is allowed to carry data. The frames that
	// get it there -- the association, and the four that agree the key --
	// are the ones with nothing to fall back to, so they go out the way
	// net80211 asks and are never an experiment.
	uint8_t rate;
	bool mcs = brcms_glue_settings()->mcs && sMcsUsable && data
		&& sSettled >= BRCMS_MCS_SETTLE
		&& (ni->ni_flags & IEEE80211_NODE_HT) != 0
		&& (sMcsMask & (1U << sMcs)) != 0;

	if (mcs) {
		rate = sMcs;
	} else if (ni->ni_txrate.type == IEEE80211_NODE_TXRATE_HT) {
		mcs = true;
		rate = ni->ni_txrate.mcs;
	} else {
		// The bit that marks a rate as one the network requires is a
		// property of the network, not of how fast to send.
		rate = ieee80211_node_get_txrate_dot11rate(ni)
			& IEEE80211_RATE_VAL;
	}

	// Older stations in the network cannot read a frame sent at these
	// rates, and a station that cannot read one talks over it. net80211
	// says when there are any; the slowest rates are the ones they can
	// read and need no protecting.
	bool protect = (ic->ic_flags & IEEE80211_F_USEPROT) != 0
		&& (mcs || rate > 22);

	bool wide = mcs && IEEE80211_IS_CHAN_HT40(ic->ic_curchan);

    struct brcms_tx_choice choice = {rate, mcs, protect, wide, false};
    // Payload-bearing data aggregates; pure ACKs keep the A-MSDU path.
    choice.aggregate = (mcs && brcms_glue_settings()->ampdu && !protect
		&& m_length(m, NULL) >= 256
		&& ((rate >= 3 && rate <= 7) || rate >= 9)
		&& ((ni->ni_htparam & 0x1c) == 0 || m_length(m, NULL) >= 1024)
		&& (wh->i_fc[0] & 0x8c) == 0x88
		&& (wh->i_fc[1] & IEEE80211_FC1_MORE_FRAG) == 0
		&& (wh->i_seq[0] & 0x0f) == 0
		&& (m->m_flags & (M_AMPDU_MPDU | M_TXCB)) == M_AMPDU_MPDU);
    return choice;
}

// No dequeue, encryption or ownership transfer until the next frame
// fits the staged batch. Timer flushes may empty it, but only this worker
// adds frames. The on-air batch must still complete before stage_flush
// publishes another batch, including all retries and the pending BAR.
static bool
brcms_single_fits(struct brcms_softc* sc, struct brcms_pending_tx* entry,
    const struct brcms_tx_choice* choice)
{
    uint busy = sc->sc_queued - sc->sc_completed;
    if (entry->raw || choice->aggregate || !choice->mcs || sStaged != 0
        || sc->sc_barNode != NULL || busy == 0
        || busy >= brcms_glue_settings()->tx_inflight
        || (entry->packet->m_flags & (M_TXCB | M_EAPOL)) != 0
        || m_length(entry->packet, NULL) < 34)
        return false;
    uint8_t header[26];
    m_copydata(entry->packet, 0, sizeof(header), (caddr_t)header);
    if ((header[0] & 0xfc) != 0x88 || (header[1] & 0x47) != 0x41
        || (header[4] & 1) != 0 || (header[22] & 15) != 0)
        return false;
    uint found = 0;
    for (uint slot = 0; slot < BRCMS_IN_FLIGHT; slot++) {
        if (sc->sc_inFlight[slot] == NULL) continue;
        if (sc->sc_inFlight[slot] != entry->node || sc->sc_baTID[slot] != 0xff
            || sc->sc_txCallback[slot] != NULL || sTxKeySlot[slot] < 4)
            return false;
        found++;
    }
    return found == busy;
}


static bool
brcms_prepare_fits(struct brcms_softc* sc, struct brcms_pending_tx* entry,
    const struct brcms_tx_choice* choice)
{
    if (brcms_single_fits(sc, entry, choice)) return true;
    if (entry->raw || !choice->aggregate || sc->sc_barNode != NULL)
        return false;
    uint8_t tid;
    m_copydata(entry->packet, 24, 1, (caddr_t)&tid);
    tid &= 15;
    struct ieee80211_node* ni = entry->node;
    uint most = ni->ni_tx_ampdu[tid].txa_wnd;
    if (most > BRCMS_STAGE_MOST)
        most = BRCMS_STAGE_MOST;
    // Conservatively allow 64 bytes of crypto growth before encap.
    uint bytes = roundup(m_length(entry->packet, NULL) + 64 + 4, 4) + 4;
    return most >= 2 && sStaged < most
        && (sStaged == 0 || (sStageNode[0] == ni && sStageTID == tid
            && sStageRate == choice->rate && sStageWide == choice->wide))
        && sStageBytes + bytes <= brcms_aggregate_limit(ni, choice->rate, choice->wide);
}

/*!	Sends a data frame, at whatever rate net80211 has settled on.

	The station is in the frame's own header field, and this driver owns
	that reference however it goes.
*/
static int
brcms_transmit_now(struct ieee80211com* ic, struct mbuf* m,
    const struct brcms_tx_choice* choice)
{
	struct brcms_softc* sc = ic->ic_softc;
	struct ieee80211_node* ni = (struct ieee80211_node*)m->m_pkthdr.rcvif;

	// Nothing here may free either the frame or the node on the way out:
	// ieee80211_parent_xmitpkt() reads the node back out of the frame and
	// releases both itself when this returns an error. A frame freed here
	// is one it then reads a dead pointer out of.
	//
	// Which leaves nowhere to report a frame that arrived without a node,
	// since the report is what would read it. Take that one and drop it.
	if (ni == NULL) {
		m_freem(m);
		return 0;
	}

	if (!sc->sc_running)
		return ENETDOWN;

    uint8_t rate = choice->rate;
    bool mcs = choice->mcs, protect = choice->protect, wide = choice->wide;

	if (mcs)
		sTxMcs++;
	else
		sTxLegacy++;
	if (protect)
		sTxProtected++;
	sTxByRate[rate < 64 ? rate : 63]++;

	// An aggregate is only ever data frames at one scheme and one width,
	// so anything else sends what is waiting first and keeps the order.
	// M_AMPDU_MPDU is the stack saying this frame belongs to a block
	// acknowledgement agreement that is up and running. Without one the
	// aggregate goes unanswered, so this is a condition and not a hint.
	// A conservative aggregate fits every peer's 8 KiB minimum, stays
	// below the PPDU time limit and satisfies even 16 us MPDU spacing.
	if (choice->aggregate)
		return brcms_stage_add(sc, ni, m, rate, wide);

	if (!brcms_stage_flush(sc))
		return ENOBUFS;

	return brcms_send_mbuf(ni, m, rate, mcs, protect, wide);
}


static int
brcms_transmit(struct ieee80211com* ic, struct mbuf* m)
{
	struct ieee80211_node* ni = (struct ieee80211_node*)m->m_pkthdr.rcvif;
	if (ni == NULL) {
		m_freem(m);
		return 0;
	}
	IEEE80211_TX_LOCK_ASSERT(ic);
	return brcms_queue_frame(ic->ic_softc, ni, m, false);
}


static unsigned
brcms_merge_ack_msdu(struct brcms_softc* sc, struct brcms_pending_tx* entry,
    const struct brcms_tx_choice* choice, struct mbuf** retired)
{
    if (entry->raw || choice->aggregate || !choice->mcs
        || sc->sc_txPending.count < 2) return 0;
    // Owned by sc_txLock, which every caller holds: too large for the stack.
    static uint8_t bytes[BRCMS_AMSDU_MOST][192];
    static struct renku_msdu_input inputs[BRCMS_AMSDU_MOST];
    static uint8_t packed[BRCMS_AMSDU_BYTES];
    unsigned count = 0;
    size_t packedBytes = 26;
    for (; count < BRCMS_AMSDU_MOST && count < sc->sc_txPending.count; count++) {
        uint slot = (sc->sc_txPending.head + count) % BRCMS_TX_PENDING;
        struct brcms_pending_tx* next = &sc->sc_txPending.entries[slot];
        struct mbuf* m = next->packet;
        if (next->raw || next->node != entry->node || m->m_nextpkt != NULL
            || (m->m_flags & (M_TXCB | M_EAPOL)) != 0) break;
        int length = m_length(m, NULL);
        if (length < 34 || length > 192) break;
        m_copydata(m, 0, length, (caddr_t)bytes[count]);
        struct brcms_ack_key ack;
        if (!brcms_ack_parse(bytes[count], length, length, 26, &ack)) break;
        // Packing keeps every byte, unlike coalescing, so any payload-free
        // TCP segment may share the A-MSDU: SACK, window updates, duplicates.
        const uint8_t* ip = bytes[count] + 34;
        size_t ipLength = (ip[0] & 15) * 4;
        size_t tcpLength = (ip[ipLength + 12] >> 4) * 4;
        if (((size_t)ip[2] << 8 | ip[3]) != ipLength + tcpLength) break;
        size_t subBytes = (14 + (size_t)length - 26 + 3) & ~(size_t)3;
        if (packedBytes + subBytes > BRCMS_AMSDU_BYTES) break;
        packedBytes += subBytes;
        inputs[count].bytes = bytes[count];
        inputs[count].length = length;
    }
    if (count < 2) return 0;
    size_t length = renku_amsdu_pack(inputs, count, packed, sizeof(packed));
    if (length == 0) return 0;
    struct mbuf* merged = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
    if (merged == NULL) return 0;
    merged->m_data += 32; // Leave room for CCMP IV insertion.
    memcpy(mtod(merged, void*), packed, length);
    merged->m_len = merged->m_pkthdr.len = length;
    merged->m_pkthdr.rcvif = (void*)entry->node;
    merged->m_flags |= entry->packet->m_flags & M_AMPDU_MPDU;
    M_WME_SETAC(merged, M_WME_GETAC(entry->packet));
    bigtime_t born = sQueueEntered[sc->sc_txPending.head];
    for (unsigned i = 0; i < count; i++) {
        struct brcms_pending_tx removed;
        brcms_tx_dequeue(&sc->sc_txPending, &removed);
        retired[i] = removed.packet;
    }
    sQueueEntered[(sc->sc_txPending.head + BRCMS_TX_PENDING - 1)
        % BRCMS_TX_PENDING] = born;
    entry->packet = merged; // First node reference now belongs to this mbuf.
    sAmsduFrames++;
    sAmsduSubframes += count;
    return count;
}


static void
brcms_tx_task(void* argument, int pending)
{
	struct brcms_softc* sc = argument;
	struct brcms_pending_tx entry;
	for (;;) {
		mtx_lock(&sc->sc_txLock);
        if (sc->sc_running && sStaged != 0
            && (sc->sc_queued == sc->sc_completed || sStaged == BRCMS_STAGE_MOST
                || sStaged >= sStageNode[0]->ni_tx_ampdu[sStageTID].txa_wnd)
            && brcms_batch_fits(sc, sStaged)) {
            // Completions wake this worker: publish already prepared data
            // immediately, even if no newly queued packet remains.
            mtx_unlock(&sc->sc_txLock);
            brcms_stage_flush(sc);
            continue;
        }
        if (!sc->sc_running || sc->sc_txPending.count == 0) {
            mtx_unlock(&sc->sc_txLock);
            return;
        }
        entry = sc->sc_txPending.entries[sc->sc_txPending.head];
        struct brcms_tx_choice choice = {0};
        if (!entry.raw)
            choice = brcms_tx_choose(sc, entry.node, entry.packet);
        bool busy = sc->sc_queued != sc->sc_completed;
        if (busy && !brcms_prepare_fits(sc, &entry, &choice)) {
            mtx_unlock(&sc->sc_txLock);
            return;
        }
        struct mbuf* retired[BRCMS_AMSDU_MOST];
        unsigned combined = brcms_merge_ack_msdu(sc, &entry, &choice, retired);
        if (combined == 0) brcms_tx_dequeue(&sc->sc_txPending, &entry);
        if (busy)
            sPreparedOverlap++;
        uint slot = (sc->sc_txPending.head + BRCMS_TX_PENDING - 1) % BRCMS_TX_PENDING;
        bigtime_t age = system_time() - sQueueEntered[slot];
        uint bucket = age < 250 ? 0 : age < 1000 ? 1 : age < 4000 ? 2
            : age < 16000 ? 3 : age < 64000 ? 4 : 5;
        sQueueWait[bucket]++;
        if (age > sQueueWaitMax) sQueueWaitMax = age;
        uint64 echo = brcms_echo_key(sc, entry.packet);
        bool printEcho = echo != 0 && age > 50000 && sEchoSlowPrinted++ < 16;
		mtx_unlock(&sc->sc_txLock);
        if (printEcho)
            device_printf(sc->sc_dev, "echo queue slow: type %u id %u seq %u wait %lld us\n",
                (uint)(echo >> 33), (uint)((echo >> 16) & 0xffff), (uint)(echo & 0xffff), (long long)age);

        for (unsigned i = 0; i < combined; i++) {
            m_freem(retired[i]);
            if (i != 0) ieee80211_free_node(entry.node);
        }

		int result = entry.raw
			? brcms_send_mbuf(entry.node, entry.packet, 0, false, false, false)
			: brcms_transmit_now(&sc->sc_ic, entry.packet, &choice);
		if (result != 0) {
			// Enqueue accepted ownership from net80211, so asynchronous
			// failures must release both references here.
			ieee80211_tx_complete(entry.node, entry.packet, result);
		}
	}
}


/*!	What this driver keeps alongside net80211's own idea of a station. */
struct brcms_vap {
	struct ieee80211vap	bv_vap;
	int					(*bv_newstate)(struct ieee80211vap*,
							enum ieee80211_state, int);
};


/*!	Follows net80211 from scanning to associated and back.

	The only thing the chip needs told is whose network it is on. Its
	receive engine matches against the address, so once it is set frames
	from that network arrive without everything else having to.
*/
static int
brcms_newstate(struct ieee80211vap* vap, enum ieee80211_state nstate, int arg)
{
	struct brcms_vap* bvp = (struct brcms_vap*)vap;
	enum ieee80211_state ostate = vap->iv_state;

	int error = bvp->bv_newstate(vap, nstate, arg);
	if (error != 0)
		return error;

	if (nstate == IEEE80211_S_RUN && vap->iv_bss != NULL) {
		if (ostate != IEEE80211_S_RUN) {
			struct brcms_softc* sc = vap->iv_ic->ic_softc;
			mtx_lock(&sc->sc_txLock);
			sSettled = sMcsGood = sMcsBad = 0;
			sMcs = 0;
			sMcsUsable = true;
			sMcsMask = 0;
			const struct ieee80211_htrateset* rates = &vap->iv_bss->ni_htrates;
			for (uint i = 0; i < rates->rs_nrates; i++) {
				uint8 rate = rates->rs_rates[i] & IEEE80211_RATE_VAL;
				if (rate <= BRCMS_MCS_TOP)
					sMcsMask |= 1U << rate;
			}
			device_printf(sc->sc_dev, "negotiated TX MCS mask %#x\n", sMcsMask);
			mtx_unlock(&sc->sc_txLock);
		}
		brcms_glue_set_bssid(vap->iv_bss->ni_bssid);
		brcms_glue_set_beacon_period(vap->iv_bss->ni_intval);

		// Dozing is what net80211 was asked for, which is nothing
		// unless somebody said so: the card stays awake by default.
		brcms_glue_set_power_save(
			(vap->iv_flags & IEEE80211_F_PMGTON) != 0);
		// Staying promiscuous after joining is for finding out what the
		// access point is willing to send anybody else, and at what
		// speed. It costs every frame in the room coming up this far.
		brcms_glue_set_filter(vap->iv_opmode == IEEE80211_M_MONITOR
			|| brcms_glue_settings()->listen_in);
	} else if (ostate == IEEE80211_S_RUN) {
		brcms_glue_set_bssid(NULL);
		brcms_glue_set_filter(true);
	}

	return 0;
}


// Shared by key-update callbacks and the static net80211 crypto library.
// Lock spans key selection through PN commit, not just the AES primitive.
void
renku_crypto_enter(struct ieee80211vap* vap)
{
	struct brcms_softc* sc = vap->iv_ic->ic_softc;
	mtx_lock(&sc->sc_cryptoLock);
}

void
renku_crypto_leave(struct ieee80211vap* vap)
{
	struct brcms_softc* sc = vap->iv_ic->ic_softc;
	mtx_unlock(&sc->sc_cryptoLock);
}

extern int renku_ccmp_installed_slot(const struct ieee80211_key*);
extern void renku_ccmp_rx_slot(const struct ieee80211_key*, int);

static int
brcms_key_set(struct ieee80211vap* vap, const struct ieee80211_key* key)
{
	if (ieee80211_is_key_global(vap, key)) {
		renku_ccmp_rx_slot(key, -1);
		unsigned keyid = key->wk_keyix;
		if (vap->iv_opmode != IEEE80211_M_STA || keyid < 1 || keyid > 3)
			return 1;
		if (key->wk_keylen == 16 && key->wk_cipher != NULL
			&& key->wk_cipher->ic_cipher == IEEE80211_CIPHER_AES_CCM
			&& (key->wk_flags & IEEE80211_KEY_RECV) != 0) {
			status_t slot = brcms_glue_group_key(keyid, key->wk_key);
			if (slot == (status_t)keyid)
				renku_ccmp_rx_slot(key, slot);
			return slot == B_ERROR ? 0 : 1;
		}
		brcms_glue_group_key(keyid, NULL);
		return 1;
	}
	renku_ccmp_rx_slot(key, -1);
	status_t slot = B_NOT_SUPPORTED;
	if (vap->iv_opmode == IEEE80211_M_STA && key->wk_keylen == 16
		&& key->wk_cipher->ic_cipher == IEEE80211_CIPHER_AES_CCM
		&& (key->wk_flags & IEEE80211_KEY_RECV) != 0)
		slot = brcms_glue_rx_key(key->wk_key, key->wk_macaddr);
	else
		brcms_glue_rx_key(NULL, NULL);
	if (slot >= 4 && slot < 54)
		renku_ccmp_rx_slot(key, slot);
	return slot == B_ERROR || slot == B_BAD_VALUE ? 0 : 1;
}

static int
brcms_key_delete(struct ieee80211vap* vap, const struct ieee80211_key* key)
{
	if (ieee80211_is_key_global(vap, key)) {
		int slot = renku_ccmp_installed_slot(key);
		renku_ccmp_rx_slot(key, -1);
		if (slot >= 1 && slot <= 3 && (unsigned)slot == key->wk_keyix)
			brcms_glue_group_key(slot, NULL);
		return 1;
	}
	if (!ieee80211_is_key_global(vap, key)) {
		int slot = renku_ccmp_installed_slot(key);
		renku_ccmp_rx_slot(key, -1);
		brcms_glue_rx_key_retire(slot);
	}
	return 1;
}

static struct ieee80211vap*
brcms_vap_create(struct ieee80211com* ic, const char name[IFNAMSIZ], int unit,
	enum ieee80211_opmode opmode, int flags,
	const uint8_t bssid[IEEE80211_ADDR_LEN],
	const uint8_t address[IEEE80211_ADDR_LEN])
{
	if (!TAILQ_EMPTY(&ic->ic_vaps))
		return NULL;

	struct brcms_vap* bvp = malloc(sizeof(struct brcms_vap), M_80211_VAP,
		M_WAITOK | M_ZERO);
	struct ieee80211vap* vap = &bvp->bv_vap;

	if (ieee80211_vap_setup(ic, vap, name, unit, opmode, flags, bssid) != 0) {
		free(bvp, M_80211_VAP);
		return NULL;
	}

	// net80211's own handling still runs; this only adds to it.
	bvp->bv_newstate = vap->iv_newstate;
	vap->iv_newstate = brcms_newstate;
	vap->iv_wme_update = brcms_wme_update;
	vap->iv_key_update_begin = renku_crypto_enter;
	vap->iv_key_update_end = renku_crypto_leave;
	vap->iv_key_set = brcms_key_set;
	vap->iv_key_delete = brcms_key_delete;

	// How large an aggregate this card will take. net80211 starts every
	// vap at eight kilobytes, which is the smallest the field can say, and
	// it is what went out in the association request:
	//
	//   tx: joining with ht caps 002c, ampdu 18, mcs ff ff 00 00
	//
	// The low two bits of that 0x18 are the length and they read zero. The
	// chip has no trouble with sixty four kilobytes -- it is what
	// brcms_c_ampdu_shm_upd() has a longer microcode timer for.
	vap->iv_ampdu_rxmax = IEEE80211_HTCAP_MAXRXAMPDU_64K;
	vap->iv_ampdu_limit = vap->iv_ampdu_rxmax;
	// Match brcmsmac's AMPDU_DEF_MPDU_DENSITY (encoding 6).
	// Small MPDUs need the device's minimum spacing as well.
	vap->iv_ampdu_density = IEEE80211_HTCAP_MPDUDENSITY_8;

	ieee80211_vap_attach(vap, ieee80211_media_change, ieee80211_media_status,
		address);

	return vap;
}


/*!	Takes the interface apart, and stops this driver first.

	Everything that could hand net80211 a frame, or that holds a node of
	its, has to have stopped before net80211 is taken apart.

	The compatibility layer deletes the vap and only afterwards calls this
	driver's detach, so at this point the watchdog is still ticking and
	the card's interrupt is still live. A frame arriving in between is
	given to a vap that is being freed, and what notices is the
	interface's own lock being destroyed while something still holds it:

	  PANIC: mutex_destroy(): the lock is held by -32768, not by the
	  caller ... if_free_inplace ... brcms_vap_delete ... stop_wlan

	So the card is quietened and everything of this driver's that runs on
	its own is stopped here, where it is still this driver's turn.
*/
// Called only after the TX task, timers and IRQ handler have drained.
// VAP teardown cancels management exchanges; do not restart their timers.
static void
brcms_discard_inflight(struct brcms_softc* sc)
{
	for (size_t i = 0; i < nitems(sc->sc_inFlight); i++) {
        brcms_glue_tx_key_release(sTxKeySlot[i]);
        sTxKeySlot[i] = 0;
		if (sc->sc_txCallback[i] != NULL) {
			m_freem(sc->sc_txCallback[i]);
			sc->sc_txCallback[i] = NULL;
		}
		if (sc->sc_inFlight[i] != NULL) {
			ieee80211_free_node(sc->sc_inFlight[i]);
			sc->sc_inFlight[i] = NULL;
		}
	}
}


static void
brcms_vap_delete(struct ieee80211vap* vap)
{
	struct brcms_softc* sc = vap->iv_ic->ic_softc;

	mtx_lock(&sc->sc_txLock);
	sc->sc_running = false;
	brcms_glue_interrupts_off();
	brcms_glue_quiet();
	mtx_unlock(&sc->sc_txLock);
	taskqueue_drain(taskqueue_thread, &sc->sc_txTask);
	brcms_discard_pending(sc);

	// Not just stopped: waited for. One already running is one that is
	// inside net80211 with a frame.
	callout_drain(&sc->sc_watchdog);
	callout_drain(&sc->sc_flush);
	callout_drain(&sc->sc_statusRetry);

	// And no interrupt may be partway through either, which only taking
	// the handler down can say. brcms_detach() tests for this having been
	// done already.
	if (sc->sc_interruptCookie != NULL) {
		bus_teardown_intr(sc->sc_dev, sc->sc_interrupt,
			sc->sc_interruptCookie);
		sc->sc_interruptCookie = NULL;
	}

	// The stations whose frames were still with the chip. Each is a
	// reference this driver took, and net80211 is about to go.
	brcms_discard_inflight(sc);
	brcms_stage_flush(sc);
	if (sc->sc_barNode != NULL) {
		ieee80211_free_node(sc->sc_barNode);
		sc->sc_barNode = NULL;
	}
	sc->sc_txTokens.occupied = 0;
	sc->sc_completed = sc->sc_queued;

	ieee80211_vap_detach(vap);
	free(vap, M_80211_VAP);		// the vap is first in struct brcms_vap
}


static int
brcms_attach(device_t dev)
{
	struct brcms_softc* sc = device_get_softc(dev);
	struct ieee80211com* ic = &sc->sc_ic;

	brcms_glue_read_settings();
	device_printf(dev, "build channel-power-refresh-1019\n");

	sc->sc_dev = dev;
	sDevice = dev;
	mtx_init(&sc->sc_mtx, device_get_nameunit(dev), MTX_NETWORK_LOCK,
		MTX_DEF);
	mtx_init(&sc->sc_txLock, "brcms tx", MTX_NETWORK_LOCK, MTX_DEF);
	mtx_init(&sc->sc_cryptoLock, "brcms crypto", MTX_NETWORK_LOCK, MTX_RECURSE);
	TASK_INIT(&sc->sc_txTask, 0, brcms_tx_task, sc);

	pci_enable_busmaster(dev);

	// The chip's registers are reached through a window in the first base
	// address register, which is where everything under brcms/ works.
	sc->sc_registersId = PCIR_BAR(0);
	sc->sc_registers = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
		&sc->sc_registersId, RF_ACTIVE);
	if (sc->sc_registers == NULL) {
		device_printf(dev, "cannot map the register window\n");
		mtx_destroy(&sc->sc_cryptoLock);
	mtx_destroy(&sc->sc_txLock);
		mtx_destroy(&sc->sc_mtx);
		return ENXIO;
	}

	// Not rman_get_virtual(): the compatibility layer leaves that at null
	// and puts the mapping in the bus handle.
	addr_t registers = (addr_t)rman_get_bushandle(sc->sc_registers);

	if (brcms_glue_attach(registers) != B_OK) {
		device_printf(dev, "the chip would not start\n");
		bus_release_resource(dev, SYS_RES_MEMORY, sc->sc_registersId,
			sc->sc_registers);
		mtx_destroy(&sc->sc_cryptoLock);
	mtx_destroy(&sc->sc_txLock);
		mtx_destroy(&sc->sc_mtx);
		return ENXIO;
	}

	brcms_glue_address(sc->sc_address);

	ic->ic_softc = sc;
	ic->ic_name = device_get_nameunit(dev);
	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_caps = IEEE80211_C_STA
		| IEEE80211_C_MONITOR
		| IEEE80211_C_SHPREAMBLE
		| IEEE80211_C_SHSLOT
		| IEEE80211_C_WPA;
	// Off-channel background scans need TX drain/resume coordination.
	// The current scan callbacks only hold PHY calibration, so advertise
	// foreground scanning until queued data can be preserved across retunes.
	if (brcms_glue_settings()->edcf)
		ic->ic_caps |= IEEE80211_C_WME;

	// What this card is: two aerials, two streams, and the short guard
	// interval on a twenty megahertz channel. That is three quarters of
	// the speed it has; the rest is forty megahertz channels, which are
	// not asked for above.
	//
	// Frames are aggregated on receive, which is where most of the gain
	// is: the stack reorders what arrives and this driver hands each one
	// up as the chip delivers it, and nothing has to be claimed for that.
	//
	// Sending aggregates is claimed only when this driver is going to
	// send them. The claim is what makes the stack agree a block
	// acknowledgement with the access point, and that agreement is the
	// point of it: an aggregate is answered by one acknowledgement
	// covering the whole run, and an access point that has agreed to no
	// such thing answers an aggregate with nothing at all. Sending them
	// without it was tried, and the card associated and then carried no
	// traffic.
	//
	// Forty megahertz is not claimed. brcms_glue_set_channel() tunes the
	// radio twenty megahertz wide and nothing here builds a wider
	// chanspec, so a network offering forty was being joined at forty and
	// then listened to at twenty: the wide frames could not be read, only
	// the legacy ones on the control channel, and every frame this card
	// was given arrived as CCK while the access point backed down to
	// 802.11b looking for something that worked.
	// IEEE80211_HTCAP_SMPS_OFF is not a nicety. The field is two bits and
	// zero means static spatial multiplexing power save: "only one of my
	// receive chains is powered, so send me one stream, and ask first if
	// you want to use more". Leaving it unset told every access point
	// this card met to be as careful as it knows how -- which is what an
	// access point four feet away was doing when it sent scheme one and
	// never tried anything quicker.
	//
	// Both chains are powered. Say so.
	ic->ic_htcaps = IEEE80211_HTCAP_SMPS_OFF
		| IEEE80211_HTCAP_SHORTGI20
		| IEEE80211_HTCAP_MAXAMSDU_3839
		| IEEE80211_HTC_HT;

	// Forty megahertz, now that brcms_glue_set_channel() can tune it. It
	// was dropped when it could not: a network joined at forty and then
	// listened to at twenty hears only what is sent on the control
	// channel, which is the legacy half.
	if (brcms_glue_settings()->ht40) {
		ic->ic_htcaps |= IEEE80211_HTCAP_CHWIDTH40
			| IEEE80211_HTCAP_SHORTGI40;
	}

	// TX block-ack: aggregates are hardware encrypted with TXC_AMIC.
	if (brcms_glue_settings()->ampdu && brcms_glue_settings()->edcf)
		ic->ic_htcaps |= IEEE80211_HTC_AMPDU;

	// How many spatial streams to tell the network this card can read.
	//
	// Two is what the hardware has. The question one answers is whether an
	// access point that keeps falling back to 802.11b is doing so because
	// it is trying schemes eight to fifteen -- two streams at once -- and
	// getting nothing back. A ladder with rungs this card cannot reach is
	// one an access point stops climbing.
	ic->ic_txstream = brcms_glue_settings()->one_stream ? 1 : 2;
	ic->ic_rxstream = ic->ic_txstream;

	brcms_getradiocaps(ic, IEEE80211_CHAN_MAX, &ic->ic_nchans,
		ic->ic_channels);

	IEEE80211_ADDR_COPY(ic->ic_macaddr, sc->sc_address);

	ieee80211_ifattach(ic);

	ic->ic_vap_create = brcms_vap_create;
	ic->ic_vap_delete = brcms_vap_delete;
	ic->ic_raw_xmit = brcms_raw_xmit;
	ic->ic_transmit = brcms_transmit;
	ic->ic_parent = brcms_parent;
	ic->ic_scan_start = brcms_scan_start;
	ic->ic_scan_end = brcms_scan_end;
	sScanMindwell = ic->ic_scan_mindwell;
	if (sScanMindwell != NULL)
		ic->ic_scan_mindwell = brcms_scan_mindwell;
	ic->ic_getradiocaps = brcms_getradiocaps;
	ic->ic_set_channel = brcms_set_channel;
	ic->ic_updateslot = brcms_updateslot;
	ic->ic_update_promisc = brcms_update_promisc;
	ic->ic_update_mcast = brcms_update_mcast;

	brcms_glue_set_frame_handler(brcms_received, sc);
	brcms_glue_set_sent_handler(brcms_sent, brcms_retry, sc);
	callout_init(&sc->sc_watchdog, 1);
	callout_init(&sc->sc_flush, 1);
	callout_init(&sc->sc_statusRetry, 1);

	// The card's own line. Nothing is unmasked on the card until the
	// interface is brought up, so this is only the arrangement.
	sc->sc_interruptId = 0;
	sc->sc_interrupt = bus_alloc_resource_any(dev, SYS_RES_IRQ,
		&sc->sc_interruptId, RF_ACTIVE | RF_SHAREABLE);
	if (sc->sc_interrupt == NULL) {
		device_printf(dev, "cannot take the interrupt\n");
		ieee80211_ifdetach(ic);
		brcms_glue_tx_key_sweep();
	brcms_glue_detach();
		bus_release_resource(dev, SYS_RES_MEMORY, sc->sc_registersId,
			sc->sc_registers);
		mtx_destroy(&sc->sc_cryptoLock);
	mtx_destroy(&sc->sc_txLock);
		mtx_destroy(&sc->sc_mtx);
		return ENXIO;
	}

	if (bus_setup_intr(dev, sc->sc_interrupt, INTR_TYPE_NET, NULL,
			brcms_interrupt, sc, &sc->sc_interruptCookie) != 0) {
		device_printf(dev, "cannot hook the interrupt up\n");
		bus_release_resource(dev, SYS_RES_IRQ, sc->sc_interruptId,
			sc->sc_interrupt);
		ieee80211_ifdetach(ic);
		brcms_glue_tx_key_sweep();
	brcms_glue_detach();
		bus_release_resource(dev, SYS_RES_MEMORY, sc->sc_registersId,
			sc->sc_registers);
		mtx_destroy(&sc->sc_cryptoLock);
	mtx_destroy(&sc->sc_txLock);
		mtx_destroy(&sc->sc_mtx);
		return ENXIO;
	}

	ieee80211_radiotap_attach(ic,
		&sc->sc_txTap.wt_ihdr, sizeof(sc->sc_txTap),
		BRCMS_TX_RADIOTAP_PRESENT,
		&sc->sc_rxTap.wr_ihdr, sizeof(sc->sc_rxTap),
		BRCMS_RX_RADIOTAP_PRESENT);

	if (bootverbose)
		ieee80211_announce(ic);

	return 0;
}


static int
brcms_detach(device_t dev)
{
	struct brcms_softc* sc = device_get_softc(dev);

	// Most of this has been done already if there was ever an interface:
	// brcms_vap_delete() runs first and has to stop the same things, so
	// that net80211 is not taken apart underneath a frame. Repeating it
	// here is for the card that never got that far.
	mtx_lock(&sc->sc_txLock);
	sc->sc_running = false;
	mtx_unlock(&sc->sc_txLock);
	taskqueue_drain(taskqueue_thread, &sc->sc_txTask);
	brcms_discard_pending(sc);
	callout_drain(&sc->sc_watchdog);
	callout_drain(&sc->sc_flush);
	callout_drain(&sc->sc_statusRetry);

	brcms_glue_interrupts_off();

	if (sc->sc_interruptCookie != NULL) {
		bus_teardown_intr(dev, sc->sc_interrupt, sc->sc_interruptCookie);
		sc->sc_interruptCookie = NULL;
	}
	if (sc->sc_interrupt != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ, sc->sc_interruptId,
			sc->sc_interrupt);
	}

	brcms_discard_inflight(sc);
	brcms_stage_flush(sc);
	if (sc->sc_barNode != NULL) {
		ieee80211_free_node(sc->sc_barNode);
		sc->sc_barNode = NULL;
	}
	sc->sc_txTokens.occupied = 0;
	sc->sc_completed = sc->sc_queued;

	if (sc->sc_ic.ic_softc != NULL)
		ieee80211_ifdetach(&sc->sc_ic);

	brcms_glue_tx_key_sweep();
	brcms_glue_detach();

	if (sc->sc_registers != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, sc->sc_registersId,
			sc->sc_registers);
	}

	mtx_destroy(&sc->sc_cryptoLock);
	mtx_destroy(&sc->sc_txLock);
	mtx_destroy(&sc->sc_mtx);

	return 0;
}


static device_method_t brcms_methods[] = {
	DEVMETHOD(device_probe,		brcms_probe),
	DEVMETHOD(device_attach,	brcms_attach),
	DEVMETHOD(device_detach,	brcms_detach),

	DEVMETHOD_END
};

static driver_t brcms_driver = {
	"brcms",
	brcms_methods,
	sizeof(struct brcms_softc)
};

DRIVER_MODULE(brcms, pci, brcms_driver, 0, 0);
MODULE_DEPEND(brcms, wlan, 1, 1, 1);
MODULE_DEPEND(brcms, pci, 1, 1, 1);
