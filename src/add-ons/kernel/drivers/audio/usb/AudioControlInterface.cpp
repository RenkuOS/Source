/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Distributed under the tems of the MIT license.
 *
 */

#include "AudioControlInterface.h"

#include <usb/USB_audio.h>

#include "Device.h"
#include "Driver.h"
#include "Settings.h"


// control id is encoded in following way
//	CS	CN	ID	IF where:
//	CS				- control selector
//		CN			- channel
//			ID		- id of this feature unit
//				IF	- interface

#define CTL_ID(_CS, _CN, _ID, _IF) \
	(((_CS) << 24) | ((_CN) << 16) | ((_ID) << 8) | (_IF))

#define CS_FROM_CTLID(_ID) (0xff & ((_ID) >> 24))
#define CN_FROM_CTLID(_ID) (0xff & ((_ID) >> 16))
#define ID_FROM_CTLID(_ID) (0xff & ((_ID) >> 8))

#define REQ_VALUE(_ID) (0xffff & ((_ID) >> 16))
#define REQ_INDEX(_ID) (0xffff & (_ID))


struct _Designation {
	uint32 ch;
	uint32 bus;
} gDesignations[AudioControlInterface::kChannels] = {
	{ B_CHANNEL_LEFT,				B_CHANNEL_STEREO_BUS	},
	{ B_CHANNEL_RIGHT,				B_CHANNEL_STEREO_BUS	},
	{ B_CHANNEL_CENTER,				B_CHANNEL_SURROUND_BUS	},
	{ B_CHANNEL_SUB,				B_CHANNEL_SURROUND_BUS	},
	{ B_CHANNEL_REARLEFT,			B_CHANNEL_STEREO_BUS	},
	{ B_CHANNEL_REARRIGHT,			B_CHANNEL_STEREO_BUS	},
	{ B_CHANNEL_FRONT_LEFT_CENTER,	B_CHANNEL_STEREO_BUS	},
	{ B_CHANNEL_FRONT_RIGHT_CENTER,	B_CHANNEL_STEREO_BUS	},
	{ B_CHANNEL_BACK_CENTER,		B_CHANNEL_SURROUND_BUS	},
	{ B_CHANNEL_SIDE_LEFT,			B_CHANNEL_STEREO_BUS	},
	{ B_CHANNEL_SIDE_RIGHT,			B_CHANNEL_STEREO_BUS	},
	{ B_CHANNEL_TOP_CENTER,			B_CHANNEL_SURROUND_BUS	},
	{ B_CHANNEL_TOP_FRONT_LEFT,		B_CHANNEL_STEREO_BUS	},
	{ B_CHANNEL_TOP_FRONT_CENTER,	B_CHANNEL_SURROUND_BUS	},
	{ B_CHANNEL_TOP_FRONT_RIGHT,	B_CHANNEL_STEREO_BUS	},
	{ B_CHANNEL_TOP_BACK_LEFT,		B_CHANNEL_STEREO_BUS	},
	{ B_CHANNEL_TOP_BACK_CENTER,	B_CHANNEL_SURROUND_BUS	},
	{ B_CHANNEL_TOP_BACK_RIGHT,		B_CHANNEL_STEREO_BUS	}
};


struct _MixPageCollector : public Vector<multi_mix_control> {
	_MixPageCollector(const char* pageName) {
		multi_mix_control page;
		memset(&page, 0, sizeof(multi_mix_control));
		page.flags = B_MULTI_MIX_GROUP;
		strlcpy(page.name, pageName, sizeof(page.name));
		PushBack(page);
	}
};


_AudioControl::_AudioControl(AudioControlInterface*	interface,
		usb_audiocontrol_header_descriptor* Header)
	:
	fStatus(B_NO_INIT),
	fInterface(interface),
	fSubType(Header->descriptor_subtype),
	fID(0),
	fSourceID(0),
	fStringIndex(0)
{
}


_AudioControl::~_AudioControl()
{
}


AudioChannelCluster*
_AudioControl::OutCluster()
{
	if (SourceID() == 0 || fInterface == NULL)
		return NULL;

	_AudioControl* control = fInterface->Find(SourceID());
	if (control == NULL)
		return NULL;

	return control->OutCluster();
}


AudioChannelCluster::AudioChannelCluster()
	:
	fOutChannelsNumber(0),
	fChannelsConfig(0),
	fChannelNames(0)
{
}


AudioChannelCluster::~AudioChannelCluster()
{
}


bool
AudioChannelCluster::HasChannel(uint32 location)
{
	return (fChannelsConfig & location) == location;
}


_Terminal::_Terminal(AudioControlInterface*	interface,
	usb_audiocontrol_header_descriptor* Header)
	:
	_AudioControl(interface, Header),
	fTerminalType(0),
	fAssociatedTerminal(0),
	fClockSourceId(0),
	fControlsBitmap(0)
{
}


_Terminal::~_Terminal()
{
}


const char*
_Terminal::Name()
{
	return _GetTerminalDescription(fTerminalType);
}


bool
_Terminal::IsUSBIO()
{
	return (fTerminalType & 0xff00) == USB_AUDIO_UNDEFINED_USB_IO;
}


const char*
_Terminal::_GetTerminalDescription(uint16 TerminalType)
{
	static struct _pair {
		uint16 type;
		const char* description;
	} termInfoPairs[] = {
		// USB Terminal Types
		{ USB_AUDIO_UNDEFINED_USB_IO,		"USB I/O" },
		{ USB_AUDIO_STREAMING_USB_IO,		"USB I/O" },
		{ USB_AUDIO_VENDOR_USB_IO,			"Vendor USB I/O" },
		// Input Terminal Types
		{ USB_AUDIO_UNDEFINED_IN,			"Undefined Input" },
		{ USB_AUDIO_MICROPHONE_IN,			"Microphone" },
		{ USB_AUDIO_DESKTOPMIC_IN,			"Desktop Microphone" },
		{ USB_AUDIO_PERSONALMIC_IN,			"Personal Microphone" },
		{ USB_AUDIO_OMNI_MIC_IN,			"Omni-directional Mic" },
		{ USB_AUDIO_MICS_ARRAY_IN,			"Microphone Array" },
		{ USB_AUDIO_PROC_MICS_ARRAY_IN,		"Processing Mic Array" },
		// Output Terminal Types
		{ USB_AUDIO_UNDEFINED_OUT,			"Undefined Output" },
		{ USB_AUDIO_SPEAKER_OUT,			"Speaker" },
		{ USB_AUDIO_HEAD_PHONES_OUT,		"Headphones" },
		{ USB_AUDIO_HMD_AUDIO_OUT,			"Head Mounted Disp.Audio" },
		{ USB_AUDIO_DESKTOP_SPEAKER,		"Desktop Speaker" },
		{ USB_AUDIO_ROOM_SPEAKER,			"Room Speaker" },
		{ USB_AUDIO_COMM_SPEAKER,			"Communication Speaker" },
		{ USB_AUDIO_LFE_SPEAKER,			"LFE Speaker" },
		// Bi-directional Terminal Types
		{ USB_AUDIO_UNDEFINED_IO,			"Undefined I/O" },
		{ USB_AUDIO_HANDSET_IO,				"Handset" },
		{ USB_AUDIO_HEADSET_IO,				"Headset" },
		{ USB_AUDIO_SPEAKER_PHONE_IO,		"Speakerphone" },
		{ USB_AUDIO_SPEAKER_PHONEES_IO,		"Echo-supp Speakerphone" },
		{ USB_AUDIO_SPEAKER_PHONEEC_IO,		"Echo-cancel Speakerphone" },
		// Telephony Terminal Types
		{ USB_AUDIO_UNDEF_TELEPHONY_IO,		"Undefined Telephony" },
		{ USB_AUDIO_PHONE_LINE_IO,			"Phone Line" },
		{ USB_AUDIO_TELEPHONE_IO,			"Telephone" },
		{ USB_AUDIO_DOWNLINE_PHONE_IO,		"Down Line Phone" },
		// External Terminal Types
		{ USB_AUDIO_UNDEFINEDEXT_IO,		"Undefined External I/O" },
		{ USB_AUDIO_ANALOG_CONNECTOR_IO,	"Analog Connector" },
		{ USB_AUDIO_DAINTERFACE_IO,			"Digital Audio Interface" },
		{ USB_AUDIO_LINE_CONNECTOR_IO,		"Line Connector" },
		{ USB_AUDIO_LEGACY_CONNECTOR_IO,	"LegacyAudioConnector" },
		{ USB_AUDIO_SPDIF_INTERFACE_IO,		"S/PDIF Interface" },
		{ USB_AUDIO_DA1394_STREAM_IO,		"1394 DA Stream" },
		{ USB_AUDIO_DV1394_STREAMSOUND_IO,	"1394 DV Stream Soundtrack" },
		{ USB_AUDIO_ADAT_LIGHTPIPE_IO,		"Alesis DAT Stream" },
		{ USB_AUDIO_TDIF_IO,				"Tascam Digital Interface" },
		{ USB_AUDIO_MADI_IO,				"AES Multi-channel interface" },
		// Embedded Terminal Types
		{ USB_AUDIO_UNDEF_EMBEDDED_IO,		"Undefined Embedded I/O" },
		{ USB_AUDIO_LC_NOISE_SOURCE_OUT,	"Level Calibration Noise Source" },
		{ USB_AUDIO_EQUALIZATION_NOISE_OUT,	"Equalization Noise" },
		{ USB_AUDIO_CDPLAYER_IN,			"CD Player" },
		{ USB_AUDIO_DAT_IO,					"DAT" },
		{ USB_AUDIO_DCC_IO,					"DCC" },
		{ USB_AUDIO_MINI_DISK_IO,			"Mini Disk" },
		{ USB_AUDIO_ANALOG_TAPE_IO,			"Analog Tape" },
		{ USB_AUDIO_PHONOGRAPH_IN,			"Phonograph" },
		{ USB_AUDIO_VCR_AUDIO_IN,			"VCR Audio" },
		{ USB_AUDIO_VIDEO_DISC_AUDIO_IN,	"Video Disc Audio" },
		{ USB_AUDIO_DVD_AUDIO_IN,			"DVD Audio" },
		{ USB_AUDIO_TV_TUNER_AUDIO_IN,		"TV Tuner Audio" },
		{ USB_AUDIO_SAT_RECEIVER_AUDIO_IN,	"Satellite Receiver Audio" },
		{ USB_AUDIO_CABLE_TUNER_AUDIO_IN,	"Cable Tuner Audio" },
		{ USB_AUDIO_DSS_AUDIO_IN,			"DSS Audio" },
		{ USB_AUDIO_RADIO_RECEIVER_IN,		"Radio Receiver" },
		{ USB_AUDIO_RADIO_TRANSMITTER_IN,	"Radio Transmitter" },
		{ USB_AUDIO_MULTI_TRACK_RECORDER_IO,"Multi-track Recorder" },
		{ USB_AUDIO_SYNTHESIZER_IO,			"Synthesizer" },
		{ USB_AUDIO_PIANO_IO,				"Piano" },
		{ USB_AUDIO_GUITAR_IO,				"Guitar" },
		{ USB_AUDIO_DRUMS_IO,				"Percussion Instrument" },
		{ USB_AUDIO_INSTRUMENT_IO,			"Musical Instrument" }
	};

	for (size_t i = 0; i < B_COUNT_OF(termInfoPairs); i++)
		if (termInfoPairs[i].type == TerminalType)
			return termInfoPairs[i].description;

	TRACE(ERR, "Unknown Terminal Type: %#06x", TerminalType);
	return "Unknown";
}


InputTerminal::InputTerminal(AudioControlInterface*	interface,
		usb_audiocontrol_header_descriptor* Header)
	:
	_AudioChannelCluster<_Terminal>(interface, Header)
{
	usb_audio_input_terminal_descriptor* Terminal
		= (usb_audio_input_terminal_descriptor*) Header;
	fID					= Terminal->terminal_id;
	fTerminalType		= Terminal->terminal_type;
	fAssociatedTerminal = Terminal->assoc_terminal;

	TRACE(UAC, "Input Terminal ID:%d >>>\n", fID);
	TRACE(UAC, "Terminal type:%s (%#06x)\n",
		_GetTerminalDescription(fTerminalType), fTerminalType);
	TRACE(UAC, "Assoc.terminal:%d\n",	fAssociatedTerminal);

	if (fInterface->SpecReleaseNumber() < 0x200) {
		fOutChannelsNumber	= Terminal->r1.num_channels;
		fChannelsConfig		= Terminal->r1.channel_config;
		fChannelNames		= Terminal->r1.channel_names;
		fStringIndex		= Terminal->r1.terminal;
	} else {
		fClockSourceId		= Terminal->r2.clock_source_id;
		fOutChannelsNumber	= Terminal->r2.num_channels;
		fChannelsConfig		= Terminal->r2.channel_config;
		fChannelNames		= Terminal->r2.channel_names;
		fControlsBitmap		= Terminal->r2.bm_controls;
		fStringIndex		= Terminal->r2.terminal;

		TRACE(UAC, "Clock Source ID:%d\n", fClockSourceId);
		TRACE(UAC, "Controls Bitmap:%#04x\n", fControlsBitmap);
	}

	TRACE(UAC, "Out.channels num:%d\n",	 fOutChannelsNumber);
	TRACE(UAC, "Channels config:%#06x\n", fChannelsConfig);
	TRACE(UAC, "Channels names:%d\n",	 fChannelNames);
	TRACE(UAC, "StringIndex:%d\n",		 fStringIndex);

	fStatus = B_OK;
}


InputTerminal::~InputTerminal()
{
}


const char*
InputTerminal::Name()
{
	if (fTerminalType == USB_AUDIO_STREAMING_USB_IO)
		return "USB Input";
	return _Terminal::Name();
}


OutputTerminal::OutputTerminal(AudioControlInterface*	interface,
		usb_audiocontrol_header_descriptor* Header)
	:
	_Terminal(interface, Header)
{
	usb_audio_output_terminal_descriptor* Terminal
		= (usb_audio_output_terminal_descriptor*) Header;

	fID					= Terminal->terminal_id;
	fTerminalType		= Terminal->terminal_type;
	fAssociatedTerminal	= Terminal->assoc_terminal;
	fSourceID			= Terminal->source_id;

	TRACE(UAC, "Output Terminal ID:%d >>>\n",	fID);
	TRACE(UAC, "Terminal type:%s (%#06x)\n",
		_GetTerminalDescription(fTerminalType), fTerminalType);
	TRACE(UAC, "Assoc.terminal:%d\n",		fAssociatedTerminal);
	TRACE(UAC, "Source ID:%d\n",				fSourceID);

	if (fInterface->SpecReleaseNumber() < 0x200) {
		fStringIndex = Terminal->r1.terminal;
	} else {
		fClockSourceId	= Terminal->r2.clock_source_id;
		fControlsBitmap	= Terminal->r2.bm_controls;
		fStringIndex	= Terminal->r2.terminal;

		TRACE(UAC, "Clock Source ID:%d\n", fClockSourceId);
		TRACE(UAC, "Controls Bitmap:%#04x\n", fControlsBitmap);
	}

	TRACE(UAC, "StringIndex:%d\n", fStringIndex);

	fStatus = B_OK;
}


OutputTerminal::~OutputTerminal()
{
}


const char*
OutputTerminal::Name()
{
	if (fTerminalType == USB_AUDIO_STREAMING_USB_IO)
		return "USB Output";
	return _Terminal::Name();
}


MixerUnit::MixerUnit(AudioControlInterface*	interface,
		usb_audiocontrol_header_descriptor* Header)
	:
	_AudioChannelCluster<_AudioControl>(interface, Header),
	fBmControlsR2(0)
{
	usb_audio_mixer_unit_descriptor* Mixer
		= (usb_audio_mixer_unit_descriptor*) Header;

	fID = Mixer->unit_id;
	TRACE(UAC, "Mixer ID:%d >>>\n", fID);

	TRACE(UAC, "Number of input pins:%d\n", Mixer->num_input_pins);
	for (size_t i = 0; i < Mixer->num_input_pins; i++) {
		fInputPins.PushBack(Mixer->input_pins[i]);
		TRACE(UAC, "Input pin #%d:%d\n", i, fInputPins[i]);
	}

	uint8* mixerControlsData = NULL;
	uint8 mixerControlsSize = 0;

	if (fInterface->SpecReleaseNumber() < 0x200) {
		usb_audio_output_channels_descriptor_r1* OutChannels
			= (usb_audio_output_channels_descriptor_r1*)
			&Mixer->input_pins[Mixer->num_input_pins];

		fOutChannelsNumber	= OutChannels->num_output_pins;
		fChannelsConfig		= OutChannels->channel_config;
		fChannelNames		= OutChannels->channel_names;

		mixerControlsData = (uint8*) ++OutChannels;
		mixerControlsSize = Mixer->length - 10 - Mixer->num_input_pins;
		fStringIndex = *(mixerControlsData + mixerControlsSize);

#if 0 // TEST
		if (fOutChannelsNumber > 2) {
		//	fOutChannelsNumber = 2;
		//	fChannelsConfig = 0x03;
			mixerControlsData[0] = 0x80;
			mixerControlsData[1] = 0x40;
			mixerControlsData[2] = 0x20;
			mixerControlsData[3] = 0x10;
			mixerControlsData[4] = 0x08;
			mixerControlsData[5] = 0x04;
			mixerControlsData[6] = 0x02;
			mixerControlsData[7] = 0x01;
			mixerControlsData[8] = 0x80;
			mixerControlsData[9] = 0x40;
			mixerControlsData[10] = 0x02;
			mixerControlsData[11] = 0x01;
		}
#endif

	} else {
		usb_audio_output_channels_descriptor* OutChannels
			= (usb_audio_output_channels_descriptor*)
			&Mixer->input_pins[Mixer->num_input_pins];

		fOutChannelsNumber	= OutChannels->num_output_pins;
		fChannelsConfig		= OutChannels->channel_config;
		fChannelNames		= OutChannels->channel_names;

		mixerControlsData = (uint8*) ++OutChannels;
		mixerControlsSize = Mixer->length - 13 - Mixer->num_input_pins;
		fBmControlsR2 = *(mixerControlsData + mixerControlsSize);
		fStringIndex = *(mixerControlsData + mixerControlsSize + 1);

		TRACE(UAC, "Control Bitmap:%#04x\n", fBmControlsR2);
	}

	TRACE(UAC, "Out channels number:%d\n",		fOutChannelsNumber);
	TRACE(UAC, "Out channels config:%#06x\n",	fChannelsConfig);
	TRACE(UAC, "Out channels names:%d\n",		fChannelNames);
	TRACE(UAC, "Controls Size:%d\n", mixerControlsSize);

	for (size_t i = 0; i < mixerControlsSize; i++) {
		fControlsBitmap.PushBack(mixerControlsData[i]);
		TRACE(UAC, "Controls Data[%d]:%#x\n", i, fControlsBitmap[i]);
	}

	TRACE(UAC, "StringIndex:%d\n", fStringIndex);

	fStatus = B_OK;
}


MixerUnit::~MixerUnit()
{
}


bool
MixerUnit::HasProgrammableControls()
{
	for (int i = 0; i < fControlsBitmap.Count(); i++)
		if (fControlsBitmap[i] != 0)
			return true;
	return false;
}


bool
MixerUnit::IsControlProgrammable(int inChannel, int outChannel)
{
	AudioChannelCluster* outCluster = OutCluster();
	if (outCluster == NULL) {
		TRACE(ERR, "Output cluster is not valid.\n");
		return false;
	}

	bool result = false;
	if (outChannel < outCluster->ChannelsCount()) {
		int index = inChannel * outCluster->ChannelsCount()+ outChannel;
		result = (fControlsBitmap[index >> 3] & (0x80 >> (index & 7))) != 0;
	}

//	TRACE(UAC, "in:%d out:%d is %s\n",
//		inChannel, outChannel, result ? "on" : "off");
	return result;
}


SelectorUnit::SelectorUnit(AudioControlInterface*	interface,
		usb_audiocontrol_header_descriptor* Header)
	:
	_AudioControl(interface, Header),
	fControlsBitmap(0)
{
	usb_audio_selector_unit_descriptor* Selector
		= (usb_audio_selector_unit_descriptor*) Header;

	fID = Selector->unit_id;
	TRACE(UAC, "Selector ID:%d >>>\n", fID);

	TRACE(UAC, "Number of input pins:%d\n", Selector->num_input_pins);
	for (size_t i = 0; i < Selector->num_input_pins; i++) {
		fInputPins.PushBack(Selector->input_pins[i]);
		TRACE(UAC, "Input pin #%d:%d\n", i, fInputPins[i]);
	}

	if (fInterface->SpecReleaseNumber() < 0x200) {
		fStringIndex = Selector->input_pins[Selector->num_input_pins];
	} else {
		fControlsBitmap = Selector->input_pins[Selector->num_input_pins];
		fStringIndex = Selector->input_pins[Selector->num_input_pins + 1];

		TRACE(UAC, "Controls Bitmap:%d\n", fControlsBitmap);
	}

	TRACE(UAC, "StringIndex:%d\n", fStringIndex);

	fStatus = B_OK;
}


SelectorUnit::~SelectorUnit()
{
}


AudioChannelCluster*
SelectorUnit::OutCluster()
{
	if (fInterface == NULL)
		return NULL;

	for (int i = 0; i < fInputPins.Count(); i++) {
		_AudioControl* control = fInterface->Find(fInputPins[i]);
		if (control == NULL)
			continue;
		// selector has the same channels number in the
		// out cluster as anyone of his inputs
		if (control->OutCluster() != NULL)
			return control->OutCluster();
	}

	return NULL;
}


FeatureUnit::FeatureUnit(AudioControlInterface*	interface,
		usb_audiocontrol_header_descriptor* Header)
	:
	_AudioControl(interface, Header)
{
	usb_audio_feature_unit_descriptor* Feature
		= (usb_audio_feature_unit_descriptor*) Header;

	fID = Feature->unit_id;
	TRACE(UAC, "Feature ID:%d >>>\n", fID);

	fSourceID = Feature->source_id;
	TRACE(UAC, "Source ID:%d\n", fSourceID);

	uint8 controlSize = 4;
	uint8 channelsCount = (Feature->length - 6) / controlSize;
	uint8* ControlsBitmapPointer = (uint8*)&Feature->r2.bma_controls[0];

	if (fInterface->SpecReleaseNumber() < 0x200) {
		controlSize = Feature->r1.control_size;
		channelsCount = (Feature->length - 7) / Feature->r1.control_size;
		ControlsBitmapPointer = &Feature->r1.bma_controls[0];
	}

	TRACE(UAC, "Channel bitmap size:%d\n", controlSize);
	TRACE(UAC, "Channels number:%d\n", channelsCount - 1); // not add master!

	for (size_t i = 0; i < channelsCount; i++) {
		uint8* controlPointer = &ControlsBitmapPointer[i* controlSize];
		switch(controlSize) {
			case 1: fControlBitmaps.PushBack(*controlPointer); break;
			case 2: fControlBitmaps.PushBack(*(uint16*)controlPointer); break;
			case 4: fControlBitmaps.PushBack(*(uint32*)controlPointer); break;
			default:
				TRACE(ERR, "Feature control of unsupported size %d ignored\n",
														controlSize);
				continue;
		}

		NormalizeAndTraceChannel(i);
	}

	fStringIndex = ControlsBitmapPointer[channelsCount* controlSize];
	TRACE(UAC, "StringIndex:%d\n", fStringIndex);

	fStatus = B_OK;
}


FeatureUnit::~FeatureUnit()
{
}


const char*
FeatureUnit::Name()
{
	// first check if source of this FU is an input terminal
	_AudioControl* control = fInterface->Find(fSourceID);
	while (control != NULL) {
		if (control->SubType() != USB_AUDIO_AC_INPUT_TERMINAL)
			break;

		// USB I/O terminal is a not good candidate to use it's name
		if (static_cast<_Terminal*>(control)->IsUSBIO())
			break;

		// use the name of source input terminal as name of this FU
		return control->Name();
	}

	// check if output of this FU is connected to output terminal
	control = fInterface->FindOutputTerminal(fID);
	while (control != NULL) {
		if (control->SubType() != USB_AUDIO_AC_OUTPUT_TERMINAL)
			break;

		// USB I/O terminal is a not good candidate to use it's name
		if (static_cast<_Terminal*>(control)->IsUSBIO())
			break;

		// use the name of this output terminal as name of this FU
		return control->Name();
	}

	// otherwise get the generic name of this FU's source
	control = fInterface->Find(fSourceID);
	if (control != NULL && control->Name() != NULL
			&& strlen(control->Name()) > 0)
		return control->Name();

	// I have no more ideas, have you one?
	return "Unknown";
}


bool
FeatureUnit::HasControl(int32 Channel, uint32 Control)
{
	if (Channel >= fControlBitmaps.Count()) {
		TRACE(ERR, "Out of limits error of retrieving control %#010x "
			"for channel %d\n", Control, Channel);
		return false;
	}

	return (Control & fControlBitmaps[Channel]) != 0;
}


void
FeatureUnit::NormalizeAndTraceChannel(int32 Channel)
{
	if (Channel >= fControlBitmaps.Count()) {
		TRACE(ERR, "Out of limits error of tracing channel %d\n", Channel);
		return;
	}

	struct _RemapInfo {
		uint32	rev1Bits;
		uint32	rev2Bits;
		const char* name;
	} remapInfos[] = {
		{ BMA_CTL_MUTE_R1,		BMA_CTL_MUTE,			"Mute"		},
		{ BMA_CTL_VOLUME_R1,	BMA_CTL_VOLUME,			"Volume"	},
		{ BMA_CTL_BASS_R1,		BMA_CTL_BASS,			"Bass"		},
		{ BMA_CTL_MID_R1,		BMA_CTL_MID,			"Mid"		},
		{ BMA_CTL_TREBLE_R1,	BMA_CTL_TREBLE,			"Treble"	},
		{ BMA_CTL_GRAPHEQ_R1,	BMA_CTL_GRAPHEQ,		"Graphic Equalizer"	},
		{ BMA_CTL_AUTOGAIN_R1,	BMA_CTL_AUTOGAIN,		"Automatic Gain"},
		{ BMA_CTL_DELAY_R1,		BMA_CTL_DELAY,			"Delay"			},
		{ BMA_CTL_BASSBOOST_R1,	BMA_CTL_BASSBOOST,		"Bass Boost"	},
		{ BMA_CTL_LOUDNESS_R1,	BMA_CTL_LOUDNESS,		"Loudness"		},
		{ 0,					BMA_CTL_INPUTGAIN,		"InputGain"		},
		{ 0,					BMA_CTL_INPUTGAINPAD,	"InputGainPad"	},
		{ 0,					BMA_CTL_PHASEINVERTER,	"PhaseInverter"	},
		{ 0,					BMA_CTL_UNDERFLOW,		"Underflow"		},
		{ 0,					BMA_CTL_OVERFLOW,		"Overflow"		}
	};

	if (Channel == 0)
		TRACE(UAC, "Master channel bitmap:%#x\n", fControlBitmaps[Channel]);
	else
		TRACE(UAC, "Channel %d bitmap:%#x\n", Channel, fControlBitmaps[Channel]);

	bool isRev1 = (fInterface->SpecReleaseNumber() < 0x200);

	uint32 remappedBitmap = 0;
	for (size_t i = 0; i < B_COUNT_OF(remapInfos); i++) {
		uint32 bits = isRev1 ? remapInfos[i].rev1Bits : remapInfos[i].rev2Bits;
		if ((fControlBitmaps[Channel] & bits) > 0) {
			if (isRev1)
				remappedBitmap |= remapInfos[i].rev2Bits;
			TRACE(UAC, "\t%s\n", remapInfos[i].name);
		}
	}

	if (isRev1) {
		TRACE(UAC, "\t%#08x -> %#08x.\n",
			fControlBitmaps[Channel], remappedBitmap);
		fControlBitmaps[Channel] = remappedBitmap;
	}
}


EffectUnit::EffectUnit(AudioControlInterface*	interface,
		usb_audiocontrol_header_descriptor* Header)
	:
	_AudioControl(interface, Header)
{
	usb_audio_input_terminal_descriptor* descriptor
		= (usb_audio_input_terminal_descriptor*) Header;
	TRACE(UAC, "Effect Unit:%d >>>\n", descriptor->terminal_id);
}


EffectUnit::~EffectUnit()
{
}


ProcessingUnit::ProcessingUnit(AudioControlInterface*	interface,
		usb_audiocontrol_header_descriptor* Header)
	:
	_AudioChannelCluster<_AudioControl>(interface, Header),
	fProcessType(0),
	fControlsBitmap(0)
{
	usb_audio_processing_unit_descriptor* Processing
		= (usb_audio_processing_unit_descriptor*) Header;

	fID = Processing->unit_id;
	TRACE(UAC, "Processing ID:%d >>>\n", fID);

	fProcessType = Processing->process_type;
	TRACE(UAC, "Processing Type:%d\n", fProcessType);

	TRACE(UAC, "Number of input pins:%d\n", Processing->num_input_pins);
	for (size_t i = 0; i < Processing->num_input_pins; i++) {
		fInputPins.PushBack(Processing->input_pins[i]);
		TRACE(UAC, "Input pin #%d:%d\n", i, fInputPins[i]);
	}

	if (fInterface->SpecReleaseNumber() < 0x200) {
		usb_audio_output_channels_descriptor_r1* OutChannels
			= (usb_audio_output_channels_descriptor_r1*)
			&Processing->input_pins[Processing->num_input_pins];

		fOutChannelsNumber	= OutChannels->num_output_pins;
		fChannelsConfig		= OutChannels->channel_config;
		fChannelNames		= OutChannels->channel_names;
	} else {
		usb_audio_output_channels_descriptor* OutChannels
			= (usb_audio_output_channels_descriptor*)
			&Processing->input_pins[Processing->num_input_pins];

		fOutChannelsNumber	= OutChannels->num_output_pins;
		fChannelsConfig		= OutChannels->channel_config;
		fChannelNames		= OutChannels->channel_names;
	}

	TRACE(UAC, "Out channels number:%d\n",		fOutChannelsNumber);
	TRACE(UAC, "Out channels config:%#06x\n",	fChannelsConfig);
	TRACE(UAC, "Out channels names:%d\n",		fChannelNames);
	/*
	uint8 controlsSize = Processing->length - 10 - Processing->num_input_pins;
	TRACE(UAC, "Controls Size:%d\n", controlsSize);

	uint8* controlsData = (uint8*) ++OutChannels;

	for (size_t i = 0; i < controlsSize; i++) {
		fProgrammableControls.PushBack(controlsData[i]);
		TRACE(UAC, "Controls Data[%d]:%#x\n", i, controlsData[i]);
	}

	fStringIndex = *(controlsData + controlsSize);

	TRACE(UAC, "StringIndex:%d\n", fStringIndex);
*/
	fStatus = B_OK;
}


ProcessingUnit::~ProcessingUnit()
{
}


ExtensionUnit::ExtensionUnit(AudioControlInterface*	interface,
		usb_audiocontrol_header_descriptor* Header)
	:
	_AudioChannelCluster<_AudioControl>(interface, Header),
	fExtensionCode(0),
	fControlsBitmap(0)
{
	usb_audio_extension_unit_descriptor* Extension
		= (usb_audio_extension_unit_descriptor*) Header;

	fID = Extension->unit_id;
	TRACE(UAC, "Extension ID:%d >>>\n", fID);

	fExtensionCode = Extension->extension_code;
	TRACE(UAC, "Extension Type:%d\n", fExtensionCode);

	TRACE(UAC, "Number of input pins:%d\n", Extension->num_input_pins);
	for (size_t i = 0; i < Extension->num_input_pins; i++) {
		fInputPins.PushBack(Extension->input_pins[i]);
		TRACE(UAC, "Input pin #%d:%d\n", i, fInputPins[i]);
	}

	if (fInterface->SpecReleaseNumber() < 0x200) {
		usb_audio_output_channels_descriptor_r1* OutChannels
			= (usb_audio_output_channels_descriptor_r1*)
			&Extension->input_pins[Extension->num_input_pins];

		fOutChannelsNumber	= OutChannels->num_output_pins;
		fChannelsConfig		= OutChannels->channel_config;
		fChannelNames		= OutChannels->channel_names;
	} else {
		usb_audio_output_channels_descriptor* OutChannels
			= (usb_audio_output_channels_descriptor*)
			&Extension->input_pins[Extension->num_input_pins];

		fOutChannelsNumber	= OutChannels->num_output_pins;
		fChannelsConfig		= OutChannels->channel_config;
		fChannelNames		= OutChannels->channel_names;
	}

	TRACE(UAC, "Out channels number:%d\n",		fOutChannelsNumber);
	TRACE(UAC, "Out channels config:%#06x\n",	fChannelsConfig);
	TRACE(UAC, "Out channels names:%d\n",		fChannelNames);
	/*
	uint8 controlsSize = Processing->length - 10 - Processing->num_input_pins;
	TRACE(UAC, "Controls Size:%d\n", controlsSize);

	uint8* controlsData = (uint8*) ++OutChannels;

	for (size_t i = 0; i < controlsSize; i++) {
		fProgrammableControls.PushBack(controlsData[i]);
		TRACE(UAC, "Controls Data[%d]:%#x\n", i, controlsData[i]);
	}

	fStringIndex = *(controlsData + controlsSize);

	TRACE(UAC, "StringIndex:%d\n", fStringIndex);
*/
	fStatus = B_OK;
}


ExtensionUnit::~ExtensionUnit()
{
}


ClockSource::ClockSource(AudioControlInterface*	interface,
		usb_audiocontrol_header_descriptor* Header)
	:
	_AudioControl(interface, Header),
	fClockType(0),
	fControlsBitmap(0)
{
	// Clock Source descriptor is a fixed 8 bytes (Audio20 Table 4-6, page 49).
	if (Header->length < sizeof(usb_audio_clocksource_descriptor)) {
		TRACE(ERR, "Clock Source descriptor too short: %d\n", Header->length);
		return;
	}

	usb_audio_clocksource_descriptor* Descriptor
		= (usb_audio_clocksource_descriptor*) Header;

	fID				= Descriptor->clock_id;
	fClockType		= Descriptor->bm_attributes;
	fControlsBitmap	= Descriptor->bm_controls;
	fStringIndex	= Descriptor->clock_source_idx;

	TRACE(UAC, "Clock Source ID:%d >>>\n",	fID);
	TRACE(UAC, "Attributes:%#04x\n",		fClockType);
	TRACE(UAC, "Controls Bitmap:%#04x\n",	fControlsBitmap);
	TRACE(UAC, "StringIndex:%d\n",			fStringIndex);

	fStatus = B_OK;
}


ClockSource::~ClockSource()
{
}


bool
ClockSource::SamplingFrequencyReadable()
{
	// bmControls bits [1:0] hold the Clock Frequency Control: 0b01 read-only,
	// 0b11 read/write (Audio20 Table 4-6).
	return (fControlsBitmap & 0x03) != 0;
}


bool
ClockSource::SamplingFrequencyWritable()
{
	return (fControlsBitmap & 0x03) == 0x03;
}


ClockSelector::ClockSelector(AudioControlInterface*	interface,
		usb_audiocontrol_header_descriptor* Header)
	:
	_AudioControl(interface, Header),
	fControlsBitmap(0)
{
	// Fixed part is 5 bytes (length, type, subtype, clock_id, nrinpins),
	// followed by nrinpins source IDs, then bmControls and iClockSelector.
	if (Header->length < 5) {
		TRACE(ERR, "Clock Selector descriptor too short: %d\n", Header->length);
		return;
	}

	usb_audio_clockselector_descriptor* Descriptor
		= (usb_audio_clockselector_descriptor*) Header;

	fID = Descriptor->clock_id;
	uint8 nrInPins = Descriptor->nrinpins;

	// Validate the variable-length pin array plus the trailing two bytes fit
	// inside the descriptor before touching any of it (untrusted input).
	if (5u + nrInPins + 2u > Header->length) {
		TRACE(ERR, "Clock Selector %d: nrInPins %d exceeds length %d\n",
			fID, nrInPins, Header->length);
		return;
	}

	TRACE(UAC, "Clock Selector ID:%d >>>\n", fID);
	TRACE(UAC, "Number of input pins:%d\n", nrInPins);
	for (uint8 i = 0; i < nrInPins; i++) {
		fInputPins.PushBack(Descriptor->Csourceid[i]);
		TRACE(UAC, "Input pin #%d:%d\n", i, fInputPins[i]);
	}

	// bmControls and iClockSelector follow the variable-length pin array. Read
	// them through a byte pointer at their validated offsets (fixed part is 5
	// bytes, then nrInPins source IDs) rather than indexing the single-element
	// Csourceid[] past its declared bound.
	const uint8* base = (const uint8*)Descriptor;
	fControlsBitmap	= base[5 + nrInPins];
	fStringIndex	= base[6 + nrInPins];

	TRACE(UAC, "Controls Bitmap:%#04x\n",	fControlsBitmap);
	TRACE(UAC, "StringIndex:%d\n",			fStringIndex);

	fStatus = B_OK;
}


ClockSelector::~ClockSelector()
{
}


ClockMultiplier::ClockMultiplier(AudioControlInterface*	interface,
		usb_audiocontrol_header_descriptor* Header)
	:
	_AudioControl(interface, Header),
	fControlsBitmap(0)
{
	// Clock Multiplier descriptor is a fixed 7 bytes (Audio20 Table 4-8).
	if (Header->length < sizeof(usb_audio_clockmultiplier_descriptor)) {
		TRACE(ERR, "Clock Multiplier descriptor too short: %d\n", Header->length);
		return;
	}

	usb_audio_clockmultiplier_descriptor* Descriptor
		= (usb_audio_clockmultiplier_descriptor*) Header;

	fID				= Descriptor->clockid;
	fSourceID		= Descriptor->clksourceid;
	fControlsBitmap	= Descriptor->bm_controls;
	fStringIndex	= Descriptor->clockmultiplier;

	TRACE(UAC, "Clock Multiplier ID:%d >>>\n",	fID);
	TRACE(UAC, "Source ID:%d\n",				fSourceID);

	fStatus = B_OK;
}


ClockMultiplier::~ClockMultiplier()
{
}


SampleRateConverter::SampleRateConverter(AudioControlInterface*	interface,
		usb_audiocontrol_header_descriptor* Header)
	:
	_AudioControl(interface, Header)
{
	usb_audio_input_terminal_descriptor* descriptor
		= (usb_audio_input_terminal_descriptor*) Header;
	TRACE(UAC, "Sample Rate Converter:%d >>>\n", descriptor->terminal_id);
}


SampleRateConverter::~SampleRateConverter()
{
}


AudioControlInterface::AudioControlInterface(Device* device)
	:
	fInterface(0),
	fStatus(B_NO_INIT),
	fADCSpecification(0),
	fFunctionCategory(0),
	fControlsBitmap(0),
	fLastClockId(0),
	fLastClockRate(0),
	fDevice(device)
{
}


AudioControlInterface::~AudioControlInterface()
{
	for (AudioControlsIterator I = fAudioControls.Begin();
			I != fAudioControls.End(); I++)
		delete I->Value();

	fAudioControls.MakeEmpty();

	// object already freed. just purge the map
	fOutputTerminals.MakeEmpty();

	// object already freed. just purge the map
	fInputTerminals.MakeEmpty();
}


status_t
AudioControlInterface::Init(size_t interface, usb_interface_info* Interface)
{
	for (size_t i = 0; i < Interface->generic_count; i++) {
		usb_audiocontrol_header_descriptor* Header
			= (usb_audiocontrol_header_descriptor* )Interface->generic[i];

		if (Header->descriptor_type != USB_AUDIO_CS_INTERFACE) {
			TRACE(ERR, "Ignore Audio Control of "
				"unknown descriptor type %#04x.\n",	Header->descriptor_type);
			continue;
		}

		_AudioControl* control = NULL;

		switch(Header->descriptor_subtype) {
			default:
				TRACE(ERR, "Ignore Audio Control of unknown "
					"descriptor sub-type %#04x\n", Header->descriptor_subtype);
				break;
			case USB_AUDIO_AC_DESCRIPTOR_UNDEFINED:
				TRACE(ERR, "Ignore Audio Control of undefined sub-type\n");
				break;
			case USB_AUDIO_AC_HEADER:
				InitACHeader(interface, Header);
				break;
			case USB_AUDIO_AC_INPUT_TERMINAL:
				control = new(std::nothrow) InputTerminal(this, Header);
				break;
			case USB_AUDIO_AC_OUTPUT_TERMINAL:
				control = new(std::nothrow) OutputTerminal(this, Header);
				break;
			case USB_AUDIO_AC_MIXER_UNIT:
				control = new(std::nothrow) MixerUnit(this, Header);
				break;
			case USB_AUDIO_AC_SELECTOR_UNIT:
				control = new(std::nothrow) SelectorUnit(this, Header);
				break;
			case USB_AUDIO_AC_FEATURE_UNIT:
				control = new(std::nothrow) FeatureUnit(this, Header);
				break;
			case USB_AUDIO_AC_PROCESSING_UNIT:
				// subtype 0x07 is PROCESSING_UNIT in R1 but EFFECT_UNIT in R2;
				// disambiguate on the AudioControl spec version (bcdADC).
				if (SpecReleaseNumber() < 0x200)
					control = new(std::nothrow) ProcessingUnit(this, Header);
				else
					control = new(std::nothrow) EffectUnit(this, Header);
				break;
			case USB_AUDIO_AC_EXTENSION_UNIT:
				// subtype 0x08 is EXTENSION_UNIT in R1 but PROCESSING_UNIT in R2.
				if (SpecReleaseNumber() < 0x200)
					control = new(std::nothrow) ExtensionUnit(this, Header);
				else
					control = new(std::nothrow) ProcessingUnit(this, Header);
				break;
			case USB_AUDIO_AC_EXTENSION_UNIT_R2:
				control = new(std::nothrow) ExtensionUnit(this, Header);
				break;
			case USB_AUDIO_AC_CLOCK_SOURCE_R2:
				control = new(std::nothrow) ClockSource(this, Header);
				break;
			case USB_AUDIO_AC_CLOCK_SELECTOR_R2:
				control = new(std::nothrow) ClockSelector(this, Header);
				break;
			case USB_AUDIO_AC_CLOCK_MULTIPLIER_R2:
				control = new(std::nothrow) ClockMultiplier(this, Header);
				break;
			case USB_AUDIO_AC_SAMPLE_RATE_CONVERTER_R2:
				control = new(std::nothrow) SampleRateConverter(this, Header);
				break;
		}

		if (control != 0 && control->InitCheck() == B_OK) {
			switch(control->SubType()) {
				case USB_AUDIO_AC_OUTPUT_TERMINAL:
					fOutputTerminals.Put(control->SourceID(), control);
					break;
				case USB_AUDIO_AC_INPUT_TERMINAL:
					fInputTerminals.Put(control->ID(), control);
					break;
			}
			fAudioControls.Put(control->ID(), control);

		} else
			delete control;
	}

	return fStatus = B_OK;
}


_AudioControl*
AudioControlInterface::Find(uint8 id)
{
	return fAudioControls.Get(id);
}


_AudioControl*
AudioControlInterface::FindOutputTerminal(uint8 id)
{
	return fOutputTerminals.Get(id);
}


status_t
AudioControlInterface::InitACHeader(size_t interface,
		usb_audiocontrol_header_descriptor* Header)
{
	if (Header == NULL)
		return fStatus = B_NO_INIT;

	uint16 specReleaseNumber = Header->bcd_release_no;
	TRACE(UAC, "ADCSpecification:%#06x\n", specReleaseNumber);

	// A composite device may expose more than one AudioControl interface - for
	// example a UAC2 audio function together with a UAC1 MIDI function - which
	// are all parsed into this single object. Keep the primary (highest bcdADC)
	// interface: a UAC1 MIDI companion must not downgrade the spec version or
	// override the interface number used to address the audio function's class
	// requests (sampling frequency and feature-unit controls). Each header body
	// below is still parsed according to its own version.
	if (specReleaseNumber > fADCSpecification) {
		fADCSpecification = specReleaseNumber;
		fInterface = interface;
	}

	if (specReleaseNumber < 0x200) {
		TRACE(UAC, "InterfacesCount:%d\n",	Header->r1.in_collection);
		for (size_t i = 0; i < Header->r1.in_collection; i++) {
			fStreams.PushBack(Header->r1.interface_numbers[i]);
			TRACE(UAC, "Interface[%d] number is %d\n", i, fStreams[i]);
		}
	} else {
		fFunctionCategory = Header->r2.function_category;
		fControlsBitmap = Header->r2.bm_controls;
		TRACE(UAC, "Function Category:%#04x\n", fFunctionCategory);
		TRACE(UAC, "Controls Bitmap:%#04x\n", fControlsBitmap);
	}

	return B_OK;
}


/*!	Whether a Clock Source's frequency can be set by the host at all.

	An externally clocked source -- S/PDIF or ADAT locked to the incoming stream
	-- reports its rate read-only, because nothing the host says can change it.
	Those are also exactly the devices whose input and output sit on *different*
	clocks, so this and SharedClockId() tend to matter together.

	Unknown clock ids answer true, so a device we failed to parse behaves as it
	did before rather than silently never being programmed.
*/
bool
AudioControlInterface::ClockRateIsWritable(uint8 clockId)
{
	_AudioControl* control = Find(clockId);
	if (control == NULL || control->SubType() != USB_AUDIO_AC_CLOCK_SOURCE_R2)
		return true;

	return static_cast<ClockSource*>(control)->SamplingFrequencyWritable();
}


/*!	Which input a Clock Selector is currently routing.

	Asking matters because the answer is used to decide whether two streams sit
	on the SAME clock (see Device::SharedClockId()). Assuming input pin 0 is good
	enough to obtain *a* usable rate -- which is all this code used to need -- but
	it is not good enough to compare two terminals: two selectors both reported as
	"pin 0" may in fact be routing different sources, and we would lock the rates
	of two independent clocks together.

	A GET CUR on the Clock Selector Control returns a 1-based index into the
	selector's input pins (Audio20 5.2.4.4). Anything unexpected falls back to the
	first pin, which is the previous behaviour.
*/
uint8
AudioControlInterface::_ActiveClockSelectorInput(ClockSelector* selector,
	uint8 selectorId)
{
	uint8 selection = 0;
	size_t actualLength = 0;
	uint16 value = USB_AUDIO_CX_CLOCK_SELECTOR_CONTROL << 8;
	uint16 index = (selectorId << 8) | (fInterface & 0xff);

	status_t status = gUSBModule->send_request(fDevice->USBDevice(),
		USB_REQTYPE_INTERFACE_IN | USB_REQTYPE_CLASS, USB_AUDIO_R2_CUR,
		value, index, sizeof(selection), &selection, &actualLength);

	if (status != B_OK || actualLength != sizeof(selection)
			|| selection < 1 || selection > selector->fInputPins.Count()) {
		TRACE(UAC, "Clock selector %d: no usable CUR (status %#010x, len %ld, "
			"value %d of %d pins); assuming the first input.\n", selectorId,
			status, actualLength, selection,
			(int)selector->fInputPins.Count());
		return selector->fInputPins[0];
	}

	return selector->fInputPins[selection - 1];
}


uint8
AudioControlInterface::ClockSourceIdForTerminal(uint8 terminalId)
{
	// In UAC2 a stream's terminal references a clock entity that may be a Clock
	// Source directly, or a Clock Selector / Multiplier that ultimately leads to
	// one. Walk that (untrusted) graph with a bounded number of hops, resolving
	// to the Clock Source entity ID used to address sampling-frequency requests.
	_AudioControl* control = Find(terminalId);
	if (control == NULL) {
		TRACE(ERR, "No terminal %d to resolve clock source.\n", terminalId);
		return 0;
	}

	if (control->SubType() != USB_AUDIO_AC_INPUT_TERMINAL
			&& control->SubType() != USB_AUDIO_AC_OUTPUT_TERMINAL)
		return 0;

	uint8 clockId = static_cast<_Terminal*>(control)->ClockSourceId();

	for (int hops = 0; hops < 8; hops++) {
		_AudioControl* clock = Find(clockId);
		if (clock == NULL)
			return 0;

		switch (clock->SubType()) {
			case USB_AUDIO_AC_CLOCK_SOURCE_R2:
				return clockId;

			case USB_AUDIO_AC_CLOCK_SELECTOR_R2:
			{
				ClockSelector* selector = static_cast<ClockSelector*>(clock);
				if (selector->fInputPins.Count() <= 0)
					return 0;
				clockId = _ActiveClockSelectorInput(selector, clockId);
				if (clockId == 0)
					return 0;
				break;
			}

			case USB_AUDIO_AC_CLOCK_MULTIPLIER_R2:
				clockId = clock->SourceID();
				break;

			default:
				return 0;
		}
	}

	TRACE(ERR, "Clock graph for terminal %d too deep; giving up.\n", terminalId);
	return 0;
}


status_t
AudioControlInterface::GetSamplingRates(uint8 clockId, Vector<uint32>& rates)
{
	// A RANGE(SAM_FREQ) request on a Clock Source returns wNumSubRanges followed
	// by that many {dMIN, dMAX, dRES} 4-byte triples (Audio20 5.2.3.2). Read the
	// 2-byte count first, then size the second read from it; every count/length
	// is device-supplied and bounds-checked before use.
	uint16 numSubRanges = 0;
	size_t actualLength = 0;
	uint16 value = USB_AUDIO_CS_CONTROL_SAM_FREQ << 8;
	uint16 index = (clockId << 8) | (fInterface & 0xff);

	status_t status = gUSBModule->send_request(fDevice->USBDevice(),
		USB_REQTYPE_INTERFACE_IN | USB_REQTYPE_CLASS, USB_AUDIO_R2_RANGE,
		value, index, sizeof(numSubRanges), &numSubRanges, &actualLength);

	if (status != B_OK || actualLength != sizeof(numSubRanges)) {
		TRACE(ERR, "Clock %d RANGE count request failed:%#010x (%d bytes)\n",
			clockId, status, actualLength);
		return status != B_OK ? status : B_ERROR;
	}

	if (numSubRanges == 0 || numSubRanges > 255) {
		TRACE(ERR, "Clock %d reports invalid sub-range count %d\n",
			clockId, numSubRanges);
		return B_BAD_DATA;
	}

	size_t blockSize = sizeof(uint16)
		+ numSubRanges * sizeof(usb_audio_range4_sub);
	usb_audio_range4* block = (usb_audio_range4*)malloc(blockSize);
	if (block == NULL)
		return B_NO_MEMORY;

	status = gUSBModule->send_request(fDevice->USBDevice(),
		USB_REQTYPE_INTERFACE_IN | USB_REQTYPE_CLASS, USB_AUDIO_R2_RANGE,
		value, index, blockSize, block, &actualLength);

	if (status != B_OK || actualLength < sizeof(uint16)) {
		TRACE(ERR, "Clock %d RANGE data request failed:%#010x\n", clockId, status);
		free(block);
		return status != B_OK ? status : B_ERROR;
	}

	// Trust only as many triples as actually arrived.
	uint16 got = block->num_sub_ranges;
	size_t maxFit = (actualLength - sizeof(uint16))
		/ sizeof(usb_audio_range4_sub);
	if (got > maxFit)
		got = maxFit;

	const int kMaxRates = 64;
	for (uint16 i = 0; i < got && rates.Count() < kMaxRates; i++) {
		uint32 min = block->subranges[i].min;
		uint32 max = block->subranges[i].max;
		uint32 res = block->subranges[i].res;

		if (max < min)
			continue;

		if (res == 0) {
			// A discrete frequency is reported as min == max (res unused).
			rates.PushBack(min);
			if (max != min)
				rates.PushBack(max);
			continue;
		}

		// Enumerate the inclusive [min, max] range in res steps, guarding
		// against overflow wrap and unbounded device-driven iteration.
		for (uint32 rate = min; rates.Count() < kMaxRates; ) {
			rates.PushBack(rate);
			uint32 next = rate + res;
			if (next < rate || next > max)
				break;
			rate = next;
		}
	}

	free(block);
	return B_OK;
}


status_t
AudioControlInterface::SetSamplingRate(uint8 clockId, uint32 rate)
{
	// SET CUR(SAM_FREQ) on the Clock Source, addressed by clock entity ID in
	// wIndex high byte and this AudioControl interface in the low byte. The
	// value is a 4-byte little-endian frequency (Audio20 5.2.5.1).
	uint32 data = rate;
	size_t actualLength = 0;
	uint16 value = USB_AUDIO_CS_CONTROL_SAM_FREQ << 8;
	uint16 index = (clockId << 8) | (fInterface & 0xff);

	status_t status = gUSBModule->send_request(fDevice->USBDevice(),
		USB_REQTYPE_INTERFACE_OUT | USB_REQTYPE_CLASS, USB_AUDIO_R2_CUR,
		value, index, sizeof(data), &data, &actualLength);

	if (status == B_OK) {
		// Remembered so a device reattach can restore the clock exactly as
		// it was programmed (see Device::CompareAndReattach()).
		fLastClockId = clockId;
		fLastClockRate = rate;
	}

	TRACE(INF, "Set clock %d sampling rate %d: %s (%d bytes)\n",
		clockId, rate, strerror(status), actualLength);

	return status;
}


void
AudioControlInterface::RouteClockSelectors()
{
	// R2: explicitly route every writable Clock Selector to its first input
	// pin instead of relying on the power-up default. A selector left
	// unrouted feeds the streaming terminals no clock at all - accepted
	// requests, healthy-looking streams, complete silence. Working hosts
	// (Linux snd-usb-audio, RMX2 usbmon capture 2026-07-06) rewrite the
	// selector at every stream open even when it already reads back right,
	// so this is also part of the stream-start sequence, not just probe.
	for (AudioControlsIterator I = fAudioControls.Begin();
			I != fAudioControls.End(); I++) {
		_AudioControl* control = I->Value();
		if (control->SubType() != USB_AUDIO_AC_CLOCK_SELECTOR_R2)
			continue;

		uint16 value = USB_AUDIO_CX_CLOCK_SELECTOR_CONTROL << 8;
		uint16 index = (control->ID() << 8) | fInterface;
		size_t actualLength = 0;
		uint8 pin = 0xff;
		status_t getStatus = gUSBModule->send_request(
			fDevice->USBDevice(),
			USB_REQTYPE_INTERFACE_IN | USB_REQTYPE_CLASS,
			USB_AUDIO_R2_CUR, value, index,
			sizeof(pin), &pin, &actualLength);
		uint8 firstPin = 1;
		status_t setStatus = gUSBModule->send_request(
			fDevice->USBDevice(),
			USB_REQTYPE_INTERFACE_OUT | USB_REQTYPE_CLASS,
			USB_AUDIO_R2_CUR, value, index,
			sizeof(firstPin), &firstPin, &actualLength);
		TRACE(INF, "clock selector %d: was pin %d (%#x),"
			" set pin 1 -> %#x\n", control->ID(), (int)pin,
			(int)getStatus, (int)setStatus);
	}
}


void
AudioControlInterface::InitHardwareGains()
{
	// Some devices (Hercules DJ Console RMX) power up with their feature
	// units muted / volume at minimum and stay silent although USB streaming
	// is perfectly healthy. Probe and log every feature-unit channel, then
	// unmute it and open the volume (to 0 dB, or the channel maximum if the
	// range is attenuation-only).
	//
	// The R1 and R2 request sets differ: R1 uses GET_CUR/GET_MIN/GET_MAX/
	// SET_CUR, R2 uses CUR (direction from bmRequestType) and RANGE. The
	// wValue/wIndex encoding and the volume units (1/256 dB int16) are the
	// same in both.
	bool isRev2 = SpecReleaseNumber() >= 0x200;

	if (isRev2)
		RouteClockSelectors();

	for (AudioControlsIterator I = fAudioControls.Begin();
			I != fAudioControls.End(); I++) {
		_AudioControl* control = I->Value();
		if (control->SubType() != USB_AUDIO_AC_FEATURE_UNIT)
			continue;

		FeatureUnit* unit = static_cast<FeatureUnit*>(control);
		uint16 index = (unit->ID() << 8) | fInterface;

		for (int32 channel = 0; channel < unit->fControlBitmaps.Count();
				channel++) {
			size_t actualLength = 0;
			status_t status;

			if (isRev2) {
				if (unit->HasControl(channel, USB_AUDIO_MUTE_CONTROL)) {
					uint16 value = (USB_AUDIO_MUTE_CONTROL << 8) | channel;
					uint8 mute = 0xff;
					status = gUSBModule->send_request(fDevice->USBDevice(),
						USB_REQTYPE_INTERFACE_IN | USB_REQTYPE_CLASS,
						USB_AUDIO_R2_CUR, value, index,
						sizeof(mute), &mute, &actualLength);
					uint8 unmute = 0;
					status_t setStatus = gUSBModule->send_request(
						fDevice->USBDevice(),
						USB_REQTYPE_INTERFACE_OUT | USB_REQTYPE_CLASS,
						USB_AUDIO_R2_CUR, value, index,
						sizeof(unmute), &unmute, &actualLength);
					TRACE(INF, "FU%d ch%ld mute(R2): was %d (get %#x),"
						" unmute -> %#x\n", unit->ID(), (long)channel,
						(int)mute, (int)status, (int)setStatus);
				}

				if (unit->HasControl(channel, USB_AUDIO_VOLUME_CONTROL)) {
					uint16 value = (USB_AUDIO_VOLUME_CONTROL << 8) | channel;
					int16 cur = 0;
					int16 min = 0;
					int16 max = 0;
					gUSBModule->send_request(fDevice->USBDevice(),
						USB_REQTYPE_INTERFACE_IN | USB_REQTYPE_CLASS,
						USB_AUDIO_R2_CUR, value, index,
						sizeof(cur), &cur, &actualLength);

					// RANGE returns wNumSubRanges plus int16 {min,max,res}
					// triples; the first sub-range bounds the control.
					struct {
						uint16 count;
						usb_audio_range2_sub sub[1];
					} _PACKED range = {};
					status = gUSBModule->send_request(fDevice->USBDevice(),
						USB_REQTYPE_INTERFACE_IN | USB_REQTYPE_CLASS,
						USB_AUDIO_R2_RANGE, value, index,
						sizeof(range), &range, &actualLength);
					if (status == B_OK && range.count >= 1) {
						min = range.sub[0].min;
						max = range.sub[0].max;
					}

					// Some devices answer the range the wrong way round, so
					// normalise before believing either end - see the R1 path
					// below for the device that made this necessary.
					if (min > max) {
						int16 swap = min;
						min = max;
						max = swap;
					}

					// Volume units are 1/256 dB; 0 means 0 dB (unity). Do
					// not engage amplification beyond unity if offered.
					int16 target = max > 0 ? 0 : max;
					if (target < min)
						target = min;
					status = gUSBModule->send_request(fDevice->USBDevice(),
						USB_REQTYPE_INTERFACE_OUT | USB_REQTYPE_CLASS,
						USB_AUDIO_R2_CUR, value, index,
						sizeof(target), &target, &actualLength);
					TRACE(INF, "FU%d ch%ld volume(R2): cur=%d min=%d"
						" max=%d, set %d -> %#x\n", unit->ID(), (long)channel,
						(int)cur, (int)min, (int)max, (int)target,
						(int)status);
				}
				continue;
			}

			if (unit->HasControl(channel, USB_AUDIO_MUTE_CONTROL)) {
				uint16 value = (USB_AUDIO_MUTE_CONTROL << 8) | channel;
				uint8 mute = 0xff;
				status = gUSBModule->send_request(fDevice->USBDevice(),
					USB_REQTYPE_INTERFACE_IN | USB_REQTYPE_CLASS,
					USB_AUDIO_GET_CUR, value, index,
					sizeof(mute), &mute, &actualLength);
				uint8 unmute = 0;
				status_t setStatus = gUSBModule->send_request(
					fDevice->USBDevice(),
					USB_REQTYPE_INTERFACE_OUT | USB_REQTYPE_CLASS,
					USB_AUDIO_SET_CUR, value, index,
					sizeof(unmute), &unmute, &actualLength);
				TRACE(INF, "FU%d ch%ld mute: was %d (get %#x),"
					" unmute -> %#x\n", unit->ID(), (long)channel,
					(int)mute, (int)status, (int)setStatus);
			}

			if (unit->HasControl(channel, USB_AUDIO_VOLUME_CONTROL)) {
				uint16 value = (USB_AUDIO_VOLUME_CONTROL << 8) | channel;
				int16 cur = 0;
				int16 min = 0;
				int16 max = 0;
				gUSBModule->send_request(fDevice->USBDevice(),
					USB_REQTYPE_INTERFACE_IN | USB_REQTYPE_CLASS,
					USB_AUDIO_GET_CUR, value, index,
					sizeof(cur), &cur, &actualLength);
				gUSBModule->send_request(fDevice->USBDevice(),
					USB_REQTYPE_INTERFACE_IN | USB_REQTYPE_CLASS,
					USB_AUDIO_GET_MIN, value, index,
					sizeof(min), &min, &actualLength);
				gUSBModule->send_request(fDevice->USBDevice(),
					USB_REQTYPE_INTERFACE_IN | USB_REQTYPE_CLASS,
					USB_AUDIO_GET_MAX, value, index,
					sizeof(max), &max, &actualLength);

				// The Denon DN-HC4500 returns GET_MIN and GET_MAX the wrong way
				// round, and not even consistently between enumerations. Taken
				// at face value the target below becomes the quietest setting
				// instead of the loudest, which silences a device whose USB
				// feature unit is its only volume control.
				if (min > max) {
					int16 swap = min;
					min = max;
					max = swap;
				}

				// Volume units are 1/256 dB; 0 means 0 dB (unity). Do not
				// engage amplification beyond unity if the device offers it.
				int16 target = max > 0 ? 0 : max;
				if (target < min)
					target = min;
				status = gUSBModule->send_request(fDevice->USBDevice(),
					USB_REQTYPE_INTERFACE_OUT | USB_REQTYPE_CLASS,
					USB_AUDIO_SET_CUR, value, index,
					sizeof(target), &target, &actualLength);
				TRACE(INF, "FU%d ch%ld volume: cur=%d min=%d max=%d,"
					" set %d -> %#x\n", unit->ID(), (long)channel, (int)cur,
					(int)min, (int)max, (int)target, (int)status);
			}
		}
	}
}


uint32
AudioControlInterface::GetChannelsDescription(
	Vector<multi_channel_info>& Channels, multi_description* Description,
	Vector<_AudioControl*>& Terminals, bool isForInput)
{
	uint32 addedChannels = 0;

	for (int32 i = 0; i < Terminals.Count(); i++) {
		AudioChannelCluster* cluster = Terminals[i]->OutCluster();
		if (cluster == NULL || cluster->ChannelsCount() <= 0) {
			TRACE(ERR, "Terminal #%d ignored due null "
				"channels cluster (%08x)\n", Terminals[i]->ID(), cluster);
			continue;
		}

		uint32 channels = GetTerminalChannels(Channels, cluster,
			isForInput ? B_MULTI_INPUT_CHANNEL : B_MULTI_OUTPUT_CHANNEL);

		if (isForInput)
			Description->input_channel_count += channels;
		else
			Description->output_channel_count += channels;

		addedChannels += channels;
	}

	return addedChannels;
}


uint32
AudioControlInterface::GetTerminalChannels(Vector<multi_channel_info>& Channels,
		AudioChannelCluster* cluster, channel_kind kind, uint32 connectors)
{
	if (cluster->ChannelsCount() < 2) { // mono channel
		multi_channel_info info;
		info.channel_id	= Channels.Count();
		info.kind		= kind;
		info.designations= B_CHANNEL_MONO_BUS;
		info.connectors	= connectors;
		Channels.PushBack(info);

		return 1;
	}

	uint32 startCount = Channels.Count();
	uint32 channelCount = cluster->ChannelsCount();

	// Haiku multi-aduio designations have the same bits
	// as USB Audio 2.0 cluster spatial locations :-)
	// Emit one channel per assigned spatial location, but never more than the
	// cluster's declared channel count (bmChannelConfig is untrusted input).
	for (size_t i = 0; i < kChannels
			&& (uint32)(Channels.Count() - startCount) < channelCount; i++) {
		// The Haiku system mixer connects only to a stereo (Left/Right) output,
		// so expose at most a stereo pair for output terminals; the remaining
		// physical output channels are reached through the driver's output-pair
		// routing, not by advertising them to the mixer (which would then
		// refuse to connect at all). Capture terminals keep every channel.
		// This must match the channel count Stream::GetBuffers reports, or the
		// media node's _WriteZeros() walks past its buffer list and crashes.
		if (kind == B_MULTI_OUTPUT_CHANNEL
				&& (uint32)(Channels.Count() - startCount) >= 2)
			break;
		uint32 designation = 1 << i;
		if ((cluster->ChannelsConfig() & designation) == designation) {
			multi_channel_info info;
			info.channel_id	= Channels.Count();
			info.kind		= kind;
			info.designations= gDesignations[i].ch | gDesignations[i].bus;
			info.connectors	= connectors;
			Channels.PushBack(info);
		}
	}

	// UAC2 allows channels with no spatial location: bmChannelConfig may be 0
	// (or set fewer bits than bNrChannels), which is common on multichannel
	// interfaces. The media stack groups a terminal's channels into stereo/mono
	// buses by their designation, and the hmulti_audio media node sizes the
	// playback buffer assuming a stereo (2-channel) grouping; a single
	// undesignated N-channel group makes it reject every incoming buffer as a
	// size mismatch, so playback is silent. Assign the remaining channels
	// designations in the standard channel order so they form ordinary stereo
	// pairs, matching how the other multichannel drivers present their outputs.
	while ((uint32)(Channels.Count() - startCount) < channelCount) {
		// Same stereo cap for output terminals as above.
		if (kind == B_MULTI_OUTPUT_CHANNEL
				&& (uint32)(Channels.Count() - startCount) >= 2)
			break;
		uint32 index = Channels.Count() - startCount;
		multi_channel_info info;
		info.channel_id	= Channels.Count();
		info.kind		= kind;
		info.designations= index < kChannels
			? gDesignations[index].ch | gDesignations[index].bus : 0;
		info.connectors	= connectors;
		Channels.PushBack(info);
	}

	return Channels.Count() - startCount;
}


uint32
AudioControlInterface::GetBusChannelsDescription(
	Vector<multi_channel_info>& Channels, multi_description* Description)
{
	uint32 addedChannels = 0;

	// first iterate output channels
	for (AudioControlsIterator I = fOutputTerminals.Begin();
			I != fOutputTerminals.End(); I++) {
		_AudioControl* control = I->Value();
		if (static_cast<_Terminal*>(control)->IsUSBIO())
			continue;

		AudioChannelCluster* cluster = control->OutCluster();
		if (cluster == 0 || cluster->ChannelsCount() <= 0) {
			TRACE(ERR, "Terminal #%d ignored due null "
				"channels cluster (%08x)\n", control->ID(), cluster);
			continue;
		}

		uint32 channels = GetTerminalChannels(Channels,
			cluster, B_MULTI_OUTPUT_BUS);

		Description->output_bus_channel_count += channels;
		addedChannels += channels;
	}

	// input channels should follow too
	for (AudioControlsIterator I = fInputTerminals.Begin();
			I != fInputTerminals.End(); I++) {
		_AudioControl* control = I->Value();
		if (static_cast<_Terminal*>(control)->IsUSBIO())
			continue;

		AudioChannelCluster* cluster = control->OutCluster();
		if (cluster == NULL || cluster->ChannelsCount() <= 0) {
			TRACE(ERR, "Terminal #%d ignored due null "
				"channels cluster (%08x)\n", control->ID(), cluster);
			continue;
		}

		uint32 channels = GetTerminalChannels(Channels,
			cluster, B_MULTI_INPUT_BUS);

		Description->input_bus_channel_count += channels;
		addedChannels += channels;
	}

	return addedChannels;
}


void
AudioControlInterface::_HarvestRecordFeatureUnits(_AudioControl* rootControl,
		AudioControlsMap& Map)
{
	if (rootControl == 0) {
		TRACE(ERR, "Not processing due NULL root control.\n");
		return;
	}

	switch(rootControl->SubType()) {
		case USB_AUDIO_AC_SELECTOR_UNIT:
			{
				SelectorUnit* unit = static_cast<SelectorUnit*>(rootControl);
				for (int i = 0; i < unit->fInputPins.Count(); i++)
					_HarvestRecordFeatureUnits(Find(unit->fInputPins[i]), Map);
				Map.Put(rootControl->ID(), rootControl);
			}
			break;

		case USB_AUDIO_AC_FEATURE_UNIT:
			Map.Put(rootControl->ID(), rootControl);
			break;
	}
}


void
AudioControlInterface::_HarvestOutputFeatureUnits(_AudioControl* rootControl,
		AudioControlsMap& Map)
{
	if (rootControl == 0) {
		TRACE(ERR, "Not processing due NULL root control.\n");
		return;
	}

	switch(rootControl->SubType()) {
		case USB_AUDIO_AC_MIXER_UNIT:
			{
				MixerUnit* unit = static_cast<MixerUnit*>(rootControl);
				for (int i = 0; i < unit->fInputPins.Count(); i++)
					_HarvestOutputFeatureUnits(Find(unit->fInputPins[i]), Map);
				Map.Put(rootControl->ID(), rootControl);
			}
			break;

		case USB_AUDIO_AC_FEATURE_UNIT:
			Map.Put(rootControl->ID(), rootControl);
			break;
	}
}


bool
AudioControlInterface::_InitGainLimits(multi_mix_control& Control)
{
	bool canControl = false;
	float current = 0.;

	if (SpecReleaseNumber() >= 0x200) {
		// R2: read the current value with CUR, then the min/max/res limits with
		// a single RANGE request that returns wNumSubRanges followed by
		// {wMIN, wMAX, wRES} 2-byte triples (Audio20 5.2.3.2). Volume uses the
		// same signed 1/256 dB scale as R1.
		Control.gain.min_gain = 0.;
		Control.gain.max_gain = 100.;
		Control.gain.granularity = 1.;

		int16 cur = 0;
		size_t length = 0;
		status_t status = gUSBModule->send_request(fDevice->USBDevice(),
			USB_REQTYPE_INTERFACE_IN | USB_REQTYPE_CLASS, USB_AUDIO_R2_CUR,
			REQ_VALUE(Control.id), REQ_INDEX(Control.id), sizeof(cur),
			&cur, &length);
		if (status != B_OK || length != sizeof(cur)) {
			TRACE(ERR, "R2 CUR (%04x:%04x) fail:%#08x; received %d of %d\n",
				REQ_VALUE(Control.id), REQ_INDEX(Control.id), status,
				length, sizeof(cur));
			return false;
		}
		canControl = true;
		current = static_cast<float>(cur) / 256.;

		usb_audio_range2 range;
		status = gUSBModule->send_request(fDevice->USBDevice(),
			USB_REQTYPE_INTERFACE_IN | USB_REQTYPE_CLASS, USB_AUDIO_R2_RANGE,
			REQ_VALUE(Control.id), REQ_INDEX(Control.id), sizeof(range),
			&range, &length);
		if (status == B_OK && length >= sizeof(range)
				&& range.num_sub_ranges >= 1) {
			Control.gain.min_gain
				= static_cast<float>(range.subranges[0].min) / 256.;
			Control.gain.max_gain
				= static_cast<float>(range.subranges[0].max) / 256.;
			float res = static_cast<float>(range.subranges[0].res) / 256.;
			Control.gain.granularity = res > 0. ? res : 1.;
		} else {
			TRACE(ERR, "R2 RANGE (%04x:%04x) fail:%#08x; %d bytes\n",
				REQ_VALUE(Control.id), REQ_INDEX(Control.id), status, length);
		}

		TRACE(ERR, "Control %s: %f dB, from %f to %f dB, step %f dB.\n",
			Control.name, current, Control.gain.min_gain,
			Control.gain.max_gain, Control.gain.granularity);
		return canControl;
	}

	struct _GainInfo {
		uint8	request;
		int16	data;
		float&	value;
	} gainInfos[] = {
		{ USB_AUDIO_GET_CUR, 0, current },
		{ USB_AUDIO_GET_MIN, 0, Control.gain.min_gain },
		{ USB_AUDIO_GET_MAX, 0, Control.gain.max_gain },
		{ USB_AUDIO_GET_RES, 0, Control.gain.granularity }
	};

	Control.gain.min_gain = 0.;
	Control.gain.max_gain = 100.;
	Control.gain.granularity = 1.;

	size_t actualLength = 0;
	for (size_t i = 0; i < B_COUNT_OF(gainInfos); i++) {
		status_t status = gUSBModule->send_request(fDevice->USBDevice(),
			USB_REQTYPE_INTERFACE_IN | USB_REQTYPE_CLASS,
			gainInfos[i].request, REQ_VALUE(Control.id),
			REQ_INDEX(Control.id), sizeof(gainInfos[i].data),
			&gainInfos[i].data, &actualLength);

		if (status != B_OK || actualLength != sizeof(gainInfos[i].data)) {
			TRACE(ERR, "Request %d (%04x:%04x) fail:%#08x; received %d of %d\n",
				i, REQ_VALUE(Control.id), REQ_INDEX(Control.id), status,
				actualLength, sizeof(gainInfos[i].data));
			continue;
		}

		if (i == 0)
			canControl = true;

		gainInfos[i].value = static_cast<float>(gainInfos[i].data) / 256.;
	}

	// Not every device answers these sensibly. The Denon DN-HC4500 reports a
	// GET_RES of 0x8000, which lands here as a -128 dB step, and hands back
	// MIN and MAX the wrong way round on some enumerations - both leave the
	// mixer with an unusable control. Normalise rather than trust the device.
	if (Control.gain.min_gain > Control.gain.max_gain) {
		float swap = Control.gain.min_gain;
		Control.gain.min_gain = Control.gain.max_gain;
		Control.gain.max_gain = swap;
	}

	// The R2 path above already guards this; the spec has no use for a
	// non-positive resolution, so fall back to 1 dB.
	if (Control.gain.granularity <= 0.)
		Control.gain.granularity = 1.;

	TRACE(ERR, "Control %s: %f dB, from %f to %f dB, step %f dB.\n",
		Control.name, current, Control.gain.min_gain, Control.gain.max_gain,
		Control.gain.granularity);
	return canControl;
}


uint32
AudioControlInterface::_ListFeatureUnitOption(uint32 controlType,
		int32& index, int32 parentIndex, multi_mix_control_info* Info,
		FeatureUnit* unit, uint32 channel, uint32 channels)
{
	int32 startIndex = index;
	uint32 id = 0;
	uint32 flags = 0;
	strind_id string = S_null;
	const char* name = NULL;
	bool initGainLimits = false;

	switch(controlType) {
		case BMA_CTL_MUTE:
			id = USB_AUDIO_MUTE_CONTROL;
			flags = B_MULTI_MIX_ENABLE;
			string = S_MUTE;
			break;
		case BMA_CTL_VOLUME:
			id = USB_AUDIO_VOLUME_CONTROL;
			flags = B_MULTI_MIX_GAIN;
			string = S_GAIN;
			initGainLimits = true;
			break;
		case BMA_CTL_AUTOGAIN:
			id = USB_AUDIO_AUTOMATIC_GAIN_CONTROL;
			flags = B_MULTI_MIX_ENABLE;
			name = "Auto Gain";
			break;
		default:
			TRACE(ERR, "Unsupported type %#08x ignored.\n", controlType);
			return 0;
	}

	multi_mix_control* Controls = Info->controls;

	if (unit->HasControl(channel, controlType)) {
		uint32 masterIndex = CTL_ID(id, channel, unit->ID(), fInterface);
		Controls[index].id  = masterIndex;
		Controls[index].flags = flags;
		Controls[index].parent = parentIndex;
		Controls[index].string = string;
		if (name != NULL)
			strlcpy(Controls[index].name, name, sizeof(Controls[index].name));
		if (initGainLimits)
			_InitGainLimits(Controls[index]);

		index++;

		if (channels == 2) {
			Controls[index].id = CTL_ID(id, channel + 1, unit->ID(), fInterface);
			Controls[index].flags = flags;
			Controls[index].parent = parentIndex;
			Controls[index].master = masterIndex;
			Controls[index].string = string;
			if (name != NULL)
				strlcpy(Controls[index].name, name, sizeof(Controls[index].name));
			if (initGainLimits)
				_InitGainLimits(Controls[index]);
			index++;
		}
	}

	return index - startIndex;
}


int32
AudioControlInterface::_ListFeatureUnitControl(int32& index, int32 parentIndex,
		multi_mix_control_info* Info, _AudioControl* control)
{
	FeatureUnit* unit = static_cast<FeatureUnit*>(control);
	if (unit == 0) {
		TRACE(ERR, "Feature Unit for null control ignored.\n");
		return 0;
	}

	if (index + 4 > Info->control_count) {
		TRACE(ERR, "Could not list feature control group."
			" Limit %d of %d has been reached.\n",
			index, Info->control_count);
		return 0;
	}

	AudioChannelCluster* cluster = unit->OutCluster();
	if (cluster == 0) {
		TRACE(ERR, "Control %s with null cluster ignored.\n", unit->Name());
		return 0;
	}

	struct _ChannelInfo {
		const char*	Name;
		uint8		channels;
		uint32		Mask;
	} channelInfos[] = {
		{ "",			1, 0 },	// Master channel entry - no bitmask
		{ "",			2, B_CHANNEL_LEFT | B_CHANNEL_RIGHT },
		{ "Left",		1, B_CHANNEL_LEFT	},
		{ "Right",		1, B_CHANNEL_RIGHT	},
		{ "Center",		1, B_CHANNEL_CENTER },
		{ "L.F.E.",		1, B_CHANNEL_SUB	},
		{ "Back",		2, B_CHANNEL_REARLEFT | B_CHANNEL_REARRIGHT },
		{ "Back Left",	1, B_CHANNEL_REARLEFT	},
		{ "Back Right",	1, B_CHANNEL_REARRIGHT	},
		{ "Front of Center",		2, B_CHANNEL_FRONT_LEFT_CENTER
										| B_CHANNEL_FRONT_RIGHT_CENTER },
		{ "Front Left of Center",	1, B_CHANNEL_FRONT_LEFT_CENTER	},
		{ "Front Right of Center",	1, B_CHANNEL_FRONT_RIGHT_CENTER	},
		{ "Back Center",		1, B_CHANNEL_BACK_CENTER },
		{ "Side",				2, B_CHANNEL_SIDE_LEFT | B_CHANNEL_SIDE_RIGHT },
		{ "Side Left",			1, B_CHANNEL_SIDE_LEFT },
		{ "Side Right",			1, B_CHANNEL_SIDE_RIGHT },
		{ "Top Center",			1, B_CHANNEL_TOP_CENTER },
		{ "Top Front Left",		1, B_CHANNEL_TOP_FRONT_LEFT },
		{ "Top Front Center",	1, B_CHANNEL_TOP_FRONT_CENTER },
		{ "Top Front Right",	1, B_CHANNEL_TOP_FRONT_RIGHT },
		{ "Top Back Left",		1, B_CHANNEL_TOP_BACK_LEFT },
		{ "Top Back Center",	1, B_CHANNEL_TOP_BACK_CENTER },
		{ "Top Back Right",		1, B_CHANNEL_TOP_BACK_RIGHT }
	};

	multi_mix_control* Controls = Info->controls;

	uint32 channelsConfig = cluster->ChannelsConfig();
	int32 groupIndex = 0;
	int32 channel = 0;
	int32 masterIndex = 0;	// in case master channel has no volume
							// control - add following "L+R" channels into it

	for (size_t i = 0; i < B_COUNT_OF(channelInfos); i++) {
		if ((channelsConfig & channelInfos[i].Mask) != channelInfos[i].Mask) {
			// ignore non-listed and possibly non-paired stereo channels.
			//	note that master channel with zero mask pass this check! ;-)
			continue;
		}

		if (masterIndex == 0) {
			groupIndex = index;
			Controls[index].id = groupIndex;
			Controls[index].flags = B_MULTI_MIX_GROUP;
			Controls[index].parent = parentIndex;
			snprintf(Controls[index].name, sizeof(Controls[index].name),
				"%s %s", unit->Name(), channelInfos[i].Name);
			index++;
		} else {
			groupIndex = masterIndex;
			masterIndex = 0;
		}

		// First list possible Mute controls
		_ListFeatureUnitOption(BMA_CTL_MUTE, index, groupIndex, Info,
				unit, channel, channelInfos[i].channels);

		// Gain controls may be usefull too
		if (_ListFeatureUnitOption(BMA_CTL_VOLUME, index, groupIndex, Info,
				unit, channel, channelInfos[i].channels) == 0) {
			masterIndex = (i == 0) ? groupIndex : 0 ;
			TRACE(UAC, "channel:%d set master index to %d\n",
				channel, masterIndex);
		}

		// Auto Gain checkbox will be listed too
		_ListFeatureUnitOption(BMA_CTL_AUTOGAIN, index, groupIndex, Info,
			unit, channel, channelInfos[i].channels);

		// Now check if the group filled with something usefull.
		// In case no controls were added into it - "remove" it
		if (Controls[index - 1].flags == B_MULTI_MIX_GROUP) {
			Controls[index - 1].id = 0;
			index--;

			masterIndex = 0;
		}

		channel += channelInfos[i].channels;

		// remove bits for already processed channels - this prevent from
		// duplication of the stereo channels as "orphaned" ones and optimize
		// exit from this iterations after all channels are processed.
		channelsConfig &= ~channelInfos[i].Mask;

		if (0 == channelsConfig)
			break;
	}

	if (channelsConfig > 0)
		TRACE(ERR, "Following channels were not processed: %#08x.\n",
			channelsConfig);

	// return last group index to stick possible selector unit to it. ;-)
	return groupIndex;
}


void
AudioControlInterface::_ListSelectorUnitControl(int32& index, int32 parentGroup,
		multi_mix_control_info* Info, _AudioControl* control)
{
	SelectorUnit* selector = static_cast<SelectorUnit*>(control);
	if (selector == 0 || selector->SubType() != USB_AUDIO_AC_SELECTOR_UNIT)
		return;

	if ((index + 1 + selector->fInputPins.Count()) > Info->control_count) {
		TRACE(ERR, "Could not list selector control."
			" Limit %d of %d has been reached.\n",
			index, Info->control_count);
		return;
	}

	multi_mix_control* Controls = Info->controls;

	int32 recordMUX = CTL_ID(0, 0, selector->ID(), fInterface);
	Controls[index].id	= recordMUX;
	Controls[index].flags = B_MULTI_MIX_MUX;
	Controls[index].parent = parentGroup;
	Controls[index].string = S_null;
	strlcpy(Controls[index].name, "Source", sizeof(Controls[index].name));
	index++;

	for (int i = 0; i < selector->fInputPins.Count(); i++) {
		Controls[index].id = CTL_ID(0, 1, selector->ID(), fInterface);
		Controls[index].flags = B_MULTI_MIX_MUX_VALUE;
		Controls[index].master = 0;
		Controls[index].string = S_null;
		Controls[index].parent = recordMUX;
		_AudioControl* control = Find(selector->fInputPins[i]);
		if (control != NULL)
			strlcpy(Controls[index].name,
				control->Name(), sizeof(Controls[index].name));
		else
			snprintf(Controls[index].name,
				sizeof(Controls[index].name), "Input #%d", i + 1);
		index++;
	}
}


size_t
AudioControlInterface::_CollectMixerUnitControls(
		const uint32 controlIds[kChannels][kChannels],
		size_t inLeft, size_t outLeft, size_t inRight, size_t outRight,
		const char* inputName, const char* name,
		Vector<multi_mix_control>& Controls)
{
	size_t count = 0;
	uint32 leftId = controlIds[inLeft][outLeft];
	uint32 rightId = controlIds[inRight][outRight];

//	TRACE(UAC, "left:%d %d: %08x; right:%d %d: %08x\n",
//			inLeft, outLeft, leftId, inRight, outRight, rightId);

	multi_mix_control control;
	memset(&control, 0, sizeof(multi_mix_control));
	snprintf(control.name, sizeof(control.name), "%s %s", inputName, name);

	for (size_t i = 0; i < 2; i++) {
		if (leftId != 0 || rightId != 0) {
			control.flags = B_MULTI_MIX_GROUP;
			control.string = S_null;
			Controls.PushBack(control);

			int gainControls = 0;
			if (leftId != 0) {
				control.id = leftId;
				control.flags = B_MULTI_MIX_GAIN;
				control.string = S_GAIN;
				if (_InitGainLimits(control)) {
					gainControls++;
					Controls.PushBack(control);
				}
			}

			if (rightId != 0) {
				control.id = rightId;
				control.flags = B_MULTI_MIX_GAIN;
				control.string = S_GAIN;
				control.master = leftId;
				if (_InitGainLimits(control)) {
					gainControls++;
					Controls.PushBack(control);
				}
			}

			// remove empty mix group
			if (gainControls == 0)
				Controls.PopBack();
			else
				count++;
		}

		// take care about surround bus
		if (inLeft == inRight)
			break;
		// handle possible reverse controls
		leftId = controlIds[inLeft][outRight];
		rightId = controlIds[inRight][outLeft];
		snprintf(control.name, sizeof(control.name),
			"%s %s (Reverse)", inputName, name);
	}

	return count;
}


void
AudioControlInterface::_ListMixerUnitControls(int32& index,
		multi_mix_control_info* Info, Vector<multi_mix_control>& controls)
{
	multi_mix_control* Controls = Info->controls;
	uint32 groupParent = 0;
	uint32 gainParent = 0;
	for (Vector<multi_mix_control>::Iterator I = controls.Begin();
			I != controls.End() && index < Info->control_count; I++) {
		memcpy(Controls + index, &*I, sizeof(multi_mix_control));
		switch (I->flags) {
			case B_MULTI_MIX_GROUP:
				Controls[index].id = index;
				Controls[index].parent = groupParent;
				if (groupParent == 0) {
					Controls[index].id |= 0x10000;
					groupParent = Controls[index].id;
				}
				gainParent = Controls[index].id;
				break;
			case B_MULTI_MIX_GAIN:
				Controls[index].parent = gainParent;
				break;
			default:
				TRACE(ERR, "Control type %d ignored\n", I->flags);
				continue;
		}

		index++;
	}

	if (index == Info->control_count)
		TRACE(ERR, "Control count limit %d has been reached.\n", index);
}


void
AudioControlInterface::_ListMixControlsForMixerUnit(int32& index,
		multi_mix_control_info* Info, _AudioControl* control)
{
	MixerUnit* mixer = static_cast<MixerUnit*>(control);
	if (mixer == 0 || mixer->SubType() != USB_AUDIO_AC_MIXER_UNIT)
		return;

	struct _ChannelPair {
		size_t inLeft;
		size_t inRight;
		const char* name;
	} channelPairs[] = {
		{ 0, 1, "" },
		{ 2, 2, "Center" },
		{ 3, 3, "L.F.E" },
		{ 4, 5, "Back" },
		{ 6, 7, "Front of Center" },
		{ 8, 8, "Back Center" },
		{ 9, 10, "Side" },
		{ 11, 11, "Top Center" },
		{ 12, 14, "Top Front" },
		{ 13, 13, "Top Front Center" },
		{ 15, 17, "Top Back" },
		{ 16, 16, "Top Back Center" }
	};

	Vector<_MixPageCollector*> mixControls;

	_MixPageCollector* genericPage = new(std::nothrow) _MixPageCollector("Mixer");
	mixControls.PushBack(genericPage);

	// page for extended in (>2) and out (>2) mixer controls
	size_t controlsOnExMixerPage = 0;
	_MixPageCollector* exMixerPage = new(std::nothrow) _MixPageCollector("Mixer");

	AudioChannelCluster* outCluster = mixer->OutCluster();

	int inOffset = 0;
	for (int iPin = 0; iPin < mixer->fInputPins.Count(); iPin++) {
		_AudioControl* control = Find(mixer->fInputPins[iPin]);
		AudioChannelCluster* inCluster = NULL;
		if (control != NULL)
			inCluster = control->OutCluster();
		if (inCluster == NULL) {
			TRACE(ERR, "control %p cluster %p failed!\n", control, inCluster);
			break;
		}

		// at first - collect programmable control ids
		uint32 controlIds[kChannels][kChannels] = { { 0 } };

		int inChannel = 0;
		for (size_t in = 0; in < kChannels
				&& inChannel < inCluster->ChannelsCount(); in++) {
			if ((inCluster->ChannelsConfig() & (1 << in)) == 0)
				continue;

			for (size_t out = 0, outChannel = 0; out < kChannels
					&& outChannel < outCluster->ChannelsCount(); out++) {
				if ((outCluster->ChannelsConfig() & (1 << out)) == 0)
					continue;

				if (mixer->IsControlProgrammable(
						inOffset + inChannel, outChannel)) {
					if (SpecReleaseNumber() < 0x200)
						// USB Audio 1.0 uses ICN/OCN for request
						controlIds[in][out] = CTL_ID(inOffset + inChannel + 1,
							outChannel + 1, mixer->ID(), fInterface);
					else
						// USB Audio 2.0 uses CS/MCN for request
						controlIds[in][out] = CTL_ID(USB_AUDIO_MIXER_CONTROL,
							(inOffset + inChannel) * outCluster->ChannelsCount()
							+ outChannel, mixer->ID(), fInterface);
				}

				outChannel++;
			}

			inChannel++;
		}

		inOffset += inChannel;

		for (size_t in = 0; in < kChannels; in++)
			for (size_t out = 0; out < kChannels; out++)
				if (controlIds[in][out] != 0)
					TRACE(UAC, "ctrl:%08x for in %d; out %d;\n",
						controlIds[in][out], in, out);

		// second step - distribute controls on
		// mixer pages in logical groups
		uint32 exChannelsMask = ~(B_CHANNEL_LEFT | B_CHANNEL_RIGHT);
		bool inIsEx = (inCluster->ChannelsConfig() & exChannelsMask) != 0;
		bool outIsEx = (outCluster->ChannelsConfig() & exChannelsMask) != 0;

		if (!inIsEx && !outIsEx) {
			// heap up all mono and stereo controls into single "Mixer" page
			for (size_t i = 0; i < 2; i++)
				_CollectMixerUnitControls(controlIds,
					kLeftChannel, channelPairs[i].inLeft,
					kRightChannel, channelPairs[i].inRight,
					control->Name(), channelPairs[i].name,
					*mixControls[0]);
			continue; // go next input cluster
		}

		if (!outIsEx) {
			// special case - extended (>2 channels) input cluster
			// connected to 2-channels output - add into generic "Mixer" page
			for (size_t i = 0; i < B_COUNT_OF(channelPairs); i++)
				_CollectMixerUnitControls(controlIds,
					channelPairs[i].inLeft, kLeftChannel,
					channelPairs[i].inRight, kRightChannel,
					control->Name(), channelPairs[i].name,
					*mixControls[0]);
			continue; // go next input cluster
		}

		// make separate mixer pages for set of extended (>2) input
		// channels connected to extended (>2 channels) output
		for (size_t in = 0; in < B_COUNT_OF(channelPairs); in++) {
			for (size_t out = 0; out < B_COUNT_OF(channelPairs); out++) {
				char outName[sizeof(Info->controls->name)] = { 0 };
				if (in == out)
					strlcpy(outName, channelPairs[out].name, sizeof(outName));
				else
					snprintf(outName, sizeof(outName), "%s to %s",
						channelPairs[in].name, channelPairs[out].name);

				controlsOnExMixerPage += _CollectMixerUnitControls(controlIds,
					channelPairs[in].inLeft, channelPairs[out].inLeft,
					channelPairs[in].inRight, channelPairs[out].inRight,
					control->Name(), outName, *exMixerPage);
			}

			if (controlsOnExMixerPage >= 6) {
				mixControls.PushBack(exMixerPage);
				exMixerPage = new(std::nothrow) _MixPageCollector("Mixer");
				controlsOnExMixerPage = 0;
			}
		}
	}

	if (exMixerPage->Count() > 1)
		mixControls.PushBack(exMixerPage);
	else
		delete exMixerPage;

	// final step - fill multiaudio controls info with
	// already structured pages/controls info arrays
	for (Vector<_MixPageCollector*>::Iterator I = mixControls.Begin();
			I != mixControls.End(); I++) {
		Vector<multi_mix_control>* controls = *I;
			TRACE(UAC, "controls count: %d\n", controls->Count());
		if (controls->Count() > 1)
			_ListMixerUnitControls(index, Info, *controls);
		delete controls;
	}
}


void
AudioControlInterface::_ListMixControlsPage(int32& index,
		multi_mix_control_info* Info, AudioControlsMap& Map, const char* Name)
{
	multi_mix_control* Controls = Info->controls;
	int32 groupIndex = index | 0x10000;
	Controls[index].id	= groupIndex;
	Controls[index].flags = B_MULTI_MIX_GROUP;
	Controls[index].parent = 0;
	strlcpy(Controls[index].name, Name, sizeof(Controls[index].name));
	index++;

	int32 group = groupIndex;
	for (AudioControlsIterator I = Map.Begin(); I != Map.End(); I++) {
		TRACE(UAC, "%s control %d listed.\n", Name, I->Value()->ID());
		switch(I->Value()->SubType()) {
			case USB_AUDIO_AC_FEATURE_UNIT:
				group = _ListFeatureUnitControl(index, groupIndex,
					Info, I->Value());
				break;
			case USB_AUDIO_AC_SELECTOR_UNIT:
				_ListSelectorUnitControl(index, group, Info, I->Value());
				break;
			default:
				break;
		}
	}
}


status_t
AudioControlInterface::ListMixControls(multi_mix_control_info* Info)
{
	// first harvest feature units that assigned to output terminal(s)
	AudioControlsMap RecordControlsMap;
	AudioControlsMap OutputControlsMap;

	for (AudioControlsIterator I = fOutputTerminals.Begin();
			I != fOutputTerminals.End(); I++) {
		_Terminal* terminal = static_cast<_Terminal*>(I->Value());
		if (terminal->IsUSBIO())
			_HarvestRecordFeatureUnits(terminal, RecordControlsMap);
		else
			_HarvestOutputFeatureUnits(terminal, OutputControlsMap);
	}

	// separate input and output Feature units
	// and collect mixer units that can be controlled
	AudioControlsMap InputControlsMap;
	AudioControlsMap MixerControlsMap;

	for (AudioControlsIterator I = fAudioControls.Begin();
			I != fAudioControls.End(); I++) {
		_AudioControl* control = I->Value();

		if (control->SubType() == USB_AUDIO_AC_MIXER_UNIT) {
			MixerUnit* mixerControl = static_cast<MixerUnit*>(control);
			if (mixerControl->HasProgrammableControls())
				MixerControlsMap.Put(control->ID(), control);
			continue;
		}

		// filter out feature units
		if (control->SubType() != USB_AUDIO_AC_FEATURE_UNIT)
			continue;

		// ignore controls that are already in the output controls maps
		if (RecordControlsMap.Find(control->ID()) != RecordControlsMap.End()
			|| OutputControlsMap.Find(control->ID()) != OutputControlsMap.End())
			continue;

		_AudioControl* sourceControl = Find(control->SourceID());
		if (sourceControl != 0
				&& sourceControl->SubType() == USB_AUDIO_AC_INPUT_TERMINAL)
			InputControlsMap.Put(control->ID(), control);
		else
			OutputControlsMap.Put(control->ID(), control);
	}

	int32 index = 0;
	if (InputControlsMap.Count() > 0)
		_ListMixControlsPage(index, Info, InputControlsMap, "Input");

	if (OutputControlsMap.Count() > 0)
		_ListMixControlsPage(index, Info, OutputControlsMap, "Output");

	if (RecordControlsMap.Count() > 0)
		_ListMixControlsPage(index, Info, RecordControlsMap, "Record");


	for (AudioControlsIterator I = MixerControlsMap.Begin();
			I != MixerControlsMap.End(); I++)
		_ListMixControlsForMixerUnit(index, Info, I->Value());

	Info->control_count = index;

	return B_OK;
}


status_t
AudioControlInterface::GetMix(multi_mix_value_info* Info)
{
	for (int32 i = 0; i < Info->item_count; i++) {
		uint16 length = 0;
		int16 data = 0;

		_AudioControl* control = Find(ID_FROM_CTLID(Info->values[i].id));
		if (control == NULL) {
			TRACE(ERR, "No control found for unit id %#02x. Ignore it.\n",
				ID_FROM_CTLID(Info->values[i].id));
			continue;
		}

		switch (control->SubType()) {
			case USB_AUDIO_AC_FEATURE_UNIT:
				switch(CS_FROM_CTLID(Info->values[i].id)) {
					case USB_AUDIO_VOLUME_CONTROL:
						length = 2;
						break;
					case USB_AUDIO_MUTE_CONTROL:
					case USB_AUDIO_AUTOMATIC_GAIN_CONTROL:
						length = 1;
						break;
					default:
						TRACE(ERR, "Unsupported control id:%08x of type %#02x "
							"ignored.\n", ID_FROM_CTLID(Info->values[i].id),
							CS_FROM_CTLID(Info->values[i].id));
						continue;
				}
				break;
			case USB_AUDIO_AC_SELECTOR_UNIT:
				length = 1;
				break;
			case USB_AUDIO_AC_MIXER_UNIT:
				length = 2;
				break;
			default:
				TRACE(ERR, "Control id:%08x of type %d is not supported\n",
					ID_FROM_CTLID(Info->values[i].id), control->SubType());
				continue;
		}

		// R1 reads the current value with GET_CUR (0x81); R2 uses CUR (0x01)
		// with the direction taken from bmRequestType.
		uint8 request = SpecReleaseNumber() >= 0x200
			? (uint8)USB_AUDIO_R2_CUR : (uint8)USB_AUDIO_GET_CUR;

		size_t actualLength = 0;
		status_t status = gUSBModule->send_request(fDevice->USBDevice(),
			USB_REQTYPE_INTERFACE_IN | USB_REQTYPE_CLASS, request,
			REQ_VALUE(Info->values[i].id), REQ_INDEX(Info->values[i].id),
			length, &data, &actualLength);

		if (status != B_OK || actualLength != length) {
			TRACE(ERR, "Request (%04x:%04x) failed:%#08x; received %d of %d\n",
				REQ_VALUE(Info->values[i].id), REQ_INDEX(Info->values[i].id),
				status, actualLength, length);
			continue;
		}

		switch (control->SubType()) {
			case USB_AUDIO_AC_FEATURE_UNIT:
				switch(CS_FROM_CTLID(Info->values[i].id)) {
					case USB_AUDIO_VOLUME_CONTROL:
						Info->values[i].gain = static_cast<float>(data) / 256.;
						TRACE(MIX, "Gain control %d; channel: %d; is %f dB.\n",
							ID_FROM_CTLID(Info->values[i].id),
							CN_FROM_CTLID(Info->values[i].id),
							Info->values[i].gain);
						break;
					case USB_AUDIO_MUTE_CONTROL:
						Info->values[i].enable = data > 0;
						TRACE(MIX, "Mute control %d; channel: %d; is %d.\n",
							ID_FROM_CTLID(Info->values[i].id),
							CN_FROM_CTLID(Info->values[i].id),
							Info->values[i].enable);
						break;
					case USB_AUDIO_AUTOMATIC_GAIN_CONTROL:
						Info->values[i].enable = data > 0;
						TRACE(MIX, "AGain control %d; channel: %d; is %d.\n",
							ID_FROM_CTLID(Info->values[i].id),
							CN_FROM_CTLID(Info->values[i].id),
							Info->values[i].enable);
						break;
					default:
						break;
				}
				break;
			case USB_AUDIO_AC_SELECTOR_UNIT:
				Info->values[i].mux = data - 1;
				TRACE(MIX, "Selector control %d; is %d.\n",
					ID_FROM_CTLID(Info->values[i].id),
					Info->values[i].mux);
				break;
			case USB_AUDIO_AC_MIXER_UNIT:
				Info->values[i].gain = static_cast<float>(data) / 256.;
				TRACE(MIX, "Mixer #%d channels in: %d; out: %d; is %f dB.\n",
					ID_FROM_CTLID(Info->values[i].id),
					CS_FROM_CTLID(Info->values[i].id),
					CN_FROM_CTLID(Info->values[i].id),
					Info->values[i].gain);
				break;
		}
	}

	return B_OK;
}


status_t
AudioControlInterface::SetMix(multi_mix_value_info* Info)
{
	for (int32 i = 0; i < Info->item_count; i++) {
		uint16 length = 0;
		int16 data = 0;

		_AudioControl* control = Find(ID_FROM_CTLID(Info->values[i].id));
		if (control == NULL) {
			TRACE(ERR, "No control found for unit id %#02x. Ignore it.\n",
				ID_FROM_CTLID(Info->values[i].id));
			continue;
		}

		switch (control->SubType()) {
			case USB_AUDIO_AC_FEATURE_UNIT:
				switch(CS_FROM_CTLID(Info->values[i].id)) {
					case USB_AUDIO_VOLUME_CONTROL:
						data = static_cast<int16>(Info->values[i].gain * 256.);
						length = 2;
						TRACE(MIX, "Gain control %d; channel: %d; "
							"about to set to %f dB.\n",
							ID_FROM_CTLID(Info->values[i].id),
							CN_FROM_CTLID(Info->values[i].id),
							Info->values[i].gain);
						break;
					case USB_AUDIO_MUTE_CONTROL:
						data = (Info->values[i].enable ? 1 : 0);
						length = 1;
						TRACE(MIX, "Mute control %d; channel: %d; "
							"about to set to %d.\n",
							ID_FROM_CTLID(Info->values[i].id),
							CN_FROM_CTLID(Info->values[i].id),
							Info->values[i].enable);
						break;
					case USB_AUDIO_AUTOMATIC_GAIN_CONTROL:
						data = (Info->values[i].enable ? 1 : 0);
						length = 1;
						TRACE(MIX, "AGain control %d; channel: %d; "
							"about to set to %d.\n",
							ID_FROM_CTLID(Info->values[i].id),
							CN_FROM_CTLID(Info->values[i].id),
							Info->values[i].enable);
						break;
					default:
						TRACE(ERR, "Unsupported control id:%08x of type %#02x "
							"ignored.\n", ID_FROM_CTLID(Info->values[i].id),
							CS_FROM_CTLID(Info->values[i].id));
						continue;
				}
				break;
			case USB_AUDIO_AC_SELECTOR_UNIT:
				data = Info->values[i].mux + 1;
				length = 1;
				TRACE(MIX, "Selector Control %d about to set to %d.\n",
					ID_FROM_CTLID(Info->values[i].id),
					Info->values[i].mux);
				break;
			case USB_AUDIO_AC_MIXER_UNIT:
				data = static_cast<int16>(Info->values[i].gain * 256.);
				length = 2;
				TRACE(MIX, "Mixer %d channels in: %d; out: %d; "
					"about to set to %f dB.\n",
					ID_FROM_CTLID(Info->values[i].id),
					CS_FROM_CTLID(Info->values[i].id),
					CN_FROM_CTLID(Info->values[i].id),
					Info->values[i].gain);
				break;
			default:
				TRACE(ERR, "Control id:%08x of type %d is not supported\n",
					Info->values[i].id, control->SubType());
				continue;
		}

		size_t actualLength = 0;
		status_t status = gUSBModule->send_request(fDevice->USBDevice(),
			USB_REQTYPE_INTERFACE_OUT | USB_REQTYPE_CLASS, USB_AUDIO_SET_CUR,
			REQ_VALUE(Info->values[i].id), REQ_INDEX(Info->values[i].id),
			length, &data, &actualLength);

		if (status != B_OK || actualLength != length) {
			TRACE(ERR, "Request (%04x:%04x) failed:%#08x; send %d of %d\n",
				REQ_VALUE(Info->values[i].id), REQ_INDEX(Info->values[i].id),
				status, actualLength, length);
			continue;
		}

		TRACE(MIX, "Value set OK\n");
	}

	return B_OK;
}

