/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Distributed under the terms of the MIT license.
 *
 */


#include "Device.h"

#include <kernel.h>
#include <usb/USB_audio.h>

#include "Driver.h"
#include "Settings.h"




Device::Device(usb_device device)
	:
	fStatus(B_ERROR),
	fOpen(false),
	fRemoved(false),
	fDevice(device),
	fNonBlocking(false),
	fAudioControl(this),
	fFeedbackFrames(0),
	fImplicitFeedbackSource(false),
	fImplicitSourceInterval(0),
	fVariableIsoOutSupport(-1),
	fFeedbackRingHead(0),
	fFeedbackRingTail(0),
	fBuffersReadySem(-1)
{
	fProductName[0] = '\0';
	fInstanceIndex = 0;

	const usb_device_descriptor* deviceDescriptor
		= gUSBModule->get_device_descriptor(device);

	if (deviceDescriptor == NULL) {
		TRACE(ERR, "Error of getting USB device descriptor.\n");
		return;
	}

	fVendorID = deviceDescriptor->vendor_id;
	fProductID = deviceDescriptor->product_id;
	fUSBVersion = deviceDescriptor->usb_version;

	_ReadProductName();

	// UAC1 (full-speed) and UAC2 (high-speed) are both handled: the class-
	// specific descriptor layout is selected per entity by the AudioControl
	// header's bcdADC, and a device whose descriptors do not parse into a
	// usable stream is rejected cleanly below via InitCheck (fail closed).

	fBuffersReadySem = create_sem(0, DRIVER_NAME "_buffers_ready");
	if (fBuffersReadySem < B_OK) {
		TRACE(ERR, "Error of creating ready "
			"buffers semaphore:%#010x\n", fBuffersReadySem);
		return;
	}

	if (_SetupEndpoints() != B_OK)
		return;

	// must be set in derived class constructor
	fStatus = B_OK;
}


Device::~Device()
{
	for (Vector<Stream*>::Iterator I = fStreams.Begin();
			I != fStreams.End(); I++)
		delete *I;

	fStreams.MakeEmpty();

	if (fBuffersReadySem > B_OK)
		delete_sem(fBuffersReadySem);
}


bool
Device::_FetchStringAscii(uint8 index, uint16 langId, char* out,
	size_t outSize)
{
	out[0] = '\0';
	if (index == 0 || outSize == 0)
		return false;

	uint8 raw[256];
	size_t actual = 0;
	status_t status = gUSBModule->get_descriptor(fDevice,
		USB_DESCRIPTOR_STRING, index, langId, raw, sizeof(raw), &actual);
	if (status != B_OK || actual < 2)
		return false;

	uint8 length = raw[0];
	if (length > actual)
		length = (uint8)actual;

	// A string descriptor is UTF-16LE after its 2-byte header. Our device
	// names are ASCII; keep the printable subset, and note whether we saw at
	// least one real (non-space) character so an all-placeholder decode of a
	// non-ASCII string is rejected rather than shown as "???".
	size_t out_i = 0;
	bool sawReal = false;
	for (size_t i = 2; i + 1 < length && out_i + 1 < outSize; i += 2) {
		uint16 c = raw[i] | ((uint16)raw[i + 1] << 8);
		if (c >= 0x20 && c < 0x7f) {
			out[out_i++] = (char)c;
			if (c != ' ')
				sawReal = true;
		} else
			out[out_i++] = '?';
	}
	out[out_i] = '\0';

	// Some devices pad the product string with trailing spaces.
	while (out_i > 0 && out[out_i - 1] == ' ')
		out[--out_i] = '\0';

	if (!sawReal)
		out[0] = '\0';

	return sawReal;
}


uint32
Device::_ReadLangIds(uint16* langs, uint32 maxCount)
{
	// String descriptor 0 is the supported-language table; it is requested
	// with a language id of 0.
	uint8 raw[256];
	size_t actual = 0;
	status_t status = gUSBModule->get_descriptor(fDevice,
		USB_DESCRIPTOR_STRING, 0, 0, raw, sizeof(raw), &actual);
	if (status != B_OK || actual < 2)
		return 0;

	uint8 length = raw[0];
	if (length > actual)
		length = (uint8)actual;

	uint32 count = 0;
	for (size_t i = 2; i + 1 < length && count < maxCount; i += 2)
		langs[count++] = raw[i] | ((uint16)raw[i + 1] << 8);

	return count;
}


void
Device::_ReadProductName()
{
	fProductName[0] = '\0';

	const usb_device_descriptor* deviceDescriptor
		= gUSBModule->get_device_descriptor(fDevice);
	if (deviceDescriptor == NULL || deviceDescriptor->product == 0)
		return;

	uint8 productIndex = deviceDescriptor->product;

	// Device names are ASCII English and 0x0409 (English US) is nearly
	// universal, so try it directly first -- the common case is one transfer.
	if (_FetchStringAscii(productIndex, 0x0409, fProductName,
			sizeof(fProductName))) {
		TRACE(INF, "Product name: \"%s\"\n", fProductName);
		return;
	}

	// English was not offered or produced nothing usable (e.g. the device
	// stalled the request for an unsupported language): fall back to asking
	// which languages it does support, and use the first that yields a name.
	uint16 langs[64];
	uint32 count = _ReadLangIds(langs, 64);
	for (uint32 i = 0; i < count; i++) {
		if (langs[i] == 0x0409)
			continue;
		if (_FetchStringAscii(productIndex, langs[i], fProductName,
				sizeof(fProductName))) {
			TRACE(INF, "Product name (lang %#06x): \"%s\"\n",
				(unsigned int)langs[i], fProductName);
			return;
		}
	}

	// Nothing usable -- leave empty; _MultiGetDescription falls back to
	// the generic "USB Audio" label.
	fProductName[0] = '\0';
}


void
Device::PublishFeedback(int32 framesPerPacket)
{
	atomic_set(&fFeedbackFrames, framesPerPacket);
}


int32
Device::Feedback()
{
	return atomic_get(&fFeedbackFrames);
}


bool
Device::PushFeedbackPacket(uint16 frames)
{
	int32 head = atomic_get(&fFeedbackRingHead);
	int32 tail = atomic_get(&fFeedbackRingTail);
	if ((uint32)(head - tail) >= kFeedbackRingSize)
		return false;

	fFeedbackRing[(uint32)head & (kFeedbackRingSize - 1)] = frames;
	atomic_set(&fFeedbackRingHead, head + 1);
	return true;
}


bool
Device::PeekFeedbackPacket(uint16& frames)
{
	int32 head = atomic_get(&fFeedbackRingHead);
	int32 tail = atomic_get(&fFeedbackRingTail);
	if (head - tail <= 0)
		return false;

	frames = fFeedbackRing[(uint32)tail & (kFeedbackRingSize - 1)];
	return true;
}


void
Device::PopFeedbackPacket()
{
	atomic_set(&fFeedbackRingTail, atomic_get(&fFeedbackRingTail) + 1);
}


status_t
Device::Open(uint32 flags)
{
	if (fOpen)
		return B_BUSY;
	if (fRemoved)
		return B_ERROR;

	status_t result = StartDevice();
	if (result != B_OK)
		return result;

	// TODO: are we need this???
	fNonBlocking = (flags & O_NONBLOCK) == O_NONBLOCK;
	fOpen = true;
	return result;
}


status_t
Device::Close()
{
	if (fRemoved) {
		fOpen = false;
		return B_OK;
	}

	for (int i = 0; i < fStreams.Count(); i++)
		fStreams[i]->Stop();

	fOpen = false;

	return StopDevice();
}


status_t
Device::Free()
{
	return B_OK;
}


status_t
Device::Read(uint8* buffer, size_t* numBytes)
{
	*numBytes = 0;
	return B_IO_ERROR;
}


status_t
Device::Write(const uint8* buffer, size_t* numBytes)
{
	*numBytes = 0;
	return B_IO_ERROR;
}


status_t
Device::Control(uint32 op, void* buffer, size_t length)
{
	// Nothing below is answerable once the device is gone: the handlers read
	// descriptors and audio controls that describe hardware which is no longer
	// there, and several issue USB requests to it. The media node keeps its fd
	// (and keeps calling) well past the removal - its own teardown does a
	// final B_MULTI_GET_MIX to save settings - so this has to be refused here
	// rather than assumed not to happen. B_MULTI_BUFFER_EXCHANGE has carried
	// its own gate for a while; this covers the rest.
	if (fRemoved)
		return B_DEV_NOT_READY;

	switch (op) {
		case B_MULTI_GET_DESCRIPTION:
		{
			multi_description description;
			multi_channel_info channels[16];
			multi_channel_info* originalChannels;

			if (user_memcpy(&description, buffer, sizeof(multi_description))
					!= B_OK)
				return B_BAD_ADDRESS;

			originalChannels = description.channels;
			description.channels = channels;
			if (description.request_channel_count > 16)
				description.request_channel_count = 16;

			status_t status = _MultiGetDescription(&description);
			if (status != B_OK)
				return status;

			description.channels = originalChannels;
			if (user_memcpy(buffer, &description, sizeof(multi_description))
					!= B_OK)
				return B_BAD_ADDRESS;
			return user_memcpy(originalChannels, channels,
				sizeof(multi_channel_info) * description.request_channel_count);
		}
		case B_MULTI_GET_EVENT_INFO:
			TRACE(ERR, "B_MULTI_GET_EVENT_INFO n/i\n");
			return B_ERROR;

		case B_MULTI_SET_EVENT_INFO:
			TRACE(ERR, "B_MULTI_SET_EVENT_INFO n/i\n");
			return B_ERROR;

		case B_MULTI_GET_EVENT:
			TRACE(ERR, "B_MULTI_GET_EVENT n/i\n");
			return B_ERROR;

		case B_MULTI_GET_ENABLED_CHANNELS:
		{
			multi_channel_enable enable;
			uint32 enable_bits;
			uchar* orig_enable_bits;

			if (user_memcpy(&enable, buffer, sizeof(enable)) != B_OK
					|| !IS_USER_ADDRESS(enable.enable_bits)) {
				return B_BAD_ADDRESS;
			}

			orig_enable_bits = enable.enable_bits;
			enable.enable_bits = (uchar*)&enable_bits;
			status_t status = _MultiGetEnabledChannels(&enable);
			if (status != B_OK)
				return status;

			enable.enable_bits = orig_enable_bits;
			if (user_memcpy(enable.enable_bits, &enable_bits,
					sizeof(enable_bits)) != B_OK
				|| user_memcpy(buffer, &enable,
					sizeof(multi_channel_enable)) != B_OK) {
				return B_BAD_ADDRESS;
			}

			return B_OK;
		}
		case B_MULTI_SET_ENABLED_CHANNELS:
		{
			multi_channel_enable enable;
			uint32 enable_bits;
			uchar* orig_enable_bits;

			if (user_memcpy(&enable, buffer, sizeof(enable)) != B_OK
				|| !IS_USER_ADDRESS(enable.enable_bits)) {
				return B_BAD_ADDRESS;
			}

			orig_enable_bits = enable.enable_bits;
			enable.enable_bits = (uchar*)&enable_bits;
			status_t status = _MultiSetEnabledChannels(&enable);
			if (status != B_OK)
				return status;

			enable.enable_bits = orig_enable_bits;
			if (user_memcpy(enable.enable_bits, &enable_bits,
					sizeof(enable_bits)) < B_OK
				|| user_memcpy(buffer, &enable, sizeof(multi_channel_enable))
					< B_OK) {
				return B_BAD_ADDRESS;
			}

			return B_OK;
		}
		case B_MULTI_GET_GLOBAL_FORMAT:
		{
			multi_format_info info;
			if (user_memcpy(&info, buffer, sizeof(multi_format_info)) != B_OK)
				return B_BAD_ADDRESS;

			status_t status = _MultiGetGlobalFormat(&info);
			if (status != B_OK)
				return status;
			if (user_memcpy(buffer, &info, sizeof(multi_format_info)) != B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}
		case B_MULTI_SET_GLOBAL_FORMAT:
		{
			multi_format_info info;
			if (user_memcpy(&info, buffer, sizeof(multi_format_info)) != B_OK)
				return B_BAD_ADDRESS;

			status_t status = _MultiSetGlobalFormat(&info);
			if (status != B_OK)
				return status;
			return user_memcpy(buffer, &info, sizeof(multi_format_info));
		}
		case B_MULTI_GET_CHANNEL_FORMATS:
			TRACE(ERR, "B_MULTI_GET_CHANNEL_FORMATS n/i\n");
			return B_ERROR;

		case B_MULTI_SET_CHANNEL_FORMATS:
			TRACE(ERR, "B_MULTI_SET_CHANNEL_FORMATS n/i\n");
			return B_ERROR;

		case B_MULTI_GET_MIX:
		case B_MULTI_SET_MIX: {
			multi_mix_value_info info;
			if (user_memcpy(&info, buffer, sizeof(multi_mix_value_info)) != B_OK)
				return B_BAD_ADDRESS;

			multi_mix_value* originalValues = info.values;
			size_t mixValueSize = info.item_count * sizeof(multi_mix_value);
			multi_mix_value* values = (multi_mix_value*)alloca(mixValueSize);
			if (user_memcpy(values, info.values, mixValueSize) != B_OK)
				return B_BAD_ADDRESS;
			info.values = values;

			status_t status;
			if (op == B_MULTI_GET_MIX)
				status = _MultiGetMix(&info);
			else
				status = _MultiSetMix(&info);
			if (status != B_OK)
				return status;
			// the multi_mix_value_info is not modified
			return user_memcpy(originalValues, values, mixValueSize);
		}

		case B_MULTI_LIST_MIX_CHANNELS:
			TRACE(ERR, "B_MULTI_LIST_MIX_CHANNELS n/i\n");
			return B_ERROR;

		case B_MULTI_LIST_MIX_CONTROLS:
		{
			multi_mix_control_info info;
			multi_mix_control* original_controls;
			size_t allocSize;
			multi_mix_control *controls;

			if (user_memcpy(&info, buffer, sizeof(multi_mix_control_info))
				!= B_OK) {
				return B_BAD_ADDRESS;
			}

			original_controls = info.controls;
			allocSize = sizeof(multi_mix_control) * info.control_count;
			controls = (multi_mix_control *)malloc(allocSize);
			if (controls == NULL)
				return B_NO_MEMORY;

			if (!IS_USER_ADDRESS(info.controls)
				|| user_memcpy(controls, info.controls, allocSize) < B_OK) {
				free(controls);
				return B_BAD_ADDRESS;
			}
			info.controls = controls;

			status_t status = _MultiListMixControls(&info);
			if (status != B_OK) {
				free(controls);
				return status;
			}

			info.controls = original_controls;
			status = user_memcpy(info.controls, controls, allocSize);
			if (status == B_OK) {
				status = user_memcpy(buffer, &info,
					sizeof(multi_mix_control_info));
			}
			if (status != B_OK)
				status = B_BAD_ADDRESS;
			free(controls);
			return status;
		}
		case B_MULTI_LIST_MIX_CONNECTIONS:
			TRACE(ERR, "B_MULTI_LIST_MIX_CONNECTIONS n/i\n");
			return B_ERROR;

		case B_MULTI_GET_BUFFERS:
			// Fill out the struct for the first time; doesn't start anything.
		{
			multi_buffer_list list;
			if (user_memcpy(&list, buffer, sizeof(multi_buffer_list)) != B_OK)
				return B_BAD_ADDRESS;

			// The request counts size on-stack arrays below and come straight
			// from userland; reject anything out of range before using them.
			if (list.request_playback_buffers < 0
				|| list.request_playback_buffers > kMaxRequestBuffers
				|| list.request_record_buffers < 0
				|| list.request_record_buffers > kMaxRequestBuffers) {
				return B_BAD_VALUE;
			}

			buffer_desc **original_playback_descs = list.playback_buffers;
			buffer_desc **original_record_descs = list.record_buffers;

			buffer_desc *playback_descs[list.request_playback_buffers];
			buffer_desc *record_descs[list.request_record_buffers];

			if (!IS_USER_ADDRESS(list.playback_buffers)
				|| user_memcpy(playback_descs, list.playback_buffers,
					sizeof(buffer_desc*) * list.request_playback_buffers)
					< B_OK
				|| !IS_USER_ADDRESS(list.record_buffers)
				|| user_memcpy(record_descs, list.record_buffers,
					sizeof(buffer_desc*) * list.request_record_buffers)
					< B_OK) {
				return B_BAD_ADDRESS;
			}

			list.playback_buffers = playback_descs;
			list.record_buffers = record_descs;
			status_t status = _MultiGetBuffers(&list);
			if (status != B_OK)
				return status;

			list.playback_buffers = original_playback_descs;
			list.record_buffers = original_record_descs;

			if (user_memcpy(buffer, &list, sizeof(multi_buffer_list)) < B_OK
				|| user_memcpy(original_playback_descs, playback_descs,
					sizeof(buffer_desc*) * list.request_playback_buffers)
					< B_OK
				|| user_memcpy(original_record_descs, record_descs,
					sizeof(buffer_desc*) * list.request_record_buffers)
					< B_OK) {
				status = B_BAD_ADDRESS;
			}

			return status;
		}

		case B_MULTI_SET_BUFFERS:
		{
			// Our client (the multi_audio media add-on) clones our buffer
			// areas into its team and tells us its base addresses here
			// (informational - the client reads/writes the pages through its
			// clone; the driver reads them through the original area VA).
			// The layout inside the area is unchanged - we only need
			// buffer[0][0].base per direction.
			multi_buffer_list list;
			if (user_memcpy(&list, buffer, sizeof(multi_buffer_list)) != B_OK)
				return B_BAD_ADDRESS;

			uint8* playbackBase = NULL;
			uint8* recordBase = NULL;
			buffer_desc* row = NULL;
			buffer_desc desc;
			if (list.request_playback_buffers > 0
				&& list.playback_buffers != NULL
				&& user_memcpy(&row, list.playback_buffers,
					sizeof(buffer_desc*)) == B_OK
				&& row != NULL
				&& user_memcpy(&desc, row, sizeof(buffer_desc)) == B_OK)
				playbackBase = (uint8*)desc.base;
			row = NULL;
			if (list.request_record_buffers > 0
				&& list.record_buffers != NULL
				&& user_memcpy(&row, list.record_buffers,
					sizeof(buffer_desc*)) == B_OK
				&& row != NULL
				&& user_memcpy(&desc, row, sizeof(buffer_desc)) == B_OK)
				recordBase = (uint8*)desc.base;

			TRACE(INF, "SET_BUFFERS client bases: playback=%p"
				" record=%p\n", playbackBase, recordBase);

			for (int i = 0; i < fStreams.Count(); i++) {
				fStreams[i]->SetClientBuffer(fStreams[i]->IsInput()
					? recordBase : playbackBase);
			}
			return B_OK;
		}

		case B_MULTI_SET_START_TIME:
			// When to actually start
			TRACE(ERR, "B_MULTI_SET_START_TIME n/i\n");
			return B_ERROR;

		case B_MULTI_BUFFER_EXCHANGE:
			// stop and go are derived from this being called
			return _MultiBufferExchange((multi_buffer_info*)buffer);

		case B_MULTI_BUFFER_FORCE_STOP:
			// force stop of playback, nothing in data
			return _MultiBufferForceStop();

		default:
			TRACE(ERR, "Unhandled IOCTL catched: %#010x\n", op);
	}

	return B_DEV_INVALID_IOCTL;
}


void
Device::Removed()
{
	fRemoved = true;

	for (int i = 0; i < fStreams.Count(); i++)
		fStreams[i]->OnRemove();

	// DO NOT wake the buffer-exchange waiter here. Doing so hard-hangs the
	// machine: no KDL, no panic, no serial -- the failure that blocked replug
	// testing since 2026-07-28.
	//
	// We are called from inside the USB stack's removal path holding its
	// locks, and the thread waiting on fBuffersReadySem holds the media node
	// lock. Releasing the semaphore hands it the CPU mid-teardown and the two
	// meet head-on. B_DO_NOT_RESCHEDULE does NOT save us -- disproved at the
	// instruction level (the running binary showed `mov $0xa,%edx` at this
	// call site and died identically). Deferring the switch is not enough:
	// any yield before we drop these locks is fatal, and the very next
	// statement after the release was one.
	//
	// The wake was never needed. _MultiBufferExchange() already waits with a
	// 2-second B_RELATIVE_TIMEOUT and re-checks fRemoved immediately after,
	// returning B_CANCELED -- and fRemoved is set at the top of this function,
	// before we get here. So the waiter frees itself within 2 s regardless.
	// The release only ever bought latency, and it cost the machine.
	//
	// History: the bounded wait landed in b8b69c1 (2026-07-04); the release
	// was added two days later in 4a34695, the rations/haiku UAC2 merge whose
	// reattach code was adopted untested. It was redundant the day it arrived.
	//
	// Proven by removing it: Device::Removed() ran to completion, where every
	// previous run died here. The same capture holds its own control -- an RMX
	// unplugged *idle* on the old binary also completed, because nothing was
	// waiting on the semaphore.
	// See docs/usb-audio-removal-hang.md, "Resolution -- 2026-08-10".
}


status_t
Device::SetupDevice(bool deviceReplugged)
{
	return B_OK;
}


status_t
Device::CompareAndReattach(usb_device device)
{
	const usb_device_descriptor* deviceDescriptor
		= gUSBModule->get_device_descriptor(device);

	if (deviceDescriptor == NULL) {
		TRACE(ERR, "Error of getting USB device descriptor.\n");
		return B_ERROR;
	}

	if (deviceDescriptor->vendor_id != fVendorID
		|| deviceDescriptor->product_id != fProductID)
		// this certainly isn't the same device
		return B_BAD_VALUE;

	// this is the same device that was replugged - clear the removed state,
	// re-bind the existing streams (and with them the sample buffers already
	// published to the consumer) to the new usb_device, and open the device
	// if it was previously opened. _SetupEndpoints() must not run again here:
	// it would create a second set of streams alongside the stale ones.
	fDevice = device;
	fRemoved = false;

	const usb_configuration_info* config
		= gUSBModule->get_nth_configuration(fDevice, 0);
	status_t result = config != NULL ? B_OK : B_ERROR;
	if (result == B_OK)
		result = gUSBModule->set_configuration(fDevice, config);
	for (int i = 0; result == B_OK && i < fStreams.Count(); i++)
		result = fStreams[i]->OnReattach(fDevice, config);
	if (result != B_OK) {
		fRemoved = true;
		return result;
	}

	// An R2 device's streams share their clock; restore the rate that was
	// last programmed before the removal, rather than letting each stream
	// re-apply its own selection (whichever came last would win and could
	// change the clock underneath the other stream).
	if (fAudioControl.SpecReleaseNumber() >= 0x200
			&& fAudioControl.LastClockId() != 0) {
		result = fAudioControl.SetSamplingRate(fAudioControl.LastClockId(),
			fAudioControl.LastClockRate());
		if (result != B_OK) {
			fRemoved = true;
			return result;
		}
	}

	// The replugged device starts with an empty FIFO; drop any feedback
	// packets left from before the unplug so they cannot pace the restarted
	// playback stream.
	atomic_set(&fFeedbackRingHead, 0);
	atomic_set(&fFeedbackRingTail, 0);

	// Drain stale buffer-ready tokens (transfers that completed after the
	// last exchange, and the removal wake-up); the restarted streams begin
	// with no processed buffers.
	if (fBuffersReadySem > B_OK) {
		while (acquire_sem_etc(fBuffersReadySem, 1, B_RELATIVE_TIMEOUT, 0)
				== B_OK)
			;
	}

	// we need to setup hardware on device replug
	result = SetupDevice(true);
	if (result != B_OK)
		return result;

	if (fOpen) {
		fOpen = false;
		result = Open(fNonBlocking ? O_NONBLOCK : 0);
	}

	return result;
}


status_t
Device::_MultiGetDescription(multi_description* multiDescription)
{
	multi_description Description;
	if (user_memcpy(&Description, multiDescription,
			sizeof(multi_description)) != B_OK)
		return B_BAD_ADDRESS;

	Description.interface_version = B_CURRENT_INTERFACE_VERSION;
	Description.interface_minimum = B_CURRENT_INTERFACE_VERSION;

	// Use the device's own product string as the name shown in Media
	// preferences; fall back to a generic label when the device reports none.
	// Identical models are disambiguated with a " #2", " #3"... suffix (the
	// first stays unnumbered). The field is fixed-size, so keep as much of the
	// product name as fits while always preserving the suffix.
	const char* baseName
		= fProductName[0] != '\0' ? fProductName : "USB Audio";
	size_t cap = sizeof(Description.friendly_name);
	if (fInstanceIndex > 0) {
		char suffix[8];
		snprintf(suffix, sizeof(suffix), " #%d", (int)(fInstanceIndex + 1));
		size_t suffixLen = strlen(suffix);
		if (suffixLen >= cap)
			suffixLen = 0;
		// Copy the product name into the room left before the suffix, then
		// append the suffix at the actual end (strlcpy only, no strlcat).
		strlcpy(Description.friendly_name, baseName, cap - suffixLen);
		size_t nameLen = strlen(Description.friendly_name);
		strlcpy(Description.friendly_name + nameLen, suffix, cap - nameLen);
	} else
		strlcpy(Description.friendly_name, baseName, cap);

	strlcpy(Description.vendor_info, "S.Zharski",
		sizeof(Description.vendor_info));

	Description.output_channel_count = 0;
	Description.input_channel_count = 0;
	Description.output_bus_channel_count = 0;
	Description.input_bus_channel_count = 0;
	Description.aux_bus_channel_count = 0;

	Description.output_rates = 0;
	Description.input_rates = 0;

	Description.min_cvsr_rate = 0;
	Description.max_cvsr_rate = 0;

	Description.output_formats = 0;
	Description.input_formats = 0;
	Description.lock_sources = B_MULTI_LOCK_INTERNAL;
	Description.timecode_sources = 0;
	Description.interface_flags = 0;
	Description.start_latency = 3000;

	Description.control_panel[0] = '\0';

	Vector<multi_channel_info> Channels;

	// channels (USB I/O terminals) are already in fStreams in outputs->inputs order.
	for (int i = 0; i < fStreams.Count(); i++) {
		Vector<_AudioControl*> USBTerminal;
		USBTerminal.PushBack(fAudioControl.Find(fStreams[i]->TerminalLink()));

		fAudioControl.GetChannelsDescription(Channels, &Description, USBTerminal,
			fStreams[i]->IsInput());
		fStreams[i]->GetFormatsAndRates(&Description);
	}

	fAudioControl.GetBusChannelsDescription(Channels, &Description);

	// Streams that share a clock cannot run at different rates, so do not
	// ADVERTISE a rate only one of them can reach: intersect the two lists and
	// offer the honest set.
	//
	// This is deliberately prevention rather than correction. Reconciling a
	// mismatched request inside B_MULTI_SET_GLOBAL_FORMAT was tried and
	// reverted (see _MultiSetGlobalFormat): the ioctl copies the adjusted
	// values back into the add-on's cached fFormatInfo, and
	// MultiAudioDevice::SetInputFrameRate() then early-returns whenever that
	// cache already matches -- so silently "fixing" a request destroyed the
	// user's only way of recovering from a mismatch, which was to set it back.
	// Never quietly change what the caller asked for; change what they are
	// allowed to ask for.
	//
	// Independent clocks keep their full separate lists, which is why the
	// detection had to come first -- an S/PDIF input locked to an incoming
	// stream genuinely does run at a different rate from the DAC.
	if (_SharedClockId() != 0
			&& Description.input_rates != 0 && Description.output_rates != 0) {
		uint32 common = Description.input_rates & Description.output_rates;
		if (common != 0) {
			if (common != Description.input_rates
					|| common != Description.output_rates) {
				TRACE(INF, "Shared clock: advertising the common rates "
					"%#010x (in was %#010x, out was %#010x).\n", common,
					Description.input_rates, Description.output_rates);
			}
			Description.input_rates = common;
			Description.output_rates = common;
		} else {
			// No rate both can do. Leave the lists alone rather than
			// advertising nothing at all -- a device like that is already
			// broken for simultaneous use, and hiding every rate would make it
			// unusable in either direction.
			TRACE(ERR, "Shared clock but no common rate (in %#010x, out "
				"%#010x); leaving both lists as they are.\n",
				Description.input_rates, Description.output_rates);
		}
	}

	// Description.request_channel_count = channels + bus_channels;

	TraceMultiDescription(&Description, Channels);

	if (user_memcpy(multiDescription, &Description,
			sizeof(multi_description)) != B_OK)
		return B_BAD_ADDRESS;

	if (user_memcpy(multiDescription->channels,
			&Channels[0], sizeof(multi_channel_info) * min_c(Channels.Count(),
			Description.request_channel_count)) != B_OK)
		return B_BAD_ADDRESS;

	return B_OK;
}


void
Device::TraceMultiDescription(multi_description* Description,
		Vector<multi_channel_info>& Channels)
{
	TRACE(API, "interface_version:%d\n", Description->interface_version);
	TRACE(API, "interface_minimum:%d\n", Description->interface_minimum);
	TRACE(API, "friendly_name:%s\n", Description->friendly_name);
	TRACE(API, "vendor_info:%s\n", Description->vendor_info);
	TRACE(API, "output_channel_count:%d\n", Description->output_channel_count);
	TRACE(API, "input_channel_count:%d\n", Description->input_channel_count);
	TRACE(API, "output_bus_channel_count:%d\n",
		Description->output_bus_channel_count);
	TRACE(API, "input_bus_channel_count:%d\n",
		Description->input_bus_channel_count);
	TRACE(API, "aux_bus_channel_count:%d\n", Description->aux_bus_channel_count);
	TRACE(API, "output_rates:%#08x\n", Description->output_rates);
	TRACE(API, "input_rates:%#08x\n", Description->input_rates);
	TRACE(API, "min_cvsr_rate:%f\n", Description->min_cvsr_rate);
	TRACE(API, "max_cvsr_rate:%f\n", Description->max_cvsr_rate);
	TRACE(API, "output_formats:%#08x\n", Description->output_formats);
	TRACE(API, "input_formats:%#08x\n", Description->input_formats);
	TRACE(API, "lock_sources:%d\n", Description->lock_sources);
	TRACE(API, "timecode_sources:%d\n", Description->timecode_sources);
	TRACE(API, "interface_flags:%#08x\n", Description->interface_flags);
	TRACE(API, "start_latency:%d\n", Description->start_latency);
	TRACE(API, "control_panel:%s\n", Description->control_panel);

	// multi_channel_info* Channels = Description->channels;
	// for (int i = 0; i < Description->request_channel_count; i++) {
	for (int i = 0; i < Channels.Count(); i++) {
		TRACE(API, " channel_id:%d\n", Channels[i].channel_id);
		TRACE(API, "  kind:%#02x\n", Channels[i].kind);
		TRACE(API, "  designations:%#08x\n", Channels[i].designations);
		TRACE(API, "  connectors:%#08x\n", Channels[i].connectors);
	}

	TRACE(API, "request_channel_count:%d\n\n",
		Description->request_channel_count);
}


status_t
Device::_MultiGetEnabledChannels(multi_channel_enable* Enable)
{
	status_t status = B_OK;

	Enable->lock_source = B_MULTI_LOCK_INTERNAL;

	uint32 offset = 0;
	for (int i = 0; i < fStreams.Count() && status == B_OK; i++)
		status = fStreams[i]->GetEnabledChannels(offset, Enable);

	return status;
}


status_t
Device::_MultiSetEnabledChannels(multi_channel_enable* Enable)
{
	status_t status = B_OK;
	uint32 offset = 0;
	for (int i = 0; i < fStreams.Count() && status == B_OK; i++)
		status = fStreams[i]->SetEnabledChannels(offset, Enable);

	return status;
}


status_t
Device::_MultiGetGlobalFormat(multi_format_info* Format)
{
	status_t status = B_OK;

	Format->output_latency = 0;
	Format->input_latency = 0;
	Format->timecode_kind = 0;

	// uint32 offset = 0;
	for (int i = 0; i < fStreams.Count() && status == B_OK; i++)
		status = fStreams[i]->GetGlobalFormat(Format);

	return status;
}


/*!	The Clock Source every stream resolves to, or 0 if they do not share one.

	UAC2 makes the clock an entity: each terminal names its clock through
	bCSourceID, so "do these streams share a clock?" is a question the descriptors
	answer, once the graph is walked (through any Clock Selector / Multiplier) to
	the Clock Source that sampling-frequency requests actually address.

	0 means "treat the streams as independent", and covers three different cases
	deliberately: genuinely separate clocks, a graph we could not resolve, and
	UAC1 -- where there are no clock entities at all, rates are set per endpoint,
	and nothing in the descriptors says whether the hardware shares a clock. In
	all three the safe answer is to leave the streams alone.
*/
uint8
Device::_SharedClockId()
{
	if (fAudioControl.SpecReleaseNumber() < 0x200)
		return 0;

	uint8 shared = 0;
	for (int i = 0; i < fStreams.Count(); i++) {
		uint8 clockId = fAudioControl.ClockSourceIdForTerminal(
			fStreams[i]->TerminalLink());
		TRACE(INF, "clock check: stream %d (%s) terminal %d -> clock %d\n",
			i, fStreams[i]->IsInput() ? "in" : "out",
			fStreams[i]->TerminalLink(), clockId);
		if (clockId == 0)
			return 0;
		if (shared == 0)
			shared = clockId;
		else if (shared != clockId)
			return 0;
	}

	return shared;
}


status_t
Device::_MultiSetGlobalFormat(multi_format_info* Format)
{
	status_t status = B_OK;

	TRACE(API, "output_latency:%lld\n", Format->output_latency);
	TRACE(API, "input_latency:%lld\n",  Format->input_latency);
	TRACE(API, "timecode_kind:%#08x\n", Format->timecode_kind);

	// Streams on one clock cannot hold different rates, and the media kit hands
	// us both halves in this one struct -- so reconcile here, before either
	// stream acts on it. Left alone, each stream programs the shared Clock
	// Source with its own rate and the last writer wins: a DN/RMX2 with
	// playback at 44100 and capture at 96000 produced set_speed 88200 followed
	// 56 ms later by set_speed 44100, while playback carried on streaming at the
	// old rate. That is audible as intermittent glitching, with no error
	// reported anywhere. See docs/usb-audio-start-retry-freeze.md, D4.
	uint8 sharedClock = _SharedClockId();
	TRACE(INF, "SET_GLOBAL_FORMAT: in rate %#010x fmt %#010x, out rate "
		"%#010x fmt %#010x, sharedClock %d, streams %d\n",
		Format->input.rate, Format->input.format,
		Format->output.rate, Format->output.format,
		sharedClock, (int)fStreams.Count());

	if (sharedClock != 0 && Format->input.rate != Format->output.rate) {
		uint32 winner = Format->output.rate;
		Stream* outputStream = NULL;
		Stream* inputStream = NULL;
		for (int i = 0; i < fStreams.Count(); i++) {
			if (fStreams[i]->IsInput())
				inputStream = fStreams[i];
			else
				outputStream = fStreams[i];
		}

		if (outputStream != NULL && inputStream != NULL) {
			bool outputChanged = !outputStream->HasRateId(Format->output.rate);
			bool inputChanged = !inputStream->HasRateId(Format->input.rate);

			// While playback is running the OUTPUT always wins, even if the
			// user just changed the input. On a shared clock there is only one
			// rate, so honouring an input change means yanking the clock out
			// from under the audio that is currently playing -- observed as
			// "music stops the moment I touch the input rate". The input
			// control appearing to snap back is confusing; silently killing
			// playback is worse.
			//
			// With nothing playing there is nothing to protect, so the side
			// that actually changed wins -- otherwise a capture-only user could
			// never set a rate at all, the input control being permanently
			// overridden by an idle output stream.
			if (!outputStream->IsRunning() && inputChanged && !outputChanged)
				winner = Format->input.rate;

			// Only force a rate the other stream can actually reach; if it
			// cannot, leave the request alone and let that stream fail on its
			// own terms rather than handing it something impossible.
			Stream* loser = (winner == Format->output.rate)
				? inputStream : outputStream;
			if (!loser->SupportsRateId(winner)) {
				TRACE(ERR, "Streams share clock %d but %s cannot do the "
					"requested rate id %#010x; leaving them mismatched.\n",
					sharedClock, loser->IsInput() ? "input" : "output", winner);
				winner = 0;
			}
		}

		if (winner != 0) {
			// DIAGNOSIS ONLY -- the rewrite this used to perform is REVERTED.
			//
			// Forcing both halves here silently changed the caller's request,
			// and the ioctl copies the result back into the add-on's cached
			// fFormatInfo. MultiAudioDevice::SetInputFrameRate() then early-
			// returns whenever its cache already matches the requested rate --
			// so after we rewrote input 44100 to 48000, asking for 48000 again
			// never reached the driver, and the user's way of recovering from a
			// mismatch (set it back) became a no-op. Observed on hardware
			// 2026-08-11: "putting the input frequency back to match no longer
			// stops the distortion."
			//
			// More fundamentally, rewriting a request leaves the driver, the
			// add-on's cache and the node's negotiated media_format free to
			// disagree with nothing upstream able to notice. An honest mismatch
			// is better than a hidden one.
			//
			// The fix belongs earlier: on a shared clock, do not ADVERTISE
			// combinations that cannot exist (intersect the rate lists in
			// GetFormatsAndRates), so an impossible pair is never offered.
			// Rejecting the ioctl outright is the other candidate, but risks
			// breaking init, where the two halves are set in separate calls and
			// are legitimately mismatched in between.
			TRACE(ERR, "Rate mismatch on shared clock %d: in %#010x / out "
				"%#010x, outputRunning %d -- not reconciled, see D4.\n",
				sharedClock, Format->input.rate, Format->output.rate,
				outputStream != NULL ? (int)outputStream->IsRunning() : -1);
		}
	}

	// uint32 offset = 0;
	for (int i = 0; i < fStreams.Count() && status == B_OK; i++)
		status = fStreams[i]->SetGlobalFormat(Format);

	return status;
}


status_t
Device::_MultiGetBuffers(multi_buffer_list* List)
{
	status_t status = B_OK;

	TRACE(API, "info_size:%d\n"
	"request_playback_buffers:%d\n"
	"request_playback_channels:%d\n"
	"request_playback_buffer_size:%d\n"
	"request_record_buffers:%d\n"
	"request_record_channels:%d\n"
	"request_record_buffer_size:%d\n",
		List->info_size,
		List->request_playback_buffers,
		List->request_playback_channels,
		List->request_playback_buffer_size,
		List->request_record_buffers,
		List->request_record_channels,
		List->request_record_buffer_size);

	List->flags = 0;
	List->return_playback_channels = 0;
	List->return_record_channels = 0;

	for (int i = 0; i < fStreams.Count() && status == B_OK; i++)
		status = fStreams[i]->GetBuffers(List);

	TRACE(API, "flags:%#x\n"
	"return_playback_buffers:%d\n"
	"return_playback_channels:%d\n"
	"return_playback_buffer_size:%d\n"
	"return_record_buffers:%d\n"
	"return_record_channels:%d\n"
	"return_record_buffer_size:%d\n",
		List->flags,
		List->return_playback_buffers,
		List->return_playback_channels,
		List->return_playback_buffer_size,
		List->return_record_buffers,
		List->return_record_channels,
		List->return_record_buffer_size);

#if 0
	TRACE(API, "playback buffers\n");
	for (int32_t b = 0; b <  List->return_playback_buffers; b++)
		for (int32 c = 0; c < List->return_playback_channels; c++)
			TRACE(API, "%d:%d %08x:%d\n", b, c, List->playback_buffers[b][c].base,
				List->playback_buffers[b][c].stride);

	TRACE(API, "record buffers:\n");
	for (int32_t b = 0; b <  List->return_record_buffers; b++)
		for (int32 c = 0; c < List->return_record_channels; c++)
			TRACE(API, "%d:%d %08x:%d\n", b, c, List->record_buffers[b][c].base,
				List->record_buffers[b][c].stride);
#endif
	return B_OK;
}


status_t
Device::_MultiBufferExchange(multi_buffer_info* multiInfo)
{
	multi_buffer_info Info;
	if (!IS_USER_ADDRESS(multiInfo)
		|| user_memcpy(&Info, multiInfo, sizeof(multi_buffer_info)) != B_OK) {
		return B_BAD_ADDRESS;
	}

	// Fail promptly and predictably once the device is gone: the caller's
	// thread must not be left blocked (it holds its node lock), and no new
	// transfers may be queued on the dead pipes.
	if (fRemoved)
		return B_CANCELED;

	// A fresh run starts when no stream is running (the first exchange after
	// open, a force-stop or a format change). Drop whatever the previous run
	// left behind before starting: ready-buffer tokens released after its
	// last exchange would otherwise satisfy this acquire with no processed
	// buffer to hand out (the exchange then fails instead of blocking), and
	// leftover feedback-ring entries describe the old run's rate.
	bool anyRunning = false;
	for (int i = 0; i < fStreams.Count(); i++)
		anyRunning = anyRunning || fStreams[i]->IsRunning();
	if (!anyRunning) {
		while (acquire_sem_etc(fBuffersReadySem, 1, B_RELATIVE_TIMEOUT, 0)
				== B_OK)
			;
		atomic_set(&fFeedbackRingHead, 0);
		atomic_set(&fFeedbackRingTail, 0);
	}

	for (int i = 0; i < fStreams.Count(); i++) {
		// "no_input true" in the driver settings leaves capture streams
		// parked at alternate 0 (see _SetupEndpoints) and never starts them.
		if (gSkipInputStreams && fStreams[i]->IsInput())
			continue;
		// A stream that has failed to start too many times is left alone. We
		// are called once per buffer exchange, so retrying one that cannot
		// start is not a harmless log line: each attempt re-arms the clock,
		// resolves a fresh set of endpoints and asks the stack for its
		// buffers. An RMX2 capture stream whose 172032-byte request exceeds
		// the USB allocator's 128 KB block did that 196 times in one session
		// -- 196 leaked endpoints and ~4800 syslog lines -- and starved the
		// media server badly enough to freeze the desktop until the device
		// was unplugged. See docs/usb-audio-start-retry-freeze.md.
		if (fStreams[i]->IsStartParked())
			continue;
		if (!fStreams[i]->IsRunning()) {
			status_t startStatus = fStreams[i]->Start();
			if (startStatus != B_OK)
				TRACE(ERR, "Stream %d Start() failed: %#010x\n", i,
					startStatus);
		}
	}

	// If nothing is running by now, nothing can ever release the semaphore, so
	// waiting the full 2 s only makes the caller sluggish for no information:
	// it holds its node lock while it waits, which is what a user experiences
	// as "the desktop froze". That is the state a parked stream leaves behind,
	// and parking is permanent until the format changes or the device is
	// replugged -- so this would otherwise stall every exchange, forever.
	// Report it promptly instead and let the node give up.
	//
	// Deliberately AFTER the start loop, and conditioned on running rather than
	// on parked: a stream that has just been started successfully is running,
	// and a stream that is running but whose completions have stalled must still
	// get the bounded wait, because detecting that stall is what the wait is for.
	bool running = false;
	for (int i = 0; i < fStreams.Count(); i++)
		running = running || fStreams[i]->IsRunning();
	if (!running) {
		// Rate-limited: the node asks once per exchange and this must not
		// become the log flood it is meant to replace.
		static bigtime_t sLastNotRunningLog = 0;
		bigtime_t now = system_time();
		if (now - sLastNotRunningLog >= 1000000) {
			sLastNotRunningLog = now;
			TRACE(ERR, "Buffer exchange with no running stream; every stream is "
				"parked or failed to start.\n");
		}
		return B_DEV_NOT_READY;
	}

	// Bounded wait instead of forever, so a completion stall shows up in the
	// syslog (and the media node gets an error instead of a wedged thread).
	status_t status = acquire_sem_etc(fBuffersReadySem, 1,
		B_CAN_INTERRUPT | B_RELATIVE_TIMEOUT, 2000000);
	if (status != B_OK) {
		// Removal must be reported AS a removal, and this is the path it
		// actually takes: once the device is gone nothing ever releases the
		// semaphore again, so the acquire times out rather than succeeding.
		// Checking fRemoved only after a successful acquire (as this did until
		// 2026-08-10) meant the caller was handed a bare B_TIMED_OUT and could
		// not tell "the device is gone" from "a completion stalled" -- so
		// multi_audio kept the device open, and the node lingered.
		if (fRemoved)
			return B_CANCELED;

		// Interrupted (the caller is being signalled/killed) or a genuine
		// stall; hand the real reason to the caller instead of falling
		// through and failing the exchange with a generic error.
		TRACE(ERR, "Buffers exchange aborted: %#010x\n", status);
		return status;
	}
	if (fRemoved)
		return B_CANCELED;

	status = B_ERROR;
	for (int i = 0; i < fStreams.Count(); i++) {
		if (fStreams[i]->ExchangeBuffer(&Info)) {
			status = B_OK;
			break;
		}
	}

	if (status != B_OK) {
		TRACE(ERR, "Error processing buffers:%08x.\n", status);
		return status;
	}

	if (user_memcpy(multiInfo, &Info, sizeof(multi_buffer_info)) != B_OK)
		return B_BAD_ADDRESS;

	return status;
}


status_t
Device::_MultiBufferForceStop()
{
	for (int i = 0; i < fStreams.Count(); i++)
		fStreams[i]->Stop();
	return B_OK;
}


status_t
Device::_MultiGetMix(multi_mix_value_info* Info)
{
	return fAudioControl.GetMix(Info);
}


status_t
Device::_MultiSetMix(multi_mix_value_info* Info)
{
	return fAudioControl.SetMix(Info);
}


status_t
Device::_MultiListMixControls(multi_mix_control_info* Info)
{
	status_t status = fAudioControl.ListMixControls(Info);
	TraceListMixControls(Info);
	return status;
}


void
Device::TraceListMixControls(multi_mix_control_info* Info)
{
	TRACE(MIX, "control_count:%d\n.", Info->control_count);

	int32 i = 0;
	while (Info->controls[i].id > 0) {
		multi_mix_control &c = Info->controls[i];
		TRACE(MIX, "id:%#08x\n",		c.id);
		TRACE(MIX, "flags:%#08x\n",	c.flags);
		TRACE(MIX, "master:%#08x\n", c.master);
		TRACE(MIX, "parent:%#08x\n", c.parent);
		TRACE(MIX, "string:%d\n",	c.string);
		TRACE(MIX, "name:%s\n",		c.name);
		i++;
	}
}


status_t
Device::_SetupEndpoints()
{
	const usb_configuration_info* config
		= gUSBModule->get_nth_configuration(fDevice, 0);

	if (config == NULL) {
		TRACE(ERR, "Error of getting USB device configuration.\n");
		return B_ERROR;
	}

	if (config->interface_count <= 0) {
		TRACE(ERR, "Error:no interfaces found in USB device configuration\n");
		return B_ERROR;
	}

	// On a replug (CompareAndReattach) the streams and the audio-control tree
	// already exist. Re-running discovery would append DUPLICATE Stream
	// objects - whose buffers are never configured, so starting them submits
	// zero-packet isochronous transfers (divide-by-zero KDL in the host
	// controller) - and duplicate audio controls. (CompareAndReattach no
	// longer calls here, but keep the guard as a backstop.)
	bool reattached = fStreams.Count() > 0;

	for (size_t i = 0; i < config->interface_count; i++) {
		usb_interface_info* Interface = config->interface[i].active;
		if (Interface == NULL || Interface->descr == NULL)
			continue;
		if (Interface->descr->interface_class != USB_AUDIO_INTERFACE_AUDIO_CLASS)
			continue;

		switch (Interface->descr->interface_subclass) {
			case USB_AUDIO_INTERFACE_AUDIOCONTROL_SUBCLASS:
				if (!reattached)
					fAudioControl.Init(i, Interface);
				break;
			case USB_AUDIO_INTERFACE_AUDIOSTREAMING_SUBCLASS:
				if (!reattached) {
					Stream* stream = new(std::nothrow) Stream(this, i,
						&config->interface[i]);
					if (stream->Init() == B_OK) {
						// put the stream in the correct order:
						// first output that input ones.
						if (stream->IsInput())
							fStreams.PushBack(stream);
						else
							fStreams.PushFront(stream);
					} else
						delete stream;
				}
				break;
			default:
				TRACE(ERR, "Ignore interface of unsupported subclass %#x.\n",
					Interface->descr->interface_subclass);
				break;
		}
	}

	if (fAudioControl.InitCheck() == B_OK && fStreams.Count() > 0) {
		TRACE(INF, "Found device %#06x:%#06x\n", fVendorID, fProductID);

		status_t status = gUSBModule->set_configuration(fDevice, config);
		if (status != B_OK)
			return status;

		// Explicitly park every streaming interface at alternate 0 before any
		// stream selects its active alternate - back-to-back, the way Windows
		// and Linux do at probe time. The Hercules RMX accepts alt 0
		// immediately but refuses a jump to an active alternate for a long
		// time after configuration; parking first is part of the sequence
		// every working host performs.
		for (int i = 0; i < fStreams.Count(); i++) {
			size_t ifaceIndex = fStreams[i]->fInterface;
			status_t parkStatus = gUSBModule->set_alt_interface(fDevice,
				&config->interface[ifaceIndex].alt[0]);
			TRACE(INF, "park iface %d alt 0 -> %#x\n", (int)ifaceIndex,
				(int)parkStatus);
		}

		// With no_input set, leave the recording interface PARKED at alt 0
		// instead of just not starting its stream. No working host ever has
		// the RMX's UAC IN alternate active while rendering (Windows runs
		// duplex only through vendor interface 7 alt 3; Linux renders with
		// interface 3 at alt 0) - the UAC personality may be half-duplex.

		// NOTE (Hercules RMX, 2026-07-03/04 findings): the RMX renders plain
		// UAC audio on interface 2 alt 1 with nothing beyond the standard
		// sampling-frequency request (verified with a Linux usbmon capture on
		// the same UHCI hardware). Its Windows driver's vendor writes (req 5
		// val 0xa0ff/0xa1a0, req 3, req 21) are a LOCK, not an unlock: once
		// vendor status (0xc0 req 6) reads 0xa0xx instead of the power-up
		// 0x20xx, the device STALLs SET_INTERFACE(alt 1) on the UAC
		// interfaces indefinitely (only a power cycle clears the latch, which
		// survives warm reboots). So: never send those vendor commands; park
		// alt 0 first (all working hosts do); the retry loop in
		// Stream::OnSetConfiguration reports how long alt 1 takes if a
		// settle window exists.

		for (int i = 0; i < fStreams.Count(); i++) {
			if (gSkipInputStreams && fStreams[i]->IsInput()) {
				TRACE(INF, "no_input: leaving input iface parked"
					" at alt 0\n");
				continue;
			}
			fStreams[i]->OnSetConfiguration(fDevice, config);
		}

		// Unmute and open the hardware audio path; some devices power up
		// muted (see InitHardwareGains). Done on replug too.
		fAudioControl.InitHardwareGains();

		return B_OK;
	}

	return B_NO_INIT;
}


status_t
Device::StopDevice()
{
	status_t result = B_OK;

	if (result != B_OK)
		TRACE(ERR, "Error of writing %#04x RX Control:%#010x\n", 0, result);

	//TRACE_RET(result);
	return result;
}

