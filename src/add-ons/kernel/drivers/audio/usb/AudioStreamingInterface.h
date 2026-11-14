/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Distributed under the terms of the MIT license.
 *
 */
#ifndef _AUDIO_STREAMING_INTERFACE_H_
#define _AUDIO_STREAMING_INTERFACE_H_


#include <USB3.h>
#include <util/VectorMap.h>

#include "AudioControlInterface.h"


class ASInterfaceDescriptor {
public:
							ASInterfaceDescriptor(uint16 specReleaseNumber,
								usb_audio_streaming_interface_descriptor*
								Descriptor);
							~ASInterfaceDescriptor();

// protected:
			bool			fIsR2;
			uint8			fTerminalLink;
			uint8			fDelay;			// R1 only
			uint16			fFormatTag;		// R1 only
			// R2: the format is a bitmap and the channel count is carried here
			// rather than in the Format Type descriptor (Audio20 Table 4-27).
			uint8			fFormatType;
			uint32			fBmFormats;
			uint8			fChannelsCount;
			uint32			fChannelConfig;
};


class ASEndpointDescriptor {
public:
							ASEndpointDescriptor(
								usb_endpoint_descriptor* Endpoint,
								usb_audio_streaming_endpoint_descriptor*
								Descriptor);
							~ASEndpointDescriptor();

// protected:
			uint8			fCSAttributes;
			uint8			fLockDelayUnits;
			uint16			fLockDelay;
			uint16			fMaxPacketSize;
			uint8			fEndpointAddress;
			uint8			fEndpointAttributes;
};


class _ASFormatDescriptor {
public:
							_ASFormatDescriptor(
								usb_audio_format_descriptor* Descriptor);
	virtual					~_ASFormatDescriptor();

// protected:
			uint8			fFormatType;
	static	uint32			GetSamFreq(const usb_audio_sampling_freq& freq);
	static	usb_audio_sampling_freq	GetSamFreq(uint32 rate);
};


class TypeIFormatDescriptor : public _ASFormatDescriptor {
public:
							TypeIFormatDescriptor(uint16 specReleaseNumber,
								usb_audio_format_descriptor* Descriptor);
	virtual					~TypeIFormatDescriptor();

			status_t		Init(uint16 specReleaseNumber,
								usb_audio_format_descriptor* Descriptor);

// protected:
			uint8			fNumChannels;
			uint8			fSubframeSize;
			uint8			fBitResolution;
			uint8			fSampleFrequencyType;
			Vector<uint32>	fSampleFrequencies;
};


class TypeIIFormatDescriptor : public _ASFormatDescriptor {
public:
							TypeIIFormatDescriptor(
								usb_audio_format_descriptor* Descriptor);
	virtual					~TypeIIFormatDescriptor();

// protected:
			uint16			fMaxBitRate;
			uint16			fSamplesPerFrame;
			uint8			fSampleFrequencyType;
			Vector<uint32>	fSampleFrequencies;
};


class TypeIIIFormatDescriptor : public TypeIFormatDescriptor {
public:
							TypeIIIFormatDescriptor(uint16 specReleaseNumber,
								usb_audio_format_descriptor* Descriptor);
	virtual					~TypeIIIFormatDescriptor();

// protected:
};


class AudioStreamAlternate {
public:
									AudioStreamAlternate(size_t alternate,
										ASInterfaceDescriptor* interface,
										ASEndpointDescriptor* endpoint,
										_ASFormatDescriptor* format);
									~AudioStreamAlternate();

			ASInterfaceDescriptor*	Interface()	{ return fInterface; }
			ASEndpointDescriptor*	Endpoint()	{ return fEndpoint;	 }
			_ASFormatDescriptor*	Format()	{ return fFormat;	 }

			status_t				SetSamplingRate(uint32 newRate);
			status_t				SetSamplingRateById(uint32 newId);
			uint32					GetSamplingRate() { return fSamplingRate; }
			uint32					GetSamplingRateId(uint32 rate);
			uint32					GetSamplingRateIds();
			uint32					GetFormatId();
			status_t				SetFormatId(uint32 newFormatId);
			uint32					SamplingRateFromId(uint32 id);

protected:

			size_t					fAlternate;
			ASInterfaceDescriptor*	fInterface;
			ASEndpointDescriptor*	fEndpoint;
			_ASFormatDescriptor*	fFormat;
			uint32					fSamplingRate;
};


class AudioStreamingInterface {
public:
								AudioStreamingInterface(
									AudioControlInterface*	controlInterface,
									size_t interface, usb_interface_list* List);
								~AudioStreamingInterface();

	//		status_t			InitCheck() { return fStatus; }
			uint8				TerminalLink();
			bool				IsInput() { return fIsInput; }

			AudioChannelCluster* ChannelCluster();

			void				GetFormatsAndRates(
									multi_description* Description);
protected:
			size_t				fInterface;
			AudioControlInterface*	fControlInterface;

	//		status_t			fStatus;
			bool				fIsInput;
		// alternates of the streams
			Vector<AudioStreamAlternate*>	fAlternates;
			size_t				fActiveAlternate;
};


#endif // _AUDIO_STREAMING_INTERFACE_H_

