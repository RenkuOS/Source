/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Distributed under the terms of the MIT license.
 *
 */
#ifndef _USB_AUDIO_STREAM_H_
#define _USB_AUDIO_STREAM_H_


#include "AudioStreamingInterface.h"


class Device;

class Stream : public AudioStreamingInterface {
	friend	class			Device;
public:
							Stream(Device* device, size_t interface,
								usb_interface_list* List);
							~Stream();

			status_t		Init();
			status_t		InitCheck() { return fStatus; }

			status_t		Start();
			status_t		Stop();
			bool			IsRunning() { return fIsRunning; }
			// A stream whose current geometry the USB stack cannot satisfy
			// fails Start() identically every time. The media node retries on
			// every buffer exchange, so the driver must stop trying by itself
			// or it spins for as long as the device stays plugged in. Parked
			// streams are skipped; the other streams keep running.
			bool			IsStartParked()
								{ return fStartFailures >= kMaxStartFailures
									|| atomic_get(&fErrorParked) != 0; }
			void			OnRemove();

			// Rate-id queries used when streams share one clock and their
			// requested rates have to be reconciled (Device::_SharedClockId).
			// HasRateId asks "is this already what I am set to", i.e. did the
			// caller leave my half of multi_format_info unchanged;
			// SupportsRateId asks "could I be set to this at all".
			bool			HasRateId(uint32 rateId);
			bool			SupportsRateId(uint32 rateId);

			status_t		GetBuffers(multi_buffer_list* List);

			status_t		OnSetConfiguration(usb_device device,
							const usb_configuration_info* config);
			status_t		OnReattach(usb_device device,
							const usb_configuration_info* config);

			bool			ExchangeBuffer(multi_buffer_info* Info);
			void			SetClientBuffer(uint8* base)
								{ fClientBuffers = base; }
			status_t		GetEnabledChannels(uint32& offset,
								multi_channel_enable* Enable);
			status_t		SetEnabledChannels(uint32& offset,
								multi_channel_enable* Enable);
			status_t		GetGlobalFormat(multi_format_info* Format);
			status_t		SetGlobalFormat(multi_format_info* Format);

protected:
	// Consecutive Start() failures tolerated before a stream is parked. Each
	// attempt costs a control transfer, a fresh set of endpoints and four
	// buffer allocations, so this is deliberately far smaller than usb_midi's
	// 100-error cap (b5ac863), whose retries were single cheap completions.
	// Five rides out a transient without letting a permanent failure run away.
	static const uint32		kMaxStartFailures = 5;

	// Consecutive errored completions before a running stream gives up and
	// stops re-queueing.
	//
	// NOT usb_midi's 100 (b5ac863), despite being the same class of bug: one
	// completion here is a whole isochronous buffer, and the host controller
	// reports an error PER PACKET within it -- measured at ~137 controller log
	// lines per completion. 100 completions would license over ten thousand.
	// usb_midi's retries were single cheap bulk transfers, so its number does
	// not transfer. Ten is enough to ride out a transient and cheap to reach.
	static const int32		kMaxConsecutiveTransferErrors = 10;

			Device*			fDevice;
			status_t		fStatus;

			// Consecutive Start() failures; see IsStartParked(). Reset by a
			// successful start, by _SetupBuffers() (the geometry that failed
			// has been replaced) and by OnReattach() (a replug earns a fresh
			// attempt). Only genuine failures count -- not "already running",
			// not "device removed", not "buffers not configured yet".
			uint32			fStartFailures;

			// Errored completions since the last good one, for the guard in
			// _TransferCallback. Written only from the completion thread, but
			// atomically: the exchange side reads nothing here, and a torn
			// count would defeat the bound it exists to enforce.
			int32			fConsecutiveErrors;

			// Set when kMaxConsecutiveTransferErrors stops a running stream, so
			// IsStartParked() reports it and the next buffer exchange does not
			// simply start it again. Kept separate from fStartFailures because
			// the two mean different things and are logged differently: that
			// counts starts that never got going, this records a run that died.
			//
			// Without it the cap only cleared fIsRunning, and the media node's
			// very next exchange called Start() -> _ArmClockAndActivate() ->
			// set_alt_interface(), whose Device::ClearEndpoints() destroys the
			// endpoints while the failed run's transfers are still outstanding.
			// Pipe::~Pipe() then panics "USB object did not become idle!" (it
			// waits 2 s for the reference count to drop and gives up). So a
			// stream that merely sounded broken took the whole machine down;
			// captured in
			// captures/2026-08-12_beta6-rmx2-xhci-underrun-then-waitforidle-KDL.log.
			//
			// Which references were still held is NOT established -- do not
			// assume the neighbouring "TRB ... was not found in the endpoint!"
			// explains it. That line fires on every xHCI teardown including
			// clean ones and is ruled out as a cause in docs/UPSTREAM.md row 16.
			//
			// Written from the completion thread, read from the ioctl thread,
			// hence atomic. Cleared by the same two events that clear
			// fStartFailures -- a new geometry (_SetupBuffers) and a replug
			// (OnReattach) -- which are exactly the user-visible recoveries the
			// park message promises.
			int32			fErrorParked;

			// Configuration tree of the (re)attached device, stored by
			// OnSetConfiguration so the R2 stream-start sequence can toggle
			// this stream's alternate settings (see _ArmClockAndActivate).
			// Owned by the USB stack; same lifetime as the endpoint handles.
			const usb_configuration_info* fUSBConfig;

			usb_pipe		fStreamEndpoint;

			bool			fIsRunning;
			uint16			fMaxPacketSize;
			area_id			fArea, fKernelArea;
			size_t			fAreaSize;

			// Capture reception scratch. Isochronous IN packets are received at
			// full wMaxPacketSize stride into this kernel-only buffer, so a
			// device running slightly fast can burst above the nominal packet
			// size without overrunning the media buffer; the completion callback
			// repacks the delivered frames contiguously into the record buffer.
			// This also lets the capture stream measure the device's true rate
			// (implicit feedback). Output streams do not use it.
			area_id			fRecordScratchArea;
			uint8*			fRecordScratch;

			// Staging for a feedback-paced playback stream. Each outgoing
			// transfer is assembled here as the sub-packet remainder of the
			// previous transfer followed by the media buffer, so packets can
			// be cut purely by the feedback schedule instead of being rounded
			// to the media buffer boundary (which would pin the stream to the
			// nominal rate). The carry-over is always smaller than one packet.
			area_id			fPlaybackScratchArea;
			uint8*			fPlaybackScratch;
			size_t			fPlaybackScratchStride;
			uint8*			fPlaybackCarry;
			size_t			fPlaybackCarryLength;
			usb_iso_packet_descriptor* fDescriptors;
			size_t			fDescriptorsCount;
			uint8*			fBuffers;
			uint8*			fKernelBuffers;
			size_t			fCurrentBuffer;
			size_t			fSamplesCount;

			// Packed-24-bit support: the device streams 3-byte samples on the
			// wire (bSubframeSize == 3), but the media kit has no 3-byte
			// sample type and hands us 4-byte samples (B_AUDIO_INT). When
			// fNeedsPacking is set we lay the mixer buffer out for 4-byte
			// samples and convert 4<->3 in place per transfer. For other
			// formats (16/32-bit) fNeedsPacking is false and nothing changes.
			bool			fNeedsPacking;
			uint32			fMediaSampleSize;	// bytes per frame, mixer side
			uint32			fWireSampleSize;	// bytes per frame, USB wire
			uint16			fStreamChannels;
			// Separate B_CONTIGUOUS kernel-only buffer holding the wire-format
			// data handed to queue_isochronous. The wire chunk is (re)built at
			// queue time in the completion thread from the ORIGINAL mixer area
			// VA (fBuffers): cross-team clone writes are not reliably visible
			// across contexts on x86_64 hrev59811 (see _SetupBuffers).
			area_id			fWireArea;
			uint8*			fWireBuffers;
			// client-team address of its clone of fArea (informational; the
			// client writes/reads the buffer pages through that clone)
			uint8*			fClientBuffers;
			// 44.1kHz cadence (full-speed, non-feedback): every Nth iso packet
			// carries one extra audio frame so the average rate is exact
			// (0 = integer rate, uniform packets)
			uint32			fExtraFramePeriod;

			// Buffer geometry requested by the consumer via
			// B_MULTI_GET_BUFFERS (e.g. small buffers for a low-latency
			// client). A zero frame request selects the kSamplesBufferSize
			// default; the raw request is remembered so repeating it does
			// not reallocate the buffers.
			uint32			fBufferCount;
			uint32			fRequestedFrames;

			bigtime_t		fRealTime;
			// Cumulative frames the device has consumed/produced, updated in
			// _TransferCallback TOGETHER with fRealTime: the media node feeds
			// (real_time, frames) pairs into its TimeComputer, and pairing a
			// callback timestamp with an exchange-time frame count skews the
			// pair by up to one buffer (10ms) - enough drift-estimate jitter
			// to make the mixer's buffers get judged late and zero-filled in
			// audible bursts. hda pairs both in its IRQ handler for the same
			// reason. Never reset while the stream object lives: the node
			// expects the count to be monotonic across stream restarts.
			bigtime_t		fFramesPlayed;
			// Completion time of the previous transfer, for the gap detector
			// below. Not part of the media node's timing.
			bigtime_t		fLastCompleteTime;

			// Completion-health counters for the run, collected in the
			// transfer callback and reported once from Stop() and OnRemove().
			// Counted rather than logged as they happen: writing the log from
			// the host controller's completion thread is blocking file I/O
			// that itself causes the gaps it would be reporting.
			//
			// These are permanent telemetry, not scaffolding. They are how a
			// degraded stream is told apart from a broken one -- a device that
			// plays but sounds wrong leaves no error status anywhere else, and
			// the summary line has now been the first hard evidence in two
			// separate investigations (the removal hard hang, and the RMX2
			// capture-start freeze of 2026-08-11). One line per run, only when
			// something actually went wrong.
			uint32			fGapCount;
			bigtime_t		fMaxGap;
			uint32			fMediaLateCount;
			uint32			fErrorCount;

			uint32			fStartingFrame;
			// Chain isochronous transfers back-to-back on full-speed (UHCI)
			// devices: the next buffer's packets go in the frames immediately
			// after the previous buffer, one packet per USB frame, so the
			// endpoint runs at real time instead of the stack stacking every
			// buffer into the same frames. Unused on high-speed devices.
			uint32			fNextStartFrame;
			bool			fFrameChainValid;
			int32			fProcessedBuffers;
			// Cycle value ExchangeBuffer reported last. Owned by the (single)
			// exchange caller; advances one step per consumed completion, so
			// it tracks the completion sequence exactly without racing the
			// callback's fCurrentBuffer updates. Invariant:
			// fLastReportedCycle + fProcessedBuffers == fCurrentBuffer (mod
			// fBufferCount).
			int32			fLastReportedCycle;
			int32			fInsideNotify;

			// Asynchronous feedback for a UAC2 async playback endpoint. The
			// device reports its true sampling rate so we can resize the
			// outgoing packets to match and keep its FIFO from under/overrunning.
			// Two sources are supported: an explicit isochronous feedback IN
			// endpoint (16.16 samples per microframe), or -- when the device
			// tags its capture endpoint with the "implicit feedback" usage type
			// -- the capture stream's own measured delivery rate. Playback and
			// capture share one clock, so the latter is exact and is preferred
			// (many devices, e.g. the Behringer UMC2xxHD, advertise an explicit
			// feedback endpoint their firmware never actually services).
			static const uint32 kFeedbackPackets = 8;

			// Raw feedback values logged per RUN (see _ProcessFeedback). Kept
			// small because that callback is the real-time path. Deliberately
			// NOT equal to kFeedbackPackets: when the two matched, one
			// transfer's worth of packets exactly exhausted the log budget, so
			// "the log stopped" and "one transfer completed" were
			// indistinguishable -- which is how a log cap got published as
			// device behaviour on 2026-08-12.
			static const uint32 kFeedbackLogPerRun = 12;

			bool			fDataEndpointIsAsync;
			bool			fIsFeedbackSource;
			bool			fUseImplicitFeedback;
			bool			fUseExplicitFeedback;

			// Running totals used by a capture feedback source to derive the
			// device's true rate. Per-buffer frame counts are integers and so
			// quantize the rate too coarsely to see crystal drift; accumulating
			// across buffers recovers the fractional part. Halved periodically
			// so the average stays responsive and the counters stay bounded.
			uint64			fCaptureFramesTotal;
			uint64			fCapturePacketsTotal;

			usb_pipe		fFeedbackEndpoint;
			area_id			fFeedbackArea;
			uint8*			fFeedbackBuffer;
			usb_iso_packet_descriptor fFeedbackDescriptors[kFeedbackPackets];
			uint16			fFeedbackPacketSize;
			uint32			fFeedbackFrame;
			size_t			fPacketsPerBuffer;
			// Raw feedback values logged so far in the current run; reset by
			// _Start() alongside the other per-run counters.
			uint32			fFeedbackLogCount;



			// Same, for the _StageWireChunk media-buffer check, which runs on
			// both playback paths.
			bigtime_t		fLastStageLog;


			// Time-based rate limit for the feedback re-queue failure log. A
			// failing re-arm fires at the endpoint's service interval, so this
			// must be bounded -- an unthrottled log on an isochronous path is
			// what livelocked the machine before (see xhci.cpp D6).
			bigtime_t		fLastFeedbackErrorLog;

			// ISODIAG (2026-08-14): does a capture device deliver more bytes in
			// an isochronous IN packet than the request_length we posted?
			//
			// Full-speed capture posts cadence-sized packets straight into the
			// wire chunk (_QueueNextTransfer, "verified UHCI path"), where
			// high-speed capture posts wMaxPacketSize into a scratch buffer
			// precisely so a device clocked fast can burst without overrunning
			// (_SetupBuffers:775). If the device out-runs our predicted cadence
			// the surplus has nowhere to go, and xHCI reports "Isoch buffer
			// overrun" -- which is what the RMX does on the Renesas card while
			// running clean on UHCI.
			//
			// Counters are unconditional (integer compares only); the LOGGING is
			// rate-limited by TIME, never by count, and a summary is emitted on a
			// timer whether or not an overrun was seen -- so silence from this
			// instrument means it is not running, not that the stream is clean.
			// uint32, and every print casts to (unsigned) with %u: uint32 is
			// "unsigned int" on x86_64 but "unsigned long" on x86_gcc2, and a
			// %lu/%u mismatch corrupts the vararg list rather than just
			// misprinting. At ~2000 descriptors/s these last 24 days.
			uint32			fIsoPacketsSeen;
			uint32			fIsoSaturated;
			uint32			fIsoErrors;
			// B_NO_INIT (no completion event -- a missed service interval) is
			// counted apart from real errors. Merging them made the post-fix
			// reading uninterpretable on 2026-08-14.
			uint32			fIsoNoEvent;
			status_t		fIsoLastError;
			uint32			fIsoMaxActual;
			uint32			fIsoMaxActualReq;
			bigtime_t		fLastIsoErrorLog;
			bigtime_t		fLastIsoSummaryLog;

			uint32			fNominalFreq;
			uint32			fMaxFreq;
			uint32			fMaxFrameSize;
			uint8			fDataInterval;
			int32			fCurrentFreq;
			uint32			fFeedbackPhase;
			int32			fFreqShift;

private:
			// The actual start sequence. Start() wraps it purely to keep the
			// failure accounting in one place instead of on every exit path.
			status_t		_Start();
			void			_ReportRunHealth();
			status_t		_ChooseAlternate();
			bool			_RateIsAdvertised(uint32 rate);
			status_t		_SetupUAC2Rates();
			status_t		_SetDeviceSamplingRate();
			status_t		_ResolveEndpoints(usb_interface_info* interface);
			status_t		_ArmClockAndActivate();
			status_t		_SetupBuffers();
			// staleSlot: the media server has not refilled this buffer, so
			// its contents are the previous rotation's audio. Silence goes on
			// the wire instead of replaying it -- see _StageWireChunk.
			status_t		_QueueNextTransfer(size_t buffer, bool start,
								bool staleSlot = false);
	static	void			_TransferCallback(void* cookie, status_t status,
								void* data, size_t actualLength);
			void			_InitFeedbackParams(uint32 rate);
			bool			_ProbeVariableIsoOut(size_t stride);
			void			_QueueWarmup();
	static	void			_ProbeCallback(void* cookie, status_t status,
									void* data, size_t actualLength);
			size_t			_FillPlaybackPackets(
								usb_iso_packet_descriptor* descriptors,
								size_t frames, uint32 stride,
								size_t& emitted);
			status_t		_QueueFeedback();
	static	void			_FeedbackCallback(void* cookie, status_t status,
								void* data, size_t actualLength);
			void			_ProcessFeedback(const uint8* data, size_t length);
			void			_PublishImplicitFeedback(size_t actualLength);
			size_t			_RepackCapture(void* scratch);
	static	void			_Pack24(const uint8* media, uint8* wire,
								size_t subframes);
	static	void			_Unpack24(const uint8* wire, uint8* media,
								size_t subframes);
			void			_StageWireChunk(size_t queuedBuffer,
								bool staleSlot);
			void			_DumpDescriptors();
			// ISODIAG: scan the completed transfer's iso descriptors for
			// truncation and missed service. Input streams only; `scratch` is
			// the completed buffer, used to pick the descriptor slice.
			void			_CheckCaptureOverrun(void* scratch);
};


#endif // _USB_AUDIO_STREAM_H_

