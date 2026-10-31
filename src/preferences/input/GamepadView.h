/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef GAMEPAD_VIEW_H
#define GAMEPAD_VIEW_H


#include <String.h>
#include <View.h>


class BJoystick;
class BMessageRunner;
class BStringView;
class GamepadTestView;


/*!	The card of a gamepad: its name, its battery when it tells it, and a live
	view of its sticks, triggers and buttons to try it.
*/
class GamepadView : public BView {
public:
								GamepadView(const char* path,
									const char* name, int32 bluetoothIndex);
	virtual						~GamepadView();

	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				MessageReceived(BMessage* message);

			const char*			Path() const { return fPath.String(); }

private:
			void				_Poll();
			void				_AskBattery();
			void				_UpdateBattery(BMessage* reply);

			BString				fPath;
			BString				fName;
			int32				fBluetoothIndex;
				// among the connected Bluetooth devices, -1 when not one
			BJoystick*			fJoystick;
			BMessageRunner*		fPollRunner;
			BMessageRunner*		fBatteryRunner;
			BStringView*		fBattery;
			GamepadTestView*	fTestView;
};


#endif	// GAMEPAD_VIEW_H
