/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	What happens to a frame once the card has one.

	brcmsmac hands every received frame to brcms_c_recv(), which upstream
	strips the chip's own headers, works out the rate and the signal
	strength and passes the rest to mac80211. There is no mac80211 here and
	no network stack bound to this driver yet, so this is where a frame
	stops: what it was, how strong it was, and -- for a beacon -- which
	network sent it.

	What arrives is not the frame on its own. The layout, which is what
	upstream's brcms_c_recv() and brcms_c_recvctl() step through together:

		38 bytes  the chip's own receive header
		 2 bytes  padding, only when RXS_PBPRES says so
		 6 bytes  the phy's header
		    ...   the 802.11 frame
		 4 bytes  the frame check sequence

	Every count below is kept separately, because a frame that is thrown
	away for a reason is not the same as a frame that never came.
*/


#include "linux/types.h"

#include <KernelExport.h>

#include "net/mac80211.h"
#include "linux/ieee80211.h"
#include "linux/skbuff.h"
#include "d11.h"
#include "debug_haiku.h"
#include "glue.h"
#include "glue_net.h"
#include "main.h"
#include "ampdu.h"
#include "pub.h"
#include "types.h"


#define TRACE(x...)	dprintf("broadcom43xxn: " x)

// How much of the chip's own header sits in front of a frame, and how much
// sits behind it.
#define HARDWARE_RX_OFFSET		38
#define FRAME_CHECK_LENGTH		4

// 802.11 frame control, and the two frame types worth naming here.
#define FC_TYPE_MASK			0x000c
#define FC_SUBTYPE_MASK			0x00f0
#define FC_SUBTYPE_SHIFT		4
#define FC_TYPE_MANAGEMENT		0x0000
#define FC_TYPE_DATA_FRAME		0x0008
#define FC_SUBTYPE_BEACON		8
#define FC_SUBTYPE_PROBE_RESP	5

// The first element of a beacon's body, after the fixed twelve bytes of
// timestamp, interval and capability.
#define BEACON_FIXED_LENGTH		12
#define ELEMENT_SSID			0

// Each network sends a beacon about ten times a second, so saying so every
// time would bury the rest of the report. Only the first beacon from a
// given sender is reported, and only so many senders: the point is to show
// that frames are arriving, not to be a scanner.
#define MAX_NETWORKS			8

// What this driver calls quiet. The chip does not measure the noise, and
// net80211 wants a signal reported as a height above something, so this is
// that something: a little below the weakest frame worth having.
#define NOISE_FLOOR				-96


static uint32 sHardwareDecryptFrames;
static uint32 sGroupDecrypted, sGroupRejected;
static uint32 sFramesSeen;
static uint32 sRunts;
static uint32 sCheckFailures;
static uint32 sAggregates;
static uint32 sManagement;
static uint32 sManagementNamed;

// What the air is actually carrying, for frames meant for this card. A
// download that crawls is either an access point choosing a slow rate or
// one sending everything twice, and the two look the same from outside.
static uint32 sByKind[4];		// CCK, OFDM, 802.11n draft, 802.11n
static uint32 sAirByKind[4];	// the same, for everything in the room
static uint32 sFirstData;		// the opening frames, named one by one
static uint32 sOthersNamed;		// and a few of what is said to others
static uint32 sAnswerNamed;		// and what the access point answered with
static uint32 sByMcs[33];		// 0-31, and 32 for anything else
static int64 sHtMacBytes;
static int64 sHtHeaderBytes[2][2];
static int64 sHtHeaderCount[2][2];
static int64 sHtHeaderMcsBytes[16];
static uint32 sRetried;
static uint32 sWide;
static uint32 sPollRounds;
static uint32 sPollFrames;
static uint32 sAnswers;
static uint32 sBeaconsSeen;
static uint32 sNetworksNamed;
static bool sDumpedOne;

static uint8 sSeen[MAX_NETWORKS][6];
static uint32 sSeenCount;


/*!	What a management frame is called, for the log.

	An association is a conversation of these, in a fixed order, and when
	one does not happen the useful thing to know is which was the last that
	did. The subtypes are the ones 802.11 numbers them by.
*/
const char*
brcms_glue_frame_name(uint16 frameControl)
{
	static const char* const kManagement[16] = {
		"assoc request", "assoc response", "reassoc request",
		"reassoc response", "probe request", "probe response", "timing",
		"?", "beacon", "atim", "disassociate", "authenticate",
		"deauthenticate", "action", "?", "?"
	};

	uint16 type = frameControl & FC_TYPE_MASK;
	uint16 subtype = (frameControl & FC_SUBTYPE_MASK) >> FC_SUBTYPE_SHIFT;

	if (type == FC_TYPE_MANAGEMENT)
		return kManagement[subtype];
	if (type == 0x0008)
		return "data";

	return "control";
}


static bool
already_reported(const uint8* address)
{
	for (uint32 i = 0; i < sSeenCount; i++) {
		if (memcmp(sSeen[i], address, 6) == 0)
			return true;
	}

	if (sSeenCount < MAX_NETWORKS) {
		memcpy(sSeen[sSeenCount], address, 6);
		sSeenCount++;
		return false;
	}

	// Past the limit nothing more is named, but the counting goes on.
	return true;
}


/*!	Whether this is a network announcing itself. */
static bool
is_beacon(uint16 frameControl)
{
	if ((frameControl & FC_TYPE_MASK) != FC_TYPE_MANAGEMENT)
		return false;

	uint16 subtype = (frameControl & FC_SUBTYPE_MASK) >> FC_SUBTYPE_SHIFT;

	return subtype == FC_SUBTYPE_BEACON || subtype == FC_SUBTYPE_PROBE_RESP;
}


/*!	Copies the network name out of a beacon body, if it is there.

	The body is a run of elements, each a one byte identifier, a one byte
	length and that many bytes. The name is the first of them, and may be
	empty for a network that hides it.
*/
static bool
beacon_name(const uint8* body, size_t length, char* out, size_t outSize)
{
	if (length <= BEACON_FIXED_LENGTH)
		return false;

	const uint8* at = body + BEACON_FIXED_LENGTH;
	const uint8* end = body + length;

	while (at + 2 <= end) {
		uint8 identifier = at[0];
		uint8 elementLength = at[1];
		if (at + 2 + elementLength > end)
			return false;

		if (identifier == ELEMENT_SSID) {
			size_t copy = elementLength;
			if (copy >= outSize)
				copy = outSize - 1;

			// A name is not a C string on the air and need not be
			// printable, so anything that is not is shown as a dot.
			for (size_t i = 0; i < copy; i++)
				out[i] = (at[2 + i] >= 0x20 && at[2 + i] < 0x7f)
					? (char)at[2 + i] : '.';
			out[copy] = '\0';

			return true;
		}

		at += 2 + elementLength;
	}

	return false;
}


/*!	The first frame, byte for byte, in case the layout is not what is
	assumed above. Said once: it is for reading, not for counting.
*/
static void
dump_first_frame(const struct d11rxhdr* header, const uint8* at, size_t length)
{
	if (sDumpedOne)
		return;

	sDumpedOne = true;

	TRACE("rx: first frame, %u bytes, RxFrameSize %u, RxStatus1 %#x, "
		"RxStatus2 %#x\n", (unsigned)length, header->RxFrameSize,
		header->RxStatus1, header->RxStatus2);

	char line[3 * 16 + 1];
	size_t show = length < 16 ? length : 16;
	for (size_t i = 0; i < show; i++)
		snprintf(line + i * 3, 4, "%02x ", at[i]);

	TRACE("rx: first frame starts %s\n", line);
}


static inline uint16
frameControlOf(const uint8* frame)
{
	return frame[0] | (frame[1] << 8);
}


static bool
brcms_rx_header_valid(const uint8* frame, size_t length)
{
	// A compressed Block Ack Request has a 20-byte header. net80211
	// needs it to advance the receive reorder window past missing MPDUs.
	if (length >= 24)
		return true;
	return length >= 20 && frame[0] == 0x84;
}


void
brcms_c_recv(struct brcms_c_info* wlc, struct sk_buff* p)
{
	sFramesSeen++;

	const struct d11rxhdr* header = (const struct d11rxhdr*)p->data;

	size_t offset = HARDWARE_RX_OFFSET;
	if ((header->RxStatus1 & RXS_PBPRES) != 0)
		offset += 2;

	offset += D11_PHY_HDR_LEN;

	if (p->len < offset + FRAME_CHECK_LENGTH + 20) {
		sRunts++;
		brcmu_pkt_buf_free_skb(p);
		return;
	}

	const uint8* frame = (const uint8*)p->data + offset;
	size_t frameLength = p->len - offset - FRAME_CHECK_LENGTH;
	if (!brcms_rx_header_valid(frame, frameLength)) {
		sRunts++;
		brcmu_pkt_buf_free_skb(p);
		return;
	}

	dump_first_frame(header, frame, frameLength);

	// A frame the chip says did not check out is not worth reading, but it
	// is worth knowing how many there were: a receiver that is tuned but
	// deaf and one that is picking up noise look the same from the counts
	// alone.
	if ((header->RxStatus1 & RXS_FCSERR) != 0) {
		sCheckFailures++;
		brcmu_pkt_buf_free_skb(p);
		return;
	}

	// A failed hardware attempt may have modified the payload: never
	// feed it to software decryption. Only our pairwise CCMP slot is used.
	// Slots 1..3 are group keys, 4..53 pairwise keys.
	unsigned keySlot = (header->RxStatus1 & RXS_SECKINDX_MASK)
		>> RXS_SECKINDX_SHIFT;
	bool attempted = (header->RxStatus1 & RXS_DECATMPT) != 0;
	if ((header->RxStatus1 & RXS_DECERR) != 0
		|| (attempted && keySlot >= 54)
		|| (attempted && keySlot < 4 && !brcms_glue_group_key_active(keySlot))) {
		if (attempted && keySlot < 4)
			sGroupRejected++;
		brcmu_pkt_buf_free_skb(p);
		return;
	}
	if (attempted && keySlot < 4)
		sGroupDecrypted++;

	// Aggregated frames carry several inside one and are not unpacked here.
	if ((header->RxStatus2 & RXS_AMSDU_MASK) != 0) {
		sAggregates++;
		brcmu_pkt_buf_free_skb(p);
		return;
	}

	// How loudly it arrived, which is what the stack above ranks one
	// network against another by. The phy works it out from the status
	// words the chip put in front of the frame; it is in dBm, and what
	// takes it wants it as a height above the noise.
	int rssi = wlc_phy_rssi_compute(brcms_glue_hardware()->band->pi,
		(struct d11rxhdr*)p->data);

	if (rssi < NOISE_FLOOR)
		rssi = NOISE_FLOOR;
	if (rssi > 0)
		rssi = 0;

	// Which channel this frame was heard on, which the chip wrote into the
	// receive header. Not the channel the radio is on now: a scan retunes
	// several times a second and the ring holds what was heard before the
	// last one. The stack above throws away a beacon that disagrees with
	// the channel it believes it was listening on, so it has to be told
	// per frame rather than asked afterwards.
	uint32 channel = BRCMS_CHAN_CHANNEL(header->RxChan);

	// How this one arrived, counted only for frames meant for this card:
	// the room's beacons go out at the slowest rate there is and would
	// drown the answer.
	// The other half of the conversation: what the access point says back
	// when this card joins, and what it tells the room about the room.
	// Element forty five is the 802.11n capabilities, sixty one is how
	// the network is being run -- which half of a wide channel is the
	// control one, whether anything in the room cannot read 802.11n, and
	// whether everyone is therefore being protected from it.
	{
		uint16 fc = frameControlOf(frame);
		uint16 sub = fc & 0x00f0;

		if ((fc & FC_TYPE_MASK) == FC_TYPE_MANAGEMENT
			&& (sub == 0x0010 || sub == 0x0030) && sAnswerNamed < 4) {
			// Past the fixed fields of an association response.
			const uint8* at = frame + 24 + 6;
			const uint8* end = frame + frameLength;

			sAnswerNamed++;

			while (at + 2 <= end && at + 2 + at[1] <= end) {
				if (at[0] == 45 && at[1] >= 18) {
					TRACE("rx: it answers ht caps %02x%02x, ampdu %02x, "
						"mcs %02x %02x\n", at[3], at[2], at[4], at[5],
						at[6]);
				} else if (at[0] == 61 && at[1] >= 5) {
					TRACE("rx: it runs the network as %02x %02x %02x "
						"%02x %02x\n", at[2], at[3], at[4], at[5],
						at[6]);
				}

				at += 2 + at[1];
			}
		}
	}

	// The same for a frame meant for somebody else, which says what this
	// access point is prepared to send a station that is not this one --
	// and whether this card can read it when it does.
	if (memcmp(frame + 4, brcms_glue_hardware()->etheraddr, 6) != 0
		&& (frame[1] & 0x02) != 0 && sOthersNamed < 16) {
		uint16 kind = header->PhyRxStatus_0 & PRXS0_FT_MASK;
		const uint8* plcp = (const uint8*)p->data + offset
			- D11_PHY_HDR_LEN;
		sOthersNamed++;
		TRACE("rx: to someone else, kind %u, plcp %02x %02x %02x, "
			"%d dBm\n", kind, plcp[0], plcp[1], plcp[2], rssi);
	}

	// What the room is carrying, whoever it is for. If the air is full of
	// 802.11n and only what comes here is not, the difference is this
	// card; if the air is 802.11b throughout, it is not.
	sAirByKind[header->PhyRxStatus_0 & PRXS0_FT_MASK]++;

	if (memcmp(frame + 4, brcms_glue_hardware()->etheraddr, 6) == 0) {
		uint16 kind = header->PhyRxStatus_0 & PRXS0_FT_MASK;
		sByKind[kind]++;

		// A first MPDU may contain PLCP without PHYRXST_VALID. Count
		// nonzero headers separately from all HT MPDU bytes, so the totals
		// can be cross-checked before treating them as PPDU rate statistics.
		if ((kind == PRXS0_PREN || kind == PRXS0_STDN)
			&& (frameControlOf(frame) & FC_TYPE_MASK) == FC_TYPE_DATA_FRAME) {
			atomic_add64(&sHtMacBytes, frameLength + FRAME_CHECK_LENGTH);
			const uint8* ht = (const uint8*)p->data + offset - D11_PHY_HDR_LEN;
			uint length = BRCMS_GET_MIMO_PLCP_LEN(ht);
			uint mcs = ht[0] & MIMO_PLCP_MCS_MASK;
			if (length >= frameLength + FRAME_CHECK_LENGTH && mcs < 16) {
				uint wide = (ht[0] & MIMO_PLCP_40MHZ) != 0;
				uint shortGi = (ht[3] & 0x80) != 0;
				atomic_add64(&sHtHeaderBytes[wide][shortGi], length);
				atomic_add64(&sHtHeaderCount[wide][shortGi], 1);
				atomic_add64(&sHtHeaderMcsBytes[mcs], length);
			}
		}

		// The first MPDU can carry PLCP without PHYRXST_VALID; the last
		// can carry PHY status with a zero continuation PLCP. Sample both.
		if ((frameControlOf(frame) & FC_TYPE_MASK) == FC_TYPE_DATA_FRAME
			&& frameLength >= 1000) {
			const uint8* plcp = (const uint8*)p->data + offset
				- D11_PHY_HDR_LEN;
			static uint32 sampledBulk;
			uint32 sample = sampledBulk++;
			if (sample < 48 || (sample < 524288 && sample % 8192 == 0)) {
				TRACE("rx context %u: len %u status %04x/%04x "
					"phy %04x/%04x/%04x plcp %02x %02x %02x %02x %02x %02x "
					"rssi %d chan %04x\n", sample, (uint)frameLength,
					header->RxStatus1, header->RxStatus2,
					header->PhyRxStatus_0, header->PhyRxStatus_1,
					header->PhyRxStatus_2, plcp[0], plcp[1], plcp[2],
					plcp[3], plcp[4], plcp[5], rssi, header->RxChan);
			}
		}

		// The first data frames of a conversation, one line each. An
		// access point that opens quickly and works its way down is a
		// different problem from one that never tries: the first says
		// this card is failing to answer what it is sent, the second
		// says it asked for 802.11b in the first place.
		if ((frameControlOf(frame) & FC_TYPE_MASK) == FC_TYPE_DATA_FRAME
			&& sFirstData < 20) {
			const uint8* plcp = (const uint8*)p->data + offset
				- D11_PHY_HDR_LEN;
			sFirstData++;
			TRACE("rx: data %u arrived as kind %u, plcp %02x %02x "
				"%02x, %d dBm\n", sFirstData, kind, plcp[0],
				plcp[1], plcp[2], rssi);
		}

		if ((kind == PRXS0_PREN || kind == PRXS0_STDN)
			&& (header->RxStatus2 & RXS_PHYRXST_VALID) != 0) {
			const uint8* plcp = (const uint8*)p->data + offset
				- D11_PHY_HDR_LEN;
			uint8 mcs = plcp[0] & ~MIMO_PLCP_40MHZ;
			if (BRCMS_GET_MIMO_PLCP_LEN(plcp) != 0) {
			sByMcs[mcs < 32 ? mcs : 32]++;
			if ((plcp[0] & MIMO_PLCP_40MHZ) != 0)
				sWide++;
			}
		}

		// The sender saying it has sent this one before.
		if ((frame[1] & 0x08) != 0)
			sRetried++;
	}

	if ((header->RxStatus1 & RXS_DECATMPT) != 0) sHardwareDecryptFrames++;
	sPollFrames++;

	// The PHY returns zero when this MPDU has no valid signal report.
	// net80211's RSSI filter ignores values below -20. Do not turn an
	// absent report into a 95 dB SNR and overwrite the AP's real signal.
	int signal = rssi == 0 ? -128 : rssi - NOISE_FLOOR;
	brcms_glue_deliver(frame, frameLength, signal, channel, header->RxStatus1);

	uint16 frameControl = frame[0] | (frame[1] << 8);
	if ((frameControl & FC_TYPE_MASK) == FC_TYPE_MANAGEMENT)
		sManagement++;

	// Every management frame that is not a beacon or a probe response,
	// which between them are almost all of them and say nothing about
	// whether this card is getting anywhere.
	// The first handful only. These are what an association is made of and
	// they are worth seeing; after that they are the neighbours probing,
	// and a line written to the system log for each one is work done in the
	// path that is trying to keep up with the air.
	if ((frameControl & FC_TYPE_MASK) == FC_TYPE_MANAGEMENT
		&& !is_beacon(frameControl) && sManagementNamed < 16) {
		const uint8* from = frame + 10;
		sManagementNamed++;
		TRACE("rx: %s from %02x:%02x:%02x:%02x:%02x:%02x, %d dBm\n",
			brcms_glue_frame_name(frameControl), from[0], from[1], from[2],
			from[3], from[4], from[5], rssi);
	}

	// A frame addressed to this card rather than to everybody, which at
	// this point can only be an answer to what this card sent. It is the
	// one thing that says the transmitter works.
	const uint8* to = frame + 4;
	if (memcmp(to, brcms_glue_hardware()->etheraddr, 6) == 0) {
		if (sAnswers == 0) {
			const uint8* from = frame + 10;
			TRACE("rx: answered by %02x:%02x:%02x:%02x:%02x:%02x\n",
				from[0], from[1], from[2], from[3], from[4], from[5]);
		}

		sAnswers++;
	}

	if (is_beacon(frameControl)) {
		sBeaconsSeen++;

		// The sender is the second of the three addresses in a management
		// frame, at a fixed place.
		const uint8* from = frame + 10;

		if (already_reported(from)) {
			brcmu_pkt_buf_free_skb(p);
			return;
		}

		char name[33];
		if (beacon_name(frame + 24, frameLength - 24, name, sizeof(name))) {
			sNetworksNamed++;
			TRACE("rx: %02x:%02x:%02x:%02x:%02x:%02x %d dBm ch %u \"%s\"\n",
				from[0], from[1], from[2], from[3], from[4], from[5], rssi,
				(unsigned)channel, name);
		} else {
			TRACE("rx: %02x:%02x:%02x:%02x:%02x:%02x unnamed, %u bytes\n",
				from[0], from[1], from[2], from[3], from[4], from[5],
				(unsigned)frameLength);
		}
	}

	brcmu_pkt_buf_free_skb(p);
}


void
brcms_glue_rx_report(void)
{
	TRACE("RX CCMP hardware plaintext frames %u; hardware MIC; software PN and slot required\n", sHardwareDecryptFrames);
	TRACE("RX CCMP hardware group frames %u, rejected %u\n", sGroupDecrypted, sGroupRejected);
	TRACE("rx HT MAC bytes %llu\n", (unsigned long long)atomic_get64(&sHtMacBytes));
	for (uint wide = 0; wide < 2; wide++) {
		for (uint gi = 0; gi < 2; gi++) {
			TRACE("rx HT PLCP width %u SGI %u headers %llu bytes %llu\n",
				wide ? 40 : 20, gi,
				(unsigned long long)atomic_get64(&sHtHeaderCount[wide][gi]),
				(unsigned long long)atomic_get64(&sHtHeaderBytes[wide][gi]));
		}
	}
	for (uint mcs = 0; mcs < 16; mcs += 4) {
		TRACE("rx HT PLCP MCS %u..%u bytes %llu/%llu/%llu/%llu\n", mcs, mcs + 3,
			(unsigned long long)atomic_get64(&sHtHeaderMcsBytes[mcs]),
			(unsigned long long)atomic_get64(&sHtHeaderMcsBytes[mcs + 1]),
			(unsigned long long)atomic_get64(&sHtHeaderMcsBytes[mcs + 2]),
			(unsigned long long)atomic_get64(&sHtHeaderMcsBytes[mcs + 3]));
	}
	TRACE("rx: %" B_PRIu32 " frames, %" B_PRIu32 " runt, %" B_PRIu32
		" bad check, %" B_PRIu32 " aggregated\n", sFramesSeen, sRunts,
		sCheckFailures, sAggregates);
	TRACE("rx: %" B_PRIu32 " management, %" B_PRIu32 " beacons, %" B_PRIu32
		" networks (%" B_PRIu32 " named), %" B_PRIu32 " addressed here\n",
		sManagement, sBeaconsSeen, sSeenCount, sNetworksNamed, sAnswers);

	TRACE("rx: to us by kind cck %" B_PRIu32 " ofdm %" B_PRIu32 " n %"
		B_PRIu32 "/%" B_PRIu32 ", %" B_PRIu32 " wide, %" B_PRIu32
		" repeated\n", sByKind[PRXS0_CCK], sByKind[PRXS0_OFDM],
		sByKind[PRXS0_PREN], sByKind[PRXS0_STDN], sWide, sRetried);

	// The three most used schemes, which is what says whether the far end
	// thinks this card is worth talking to quickly.
	uint32 best[3] = { 0, 0, 0 };
	int which[3] = { -1, -1, -1 };
	for (int i = 0; i <= 32; i++) {
		for (int slot = 0; slot < 3; slot++) {
			if (sByMcs[i] > best[slot]) {
				for (int move = 2; move > slot; move--) {
					best[move] = best[move - 1];
					which[move] = which[move - 1];
				}
				best[slot] = sByMcs[i];
				which[slot] = i;
				break;
			}
		}
	}

	TRACE("rx: most used mcs %d (%" B_PRIu32 "), %d (%" B_PRIu32 "), %d (%"
		B_PRIu32 ")\n", which[0], best[0], which[1], best[1], which[2],
		best[2]);

	TRACE("rx: the room by kind cck %" B_PRIu32 " ofdm %" B_PRIu32 " n %"
		B_PRIu32 "/%" B_PRIu32 "\n", sAirByKind[PRXS0_CCK],
		sAirByKind[PRXS0_OFDM], sAirByKind[PRXS0_PREN],
		sAirByKind[PRXS0_STDN]);

	TRACE("rx: %" B_PRIu32 " polls, %" B_PRIu32 " frames taken, %" B_PRIu32
		" per poll\n", sPollRounds, sPollFrames,
		sPollRounds != 0 ? sPollFrames / sPollRounds : 0);
}


void
brcms_glue_rx_polled(void)
{
	sPollRounds++;
}


void
brcms_glue_rx_reset(void)
{
	sFramesSeen = 0;
	sRunts = 0;
	sCheckFailures = 0;
	sAggregates = 0;
	sManagement = 0;
	sBeaconsSeen = 0;
	sNetworksNamed = 0;
	sSeenCount = 0;
	sAnswers = 0;
	sDumpedOne = false;
}


/* --- the parts of brcmsmac this driver does not carry --------------------

	Frame aggregation, the transmit status path, the antenna selector and
	the beacon the driver would send as an access point. None of them is on
	the way to receiving a frame, and all of them are reached from code that
	is imported whole, so they are answered here rather than cut out of it.
*/

void
brcms_c_ampdu_reset_session(struct brcms_ampdu_session* session,
	struct brcms_c_info* wlc)
{
	memset(session, 0, sizeof(*session));
	session->wlc = wlc;
	skb_queue_head_init(&session->skb_list);
}


int
brcms_c_ampdu_add_frame(struct brcms_ampdu_session* session,
	struct sk_buff* p)
{
	return -ENOTSUP;
}


void
brcms_c_ampdu_finalize(struct brcms_ampdu_session* session)
{
}


void
brcms_b_txstatus(struct brcms_hardware* wlc_hw, bool bound, bool* fatal)
{
	*fatal = false;
}


void
brcms_c_stf_phytxchain_sel(struct brcms_c_info* wlc, u16 chanspec)
{
}


void
brcms_c_update_beacon(struct brcms_c_info* wlc)
{
}


/* --- the PCI host, which here is the bridge this driver came in through -- */

/*!	Lets the card master the bus, or stops it.

	Upstream this also hands the interrupt line to the bus layer. There is
	no interrupt handler installed yet, so only the mastering matters: the
	DMA engine cannot fetch a descriptor without it.
*/
void
bcma_host_pci_irq_ctl(struct bcma_bus* bus, struct bcma_device* core,
	bool enable)
{
	brcms_glue_set_bus_master(enable);
}


void
bcma_host_pci_up(struct bcma_bus* bus)
{
	brcms_glue_set_bus_master(true);
}


void
bcma_host_pci_down(struct bcma_bus* bus)
{
	brcms_glue_set_bus_master(false);
}
