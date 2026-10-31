/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "HIDProfile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <Autolock.h>
#include <File.h>
#include <Directory.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Locker.h>
#include <ObjectList.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/L2CAP/btL2CAP.h>
#include <bluetooth/l2cap.h>
#include <bluetooth_hid.h>

#include "Debug.h"
#include "HIDDescriptor.h"


// Bluetooth HID profile 1.1: the channels and the transaction headers
#define HID_PSM_CONTROL			0x0011
#define HID_PSM_INTERRUPT		0x0013
#define HID_DATA_INPUT			0xa1

namespace {


struct Connection {
	bdaddr_t	address;
	uint8*		descriptor;
	size_t		descriptorLength;
	int			control;		// -1: we connect the channels
	int			interrupt;
	HIDDescriptor::BatteryReport battery;
};


struct BatteryLevel {
	bdaddr_t	address;
	int32		percent;
};


// the channels a device opened to us, waiting for each other
struct IncomingChannels {
	bdaddr_t	address;
	int			control;
	int			interrupt;
};


BLocker sLock("hid profile");
BObjectList<bdaddr_t, true> sConnected;
BObjectList<IncomingChannels, true> sIncoming;
BObjectList<BatteryLevel, true> sBatteryLevels;


bool
IsConnected(const bdaddr_t& address)
{
	for (int32 i = 0; i < sConnected.CountItems(); i++) {
		if (bdaddrUtils::Compare(*sConnected.ItemAt(i), address))
			return true;
	}
	return false;
}


void
SetConnected(const bdaddr_t& address, bool connected)
{
	BAutolock locker(sLock);
	for (int32 i = 0; i < sConnected.CountItems(); i++) {
		if (bdaddrUtils::Compare(*sConnected.ItemAt(i), address)) {
			if (!connected)
				delete sConnected.RemoveItemAt(i);
			return;
		}
	}
	if (connected)
		sConnected.AddItem(new bdaddr_t(address));
}


void
SetBatteryLevel(const bdaddr_t& address, int32 percent)
{
	BAutolock locker(sLock);
	for (int32 i = 0; i < sBatteryLevels.CountItems(); i++) {
		BatteryLevel* level = sBatteryLevels.ItemAt(i);
		if (bdaddrUtils::Compare(level->address, address)) {
			if (percent < 0)
				delete sBatteryLevels.RemoveItemAt(i);
			else
				level->percent = percent;
			return;
		}
	}
	if (percent >= 0) {
		BatteryLevel* level = new(std::nothrow) BatteryLevel;
		if (level != NULL) {
			level->address = address;
			level->percent = percent;
			sBatteryLevels.AddItem(level);
		}
	}
}


void
ReadBatteryLevel(const Connection* connection, const uint8* report,
	size_t length)
{
	const HIDDescriptor::BatteryReport& battery = connection->battery;
	if (battery.id < 0 || battery.maximum <= 0 || battery.bitSize == 0
		|| battery.bitSize > 16)
		return;

	// with report IDs, the first byte is the ID
	size_t start = 0;
	if (battery.id != 0) {
		if (length < 1 || report[0] != battery.id)
			return;
		start = 1;
	}

	uint32 firstBit = battery.bitOffset;
	if ((firstBit + battery.bitSize + 7) / 8 + start > length)
		return;

	uint32 value = 0;
	for (uint32 bit = 0; bit < battery.bitSize; bit++) {
		uint32 position = firstBit + bit;
		if ((report[start + position / 8] >> (position % 8)) & 1)
			value |= 1 << bit;
	}

	int32 percent = value * 100 / battery.maximum;
	SetBatteryLevel(connection->address, percent > 100 ? 100 : percent);
}


/*!	The report descriptors of the devices connected once are kept in the
	settings, so that they can connect again even when their SDP records
	cannot be read.
*/
status_t
KnownDescriptorPath(const bdaddr_t& address, BPath& path)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status != B_OK)
		return status;

	BString name = bdaddrUtils::ToString(address);
	name.ReplaceAll(":", "");
	path.Append("system/bluetooth/hid");
	create_directory(path.Path(), 0755);
	return path.Append(name.String());
}


void
SaveKnownDescriptor(const bdaddr_t& address, const uint8* descriptor,
	size_t length)
{
	BPath path;
	if (KnownDescriptorPath(address, path) != B_OK)
		return;

	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() == B_OK)
		file.Write(descriptor, length);
}


int
ConnectChannel(const bdaddr_t& address, uint16 psm)
{
	int fd = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BLUETOOTH_PROTO_L2CAP);
	if (fd < 0)
		return -1;

	sockaddr_l2cap socketAddress;
	memset(&socketAddress, 0, sizeof(socketAddress));
	socketAddress.l2cap_len = sizeof(socketAddress);
	socketAddress.l2cap_family = AF_BLUETOOTH;
	socketAddress.l2cap_psm = psm;
	socketAddress.l2cap_bdaddr = address;

	// a device that does not answer must not block the thread forever
	timeval timeout = { 10, 0 };
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

	TRACE_BT("HID: connecting PSM %#x\n", psm);
	if (connect(fd, (sockaddr*)&socketAddress, sizeof(socketAddress)) < 0) {
		TRACE_BT("HID: cannot connect PSM %#x: %s\n", psm, strerror(errno));
		close(fd);
		return -1;
	}
	TRACE_BT("HID: PSM %#x connected\n", psm);

	return fd;
}


status_t
ConnectionThread(void* data)
{
	Connection* connection = (Connection*)data;
	BString address = bdaddrUtils::ToString(connection->address);

	int control = connection->control;
	int interrupt = connection->interrupt;
	if (control < 0) {
		// the HID profile wants the control channel first
		control = ConnectChannel(connection->address, HID_PSM_CONTROL);
		if (control >= 0)
			interrupt = ConnectChannel(connection->address, HID_PSM_INTERRUPT);
	}

	int driver = -1;
	if (interrupt >= 0) {
		driver = open(BLUETOOTH_HID_CONTROL_PATH, O_RDWR);
		if (driver < 0) {
			TRACE_BT("HID: cannot open %s: %s\n", BLUETOOTH_HID_CONTROL_PATH,
				strerror(errno));
		}
	}

	if (driver >= 0) {
		bluetooth_hid_create_info* info = (bluetooth_hid_create_info*)
			calloc(1, sizeof(bluetooth_hid_create_info));
		if (info != NULL) {
			info->descriptor_length = connection->descriptorLength;
			memcpy(info->descriptor, connection->descriptor,
				connection->descriptorLength);
		}
		if (info == NULL || ioctl(driver, BLUETOOTH_HID_CREATE, info,
				sizeof(bluetooth_hid_create_info)) != 0) {
			TRACE_BT("HID: the driver refused %s: %s\n", address.String(),
				strerror(errno));
			close(driver);
			driver = -1;
		}
		free(info);
	}

	if (driver >= 0) {
		TRACE_BT("HID: %s connected\n", address.String());

		// L2CAP does not wake up a reader when the device goes away, it only
		// marks the socket: look at it now and then
		timeval timeout = { 2, 0 };
		setsockopt(interrupt, SOL_SOCKET, SO_RCVTIMEO, &timeout,
			sizeof(timeout));

		uint8 buffer[BLUETOOTH_HID_MAX_REPORT + 1];
		while (true) {
			ssize_t length = recv(interrupt, buffer, sizeof(buffer), 0);
			if (length < 0 && (errno == B_TIMED_OUT || errno == EAGAIN
					|| errno == B_WOULD_BLOCK)) {
				int error = 0;
				socklen_t errorLength = sizeof(error);
				if (getsockopt(interrupt, SOL_SOCKET, SO_ERROR, &error,
						&errorLength) == 0 && error == 0) {
					continue;
				}
				break;
			}
			if (length <= 0)
				break;
			if (length < 2 || buffer[0] != HID_DATA_INPUT)
				continue;
			ReadBatteryLevel(connection, buffer + 1, length - 1);
			write(driver, buffer + 1, length - 1);
		}

		TRACE_BT("HID: %s disconnected\n", address.String());
		SetBatteryLevel(connection->address, -1);
		close(driver);
	}

	if (interrupt >= 0)
		close(interrupt);
	if (control >= 0)
		close(control);

	SetConnected(connection->address, false);
	free(connection->descriptor);
	delete connection;
	return B_OK;
}


status_t
LoadKnownDescriptor(const bdaddr_t& address, uint8* descriptor,
	size_t& length)
{
	BPath path;
	status_t status = KnownDescriptorPath(address, path);
	if (status != B_OK)
		return status;

	BFile file(path.Path(), B_READ_ONLY);
	off_t size;
	if (file.InitCheck() != B_OK || file.GetSize(&size) != B_OK || size <= 0
		|| size > BLUETOOTH_HID_MAX_DESCRIPTOR)
		return B_ENTRY_NOT_FOUND;

	if (file.Read(descriptor, size) != size)
		return B_IO_ERROR;

	length = size;
	return B_OK;
}


/*!	Starts the thread that serves a device. With \a control and \a interrupt
	at -1 the thread connects the channels itself.
*/
status_t
StartConnection(const bdaddr_t& address, const uint8* descriptor,
	size_t descriptorLength, int control = -1, int interrupt = -1)
{
	BAutolock locker(sLock);
	if (IsConnected(address))
		return B_BUSY;

	Connection* connection = new(std::nothrow) Connection;
	if (connection == NULL)
		return B_NO_MEMORY;
	connection->address = address;
	connection->control = control;
	connection->interrupt = interrupt;
	connection->descriptorLength = descriptorLength;
	connection->descriptor = (uint8*)malloc(descriptorLength);
	if (connection->descriptor == NULL) {
		delete connection;
		return B_NO_MEMORY;
	}
	memcpy(connection->descriptor, descriptor, descriptorLength);
	connection->battery = HIDDescriptor::FindBatteryReport(descriptor,
		descriptorLength);

	thread_id thread = spawn_thread(ConnectionThread, "bluetooth hid",
		B_URGENT_DISPLAY_PRIORITY, connection);
	if (thread < 0) {
		free(connection->descriptor);
		delete connection;
		return thread;
	}

	sConnected.AddItem(new bdaddr_t(address));
	return resume_thread(thread);
}


/*!	A device connected one of its channels to us: once both are there, the
	device is served like one we connected, with its known descriptor.
*/
void
IncomingChannel(const bdaddr_t& address, uint16 psm, int fd)
{
	BString name = bdaddrUtils::ToString(address);
	TRACE_BT("HID: %s opened PSM %#x\n", name.String(), psm);

	BAutolock locker(sLock);
	IncomingChannels* channels = NULL;
	for (int32 i = 0; i < sIncoming.CountItems(); i++) {
		if (bdaddrUtils::Compare(sIncoming.ItemAt(i)->address, address))
			channels = sIncoming.ItemAt(i);
	}
	if (channels == NULL) {
		channels = new(std::nothrow) IncomingChannels;
		if (channels == NULL) {
			close(fd);
			return;
		}
		channels->address = address;
		channels->control = -1;
		channels->interrupt = -1;
		sIncoming.AddItem(channels);
	}

	int& slot = psm == HID_PSM_CONTROL ? channels->control : channels->interrupt;
	if (slot >= 0)
		close(slot);
	slot = fd;

	if (channels->control < 0 || channels->interrupt < 0)
		return;

	int control = channels->control;
	int interrupt = channels->interrupt;
	sIncoming.RemoveItem(channels, false);
	delete channels;

	// a device switched off and on again: its old session notices it is gone
	// within a few seconds
	locker.Unlock();
	for (int32 i = 0; i < 30; i++) {
		sLock.Lock();
		bool connected = IsConnected(address);
		sLock.Unlock();
		if (!connected)
			break;
		snooze(100000);
	}

	uint8 descriptor[BLUETOOTH_HID_MAX_DESCRIPTOR];
	size_t length;
	if (LoadKnownDescriptor(address, descriptor, length) != B_OK
		|| StartConnection(address, descriptor, length, control, interrupt)
			!= B_OK) {
		TRACE_BT("HID: %s is not a known HID device\n", name.String());
		close(control);
		close(interrupt);
	}
}


status_t
ListenThread(void* data)
{
	uint16 psm = (uint16)(addr_t)data;

	int server = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BLUETOOTH_PROTO_L2CAP);
	if (server < 0)
		return errno;

	sockaddr_l2cap address;
	memset(&address, 0, sizeof(address));
	address.l2cap_len = sizeof(address);
	address.l2cap_family = AF_BLUETOOTH;
	address.l2cap_psm = psm;
	address.l2cap_bdaddr = BDADDR_ANY;
	if (bind(server, (sockaddr*)&address, sizeof(address)) < 0
		|| listen(server, 4) < 0) {
		TRACE_BT("HID: cannot listen on PSM %#x: %s\n", psm, strerror(errno));
		close(server);
		return errno;
	}

	while (true) {
		sockaddr_l2cap peer;
		socklen_t peerLength = sizeof(peer);
		memset(&peer, 0, sizeof(peer));
		int fd = accept(server, (sockaddr*)&peer, &peerLength);
		if (fd < 0) {
			if (errno == EINTR)
				continue;
			TRACE_BT("HID: accept on PSM %#x: %s\n", psm, strerror(errno));
			break;
		}
		IncomingChannel(peer.l2cap_bdaddr, psm, fd);
	}

	close(server);
	return B_OK;
}


}	// namespace


status_t
HIDProfile::Connect(const bdaddr_t& address, const BMessage& serviceRecord)
{
	const uint8* descriptor;
	size_t descriptorLength;
	status_t status = HIDDescriptor::FindReportDescriptor(serviceRecord,
		descriptor, descriptorLength);
	if (status != B_OK) {
		TRACE_BT("HID: no report descriptor for %s\n",
			bdaddrUtils::ToString(address).String());
		return status;
	}
	if (descriptorLength > BLUETOOTH_HID_MAX_DESCRIPTOR)
		return B_BAD_DATA;

	SaveKnownDescriptor(address, descriptor, descriptorLength);
	TRACE_BT("HID: report descriptor of %" B_PRIuSIZE " bytes, connecting\n",
		descriptorLength);
	status = StartConnection(address, descriptor, descriptorLength);
	return status == B_BUSY ? B_OK : status;
}


status_t
HIDProfile::ConnectKnown(const bdaddr_t& address)
{
	uint8 descriptor[BLUETOOTH_HID_MAX_DESCRIPTOR];
	size_t length;
	status_t status = LoadKnownDescriptor(address, descriptor, length);
	if (status != B_OK)
		return status;

	status = StartConnection(address, descriptor, length);
	return status == B_BUSY ? B_OK : status;
}


status_t
HIDProfile::Listen()
{
	// devices paired before connect their channels to us when they come
	// back on
	const uint16 psms[] = { HID_PSM_CONTROL, HID_PSM_INTERRUPT };
	for (size_t i = 0; i < B_COUNT_OF(psms); i++) {
		thread_id thread = spawn_thread(ListenThread, "bluetooth hid listen",
			B_NORMAL_PRIORITY, (void*)(addr_t)psms[i]);
		if (thread < 0)
			return thread;
		resume_thread(thread);
	}
	return B_OK;
}


int32
HIDProfile::BatteryLevel(const bdaddr_t& address)
{
	BAutolock locker(sLock);
	for (int32 i = 0; i < sBatteryLevels.CountItems(); i++) {
		::BatteryLevel* level = sBatteryLevels.ItemAt(i);
		if (bdaddrUtils::Compare(level->address, address))
			return level->percent;
	}
	return -1;
}
