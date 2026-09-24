/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Distributed under the terms of the MIT license.
 *
 */
#ifndef _USB_AUDIO_DEVICE_H_
#define _USB_AUDIO_DEVICE_H_


#include "AudioControlInterface.h"
#include "Stream.h"


class Device {
	friend	class			Stream;

public:
							Device(usb_device device);
	virtual					~Device();

			status_t		InitCheck() { return fStatus; };

			status_t		Open(uint32 flags);
			bool			IsOpen() { return fOpen; };

			status_t		Close();
			status_t		Free();

			status_t		Read(uint8* buffer, size_t* numBytes);
			status_t		Write(const uint8* buffer, size_t* numBytes);
			status_t		Control(uint32 op, void* buffer, size_t length);

			void			Removed();
			bool			IsRemoved() { return fRemoved; };

			status_t		CompareAndReattach(usb_device device);
	virtual	status_t		SetupDevice(bool deviceReplugged);

			usb_device		USBDevice() { return fDevice; }

			// Device's own product string (from the iProduct descriptor),
			// shown as the multi_audio friendly_name. fInstanceIndex is 0 for
			// the first device of a given model and N>0 for later identical
			// ones (rendered as a " #(N+1)" suffix); it is assigned by the
			// driver when the device is added, under the driver lock.
			const char*		ProductName() { return fProductName; }
			int32			InstanceIndex() { return fInstanceIndex; }
			void			SetInstanceIndex(int32 index)
								{ fInstanceIndex = index; }

			AudioControlInterface&
							AudioControl() { return fAudioControl; }

			// Implicit feedback: a capture stream whose endpoint carries the
			// "implicit feedback data" usage type is the sampling-clock
			// reference for an asynchronous playback stream on the same device
			// (both run off one crystal). The capture stream publishes its
			// measured rate here and the playback stream reads it to size its
			// outgoing packets. The value is audio frames per (micro)frame in
			// 16.16 fixed point, or 0 until the first buffer has been measured.
			void			PublishFeedback(int32 framesPerPacket);
			int32			Feedback();
			bool			HasImplicitFeedbackSource()
								{ return fImplicitFeedbackSource; }
			void			SetImplicitFeedbackSource(uint8 interval)
								{
									fImplicitFeedbackSource = true;
									fImplicitSourceInterval = interval;
								}
			uint8			ImplicitSourceInterval()
								{ return fImplicitSourceInterval; }

			// Besides the averaged rate above, the capture stream records the
			// exact frame count of every isochronous packet the device
			// delivered into this single-producer/single-consumer ring, and
			// the playback stream sizes its outgoing packets by replaying
			// them 1:1 (each entry is one service interval; 0 = errored
			// packet). Playback then tracks the device's clock frame-for-
			// frame with no estimation noise -- some devices (e.g. the
			// Behringer UMC2xxHD family) audibly glitch on anything less
			// exact. Producer and consumer both run in the USB stack's
			// transfer-completion context; the indices are only ever advanced
			// by their own side.
			bool			PushFeedbackPacket(uint16 frames);
			bool			PeekFeedbackPacket(uint16& frames);
			void			PopFeedbackPacket();

			// Cached result of the variable-length isochronous OUT capability
			// probe (Stream::_ProbeVariableIsoOut): the capability belongs to
			// the host controller path, not to a stream, and probing sends
			// packets on the wire -- do it at most once per device.
			int8			VariableIsoOutSupport()
								{ return fVariableIsoOutSupport; }
			void			SetVariableIsoOutSupport(bool supported)
								{ fVariableIsoOutSupport = supported ? 1 : 0; }

private:
			status_t		_SetupEndpoints();
			void			_ReadProductName();
			bool			_FetchStringAscii(uint8 index, uint16 langId,
								char* out, size_t outSize);
			uint32			_ReadLangIds(uint16* langs, uint32 maxCount);

// protected:
	virtual	status_t		StartDevice() { return B_OK; }
	virtual	status_t		StopDevice();

			void			TraceMultiDescription(multi_description* Description,
								Vector<multi_channel_info>& Channels);
			void			TraceListMixControls(multi_mix_control_info* Info);

		// state tracking
			status_t		fStatus;
			bool			fOpen;
			bool			fRemoved;
			usb_device		fDevice;
			uint16			fUSBVersion;
			uint16			fVendorID;
			uint16			fProductID;
			// Cached product string and per-model instance index; see the
			// ProductName()/InstanceIndex() accessors above.
			char			fProductName[64];
			int32			fInstanceIndex;
	const	char*			fDescription;
			bool			fNonBlocking;

			AudioControlInterface	fAudioControl;
			Vector<Stream*>	fStreams;

			int32			fFeedbackFrames;
			bool			fImplicitFeedbackSource;
			uint8			fImplicitSourceInterval;
			int8			fVariableIsoOutSupport;

			// Implicit-feedback packet-size ring; must be a power of two.
			// 4096 entries buffer ~512 ms of microframes -- comfortably more
			// than the sample buffers in flight (kSamplesBufferCount worth),
			// so scheduling skew between the two streams' completions cannot
			// overflow it and break the 1:1 packet mirroring.
	static	const uint32	kFeedbackRingSize = 4096;
			uint16			fFeedbackRing[kFeedbackRingSize];
			int32			fFeedbackRingHead;
			int32			fFeedbackRingTail;

// protected:
			status_t		_MultiGetDescription(multi_description* Description);
			status_t		_MultiGetEnabledChannels(multi_channel_enable* Enable);
			status_t		_MultiSetEnabledChannels(multi_channel_enable* Enable);
			status_t		_MultiGetBuffers(multi_buffer_list* List);
			status_t		_MultiGetGlobalFormat(multi_format_info* Format);
			status_t		_MultiSetGlobalFormat(multi_format_info* Format);
			uint8			_SharedClockId();
			status_t		_MultiGetMix(multi_mix_value_info* Info);
			status_t		_MultiSetMix(multi_mix_value_info* Info);
			status_t		_MultiListMixControls(multi_mix_control_info* Info);
			status_t		_MultiBufferExchange(multi_buffer_info* Info);
			status_t		_MultiBufferForceStop();

			sem_id			fBuffersReadySem;

};


#endif // _USB_AUDIO_DEVICE_H_

