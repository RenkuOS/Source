/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Distributed under the terms of the MIT license.
 *
 */


#include "Settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <driver_settings.h>
#include <lock.h>

#include "Driver.h"


uint32 gTraceMask = ERR;
bool gSkipInputStreams = false;
bool gSkipFeedback = false;
bool gTestTone = false;
uint32 gTestToneLevel = 0x04;
bool gRearmAlternate = false;
bool gRejectUnverifiedRate = true;
int32 gForceAlternate = -1;
uint32 gMaxRate = 0;
bool gTruncateLogFile = false;
bool gAddTimeStamp = true;
static char* gLogFilePath = NULL;
mutex gLogLock;

static
void create_log()
{
	if (gLogFilePath == NULL)
		return;

	int flags = O_WRONLY | O_CREAT | ((gTruncateLogFile) ? O_TRUNC : 0);
	int fd = open(gLogFilePath, flags, 0666);
	if (fd >= 0)
		close(fd);

	mutex_init(&gLogLock, DRIVER_NAME"-logging");
}


void load_settings()
{
	void* handle = load_driver_settings(DRIVER_NAME);
	if (handle == 0)
		return;

	gTraceMask = strtoul(get_driver_parameter(handle, "trace", "1", "0"), 0, 0);
	gSkipInputStreams = get_driver_boolean_parameter(handle, "no_input",
						gSkipInputStreams, true);
	gSkipFeedback = get_driver_boolean_parameter(handle, "no_feedback",
						gSkipFeedback, true);
	gTestTone = get_driver_boolean_parameter(handle, "test_tone",
						gTestTone, true);

	// Clamped, not trusted: a fat-fingered level here goes straight into
	// somebody's monitors at whatever gain the device is set to. The ceiling is
	// the level this used to ship with, which was measured as far too loud.
	gTestToneLevel = strtoul(get_driver_parameter(handle, "test_tone_level",
						"4", "4"), 0, 0);
	if (gTestToneLevel < 1)
		gTestToneLevel = 1;
	if (gTestToneLevel > 0x40)
		gTestToneLevel = 0x40;

	// Re-select the streaming alternate after programming a UAC1 endpoint's
	// sampling frequency (see Stream::_SetDeviceSamplingRate).
	gRearmAlternate = get_driver_boolean_parameter(handle, "rearm_alternate",
						gRearmAlternate, true);

	// Whether a sampling rate the device did not confirm is rejected outright
	// (see Stream::_SetDeviceSamplingRate). Default on, matching the shipped
	// behaviour; set false to let the stream run anyway and find out whether a
	// device that answers GET_CUR with nonsense still plays correctly.
	gRejectUnverifiedRate = get_driver_boolean_parameter(handle,
						"reject_unverified_rate", gRejectUnverifiedRate, true);
	// Override the streaming alternate _ChooseAlternate would have picked.
	// Selection scores channels*100 + bitResolution and ignores sample rate, so
	// a device advertising a rate on every alternate always gets its heaviest
	// one - even if only a lighter alternate really implements that rate.
	// -1 keeps the automatic choice.
	gForceAlternate = strtol(get_driver_parameter(handle, "force_alternate",
						"-1", "-1"), 0, 0);

	// Highest sampling rate to advertise to the media kit, in Hz (0 = no cap).
	// The media kit selects the highest rate offered, so a device that lists a
	// rate it cannot actually take gets that rate programmed on every attach.
	// Capping keeps the unusable rate out of the negotiation entirely.
	gMaxRate = strtoul(get_driver_parameter(handle, "max_rate", "0", "0"), 0, 0);

	gTruncateLogFile = get_driver_boolean_parameter(handle,	"truncate_logfile",
						gTruncateLogFile, true);
	gAddTimeStamp = get_driver_boolean_parameter(handle, "add_timestamp",
						gAddTimeStamp, true);
	const char* logFilePath = get_driver_parameter(handle, "logfile",
						NULL, "/var/log/" DRIVER_NAME ".log");
	if (logFilePath != NULL)
		gLogFilePath = strdup(logFilePath);

	unload_driver_settings(handle);

	create_log();
}


void release_settings()
{
	if (gLogFilePath != NULL) {
		mutex_destroy(&gLogLock);
		free(gLogFilePath);
	}
}


void usb_audio_trace(uint32 bits, const char* func, const char* fmt, ...)
{
	if ((gTraceMask & bits) == 0)
		return;

	va_list arg_list;
	static const char* prefix = DRIVER_NAME":";
	static char buffer[1024];
	char* buf_ptr = buffer;
	if (gLogFilePath == NULL) {
		strlcpy(buffer, prefix, sizeof(buffer));
		buf_ptr += strlen(prefix);
	}

	if (gAddTimeStamp) {
		bigtime_t time = system_time();
		uint32 msec = time / 1000;
		uint32 sec = msec / 1000;
		sprintf(buf_ptr, "%02" B_PRIu32 ".%02" B_PRIu32 ".%03" B_PRIu32 ":",
				sec / 60, sec % 60, msec % 1000);
		buf_ptr += strlen(buf_ptr);
	}

	if (func	!= NULL) {
		sprintf(buf_ptr, "%s::", func);
		buf_ptr += strlen(buf_ptr);
	}

	va_start(arg_list, fmt);
	vsprintf(buf_ptr, fmt, arg_list);
	va_end(arg_list);

	if (gLogFilePath == NULL) {
		dprintf("%s", buffer);
		return;
	}

	mutex_lock(&gLogLock);
	int fd = open(gLogFilePath, O_WRONLY | O_APPEND);
	if (fd >= 0) {
		write(fd, buffer, strlen(buffer));
		close(fd);
	}
	mutex_unlock(&gLogLock);
}

