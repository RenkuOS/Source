/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Distributed under the terms of the MIT license.
 *
 */
#ifndef _USB_AUDIO_SETTINGS_H_
#define _USB_AUDIO_SETTINGS_H_

#include <SupportDefs.h>

enum {
	ERR = 0x00000001,
	INF = 0x00000002,
	MIX = 0x00000004,
	API = 0x00000008,
	DTA = 0x00000010,
	ISO = 0x00000020,
	UAC = 0x00000040
};

// DIAG: skip starting input (capture) streams when the "no_input" driver
// setting is true, to test whether a concurrent input iso stream is what
// keeps the output stream from sustaining (Hercules RMX debugging).
extern bool gSkipInputStreams;

// "no_feedback" driver setting: never pace playback from rate feedback
// (explicit endpoint or implicit capture source); run the exact-rate cadence
// instead.
//
// ⚠️ The original rationale here was "needed on EHCI, whose scheduler cannot
// service a feedback endpoint at its real interval (it packs transactions into
// consecutive microframes)". That was written 2026-07-06 (4a34695) and the
// defect it describes was FIXED four weeks later by 090d3a8, which added
// interval-aware iso scheduling to our EHCI (itd_slot_stride / uframeStride /
// frameStride). So the stated reason no longer holds, and whether EHCI still
// needs this at all is UNVERIFIED - do not repeat the old rationale as fact.
//
// What IS known (2026-08-12, hardware): on xHCI the Hercules RMX2's explicit
// feedback endpoint delivers exactly 8 values and is then never serviced
// again, which stalls playback into silence -- so no_feedback is currently
// required on xHCI for that device. On EHCI the same endpoint kept answering
// (it was even over-polled at 8x/ms before 090d3a8) and tolerated it. The
// asymmetry is not explained; see docs/NEXT-SESSION-usb-audio.md.
//
// This is a WORKAROUND either way: the RMX2's data endpoint is asynchronous,
// and an async sink run without rate feedback drifts.
extern bool gSkipFeedback;

// DIAG: "test_tone" driver setting - playback streams ignore the media data
// and put a generated 440Hz square wave on the wire instead, isolating the
// USB/device path from the entire media stack.
extern bool gTestTone;

// "test_tone_level" driver setting - the sample byte the test tone is built
// from, repeated through every byte of the wire slot (see _StageWireChunk for
// why it must repeat). Clamped to 1..0x40.
//
// This is a level knob because getting it wrong HURTS. The tone is a square
// wave, so its RMS equals its peak - roughly 8-10 dB hotter than music at the
// same nominal amplitude, and harsh with it. It is played into DJ gear whose
// own volume controls this driver has just set to 0 dB (max), sometimes into
// headphones. 0x40 (the level this shipped with) is -12 dBFS and was reported
// from hardware as "glaringly loud, I had to unplug the audio cables".
//
// The default 0x04 is -30 dBFS: plainly audible on monitored gear, harmless if
// it catches someone by surprise. Raise it only if a real test came back
// ambiguous, and turn the gear down first.
extern uint32 gTestToneLevel;

// DIAG: "rearm_alternate" driver setting - after programming a UAC1 endpoint's
// sampling frequency, park the streaming interface at alternate 0 and
// re-select the active one, the way the UAC2 path arms a clock. For devices
// that ACK a SET_CUR sampling frequency without latching it.
extern bool gRearmAlternate;

// DIAG: "reject_unverified_rate" driver setting - when a GET_CUR readback does
// not confirm the sampling rate just programmed, refuse the format (default)
// rather than streaming to a device that may be on a different clock. Turn it
// off to test whether a device whose readback is untrustworthy nonetheless
// plays correctly - notably one in a mode where it follows the USB data's rate
// instead of latching SET_CUR.
extern bool gRejectUnverifiedRate;

// DIAG: "force_alternate" driver setting - use this streaming alternate instead
// of the one _ChooseAlternate scores highest (-1 = automatic). Selection ranks
// by channels*100 + bitResolution and ignores sample rate, so a device is
// always driven on its widest alternate; this pins a lighter one to test
// whether a rate is only really implemented there.
extern int32 gForceAlternate;

// DIAG: "max_rate" driver setting - highest sampling rate advertised to the
// media kit, in Hz (0 = advertise everything the descriptors list). The media
// kit picks the highest rate on offer, so this is the only effective way to
// keep it from selecting a rate the device cannot actually take.
extern uint32 gMaxRate;

void load_settings();
void release_settings();

// Active trace bits (the "trace" driver setting; ERR only by default).
// Exposed so a diagnostic whose COST is significant -- e.g. one that scans a
// whole audio buffer -- can skip the work when its bit is off, instead of
// paying for it and discarding the result inside TRACE().
extern uint32 gTraceMask;

void usb_audio_trace(uint32 bits, const char* func, const char* fmt, ...);

#define TRACE_USB_AUDIO

#ifdef TRACE
#undef TRACE
#endif

#ifdef TRACE_USB_AUDIO
#define TRACE(__mask__, x...) usb_audio_trace(__mask__, __func__, x)
#else
#define TRACE(__mask__, x...) // nothing
#endif

#endif // _USB_AUDIO_SETTINGS_H_

