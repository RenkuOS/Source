/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Distributed under the terms of the MIT license.
 */


#include "Stream.h"

#include <new>
#include <string.h>

#include <kernel.h>
#include <usb/USB_audio.h>

#include "Device.h"
#include "Driver.h"
#include "Settings.h"


// Sentinel for fFreqShift: the feedback format has not been auto-detected yet.
static const int32 kFreqShiftUnset = -2147483647 - 1;


Stream::Stream(Device* device, size_t interface, usb_interface_list* List)
	:
	AudioStreamingInterface(&device->AudioControl(), interface, List),
	fDevice(device),
	fStatus(B_NO_INIT),
	fStartFailures(0),
	fConsecutiveErrors(0),
	fErrorParked(0),
	fUSBConfig(NULL),
	fStreamEndpoint(0),
	fIsRunning(false),
	fMaxPacketSize(0),
	fArea(-1),
	fKernelArea(-1),
	fAreaSize(0),
	fRecordScratchArea(-1),
	fRecordScratch(NULL),
	fPlaybackScratchArea(-1),
	fPlaybackScratch(NULL),
	fPlaybackScratchStride(0),
	fPlaybackCarry(NULL),
	fPlaybackCarryLength(0),
	fDescriptors(NULL),
	fDescriptorsCount(0),
	fCurrentBuffer(0),
	fSamplesCount(0),
	fNeedsPacking(false),
	fMediaSampleSize(0),
	fWireSampleSize(0),
	fStreamChannels(0),
	fWireArea(-1),
	fWireBuffers(NULL),
	fClientBuffers(NULL),
	fExtraFramePeriod(0),
	fBufferCount(kSamplesBufferCount),
	fRequestedFrames(0),
	fRealTime(0),
	fFramesPlayed(0),
	fLastCompleteTime(0),
	fGapCount(0),
	fMaxGap(0),
	fMediaLateCount(0),
	fErrorCount(0),
	fStartingFrame(0),
	fNextStartFrame(0),
	fFrameChainValid(false),
	fProcessedBuffers(0),
	fLastReportedCycle(0),
	fInsideNotify(0),
	fDataEndpointIsAsync(false),
	fIsFeedbackSource(false),
	fUseImplicitFeedback(false),
	fUseExplicitFeedback(false),
	fCaptureFramesTotal(0),
	fCapturePacketsTotal(0),
	fFeedbackEndpoint(0),
	fFeedbackArea(-1),
	fFeedbackBuffer(NULL),
	fFeedbackPacketSize(0),
	fFeedbackFrame(0),
	fPacketsPerBuffer(0),
	fFeedbackLogCount(0),
	fLastStageLog(0),
	fLastFeedbackErrorLog(0),
	fIsoPacketsSeen(0),
	fIsoSaturated(0),
	fIsoErrors(0),
	fIsoLastError(B_OK),
	fIsoMaxActual(0),
	fIsoMaxActualReq(0),
	fLastIsoErrorLog(0),
	fLastIsoSummaryLog(0),
	fNominalFreq(0),
	fMaxFreq(0),
	fMaxFrameSize(0),
	fDataInterval(0),
	fCurrentFreq(0),
	fFeedbackPhase(0),
	fFreqShift(kFreqShiftUnset)
{
}


Stream::~Stream()
{
	delete_area(fArea);
	delete_area(fKernelArea);
	delete_area(fWireArea);
	delete_area(fFeedbackArea);
	delete_area(fRecordScratchArea);
	delete_area(fPlaybackScratchArea);
	delete fDescriptors;
}


status_t
Stream::_ChooseAlternate()
{
	// lookup alternate with maximal (ch * 100 + resolution)
	uint16 maxChxRes = 0;
	for (int i = 0; i < fAlternates.Count(); i++) {
		if (fAlternates[i]->Interface() == 0) {
			TRACE(INF, "Ignore alternate %d - zero interface description.\n", i);
			continue;
		}

		if (fAlternates[i]->Format() == 0) {
			TRACE(INF, "Ignore alternate %d - zero format description.\n", i);
			continue;
		}

		if (fAlternates[i]->Format()->fFormatType
				!= USB_AUDIO_FORMAT_TYPE_I) {
			TRACE(ERR, "Ignore alternate %d - format type %#02x "
				"is not supported.\n", i, fAlternates[i]->Format()->fFormatType);
			continue;
		}

		ASInterfaceDescriptor* asInterface = fAlternates[i]->Interface();
		TypeIFormatDescriptor* format
			= static_cast<TypeIFormatDescriptor*>(fAlternates[i]->Format());

		if (asInterface->fIsR2) {
			// R2: supported formats are a bitmap; accept PCM / PCM8 / FLOAT.
			uint32 supported = USB_AUDIO_R2_FORMAT_PCM
				| USB_AUDIO_R2_FORMAT_PCM8 | USB_AUDIO_R2_FORMAT_IEEE_FLOAT;
			if ((asInterface->fBmFormats & supported) == 0) {
				TRACE(ERR, "Ignore alternate %d - formats %#08x are not "
					"supported.\n", i, asInterface->fBmFormats);
				continue;
			}

			if ((asInterface->fBmFormats & USB_AUDIO_R2_FORMAT_PCM) != 0) {
				switch (format->fBitResolution) {
					default:
					TRACE(ERR, "Ignore alternate %d - bit resolution %d "
						"is not supported.\n", i, format->fBitResolution);
						continue;
					case 8: case 16: case 18: case 20: case 24: case 32:
						break;
				}
			}
		} else {
			switch (asInterface->fFormatTag) {
				case USB_AUDIO_FORMAT_PCM:
				case USB_AUDIO_FORMAT_PCM8:
				case USB_AUDIO_FORMAT_IEEE_FLOAT:
			//	case USB_AUDIO_FORMAT_ALAW:
			//	case USB_AUDIO_FORMAT_MULAW:
					break;
				default:
					TRACE(ERR, "Ignore alternate %d - format %#04x is not "
						"supported.\n", i, asInterface->fFormatTag);
				continue;
			}

			if (asInterface->fFormatTag == USB_AUDIO_FORMAT_PCM) {
				switch(format->fBitResolution) {
					default:
					TRACE(ERR, "Ignore alternate %d - bit resolution %d "
						"is not supported.\n", i, format->fBitResolution);
						continue;
					case 8: case 16: case 18: case 20: case 24: case 32:
						break;
				}
			}
		}

		// Multichannel R1 devices exist too (DDJ-SR, Hercules RMX: 4-out/4-in
		// UAC1), so bound both revisions only by the hmulti channel maximum.
		uint8 maxChannels = AudioControlInterface::kChannels;
		if (format->fNumChannels > maxChannels) {
			TRACE(ERR, "Ignore alternate %d - channel count %d "
				"is not supported.\n", i, format->fNumChannels);
			continue;
		}

		uint16 chxRes = format->fNumChannels * 100 + format->fBitResolution;
		if (chxRes > maxChxRes) {
			maxChxRes = chxRes;
			fActiveAlternate = i;
		}
	}

	if (maxChxRes <= 0) {
		TRACE(ERR, "No compatible alternate found. "
			"Stream initialization failed.\n");
		return B_NO_INIT;
	}

	// Manual override, for testing whether a rate is only truly implemented on
	// a narrower alternate than the widest one we would otherwise pick. Only
	// accept an alternate that carries the descriptors the rest of the stream
	// setup dereferences, so a bad setting cannot crash the driver.
	if (gForceAlternate >= 0) {
		if (gForceAlternate < fAlternates.Count()
				&& fAlternates[gForceAlternate]->Interface() != 0
				&& fAlternates[gForceAlternate]->Format() != 0
				&& fAlternates[gForceAlternate]->Endpoint() != 0) {
			TRACE(ERR, "force_alternate: using alternate %d instead of %d.\n",
				(int)gForceAlternate, fActiveAlternate);
			fActiveAlternate = gForceAlternate;
		} else {
			TRACE(ERR, "force_alternate: alternate %d is unusable; keeping "
				"%d.\n", (int)gForceAlternate, fActiveAlternate);
		}
	}

	const ASEndpointDescriptor* endpoint
		= fAlternates[fActiveAlternate]->Endpoint();
	fIsInput = (endpoint->fEndpointAddress & USB_ENDPOINT_ADDR_DIR_IN)
		== USB_ENDPOINT_ADDR_DIR_IN;

	if (fIsInput)
		fCurrentBuffer = (size_t)-1;

	TRACE(INF, "Alternate %d EP:%x selected for %s!\n",
		fActiveAlternate, endpoint->fEndpointAddress,
		fIsInput ? "recording" : "playback");

	return B_OK;
}


status_t
Stream::Init()
{
	fStatus = _ChooseAlternate();
	if (fStatus != B_OK)
		return fStatus;

	// R2 devices do not carry the sample-rate list in the format descriptor;
	// query it from the Clock Source that feeds this stream's terminal.
	if (fControlInterface->SpecReleaseNumber() >= 0x200)
		fStatus = _SetupUAC2Rates();

	return fStatus;
}


// Whether the active alternate actually offers this sampling rate. Used to
// judge whether a GET_CUR readback is saying anything meaningful: a value the
// device never advertised is not a report of what it did, it is noise.
bool
Stream::_RateIsAdvertised(uint32 rate)
{
	TypeIFormatDescriptor* format = static_cast<TypeIFormatDescriptor*>(
		fAlternates[fActiveAlternate]->Format());
	if (format == NULL)
		return false;

	Vector<uint32>& frequencies = format->fSampleFrequencies;

	// A continuous range is expressed as its two bounds, in either order.
	if (format->fSampleFrequencyType == 0) {
		if (frequencies.Count() < 2)
			return false;
		uint32 low = frequencies[0];
		uint32 high = frequencies[1];
		if (low > high) {
			uint32 swap = low;
			low = high;
			high = swap;
		}
		return rate >= low && rate <= high;
	}

	for (int i = 0; i < frequencies.Count(); i++) {
		if (frequencies[i] == rate)
			return true;
	}

	return false;
}


status_t
Stream::_SetupUAC2Rates()
{
	AudioStreamAlternate* alternate = fAlternates[fActiveAlternate];
	TypeIFormatDescriptor* format
		= static_cast<TypeIFormatDescriptor*>(alternate->Format());
	if (format == NULL)
		return B_NO_INIT;

	uint8 clockId = fControlInterface->ClockSourceIdForTerminal(TerminalLink());
	if (clockId == 0) {
		TRACE(ERR, "No clock source for terminal %d.\n", TerminalLink());
		return B_ERROR;
	}

	Vector<uint32> rates;
	status_t status = fControlInterface->GetSamplingRates(clockId, rates);
	if (status != B_OK)
		return status;

	if (rates.Count() <= 0) {
		TRACE(ERR, "Clock %d reported no sampling rates.\n", clockId);
		return B_ERROR;
	}

	// Cache the discrete rates on the active alternate so the existing rate
	// reporting / selection paths (which read the format descriptor) work
	// unchanged. A non-zero frequency type marks a discrete list.
	format->fSampleFrequencies.MakeEmpty();
	for (int i = 0; i < rates.Count(); i++)
		format->fSampleFrequencies.PushBack(rates[i]);
	format->fSampleFrequencyType = rates.Count();

	// Pick a sensible default (highest available) until the media kit sets one.
	alternate->SetSamplingRate(0);

	return B_OK;
}


void
Stream::OnRemove()
{
	// Streaming stopped with the device; marking it so also lets the next
	// buffer exchange restart the stream if the device is plugged back in
	// and reattached.
	fIsRunning = false;

	// The media add-on keeps the device open across a removal, so Stop() may
	// never run for this stream. Report the run from here instead; this is the
	// USB stack's notification thread, where a blocking trace is harmless.
	_ReportRunHealth();

	// the transfer callback schedule traffic - so we must ensure that we are
	// not inside the callback anymore before returning, as we would otherwise
	// violate the promise not to use any of the pipes after returning from the
	// removed callback
	while (atomic_get(&fInsideNotify) != 0)
		snooze(100);

	if (fFeedbackEndpoint != 0)
		gUSBModule->cancel_queued_transfers(fFeedbackEndpoint);
	gUSBModule->cancel_queued_transfers(fStreamEndpoint);
}


/*!	One line per run, and only when something actually went wrong. Called from
	both Stop() and OnRemove() because a stream can end either way: a removal
	while the media add-on still holds the device open never reaches Stop().
*/
void
Stream::_ReportRunHealth()
{
	if (fGapCount == 0 && fMediaLateCount == 0 && fErrorCount == 0)
		return;

	TRACE(ERR, "%s run summary: %" B_PRIu32 " completion gaps (max %"
		B_PRIdBIGTIME " us), %" B_PRIu32 " media-late requeues, %"
		B_PRIu32 " transfer errors\n", fIsInput ? "rec" : "pb",
		fGapCount, fMaxGap, fMediaLateCount, fErrorCount);
}


status_t
Stream::_SetupBuffers()
{
	// The geometry that could not be started is about to be replaced, so a
	// parked stream is unparked here: picking a different rate or format in
	// Media Preferences is exactly how a user recovers one. That covers both
	// park reasons -- a start that never succeeded, and a run the transfer-error
	// cap killed (fErrorParked); the transfers of that dead run are reaped by
	// the Stop() below before any new endpoint is armed.
	fStartFailures = 0;
	atomic_set(&fErrorParked, 0);

	// allocate buffer for worst (maximal size) case
	TypeIFormatDescriptor* format = static_cast<TypeIFormatDescriptor*>(
		fAlternates[fActiveAlternate]->Format());

	uint32 samplingRate = fAlternates[fActiveAlternate]->GetSamplingRate();

	// The device streams format->fSubframeSize bytes per sample on the wire.
	// The media kit has no 3-byte sample type, so for packed 24-bit devices
	// (subframe 3) it hands us 4-byte samples (B_AUDIO_INT). In that case we
	// size the mixer-facing buffer for 4-byte samples and convert 4<->3 per
	// transfer. For 16-bit (subframe 2) and 32-bit / 24-in-32 (subframe 4)
	// devices the two sizes are equal and nothing changes.
	fStreamChannels = format->fNumChannels;
	fNeedsPacking = (format->fSubframeSize == 3);
	uint8 mediaSubframe = fNeedsPacking ? 4 : format->fSubframeSize;
	fWireSampleSize = format->fNumChannels * format->fSubframeSize;
	fMediaSampleSize = format->fNumChannels * mediaSubframe;
	uint32 sampleSize = fWireSampleSize;	// wire bytes per audio frame

	const bool highSpeed = fDevice->fUSBVersion >= 0x0200;

	// The endpoint is serviced once every (1 << fDataInterval) microframes, and
	// THAT - not the microframe - is the unit a packet has to fill. Sizing per
	// microframe hands a device that asked for one service per millisecond
	// eight packets instead, leaving its clock recovery to reassemble them.
	// An adaptive sink with no feedback path (DN-HC4500) has no way to tell us
	// it is losing lock, and degrades audibly over minutes on that diet - the
	// onset time scales with how much we split up, but no split is clean.
	//
	// bInterval 1 (one microframe per service - the Hercules RMX2, the only
	// high-speed device this code was previously exercised on) and every
	// full-speed device keep their existing geometry bit for bit.
	uint8 serviceShift = 0;
	if (highSpeed) {
		serviceShift = fDataInterval;
		// An iTD spans 8 microframes, so one packet per frame-list frame is the
		// sparsest the host controller schedules; a longer service interval
		// still gets one packet per millisecond rather than a stalled stream.
		if (serviceShift > 3)
			serviceShift = 3;
	}
	// Packets the controller can place in one 1 ms frame-list frame.
	const uint32 packetsPerFrame = highSpeed ? (8u >> serviceShift) : 1;
	const uint32 divisor = highSpeed ? (8000u >> serviceShift) : 1000;

	// data size pro 1 ms USB 1 frame or 1/8 ms USB 2 microframe.
	// A packet must carry whole audio frames: a byte count truncated
	// mid-frame (4ch/24-bit: 44100 * 12 / 1000 -> 529, but a frame is 12
	// bytes) shifts the device's channel/byte framing on every packet. The
	// fractional frame per (micro)frame is carried by the exact-rate cadence
	// below (or by rate feedback), never by odd trailing bytes.
	size_t packetSize = (size_t)samplingRate * sampleSize / divisor;
	packetSize -= packetSize % sampleSize;
	TRACE(INF, "packetSize:%ld (wire ss:%d media ss:%d packing:%d)\n",
		packetSize, fWireSampleSize, fMediaSampleSize, fNeedsPacking);

	if (packetSize == 0) {
		TRACE(ERR, "computed packet size is 0!");
		return B_BAD_VALUE;
	}

	// Exact-rate cadence for non-feedback streams: with a fractional frame
	// count per (micro)frame (44.1kHz full-speed: 44.1; high-speed: 5.5125)
	// the packet sizes follow the floor-accumulator pattern (full-speed
	// 44.1kHz: 9x44 + 1x45 per 10ms - byte-identical to what the Windows
	// driver sends). A flat floor-sized stream is audibly slow and keeps
	// rate-locking DACs muted entirely. The pattern repeats every
	// divisor/gcd(rate, divisor) packets; buffers hold whole patterns so
	// every buffer carries the same (whole) frame count.
	uint32 remainder = samplingRate % divisor;
	uint32 cadencePeriod = 1;
	if (remainder != 0) {
		uint32 a = samplingRate, b = divisor;
		while (b != 0) {
			uint32 t = a % b;
			a = b;
			b = t;
		}
		cadencePeriod = divisor / a;
	}
	fExtraFramePeriod = remainder != 0 ? cadencePeriod : 0;

	// Restart the rate estimate whenever the format/rate changes, seeding it at
	// the nominal rate. A capture feedback source then starts the playback
	// stream from the right rate instead of ramping up from zero (which would
	// underfeed the device for the first seconds); the small seed window is
	// quickly outweighed by real per-buffer measurements.
	fCapturePacketsTotal = 256;
	fCaptureFramesTotal = (uint64)samplingRate * fCapturePacketsTotal / divisor;

	if (fArea != -1) {
		Stop();
		delete_area(fArea);
		delete_area(fKernelArea);
		delete_area(fWireArea);
		fWireArea = -1;
		fWireBuffers = NULL;
		delete_area(fRecordScratchArea);
		fRecordScratchArea = -1;
		fRecordScratch = NULL;
		delete_area(fPlaybackScratchArea);
		fPlaybackScratchArea = -1;
		fPlaybackScratch = NULL;
		fPlaybackCarry = NULL;
		fPlaybackCarryLength = 0;
		delete fDescriptors;
		fDescriptors = NULL;
	}

	if (cadencePeriod > kMaxPacketsPerBuffer) {
		TRACE(ERR, "Cadence period %lu exceeds the per-buffer packet bound.\n",
			cadencePeriod);
		return B_BAD_VALUE;
	}

	if (!highSpeed) {
		// Full-speed (UHCI) path, verified on the DDJ-SR / Hercules RMX: keep
		// the exact geometry that ships audio there - two buffers of one
		// cadence period each (44.1kHz: 10 packets, 441 frames), or the stock
		// envelope for integer rates. Consumer geometry requests are ignored.
		fBufferCount = 2;
		if (fExtraFramePeriod > 0) {
			// one buffer = one cadence period (44.1kHz: 10 packets, 441 frames)
			size_t packetsPerBuffer = fExtraFramePeriod;
			size_t framesPerBuffer = (size_t)samplingRate * packetsPerBuffer
				/ divisor;
			fPacketsPerBuffer = packetsPerBuffer;
			fDescriptorsCount = packetsPerBuffer * fBufferCount;
			fSamplesCount = framesPerBuffer * fBufferCount;
		} else {
			size_t scheduleSize = sampleSize * kSamplesBufferSize
				* fBufferCount;
			fDescriptorsCount = scheduleSize / packetSize;
			fDescriptorsCount = ROUNDDOWN(fDescriptorsCount, fBufferCount);
			fPacketsPerBuffer = fDescriptorsCount / fBufferCount;
			fSamplesCount = fDescriptorsCount * packetSize / sampleSize;
		}
	} else {
		// High-speed geometry: fBufferCount buffers of the frame count the
		// consumer requested via B_MULTI_GET_BUFFERS (0 = the
		// kSamplesBufferSize default), cut to a whole number of cadence
		// periods so every buffer holds the same whole frame count; the
		// packet count is bounded well below the host controller's
		// per-transfer limit so feedback still has room to resize.
		if (fBufferCount > kMaxHighSpeedBuffers)
			fBufferCount = kMaxHighSpeedBuffers;

		// A buffer must span whole cadence periods (same whole audio-frame
		// count per buffer) AND whole frame-list frames (packetsPerFrame
		// packets per frame): a buffer ending mid-frame leaves the rest of
		// that frame's microframes empty - a repeating hole in the stream.
		// Round to the least common multiple of both. At one packet per frame
		// the frame constraint is vacuous and only the cadence period binds.
		uint32 packetAlign = cadencePeriod;
		while (packetAlign % packetsPerFrame != 0)
			packetAlign += cadencePeriod;

		uint32 frames = fRequestedFrames != 0 ? fRequestedFrames
			: kSamplesBufferSize;
		if (frames > kMaxRequestFrames)
			frames = kMaxRequestFrames;
		size_t packets = (size_t)frames * divisor / samplingRate;
		packets -= packets % packetAlign;
		if (packets < packetAlign)
			packets = packetAlign;
		if (packets > kMaxPacketsPerBuffer)
			packets = kMaxPacketsPerBuffer - kMaxPacketsPerBuffer
				% packetAlign;

		// Keep the whole in-flight window inside the host controller's
		// isochronous horizon (see kMaxHighSpeedScheduleFrames): each frame
		// carries packetsPerFrame packets, so a longer service interval means
		// fewer, larger packets spanning the same wall-clock window.
		size_t packetCap = kMaxHighSpeedScheduleFrames * packetsPerFrame
			/ fBufferCount;
		packetCap -= packetCap % packetAlign;
		if (packetCap >= packetAlign && packets > packetCap)
			packets = packetCap;

		// Keep a capture transfer inside the single block the USB stack can
		// allocate (kMaxIsochronousTransferSize). Capture receives at full
		// wMaxPacketSize stride, so its transfer is packets * fMaxPacketSize --
		// NOT packets * packetSize, which is what every other bound here is
		// reasoning about. A device with a large wMaxPacketSize and a 125 us
		// service interval blows the limit while carrying very little audio,
		// and before this cap existed that failed every start forever.
		// Playback stages its real byte count, so it is left alone.
		if (fIsInput && fMaxPacketSize > 0) {
			size_t allocCap = kMaxIsochronousTransferSize / fMaxPacketSize;
			allocCap -= allocCap % packetAlign;
			if (allocCap < packetAlign) {
				// One aligned group of packets already exceeds the limit;
				// nothing this stream can ask for will ever be satisfied.
				TRACE(ERR, "Capture packet stride %u x %lu exceeds the USB "
					"stack's %lu-byte transfer limit.\n", fMaxPacketSize,
					(unsigned long)packetAlign,
					(unsigned long)kMaxIsochronousTransferSize);
				return B_BAD_VALUE;
			}
			if (packets > allocCap)
				packets = allocCap;
		}

		if (packets == 0 || packets > kMaxPacketsPerBuffer) {
			TRACE(ERR, "No usable packet alignment (period %lu).\n",
				cadencePeriod);
			return B_BAD_VALUE;
		}

		fPacketsPerBuffer = packets;
		fDescriptorsCount = fPacketsPerBuffer * fBufferCount;
		// whole frames per buffer: packets/period whole patterns of
		// rate*period/divisor frames each
		size_t framesPerBuffer = (size_t)samplingRate * fPacketsPerBuffer
			/ divisor;
		fSamplesCount = framesPerBuffer * fBufferCount;
	}
	TRACE(INF, "samplesCount:%d\n", fSamplesCount);

	// The mixer-facing buffer holds fSamplesCount frames at the media sample
	// size (4-byte samples when packing).
	fAreaSize = fSamplesCount * fMediaSampleSize;
	TRACE(INF, "estimate fAreaSize:%d\n", fAreaSize);

	// round up to B_PAGE_SIZE and create area
	fAreaSize = (fAreaSize + (B_PAGE_SIZE - 1)) &~ (B_PAGE_SIZE - 1);
	TRACE(INF, "rounded up fAreaSize:%d\n", fAreaSize);

	// B_FULL_LOCK, not B_NO_LOCK: the client (media_addon_server) writes this
	// area through ITS mapping while the kernel reads it through its own /
	// the clone's mapping. With lazily-committed pages those two fault paths
	// can end up on DIFFERENT pages for the same offsets (observed on
	// x86_64 hrev59811: client-written audio invisible to the kernel ->
	// eternal silence on USB). Pre-committing the pages makes every mapping
	// coherent - the same reason hda allocates its buffers B_CONTIGUOUS.
	// B_CLONEABLE_AREA: a client may clone this area into its own team and
	// report the rebased addresses with B_MULTI_SET_BUFFERS instead of writing
	// through the address GetBuffers hands out. Cloning was added alongside
	// B_FULL_LOCK on hrev59811, before it was known which of the two cured the
	// silence; the stock multi_audio add-on, which does not clone, plays
	// correctly with B_FULL_LOCK alone (2026-09-29, hrev60072+112, on EHCI
	// and UHCI).
	fArea = create_area(fIsInput ? DRIVER_NAME "_record_area"
		: DRIVER_NAME "_playback_area", (void**)&fBuffers,
		B_ANY_ADDRESS, fAreaSize, B_FULL_LOCK,
		B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
	if (fArea < 0) {
		TRACE(ERR, "Error of creating %#x - "
			"bytes size buffer area:%#010x\n", fAreaSize, fArea);
		fStatus = fArea;
		return fStatus;
	}

	// The kernel is not allowed to touch userspace areas, so we clone our
	// area into kernel space.
	fKernelArea = clone_area("usb_audio cloned area", (void**)&fKernelBuffers,
		B_ANY_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, fArea);
	if (fKernelArea < 0) {
		fStatus = fKernelArea;
		return fStatus;
	}

	TRACE(INF, "Created area id:%d at addr:%#010x size:%#010lx\n",
		fArea, fDescriptors, fAreaSize);

	// The buffer poison that proved the media node writes digital silence
	// (2026-08-12) lived here: create_area() returns ZEROED pages, so a
	// non-zero count of 0 could not distinguish "the client never wrote" from
	// "the client wrote silence". Filling with 0x01 first answered it -- exactly
	// half the bytes came back zeroed, i.e. the stereo pair the mixer binds, so
	// the client writes and writes silence.
	//
	// REMOVED once it had answered, deliberately: it puts a DC offset on the
	// wire and was audible as low pops before the node's first write. If that
	// question ever needs re-asking, put it back behind a driver setting rather
	// than unconditionally, and keep the value tiny (0x01 is ~-42 dBFS; 0xAA
	// would be full-scale noise into someone's monitors).

	// The wire buffer - what queue_isochronous hands to the host controller -
	// is a separate B_CONTIGUOUS kernel area (the same allocation policy hda
	// uses for its DMA buffers). The USB finisher thread fills it at queue
	// time from the ORIGINAL mixer area VA (fBuffers) - packing to 3-byte
	// samples, or memcpy + channel mirror for native formats. It must not
	// alias the mixer area, and no other context may be responsible for
	// filling it: cross-context writes through clones of these areas are not
	// reliably visible to the finisher on x86_64 hrev59811 (a wire capture
	// showed 68% zeros while the writing context saw all buffers filled).
	{
		size_t wireAreaSize = fSamplesCount * fWireSampleSize;
		wireAreaSize = (wireAreaSize + (B_PAGE_SIZE - 1)) &~ (B_PAGE_SIZE - 1);
		fWireArea = create_area("usb_audio wire buffer",
			(void**)&fWireBuffers, B_ANY_ADDRESS, wireAreaSize, B_CONTIGUOUS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
		if (fWireArea < 0) {
			TRACE(ERR, "Error creating wire buffer area:%#010x\n", fWireArea);
			fStatus = fWireArea;
			return fStatus;
		}
		memset(fWireBuffers, 0, wireAreaSize);
	}

	// Pace a playback stream from the capture stream's measured delivery rate:
	// both run off the device's one crystal, so the capture rate is the device's
	// exact playback rate (USB Audio 2.0 § 5.12.4.2). Rate feedback of either
	// kind resizes the outgoing packets individually, which the host controller
	// driver must also support; probe that once per device and run at the
	// nominal rate (uncorrected drift, but working audio) if it refuses.
	bool variableIsoOut = false;
	if (!fIsInput && !gSkipFeedback
			&& (fDevice->HasImplicitFeedbackSource()
				|| fFeedbackEndpoint != 0)) {
		if (fDevice->VariableIsoOutSupport() < 0)
			fDevice->SetVariableIsoOutSupport(_ProbeVariableIsoOut(sampleSize));
		variableIsoOut = fDevice->VariableIsoOutSupport() > 0;
	}
	fUseImplicitFeedback = variableIsoOut
		&& fDevice->HasImplicitFeedbackSource();

	// An explicit feedback endpoint would be the fallback drift source, but some
	// devices advertise one their firmware never services (e.g. the Behringer
	// UMCxxHD, which Linux flags GENERIC_IMPLICIT_FB): polling that dead endpoint
	// only floods the host controller with transaction errors that starve the
	// shared transfer-completion path and glitch playback. Leave it unqueried
	// when the device exposes an implicit source instead.
	fUseExplicitFeedback = variableIsoOut && fFeedbackEndpoint != 0
		&& !fDevice->HasImplicitFeedbackSource();

	bool useFeedback = fUseImplicitFeedback || fUseExplicitFeedback;
	if (useFeedback) {
		_InitFeedbackParams(samplingRate);

		// With feedback the packet size varies per microframe: when the
		// device asks for a lower rate more (smaller) packets are needed to
		// carry the same number of samples. freqm is clamped to >= 7/8 of
		// nominal, so twice the nominal packet count is always sufficient.
		fPacketsPerBuffer *= 2;

		// Only the explicit feedback endpoint needs a DMA buffer to receive
		// into; implicit feedback reads the rate straight from the device.
		if (fUseExplicitFeedback && fFeedbackArea < 0) {
			fFeedbackArea = create_area(DRIVER_NAME "_feedback_area",
				(void**)&fFeedbackBuffer, B_ANY_ADDRESS, B_PAGE_SIZE,
				B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
			if (fFeedbackArea < 0) {
				TRACE(ERR, "Error creating feedback area:%#010x\n",
					fFeedbackArea);
				fStatus = fFeedbackArea;
				return fStatus;
			}
		}

		// A feedback-paced playback stream assembles each outgoing transfer
		// in a staging buffer: the remainder the previous transfer could not
		// send, followed by the media buffer. Packets can then be cut purely
		// by the feedback schedule instead of being rounded to the media
		// buffer boundary, which would pin the average rate back to nominal
		// (see _QueueNextTransfer). The carry is up to one USB frame's worth
		// of packets (8 x wMaxPacketSize): transfers are rounded down to
		// whole frames so the EHCI's frame-granular scheduling never leaves
		// silent microframes between transfers (see _FillPlaybackPackets).
		size_t bufferBytes = (fSamplesCount / fBufferCount) * sampleSize;
		fPlaybackScratchStride = bufferBytes + 8 * (size_t)fMaxPacketSize;
		size_t scratchSize = fPlaybackScratchStride * fBufferCount
			+ 8 * (size_t)fMaxPacketSize;
		scratchSize = (scratchSize + B_PAGE_SIZE - 1)
			& ~(size_t)(B_PAGE_SIZE - 1);
		fPlaybackScratchArea = create_area(DRIVER_NAME "_playback_scratch",
			(void**)&fPlaybackScratch, B_ANY_ADDRESS, scratchSize, B_NO_LOCK,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
		if (fPlaybackScratchArea < 0) {
			TRACE(ERR, "Error creating playback scratch area:%#010x\n",
				fPlaybackScratchArea);
			fStatus = fPlaybackScratchArea;
			return fStatus;
		}
		fPlaybackCarry = fPlaybackScratch
			+ fPlaybackScratchStride * fBufferCount;
		fPlaybackCarryLength = 0;
	}

	fDescriptorsCount = fPacketsPerBuffer * fBufferCount;
	fDescriptors = new(std::nothrow)
		usb_iso_packet_descriptor[fDescriptorsCount];
	if (fDescriptors == NULL) {
		TRACE(ERR, "Cannot allocate iso packet descriptors.\n");
		fStatus = B_NO_MEMORY;
		return fStatus;
	}
	TRACE(INF, "descriptorsCount:%d\n", fDescriptorsCount);

	// A capture stream receives each isochronous IN packet at full
	// wMaxPacketSize into a kernel-only scratch buffer, so a device clocked
	// slightly fast can burst above the nominal packet size without overrunning
	// the media buffer (and so its true rate can be measured). Guard against a
	// device advertising a max packet size below the nominal one.
	//
	// This used to be high-speed only, with full-speed "keeping the verified
	// UHCI path": cadence-sized packets received straight into the wire buffer.
	// That path is wrong on any controller and was merely tolerated by UHCI. On
	// an isochronous IN endpoint the DEVICE chooses how many bytes to send, so a
	// buffer sized to our predicted cadence is a guess the device never agreed
	// to. xHCI enforces the posted TRB length: the surplus is truncated and the
	// packet completes with COMP_ISOC_OVERRUN.
	//
	// Measured on a Hercules RMX (06f8:b101, USB 1.1) on a Renesas xHCI root
	// port, 2026-08-14, before this change: actual_length pinned at 352 against
	// a 352-byte request across ~9000 descriptor scans while the endpoint
	// advertised maxpkt 384, ~6-7 of every 20 descriptors carrying the overrun
	// status, the capture endpoint stalled to ~1 completion/s, and playback
	// starved through the shared semaphore -- 209034 of 661500 frames. Evidence:
	// captures/2026-08-14_beta6-rmx-xhci-ISODIAG-capture-truncation-CONFIRMED.log
	size_t recordPacketSize = packetSize;
	if (fIsInput) {
		if (fMaxPacketSize < packetSize) {
			TRACE(ERR, "Record wMaxPacketSize %u below nominal %lu.\n",
				fMaxPacketSize, packetSize);
			fStatus = B_BAD_VALUE;
			return fStatus;
		}
		recordPacketSize = fMaxPacketSize;

		size_t scratchSize = recordPacketSize * fDescriptorsCount;
		scratchSize = (scratchSize + (B_PAGE_SIZE - 1)) &~ (B_PAGE_SIZE - 1);
		fRecordScratchArea = create_area(DRIVER_NAME "_record_scratch",
			(void**)&fRecordScratch, B_ANY_KERNEL_ADDRESS, scratchSize,
			B_NO_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
		if (fRecordScratchArea < 0) {
			TRACE(ERR, "Error creating record scratch area:%#010x\n",
				fRecordScratchArea);
			fStatus = fRecordScratchArea;
			return fStatus;
		}
	}

	// initialize descriptors array. Feedback-paced playback fills
	// request_length per queue; capture receives at wMaxPacketSize regardless of
	// speed (the device picks the length, we cannot); playback follows the
	// exact-rate cadence (uniform packets for integer rates).
	uint32 framesBase = samplingRate / divisor;
	for (size_t i = 0; i < fDescriptorsCount; i++) {
		uint32 cadenceFrames = framesBase;
		if (fExtraFramePeriod > 0) {
			size_t j = i % fPacketsPerBuffer;
			cadenceFrames = (uint32)((uint64)(j + 1) * samplingRate / divisor
				- (uint64)j * samplingRate / divisor);
		}
		if (fIsInput) {
			fDescriptors[i].request_length = recordPacketSize;
		} else {
			fDescriptors[i].request_length = useFeedback
				? 0 : cadenceFrames * sampleSize;
		}
		fDescriptors[i].actual_length = 0;
		fDescriptors[i].status = B_OK;
	}

	return fStatus;
}


status_t
Stream::OnSetConfiguration(usb_device device,
		const usb_configuration_info* config)
{
	if (config == NULL) {
		TRACE(ERR, "NULL configuration. Not set.\n");
		return B_ERROR;
	}

	usb_interface_info* interface
		= &config->interface[fInterface].alt[fActiveAlternate];
	if (interface == NULL) {
		TRACE(ERR, "NULL interface. Not set.\n");
		return B_ERROR;
	}

	// Remembered for the R2 stream-start sequence, which re-toggles this
	// stream's alternate setting (see _ArmClockAndActivate).
	fUSBConfig = config;

	// The Hercules RMX accepts SET_INTERFACE(alt 0) immediately but STALLs
	// SET_INTERFACE(alt 1) for a long time after configuration (Windows'
	// first successful alt 1 comes 16s after SET_CONFIGURATION; Linux only
	// selects alt 1 at stream-open time, minutes after plug). Retry with
	// backoff: without a valid endpoint handle every later queue_isochronous
	// fails with B_DEV_INVALID_PIPE.
	status_t status = gUSBModule->set_alt_interface(device, interface);
	for (int retry = 0; status != B_OK && retry < 60; retry++) {
		snooze(1000000);
		status = gUSBModule->set_alt_interface(device, interface);
		if (status == B_OK || retry % 5 == 4 || retry == 0) {
			TRACE(ERR, "%s set_alt_interface retry %d (t=%lld)"
				" -> %#x\n", fIsInput ? "input" : "output", retry + 1,
				(long long)system_time(), (int)status);
		}
	}

	TRACE(INF, "set_alt_interface %x\n", status);

	return _ResolveEndpoints(interface);
}


// Locate this stream's data (and optional explicit feedback) endpoint in an
// activated alternate setting and cache their pipe handles and endpoint
// parameters. Handles are only valid while that alternate stays active: every
// set_alt_interface invalidates them, so each one is followed by a call here.
status_t
Stream::_ResolveEndpoints(usb_interface_info* interface)
{
	uint8 address = fAlternates[fActiveAlternate]->Endpoint()->fEndpointAddress;

	fStreamEndpoint = 0;
	fFeedbackEndpoint = 0;

	fDataEndpointIsAsync = false;
	fIsFeedbackSource = false;

	for (size_t i = 0; i < interface->endpoint_count; i++) {
		const usb_endpoint_descriptor* descriptor
			= interface->endpoint[i].descr;

		if (address == descriptor->endpoint_address) {
			fStreamEndpoint = interface->endpoint[i].handle;
			fMaxPacketSize = descriptor->max_packet_size;
			// bInterval encodes the service interval as 2^(bInterval-1)
			// (micro)frames; the feedback math needs the exponent.
			fDataInterval = descriptor->interval > 0
				? descriptor->interval - 1 : 0;
			fDataEndpointIsAsync = (descriptor->attributes
				& USB_ENDPOINT_ATTR_SYNCHRONIZE_MASK)
					== USB_ENDPOINT_ATTR_ASYNCRONOUS;
			// A capture endpoint tagged with the implicit-feedback usage type
			// is the device's designated clock reference for its asynchronous
			// playback endpoint (USB Audio 2.0). Advertise it on the device so
			// the playback stream can pick it up.
			if (fIsInput && (descriptor->attributes
					& USB_ENDPOINT_ATTR_USAGE_MASK)
						== USB_ENDPOINT_ATTR_IMPLICIT_USAGE) {
				fIsFeedbackSource = true;
				fDevice->SetImplicitFeedbackSource(fDataInterval);
			}
			TRACE(INF, "%s Stream Endpoint [address %#04x] handle is: %#010x.\n",
				fIsInput ? "Input" : "Output", address, fStreamEndpoint);
			continue;
		}

		// A separate isochronous IN endpoint carrying the feedback usage
		// type is the explicit feedback endpoint of an asynchronous playback
		// stream. Only high-speed devices use the 16.16 format handled here;
		// capture streams never have one.
		if (!fIsInput && fDevice->fUSBVersion >= 0x0200
			&& (descriptor->attributes & USB_ENDPOINT_ATTR_MASK)
				== USB_ENDPOINT_ATTR_ISOCHRONOUS
			&& (descriptor->attributes & USB_ENDPOINT_ATTR_USAGE_MASK)
				== USB_ENDPOINT_ATTR_FEEDBACK_USAGE
			&& (descriptor->endpoint_address & USB_ENDPOINT_ADDR_DIR_IN) != 0) {
			fFeedbackEndpoint = interface->endpoint[i].handle;
			fFeedbackPacketSize = descriptor->max_packet_size;
			if (fFeedbackPacketSize > B_PAGE_SIZE)
				fFeedbackPacketSize = B_PAGE_SIZE;
			TRACE(INF, "Feedback Endpoint [address %#04x] handle is: %#010x.\n",
				descriptor->endpoint_address, fFeedbackEndpoint);
		}
	}

	if (fStreamEndpoint == 0) {
		TRACE(INF, "%s Stream Endpoint [address %#04x] was not found.\n",
			fIsInput ? "Input" : "Output", address);
		return B_ERROR;
	}

	return B_OK;
}


// UAC2 stream-start sequence, replayed before queuing the first transfer of
// every run. The Hercules RMX2 renders eternal silence and answers feedback
// polls with zero-length packets - while ACKing every request and iso packet -
// unless its clock chain is programmed WHILE the streaming interface sits at
// alternate 0 and the active alternate is selected AFTERWARDS. That is the
// order every working host uses (usbmon capture of Linux snd-usb-audio,
// 2026-07-06: park alt 0 -> write clock selector -> write sample rate, a 19ms
// PLL lock -> SET_INTERFACE(alt 1) -> data; real feedback follows within 2ms).
// Our historical order - active alternate at configure time, clock programmed
// whenever the format ioctl arrived - never starts the device's engine.
//
// When another stream of the device is already running the (shared) clock is
// locked and must not be reprogrammed mid-stream; only this stream's
// alternate is then cycled.
status_t
Stream::_ArmClockAndActivate()
{
	if (fUSBConfig == NULL)
		return B_NO_INIT;

	usb_interface_info* park = &fUSBConfig->interface[fInterface].alt[0];
	usb_interface_info* active
		= &fUSBConfig->interface[fInterface].alt[fActiveAlternate];

	status_t status = gUSBModule->set_alt_interface(fDevice->fDevice, park);
	if (status != B_OK) {
		TRACE(ERR, "%s arm: park alt 0 failed %#x\n",
			fIsInput ? "input" : "output", (int)status);
		// Continue: some devices may refuse a redundant alt 0; the clock
		// writes and the alt switch below are the essential part.
	}

	bool clockBusy = false;
	for (int i = 0; i < fDevice->fStreams.Count(); i++) {
		if (fDevice->fStreams[i] != this && fDevice->fStreams[i]->IsRunning())
			clockBusy = true;
	}

	if (!clockBusy) {
		fControlInterface->RouteClockSelectors();
		status = _SetDeviceSamplingRate();
		if (status != B_OK) {
			TRACE(ERR, "%s arm: set rate failed %#x\n",
				fIsInput ? "input" : "output", (int)status);
		}
	}

	status = gUSBModule->set_alt_interface(fDevice->fDevice, active);
	if (status != B_OK) {
		TRACE(ERR, "%s arm: alt %d failed %#x\n",
			fIsInput ? "input" : "output", (int)fActiveAlternate, (int)status);
		return status;
	}

	return _ResolveEndpoints(active);
}


status_t
Stream::OnReattach(usb_device device, const usb_configuration_info* config)
{
	status_t status = OnSetConfiguration(device, config);
	if (status != B_OK)
		return status;

	// A replug earns a fresh attempt: the endpoints are new, and whatever the
	// stack could not satisfy before it may satisfy now. The transfers that
	// parked the stream died with the old device, so clear that too.
	fStartFailures = 0;
	atomic_set(&fErrorParked, 0);

	// The replugged device was power cycled and lost its sampling rate. R1
	// rates are per endpoint, so each stream restores its own here. An R2
	// clock is shared between the streams -- and a stream's selected rate
	// may lag what the clock was last set to -- so it is restored once by
	// Device::CompareAndReattach() instead.
	if (fControlInterface->SpecReleaseNumber() < 0x200)
		return _SetDeviceSamplingRate();

	return B_OK;
}


bool
Stream::HasRateId(uint32 rateId)
{
	AudioStreamAlternate* alternate = fAlternates[fActiveAlternate];
	if (alternate == NULL)
		return false;

	// GetSamplingRateId(0) means "the id of whatever I am currently set to".
	return alternate->GetSamplingRateId(0) == rateId;
}


bool
Stream::SupportsRateId(uint32 rateId)
{
	AudioStreamAlternate* alternate = fAlternates[fActiveAlternate];
	if (alternate == NULL)
		return false;

	return (alternate->GetSamplingRateIds() & rateId) != 0;
}


status_t
Stream::Start()
{
	if (IsStartParked())
		return B_NOT_ALLOWED;

	status_t result = _Start();

	switch (result) {
		case B_OK:
			// A run began: forget any earlier stumbles.
			fStartFailures = 0;
			return result;

		// States, not failures. None of these say anything about whether this
		// stream's geometry can ever be started, so none of them may push it
		// towards being parked:
		case B_BUSY:			// already running
		case B_DEV_NOT_READY:	// device removed; a replug resets us anyway
		case B_CANCELED:		// transfers cancelled mid-removal
		case B_NO_INIT:			// no buffers yet, before the first
								// SET_GLOBAL_FORMAT - counting this would park
								// a stream before it was ever configured
			return result;

		default:
			break;
	}

	if (++fStartFailures >= kMaxStartFailures) {
		// Raw dprintf, once per park: this is the line that explains why a
		// device is half-working, and it must not be lost behind a trace level.
		TRACE(ERR, "%s stream parked after %lu failed starts (last"
			" %#010x) -- it will not be retried until the format changes or"
			" the device is replugged. Other streams are unaffected.\n",
			fIsInput ? "input" : "output",
			(unsigned long)fStartFailures, (unsigned int)result);
	}

	return result;
}


status_t
Stream::_Start()
{
	// Never (re)start a stream on a removed device: the USB stack is tearing
	// the pipes down, and freshly queued transfers keep their reference
	// counts from ever reaching idle (KDL in the pipe destructor).
	if (fDevice->fRemoved)
		return B_DEV_NOT_READY;

	// Refuse to start a stream whose buffers were never configured (e.g. a
	// stream on a replugged device before any SET_GLOBAL_FORMAT ran):
	// queueing it would submit zero-packet isochronous transfers.
	if (fDescriptorsCount == 0 || fSamplesCount == 0) {
		// Rate-limited: the media node may retry every buffer exchange.
		static bigtime_t sLastRefuseLog = 0;
		bigtime_t now = system_time();
		if (now - sLastRefuseLog >= 1000000) {
			sLastRefuseLog = now;
			TRACE(ERR, "%s Stream::Start() refused: no buffers"
				" configured\n", fIsInput ? "input" : "output");
		}
		return B_NO_INIT;
	}

	status_t result = B_BUSY;
	if (!fIsRunning) {
		// R2 devices get the clock-then-alternate arming sequence on every
		// run; without it the RMX2 never starts its audio engine (see
		// _ArmClockAndActivate). R1 keeps its configure-time alternate: the
		// full-speed RMX plays correctly with it, and its endpoints STALL
		// alt changes for a long window after configuration.
		if (fControlInterface->SpecReleaseNumber() >= 0x200) {
			result = _ArmClockAndActivate();
			if (result != B_OK)
				return result;
		}

		// Start a fresh isochronous chain (on full-speed, buffer 0 uses ASAP
		// and the rest chain off it one packet per frame).
		fFrameChainValid = false;
		// Nothing has been played or recorded yet for this run; clear any count
		// left over from a previous run (e.g. a sample-rate change stops and
		// restarts the stream) so it does not corrupt the media server's timing.
		atomic_set(&fProcessedBuffers, 0);

		// Likewise drop any staged remainder and schedule state from a
		// previous run.
		fPlaybackCarryLength = 0;
		fFeedbackPhase = 0;
		// Per-RUN, so every run yields fresh feedback evidence instead of the
		// budget being consumed once for the life of the driver image.
		fFeedbackLogCount = 0;
		// Zero, not system_time(): the first schedule of a run should log
		// immediately rather than after a second of silence.
		fLastStageLog = 0;
		fLastCompleteTime = 0;
		atomic_set(&fConsecutiveErrors, 0);
		fGapCount = 0;
		fMaxGap = 0;
		fMediaLateCount = 0;
		fErrorCount = 0;

		// ISODIAG counters are per-run, like everything above: a cumulative
		// figure spanning several starts cannot be compared against a single
		// audiotone run.
		fIsoPacketsSeen = 0;
		fIsoSaturated = 0;
		fIsoErrors = 0;
		fIsoNoEvent = 0;
		fIsoLastError = B_OK;
		fIsoMaxActual = 0;
		fIsoMaxActualReq = 0;
		fLastIsoErrorLog = 0;
		fLastIsoSummaryLog = 0;

		// The whole geometry this run will stream with, in one line. The
		// packet sizing below emits one packet per service interval, not per
		// microframe: a device declaring bInterval 4 is served once per
		// millisecond, and sizing per microframe fed it eight fragments per
		// interval, which an adaptive sink cannot clock off.
		// Gated at INF for release (2026-08-17). This is how a device's actual
		// mode gets read off a box -- maxpkt, alternate, channel count -- so it
		// stays, but it was a raw dprintf firing on every stream start regardless
		// of the trace setting. One `trace 2` in the driver settings brings it
		// back. Note the emitted prefix changes from "usb_audio DIAG" to
		// "usb_audio: DIAG", so captures older than this commit grep differently.
		{
			uint32 rate = fAlternates[fActiveAlternate]->GetSamplingRate();
			TRACE(INF, "DIAG %s run: rate %lu, alt %u, %lu ch x %u B,"
				" %lu buffers x %lu packets (%lu frames), wire %lu B/frame,"
				" service %u uframes, maxpkt %u, async %d, feedback %s\n",
				fIsInput ? "rec" : "pb", (unsigned long)rate,
				(unsigned)fActiveAlternate, (unsigned long)fStreamChannels,
				(unsigned)(fWireSampleSize / (fStreamChannels != 0
					? fStreamChannels : 1)),
				(unsigned long)fBufferCount,
				(unsigned long)fPacketsPerBuffer,
				(unsigned long)(fSamplesCount / fBufferCount),
				(unsigned long)fWireSampleSize, (unsigned)(1 << fDataInterval),
				(unsigned)fMaxPacketSize, (int)fDataEndpointIsAsync,
				fUseImplicitFeedback ? "implicit"
					: (fUseExplicitFeedback ? "explicit" : "none"));
		}

		// Mark running before queuing so completion callbacks re-queue.
		fIsRunning = true;

		// Start reading the feedback endpoint first so a rate estimate is
		// available as the data packets begin flowing.
		if (fUseExplicitFeedback && fFeedbackBuffer != NULL) {
			result = _QueueFeedback();
			if (result != B_OK) {
				fIsRunning = false;
				return result;
			}
		}

		// Let the endpoint spin-up losses land in silence, not in the first
		// audible buffer.
		if (!fIsInput && (fUseImplicitFeedback || fUseExplicitFeedback))
			_QueueWarmup();

		for (size_t i = 0; i < fBufferCount; i++)
			result = _QueueNextTransfer(i, i == 0);
		fIsRunning = result == B_OK;
	}
	return result;
}


status_t
Stream::Stop()
{
	if (fIsRunning) {
		// Signal stop first so callbacks that start during the wait bail
		// without calling queue_isochronous (and holding a pipe reference).
		fIsRunning = false;
		while (atomic_get(&fInsideNotify) != 0)
			snooze(100);

		_ReportRunHealth();
	}
	if (fFeedbackEndpoint != 0)
		gUSBModule->cancel_queued_transfers(fFeedbackEndpoint);
	gUSBModule->cancel_queued_transfers(fStreamEndpoint);

	return B_OK;
}


// Build the wire chunk for an outgoing buffer at queue time, in the USB
// finisher thread, reading the ORIGINAL buffer area VA (fBuffers). Rationale
// (x86_64 hrev59811, proven by a wire capture): the controller's TD data is
// snapshotted at submit; and kernel-CLONE writes made in the ioctl context
// are NOT reliably visible to this thread - the captured wire stream was 68%
// zeros while the ioctl context saw every buffer tone-filled. The original
// area VA is the one view this thread provably reads correctly (B_FULL_LOCK
// makes client writes visible there), so it is the single source of truth
// for the wire.
void
Stream::_StageWireChunk(size_t queuedBuffer, bool staleSlot)
{
	size_t framesPerBuffer = fSamplesCount / fBufferCount;
	size_t mediaChunkSize = fMediaSampleSize * framesPerBuffer;
	size_t wireChunkSize = fWireSampleSize * framesPerBuffer;
	uint8* mediaChunk = fBuffers + mediaChunkSize * queuedBuffer;
	uint8* wireChunk = fWireBuffers + wireChunkSize * queuedBuffer;

	// DIAG (test_tone setting): put a generated 440Hz square wave on the wire
	// instead of the media data, isolating the USB/device path from the
	// entire media stack. Assumes 4-byte wire subslots (the RMX2).
	if (gTestTone && fWireSampleSize / fStreamChannels == 4) {
		uint32 rate = fAlternates[fActiveAlternate]->GetSamplingRate();
		uint32 halfPeriod = rate > 880 ? rate / 880 : 1;
		static uint32 sTonePhase = 0;
		// The level byte must REPEAT through the whole slot so the tone is
		// audible whether the device reads the top 24 bits of the 4-byte slot
		// (spec) or the bottom 24 (seen in the wild): an amplitude like
		// 0x10000000 has all-zero low 24 bits and is digital silence to a
		// bottom-justified engine. That is why the level is scaled per byte
		// rather than by shifting the whole word - and why gTestToneLevel is a
		// byte value (see Settings.h for how loud each one actually is).
		const uint32 level = gTestToneLevel & 0xff;
		const int32 amplitude = (int32)(level | (level << 8) | (level << 16)
			| (level << 24));

		int32* out = (int32*)wireChunk;
		for (size_t f = 0; f < framesPerBuffer; f++) {
			int32 v = ((sTonePhase / halfPeriod) & 1) != 0
				? amplitude : -amplitude;
			sTonePhase++;
			for (uint16 c = 0; c < fStreamChannels; c++)
				*out++ = v;
		}
		return;
	}

	// Is the mixer's buffer actually carrying audio? This is the measurement
	// that ended the 2026-08-12 hunt, so it is kept -- OFF by default, behind
	// the ISO trace bit, because it scans the whole buffer and writes a line a
	// second. Enable with "trace 0x21" (ERR|ISO) in the driver settings.
	//
	// It lives HERE, in _StageWireChunk, because this runs on BOTH playback
	// paths -- feedback-paced and plain -- which is what makes a `no_feedback`
	// control run possible. Placed in the feedback-only branch of
	// _QueueNextTransfer first, it produced no control data at all and the A/B
	// could not be run. If you move it, keep that property.
	//
	// Reference values, RMX2 at 44100/4ch/4B (14112 bytes per buffer):
	//   ~6000/14112 varying  playing normally
	//    7056/14112 exactly  the mixer wrote silence into the stereo pair it
	//                        binds, leaving the other two channels untouched --
	//                        i.e. the node has no audio, and usb_audio is fine
	if ((gTraceMask & ISO) != 0) {
		bigtime_t now = system_time();
		if (now - fLastStageLog >= 1000000) {
			fLastStageLog = now;
			size_t mediaNonZero = 0;
			for (size_t b = 0; b < mediaChunkSize; b++)
				if (mediaChunk[b] != 0)
					mediaNonZero++;
			TRACE(ISO, "%s stage: buffer %lu, nonzero media %lu/%lu,"
				" feedback %s\n", fIsInput ? "rec" : "pb",
				(unsigned long)queuedBuffer, (unsigned long)mediaNonZero,
				(unsigned long)mediaChunkSize,
				fUseImplicitFeedback ? "implicit"
					: (fUseExplicitFeedback ? "explicit" : "none"));
		}
	}

	// ---------------------------------------------------------------------
	// The media server did not refill this slot: send SILENCE, not the
	// previous rotation's audio.
	// ---------------------------------------------------------------------
	//
	// The completion callback re-queues unconditionally, so when the node
	// falls a full rotation behind, the slot about to go out still holds the
	// audio it carried last time round. Replaying it is not a soft dropout:
	// splicing old samples into a live waveform puts a full-scale STEP
	// DISCONTINUITY at every buffer seam, and at 10 ms buffers that is a
	// ~100 Hz train of edges -- broadband, loud, and hard on tweeters. It is
	// the "harsh squawk" a user reports, as distinct from the mild flutter
	// that completion gaps produce.
	//
	// Zeroing costs nothing: no added latency, no extra packets, one memset
	// on a path that already does a memcpy of the same size. All-zero is
	// digital silence in every Type I PCM layout we emit, packed or not, so
	// this is correct before the fNeedsPacking split rather than after it.
	//
	// Measured on Haiku32 2026-08-31, DDJ-SR over UHCI, 90 s under video-decode
	// load: 56 media-late requeues against 1315 completion gaps. The gaps are
	// the flutter and are NOT addressed here -- they are starvation, and the
	// remedy for those is buffer depth (a Media Preferences knob, deferred).
	// This fixes the SEVERITY of the rarer event, which is the one that can
	// damage speakers.
	//
	// Deliberately placed after both diagnostics above, so the test tone still
	// overrides everything and the ISO buffer-content check still reports what
	// the node actually left behind.
	if (staleSlot) {
		memset(wireChunk, 0, wireChunkSize);
		return;
	}

	if (fNeedsPacking) {
		// Pack the mixer's 4-byte samples into the device's 3-byte 24-bit
		// wire format. The mixer buffer is left intact so re-queuing the
		// same buffer re-packs identically.
		_Pack24(mediaChunk, wireChunk, framesPerBuffer * fStreamChannels);
		return;
	}

	memcpy(wireChunk, mediaChunk, wireChunkSize);

	// The mixer only fills the first stereo pair; mirror it onto the second
	// pair so 4-channel devices whose main output is wired to channels 3/4
	// (DJ consoles: deck B / master pair) are audible too.
	if (fStreamChannels == 4) {
		size_t sub = fWireSampleSize / fStreamChannels;
		uint8* s = wireChunk;
		for (size_t f = 0; f < framesPerBuffer; f++, s += fWireSampleSize)
			memcpy(s + 2 * sub, s, 2 * sub);
	}
}


status_t
Stream::_QueueNextTransfer(size_t queuedBuffer, bool start, bool staleSlot)
{
	usb_iso_packet_descriptor* descriptors
		= fDescriptors + queuedBuffer * fPacketsPerBuffer;

	const bool highSpeed = fDevice->fUSBVersion >= 0x0200;
	size_t framesPerBuffer = fSamplesCount / fBufferCount;
	size_t wireChunkSize = fWireSampleSize * framesPerBuffer;
	uint8* wireChunk = fWireBuffers + wireChunkSize * queuedBuffer;

	// Chain full-speed transfers directly after the previous one (one packet
	// per USB frame, cooperating with the custom UHCI iso scheduler) so the
	// endpoint stays at real time. The very first transfer, or one queued
	// after the chain was reset, uses ASAP to pick a fresh slot. High-speed
	// controllers schedule each ASAP transfer themselves.
	uint32 isoFlags = USB_ISO_ASAP;
	if (!highSpeed && !start && fFrameChainValid) {
		isoFlags = 0;
		fStartingFrame = fNextStartFrame;
	}

	status_t status;

	if (fIsInput) {
		// Receive at full wMaxPacketSize into the scratch buffer; the
		// completion callback repacks the delivered frames into the media
		// record buffer. The per-packet request_length is fixed (set in
		// _SetupBuffers), so nothing is computed here.
		//
		// This is no longer conditional on speed. The full-speed branch used to
		// receive cadence-sized packets straight into the wire chunk, which
		// truncates any packet the device sizes above our prediction -- proven
		// on hardware 2026-08-14, see _SetupBuffers.
		size_t scratchSize = fPacketsPerBuffer * fMaxPacketSize;
		uint8* buffer = fRecordScratch + queuedBuffer * scratchSize;
		status = gUSBModule->queue_isochronous(fStreamEndpoint,
			buffer, scratchSize, descriptors, fPacketsPerBuffer,
			&fStartingFrame, isoFlags, Stream::_TransferCallback, this);
	} else {
		_StageWireChunk(queuedBuffer, staleSlot);

		if (fUseImplicitFeedback || fUseExplicitFeedback) {
			// Assemble the outgoing transfer in the staging buffer: the
			// sub-packet remainder the previous transfer could not send,
			// followed by this buffer's wire data. Packets are cut purely by
			// the feedback schedule; a packet is never shortened to make the
			// transfer end at the buffer boundary, because a transfer of a
			// fixed number of frames over a fixed number of service intervals
			// would run at exactly the nominal rate no matter what the
			// feedback says. Whatever does not fill the schedule's last
			// packet is carried into the next transfer instead.
			uint32 stride = fWireSampleSize;
			uint8* scratch = fPlaybackScratch
				+ fPlaybackScratchStride * queuedBuffer;
			memcpy(scratch, fPlaybackCarry, fPlaybackCarryLength);
			memcpy(scratch + fPlaybackCarryLength, wireChunk, wireChunkSize);

			size_t availableBytes = fPlaybackCarryLength + wireChunkSize;
			size_t emitted = 0;
			size_t packetsCount = _FillPlaybackPackets(descriptors,
				availableBytes / stride, stride, emitted);

			size_t consumed = emitted * stride;
			fPlaybackCarryLength = availableBytes - consumed;
			memcpy(fPlaybackCarry, scratch + consumed, fPlaybackCarryLength);

			// The per-second "pb sched" diagnostic that measured this path lived
			// here (packets / bytes / carry / freqm / non-zero counts). It did
			// its job and is removed: freqm was exonerated at 8 ppm, the packet
			// count was never zero, and the non-zero counts moved to
			// _StageWireChunk, which runs on BOTH playback paths and so can be
			// compared against a no_feedback control. See
			// docs/NEXT-SESSION-usb-audio.md.
			//
			// It did leave one unfixed finding: fPlaybackCarryLength grows
			// monotonically (~one frame per second, never draining) against a
			// fixed 8 * wMaxPacketSize carry budget, so it overflows in roughly
			// 8 minutes of continuous playback. Measured, not yet fixed.
			status = gUSBModule->queue_isochronous(fStreamEndpoint,
				scratch, consumed, descriptors, packetsCount,
				&fStartingFrame, USB_ISO_ASAP,
				Stream::_TransferCallback, this);

			// A failed re-queue silently stalls the stream; that is worth a
			// (rare) blocking trace even from the completion path.
			if (status != B_OK) {
				TRACE(ERR, "pb queue buf %lu: count %lu bytes %lu carry %lu "
					"len0 %u status %#010x\n", queuedBuffer, packetsCount,
					consumed, fPlaybackCarryLength,
					(unsigned)descriptors[0].request_length, status);
			}
			return status;
		}

		TRACE(DTA, "buffers:%#010x[%#x]\ndescrs:%#010x[%#x]\n",
			wireChunk, wireChunkSize, descriptors, fPacketsPerBuffer);

		status = gUSBModule->queue_isochronous(fStreamEndpoint,
			wireChunk, wireChunkSize, descriptors, fPacketsPerBuffer,
			&fStartingFrame, isoFlags,
			Stream::_TransferCallback, this);
	}

	if (status != B_OK) {
		// A failed re-queue silently stalls the stream; that is worth a
		// (rare) blocking trace even from the completion path.
		TRACE(ERR, "%s queue_isochronous buf:%d ep:%#x failed:"
			" %#010x\n", fIsInput ? "input" : "output", (int)queuedBuffer,
			(unsigned int)fStreamEndpoint, (int)status);
		// Re-establish the chain from scratch on the next queue.
		fFrameChainValid = false;
	} else if (!highSpeed) {
		// fStartingFrame now holds the frame the stack actually used; place
		// the next buffer's packets in the frames immediately after this one.
		fNextStartFrame = fStartingFrame + fPacketsPerBuffer;
		fFrameChainValid = true;
	}

	TRACE(DTA, "frame:%#010x\n", fStartingFrame);
	return status;
}


void
Stream::_InitFeedbackParams(uint32 rate)
{
	// Nominal feedback value for a high-speed endpoint: audio frames per
	// microframe (fs / 8000) in 16.16 fixed point, rounded to nearest
	// (USB 2.0 spec 5.12.4.2). freqm is later clamped around this value.
	fNominalFreq = (uint32)((((uint64)rate << 10) + 62) / 125);
	fMaxFreq = fNominalFreq + fNominalFreq / 2;
	fMaxFrameSize = ((fMaxFreq << fDataInterval) + 0xffff) >> 16;

	atomic_set(&fCurrentFreq, (int32)fNominalFreq);
	fFeedbackPhase = 0;
	fFreqShift = kFreqShiftUnset;
}


bool
Stream::_ProbeVariableIsoOut(size_t stride)
{
	// Whether the host controller accepts an isochronous OUT transfer whose
	// packets differ in length. Rate feedback requires such transfers, but a
	// host controller driver may only implement uniformly-sized packets and
	// refuse anything else outright (B_BAD_VALUE) -- were that to happen
	// mid-stream, playback would silently stall. There is no capability query
	// in the USB module interface, so probe with a minimal two-packet transfer
	// of silence; the data endpoint is still idle here and the two short
	// packets are inaudible.
	// TODO: replace with a proper capability query if the USB stack grows one.
	if (stride == 0 || fMaxPacketSize < 2 * stride)
		return false;

	const size_t dataLength = 3 * stride;
	uint8* probe = new(std::nothrow) uint8[
		2 * sizeof(usb_iso_packet_descriptor) + dataLength];
	if (probe == NULL)
		return false;
	memset(probe, 0, 2 * sizeof(usb_iso_packet_descriptor) + dataLength);

	usb_iso_packet_descriptor* descriptors
		= reinterpret_cast<usb_iso_packet_descriptor*>(probe);
	descriptors[0].request_length = stride;
	descriptors[1].request_length = 2 * stride;

	status_t status = gUSBModule->queue_isochronous(fStreamEndpoint,
		probe + 2 * sizeof(usb_iso_packet_descriptor), dataLength,
		descriptors, 2, NULL, USB_ISO_ASAP, Stream::_ProbeCallback, probe);
	if (status != B_OK) {
		TRACE(ERR, "no variable-length isochronous OUT on this host "
			"controller (%#010x); playback runs at the nominal rate.\n",
			status);
		delete[] probe;
		return false;
	}
	return true;
}


void
Stream::_ProbeCallback(void* cookie, status_t status, void* data,
	size_t actualLength)
{
	// The probe or warmup transfer completed (or was cancelled); its buffer
	// is no longer referenced by the stack.
	delete[] static_cast<uint8*>(cookie);
}


void
Stream::_QueueWarmup()
{
	// Prime the isochronous schedule with a short run of silence ahead of the
	// first real buffer. The controller misses the first service interval(s)
	// while the endpoint spins up (and reports the ring underrun latched
	// since the capability probe drained the ring), which would otherwise
	// clip the start of the audible stream; this way both land in silence and
	// the real transfers queue behind with no gap. Best effort: on any
	// failure the stream simply starts as before.
	TypeIFormatDescriptor* format = static_cast<TypeIFormatDescriptor*>(
		fAlternates[fActiveAlternate]->Format());
	uint32 stride = format->fNumChannels * format->fSubframeSize;
	uint32 rate = fAlternates[fActiveAlternate]->GetSamplingRate();
	uint32 divisor = fDevice->fUSBVersion < 0x0200 ? 1000 : 8000;
	size_t packetSize = (size_t)rate * stride / divisor;
	packetSize -= packetSize % stride;	// whole audio frames per packet
	if (packetSize == 0 || packetSize > fMaxPacketSize)
		return;

	const uint32 kWarmupPackets = 16;
	size_t dataLength = kWarmupPackets * packetSize;
	uint8* warmup = new(std::nothrow) uint8[
		kWarmupPackets * sizeof(usb_iso_packet_descriptor) + dataLength];
	if (warmup == NULL)
		return;
	memset(warmup, 0,
		kWarmupPackets * sizeof(usb_iso_packet_descriptor) + dataLength);

	usb_iso_packet_descriptor* descriptors
		= reinterpret_cast<usb_iso_packet_descriptor*>(warmup);
	for (uint32 i = 0; i < kWarmupPackets; i++)
		descriptors[i].request_length = packetSize;

	status_t status = gUSBModule->queue_isochronous(fStreamEndpoint,
		warmup + kWarmupPackets * sizeof(usb_iso_packet_descriptor),
		dataLength, descriptors, kWarmupPackets, NULL, USB_ISO_ASAP,
		Stream::_ProbeCallback, warmup);
	if (status != B_OK)
		delete[] warmup;
}


size_t
Stream::_FillPlaybackPackets(usb_iso_packet_descriptor* descriptors,
	size_t frames, uint32 stride, size_t& emitted)
{
	uint32 freqm;
	if (fUseImplicitFeedback) {
		// The capture stream publishes its measured rate; use the nominal rate
		// until the first buffer has been measured. Clamp to a tight window
		// around nominal (~0.8%): a real crystal is within a few hundred ppm
		// of nominal, so anything outside is a measurement artifact -- and an
		// async sink rejects (mutes on) a stream far off its own rate, which
		// is worse than momentarily uncorrected drift.
		int32 measured = fDevice->Feedback();
		freqm = measured > 0 ? (uint32)measured : fNominalFreq;
		if (freqm < fNominalFreq - fNominalFreq / 128)
			freqm = fNominalFreq - fNominalFreq / 128;
		if (freqm > fNominalFreq + fNominalFreq / 128)
			freqm = fNominalFreq + fNominalFreq / 128;
	} else
		freqm = (uint32)atomic_get(&fCurrentFreq);


	emitted = 0;
	size_t count = 0;

	// Round high-speed transfers down to whole USB frames (8 microframes).
	// The EHCI links isochronous transfers with frame granularity: a
	// transfer ending mid-frame leaves its last iTD partially filled and the
	// next transfer starts on the next frame boundary, so every packet count
	// that is not a multiple of 8 drops up to 7 microframes of audio on the
	// wire - an audible rhythmic click (Hercules RMX2, 2026-07-06). The
	// snapshot taken at each whole-frame boundary lets the tail packets be
	// deferred: their frames stay staged and are carried into the next
	// transfer, so the emitted rate is unchanged. Mirrored (implicit
	// feedback) packets are exempt: their ring entries cannot be un-popped,
	// and the 1:1 interval replay matters more than the seam there.
	uint32 frameAlign = 1;
	if (fDevice->fUSBVersion >= 0x0200 && !fUseImplicitFeedback)
		frameAlign = 8 >> (fDataInterval > 3 ? 3 : fDataInterval);
	size_t alignedCount = 0;
	size_t alignedEmitted = 0;
	uint32 alignedPhase = fFeedbackPhase;

	// Cut whole packets off the staged frames. An implicit feedback sink
	// preferably replays the frame count of each packet the capture stream
	// delivered, 1:1 -- each ring entry stands for one service interval, so
	// playback then follows the device's clock frame-for-frame with no
	// estimation noise (some devices audibly glitch on anything less exact).
	// When no mirrored size is available (stream startup, capture hiccup,
	// explicit-feedback mode) a packet is sized from the rate accumulator
	// instead: fFeedbackPhase carries, in 16.16 frames, the difference
	// between the demand (freqm per interval) and what has been sent. In
	// both modes a packet is never shortened to land on the end of the
	// staged data -- the loop stops instead and the caller carries the
	// leftover frames into the next transfer, so the emitted rate is
	// preserved exactly across transfers. fPacketsPerBuffer is sized for the
	// worst (slowest) case, so this cannot overrun the descriptor array.
	while (count < fPacketsPerBuffer) {
		if (count % frameAlign == 0) {
			alignedCount = count;
			alignedEmitted = emitted;
			alignedPhase = fFeedbackPhase;
		}
		uint32 packetFrames;
		uint16 mirrored;
		// Mirroring maps ring entries to service intervals 1:1, so it is
		// only exact when both endpoints share the same interval; fall back
		// to the averaged rate otherwise.
		bool useMirrored = fUseImplicitFeedback
			&& fDevice->ImplicitSourceInterval() == fDataInterval
			&& fDevice->PeekFeedbackPacket(mirrored);
		uint32 demand = 0;
		if (useMirrored && mirrored == 0) {
			// The host missed that interval (errored capture packet), but the
			// device still consumed audio during it: emit a rate-law sized
			// packet in its place so the interval alignment is kept -- also,
			// an empty isochronous OUT packet cannot be queued on this stack.
			fDevice->PopFeedbackPacket();
			useMirrored = false;
		}
		if (useMirrored) {
			packetFrames = mirrored;
		} else {
			demand = fFeedbackPhase + (freqm << fDataInterval);
			packetFrames = demand >> 16;
		}
		if (packetFrames > fMaxFrameSize)
			packetFrames = fMaxFrameSize;
		// A packet must never exceed the endpoint's wMaxPacketSize.
		if (packetFrames * stride > fMaxPacketSize)
			packetFrames = fMaxPacketSize / stride;
		if (packetFrames == 0)
			packetFrames = 1;
		if (packetFrames > frames - emitted)
			break;
		if (useMirrored) {
			fDevice->PopFeedbackPacket();
		} else {
			fFeedbackPhase = demand > (packetFrames << 16)
				? demand - (packetFrames << 16) : 0;
		}

		descriptors[count].request_length = packetFrames * stride;
		descriptors[count].actual_length = 0;
		descriptors[count].status = B_OK;

		emitted += packetFrames;
		count++;
	}

	// Defer the sub-frame tail into the carry (see the frameAlign comment
	// above). A transfer shorter than one whole frame keeps its packets:
	// a one-off seam beats an empty queue.
	if (frameAlign > 1 && count % frameAlign != 0 && alignedCount > 0) {
		count = alignedCount;
		emitted = alignedEmitted;
		fFeedbackPhase = alignedPhase;
	}

	// Masked off by default: tracing from the completion path is blocking
	// file I/O on the host controller's completion thread and audibly
	// disrupts the stream (enable the DTA bit only for bench diagnosis).
	TRACE(DTA, "playback fb: freqm %u.%06u fr/uframe; %lu of %lu frames "
		"in %lu packets\n",
		freqm >> 16,
		(uint32)(((uint64)(freqm & 0xffff) * 1000000) >> 16),
		emitted, frames, count);

	return count;
}


status_t
Stream::_QueueFeedback()
{
	for (uint32 i = 0; i < kFeedbackPackets; i++) {
		fFeedbackDescriptors[i].request_length = fFeedbackPacketSize;
		fFeedbackDescriptors[i].actual_length = 0;
		fFeedbackDescriptors[i].status = B_OK;
	}

	// Queue several packets at once so the isochronous IN ring stays
	// continuously scheduled at the endpoint's service interval, matching how
	// the data endpoints are driven.
	return gUSBModule->queue_isochronous(fFeedbackEndpoint,
		fFeedbackBuffer, fFeedbackPacketSize * kFeedbackPackets,
		fFeedbackDescriptors, kFeedbackPackets,
		&fFeedbackFrame, USB_ISO_ASAP, Stream::_FeedbackCallback, this);
}


void
Stream::_FeedbackCallback(void* cookie, status_t status, void* data,
	size_t actualLength)
{
	Stream* stream = (Stream*)cookie;

	atomic_add(&stream->fInsideNotify, 1);
	if (status == B_CANCELED || stream->fDevice->fRemoved
			|| !stream->fIsRunning) {
		atomic_add(&stream->fInsideNotify, -1);
		return;
	}

	// Apply the most recent valid feedback packet in this transfer.
	for (uint32 i = 0; i < kFeedbackPackets; i++) {
		if (stream->fFeedbackDescriptors[i].status == B_OK
				&& stream->fFeedbackDescriptors[i].actual_length >= 3) {
			stream->_ProcessFeedback(
				stream->fFeedbackBuffer + i * stream->fFeedbackPacketSize,
				stream->fFeedbackDescriptors[i].actual_length);
		}
	}

	// Re-arm, and do NOT discard the result. This return value was ignored
	// entirely, so a feedback endpoint that stopped being polled because its
	// re-queue kept failing would look exactly like a device that had gone
	// quiet -- indistinguishable in the log, and the same class of defect as
	// D1 and D6: a path that re-arms itself needs its failures visible.
	// Rate-limited by time; a failing re-arm would otherwise flood at the
	// endpoint's service interval, which is what livelocked the machine once
	// already.
	status_t requeue = stream->_QueueFeedback();
	if (requeue != B_OK) {
		bigtime_t now = system_time();
		if (now - stream->fLastFeedbackErrorLog >= 1000000) {
			stream->fLastFeedbackErrorLog = now;
			TRACE(ERR, "%s feedback re-queue failed: %#010x --"
				" rate feedback has stopped arriving\n",
				stream->fIsInput ? "rec" : "pb", (unsigned int)requeue);
		}
	}

	atomic_add(&stream->fInsideNotify, -1);
}


void
Stream::_ProcessFeedback(const uint8* buffer, size_t length)
{
	uint32 value = buffer[0] | (buffer[1] << 8)
		| (buffer[2] << 16) | (buffer[3] << 24);

	// DIAG: log the first few raw feedback values of THIS RUN. What the device
	// reports reveals whether its audio engine is actually consuming
	// (~rate/8000 in 16.16 at high speed) or idle (0 / garbage). Startup only:
	// this callback is the stream's real-time path, and a periodic blocking
	// syslog write here audibly clicks the stream.
	//
	// ⚠️ The counter is PER RUN and PER STREAM, and the message says so. It used
	// to be a function-local `static int32` capped at 8 for the lifetime of the
	// driver image, which on 2026-08-12 was misread as device behaviour: the log
	// showing values #0..#7 and then nothing was taken as evidence that "the
	// feedback endpoint delivers exactly 8 values and is then never serviced
	// again", and a whole root-cause story was published on it. The endpoint had
	// not stopped; the LOGGING had. It was reported "reproduced 3 for 3", which
	// a hardcoded `if (n < 8)` guarantees and therefore cannot evidence.
	//
	// So: the cap is stated in the message, and it resets per run so a fresh run
	// always yields fresh data. If you need to know whether feedback is STILL
	// arriving later in a run, do not raise this cap -- count completions and
	// report from Stop() like _ReportRunHealth does, off the real-time path.
	if (fFeedbackLogCount < kFeedbackLogPerRun) {
		uint32 n = fFeedbackLogCount++;
		TRACE(ISO, "%s feedback raw %#010lx len %ld (#%lu of first"
			" %lu logged this run -- LOG CAP, not device behaviour)\n",
			fIsInput ? "rec" : "pb", (unsigned long)value, (long)length,
			(unsigned long)n, (unsigned long)kFeedbackLogPerRun);
	}

	// High-speed devices report a 4-byte 16.16 value, full-speed a 3-byte
	// 10.14 value; the remaining upper bits are reserved and masked off.
	if (length == 3)
		value &= 0x00ffffff;
	else
		value &= 0x0fffffff;

	if (value == 0)
		return;

	// Some devices place the value at the wrong bit position. Detect the
	// shift once by aligning the first sample near the nominal frequency,
	// assuming a deviation of no more than +50% / -25%.
	if (fFreqShift == kFreqShiftUnset) {
		int32 shift = 0;
		while (value < fNominalFreq - fNominalFreq / 4) {
			value <<= 1;
			shift++;
		}
		while (value > fNominalFreq + fNominalFreq / 2) {
			value >>= 1;
			shift--;
		}
		fFreqShift = shift;
	} else if (fFreqShift >= 0)
		value <<= fFreqShift;
	else
		value >>= -fFreqShift;

	// Only accept plausible values; otherwise re-detect the format next time.
	bool accepted = value >= fNominalFreq - fNominalFreq / 8
		&& value <= fMaxFreq;
	if (accepted)
		atomic_set(&fCurrentFreq, (int32)value);
	else
		fFreqShift = kFreqShiftUnset;
}


void
Stream::_PublishImplicitFeedback(size_t actualLength)
{
	// Convert the audio frames the device delivered over this buffer into an
	// average frames-per-(micro)frame value in 16.16 fixed point and hand it to
	// the playback stream. Both streams run off the device's single crystal, so
	// this is its true sampling rate. The buffer spanned fPacketsPerBuffer
	// service intervals (one packet each); actualLength is the true delivered
	// byte count summed from the per-packet iso descriptors by _RepackCapture.
	TypeIFormatDescriptor* format = static_cast<TypeIFormatDescriptor*>(
		fAlternates[fActiveAlternate]->Format());
	uint32 stride = format->fNumChannels * format->fSubframeSize;
	if (stride == 0 || fPacketsPerBuffer == 0)
		return;

	// A buffer missing more than one packet's worth of frames means service
	// intervals were missed (common while the endpoint spins up right after
	// start), not that the device's clock is slow. Folding such a buffer into
	// the average would drag the estimate far below the device's true rate --
	// enough for it to reject the stream -- so skip it.
	uint32 rate = fAlternates[fActiveAlternate]->GetSamplingRate();
	uint32 divisor = fDevice->fUSBVersion < 0x0200 ? 1000 : 8000;
	size_t frames = actualLength / stride;
	size_t nominalFrames = (size_t)rate * fPacketsPerBuffer / divisor;
	if (frames + fMaxPacketSize / stride < nominalFrames)
		return;

	// Accumulate the frames delivered against the (micro)frames they spanned.
	// The ratio is the device's true samples-per-(micro)frame; a single buffer
	// only resolves it to whole frames. Halve both totals every couple of
	// seconds so the average forgets old data: it then both converges quickly
	// after start and keeps tracking the (slowly wandering) crystal.
	fCaptureFramesTotal += frames;
	fCapturePacketsTotal += fPacketsPerBuffer;
	if (fCapturePacketsTotal >= 16384) {
		fCaptureFramesTotal >>= 1;
		fCapturePacketsTotal >>= 1;
	}

	int32 framesPerPacket
		= (int32)((fCaptureFramesTotal << 16) / fCapturePacketsTotal);
	fDevice->PublishFeedback(framesPerPacket);

	// Masked off by default: tracing from the completion path is blocking
	// file I/O on the host controller's completion thread and audibly
	// disrupts the stream (enable the DTA bit only for bench diagnosis).
	if (((fCapturePacketsTotal / fPacketsPerBuffer) & 63) == 0) {
		uint32 nominal = (uint32)((((uint64)rate << 16) + divisor / 2)
			/ divisor);
		TRACE(DTA, "implicit fb: measured %u.%06u fr/frame (nominal %u.%06u); "
			"this buffer %u fr over %u frames\n",
			framesPerPacket >> 16,
			(uint32)(((uint64)(framesPerPacket & 0xffff) * 1000000) >> 16),
			nominal >> 16,
			(uint32)(((uint64)(nominal & 0xffff) * 1000000) >> 16),
			(uint32)(actualLength / stride), (uint32)fPacketsPerBuffer);
	}
}


size_t
Stream::_RepackCapture(void* scratch)
{
	// The isochronous IN packets were received at wMaxPacketSize stride into the
	// scratch buffer; copy the frames the device actually delivered into the
	// contiguous media record buffer. The record buffer is a fixed size, so a
	// device delivering more than nominal is truncated here (its surplus is
	// still counted for the rate estimate) and one delivering less is
	// zero-padded.
	// Returns the true delivered byte count (the sum of the per-packet
	// actual_length values, each clamped to wMaxPacketSize). The caller uses
	// this for the implicit-feedback rate estimate: it must come from the
	// individual iso descriptors, not from the transfer's aggregate
	// actualLength, because the host controller can report the latter as the
	// sum of the requested (maximum) packet sizes rather than the bytes truly
	// received (xHCI fills in request_length for packets it did not
	// individually complete), which would double-count a lightly loaded stream.
	// TODO: carry the surplus/deficit across buffers for sample-accurate
	// capture; a fixed-size record buffer cannot express a drifting rate.
	TypeIFormatDescriptor* format = static_cast<TypeIFormatDescriptor*>(
		fAlternates[fActiveAlternate]->Format());
	uint32 stride = format->fNumChannels * format->fSubframeSize;

	if (fRecordScratch == NULL)
		return 0;

	if (stride == 0 || fMaxPacketSize == 0)
		return 0;

	size_t framesPerBuffer = fSamplesCount / fBufferCount;
	size_t bufferSize = framesPerBuffer * stride;
	size_t scratchStride = fPacketsPerBuffer * fMaxPacketSize;
	size_t index = ((uint8*)scratch - fRecordScratch) / scratchStride;
	if (index >= fBufferCount)
		return 0;

	usb_iso_packet_descriptor* descriptors
		= fDescriptors + index * fPacketsPerBuffer;
	uint8* source = fRecordScratch + index * scratchStride;
	// Repack contiguous wire-format frames into the wire chunk first; the
	// media conversion below writes the record buffer through the ORIGINAL
	// area VA (fBuffers) in this (completion) thread, the one view proven
	// coherent with the client's mapping (see _StageWireChunk).
	uint8* wireTarget = fWireBuffers + index * bufferSize;

	size_t delivered = 0;
	size_t filled = 0;
	for (size_t i = 0; i < fPacketsPerBuffer; i++) {
		size_t length = descriptors[i].actual_length;
		if (length > fMaxPacketSize)
			length = fMaxPacketSize;
		delivered += length;

		// Record every packet's frame count (0 for an errored packet) for
		// the playback stream to replay 1:1 -- each entry stands for one
		// service interval of the device's clock. Frame counts are channel-
		// agnostic, so a differing playback channel count is fine. If the
		// ring is full the entry is dropped; the playback stream falls back
		// to its averaged rate and re-locks once entries flow again.
		if (fIsFeedbackSource) {
			fDevice->PushFeedbackPacket(descriptors[i].status == B_OK
				? (uint16)(length / stride) : 0);
		}

		if (filled >= bufferSize)
			continue;
		if (filled + length > bufferSize)
			length = bufferSize - filled;
		if (length > 0)
			memcpy(wireTarget + filled, source + i * fMaxPacketSize, length);
		filled += length;
	}

	if (filled < bufferSize)
		memset(wireTarget + filled, 0, bufferSize - filled);

	uint8* mediaChunk = fBuffers + index * framesPerBuffer * fMediaSampleSize;
	if (fNeedsPacking) {
		_Unpack24(wireTarget, mediaChunk,
			framesPerBuffer * fStreamChannels);
	} else
		memcpy(mediaChunk, wireTarget, bufferSize);

	return delivered;
}


void
Stream::_TransferCallback(void* cookie, status_t status, void* data,
	size_t actualLength)
{
	Stream* stream = (Stream*)cookie;

	TRACE(status == B_OK ? DTA : ERR,
		"stream:%010x: status:%#010x, data:%#010x, len:%d\n",
		stream->fStreamEndpoint, status, data, actualLength);

	atomic_add(&stream->fInsideNotify, 1);
	if (status == B_CANCELED || stream->fDevice->fRemoved || !stream->fIsRunning) {
		TRACE(ERR, "Cancelled: c:%p st:%#010x, data:%#010x, len:%d\n",
			cookie, status, data, actualLength);
		atomic_add(&stream->fInsideNotify, -1);
		return;
	}

#if 0
	stream->_DumpDescriptors();
#endif

	// ISODIAG: runs before the error handling below, so a stream that is being
	// torn down by the transfer-error cap still reports what it saw on the way
	// out. No-op for playback. `data` identifies the completed buffer, so only
	// that transfer's descriptor slice is scanned.
	stream->_CheckCaptureOverrun(data);

	// Give up on a stream that is only producing errors. A device yanked mid
	// playback completes with B_DEV_FIFO_UNDERRUN / B_DEV_FIFO_OVERRUN, which
	// is neither B_CANCELED nor (yet) fRemoved -- the removal notification has
	// not been processed. The checks above therefore all pass, we re-queue, it
	// fails again, and the host controller logs a line per attempt: 24422 of
	// them in one capture, whose blocking log I/O starved the machine into a
	// livelock with no panic and no KDL
	// (captures/2026-08-11_beta6-rmx2-xhci-unplug-during-playback-error-flood.log).
	//
	// This is the third instance of one bug: any USB path that re-arms itself
	// on completion needs a bound on consecutive failures. usb_midi got one in
	// b5ac863 after the UX-256 wedge; Stream::Start() got one for the same
	// reason earlier today.
	//
	// This bound is defence in depth, not the cure. The flood is emitted by the
	// host controller, one line per errored PACKET, so it runs ~137 lines ahead
	// of every completion we see here -- a cap on completions can never be
	// tight enough on its own. The controller-side rate limit in
	// xhci.cpp:HandleTransferComplete is what actually stops the livelock.
	if (status != B_OK) {
		if (atomic_add(&stream->fConsecutiveErrors, 1) + 1
				>= kMaxConsecutiveTransferErrors) {
			// Stop the ring rather than this one buffer: fIsRunning is what the
			// other in-flight completions test, so clearing it retires them all
			// as they land instead of letting each re-arm in turn.
			stream->fIsRunning = false;
			// And park it, or the media node's next buffer exchange starts it
			// straight back up -- through set_alt_interface(), which destroys
			// endpoints this dead run still has transfers on, panicking in
			// Pipe::~Pipe(). See fErrorParked.
			atomic_set(&stream->fErrorParked, 1);
			TRACE(ERR, "%s stream stopped after %d consecutive transfer "
				"errors (last %#010x) -- the device is not completing "
				"transfers; not re-queueing. It will not be retried until the "
				"format changes or the device is replugged.\n",
				stream->fIsInput ? "input" : "output",
				(int)kMaxConsecutiveTransferErrors, (unsigned int)status);
			atomic_add(&stream->fInsideNotify, -1);
			return;
		}
	} else
		atomic_set(&stream->fConsecutiveErrors, 0);

	// Count completions arriving much later than one buffer duration: the
	// completion path stalled and the endpoint ring may have run dry, an
	// audible gap that leaves no error status behind. Counted here, reported
	// from Stop()/OnRemove() -- logging from this thread is blocking I/O that
	// would itself cause gaps.
	if (status != B_OK)
		stream->fErrorCount++;
	if (!stream->fIsInput) {
		bigtime_t now = system_time();
		uint32 rate = stream->fAlternates[stream->fActiveAlternate]
			->GetSamplingRate();
		if (rate > 0 && stream->fLastCompleteTime != 0) {
			bigtime_t expected = (bigtime_t)(stream->fSamplesCount
				/ stream->fBufferCount) * 1000000 / rate;
			bigtime_t delta = now - stream->fLastCompleteTime;
			if (delta > expected + expected / 2) {
				stream->fGapCount++;
				if (delta > stream->fMaxGap)
					stream->fMaxGap = delta;
			}
		}
		stream->fLastCompleteTime = now;
	}

	// Repack a completed capture buffer from the wMaxPacketSize-strided scratch
	// into the contiguous media record buffer, before it is handed to the media
	// server and before the delivered frame count is used to estimate the rate.
	// The repack returns the true delivered byte count (summed from the
	// per-packet iso descriptors); the transfer's aggregate actualLength is not
	// a reliable byte count for a capture stream (see _RepackCapture).
	size_t delivered = actualLength;
	if (stream->fIsInput) {
		// Every capture stream now receives into the scratch buffer at
		// wMaxPacketSize and repacks here, at every speed. The full-speed branch
		// this replaces converted the wire chunk in place, which only worked
		// because the packets had been sized to our own cadence -- the sizing
		// that truncates whatever the device actually sends (see _SetupBuffers).
		delivered = stream->_RepackCapture(data);
	}

	// A capture stream tagged as the implicit-feedback source paces the
	// asynchronous playback stream from its own delivery rate.
	if (stream->fIsFeedbackSource)
		stream->_PublishImplicitFeedback(delivered);

	int32 pending = atomic_add(&stream->fProcessedBuffers, 1) + 1;
	if (pending > (int32)stream->fBufferCount)
		TRACE(ERR, "Processed buffers overflow:%d\n", pending);
	// With EVERY buffer still unfetched, the buffer about to be requeued has not
	// been refilled by the media server -- stale audio goes out with no error
	// status. Counted; reported from Stop()/OnRemove().
	//
	// The threshold is `>= fBufferCount`, not `>= fBufferCount - 1`. "Nearly
	// every buffer outstanding" needs a margin to be meaningful, and at
	// fBufferCount == 2 -- which is what the full-speed path uses (see
	// _SetupBuffers) -- there is none: `pending` is `atomic_add(...) + 1`, so it
	// is >= 1 by construction and `>= 2 - 1` can NEVER be false. The old test
	// fired on every playback transfer.
	//
	// Measured on hardware 2026-08-27, DDJ-SR over OHCI, one ~38 min run:
	// "224307 media-late requeues" against "0 completion gaps (max 0 us)" and
	// "0 transfer errors" -- i.e. one per transfer, on a stream that was
	// audibly clean. A counter that fires unconditionally measures nothing.
	//
	// It also broke the report's silence-means-healthy property:
	// _ReportRunHealth() returns early only when all three counters are zero, so
	// a permanently non-zero one made the summary print on EVERY run and buried
	// the runs that actually deserved attention.
	// One expression, used for both the counter and the silencing below, so the
	// number in the run summary always describes exactly the slots that were
	// silenced -- they cannot drift apart later.
	bool staleSlot = !stream->fIsInput
		&& pending >= (int32)stream->fBufferCount;
	if (staleSlot)
		stream->fMediaLateCount++;
	// Update the (frames, real_time) pair together - see fFramesPlayed in
	// Stream.h. Frames first: ExchangeBuffer re-reads fRealTime to detect a
	// torn pair.
	stream->fFramesPlayed
		+= (bigtime_t)(stream->fSamplesCount / stream->fBufferCount);
	stream->fRealTime = system_time();

	release_sem_etc(stream->fDevice->fBuffersReadySem, 1, B_DO_NOT_RESCHEDULE);

	stream->fCurrentBuffer = (stream->fCurrentBuffer + 1)
		% stream->fBufferCount;
	status = stream->_QueueNextTransfer(stream->fCurrentBuffer, false,
		staleSlot);

	atomic_add(&stream->fInsideNotify, -1);
}


// ISODIAG (2026-08-14). Hypothesis under test: full-speed capture posts
// cadence-sized isochronous IN packets (_QueueNextTransfer, the "verified UHCI
// path"), but on an iso IN endpoint the DEVICE decides how many bytes to send.
// If it sends more than we predicted, the surplus has nowhere to go and xHCI
// reports "Isoch buffer overrun". High-speed capture is immune because it posts
// wMaxPacketSize into a scratch buffer for exactly this reason (_SetupBuffers).
//
// Confirmation is act_len > req_len on the capture stream. Refutation is a
// summary showing packets scanned with over=0 -- which is why the summary is
// emitted on a timer regardless of whether anything was found. An instrument
// that only speaks when it has news is indistinguishable from a dead one, and
// that has cost this project three sessions.
void
Stream::_CheckCaptureOverrun(void* scratch)
{
	if (!fIsInput || fDescriptors == NULL || fDescriptorsCount == 0)
		return;

	// Scan ONLY the descriptors belonging to the transfer that just completed.
	//
	// The first version of this scanned the whole fDescriptors array on every
	// callback. That double-counts: descriptors are re-examined on each
	// completion, so "pkts" became descriptor-scans rather than packets and the
	// derived percentages were array occupancy, not rates (measured 2026-08-14:
	// deltas of exactly +40 pkts per 2 callbacks x 20 descriptors). Worse, it
	// swept in descriptors for transfers still in flight, which carry B_NO_INIT
	// until they get an event -- indistinguishable from a genuine missed service
	// interval. Slicing to the completed transfer removes both problems.
	if (fRecordScratch == NULL || fPacketsPerBuffer == 0 || fMaxPacketSize == 0)
		return;
	size_t scratchStride = fPacketsPerBuffer * fMaxPacketSize;
	size_t index = ((uint8*)scratch - fRecordScratch) / scratchStride;
	if (index >= fBufferCount)
		return;
	usb_iso_packet_descriptor* slice = fDescriptors + index * fPacketsPerBuffer;

	// ⚠️ actual_length > request_length is NOT the signal, and testing for it
	// would produce a FALSE REFUTATION. xhci.cpp:2907 sets
	//   descriptor.actual_length = transferred
	// where transferred = TRB_2_BYTES_GET(...) - remainder, i.e. how much of OUR
	// buffer got filled. It is structurally incapable of exceeding
	// request_length. A device that sends more is TRUNCATED, and the surplus is
	// reported out-of-band as completion code 31 (COMP_ISOC_OVERRUN).
	//
	// So the fingerprint of the hypothesis is SATURATION, not excess:
	//   sat   -- actual == request (> 0): the packet exactly filled the buffer,
	//            which is what truncation looks like from in here.
	//   err   -- status != B_OK, EXCLUDING B_NO_INIT (see noev).
	//            COMP_ISOC_OVERRUN is absent from xhci_error_status(), so it
	//            falls to "default: B_DEV_STALLED" = 0x8000a015
	//            (B_DEVICE_ERROR_BASE + 21). Expect THAT value, not
	//            B_DEV_DATA_OVERRUN (+24, 0x8000a018) which is what it should be.
	//   noev  -- status == B_NO_INIT: the packet never received a completion
	//            event. Counted SEPARATELY from err because it is a different
	//            condition (missed service interval) with a different cause, and
	//            lumping the two in one counter is what made the post-fix reading
	//            uninterpretable on 2026-08-14.
	//
	// Reading: high sat + err=B_DEV_STALLED on a full-speed capture stream is the
	// truncation bug. sat near zero with actual consistently BELOW request means
	// the sizing is right and nothing is being lost.
	bool sawOverrun = false;
	for (size_t i = 0; i < fPacketsPerBuffer; i++) {
		// int16 in the API, and signed -- widen deliberately rather than
		// letting a negative promote into a huge unsigned value.
		int32 request = slice[i].request_length;
		int32 actual = slice[i].actual_length;
		status_t status = slice[i].status;

		fIsoPacketsSeen++;
		if (actual > (int32)fIsoMaxActual) {
			fIsoMaxActual = (uint32)actual;
			fIsoMaxActualReq = (uint32)(request > 0 ? request : 0);
		}
		if (request > 0 && actual == request)
			fIsoSaturated++;
		if (status == B_NO_INIT)
			fIsoNoEvent++;
		else if (status != B_OK) {
			fIsoErrors++;
			fIsoLastError = status;
			sawOverrun = true;
		}
	}

	// ⚠️ LOGGING FROM HERE IS AUDIBLE. This runs in the isochronous completion
	// callback; a TRACE() is a synchronous kernel log write on the very path
	// that has to re-queue the next transfer. When ISODIAG rode on the ERR bit
	// (always on) its 2-second summary produced a CLEARLY AUDIBLE GLITCH EVERY
	// 2 SECONDS on an RMX at 44.1k -- reported by ear 2026-08-15 and confirmed
	// by silencing it -- and worse, it delayed the re-queue enough to push the
	// frame chain behind on every occurrence: 3.5 xhci "iso chain fallback"
	// events per second, always at exactly "ahead 2043" (5 frames late). With
	// the logging off the fallback rate is ZERO and a 30 s tone renders
	// 1323000/1323000 exactly. The instrument was manufacturing the defect it
	// was measuring, and the unnatural constancy of that 2043 was the tell.
	//
	// So: the COUNTERS above stay unconditional (integer compares, no I/O) and
	// the LOGGING below is gated on the ISO trace bit, off unless the driver
	// settings say `trace 0x21`. ERR then stays usable for real errors without
	// dragging ISODIAG in with it -- which `trace 0` had forced us to lose.
	if ((gTraceMask & ISO) == 0)
		return;

	bigtime_t now = system_time();

	// Print what we compare AGAINST, not just the measurement: req is the
	// cadence we predicted, maxpkt is the headroom the endpoint advertises. The
	// BufferDuration()==0 bug was found only because a log line carried the
	// nominal value beside the measured one.
	if (sawOverrun && now - fLastIsoErrorLog >= 250000) {
		fLastIsoErrorLog = now;
		TRACE(ISO, "ISODIAG err ep:%#010x err:%u/%u last:%#010x sat:%u noev:%u "
			"maxact:%u (req %u) maxpkt:%u\n", (unsigned)fStreamEndpoint,
			(unsigned)fIsoErrors, (unsigned)fIsoPacketsSeen,
			(unsigned)fIsoLastError, (unsigned)fIsoSaturated,
			(unsigned)fIsoNoEvent, (unsigned)fIsoMaxActual,
			(unsigned)fIsoMaxActualReq, (unsigned)fMaxPacketSize);
	}

	// Emitted on a timer whether or not anything was found, so silence from
	// ISODIAG means the instrument is not running -- never "the stream is
	// clean". That distinction is the one this project keeps paying for.
	if (now - fLastIsoSummaryLog >= 2000000) {
		fLastIsoSummaryLog = now;
		TRACE(ISO, "ISODIAG summary ep:%#010x pkts:%u sat:%u err:%u noev:%u "
			"last:%#010x maxact:%u (req %u) maxpkt:%u hs:%u\n",
			(unsigned)fStreamEndpoint, (unsigned)fIsoPacketsSeen,
			(unsigned)fIsoSaturated, (unsigned)fIsoErrors,
			(unsigned)fIsoNoEvent, (unsigned)fIsoLastError,
			(unsigned)fIsoMaxActual, (unsigned)fIsoMaxActualReq,
			(unsigned)fMaxPacketSize,
			(unsigned)(fDevice->fUSBVersion >= 0x0200 ? 1 : 0));
	}
}


void
Stream::_DumpDescriptors()
{
	for (size_t i = 0; i < fDescriptorsCount; i++)
		TRACE(ISO, "%d:req_len:%d; act_len:%d; stat:%#010x\n", i,
			fDescriptors[i].request_length,	fDescriptors[i].actual_length,
			fDescriptors[i].status);
}


// Convert the mixer's 4-byte samples to the device's 3-byte packed 24-bit
// (playback). The media kit produces full-range 32-bit little-endian samples
// (B_AUDIO_INT) that are left-justified: the audio occupies the most
// significant 24 bits, so the device's 3-byte little-endian sample is the top
// three bytes (media [1..3]; byte [0] is the low-order remainder). Verified by
// ear: the signal is in the high bytes (low-byte selection yields only noise).
void
Stream::_Pack24(const uint8* media, uint8* wire, size_t subframes)
{
	for (size_t i = 0; i < subframes; i++) {
		wire[0] = media[1];
		wire[1] = media[2];
		wire[2] = media[3];
		media += 4;
		wire += 3;
	}
}


// Reverse of _Pack24 (record): place the device's 3-byte little-endian sample
// into the high 24 bits of a 32-bit little-endian value, zero the low byte.
void
Stream::_Unpack24(const uint8* wire, uint8* media, size_t subframes)
{
	for (size_t i = 0; i < subframes; i++) {
		media[0] = 0;
		media[1] = wire[0];
		media[2] = wire[1];
		media[3] = wire[2];
		media += 4;
		wire += 3;
	}
}


status_t
Stream::GetEnabledChannels(uint32& offset, multi_channel_enable* Enable)
{
	AudioChannelCluster* cluster = ChannelCluster();
	if (cluster == 0)
		return B_ERROR;

	for (size_t i = 0; i < cluster->ChannelsCount(); i++) {
		B_SET_CHANNEL(Enable->enable_bits, offset++, true);
		TRACE(INF, "Report channel %d as enabled.\n", offset);
	}

	return B_OK;
}


status_t
Stream::SetEnabledChannels(uint32& offset, multi_channel_enable* Enable)
{
	AudioChannelCluster* cluster = ChannelCluster();
	if (cluster == 0)
		return B_ERROR;

	for (size_t i = 0; i < cluster->ChannelsCount(); i++, offset++) {
		TRACE(INF, "%s channel %d.\n",
			(B_TEST_CHANNEL(Enable->enable_bits, offset)
			? "Enable" : "Disable"), offset + 1);
	}

	return B_OK;
}


status_t
Stream::GetGlobalFormat(multi_format_info* Format)
{
	_multi_format* format = fIsInput ? &Format->input : &Format->output;
	format->cvsr = fAlternates[fActiveAlternate]->GetSamplingRate();
	format->rate = fAlternates[fActiveAlternate]->GetSamplingRateId(0);
	format->format = fAlternates[fActiveAlternate]->GetFormatId();
	TRACE(INF, "%s.rate:%d cvsr:%f format:%#08x\n",
		fIsInput ? "input" : "ouput",
		format->rate, format->cvsr, format->format);
	return B_OK;
}


status_t
Stream::SetGlobalFormat(multi_format_info* Format)
{
	_multi_format* format = fIsInput ? &Format->input : &Format->output;
	AudioStreamAlternate* alternate = fAlternates[fActiveAlternate];
	// Skip reconfiguration only when nothing changed AND the buffers have
	// already been allocated. On the first call the requested format usually
	// already matches the alternate's default, but the sample buffers still
	// need to be set up (and the device's sampling rate programmed) before any
	// streaming can happen, so fall through while no area exists yet.
	if (fArea >= 0 && format->rate == alternate->GetSamplingRateId(0)
			&& format->format == alternate->GetFormatId()) {
		// Nothing changed for THIS stream, so leave it alone - including when
		// the call was made for the other stream's sake, which is the common
		// case: _MultiSetGlobalFormat hands the same ioctl to every stream, and
		// the media node changes the input and output rates in two separate
		// ioctls. Falling through here would Stop() and rebuild a running
		// playback stream every time the capture rate was touched.
		//
		// This guard used to also require that the current caller still owned
		// fArea, meaning to catch a client team that had died (media services
		// restart) and left the descriptors we hand out dangling. It did not do
		// that, and could not: create_area() called from kernel code allocates
		// in the KERNEL address space, not the calling team's
		// (VMAddressSpace::KernelID() - src/system/kernel/vm/vm.cpp,
		// __create_area_haiku), so info.team is always B_SYSTEM_TEAM and never
		// equals an ioctl caller's team. The comparison was vacuous, the early
		// return unreachable, and every redundant SET_GLOBAL_FORMAT tore down
		// and rebuilt all the buffer areas instead.
		//
		// It is also unnecessary. The client already knows these are kernel
		// areas and re-clones them unconditionally on every B_MULTI_GET_BUFFERS
		// (MultiAudioDevice::_RebaseBuffers scans B_SYSTEM_TEAM for the area
		// backing the address we return, deletes its previous clone and makes a
		// fresh one), and every set_global_format call site is followed by
		// _GetBuffers. A new client therefore gets its own mapping whether or
		// not we rebuild, so there is nothing here for a dead team to strand.
		TRACE(INF, "No changes required\n");
		return B_OK;
	}

	alternate->SetSamplingRateById(format->rate);
	alternate->SetFormatId(format->format);
	TRACE(INF, "%s.rate:%d cvsr:%f format:%#08x\n",
		fIsInput ? "input" : "ouput",
		format->rate, format->cvsr, format->format);

	// cancel data flow - it will be rewaked at next buffer exchange call
	Stop();

	// TODO: wait for cancelling?

	// layout of buffers should be adjusted after changing sampling rate/format
	status_t status = _SetupBuffers();

	if (status != B_OK)
		return status;

	return _SetDeviceSamplingRate();
}


status_t
Stream::_SetDeviceSamplingRate()
{
	uint32 samplingRate = fAlternates[fActiveAlternate]->GetSamplingRate();
	status_t status = B_OK;

	if (fControlInterface->SpecReleaseNumber() >= 0x200) {
		// R2: the rate is set with a Clock Source class request on the
		// AudioControl interface, not an endpoint request as in R1.
		uint8 clockId
			= fControlInterface->ClockSourceIdForTerminal(TerminalLink());
		if (clockId == 0) {
			TRACE(ERR, "No clock source for terminal %d.\n", TerminalLink());
			return B_ERROR;
		}

		// An externally clocked source (S/PDIF, ADAT) reports its rate
		// read-only: nothing we send can change it. Trying anyway earns a STALL
		// that fails the start, which would now park the stream -- so treat it
		// as "the device decides" and carry on at whatever it is running at.
		//
		// Incomplete on purpose: we should then GET CUR the clock and adopt the
		// rate it reports, rather than continuing to believe our own selection.
		// No device here has a read-only clock to test that against, and a
		// guess would be worse than the honest gap.
		if (!fControlInterface->ClockRateIsWritable(clockId)) {
			TRACE(ERR, "Clock %d is read-only; leaving the device's own rate "
				"in place instead of requesting %lu Hz.\n", clockId,
				(unsigned long)samplingRate);
			return B_OK;
		}

		status = fControlInterface->SetSamplingRate(clockId, samplingRate);
		TRACE(ERR, "set_speed %d for clock %d: %s\n",
			samplingRate, clockId, strerror(status));
		return status;
	}

	// R1: set endpoint speed
	size_t actualLength = 0;
	usb_audio_sampling_freq freq = _ASFormatDescriptor::GetSamFreq(samplingRate);
	uint8 address = fAlternates[fActiveAlternate]->Endpoint()->fEndpointAddress;

	status = gUSBModule->send_request(fDevice->fDevice,
		USB_REQTYPE_CLASS | USB_REQTYPE_ENDPOINT_OUT,
		USB_AUDIO_SET_CUR, USB_AUDIO_SAMPLING_FREQ_CONTROL << 8,
		address, sizeof(freq), &freq, &actualLength);

	TRACE(ERR, "set_speed %02x%02x%02x for ep %#x %d: %s\n",
		freq.bytes[0], freq.bytes[1], freq.bytes[2],
		address, actualLength, strerror(status));

	// Ask the device what rate it believes it is running. A UAC1 endpoint may
	// ACK a SET_CUR it did not honour, which is indistinguishable from success
	// at the host, and streaming to a device whose clock never moved produces
	// silence or garbage over a provably healthy wire.
	//
	// A settling delay between the SET_CUR and this read was tried and made no
	// difference on the DN-HC4500 (2026-07-31), so the read follows immediately.
	{
		usb_audio_sampling_freq current;
		memset(&current, 0, sizeof(current));
		size_t readLength = 0;
		status_t readStatus = gUSBModule->send_request(fDevice->fDevice,
			USB_REQTYPE_CLASS | USB_REQTYPE_ENDPOINT_IN,
			USB_AUDIO_GET_CUR, USB_AUDIO_SAMPLING_FREQ_CONTROL << 8,
			address, sizeof(current), &current, &readLength);
		uint32 reported = current.bytes[0] | (current.bytes[1] << 8)
			| (current.bytes[2] << 16);
		// Log the RAW bytes as well as the decoded value: a decoded number
		// alone cannot distinguish "the device sent shifted bytes" from "we
		// decoded good bytes wrongly", and that distinction has been argued
		// more than once. 96000 is sent as 00 77 01; a device echoing
		// 77 01 00 is shifting on the wire.
		TRACE(INF, "%s set_speed %lu -> device reports %lu Hz"
			" (raw %02x %02x %02x, %lu bytes, %s)\n", fIsInput ? "rec" : "pb",
			(unsigned long)samplingRate, (unsigned long)reported,
			current.bytes[0], current.bytes[1], current.bytes[2],
			(unsigned long)readLength, strerror(readStatus));

		// Refuse a format the device demonstrably did not take - but only when
		// the readback is actually saying something. Three conditions:
		//
		//  - the request succeeded and returned the full value. A device that
		//    does not implement GET_CUR here (or answers short) says nothing
		//    about whether the SET_CUR took, and must keep working as before.
		//  - the reported rate differs from the one we programmed.
		//  - the reported rate is one this alternate ADVERTISES. This is the
		//    load-bearing test. The DN-HC4500 answers with our own bytes
		//    shifted one position whenever it is not rate-locked: 96000
		//    (00 77 01) comes back as 375, 44100 (44 ac 00) as 172. Neither is
		//    a rate it offers. Treating any mismatch as a refusal made this
		//    driver reject EVERY format on that device, so it would not play at
		//    all - a regression introduced when this check was added.
		//
		// A device reporting a real, supported, different rate ("you asked for
		// 96000, I am at 48000") is still caught, which is the case worth
		// catching: the media kit would otherwise stream into a wrong clock
		// with healthy counters and no error anywhere.
		if (readStatus == B_OK && readLength == sizeof(current)
				&& reported != samplingRate && _RateIsAdvertised(reported)) {
			if (gRejectUnverifiedRate) {
				TRACE(ERR, "Device refused %lu Hz (reports %lu Hz, which it "
					"does advertise); rejecting the format.\n",
					(unsigned long)samplingRate, (unsigned long)reported);
				return B_DEV_CONFIGURATION_ERROR;
			}
			TRACE(ERR, "Device did not confirm %lu Hz (reports %lu Hz); "
				"streaming anyway (reject_unverified_rate off).\n",
				(unsigned long)samplingRate, (unsigned long)reported);
		} else if (readStatus == B_OK && readLength == sizeof(current)
				&& reported != samplingRate) {
			// Name the one misbehaviour we understand. The DN-HC4500 returns
			// the three bytes we sent, shifted down one position, so the
			// decoded value is exactly the request >> 8 (96000 -> 375,
			// 44100 -> 172). That proves the device RECEIVED our value and
			// only its GET_CUR read path is wrong - it plays correctly while
			// reporting this. It says nothing about the clock it is actually
			// running, so there is nothing to correct; it is worth
			// distinguishing only so an unrecognised readback stands out as
			// something we have never seen before.
			if (reported == (samplingRate >> 8)) {
				TRACE(ERR, "Readback %lu Hz for %lu Hz is our own bytes shifted "
					"one position - known DN-HC4500 GET_CUR behaviour, "
					"proceeding.\n", (unsigned long)reported,
					(unsigned long)samplingRate);
			} else {
				TRACE(ERR, "Unrecognised readback %lu Hz for %lu Hz - not an "
					"advertised rate and not a byte-shifted echo, so GET_CUR is "
					"untrustworthy here; proceeding.\n",
					(unsigned long)reported, (unsigned long)samplingRate);
			}
		}
	}

	// The R2 path re-selects the alternate after programming the clock, which
	// is what finally started the RMX2's engine (see _ArmClockAndActivate).
	// Some UAC1 devices likewise only latch a new sampling frequency when the
	// streaming interface is re-selected afterwards. Off by default: this is
	// an extra SET_INTERFACE on every format change, and the full-speed
	// devices are verified working without it.
	if (gRearmAlternate && fUSBConfig != NULL) {
		usb_interface_info* park = &fUSBConfig->interface[fInterface].alt[0];
		usb_interface_info* active
			= &fUSBConfig->interface[fInterface].alt[fActiveAlternate];
		status_t rearm = gUSBModule->set_alt_interface(fDevice->fDevice, park);
		if (rearm == B_OK)
			rearm = gUSBModule->set_alt_interface(fDevice->fDevice, active);
		TRACE(INF, "%s re-armed alternate %u after rate %lu: %s\n",
			fIsInput ? "rec" : "pb", (unsigned)fActiveAlternate,
			(unsigned long)samplingRate, strerror(rearm));
		// The handles are invalidated by every set_alt_interface.
		if (rearm == B_OK)
			_ResolveEndpoints(active);
	}

	return status;
}


status_t
Stream::GetBuffers(multi_buffer_list* List)
{
	if (fAreaSize == 0)
		return B_NO_INIT;

	// Apply the consumer's requested buffer geometry (e.g. a low-latency
	// client asking for small buffers). A frame size of 0 selects the driver
	// default, an out-of-range buffer count keeps the default count; the
	// frame count is bounded further by _SetupBuffers(). Only a changed
	// request reallocates the buffers. Full-speed devices keep their fixed,
	// verified geometry (_SetupBuffers forces it), so requests are ignored
	// there - honoring them would reallocate the areas on every call and
	// dangle the client's clones.
	if (fDevice->fUSBVersion >= 0x0200) {
		int32 requestBuffers = fIsInput
			? List->request_record_buffers : List->request_playback_buffers;
		uint32 requestFrames = fIsInput
			? List->request_record_buffer_size
			: List->request_playback_buffer_size;

		uint32 count = kSamplesBufferCount;
		if (requestBuffers >= 2 && requestBuffers <= (int32)kSamplesBufferCount)
			count = requestBuffers;
		// same horizon cap _SetupBuffers applies, so the comparison below
		// does not re-trigger setup on every call
		if (count > kMaxHighSpeedBuffers)
			count = kMaxHighSpeedBuffers;

		if (count != fBufferCount || requestFrames != fRequestedFrames) {
			fBufferCount = count;
			fRequestedFrames = requestFrames;
			status_t status = _SetupBuffers();
			if (status != B_OK)
				return status;
		}
	}

	int32 startChannel = List->return_playback_channels;
	buffer_desc** Buffers = List->playback_buffers;

	if (fIsInput) {
		List->flags |= B_MULTI_BUFFER_RECORD;
		List->return_record_buffer_size = fSamplesCount / fBufferCount;
		List->return_record_buffers = fBufferCount;
		startChannel = List->return_record_channels;
		Buffers = List->record_buffers;

		TRACE(DTA, "flags:%#10x\nreturn_record_buffer_size:%#010x\n"
			"return_record_buffers:%#010x\n", List->flags,
			List->return_record_buffer_size, List->return_record_buffers);
	} else {
		List->flags |= B_MULTI_BUFFER_PLAYBACK;
		List->return_playback_buffer_size = fSamplesCount / fBufferCount;
		List->return_playback_buffers = fBufferCount;

		TRACE(DTA, "flags:%#10x\nreturn_playback_buffer_size:%#010x\n"
			"return_playback_buffers:%#010x\n", List->flags,
			List->return_playback_buffer_size, List->return_playback_buffers);
	}

	TypeIFormatDescriptor* format = static_cast<TypeIFormatDescriptor*>(
		fAlternates[fActiveAlternate]->Format());

	// The Haiku system mixer only binds to a stereo output, so present a stereo
	// pair to the media node for playback (input/capture keeps all channels).
	// This MUST match the output channel count _MultiGetDescription advertises,
	// or the media node's _WriteZeros() walks past the buffer list and crashes
	// media_addon_server. The USB wire still carries all fNumChannels; which
	// physical pair this stereo lands on is chosen by the wire staging's
	// channel mirror.
	size_t numReport = (fIsInput || format->fNumChannels <= 2)
		? (size_t)format->fNumChannels : 2;

	// [buffer][channel] init buffers
	for (size_t buffer = 0; buffer < fBufferCount; buffer++) {
		TRACE(DTA, "%s buffer #%d:\n", fIsInput ? "input" : "output", buffer + 1);

		// The mixer writes media-sized samples (4 bytes each when we are
		// packing to 3-byte 24-bit on the wire), so the buffer layout uses
		// fMediaSampleSize / mediaSubframe, not the wire subframe size.
		uint8 mediaSubframe = fMediaSampleSize / format->fNumChannels;
		struct buffer_desc descs[format->fNumChannels];
		for (size_t channel = startChannel;
				channel < startChannel + numReport; channel++) {
			// init stride to the same for all buffers
			uint32 stride = fMediaSampleSize;
			descs[channel].stride = stride;

			size_t bufferSize = (fSamplesCount / fBufferCount) * stride;
			descs[channel].base = (char*)fBuffers;
			descs[channel].base += buffer * bufferSize;
			descs[channel].base += channel * mediaSubframe;

			TRACE(DTA, "%d:%d: base:%#010x; stride:%#010x\n", buffer, channel,
				descs[channel].base, descs[channel].stride);
		}
		if (!IS_USER_ADDRESS(Buffers[buffer])
				|| user_memcpy(Buffers[buffer], descs,
					sizeof(buffer_desc) * numReport) < B_OK) {
			return B_BAD_ADDRESS;
		}
	}

	if (fIsInput) {
		List->return_record_channels += format->fNumChannels;
		TRACE(MIX, "return_record_channels:%#010x\n",
			List->return_record_channels);
	} else {
		List->return_playback_channels += numReport;
		TRACE(MIX, "return_playback_channels:%#010x\n",
			List->return_playback_channels);
	}

	return B_OK;
}


bool
Stream::ExchangeBuffer(multi_buffer_info* Info)
{
	if (atomic_get(&fProcessedBuffers) <= 0)
		return false;

	// Read the (real_time, frames) pair the callback publishes without
	// tearing: single writer, so re-reading fRealTime is enough. Reported as
	// absolute values (both taken at the same callback instant) instead of
	// timestamp-now/increment-later, which skewed the pair by up to one
	// buffer and made the node's drift estimate jitter (audible zero-fill
	// bursts).
	bigtime_t realTime, framesCount;
	do {
		realTime = fRealTime;
		framesCount = fFramesPlayed;
	} while (realTime != fRealTime);

	// Report the cycle by walking one completion per exchange instead of
	// reading fCurrentBuffer: the callback releases the wake semaphore before
	// advancing fCurrentBuffer (and a late node can let two completions
	// accumulate), so reading the callback's counter here yields the SAME
	// cycle on consecutive exchanges. The node's duplicate filter then skips
	// a refill and that ring slot replays last rotation's audio - the madiag
	// "cycle-jump" glitch, observed at up to ~1/s. Exchanges consume pending
	// completions exactly 1:1, so this single-caller counter is the true
	// completion sequence, race-free by construction.
	fLastReportedCycle = (fLastReportedCycle + 1) % (int32)fBufferCount;

	if (fIsInput) {
		Info->recorded_real_time = realTime;
		Info->recorded_frames_count = framesCount;
		Info->record_buffer_cycle = fLastReportedCycle;
	} else {
		Info->played_real_time = realTime;
		Info->played_frames_count = framesCount;
		Info->playback_buffer_cycle = fLastReportedCycle;
	}

	atomic_add(&fProcessedBuffers, -1);

	return true;
}
