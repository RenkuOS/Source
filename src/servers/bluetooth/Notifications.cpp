/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "Notifications.h"

#include <Catalog.h>
#include <Notification.h>
#include <String.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetoothserver_p.h>

#include "LocalDeviceImpl.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Notifications"


namespace {


BString
DeviceName(const ServerRemoteDevice* device)
{
	if (!device->friendly_name.IsEmpty())
		return device->friendly_name;

	BString name(B_TRANSLATE("Bluetooth device %address%"));
	name.ReplaceFirst("%address%",
		bdaddrUtils::ToString(device->bdaddr).String());
	return name;
}


void
Send(const ServerRemoteDevice* device, notification_type type,
	const char* title, const char* content)
{
	BNotification notification(type);
	notification.SetGroup(B_TRANSLATE("Bluetooth"));
	// one notification per device: the newest replaces the older
	notification.SetMessageID(bdaddrUtils::ToString(device->bdaddr));
	notification.SetTitle(title);

	BString text(content);
	text.ReplaceAll("%device%", DeviceName(device).String());
	notification.SetContent(text);

	notification.SetOnClickApp(BLUETOOTH_APP_SIGNATURE);
	notification.Send();
}


}	// namespace


void
Notifications::Connected(const ServerRemoteDevice* device)
{
	if (device == NULL)
		return;
	Send(device, B_INFORMATION_NOTIFICATION, B_TRANSLATE("Connected"),
		B_TRANSLATE("%device% is connected."));
}


void
Notifications::Disconnected(const ServerRemoteDevice* device)
{
	if (device == NULL)
		return;
	Send(device, B_INFORMATION_NOTIFICATION, B_TRANSLATE("Disconnected"),
		B_TRANSLATE("%device% is disconnected."));
}


void
Notifications::Paired(const ServerRemoteDevice* device, bool success)
{
	if (device == NULL)
		return;
	if (success) {
		Send(device, B_INFORMATION_NOTIFICATION, B_TRANSLATE("Paired"),
			B_TRANSLATE("%device% is paired and connects when it is switched "
				"on."));
	} else {
		Send(device, B_ERROR_NOTIFICATION, B_TRANSLATE("Pairing failed"),
			B_TRANSLATE("%device% could not be paired. Put it in pairing mode "
				"and try again."));
	}
}
