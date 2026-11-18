/*
 * Copyright 2008, François Revol, revol@free.fr. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "SystemInfoHandler.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Catalog.h>
#include <Clipboard.h>
#include <Drivers.h>
#include <Handler.h>
#include <Input.h>
#include <List.h>
#include <MediaRoster.h>
#include <Messenger.h>
#include <Roster.h>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "SystemInfoHandler"


SystemInfoHandler::SystemInfoHandler()
	: BHandler("SystemInfoHandler")
{
	fRunningApps = 0;
	fClipboardSize = 0;
	fClipboardTextSize = 0;
	fMediaNodes = 0;
	fMediaConnections = 0;
	fMediaBuffers = 0;

	fThermalFD = _OpenThermalSensor();
	if (fThermalFD < 0) {
		fThermalZoneName = B_TRANSLATE("No thermal sensor");
		return;
	}

	char buffer[256];
	if (ioctl(fThermalFD, B_GET_DEVICE_NAME, buffer, sizeof(buffer)) == B_OK)
		fThermalZoneName = buffer;
	else
		fThermalZoneName = B_TRANSLATE("Unknown thermal sensor");
}


SystemInfoHandler::~SystemInfoHandler()
{
	if (fThermalFD >= 0)
		close(fThermalFD);
}


/*!	Opens the first device that reports a "Current Temperature": ACPI thermal
	zones first, then the other thermal drivers in /dev/power, then the sensors
	in /dev/sensor (as on boards without ACPI).
*/
/*static*/ int
SystemInfoHandler::_OpenThermalSensor()
{
	int fd = open("/dev/power/acpi_thermal/0", O_RDONLY);
	if (fd >= 0)
		return fd;

	DIR* dir = opendir("/dev/power");
	if (dir != NULL) {
		dirent* entry;
		while ((entry = readdir(dir)) != NULL) {
			size_t length = strlen(entry->d_name);
			if (length <= 8 || strcmp(entry->d_name + length - 8, "_thermal") != 0)
				continue;
			BString path("/dev/power/");
			path << entry->d_name << "/0";
			fd = open(path.String(), O_RDONLY);
			if (fd >= 0 && !isnan(_ReadTemperature(fd)))
				break;
			if (fd >= 0)
				close(fd);
			fd = -1;
		}
		closedir(dir);
	}
	if (fd >= 0)
		return fd;

	dir = opendir("/dev/sensor");
	if (dir != NULL) {
		dirent* entry;
		while ((entry = readdir(dir)) != NULL) {
			if (entry->d_name[0] == '.')
				continue;
			BString path("/dev/sensor/");
			path << entry->d_name;
			fd = open(path.String(), O_RDONLY);
			if (fd >= 0 && !isnan(_ReadTemperature(fd)))
				break;
			if (fd >= 0)
				close(fd);
			fd = -1;
		}
		closedir(dir);
	}
	return fd;
}


/*static*/ float
SystemInfoHandler::_ReadTemperature(int fd)
{
	float temperature = std::nanf("");
	char buffer[512];
	// TODO it would be nice to use ioctl to get direct access to the temperature instead,
	// but that provides raw values from the sensors without the "kelvin offset" fixup, making
	// it unusable.
	// The ioctl reply also uses a different format for each of the *_thermal drivers.
	ssize_t length = pread(fd, buffer, sizeof(buffer) - 1, 0);
	if (length <= 0)
		return temperature;
	buffer[length] = '\0';

	const char* currTemp = strstr(buffer, "Current Temperature: ");
	if (currTemp != NULL)
		sscanf(currTemp + strlen("Current Temperature: "), "%f", &temperature);
	return temperature;
}


status_t
SystemInfoHandler::Archive(BMessage* data, bool deep) const
{
	// we don't want ourselves to be archived at all...
	// return BHandler::Archive(data, deep);
	return B_OK;
}


void
SystemInfoHandler::StartWatching()
{
	status_t status;
	fRunningApps = 0;
	fClipboardSize = 0;
	fClipboardTextSize = 0;
	fMediaNodes = 0;
	fMediaConnections = 0;
	fMediaBuffers = 0;

	// running applications count
	BList teamList;
	if (be_roster) {
		be_roster->StartWatching(BMessenger(this),
			B_REQUEST_LAUNCHED | B_REQUEST_QUIT);
		be_roster->GetAppList(&teamList);
		fRunningApps = teamList.CountItems();
		teamList.MakeEmpty();
	}

	// useless clipboard size
	if (be_clipboard) {
		be_clipboard->StartWatching(BMessenger(this));
		_UpdateClipboardData();
	}

	if (BMediaRoster::Roster(&status) && (status >= B_OK)) {
		BMediaRoster::Roster()->StartWatching(BMessenger(this), B_MEDIA_NODE_CREATED);
		BMediaRoster::Roster()->StartWatching(BMessenger(this), B_MEDIA_NODE_DELETED);
		BMediaRoster::Roster()->StartWatching(BMessenger(this), B_MEDIA_CONNECTION_MADE);
		BMediaRoster::Roster()->StartWatching(BMessenger(this), B_MEDIA_CONNECTION_BROKEN);
		BMediaRoster::Roster()->StartWatching(BMessenger(this), B_MEDIA_BUFFER_CREATED);
		BMediaRoster::Roster()->StartWatching(BMessenger(this), B_MEDIA_BUFFER_DELETED);
		// XXX: this won't survive a media_server restart...

		live_node_info nodeInfo; // I just need one
		int32 nodeCount = 1;
		if (BMediaRoster::Roster()->GetLiveNodes(&nodeInfo, &nodeCount)) {
			if (nodeCount > 0)
				fMediaNodes = (uint32)nodeCount;
			// TODO: retry with an array, and use GetNodeInput/Output
			// to find initial connection count
		}
		// TODO: get initial buffer count
	}

	// doesn't work on R5
	watch_input_devices(BMessenger(this), true);
}


void
SystemInfoHandler::StopWatching()
{
	status_t status;
	watch_input_devices(BMessenger(this), false);
	if (BMediaRoster::Roster(&status) && (status >= B_OK)) {
		BMediaRoster::Roster()->StopWatching(BMessenger(this), B_MEDIA_NODE_CREATED);
		BMediaRoster::Roster()->StopWatching(BMessenger(this), B_MEDIA_NODE_DELETED);
		BMediaRoster::Roster()->StopWatching(BMessenger(this), B_MEDIA_CONNECTION_MADE);
		BMediaRoster::Roster()->StopWatching(BMessenger(this), B_MEDIA_CONNECTION_BROKEN);
		BMediaRoster::Roster()->StopWatching(BMessenger(this), B_MEDIA_BUFFER_CREATED);
		BMediaRoster::Roster()->StopWatching(BMessenger(this), B_MEDIA_BUFFER_DELETED);
	}
	if (be_clipboard)
		be_clipboard->StopWatching(BMessenger(this));
	if (be_roster)
		be_roster->StopWatching(BMessenger(this));
}


void
SystemInfoHandler::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case B_SOME_APP_LAUNCHED:
			fRunningApps++;
			// TODO: maybe resync periodically in case we miss one
			break;
		case B_SOME_APP_QUIT:
			fRunningApps--;
			// TODO: maybe resync periodically in case we miss one
			break;
		case B_CLIPBOARD_CHANGED:
			_UpdateClipboardData();
			break;
		case B_MEDIA_NODE_CREATED:
			fMediaNodes++;
			break;
		case B_MEDIA_NODE_DELETED:
			fMediaNodes--;
			break;
		case B_MEDIA_CONNECTION_MADE:
			fMediaConnections++;
			break;
		case B_MEDIA_CONNECTION_BROKEN:
			fMediaConnections--;
			break;
		case B_MEDIA_BUFFER_CREATED:
			fMediaBuffers++;
			break;
		case B_MEDIA_BUFFER_DELETED:
			fMediaBuffers--;
			break;
		default:
			message->PrintToStream();
			BHandler::MessageReceived(message);
	}
}


uint32
SystemInfoHandler::RunningApps() const
{
	return fRunningApps;
}


uint32
SystemInfoHandler::ClipboardSize() const
{
	return fClipboardSize;
}


uint32
SystemInfoHandler::ClipboardTextSize() const
{
	return fClipboardTextSize;
}


uint32
SystemInfoHandler::MediaNodes() const
{
	return fMediaNodes;
}


uint32
SystemInfoHandler::MediaConnections() const
{
	return fMediaConnections;
}


uint32
SystemInfoHandler::MediaBuffers() const
{
	return fMediaBuffers;
}


float
SystemInfoHandler::Temperature() const
{
	if (fThermalFD < 0)
		return std::nanf("");
	return _ReadTemperature(fThermalFD);
}


const BString&
SystemInfoHandler::ThermalZone() const
{
	return fThermalZoneName;
}


void
SystemInfoHandler::_UpdateClipboardData()
{
	fClipboardSize = 0;
	fClipboardTextSize = 0;

	if (be_clipboard == NULL || !be_clipboard->Lock())
		return;

	BMessage* data = be_clipboard->Data();
	if (data) {
		ssize_t size = data->FlattenedSize();
		fClipboardSize = size < 0 ? 0 : (uint32)size;

		const void* text;
		ssize_t textSize;
		if (data->FindData("text/plain", B_MIME_TYPE, &text, &textSize) >= B_OK)
			fClipboardTextSize = textSize;
	}

	be_clipboard->Unlock();
}

