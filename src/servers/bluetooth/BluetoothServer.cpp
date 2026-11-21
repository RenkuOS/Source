/*
 * Copyright 2007-2009 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * Copyright 2008 Mika Lindqvist, monni1995_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>

#include <Autolock.h>
#include <Entry.h>
#include <Deskbar.h>
#include <Directory.h>
#include <Message.h>
#include <MessageRunner.h>
#include <Path.h>
#include <Roster.h>
#include <String.h>
#include <Window.h>

#include <TypeConstants.h>
#include <syslog.h>

#include <bluetoothserver_p.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/L2CAP/btL2CAP.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/bdaddrUtils.h>

#include "BluetoothServer.h"


static const uint32 kMsgSetConnectable = 'stCn';
static const uint32 kMsgAllowRoleSwitch = 'alRs';
#include "Debug.h"
#include "DeskbarReplicant.h"
#include "LocalDeviceImpl.h"
#include "SDPServer.h"
#include "SDPClient.h"


status_t
DispatchEvent(struct hci_event_header* header, int32 code, size_t size)
{
	// we only handle events
	if (GET_PORTCODE_TYPE(code)!= BT_EVENT) {
		TRACE_BT("BluetoothServer: Wrong type frame code\n");
		return B_OK;
	}

	BluetoothServer* server = (BluetoothServer*)be_app;
	BAutolock locker(server->DevicesLock());

	// fetch the LocalDevice who belongs this event
	LocalDeviceImpl* lDeviceImplementation
		= server->LocateLocalDeviceImpl(GET_PORTCODE_HID(code));

	if (lDeviceImplementation == NULL) {
		TRACE_BT("BluetoothServer: LocalDevice could not be fetched\n");
		return B_OK;
	}

	lDeviceImplementation->HandleEvent(header);

	return B_OK;
}


BluetoothServer::BluetoothServer()
	:
	BApplication(BLUETOOTH_SIGNATURE),
	fDevicesLock("bluetooth devices")
{
	fDeviceManager = new DeviceManager();
	fLocalDevicesList.MakeEmpty();
	fWatchersList.MakeEmpty();

	fEventListener2 = new BluetoothPortListener(BT_USERLAND_PORT_NAME,
		(BluetoothPortListener::port_listener_func)&DispatchEvent);
	fSDPServer = new SDPServer();
}


bool BluetoothServer::QuitRequested(void)
{
	// Close the links ourselves: the devices learn at once that we are
	// gone, and the controller is left without any.
	fDevicesLock.Lock();
	for (int32 i = 0; i < fLocalDevicesList.CountItems(); i++)
		fLocalDevicesList.ItemAt(i)->DisconnectAll();
	fDevicesLock.Unlock();

	// the disconnection events come through the event thread
	for (int32 wait = 0; wait < 20; wait++) {
		BAutolock locker(fDevicesLock);
		bool links = false;
		for (int32 i = 0; i < fLocalDevicesList.CountItems(); i++)
			links |= fLocalDevicesList.ItemAt(i)->HasLinks();
		if (!links)
			break;
		locker.Unlock();
		snooze(50000);
	}

	fDevicesLock.Lock();
	while (!fLocalDevicesList.IsEmpty())
		_RemoveLocalDevice(fLocalDevicesList.ItemAt(0));
	fDevicesLock.Unlock();

	_RemoveDeskbarIcon();

	fSDPServer->Stop();
	delete fSDPServer;

	delete fEventListener2;
	TRACE_BT("Shutting down bluetooth_server.\n");

	return BApplication::QuitRequested();
}


void BluetoothServer::ArgvReceived(int32 argc, char **argv)
{
	if (argc > 1) {
		if (strcmp(argv[1], "--finish") == 0)
			PostMessage(B_QUIT_REQUESTED);
	}
}


void BluetoothServer::ReadyToRun(void)
{
	fDeviceManager->StartMonitoringDevice("bluetooth/h2");
	fDeviceManager->StartMonitoringDevice("bluetooth/h3");
	fDeviceManager->StartMonitoringDevice("bluetooth/h4");
	fDeviceManager->StartMonitoringDevice("bluetooth/h5");

	if (fEventListener2->Launch() != B_OK)
		TRACE_BT("General: Bluetooth event listener failed\n");
	else
		TRACE_BT("General: Bluetooth event listener Ready\n");

	_InstallDeskbarIcon();

	status_t status = fSDPServer->Start();
	if (status != B_OK)
		TRACE_BT("BluetoothServer: Failed launching the SDP server thread\n");
}


void BluetoothServer::AppActivated(bool act)
{
	printf("Activated %d\n",act);
}


void BluetoothServer::MessageReceived(BMessage* message)
{
	BMessage reply;
	status_t status = B_WOULD_BLOCK; // mark somehow to do not reply anything

	BAutolock locker(fDevicesLock);

	switch (message->what) {
		case BT_MSG_ADD_DEVICE:
		{
			BString path;
			if (message->FindString("name", &path) == B_OK)
				_AddLocalDevice(path.String());
		}
		break;

		case BT_MSG_REMOVE_DEVICE:
		{
			// by the controller ID when the device failed, by its path when
			// it left the bus
			LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
			BString path;
			if (lDeviceImpl == NULL && message->FindString("name", &path) == B_OK)
				lDeviceImpl = _LocalDeviceByPath(path.String());
			if (lDeviceImpl != NULL)
				_RemoveLocalDevice(lDeviceImpl);
		}
		break;

		case kMsgSetConnectable:
		case kMsgAllowRoleSwitch:
		{
			LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
			if (lDeviceImpl == NULL)
				return;
			if (message->what == kMsgSetConnectable)
				lDeviceImpl->SetConnectable();
			else
				lDeviceImpl->AllowRoleSwitch();
			return;
		}

		case BT_MSG_COUNT_LOCAL_DEVICES:
			status = HandleLocalDevicesCount(message, &reply);
			break;

		case BT_MSG_ACQUIRE_LOCAL_DEVICE:
			status = HandleAcquireLocalDevice(message, &reply);
			break;

		case BT_MSG_HANDLE_SIMPLE_REQUEST:
			status = HandleSimpleRequest(message, &reply);
			break;

		case BT_MSG_GET_PROPERTY:
			status = HandleGetProperty(message, &reply);
			break;

		// Handle if the bluetooth preferences is running?
		case B_SOME_APP_LAUNCHED:
		{
			const char* signature;

			if (message->FindString("be:signature", &signature) == B_OK) {
				printf("input_server : %s\n", signature);
				if (strcmp(signature, "application/x-vnd.Be-TSKB") == 0) {

				}
			}
		}
		return;

		case BT_REQ_CREATE_CONN:
		{
			LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
			if (lDeviceImpl == NULL)
				break;

			lDeviceImpl->CreateConnection(message);
			break;
		}

		case BT_REQ_CANCEL_CONN:
		{
			LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
			if (lDeviceImpl == NULL)
				break;

			lDeviceImpl->CancelConnection(message);
		}
		break;

		case BT_REQ_DISCONNECT:
		{
			LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
			if (lDeviceImpl == NULL)
				break;

			lDeviceImpl->Disconnect(message);
		}
		break;

		case BT_REQ_REMOVE_DEVICE:
		{
			LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
			if (lDeviceImpl == NULL)
				break;

			lDeviceImpl->Disconnect(message);

			const bdaddr_t* bdaddr;
			ssize_t addr_size;
			message->FindData("bdaddr", B_ANY_TYPE, (const void**)&bdaddr, &addr_size);

			ServerRemoteDevice* rd = lDeviceImpl->RemoteDeviceByAddr(*bdaddr);
			lDeviceImpl->RemoveRemoteDevice(rd);
		}
		break;

		case BT_START_WATCHING_CONNECTIONS:
		{
			BMessenger* messenger = new BMessenger();
			if (message->FindMessenger("messenger", messenger) == B_OK)
				fWatchersList.AddItem(messenger);
		}
		break;

		case BT_STOP_WATCHING_CONNECTIONS:
		{
			BMessenger* messenger = new BMessenger();
			if (message->FindMessenger("messenger", messenger) == B_OK)
				fWatchersList.RemoveItem(messenger);
		}
		break;

		case BT_MSG_GET_REMOTE_DEVICES:
		{
			LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
			if (lDeviceImpl == NULL)
				break;

			RemoteDevicesList* rdList = lDeviceImpl->GetRemoteDevicesList();

			for (int32 i = 0; i < rdList->CountItems(); i++) {
				ServerRemoteDevice* rd = rdList->ItemAt(i);
				BMessage device;
				bdaddr_t bdaddr = rd->bdaddr;
				device.AddData("bdaddr", B_ANY_TYPE, &bdaddr, sizeof(bdaddr_t));
				device.AddString("name", rd->friendly_name);
				device.AddData("class_of_device", B_RAW_TYPE, rd->classOfDevice,
					sizeof(rd->classOfDevice));
				device.AddUInt16("clock_offset", rd->clock_offset);
				device.AddUInt8("pscan_rep_mode", rd->pscan_rep_mode);

				reply.AddMessage("remote", &device);
			}

			message->SendReply(&reply);
		}
		break;

		case BT_MSG_GET_CONNECTED_DEVICES:
		{
			for (int32 i = 0; i < fLocalDevicesList.CountItems(); i++) {
				RemoteDevicesList* devices
					= fLocalDevicesList.ItemAt(i)->GetRemoteDevicesList();
				for (int32 j = 0; j < devices->CountItems(); j++) {
					ServerRemoteDevice* rd = devices->ItemAt(j);
					if (rd->conn_state != RemoteDevice::CONNECTED)
						continue;
					reply.AddString("name", rd->friendly_name);
					reply.AddData("class_of_device", B_RAW_TYPE,
						rd->classOfDevice, sizeof(rd->classOfDevice));
					reply.AddString("address",
						bdaddrUtils::ToString(rd->bdaddr));
				}
			}
			reply.what = BT_MSG_GET_CONNECTED_DEVICES;
			message->SendReply(&reply);
			return;
		}

		case BT_REQ_CONN_STATE:
		{
			LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
			if (lDeviceImpl == NULL)
				break;

			const bdaddr_t* bdaddr = NULL;
			ssize_t addr_size = 0;
			message->FindData("bdaddr", B_ANY_TYPE, (const void**)&bdaddr, &addr_size);

			// a device the server does not know is not connected
			ServerRemoteDevice* rd = NULL;
			if (bdaddr != NULL && addr_size == sizeof(bdaddr_t))
				rd = lDeviceImpl->RemoteDeviceByAddr(*bdaddr);
			reply.AddUInt8("conn state", static_cast<uint8>(rd != NULL
				? rd->conn_state : RemoteDevice::DISCONNECTED));

			message->SendReply(&reply);
		}
		break;

		default:
			BApplication::MessageReceived(message);
			break;
	}

	// Can we reply right now?
	// TODO: review this condition
	if (status != B_WOULD_BLOCK) {
		reply.AddInt32("status", status);
		message->SendReply(&reply);
//		printf("Sending reply message for->\n");
//		message->PrintToStream();
	}
}


void
BluetoothServer::NotifyWatchers(BMessage* notice)
{
	for (int32 i = 0; i < fWatchersList.CountItems(); i++) {
		BMessenger* messenger = fWatchersList.ItemAt(i);
		if (!messenger->IsValid()) {
			fWatchersList.RemoveItemAt(i);
			i--;
			continue;
		}
		messenger->SendMessage(notice);
	}
}


/*!	Opens the controller at \a path and brings it up. A controller is opened
	once: the device monitor can announce the same node more than once.
*/
void
BluetoothServer::_AddLocalDevice(const char* path)
{
	if (_LocalDeviceByPath(path) != NULL)
		return;

	TRACE_BT("BluetoothServer: Requested LocalDevice %s\n", path);

	BPath devicePath(path);
	LocalDeviceImpl* lDeviceImpl
		= LocalDeviceImpl::CreateTransportAccessor(&devicePath);
	if (lDeviceImpl == NULL)
		return;
	if (lDeviceImpl->GetID() < 0) {
		TRACE_BT("BluetoothServer: Adding LocalDevice hci id invalid\n");
		delete lDeviceImpl;
		return;
	}

	fLocalDevicesList.AddItem(lDeviceImpl);
	TRACE_BT("LocalDevice %s id=%" B_PRId32 " added\n", path,
		lDeviceImpl->GetID());

	/* TODO: This should be by user request only! */
	lDeviceImpl->Launch();

	// Commands sent right after the controller is opened, or right
	// after another one, can get lost: one at a time, a bit later.
	BMessage connectable(kMsgSetConnectable);
	connectable.AddInt32("hci_id", lDeviceImpl->GetID());
	BMessageRunner::StartSending(be_app_messenger, &connectable, 1000000, 1);
	BMessage roleSwitch(kMsgAllowRoleSwitch);
	roleSwitch.AddInt32("hci_id", lDeviceImpl->GetID());
	BMessageRunner::StartSending(be_app_messenger, &roleSwitch, 1500000, 1);
}


/*!	Forgets a controller that failed or left the bus. Its links go with it:
	closing it makes the kernel drop their connections and wake up whoever
	uses their channels.
*/
void
BluetoothServer::_RemoveLocalDevice(LocalDeviceImpl* device)
{
	TRACE_BT("BluetoothServer: Removing LocalDevice %s id=%" B_PRId32 "\n",
		device->Path(), device->GetID());

	fLocalDevicesList.RemoveItem(device);
	delete device;
}


LocalDeviceImpl*
BluetoothServer::_LocalDeviceByPath(const char* path)
{
	for (int32 i = 0; i < fLocalDevicesList.CountItems(); i++) {
		LocalDeviceImpl* device = fLocalDevicesList.ItemAt(i);
		if (strcmp(device->Path(), path) == 0)
			return device;
	}
	return NULL;
}


/*!	Reads the services of \a rd and starts its profile, in a thread of its
	own: reading the records takes seconds, and neither the events of the
	controllers nor the looper may wait for it. The remote devices are never
	deleted, so the thread can keep \a rd.
*/
void
BluetoothServer::StartServices(ServerRemoteDevice* rd)
{
	// One thread at a time per device: encryption goes on again after a
	// pause (a role switch before Bluetooth 2.1 does that), and two threads
	// filled the same services message at once.
	if (atomic_test_and_set(&rd->services_running, 1, 0) != 0)
		return;

	thread_id thread = spawn_thread(_ServicesThread, "bluetooth services",
		B_NORMAL_PRIORITY, rd);
	if (thread >= 0)
		resume_thread(thread);
	else
		atomic_set(&rd->services_running, 0);
}


/*static*/ status_t
BluetoothServer::_ServicesThread(void* data)
{
	ServerRemoteDevice* rd = (ServerRemoteDevice*)data;
	BluetoothServer* server = (BluetoothServer*)be_app;
	server->DiscoverServices(rd);
	server->NotifyServices(rd);
	atomic_set(&rd->services_running, 0);
	return B_OK;
}


void
BluetoothServer::DiscoverServices(ServerRemoteDevice* rd)
{
	TRACE_BT("BluetoothServer: Discovering Services for %s\n", rd->friendly_name.String());
	// The device can be busy reading our own records right after the
	// pairing: try again a few times
	for (int32 attempt = 0; attempt < 3; attempt++) {
		rd->services.MakeEmpty();
		SDPClient sdpClient(rd);
		if (sdpClient.Start() >= 0 && sdpClient.RequestServiceRecords() == B_OK) {
			TRACE_BT("SDP: Service records loaded successfully\n");
			return;
		}
		TRACE_BT("SDP: Error service records could not be loaded\n");
		snooze(500000);
	}

	// rd->services.PrintToStream();
}


void
BluetoothServer::NotifyServices(ServerRemoteDevice* rd)
{
	TRACE_BT("BluetoothServer: Notifying Services for %s\n", rd->friendly_name.String());
	SDPClient sdpClient(rd);

	if (sdpClient.NotifyProfiles() == B_OK)
		TRACE_BT("SDP: Notified the dedicated profile successfully\n");
	else
		TRACE_BT("SDP: Error could not notify the dedicated profile\n");

}


#if 0
#pragma mark -
#endif

LocalDeviceImpl*
BluetoothServer::LocateDelegateFromMessage(BMessage* message)
{
	hci_id hid;

	if (message->FindInt32("hci_id", &hid) != B_OK)
		return NULL;

	return LocateLocalDeviceImpl(hid);
}


LocalDeviceImpl*
BluetoothServer::LocateLocalDeviceImpl(hci_id hid)
{
	// Try to find out when a ID was specified
	int index;

	for (index = 0; index < fLocalDevicesList.CountItems(); index++) {
		LocalDeviceImpl* lDeviceImpl = fLocalDevicesList.ItemAt(index);
		if (lDeviceImpl->GetID() == hid)
			return lDeviceImpl;
	}

	return NULL;
}


#if 0
#pragma - Messages reply
#endif

status_t
BluetoothServer::HandleLocalDevicesCount(BMessage* message, BMessage* reply)
{
	TRACE_BT("BluetoothServer: count requested\n");

	return reply->AddInt32("count", fLocalDevicesList.CountItems());
}


status_t
BluetoothServer::HandleAcquireLocalDevice(BMessage* message, BMessage* reply)
{
	hci_id hid;
	ssize_t size;
	bdaddr_t bdaddr;
	LocalDeviceImpl* lDeviceImpl = NULL;
	static int32 lastIndex = 0;

	if (message->FindInt32("hci_id", &hid) == B_OK)	{
		TRACE_BT("BluetoothServer: GetLocalDevice requested with id\n");
		lDeviceImpl = LocateDelegateFromMessage(message);

	} else if (message->FindData("bdaddr", B_ANY_TYPE,
		(const void**)&bdaddr, &size) == B_OK) {

		// Try to find out when the user specified the address
		TRACE_BT("BluetoothServer: GetLocalDevice requested with bdaddr\n");
		for (lastIndex = 0; lastIndex < fLocalDevicesList.CountItems();
			lastIndex ++) {
			// TODO: Only possible if the property is available
			// bdaddr_t local;
			// lDeviceImpl = fLocalDevicesList.ItemAt(lastIndex);
			// if ((lDeviceImpl->GetAddress(&local, message) == B_OK)
			// 	&& bacmp(&local, &bdaddr)) {
			// 	break;
			// }
		}

	} else {
		// Careless, any device not performing operations will be fine
		TRACE_BT("BluetoothServer: GetLocalDevice plain request\n");
		// from last assigned till end
		for (int index = lastIndex + 1;
			index < fLocalDevicesList.CountItems();	index++) {
			lDeviceImpl= fLocalDevicesList.ItemAt(index);
			if (lDeviceImpl != NULL && lDeviceImpl->Available()) {
				printf("Requested local device %" B_PRId32 "\n",
					lDeviceImpl->GetID());
				TRACE_BT("BluetoothServer: Device available: %" B_PRId32 "\n", lDeviceImpl->GetID());
				lastIndex = index;
				break;
			}
		}

		// from starting till last assigned if not yet found
		if (lDeviceImpl == NULL) {
			for (int index = 0; index <= lastIndex ; index ++) {
				lDeviceImpl = fLocalDevicesList.ItemAt(index);
				if (lDeviceImpl != NULL && lDeviceImpl->Available()) {
					printf("Requested local device %" B_PRId32 "\n",
						lDeviceImpl->GetID());
					TRACE_BT("BluetoothServer: Device available: %" B_PRId32 "\n", lDeviceImpl->GetID());
					lastIndex = index;
					break;
				}
			}
		}
	}

	if (lastIndex <= fLocalDevicesList.CountItems() && lDeviceImpl != NULL
		&& lDeviceImpl->Available()) {

		hid = lDeviceImpl->GetID();
		lDeviceImpl->Acquire();

		TRACE_BT("BluetoothServer: Device acquired %" B_PRId32 "\n", hid);
		return reply->AddInt32("hci_id", hid);
	}

	return B_ERROR;

}


status_t
BluetoothServer::HandleSimpleRequest(BMessage* message, BMessage* reply)
{
	LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
	if (lDeviceImpl == NULL) {
		return B_ERROR;
	}

	const char* propertyRequested;

	// Find out if there is a property being requested,
	if (message->FindString("property", &propertyRequested) == B_OK) {
		// Check if the property has been already retrieved
		if (lDeviceImpl->IsPropertyAvailable(propertyRequested)) {
			// Dump everything
			reply->AddMessage("properties", lDeviceImpl->GetPropertiesMessage());
			return B_OK;
		}
	}

	// we are gonna need issue the command ...
	if (lDeviceImpl->ProcessSimpleRequest(DetachCurrentMessage()) == B_OK)
		return B_WOULD_BLOCK;
	else {
		lDeviceImpl->Unregister();
		return B_ERROR;
	}

}


status_t
BluetoothServer::HandleGetProperty(BMessage* message, BMessage* reply)
{
	// User side will look for the reply in a result field and will
	// not care about status fields, therefore we return OK in all cases

	LocalDeviceImpl* lDeviceImpl = LocateDelegateFromMessage(message);
	if (lDeviceImpl == NULL) {
		return B_ERROR;
	}

	const char* propertyRequested;

	// Find out if there is a property being requested,
	if (message->FindString("property", &propertyRequested) == B_OK) {

		TRACE_BT("BluetoothServer: Searching %s property...\n", propertyRequested);

		// Check if the property has been already retrieved
		if (lDeviceImpl->IsPropertyAvailable(propertyRequested)) {

			// 1 bytes requests
			if (strcmp(propertyRequested, "hci_version") == 0
				|| strcmp(propertyRequested, "lmp_version") == 0
				|| strcmp(propertyRequested, "sco_mtu") == 0) {

				uint8 result = lDeviceImpl->GetPropertiesMessage()->
					FindInt8(propertyRequested);
				reply->AddInt32("result", result);

			// 2 bytes requests
			} else if (strcmp(propertyRequested, "hci_revision") == 0
					|| strcmp(propertyRequested, "lmp_subversion") == 0
					|| strcmp(propertyRequested, "manufacturer") == 0
					|| strcmp(propertyRequested, "acl_mtu") == 0
					|| strcmp(propertyRequested, "acl_max_pkt") == 0
					|| strcmp(propertyRequested, "sco_max_pkt") == 0
					|| strcmp(propertyRequested, "packet_type") == 0 ) {

				uint16 result = lDeviceImpl->GetPropertiesMessage()->
					FindInt16(propertyRequested);
				reply->AddInt32("result", result);

			// 1 bit requests
			} else if (strcmp(propertyRequested, "role_switch_capable") == 0
					|| strcmp(propertyRequested, "encrypt_capable") == 0) {

				bool result = lDeviceImpl->GetPropertiesMessage()->
					FindBool(propertyRequested);

				reply->AddInt32("result", result);



			} else {
				TRACE_BT("BluetoothServer: Property %s could not be satisfied\n", propertyRequested);
			}
		}
	}

	return B_OK;
}


#if 0
#pragma mark -
#endif


void
BluetoothServer::ShowWindow(BWindow* pWindow)
{
	pWindow->Lock();
	if (pWindow->IsHidden())
		pWindow->Show();
	else
		pWindow->Activate();
	pWindow->Unlock();
}


void
BluetoothServer::_InstallDeskbarIcon()
{
	app_info appInfo;
	be_app->GetAppInfo(&appInfo);

	BDeskbar deskbar;

	if (deskbar.HasItem(kDeskbarItemName)) {
		_RemoveDeskbarIcon();
	}

	status_t res = deskbar.AddItem(&appInfo.ref);
	if (res != B_OK)
		TRACE_BT("Failed adding deskbar icon: %" B_PRId32 "\n", res);
}


void
BluetoothServer::_RemoveDeskbarIcon()
{
	BDeskbar deskbar;
	status_t res = deskbar.RemoveItem(kDeskbarItemName);
	if (res != B_OK)
		TRACE_BT("Failed removing Deskbar icon: %" B_PRId32 ": \n", res);
}


#if 0
#pragma mark -
#endif

int
main(int /*argc*/, char** /*argv*/)
{
	BluetoothServer* bluetoothServer = new BluetoothServer;

	bluetoothServer->Run();
	delete bluetoothServer;

	return 0;
}

