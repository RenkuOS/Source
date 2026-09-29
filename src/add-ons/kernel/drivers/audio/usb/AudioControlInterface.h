/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Distributed under the terms of the MIT license.
 *
 */
#ifndef _AUDIO_CONTROL_INTERFACE_H_
#define _AUDIO_CONTROL_INTERFACE_H_


#include <USB3.h>
#include <hmulti_audio.h>
#include <usb/USB_audio.h>
#include <util/VectorMap.h>


class Device;
class AudioControlInterface;

class _AudioControl;
typedef	VectorMap<uint32, _AudioControl*> AudioControlsMap;
typedef	VectorMap<uint32, _AudioControl*>::Iterator AudioControlsIterator;


class AudioChannelCluster {
public:
							AudioChannelCluster();
	virtual					~AudioChannelCluster();

			uint8			ChannelsCount()		{ return fOutChannelsNumber; }
			uint32			ChannelsConfig()	{ return fChannelsConfig;	 }

			bool			HasChannel(uint32 location);

protected:
			uint8			fOutChannelsNumber;
			uint32			fChannelsConfig;
			uint8			fChannelNames;
};


template<class __base_class_name>
class _AudioChannelCluster : public __base_class_name,
	public AudioChannelCluster {
public:
							_AudioChannelCluster(AudioControlInterface* interface,
								usb_audiocontrol_header_descriptor* Header)
							:
							__base_class_name(interface, Header) {}
	virtual					~_AudioChannelCluster() {}

	virtual	AudioChannelCluster*
							OutCluster() { return this; }
};


class _AudioControl {
public:
							_AudioControl(AudioControlInterface* interface,
								usb_audiocontrol_header_descriptor* Header);
	virtual					~_AudioControl();

			uint8			ID() { return fID; }
			uint8			SourceID() { return fSourceID; }
			uint8			SubType() { return fSubType; }
			status_t		InitCheck() { return fStatus; };

	virtual	const char*		Name() { return ""; }

	virtual AudioChannelCluster*
							OutCluster();

protected:
			status_t		fStatus;
			AudioControlInterface*	fInterface;
			uint8			fSubType;
			uint8			fID;
			uint8			fSourceID;
			uint8			fStringIndex;
};


class _Terminal : public _AudioControl {
public:
							_Terminal(AudioControlInterface* interface,
								usb_audiocontrol_header_descriptor* Header);
	virtual					~_Terminal();

			uint16			TerminalType() { return fTerminalType; }
			bool			IsUSBIO();
			uint8			ClockSourceId() { return fClockSourceId; }
	virtual	const char*		Name();
	static	const char*		_GetTerminalDescription(uint16 TerminalType);

protected:
			uint16			fTerminalType;
			uint8			fAssociatedTerminal;
			uint8			fClockSourceId;
			uint16			fControlsBitmap;
};


class InputTerminal : public _AudioChannelCluster<_Terminal> {
public:
							InputTerminal(AudioControlInterface* interface,
								usb_audiocontrol_header_descriptor* Header);
	virtual					~InputTerminal();

	virtual	const char*		Name();

protected:
};


class OutputTerminal : public _Terminal {
public:
							OutputTerminal(AudioControlInterface* interface,
								usb_audiocontrol_header_descriptor* Header);
	virtual					~OutputTerminal();

	virtual	const char*		Name();

protected:
};


class MixerUnit : public _AudioChannelCluster<_AudioControl> {
public:
							MixerUnit(AudioControlInterface* interface,
								usb_audiocontrol_header_descriptor* Header);
	virtual					~MixerUnit();

	virtual	const char*		Name() { return "Mixer"; }
			bool			HasProgrammableControls();
			bool			IsControlProgrammable(int inChannel, int outChannel);

//protected:
			Vector<uint8>	fInputPins;
			Vector<uint8>	fControlsBitmap;
			uint8			fBmControlsR2;
};


class SelectorUnit : public _AudioControl {
public:
							SelectorUnit(AudioControlInterface* interface,
								usb_audiocontrol_header_descriptor* Header);
	virtual					~SelectorUnit();

	virtual	AudioChannelCluster*
							OutCluster();
	virtual	const char*		Name() { return "Selector"; }

// protected:
			Vector<uint8>	fInputPins;
			uint8			fControlsBitmap;
};


class FeatureUnit : public _AudioControl {
public:
							FeatureUnit(AudioControlInterface* interface,
								usb_audiocontrol_header_descriptor* Header);
	virtual					~FeatureUnit();

	virtual	const char*		Name();
			bool			HasControl(int32 Channel, uint32 Control);

// protected:
			void			NormalizeAndTraceChannel(int32 Channel);

			Vector<uint32>	fControlBitmaps;
};


class EffectUnit : public _AudioControl {
public:
							EffectUnit(AudioControlInterface* interface,
								usb_audiocontrol_header_descriptor* Header);
	virtual					~EffectUnit();

	virtual	const char*		Name() { return "Effect"; }
protected:
/*			uint16			fProcessType;
			Vector<uint8>	fInputPins;
			uint8			fControlsBitmap;
			Vector<uint16>	fModes;
*/};


class ProcessingUnit : public _AudioChannelCluster<_AudioControl> {
public:
							ProcessingUnit(AudioControlInterface* interface,
								usb_audiocontrol_header_descriptor* Header);
	virtual					~ProcessingUnit();

	virtual	const char*		Name() { return "Processing"; }

protected:
			uint16			fProcessType;
			Vector<uint8>	fInputPins;
			uint8			fControlsBitmap;
			Vector<uint16>	fModes;
};


class ExtensionUnit : public _AudioChannelCluster<_AudioControl> {
public:
							ExtensionUnit(AudioControlInterface* interface,
								usb_audiocontrol_header_descriptor* Header);
	virtual					~ExtensionUnit();

	virtual	const char*		Name() { return "Extension"; }

protected:
			uint16			fExtensionCode;
			Vector<uint8>	fInputPins;
			uint8			fControlsBitmap;
};


class ClockSource : public _AudioControl {
public:
							ClockSource(AudioControlInterface* interface,
								usb_audiocontrol_header_descriptor* Header);
	virtual					~ClockSource();

	virtual	const char*		Name() { return "Clock Source"; }
			bool			SamplingFrequencyReadable();
			bool			SamplingFrequencyWritable();

protected:
			uint8			fClockType;			// bmAttributes
			uint8			fControlsBitmap;	// bmControls
};


class ClockSelector : public _AudioControl {
public:
							ClockSelector(AudioControlInterface* interface,
								usb_audiocontrol_header_descriptor* Header);
	virtual					~ClockSelector();

	virtual	const char*		Name() { return "Clock Selector"; }

//	protected:
			Vector<uint8>	fInputPins;
			uint8			fControlsBitmap;
};


class ClockMultiplier : public _AudioControl {
public:
							ClockMultiplier(AudioControlInterface* interface,
								usb_audiocontrol_header_descriptor* Header);
	virtual					~ClockMultiplier();

	virtual	const char*		Name() { return "Clock Multiplier"; }

protected:
			uint8			fControlsBitmap;
};


class SampleRateConverter : public _AudioControl {
public:
							SampleRateConverter(
								AudioControlInterface* interface,
								usb_audiocontrol_header_descriptor* Header);
	virtual					~SampleRateConverter();

protected:
};


class AudioControlInterface {
public:
			enum {
				kLeftChannel = 0,
				kRightChannel = 1,
				kChannels = 18
			};

							AudioControlInterface(Device* device);
							~AudioControlInterface();

			status_t		InitCheck() { return fStatus; }
			status_t		Init(size_t interface, usb_interface_info* Interface);

			_AudioControl*	Find(uint8 id);
			_AudioControl*	FindOutputTerminal(uint8 id);
			uint16			SpecReleaseNumber() { return fADCSpecification; }

			// R2: Clock Source handling. The stream identifies its clock via
			// its terminal; sampling frequency is get/set through Clock Source
			// class requests on the AudioControl interface (not on the endpoint
			// as in R1).
			uint8			ClockSourceIdForTerminal(uint8 terminalId);
			bool			ClockRateIsWritable(uint8 clockId);
			status_t		GetSamplingRates(uint8 clockId,
								Vector<uint32>& rates);
			status_t		SetSamplingRate(uint8 clockId, uint32 rate);
			uint8			LastClockId() { return fLastClockId; }
			uint32			LastClockRate() { return fLastClockRate; }

			AudioControlsMap&
							Controls() { return fAudioControls; }

			// Probe, log, unmute and open the volume of every feature-unit
			// channel. Some devices (Hercules DJ Console RMX) power up muted,
			// staying silent although USB streaming is perfectly healthy.
			// Handles both R1 (GET/SET_CUR) and R2 (CUR/RANGE) request sets.
			void			InitHardwareGains();

			// R2: route every writable Clock Selector to its first input pin.
			// Part of the stream-start sequence (working hosts rewrite the
			// selector at every stream open), not just initial probe.
			void			RouteClockSelectors();

			uint32			GetChannelsDescription(
								Vector<multi_channel_info>& Channels,
								multi_description* Description,
								Vector<_AudioControl*>& USBTerminals,
								bool isForInput);
			uint32			GetBusChannelsDescription(
								Vector<multi_channel_info>& Channels,
								multi_description* Description);

			status_t		GetMix(multi_mix_value_info* Info);
			status_t		SetMix(multi_mix_value_info* Info);
			status_t		ListMixControls(multi_mix_control_info* Info);

protected:
			status_t		InitACHeader(size_t interface,
								usb_audiocontrol_header_descriptor* Header);

			uint32			GetTerminalChannels(
								Vector<multi_channel_info>& Channels,
								AudioChannelCluster* cluster,
								channel_kind kind, uint32 connectors = 0);

			void			_HarvestRecordFeatureUnits(_AudioControl* rootControl,
								AudioControlsMap& Map);
			void			_HarvestOutputFeatureUnits(_AudioControl* rootControl,
								AudioControlsMap& Map);
			void			_ListMixControlsPage(int32& index,
								multi_mix_control_info* Info,
								AudioControlsMap& Map, const char* Name);
			int32			_ListFeatureUnitControl(int32& index, int32 parentId,
								multi_mix_control_info* Info,
								_AudioControl* control);
			uint32			_ListFeatureUnitOption(uint32 controlType,
								int32& index, int32 parentIndex,
								multi_mix_control_info* Info, FeatureUnit* unit,
								uint32 channel, uint32 channels);
			void			_ListSelectorUnitControl(int32& index,
								int32 parentGroup, multi_mix_control_info* Info,
								_AudioControl* control);
			void			_ListMixControlsForMixerUnit(int32& index,
								multi_mix_control_info* Info,
								_AudioControl* control);
			void			_ListMixerUnitControls(int32& index,
								multi_mix_control_info* Info,
								Vector<multi_mix_control>& controls);
			size_t			_CollectMixerUnitControls(
								const uint32 controlIds[kChannels][kChannels],
								size_t inLeft, size_t outLeft,
								size_t inRight, size_t outRight,
								const char* inputName, const char* name,
								Vector<multi_mix_control>& Controls);
			bool			_InitGainLimits(multi_mix_control& Control);
			uint8			_ActiveClockSelectorInput(ClockSelector* selector,
								uint8 selectorId);

			size_t			fInterface;
			status_t		fStatus;
			// part of AudioControl Header description
			uint16			fADCSpecification;
			Vector<uint8>	fStreams;
			uint8			fFunctionCategory;
			uint8			fControlsBitmap;
			// R2: last rate programmed to a Clock Source, for restoring the
			// (stream-shared) clock after a device reattach.
			uint8			fLastClockId;
			uint32			fLastClockRate;
			Device*			fDevice;

			// map to store all controls and lookup by control ID
			AudioControlsMap	fAudioControls;
			// map to store output terminal and lookup them by source ID
			AudioControlsMap	fOutputTerminals;
			// map to store output terminal and lookup them by control ID
			AudioControlsMap	fInputTerminals;
};


#endif // _AUDIO_CONTROL_INTERFACE_H_

