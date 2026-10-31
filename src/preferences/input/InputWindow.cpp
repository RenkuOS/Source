/*
 * Copyright 2019-2020, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Preetpal Kaur <preetpalok123@gmail.com>
 *		Adrien Destugues <pulkomandy@gmail.com>
 */


#include <CardLayout.h>
#include <CardView.h>
#include <Catalog.h>
#include <Control.h>
#include <ControlLook.h>
#include <LayoutBuilder.h>
#include <ScrollView.h>
#include <StringList.h>

#include <Joystick.h>
#include <Messenger.h>
#include <private/input/InputServerTypes.h>

#include <bluetoothserver_p.h>

#include "InputConstants.h"
#include "GamepadView.h"
#include "InputDeviceView.h"
#include "InputKeyboard.h"
#include "InputMouse.h"
#include "InputTouchpadPref.h"
#include "InputTouchpadPrefView.h"
#include "InputWindow.h"
#include "MouseSettings.h"
#include "SettingsView.h"

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "InputWindow"


static const uint32 kMsgScanGamepads = 'scGp';


InputWindow::InputWindow(BRect rect)
	:
	BWindow(rect, B_TRANSLATE_SYSTEM_NAME("Input"), B_TITLED_WINDOW,
		B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS
			| B_AUTO_UPDATE_SIZE_LIMITS | B_QUIT_ON_WINDOW_CLOSE)
{
	fDeviceListView = new BListView(B_TRANSLATE("Device List"));
	fDeviceListView->SetSelectionMessage(new BMessage(ITEM_SELECTED));
	fDeviceListView->SetExplicitMinSize(
		BSize(be_control_look->ComposeIconSize(32).Width()
				+ fDeviceListView->StringWidth("Extended PS/2 Mouse 1"),
			B_SIZE_UNSET));

	BScrollView* scrollView = new BScrollView(
		"scrollView", fDeviceListView, 0, false, B_FANCY_BORDER);
	fCardView = new BCardView();

	BLayoutBuilder::Group<>(this, B_HORIZONTAL, 10)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(scrollView, 1)
		.Add(fCardView, 3);

	FindDevice();

	// the gamepads are not input_server devices: look for them now and then
	_ScanGamepads();
	BMessage scan(kMsgScanGamepads);
	fGamepadRunner = new BMessageRunner(BMessenger(this), &scan, 2000000);
}


void
InputWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case ITEM_SELECTED:
		{
			int32 index = message->GetInt32("index", 0);
			if (index >= 0)
				fCardView->CardLayout()->SetVisibleItem(index);
			break;
		}
		case kMsgMouseType:
		case kMsgMouseMap:
		case kMsgMouseFocusMode:
		case kMsgFollowsMouseMode:
		case kMsgAcceptFirstClick:
		case kMsgDoubleClickSpeed:
		case kMsgMouseSpeed:
		case kMsgAccelerationFactor:
		case kMsgDefaults:
		case kMsgRevert:
		{
			PostMessage(
				message, fCardView->CardLayout()->VisibleItem()->View());
			break;
		}
		case SCROLL_AREA_CHANGED:
		case SCROLL_CONTROL_CHANGED:
		case TAP_CONTROL_CHANGED:
		case PAD_SPEED_CHANGED:
		case PAD_ACCELERATION_CHANGED:
		case DEFAULT_SETTINGS:
		case REVERT_SETTINGS:
		{
			PostMessage(
				message, fCardView->CardLayout()->VisibleItem()->View());
			break;
		}
		case kMsgSliderrepeatrate:
		case kMsgSliderdelayrate:
		{
			PostMessage(
				message, fCardView->CardLayout()->VisibleItem()->View());
			break;
		}

		case kMsgScanGamepads:
			_ScanGamepads();
			break;

		case B_INPUT_DEVICES_CHANGED:
		{
			int32 operation = message->FindInt32("be:opcode");
			BString name = message->FindString("be:device_name");

			if (operation == B_INPUT_DEVICE_ADDED) {
				BInputDevice* device = find_input_device(name);
				if (device)
					AddDevice(device);
			} else if (operation == B_INPUT_DEVICE_REMOVED) {
				for (int i = 0; i < fDeviceListView->CountItems(); i++) {
					DeviceListItemView* item
						= dynamic_cast<DeviceListItemView*>(
							fDeviceListView->ItemAt(i));
					if (item != NULL && item->Label() == name) {
						fDeviceListView->RemoveItem(i);
						BView* settings = fCardView->ChildAt(i);
						fCardView->RemoveChild(settings);
						delete settings;
						break;
					}
				}
			}

			break;
		}

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


void
InputWindow::Show()
{
	CenterOnScreen();
	BWindow::Show();
	watch_input_devices(this, true);
}


void
InputWindow::Hide()
{
	BWindow::Hide();
	watch_input_devices(this, false);
}


status_t
InputWindow::FindDevice()
{
	BList devList;
	status_t status = get_input_devices(&devList);
	if (status != B_OK)
		return status;

	int32 i = 0;

	while (true) {
		BInputDevice* dev = (BInputDevice*)devList.ItemAt(i);
		if (dev == NULL)
			break;
		i++;

		AddDevice(dev);
	}

	return B_OK;
}


void
InputWindow::AddDevice(BInputDevice* dev)
{
	BString name = dev->Name();

	if (dev->Type() == B_POINTING_DEVICE && name.FindFirst("Touchpad") >= 0) {
		TouchpadPrefView* view = new TouchpadPrefView(dev);
		fCardView->AddChild(view);

		DeviceListItemView* touchpad
			= new DeviceListItemView(name, TOUCHPAD_TYPE);
		fDeviceListView->AddItem(touchpad);
	} else if (dev->Type() == B_POINTING_DEVICE) {
		MouseSettings* settings;
		settings = fMultipleMouseSettings.AddMouseSettings(name);

		InputMouse* view = new InputMouse(dev, settings);
		fCardView->AddChild(view);

		DeviceListItemView* mouse = new DeviceListItemView(name, MOUSE_TYPE);
		fDeviceListView->AddItem(mouse);
	} else if (dev->Type() == B_KEYBOARD_DEVICE) {
		InputKeyboard* view = new InputKeyboard(dev);
		fCardView->AddChild(view);

		DeviceListItemView* keyboard
			= new DeviceListItemView(name, KEYBOARD_TYPE);
		fDeviceListView->AddItem(keyboard);
	} else
		delete dev;
}


void
InputWindow::_ScanGamepads()
{
	BJoystick joystick;
	BStringList paths;
	int32 count = joystick.CountDevices();
	for (int32 i = 0; i < count; i++) {
		char name[B_OS_NAME_LENGTH];
		if (joystick.GetDeviceName(i, name, sizeof(name)) == B_OK)
			paths.Add(name);
	}

	// the cards of the gamepads gone
	for (int32 i = fCardView->CountChildren() - 1; i >= 0; i--) {
		GamepadView* view = dynamic_cast<GamepadView*>(fCardView->ChildAt(i));
		if (view == NULL || paths.HasString(view->Path()))
			continue;
		delete fDeviceListView->RemoveItem(i);
		fCardView->RemoveChild(view);
		delete view;
	}

	// the names of the Bluetooth ones, from the Bluetooth server
	BMessage connected;
	BMessage request(BT_MSG_GET_CONNECTED_DEVICES);
	if (BMessenger(BLUETOOTH_SIGNATURE).SendMessage(&request, &connected,
			500000, 500000) != B_OK)
		connected.MakeEmpty();

	int32 usbIndex = 0;
	int32 bluetoothIndex = 0;
	for (int32 i = 0; i < paths.CountStrings(); i++) {
		BString path = paths.StringAt(i);
		bool bluetooth = path.StartsWith("bluetooth/");
		int32 index = bluetooth ? bluetoothIndex++ : usbIndex++;

		bool known = false;
		for (int32 j = 0; j < fCardView->CountChildren(); j++) {
			GamepadView* view
				= dynamic_cast<GamepadView*>(fCardView->ChildAt(j));
			if (view != NULL && path == view->Path())
				known = true;
		}
		if (known)
			continue;

		BString name;
		if (bluetooth) {
			// the gamepads among the connected devices, in their order
			int32 gamepad = 0;
			const void* data;
			ssize_t size;
			for (int32 j = 0; connected.FindData("class_of_device", B_RAW_TYPE,
					j, &data, &size) == B_OK; j++) {
				const uint8* deviceClass = (const uint8*)data;
				int kind = (deviceClass[0] >> 2) & 0x0f;
				if (size < 3 || (deviceClass[1] & 0x1f) != 5 || kind < 1
					|| kind > 2)
					continue;
				if (gamepad++ == index) {
					name = connected.GetString("name", j, "");
					break;
				}
			}
			if (name.IsEmpty())
				name = B_TRANSLATE("Bluetooth gamepad");
		} else {
			name = B_TRANSLATE("USB gamepad %number%");
			BString number;
			number << index + 1;
			name.ReplaceFirst("%number%", number.String());
		}

		fCardView->AddChild(new GamepadView(path.String(), name.String(),
			bluetooth ? index : -1));
		fDeviceListView->AddItem(new DeviceListItemView(name, GAMEPAD_TYPE));
	}
}
