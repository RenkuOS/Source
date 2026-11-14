/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	Putting a frame on the air.

	Receiving says the radio hears. It says nothing about whether it can be
	heard, and that is the last thing about this card that is not known.
	The quietest way to ask is a probe request: a short management frame
	that every access point in range answers, addressed to nobody in
	particular, carrying no data and joining nothing.

	What goes into the ring is not the frame on its own. In front of it the
	chip wants its own transmit header and then the phy's:

		112 bytes  the chip's transmit header
		  6 bytes  the PLCP, which carries the rate and the length
		     ...   the 802.11 frame

	brcms_c_d11hdrs_mac80211() builds that header, in six hundred lines,
	out of what mac80211 asked for: a rate from the rate controller, a
	fallback rateSpec, aggregation, protection, a key. None of it applies to
	one broadcast management frame at the lowest rateSpec, so the header is
	filled in here instead -- but every field that function writes
	unconditionally is written here too, and worked out with the same
	helpers, because a field left at whatever the buffer held is not the
	same as a field that means nothing.
*/


#include "linux/types.h"

#include <KernelExport.h>
#include <stddef.h>

#include "net/mac80211.h"
#include "linux/ieee80211.h"
#include "linux/skbuff.h"
#include "d11.h"
#include "debug_haiku.h"
#include "dma.h"
#include "glue.h"
#include "glue_net.h"
#include "main.h"
#include "pub.h"
#include "rate.h"
#include "types.h"
#include "tx_fields.h"


#define TRACE(x...)	dprintf("broadcom43xxn: " x)

// A probe request is management, subtype 4, and goes to everybody.
#define FC_MANAGEMENT_PROBE_REQ	0x0040

// A clear-to-send: control frame, subtype 12, and how long one is on the
// air with its check sequence behind it.
#define FC_CONTROL_CTS			0x00c4
#define DOT11_CTS_LEN			10
#define FCS_LEN					4

// The frame check sequence the chip appends, which the length fields have
// to account for.
#define FRAME_CHECK_LENGTH		4

// 802.11 element identifiers used in a probe request.
#define ELEMENT_SSID			0
#define ELEMENT_RATES			1


#define BRCMS_AGGREGATE_MOST	32

// What the chip calls a frame back by. The low bits are the queue it was
// given to; the rest is the frame's own sequence number, which is how one
// frame of a run is told from the next.
#define TXFID_QUEUE_MASK	0x0007
#define TXFID_SEQ_MASK		0x7fe0
#define TXFID_SEQ_SHIFT		5

static uint32 sFramesSent;
static uint32 sDeferredStatus;
static uint32 sAggregatesSent;
static uint32 sAggregatedFrames;
static uint32 sRetryTransmissions;
static uint32 sRetryFallbacks;
static uint32 sRetryLowerRates[16];
static uint32 sSendFailures;
static uint32 sSuppressed;
static uint32 sSuppressedSeen;
static uint32 sFramesCompleted;
static uint32 sFramesAcknowledged;
static uint32 sFramesUnanswered;
static spinlock sTxRingLock = B_SPINLOCK_INITIALIZER;
static int32 sReclaiming;

// Only the first probes are traced. Keep their DMA tokens until completion,
// so a later unrelated frame can never consume a probe's completion log.
// Capture only the D11 descriptor and six-byte PLCP. Never copy the
// 802.11 body. The IV and all descriptor MAC address fields are redacted.
_Static_assert(sizeof(struct d11txh) + D11_PHY_HDR_LEN == TXOFF,
    "probe metadata must contain exactly D11 header and PLCP");

struct brcms_probe_descriptor {
    uint8 bytes[TXOFF];
    bigtime_t time;
    u32 mac;
    u32 ioctl;
    uint length;
};

static bool
probe_descriptor_metadata(const struct sk_buff* packet, uint8* bytes)
{
    if (packet->len < TXOFF + 24)
        return false;
    memcpy(bytes, packet->data, TXOFF);
    memset(bytes + offsetof(struct d11txh, IV), 0,
        sizeof(((struct d11txh*)0)->IV));
    memset(bytes + offsetof(struct d11txh, TxFrameRA), 0,
        sizeof(((struct d11txh*)0)->TxFrameRA));
    memset(bytes + offsetof(struct d11txh, rts_frame)
        + offsetof(struct ieee80211_rts, ra), 0,
        sizeof(((struct ieee80211_rts*)0)->ra));
    memset(bytes + offsetof(struct d11txh, rts_frame)
        + offsetof(struct ieee80211_rts, ta), 0,
        sizeof(((struct ieee80211_rts*)0)->ta));
    return true;
}

static void
probe_descriptor_capture(const struct sk_buff* packet,
    struct brcms_probe_descriptor* snapshot)
{
    struct brcms_hardware* hardware = brcms_glue_hardware();
    probe_descriptor_metadata(packet, snapshot->bytes);
    snapshot->time = system_time();
    snapshot->mac = bcma_read32(hardware->d11core, D11REGOFFS(maccontrol));
    snapshot->ioctl = bcma_aread32(hardware->d11core, BCMA_IOCTL);
    snapshot->length = packet->len;
}

static void
probe_descriptor_emit(const struct brcms_probe_descriptor* snapshot,
    uint32 token, uint phase)
{
    for (uint offset = 0; offset < TXOFF; offset += 32) {
        char hex[65];
        uint count = TXOFF - offset;
        if (count > 32)
            count = 32;
        for (uint i = 0; i < count; i++)
            snprintf(hex + 2 * i, 3, "%02x", snapshot->bytes[offset + i]);
        hex[2 * count] = 0;
        TRACE("probe descriptor: t %lld token %lu phase %u mac %#lx "
            "ioctl %#lx length %u offset %u bytes %s\n",
            (long long)snapshot->time, (unsigned long)token, phase,
            (unsigned long)snapshot->mac, (unsigned long)snapshot->ioctl,
            snapshot->length, offset, hex);
    }
}


struct brcms_probe_trace {
    struct brcms_probe_descriptor before;
    int32 published;
	uint32 token;
	int32 active;
};
static struct brcms_probe_trace sProbeTrace[64];
static int32 sProbeTraceUsed;
static int32 sProbeTraceActive;

bool
brcms_glue_probe_trace_active(void)
{
	return atomic_get(&sProbeTraceActive) != 0;
}

static int
probe_trace_begin(const struct sk_buff* packet, uint32 token)
{
	if (packet->len < TXOFF + 24
		|| (packet->data[TXOFF] & 0xfc) != 0x40
		|| atomic_get(&sProbeTraceUsed) >= 64)
		return -1;
	int slot = atomic_add(&sProbeTraceUsed, 1);
	if (slot >= 64)
		return -1;
	sProbeTrace[slot].token = token;
    probe_descriptor_capture(packet, &sProbeTrace[slot].before);
    atomic_set(&sProbeTrace[slot].published, 1);
	atomic_add(&sProbeTraceActive, 1);
	atomic_set(&sProbeTrace[slot].active, 1);
	return slot;
}

static bool
probe_trace_end(uint32 token)
{
	for (uint i = 0; i < 64; i++) {
		if (atomic_get(&sProbeTrace[i].active) != 0
			&& sProbeTrace[i].token == token
			&& atomic_test_and_set(&sProbeTrace[i].active, 0, 1) == 1) {
			atomic_add(&sProbeTraceActive, -1);
			return true;
		}
	}
	return false;
}

// Called only after the matching tracked completion has been consumed.
// The saved BEFORE descriptor is printed now, never before its transmission.
static void
probe_descriptor_report(const struct sk_buff* packet, uint32 token)
{
    for (uint i = 0; i < 64; i++) {
        if (atomic_get(&sProbeTrace[i].published) == 0
            || sProbeTrace[i].token != token)
            continue;
        struct brcms_probe_descriptor after;
        probe_descriptor_capture(packet, &after);
        probe_descriptor_emit(&sProbeTrace[i].before, token, 0);
        probe_descriptor_emit(&after, token, 1);
        return;
    }
}


struct brcms_tx_packet_info {
	struct ieee80211_tx_info macInfo;
	uint32 token;
	u32 rateSpec;
	uint8 retries;
    uint8 keySlot;
};
_Static_assert(sizeof(struct brcms_tx_packet_info)
	<= sizeof(((struct sk_buff*)0)->cb), "TX metadata must fit skb scratch space");

static struct brcms_tx_packet_info*
packet_info(struct sk_buff* packet)
{
	return (struct brcms_tx_packet_info*)packet->cb;
}



static int
submit_frame(struct brcms_c_info* wlc, struct sk_buff* packet)
{
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sTxRingLock);
	int result = brcms_c_txfifo(wlc, TX_AC_BE_FIFO, packet);
	release_spinlock(&sTxRingLock);
	restore_interrupts(state);
	return result;
}


static void
set_frame_duration(struct d11txh* header, uint8* frame, uint16 ackDuration)
{
	uint8 type = frame[0] & 0x0c;
	// Fragment bursts and control frames (notably PS-Poll's association
	// ID) keep the duration supplied by net80211.
	if ((type == 0 || type == 0x08) && (frame[1] & 0x04) == 0
		&& (frame[22] & 0x0f) == 0) {
		uint16 duration = (frame[4] & 1) != 0 ? 0 : ackDuration;
		frame[2] = duration & 0xff;
		frame[3] = duration >> 8;
	}
	header->FragDurFallback = cpu_to_le16(frame[2] | (frame[3] << 8));
	// No explicit TXOP burst is requested. A nonzero exchange time must
	// include the response, not just this MPDU's airtime.
	header->TxFesTimeNormal = 0;
	header->TxFesTimeFallback = 0;
}


/*!	Puts one frame on the air.

	The MAC has to be running already, which it is once the receiver has
	been opened.
*/
/*!	Builds one frame, headers and all, ready for the ring.

	Handed back rather than sent, because an aggregate is several of these
	with the first and the last corrected once all of them are known.
*/
static struct sk_buff*
build_frame(const void* outgoing, size_t outgoingLength,
	uint8 rate, bool mcs, bool protect, bool wide, bool aggregated, uint32 token)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	struct brcms_c_info* wlc = brcms_glue_common();

	// What the caller asked to send it at: a modulation and coding scheme
	// for an HT network, or a speed in half megabit units otherwise, or
	// the slowest there is if it had no opinion -- which is what a frame
	// sent to nobody in particular goes at, since everything in the room
	// can hear that.
	//
	// The width belongs in the rate specification rather than being left
	// out of it: brcms_c_phytxctl1_calc() reads it from there, and a
	// specification without one reads as a width this chip does not
	// support, which it then complains about and corrects.
	// As wide as the channel the network is using. A frame sent narrow on
	// a wide channel is heard; one sent wide on a narrow channel is not.
	u32 rateSpec = (wide ? PHY_TXC1_BW_40MHZ : PHY_TXC1_BW_20MHZ)
		<< RSPEC_BW_SHIFT;

	if (mcs) {
		if ((rate & BRCMS_TX_RATE_SGI) != 0)
			rateSpec |= RSPEC_SHORT_GI;
		rate &= ~BRCMS_TX_RATE_SGI;
		// Two streams where the scheme asks for them, one where it does
		// not, and the stream mode said alongside.
		rateSpec |= RSPEC_MIMORATE | rate;
		rateSpec |= (rate < 8 ? PHY_TXC1_MODE_SISO : PHY_TXC1_MODE_SDM)
			<< RSPEC_STF_SHIFT;
	} else {
		// The slowest thing that exists depends on which band this is.
		// One megabit is CCK, and there is no CCK above five gigahertz:
		// a frame sent at it there is not a slow frame, it is a frame
		// nothing in the band can hear. Six megabits is the floor
		// there, and it is also the floor for anything that arrived
		// here asking for a CCK rate by mistake.
		bool quiet_band = wlc_hw->band->bandtype == BRCM_BAND_5G;
		uint8 chosen = rate;

		if (chosen == 0)
			chosen = quiet_band ? BRCM_RATE_6M : BRCM_RATE_1M;
		else if (quiet_band && chosen < BRCM_RATE_6M)
			chosen = BRCM_RATE_6M;

		rateSpec |= chosen;
	}

	size_t frameLength = outgoingLength;
	struct sk_buff* p = brcmu_pkt_buf_get_skb(TXOFF + frameLength);
	if (p == NULL) {
		sSendFailures++;
		return NULL;
	}

	memset(p->cb, 0, sizeof(p->cb));
	packet_info(p)->token = token;
	packet_info(p)->rateSpec = rateSpec;
	memset(p->data, 0, TXOFF);

	struct d11txh* header = (struct d11txh*)p->data;
	uint8* plcp = (uint8*)p->data + D11_TXH_LEN;
	uint8* frame = (uint8*)p->data + TXOFF;

	memcpy(frame, outgoing, outgoingLength);
	size_t written = outgoingLength;
	__skb_trim(p, TXOFF + written);

	// How long the air will carry it, which is the frame and the check
	// sequence the chip appends.
	uint onAir = (uint)(written + FRAME_CHECK_LENGTH);

	// Who it is going to is the first of the addresses in the frame, and
	// whether anything will answer for it is the low bit of that: a group
	// address is nobody in particular and nothing acknowledges it.
	const uint8* receiver = frame + 4;
	bool expectAck = (receiver[0] & 1) == 0;

	// What the chip is to do with it: the frame starts a message, the
	// power management queue is not to interrupt it, and the stack supplies
	// the sequence number. An acknowledgement is waited for when one is
	// coming, which is what makes an association possible at all: every
	// step of it is a frame to the access point that it answers.
	u16 controlLow = TXC_IGNOREPMQ;
	if (expectAck)
		controlLow |= TXC_IMMEDACK;

	if (aggregated) {
		// Every frame of an aggregate starts as a middle one; the first
		// and the last are corrected once they are known.
		//
		// And the sequence number stays the one the stack assigned.
		// What answers an aggregate is a block acknowledgement, which
		// is a window over those numbers -- a chip picking its own
		// would answer a window the other end is not keeping.
		controlLow |= (TXC_AMPDU_MIDDLE << TXC_AMPDU_SHIFT);
	} else {
		controlLow |= TXC_STARTMSDU;
		// QoS sequence numbers belong to a per-TID space in net80211.
		// The hardware's shared counter must not replace them.
		if ((frame[0] & 0x0c) != 0x04 && (frame[0] & 0x8c) != 0x88)
			controlLow |= TXC_HWSEQ;
	}

	// Which band and how wide, which the frame has to say for itself. The
	// microcode will not send a frame whose channel is not the one it has
	// been told it is on, and the channel it compares is not just the
	// number: the number comes from this frame's header, eight bits of it,
	// and the band and the width come from here. A frame built on channel
	// 36 without this bit is a frame for channel 36 of the 2.4 GHz band,
	// which does not exist, and the microcode refuses it -- which is why
	// nothing this card built for five gigahertz had ever gone out.
	if (wlc->band->bandtype == BRCM_BAND_5G)
		controlLow |= TXC_FREQBAND_5G;
	if (CHSPEC_IS40(wlc_phy_chanspec_get(wlc->band->pi)))
		controlLow |= TXC_BW_40;

	header->MacTxControlLow = cpu_to_le16(controlLow);
	header->MacTxControlHigh = 0;
	header->MacFrameControl = cpu_to_le16(frame[0] | (frame[1] << 8));

	uint8 preamble = mcs ? BRCMS_MM_PREAMBLE : BRCMS_LONG_PREAMBLE;
	set_frame_duration(header, frame, aggregated
		? brcms_glue_ba_duration(rateSpec)
		: brcms_glue_ack_duration(rateSpec, preamble));

	// Which modulation, which aerial, and -- in the extra word -- which
	// channel. The phy reads the channel from here, so a frame with it
	// unset goes out on whatever the phy was last left on.
	u16 phyControl = frametype(rateSpec, wlc->mimoft);
	phyControl |= brcms_c_stf_d11hdrs_phyctl_txant(wlc, rateSpec);
	header->PhyTxControlWord = cpu_to_le16(phyControl);

	u16 phyControl1 = brcms_c_phytxctl1_calc(wlc, rateSpec);
	header->PhyTxControlWord_1 = cpu_to_le16(phyControl1);
	header->PhyTxControlWord_1_Fbr = cpu_to_le16(phyControl1);

	u16 extra = frametype(rateSpec, wlc->mimoft);
	extra |= CHSPEC_CHANNEL(wlc_phy_chanspec_get(wlc->band->pi))
		<< XFTS_CHANNEL_SHIFT;
	header->XtraFrameTypes = cpu_to_le16(extra);


	memcpy(header->TxFrameRA, receiver, 6);
	// What the chip calls this frame back by. A frame on its own has only
	// ever needed the queue, which is what the one was; a frame in a run
	// needs telling apart from the others, so it carries its own sequence
	// number as well -- which sits in the high twelve bits of the two
	// bytes after the three addresses.
	//
	// Only for a run. Changing it for every frame was tried and the card
	// stopped carrying a network, and a frame sent on its own has no need
	// of it.
	if (aggregated) {
		u16 sequence = (frame[22] | (frame[23] << 8)) >> 4;

		header->TxFrameID = cpu_to_le16(
			((sequence << TXFID_SEQ_SHIFT) & TXFID_SEQ_MASK)
			| (TX_AC_BE_FIFO & TXFID_QUEUE_MASK));
	} else
		header->TxFrameID = cpu_to_le16(1);
	header->TxStatus = 0;

	// How long the frame is, said again in terms everything in the room
	// can read. An 802.11n frame whose legacy header says nothing is a
	// greenfield frame, which only another 802.11n station can make sense
	// of -- and an access point that cannot read a frame does not answer
	// it. Saying the length here is what makes it a mixed mode frame,
	// which is what brcmsmac sends unless the other end has agreed
	// otherwise.
	header->MModeLen = mcs
		? cpu_to_le16(brcms_c_calc_lsig_len(wlc, rateSpec, onAir)) : 0;
	header->MModeFbrLen = header->MModeLen;

	brcms_c_compute_plcp(wlc, rateSpec, onAir, plcp);
	// MainRates carries the PLCP rate encoding, not the internal ratespec.
	header->MainRates = cpu_to_le16(is_ofdm_rate(rateSpec)
		? D11A_PHY_HDR_GRATE((struct ofdm_phy_hdr*)plcp) : plcp[0]);
	memcpy(header->FragPLCPFallback, plcp, D11_PHY_HDR_LEN);

	// A network with older stations in it has stations that cannot read
	// what this card is about to send: they hear the air go quiet and
	// start talking over it. So the chip is asked to send a clear-to-send
	// first, addressed to this card itself, at a rate every one of them
	// can read, saying how long the air will be busy. They cannot read
	// the frame; they can read that.
	if (protect) {
		// Narrow and slow whatever the frame is: it is for the stations
		// that have no other way of hearing what is about to happen.
		u32 ctsRate = BRCM_RATE_1M | (PHY_TXC1_BW_20MHZ << RSPEC_BW_SHIFT);

		header->MacTxControlLow |= cpu_to_le16(TXC_SENDCTS);

		brcms_c_compute_plcp(wlc, ctsRate, DOT11_CTS_LEN + FCS_LEN,
			header->RTSPhyHeader);
		memcpy(header->RTSPLCPFallback, header->RTSPhyHeader,
			sizeof(header->RTSPLCPFallback));

		u16 held = brcms_c_compute_rtscts_dur(wlc, true, ctsRate, rateSpec,
			BRCMS_LONG_PREAMBLE, preamble, onAir, false);

		struct ieee80211_cts* cts = (struct ieee80211_cts*)
			&header->rts_frame;
		cts->frame_control = cpu_to_le16(FC_CONTROL_CTS);
		cts->duration = cpu_to_le16(held);
		memcpy(cts->ra, wlc_hw->etheraddr, 6);

		header->RTSDurFallback = cpu_to_le16(held);

		// The rate the clear-to-send goes at rides in the high half of
		// the same field the frame's own rate is in.
		header->MainRates |= cpu_to_le16(header->RTSPhyHeader[0] << 8);
	}

	// What this card tells the network about itself when it joins, as it
	// actually goes out. Everything else is a reading of what was meant;
	// this is what was said.
	//
	// The capabilities travel as elements in the body of the request, and
	// element forty five is the 802.11n one: two bytes of flags, then the
	// aggregation parameters, then sixteen bytes saying which schemes can
	// be received.
	uint16 subtype = (frame[0] | (frame[1] << 8)) & 0x00f0;
	if ((frame[0] & 0x0c) == 0 && (subtype == 0x0000 || subtype == 0x0020)
		&& written > 28) {
		const uint8* at = frame + 28;
		const uint8* end = frame + written;

		while (at + 2 <= end && at + 2 + at[1] <= end) {
			if (at[0] == 45 && at[1] >= 18) {
				TRACE("tx: joining with ht caps %02x%02x, ampdu %02x, "
					"mcs %02x %02x %02x %02x\n", at[3], at[2], at[4],
					at[5], at[6], at[7], at[8]);
				break;
			}

			at += 2 + at[1];
		}
	}

	// Say what is being sent while an association is being made. Once one
	// has been, the frames are data and there are far too many of them.
	uint16 frameControl = frame[0] | (frame[1] << 8);
	if ((frameControl & 0x000c) == 0 && (frameControl & 0x00f0) != 0x0040) {
		const uint8* to = frame + 4;
		TRACE("tx: %s to %02x:%02x:%02x:%02x:%02x:%02x\n",
			brcms_glue_frame_name(frameControl), to[0], to[1], to[2],
			to[3], to[4], to[5]);
	}

	return p;
}


static uint32 sHardwareTxFrames;
static struct sk_buff*
build_frame_hw(const void* outgoing, size_t length, uint8 rate, bool mcs,
    bool protect, bool wide, bool aggregated, uint32 token, uint8 keySlot)
{
    if (keySlot == 0)
        return build_frame(outgoing, length, rate, mcs, protect, wide, aggregated, token);
    struct brcms_ccmp_fields fields;
    if (!brcms_glue_tx_key_valid(keySlot)
        || !brcms_ccmp_prepare_fields(keySlot, outgoing, length, &fields))
        return NULL;
    uint8 padded[2048];
    memcpy(padded, outgoing, length);
    memset(padded + length, 0, 8);
    struct sk_buff* p = build_frame(padded, length + 8, rate, mcs, protect,
        wide, aggregated, token);
    if (p == NULL) return NULL;
    struct d11txh* txh = (struct d11txh*)p->data;
    txh->MacTxControlHigh |= cpu_to_le16(fields.security);
    // Inside an A-MPDU the microcode appends the hardware MIC only when
    // TXC_AMIC is set; without it every multi-MPDU aggregate stalls.
    if (aggregated)
        txh->MacTxControlLow |= cpu_to_le16(TXC_AMIC);
    memcpy(txh->IV, fields.iv, 8);
    __skb_trim(p, p->len - 8);
    packet_info(p)->keySlot = keySlot;
    sHardwareTxFrames++;
    return p;
}

// The four bytes that separate one frame of an aggregate from the next.
// The chip writes them; what it needs from here is them counted.
#define AMPDU_DELIMITER_LEN	4


/*!	Puts several frames on the air as one.

	An aggregate is several whole frames sent inside a single
	transmission: one preamble, one header, one stretch of air, and one
	acknowledgement covering all of them. The fixed cost of a frame on
	802.11n -- the gap before it, the gap after it, and the
	acknowledgement -- is larger than the time it takes to send fifteen
	hundred bytes at the schemes this card can manage, so a frame sent on
	its own spends more of the air on being a frame than on carrying
	anything.

	The chip builds the aggregate itself. What it is told is where each
	frame sits in the run, and -- in the first frame's legacy header --
	how long the whole run is, so that everything else in the room knows
	how long the air will be busy.
*/
status_t
brcms_glue_send_aggregate(const struct brcms_glue_frame* frames, uint count,
	uint8 rate, bool wide)
{
	if (count == 0 || count > BRCMS_AGGREGATE_MOST)
		return B_BAD_VALUE;


	struct sk_buff* built[BRCMS_AGGREGATE_MOST];
	uint made = 0;

	for (uint i = 0; i < count; i++) {
		built[made] = build_frame_hw(frames[i].bytes, frames[i].length, rate,
			true, false, wide, true, frames[i].token, frames[i].keySlot);
		if (built[made] == NULL)
			break;
		made++;
	}

	if (made != count) {
		for (uint i = 0; i < made; i++)
			brcmu_pkt_buf_free_skb(built[i]);
		return B_NO_MEMORY;
	}

	// How long the run is, in the units the legacy header counts in: each
	// frame rounded up to four bytes, plus the separator before it.
	uint total = 0;
	for (uint i = 0; i < made; i++) {
		struct d11txh* txh = (struct d11txh*)built[i]->data;
		uint one = BRCMS_GET_MIMO_PLCP_LEN(txh->FragPLCPFallback);
		total += roundup(one, 4) + AMPDU_DELIMITER_LEN;
	}

	// The last frame needs no padding after it, so take that back off.
	struct d11txh* last = (struct d11txh*)built[made - 1]->data;
	uint lastLength = BRCMS_GET_MIMO_PLCP_LEN(last->FragPLCPFallback);
	total -= roundup(lastLength, 4) - lastLength;

	u16 lastControl = le16_to_cpu(last->MacTxControlLow);
	lastControl &= ~TXC_AMPDU_MASK;
	lastControl |= (TXC_AMPDU_LAST << TXC_AMPDU_SHIFT);
	last->MacTxControlLow = cpu_to_le16(lastControl);

	// The first frame carries the whole run's length and says that a run
	// is what this is.
	struct d11txh* first = (struct d11txh*)built[0]->data;
	uint8* plcp = (uint8*)built[0]->data + D11_TXH_LEN;

	u16 firstControl = le16_to_cpu(first->MacTxControlLow);
	if (made > 1) {
		firstControl &= ~TXC_AMPDU_MASK;
		firstControl |= (TXC_AMPDU_FIRST << TXC_AMPDU_SHIFT);
	}
	firstControl |= TXC_STARTMSDU;
	first->MacTxControlLow = cpu_to_le16(firstControl);

	BRCMS_SET_MIMO_PLCP_LEN(plcp, total);
	BRCMS_SET_MIMO_PLCP_AMPDU(plcp);

	if (first->MModeLen != 0) {
		first->MModeLen = cpu_to_le16(brcms_c_calc_lsig_len(
			brcms_glue_common(), packet_info(built[0])->rateSpec, total));
		first->MModeFbrLen = first->MModeLen;
	}

	struct brcms_hardware* hardware = brcms_glue_hardware();
	// The ucode must have enough bytes ready before it starts a fast
	// aggregate. Keep 2 KiB of FIFO headroom, as the hardware requires.
	uint dmaBytes = 0;
	for (uint i = 0; i < made; i++)
		dmaBytes += built[i]->len;
	uint capacity = hardware->xmtfifo_sz != NULL
		? hardware->xmtfifo_sz[TX_AC_BE_FIFO] * TXFIFO_SIZE_UNIT : 0;
	if (capacity > 2048) {
		uint preload = capacity - 2048;
		if (preload > dmaBytes)
			preload = dmaBytes;
		first->PreloadSize = cpu_to_le16(preload);
	}
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sTxRingLock);
	int result = dma_tx_batch(hardware->di[TX_AC_BE_FIFO], built, made);
	release_spinlock(&sTxRingLock);
	restore_interrupts(state);
	if (result != 0) {
		// No descriptor was published; the batch API retains ownership.
		for (uint i = 0; i < made; i++)
			brcmu_pkt_buf_free_skb(built[i]);
		sSendFailures += made;
		return B_ERROR;
	}

	sFramesSent += made;
	sAggregatesSent++;
	sAggregatedFrames += made;
	return B_OK;
}


/*!	Puts one frame on the air, whoever built it. */
status_t
brcms_glue_send_frame(const void* outgoing, size_t outgoingLength,
	uint8 rate, bool mcs, bool protect, bool wide, uint32 token)
{
	struct sk_buff* p = build_frame(outgoing, outgoingLength, rate, mcs,
		protect, wide, false, token);
	if (p == NULL)
		return B_NO_MEMORY;

	int probe = probe_trace_begin(p, token);
	if (probe >= 0) {
		const struct d11txh* header = (const struct d11txh*)p->data;
		struct brcms_hardware* hardware = brcms_glue_hardware();
		TRACE("probe submit: t %lld slot %d token %lu built %u ctl %#x "
			"ucode %#x phy %#x\n", (long long)system_time(), probe,
			(unsigned long)token,
			(le16_to_cpu(header->XtraFrameTypes) >> XFTS_CHANNEL_SHIFT) & 0xff,
			le16_to_cpu(header->MacTxControlLow),
			brcms_b_read_shm(hardware, M_CURCHANNEL),
			wlc_phy_chanspec_get(hardware->band->pi));
	}
	int result = submit_frame(brcms_glue_common(), p);
	if (result != 0 && probe >= 0) {
		probe_trace_end(token);
		TRACE("probe submit failed: t %lld token %lu result %d\n",
			(long long)system_time(), (unsigned long)token, result);
	}
	// Imported DMA errors can be positive with Haiku errno definitions.
	if (result != 0) {
		sSendFailures++;
		return B_ERROR;
	}

	sFramesSent++;

	return B_OK;
}


status_t
brcms_glue_send_frame_hw(const void* outgoing, size_t outgoingLength,
	uint8 rate, bool mcs, bool protect, bool wide, uint32 token, uint8 keySlot)
{
	struct sk_buff* p = build_frame_hw(outgoing, outgoingLength, rate, mcs,
		protect, wide, false, token, keySlot);
	if (p == NULL)
		return B_NO_MEMORY;

	int probe = probe_trace_begin(p, token);
	if (probe >= 0) {
		const struct d11txh* header = (const struct d11txh*)p->data;
		struct brcms_hardware* hardware = brcms_glue_hardware();
		TRACE("probe submit: t %lld slot %d token %lu built %u ctl %#x "
			"ucode %#x phy %#x\n", (long long)system_time(), probe,
			(unsigned long)token,
			(le16_to_cpu(header->XtraFrameTypes) >> XFTS_CHANNEL_SHIFT) & 0xff,
			le16_to_cpu(header->MacTxControlLow),
			brcms_b_read_shm(hardware, M_CURCHANNEL),
			wlc_phy_chanspec_get(hardware->band->pi));
	}
	int result = submit_frame(brcms_glue_common(), p);
	if (result != 0 && probe >= 0) {
		probe_trace_end(token);
		TRACE("probe submit failed: t %lld token %lu result %d\n",
			(long long)system_time(), (unsigned long)token, result);
	}
	// Imported DMA errors can be positive with Haiku errno definitions.
	if (result != 0) {
		sSendFailures++;
		return B_ERROR;
	}

	sFramesSent++;

	return B_OK;
}


void
brcms_glue_tx_report(void)
{
    TRACE("TX CCMP hardware frames built %u\n", sHardwareTxFrames);
	if (sSuppressed != 0) {
		TRACE("tx: %" B_PRIu32 " the chip would not send\n", sSuppressed);
	}

	TRACE("tx: %" B_PRIu32 " sent, %" B_PRIu32 " refused, %" B_PRIu32
		" completed, %" B_PRIu32 " answered for, %" B_PRIu32 " not\n",
		sFramesSent, sSendFailures, sFramesCompleted, sFramesAcknowledged,
		sFramesUnanswered);
	TRACE("tx: %" B_PRIu32 " sent without an ACK request\n",
		sFramesCompleted - sFramesAcknowledged - sFramesUnanswered);
	TRACE("tx: %u deferred block-ACK statuses\n", sDeferredStatus);
	TRACE("tx: %u retry fallbacks; MCS7 %u MCS3 %u MCS0 %u\n",
        sRetryFallbacks, sRetryLowerRates[7], sRetryLowerRates[3], sRetryLowerRates[0]);
	TRACE("tx: %u aggregates, %u aggregate MPDUs, %u software retries\n",
		sAggregatesSent, sAggregatedFrames, sRetryTransmissions);
}


/* --- the queues mac80211 would be keeping, which there are none of ------

	brcms_c_txfifo() tells the stack above to stop sending when the ring
	fills. There is no stack above and one frame at a time goes out, so
	these say the queue is already stopped and nothing more is asked of
	them.
*/

u16
skb_get_queue_mapping(const struct sk_buff* skb)
{
	return 0;
}


bool
ieee80211_queue_stopped(struct ieee80211_hw* hw, u16 queue)
{
	return true;
}


void
ieee80211_stop_queue(struct ieee80211_hw* hw, u16 queue)
{
}


/* --- what the chip says about what it sent ---------------------------- */

/*!	Empties the transmit status queue and takes the buffers back.

	The chip reports every frame it has finished with, and holds the
	buffer until it is asked for. Upstream's brcms_b_txstatus() hands each
	report to mac80211's rate control; there is nothing here that wants
	one, but the queue still has to be emptied -- the card raises its
	interrupt while anything is in it -- and the buffer still has to come
	back, or a pool of sixty four runs out after sixty four frames.
*/
static bool sHasPendingStatus;
static u32 sPendingStatus, sPendingStatus2;

bool
brcms_glue_tx_status_pending(void)
{
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sTxRingLock);
	bool pending = sHasPendingStatus;
	release_spinlock(&sTxRingLock);
	restore_interrupts(state);
	return pending;
}



// Keep the existing retry budget. Later failed attempts use progressively
// more robust negotiated MCS rates, instead of repeating MCS7 to exhaustion.
static uint8
brcms_retry_target(const struct brcms_tx_packet_info* info, uint32 negotiatedMcs)
{
	uint8 current = info->rateSpec & RSPEC_RATE_MASK;
	if ((info->rateSpec & RSPEC_MIMORATE) == 0 || current > 15)
		return current;
	uint8 target;
	if (info->retries >= 4)
		target = 0;
	else if (info->retries >= 2)
		target = 3;
	else if (current >= 12)
		target = 7;
	else
		return current;
	return target < current && (negotiatedMcs & (1U << target)) != 0
		? target : current;
}


status_t
brcms_glue_retry_frame(void* opaque, uint32 negotiatedMcs)
{
	struct sk_buff* packet = opaque;
	struct brcms_tx_packet_info* info = packet_info(packet);
    struct sk_buff* original = packet;
    // Preserve the encrypted MPDU, sequence number and PN. Rebuild only
    // its hardware/PHY header at a lower negotiated MCS rate.
    uint8 target = brcms_retry_target(info, negotiatedMcs);
    if (target != (info->rateSpec & RSPEC_RATE_MASK)) {
        uint8 rate = target | ((info->rateSpec & RSPEC_SHORT_GI) != 0 ? BRCMS_TX_RATE_SGI : 0);
        bool wide = ((info->rateSpec & RSPEC_BW_MASK) >> RSPEC_BW_SHIFT) == PHY_TXC1_BW_40MHZ;
        packet = build_frame_hw(original->data + TXOFF, original->len - TXOFF,
            rate, true, false, wide, true, info->token, info->keySlot);
        if (packet == NULL) return B_NO_MEMORY;
        packet_info(packet)->retries = info->retries;
        info = packet_info(packet);
    }

	struct d11txh* header = (struct d11txh*)packet->data;
	uint8* plcp = packet->data + D11_TXH_LEN;
	uint8* frame = packet->data + TXOFF;
	struct brcms_c_info* wlc = brcms_glue_common();

	// Keep the retry in the BA reorder path: an individually ACKed
	// encrypted retry can arrive after a higher PN was already delivered.
	// A singleton A-MPDU retains sequence, ciphertext and packet number.
	frame[1] |= 0x08;
	header->MacFrameControl = cpu_to_le16(frame[0] | (frame[1] << 8));
	u16 control = le16_to_cpu(header->MacTxControlLow);
	control &= ~(TXC_AMPDU_MASK | TXC_HWSEQ);
	control |= TXC_STARTMSDU | (TXC_AMPDU_LAST << TXC_AMPDU_SHIFT);
	header->MacTxControlLow = cpu_to_le16(control);
	header->TxStatus = 0;
	header->PreloadSize = 0;
	memcpy(plcp, header->FragPLCPFallback, D11_PHY_HDR_LEN);
	uint aggregateLength = packet->len - TXOFF + (info->keySlot != 0 ? 8 : 0) + FRAME_CHECK_LENGTH
		+ AMPDU_DELIMITER_LEN;
	BRCMS_SET_MIMO_PLCP_LEN(plcp, aggregateLength);
	BRCMS_SET_MIMO_PLCP_AMPDU(plcp);
	header->MModeLen = cpu_to_le16(brcms_c_calc_lsig_len(wlc, info->rateSpec,
		aggregateLength));
	header->MModeFbrLen = header->MModeLen;
	set_frame_duration(header, frame,
		brcms_glue_ba_duration(info->rateSpec));
	info->retries++;

	cpu_status state = disable_interrupts();
	acquire_spinlock(&sTxRingLock);
	int result = dma_tx_batch(brcms_glue_hardware()->di[TX_AC_BE_FIFO],
		&packet, 1);
	release_spinlock(&sTxRingLock);
	restore_interrupts(state);
    if (packet != original) {
        if (result == 0) {
            sRetryFallbacks++;
            sRetryLowerRates[target]++;
            brcmu_pkt_buf_free_skb(original);
        } else
            brcmu_pkt_buf_free_skb(packet);
    }

	return result == 0 ? B_OK : B_ERROR;
}


static void
finish_packet(struct sk_buff* p, u32 status, bool acknowledged, uint retries,
	bool aggregate, uint16 phyError)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	struct brcms_tx_packet_info* info = packet_info(p);
	uint32 token = info->token;
	struct d11txh* txh = (struct d11txh*)p->data;
	uint sentOn = (le16_to_cpu(txh->XtraFrameTypes)
		>> XFTS_CHANNEL_SHIFT) & 0xff;
	uint builtWith = le16_to_cpu(txh->MacTxControlLow);
	if (p->len >= TXOFF + 24 && (p->data[TXOFF] & 0xfc) == 0x40
		&& probe_trace_end(token)) {
		TRACE("probe complete: t %lld token %lu status %#lx ack %u "
			"attempts %u built %u ctl %#x ucode %#x phy %#x\n",
			(long long)system_time(), (unsigned long)token,
			(unsigned long)status, acknowledged, retries, sentOn, builtWith,
			brcms_b_read_shm(wlc_hw, M_CURCHANNEL),
			wlc_phy_chanspec_get(wlc_hw->band->pi));
        probe_descriptor_report(p, token);
	}
	// Authentication must remain visible after the scan's suppression
	// log has reached its cap. Record hardware metadata, never payload.
	static uint32 authSamples;
	if (p->len >= TXOFF + 24 && p->data[TXOFF] == 0xb0
		&& authSamples++ < 64) {
		TRACE("auth tx: status %#lx phyerr %#x ack %u attempts %u "
			"channel %u ctl %#x phy %#x/%#x current %#x "
			"mac %#lx ioctl %#lx depth %u\n",
			(unsigned long)status, phyError, acknowledged, retries,
			sentOn, builtWith, le16_to_cpu(txh->PhyTxControlWord),
			le16_to_cpu(txh->PhyTxControlWord_1),
			brcms_b_read_shm(wlc_hw, M_CURCHANNEL),
			(unsigned long)bcma_read32(wlc_hw->d11core,
				D11REGOFFS(maccontrol)),
			(unsigned long)bcma_aread32(wlc_hw->d11core, BCMA_IOCTL),
			wlc_hw->mac_suspend_depth);
	}
	// Whether the chip sent it at all. A frame can come back off
	// the ring complete and never have been on the air: the MAC
	// suppresses what it will not send, and says why. The commonest
	// why is a frame whose header names a channel the MAC is not
	// on, which is what every frame looks like if the two were told
	// about a change of band separately.
	uint suppressed = (status & TX_STATUS_SUPR_MASK)
		>> TX_STATUS_SUPR_SHIFT;
	if (suppressed != 0 && sSuppressedSeen < 40) {
		struct brcms_c_info* wlc = brcms_glue_common();

		sSuppressedSeen++;

		// Reason four is a channel mismatch. The comparison the
		// microcode makes is between the channel written into
		// this frame's own header when it was built and the one
		// it is on now, so say both of those -- not the channel
		// the phy happens to be on by the time this is read,
		// which during a scan has moved on several times.
		TRACE("tx: the chip would not send it, reason %u; frame "
			"built for %u control %#x, microcode on %#x, "
			"band %u chanspec %#x\n",
			suppressed, sentOn, builtWith,
			brcms_b_read_shm(wlc_hw, M_CURCHANNEL),
			(unsigned)wlc->band->bandtype,
			wlc_phy_chanspec_get(wlc->band->pi));
	}
	if (suppressed != 0)
		sSuppressed++;

	if (!acknowledged && (aggregate || info->retries != 0)
		&& info->retries < 5
		&& (status & TX_STATUS_SUPR_MASK) != TX_STATUS_SUPR_BADCH
		&& (status & TX_STATUS_SUPR_MASK) != TX_STATUS_SUPR_EXPTIME
		&& brcms_glue_report_retry(token, p)) {
		sRetryTransmissions++;
		return;
	}

    const uint8* failedFrame = p->data + TXOFF;
    static int32 sFinalDataFailures;
    if (!acknowledged && (failedFrame[0] & 0x0c) == 8
        && (failedFrame[4] & 1) == 0
        && atomic_add(&sFinalDataFailures, 1) < 512) {
        TRACE("data TX failure: token %u status %#lx suppression %u phy %#x software-retries %u seq %u rate %#lx\n",
            token, (unsigned long)status, suppressed, phyError, info->retries,
            (failedFrame[22] | (failedFrame[23] << 8)) >> 4,
            (unsigned long)info->rateSpec);
    }
	// Broadcast and other no-ACK transmissions cannot receive an ACK.
	// A valid, unsuppressed completion without a PHY error succeeds.
	bool noAckSent = !aggregate && (builtWith & TXC_IMMEDACK) == 0
		&& (status & TXS_V) != 0 && suppressed == 0 && phyError == 0;
	retries += info->retries;
	brcmu_pkt_buf_free_skb(p);
	sFramesCompleted++;
	if (acknowledged)
		sFramesAcknowledged++;
	else if (!noAckSent)
		sFramesUnanswered++;
	brcms_glue_report_sent(token, acknowledged || noAckSent, retries);
}


void
brcms_glue_reclaim_sent(void)
{
	struct brcms_hardware* hardware = brcms_glue_hardware();
	struct bcma_device* core = hardware->d11core;
	if (core == NULL || !hardware->clk)
		return;
	if (atomic_test_and_set(&sReclaiming, 1, 0) != 0)
		return;

	for (int n = 0; n < 32; n++) {
		struct sk_buff* packets[BRCMS_AGGREGATE_MOST];
		uint count = 0;
		u32 status, status2;
		uint64 bitmap = 0;
		bool haveBA = false;
		cpu_status state = disable_interrupts();
		acquire_spinlock(&sTxRingLock);
		if (sHasPendingStatus) {
			status = sPendingStatus;
			status2 = sPendingStatus2;
			sHasPendingStatus = false;
		} else {
			status = bcma_read32(core, D11REGOFFS(frmtxstatus));
			if (status == 0xffffffff || (status & TXS_V) == 0) {
				release_spinlock(&sTxRingLock);
				restore_interrupts(state);
				break;
			}
			status2 = bcma_read32(core, D11REGOFFS(frmtxstatus2));
		}
		bool aggregate = (status & TX_STATUS_AMPDU) != 0;
		if (!aggregate && (status & TX_STATUS_INTERMEDIATE) != 0) {
			release_spinlock(&sTxRingLock);
			restore_interrupts(state);
			continue;
		}
		if (aggregate && (status & TX_STATUS_ACK_RCV) != 0) {
			u32 second = bcma_read32(core, D11REGOFFS(frmtxstatus));
			for (uint delay = 0; (second & TXS_V) == 0 && delay < 10;
				delay++) {
				udelay(1);
				second = bcma_read32(core, D11REGOFFS(frmtxstatus));
			}
			if (second == 0xffffffff || (second & TXS_V) == 0) {
				// Keep the first half until the second status arrives.
				sDeferredStatus++;
				sHasPendingStatus = true;
				sPendingStatus = status;
				sPendingStatus2 = status2;
				release_spinlock(&sTxRingLock);
				restore_interrupts(state);
				break;
			}
			u32 second2 = bcma_read32(core, D11REGOFFS(frmtxstatus2));
			haveBA = (second & (TX_STATUS_AMPDU | TX_STATUS_INTERMEDIATE))
				== TX_STATUS_AMPDU;
			if (haveBA) {
				bitmap = ((status & TX_STATUS_BA_BMAP03_MASK)
					>> TX_STATUS_BA_BMAP03_SHIFT)
					| ((second & TX_STATUS_BA_BMAP47_MASK)
						<< TX_STATUS_BA_BMAP47_SHIFT)
					| (second & 0xffffff00)
					| ((uint64)second2 << 32);
			} else {
				// Do not mistake another packet's final status for a BA.
				sHasPendingStatus = true;
				sPendingStatus = second;
				sPendingStatus2 = second2;
			}
		}
		while (count < BRCMS_AGGREGATE_MOST) {
			struct sk_buff* packet = dma_getnexttxp(
				hardware->di[TX_AC_BE_FIFO], DMA_RANGE_TRANSMITTED);
			if (packet == NULL)
				break;
			packets[count++] = packet;
			struct d11txh* header = (struct d11txh*)packet->data;
			u16 type = le16_to_cpu(header->MacTxControlLow) & TXC_AMPDU_MASK;
			if (!aggregate || type == 0
				|| type == (TXC_AMPDU_LAST << TXC_AMPDU_SHIFT))
				break;
		}
		release_spinlock(&sTxRingLock);
		restore_interrupts(state);

		for (uint i = 0; i < count; i++) {
			bool acknowledged = (status & TX_STATUS_ACK_RCV) != 0;
			uint retries = (status & TX_STATUS_FRM_RTX_MASK)
				>> TX_STATUS_FRM_RTX_SHIFT;
			if (aggregate) {
				const uint8* frame = packets[i]->data + TXOFF;
				uint sequence = (frame[22] | (frame[23] << 8)) >> 4;
				uint start = (status2 & TXS_SEQ_MASK) >> 4;
				uint bit = (sequence - start) & 0xfff;
				acknowledged = haveBA && bit < 64 && ((bitmap >> bit) & 1);
				retries = 1;
			}
			finish_packet(packets[i], status, acknowledged, retries, aggregate,
				(status2 & TXS_PTX_MASK) >> TXS_PTX_SHIFT);
		}
	}
	atomic_set(&sReclaiming, 0);
}
