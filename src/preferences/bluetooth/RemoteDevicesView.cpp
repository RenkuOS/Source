/*
 * Copyright 2008-2009, Oliver Ruiz Dorantes, <oliver.ruiz.dorantes@gmail.com>
 * Copyright 2021, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 * 		Fredrik Modéen <fredrik_at_modeen.se>
 */

#include <stdio.h>

#include <Alert.h>
#include <Catalog.h>
#include <Messenger.h>

#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <Path.h>

#include <LayoutBuilder.h>
#include <SpaceLayoutItem.h>

#include <PincodeWindow.h>
#include <bluetooth/RemoteDevice.h>

#include "BluetoothWindow.h"
#include "defs.h"
#include "InquiryPanel.h"
#include "RemoteDevicesView.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Remote devices"

static const uint32 kMsgAddDevices = 'ddDv';
static const uint32 kMsgRemoveDevice = 'rmDv';
static const uint32 kMsgPairDevice = 'trDv';
static const uint32 kMsgDisconnectDevice = 'dsDv';
static const uint32 kMsgConnComplete = 'cmDv';
static const uint32 kMsgCancelConn = 'cnDv';
static const uint32 kMsgConnectToggle = 'ctDv';
//static const uint32 kMsgBlockDevice = 'blDv';
//static const uint32 kMsgRefreshDevices = 'rfDv';


class DeviceListView : public BListView
{
public:
	DeviceListView(const char* name, list_view_type type)
		:
		BListView(name, type)
	{
	}


	void
	SelectionChanged()
	{
		RemoteDevicesView* remoteDevicesView = static_cast<RemoteDevicesView*>(Parent()->Parent());
		remoteDevicesView->DeviceSelected();
	}
};


RemoteDevicesView::RemoteDevicesView(const char* name, uint32 flags)
 :	BView(name, flags)
{
	// the catalog misses the keys with an ellipsis: add it afterwards
	BString addLabel(B_TRANSLATE("Add device"));
	addLabel << B_UTF8_ELLIPSIS;
	addButton = new BButton("add", addLabel.String(),
		new BMessage(kMsgAddDevices));

	removeButton = new BButton("remove", B_TRANSLATE("Remove"),
		new BMessage(kMsgRemoveDevice));
	removeButton->SetEnabled(false);

	fConnectButton = new BButton("connect", B_TRANSLATE("Disconnect"),
		new BMessage(kMsgConnectToggle));
	fConnectButton->SetEnabled(false);

	/*
		blockButton = new BButton("block", B_TRANSLATE("As blocked"),
			new BMessage(kMsgBlockDevice));

		//TODO:Here use GetFriendlyName(true)
		availButton = new BButton("check", B_TRANSLATE("Refresh" B_UTF8_ELLIPSIS),
			new BMessage(kMsgRefreshDevices));
	*/
	// Set up device list
	fDeviceList = new DeviceListView("DeviceList", B_SINGLE_SELECTION_LIST);

	fScrollView = new BScrollView("ScrollView", fDeviceList, 0, false, true);

	BLayoutBuilder::Group<>(this, B_HORIZONTAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(fScrollView)
		.AddGroup(B_VERTICAL)
			.Add(addButton)
			.Add(fConnectButton)
			.Add(removeButton)
			.AddGlue()
		.End()
	.End();
}


RemoteDevicesView::~RemoteDevicesView(void)
{
}


void
RemoteDevicesView::AttachedToWindow(void)
{
	fDeviceList->SetTarget(this);
	addButton->SetTarget(this);
	removeButton->SetTarget(this);
	fConnectButton->SetTarget(this);
//	blockButton->SetTarget(this);
//	availButton->SetTarget(this);

	LoadRemoteDevices();
	LoadSettings();
	fDeviceList->Select(0);
	fConnectingDeviceItem = NULL;
}


void
RemoteDevicesView::AddRemoteDevice(BMessage* device)
{
	RemoteDevice* rd = RemoteDevice::ParseRemoteDevice(ActiveLocalDevice, device);

	DeviceListItem* deviceItem = new DeviceListItem(rd);
	deviceItem->SetConnectionState(rd->GetConnectionState());

	BMessage message(kMsgAddToRemoteList);
	message.AddPointer("device", deviceItem);
	MessageReceived(&message);
}


void
RemoteDevicesView::LoadRemoteDevices()
{
	BObjectList<RemoteDevice> rdList = RemoteDevice::GetRemoteDevices(ActiveLocalDevice);

	if (rdList.IsEmpty())
		return;

	BMessage device;
	for (int32 i = 0; i < rdList.CountItems(); i++) {
		RemoteDevice* remote = rdList.ItemAt(i);

		DeviceListItem* deviceItem = new DeviceListItem(remote);
		deviceItem->SetConnectionState(remote->GetConnectionState());

		BMessage message(kMsgAddToRemoteList);
		message.AddPointer("device", deviceItem);
		MessageReceived(&message);
	}
}


void
RemoteDevicesView::PairDevice(DeviceListItem* device)
{
	if (device == NULL)
		return;

	RemoteDevice* remote = dynamic_cast<RemoteDevice*>(device->Device());
	if (remote == NULL)
		return;

	fConnectingDeviceItem = device;
	fConnectingDeviceItem->SetConnectionState(RemoteDevice::CONNECTING);
	DeviceSelected();

	remote->Connect();
}


void
RemoteDevicesView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgAddDevices:
		{
			InquiryPanel* inquiryPanel= new InquiryPanel(
				BRect(100, 100, 450, 450), ActiveLocalDevice);
			inquiryPanel->Show();
			break;
		}

		case kMsgRemoveDevice:
		{
			RemoteDevice* remote = NULL;
			DeviceListItem* device = static_cast<DeviceListItem*>(fDeviceList
				->ItemAt(fDeviceList->CurrentSelection(0)));

			if (device != NULL)
				remote = dynamic_cast<RemoteDevice*>(device->Device());

			fDeviceList->RemoveItem(fDeviceList->CurrentSelection(0));

			if (remote == NULL)
				break;

			remote->Disconnect(true);

			break;
		}

		case kMsgAddToRemoteList:
		{
			DeviceListItem* device = NULL;
			message->FindPointer("device", (void**)&device);
			bool isDuplicate = false;

			// check the list for duplicates
			for (int32 i = 0; i < fDeviceList->CountItems(); i++) {
				DeviceListItem* existingDevice
					= static_cast<DeviceListItem*>(fDeviceList->ItemAt(i));

				if (DeviceListItem::Compare(&existingDevice, &device)) {
					isDuplicate = true;
					break;
				}
			}

			DeviceListItem* listed = device;
			for (int32 i = 0; isDuplicate && i < fDeviceList->CountItems(); i++) {
				DeviceListItem* existingDevice
					= static_cast<DeviceListItem*>(fDeviceList->ItemAt(i));
				if (DeviceListItem::Compare(&existingDevice, &device))
					listed = existingDevice;
			}

			if (!isDuplicate) {
				fDeviceList->AddItem((BListItem*)device);
				fDeviceList->Invalidate();
			} else {
				delete device;
			}

			// the inquiry panel asks to pair the device it adds
			if (message->GetBool("pair", false)) {
				fDeviceList->Select(fDeviceList->IndexOf(listed));
				PairDevice(listed);
			}

			break;
		}

		case kMsgPairDevice:
		{
			PairDevice(static_cast<DeviceListItem*>(fDeviceList
				->ItemAt(fDeviceList->CurrentSelection(0))));
			break;
		}

		case BT_MSG_CONN_COMPLETED:
		{
			uint8 status;
			bdaddr_t* bdaddr;
			ssize_t size;

			message->FindUInt8("status", &status);

			// Remote Connection
			if (fConnectingDeviceItem == NULL &&
				message->FindData("bdaddr", B_ANY_TYPE, (const void**)&bdaddr, &size) == B_OK) {

				for (int32 i = 0; i < fDeviceList->CountItems(); i++) {
					DeviceListItem* item = static_cast<DeviceListItem*>(fDeviceList->ItemAt(i));

					if (bdaddrUtils::Compare(item->Device()->GetBluetoothAddress(), *bdaddr))
						fConnectingDeviceItem = item;
				}
			}

			if (fConnectingDeviceItem == NULL)
				break;

			if (status == BT_OK) {
				fConnectingDeviceItem->SetConnectionState(RemoteDevice::CONNECTED);

				// a device paired right after the inquiry has no name yet;
				// with the connection up asking for it is quick
				RemoteDevice* remote = fConnectingDeviceItem->Device();
				if (remote != NULL && remote->GetCachedFriendlyName().IsEmpty()) {
					fConnectingDeviceItem->SetDevice(remote);
					fDeviceList->InvalidateItem(
						fDeviceList->IndexOf(fConnectingDeviceItem));
				}
			} else
				fConnectingDeviceItem->SetConnectionState(RemoteDevice::DISCONNECTED);

			DeviceSelected();

			fConnectingDeviceItem = NULL;
			break;
		}

		case kMsgDisconnectDevice:
		{
			DeviceListItem* device = static_cast<DeviceListItem*>(fDeviceList
				->ItemAt(fDeviceList->CurrentSelection(0)));
			if (device == NULL)
				break;

			RemoteDevice* remote = dynamic_cast<RemoteDevice*>(device->Device());
			if (remote == NULL)
				break;
			fConnectingDeviceItem = device;
			remote->Disconnect(false);

			break;
		}

		case BT_MSG_CONN_FAILED:
		case BT_MSG_DISCONN_COMPLETED:
		{
			bdaddr_t* bdaddr;
			ssize_t size;

			// Remote Disconnection
			if (fConnectingDeviceItem == NULL &&
				message->FindData("bdaddr", B_ANY_TYPE, (const void**)&bdaddr, &size) == B_OK) {

				for (int32 i = 0; i < fDeviceList->CountItems(); i++) {
					DeviceListItem* item = static_cast<DeviceListItem*>(fDeviceList->ItemAt(i));

					if (bdaddrUtils::Compare(item->Device()->GetBluetoothAddress(), *bdaddr))
						fConnectingDeviceItem = item;
				}
			}

			if (fConnectingDeviceItem == NULL)
				break;

			fConnectingDeviceItem->SetConnectionState(RemoteDevice::DISCONNECTED);
			fConnectingDeviceItem = NULL;

			DeviceSelected();
			break;
		}

		case kMsgCancelConn:
		{
			if (fConnectingDeviceItem != NULL)
				fConnectingDeviceItem->Device()->CancelConnection();
			break;
		}

		case kMsgConnectToggle:
		{
			DeviceListItem* device = static_cast<DeviceListItem*>(fDeviceList
				->ItemAt(fDeviceList->CurrentSelection(0)));
			if (device == NULL)
				break;

			BMessage action;
			switch (device->GetConnectionState()) {
				case RemoteDevice::CONNECTED:
					action.what = kMsgDisconnectDevice;
					break;
				case RemoteDevice::CONNECTING:
					action.what = kMsgCancelConn;
					break;
				default:
					action.what = kMsgPairDevice;
					break;
			}
			MessageReceived(&action);
			break;
		}

		default:
			BView::MessageReceived(message);
			break;
	}
}


void
RemoteDevicesView::DeviceSelected()
{
	DeviceListItem* device = static_cast<DeviceListItem*>(fDeviceList
				->ItemAt(fDeviceList->CurrentSelection(0)));

	removeButton->SetEnabled(device != NULL);
	fConnectButton->SetEnabled(device != NULL);
	if (device == NULL)
		return;

	switch (device->GetConnectionState()) {
		case RemoteDevice::CONNECTING:
			fConnectButton->SetLabel(B_TRANSLATE("Cancel"));
			break;
		case RemoteDevice::CONNECTED:
			fConnectButton->SetLabel(B_TRANSLATE("Disconnect"));
			break;
		case RemoteDevice::DISCONNECTED:
			fConnectButton->SetLabel(B_TRANSLATE("Connect"));
			break;
	}

	// the states are shown in the list too
	fDeviceList->Invalidate();
}


void
RemoteDevicesView::LoadSettings(void)
{

}


bool
RemoteDevicesView::IsDefaultable(void)
{
	return true;
}

