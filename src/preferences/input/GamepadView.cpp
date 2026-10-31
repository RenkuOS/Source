/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "GamepadView.h"

#include <Catalog.h>
#include <ControlLook.h>
#include <Joystick.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <StringView.h>

#include <bluetoothserver_p.h>

#include <stdio.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "GamepadView"


static const uint32 kMsgPoll = 'gpPo';
static const uint32 kMsgAskBattery = 'gpBa';

static const int32 kMaxAxes = 8;
static const int32 kMaxHats = 4;


//	#pragma mark - GamepadTestView


/*!	Shows the state of a gamepad: the first two pairs of axes as sticks, the
	next two as triggers, the first hat as a cross and the buttons.
*/
class GamepadTestView : public BView {
public:
	GamepadTestView(bool xboxLabels)
		:
		BView("test", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
		fXboxLabels(xboxLabels),
		fAxisCount(0),
		fButtonCount(0),
		fHat(0),
		fButtons(0)
	{
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		memset(fAxes, 0, sizeof(fAxes));
		float size = be_control_look->DefaultLabelSpacing();
		SetExplicitMinSize(BSize(size * 50, size * 30));
	}

	void SetState(int32 axisCount, const int16* axes, uint8 hat,
		int32 buttonCount, uint32 buttons)
	{
		if (axisCount == fAxisCount && hat == fHat && buttonCount == fButtonCount
			&& buttons == fButtons
			&& memcmp(axes, fAxes, sizeof(int16) * axisCount) == 0)
			return;

		fAxisCount = std::min(axisCount, kMaxAxes);
		memcpy(fAxes, axes, sizeof(int16) * fAxisCount);
		fHat = hat;
		fButtonCount = buttonCount;
		fButtons = buttons;
		Invalidate();
	}

	virtual void Draw(BRect updateRect)
	{
		BRect bounds = Bounds();
		float spacing = be_control_look->DefaultLabelSpacing();
		float stickRadius = std::min(bounds.Width() / 8,
			bounds.Height() / 4.5f);

		BPoint left(bounds.left + bounds.Width() * 0.2f,
			bounds.top + stickRadius + spacing);
		BPoint right(bounds.left + bounds.Width() * 0.8f, left.y);
		BPoint hat(bounds.left + bounds.Width() * 0.5f, left.y);

		if (fAxisCount >= 2)
			_DrawStick(left, stickRadius, fAxes[0], fAxes[1]);
		if (fAxisCount >= 4)
			_DrawStick(right, stickRadius, fAxes[2], fAxes[3]);
		_DrawHat(hat, stickRadius * 0.6f);

		// the triggers under the sticks
		float barTop = left.y + stickRadius + spacing * 1.5f;
		float barHeight = spacing * 1.2f;
		if (fAxisCount >= 5) {
			_DrawTrigger(BRect(left.x - stickRadius, barTop,
				left.x + stickRadius, barTop + barHeight), fAxes[4]);
		}
		if (fAxisCount >= 6) {
			_DrawTrigger(BRect(right.x - stickRadius, barTop,
				right.x + stickRadius, barTop + barHeight), fAxes[5]);
		}

		// the buttons in a row, or two
		float buttonTop = barTop + barHeight + spacing * 2;
		int32 perRow = std::max((int32)1, std::min(fButtonCount, (int32)8));
		float cell = std::min(bounds.Width() / perRow,
			(bounds.bottom - buttonTop) / 2);
		float radius = std::max(cell / 2 - spacing / 2, 4.0f);
		for (int32 i = 0; i < fButtonCount && i < 32; i++) {
			int32 row = i / perRow;
			int32 column = i % perRow;
			BPoint center(bounds.left + cell * (column + 0.5f),
				buttonTop + cell * (row + 0.5f));
			_DrawButton(center, radius, i, (fButtons & (1UL << i)) != 0);
		}
	}

private:
	rgb_color _ActiveColor()
	{
		return ui_color(B_KEYBOARD_NAVIGATION_COLOR);
	}

	void _DrawStick(BPoint center, float radius, int16 x, int16 y)
	{
		SetHighUIColor(B_CONTROL_BACKGROUND_COLOR);
		FillEllipse(center, radius, radius);
		SetHighUIColor(B_CONTROL_BORDER_COLOR);
		StrokeEllipse(center, radius, radius);
		StrokeLine(center - BPoint(radius, 0), center + BPoint(radius, 0));
		StrokeLine(center - BPoint(0, radius), center + BPoint(0, radius));

		float knob = radius / 4;
		BPoint position(center.x + x / 32768.0f * (radius - knob),
			center.y + y / 32768.0f * (radius - knob));
		SetHighColor(_ActiveColor());
		FillEllipse(position, knob, knob);
	}

	void _DrawTrigger(BRect frame, int16 value)
	{
		SetHighUIColor(B_CONTROL_BACKGROUND_COLOR);
		FillRect(frame);
		float level = (value + 32768) / 65535.0f;
		BRect filled(frame);
		filled.right = frame.left + frame.Width() * level;
		SetHighColor(_ActiveColor());
		FillRect(filled);
		SetHighUIColor(B_CONTROL_BORDER_COLOR);
		StrokeRect(frame);
	}

	void _DrawHat(BPoint center, float size)
	{
		// 1 is up, then clockwise in steps of 45 degrees, 0 is centered
		static const int8 kDirections[9][2] = {
			{ 0, 0 }, { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 }, { 0, 1 },
			{ -1, 1 }, { -1, 0 }, { -1, -1 }
		};
		float arm = size / 3;
		int dx = fHat <= 8 ? kDirections[fHat][0] : 0;
		int dy = fHat <= 8 ? kDirections[fHat][1] : 0;

		for (int direction = 0; direction < 4; direction++) {
			int x = direction == 1 ? 1 : direction == 3 ? -1 : 0;
			int y = direction == 0 ? -1 : direction == 2 ? 1 : 0;
			BRect arrow(center.x + x * arm * 1.5f - arm / 2,
				center.y + y * arm * 1.5f - arm / 2,
				center.x + x * arm * 1.5f + arm / 2,
				center.y + y * arm * 1.5f + arm / 2);
			bool pressed = (x != 0 && x == dx) || (y != 0 && y == dy);
			if (pressed)
				SetHighColor(_ActiveColor());
			else
				SetHighUIColor(B_CONTROL_BACKGROUND_COLOR);
			FillRect(arrow);
			SetHighUIColor(B_CONTROL_BORDER_COLOR);
			StrokeRect(arrow);
		}
	}

	void _DrawButton(BPoint center, float radius, int32 index, bool pressed)
	{
		static const char* kXboxLabels[] = { "A", "B", "X", "Y", "LB", "RB",
			"View", "Menu", "LS", "RS" };

		if (pressed)
			SetHighColor(_ActiveColor());
		else
			SetHighUIColor(B_CONTROL_BACKGROUND_COLOR);
		FillEllipse(center, radius, radius);
		SetHighUIColor(B_CONTROL_BORDER_COLOR);
		StrokeEllipse(center, radius, radius);

		char number[12];
		const char* label;
		if (fXboxLabels && index < (int32)B_COUNT_OF(kXboxLabels))
			label = kXboxLabels[index];
		else {
			snprintf(number, sizeof(number), "%" B_PRId32, index + 1);
			label = number;
		}

		BFont font;
		GetFont(&font);
		if (font.StringWidth(label) > radius * 1.8f)
			font.SetSize(font.Size() * 0.75f);
		SetFont(&font);
		font_height height;
		font.GetHeight(&height);

		SetHighUIColor(pressed ? B_CONTROL_BACKGROUND_COLOR
			: B_CONTROL_TEXT_COLOR);
		SetLowColor(pressed ? _ActiveColor()
			: ui_color(B_CONTROL_BACKGROUND_COLOR));
		DrawString(label, BPoint(center.x - font.StringWidth(label) / 2,
			center.y + (height.ascent - height.descent) / 2));
		SetFont(be_plain_font);
	}

	bool	fXboxLabels;
	int32	fAxisCount;
	int16	fAxes[kMaxAxes];
	int32	fButtonCount;
	uint8	fHat;
	uint32	fButtons;
};


//	#pragma mark - GamepadView


GamepadView::GamepadView(const char* path, const char* name,
	int32 bluetoothIndex)
	:
	BView(path, B_WILL_DRAW),
	fPath(path),
	fName(name),
	fBluetoothIndex(bluetoothIndex),
	fJoystick(NULL),
	fPollRunner(NULL),
	fBatteryRunner(NULL),
	fBattery(NULL)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

	BStringView* title = new BStringView("name", name);
	title->SetFont(be_bold_font);

	fBattery = new BStringView("battery", "");

	BStringView* hint = new BStringView("hint",
		B_TRANSLATE("Move the sticks and press the buttons to try them."));

	fTestView = new GamepadTestView(fName.FindFirst("Xbox") >= 0);

	BLayoutBuilder::Group<>(this, B_VERTICAL)
		.Add(title)
		.Add(fBattery)
		.Add(hint)
		.Add(fTestView, 1)
		.End();
}


GamepadView::~GamepadView()
{
	delete fJoystick;
}


void
GamepadView::AttachedToWindow()
{
	BView::AttachedToWindow();

	fJoystick = new BJoystick;
	if (fJoystick->Open(fPath.String(), true) < B_OK) {
		delete fJoystick;
		fJoystick = NULL;
	}

	BMessage poll(kMsgPoll);
	fPollRunner = new BMessageRunner(BMessenger(this), &poll, 33000);

	if (fBluetoothIndex >= 0) {
		BMessage battery(kMsgAskBattery);
		fBatteryRunner = new BMessageRunner(BMessenger(this), &battery,
			2000000);
		_AskBattery();
	} else
		fBattery->SetText(B_TRANSLATE("Connected by USB"));
}


void
GamepadView::DetachedFromWindow()
{
	delete fPollRunner;
	fPollRunner = NULL;
	delete fBatteryRunner;
	fBatteryRunner = NULL;
	delete fJoystick;
	fJoystick = NULL;

	BView::DetachedFromWindow();
}


void
GamepadView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgPoll:
			_Poll();
			break;

		case kMsgAskBattery:
			_AskBattery();
			break;

		case BT_MSG_GET_CONNECTED_DEVICES:
			_UpdateBattery(message);
			break;

		default:
			BView::MessageReceived(message);
			break;
	}
}


void
GamepadView::_Poll()
{
	// only the card on screen reads its gamepad
	if (fJoystick == NULL || IsHidden() || fJoystick->Update() != B_OK)
		return;

	int16 axes[kMaxAxes] = {};
	uint8 hats[kMaxHats] = {};
	int32 axisCount = std::min(fJoystick->CountAxes(), kMaxAxes);
	if (axisCount > 0)
		fJoystick->GetAxisValues(axes);
	if (fJoystick->CountHats() > 0)
		fJoystick->GetHatValues(hats);

	fTestView->SetState(axisCount, axes, hats[0], fJoystick->CountButtons(),
		fJoystick->ButtonValues());
}


void
GamepadView::_AskBattery()
{
	BMessage request(BT_MSG_GET_CONNECTED_DEVICES);
	BMessenger(BLUETOOTH_SIGNATURE).SendMessage(&request, this);
}


void
GamepadView::_UpdateBattery(BMessage* reply)
{
	// the Bluetooth gamepads are published in the order they connected
	int32 index = 0;
	int32 battery = -1;
	const void* data;
	ssize_t size;
	for (int32 i = 0; reply->FindData("class_of_device", B_RAW_TYPE, i, &data,
			&size) == B_OK; i++) {
		const uint8* deviceClass = (const uint8*)data;
		// major class 5: peripheral, with a joystick or gamepad minor class
		if (size < 3 || (deviceClass[1] & 0x1f) != 5
			|| ((deviceClass[0] >> 2) & 0x0f) == 0
			|| ((deviceClass[0] >> 2) & 0x0f) > 2)
			continue;
		if (index++ == fBluetoothIndex) {
			battery = reply->GetInt32("battery", i, -1);
			break;
		}
	}

	if (battery < 0) {
		fBattery->SetText(B_TRANSLATE("Connected by Bluetooth"));
		return;
	}

	BString text(B_TRANSLATE("Connected by Bluetooth, battery %level%%"));
	BString level;
	level << battery;
	text.ReplaceFirst("%level%", level.String());
	fBattery->SetText(text.String());
}
