/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "DeviceIcon.h"

#include <Catalog.h>
#include <Gradient.h>
#include <GradientLinear.h>
#include <Shape.h>
#include <View.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Device kind"


device_kind
DeviceKind(uint8 majorClass, uint8 minorClass)
{
	switch (majorClass) {
		case 1:
			return DEVICE_KIND_COMPUTER;
		case 2:
			return DEVICE_KIND_PHONE;
		case 4:
			return DEVICE_KIND_AUDIO;
		case 5:
		{
			// peripheral: bits 4-5 keyboard and pointer, bits 0-3 the kind
			uint8 kind = minorClass & 0x0f;
			if (kind == 1 || kind == 2)
				return DEVICE_KIND_GAMEPAD;
			if ((minorClass & 0x10) != 0)
				return DEVICE_KIND_KEYBOARD;
			if ((minorClass & 0x20) != 0)
				return DEVICE_KIND_MOUSE;
			return DEVICE_KIND_OTHER;
		}
	}
	return DEVICE_KIND_OTHER;
}


const char*
DeviceKindName(device_kind kind)
{
	switch (kind) {
		case DEVICE_KIND_COMPUTER:
			return B_TRANSLATE("Computer");
		case DEVICE_KIND_PHONE:
			return B_TRANSLATE("Phone");
		case DEVICE_KIND_AUDIO:
			return B_TRANSLATE("Audio");
		case DEVICE_KIND_KEYBOARD:
			return B_TRANSLATE("Keyboard");
		case DEVICE_KIND_MOUSE:
			return B_TRANSLATE("Mouse");
		case DEVICE_KIND_GAMEPAD:
			return B_TRANSLATE("Gamepad");
		default:
			return B_TRANSLATE("Device");
	}
}


namespace {


// Draws in a unit square mapped onto the icon frame
class Pen {
public:
	Pen(BView* view, BRect frame)
		:
		fView(view),
		fFrame(frame),
		fSize(frame.Width())
	{
	}

	BPoint P(float x, float y) const
	{
		return BPoint(fFrame.left + x * fSize, fFrame.top + y * fSize);
	}

	BRect R(float left, float top, float right, float bottom) const
	{
		return BRect(P(left, top), P(right, bottom));
	}

	float S(float value) const
	{
		return value * fSize;
	}

	void FillRound(float left, float top, float right, float bottom,
		float radius) const
	{
		fView->FillRoundRect(R(left, top, right, bottom), S(radius), S(radius));
	}

	void FillCircle(float x, float y, float radius) const
	{
		fView->FillEllipse(P(x, y), S(radius), S(radius));
	}

private:
	BView*	fView;
	BRect	fFrame;
	float	fSize;
};


}	// namespace


void
DrawDeviceIcon(BView* view, BRect frame, device_kind kind)
{
	view->PushState();
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);

	Pen pen(view, frame);

	// the tile, in the blue of Bluetooth
	const rgb_color top = { 74, 156, 242, 255 };
	const rgb_color bottom = { 23, 98, 196, 255 };
	BGradientLinear gradient(frame.LeftTop(), frame.LeftBottom());
	gradient.AddColor(top, 0);
	gradient.AddColor(bottom, 255);
	view->FillRoundRect(frame, pen.S(0.22), pen.S(0.22), gradient);

	const rgb_color white = { 255, 255, 255, 255 };
	view->SetHighColor(white);

	switch (kind) {
		case DEVICE_KIND_GAMEPAD:
			pen.FillRound(0.16, 0.34, 0.84, 0.66, 0.13);
			pen.FillCircle(0.28, 0.64, 0.12);
			pen.FillCircle(0.72, 0.64, 0.12);
			view->SetHighColor(bottom);
			// direction pad and buttons
			view->FillRect(pen.R(0.29, 0.42, 0.35, 0.58));
			view->FillRect(pen.R(0.24, 0.47, 0.40, 0.53));
			pen.FillCircle(0.65, 0.45, 0.035);
			pen.FillCircle(0.73, 0.53, 0.035);
			break;

		case DEVICE_KIND_KEYBOARD:
		{
			pen.FillRound(0.14, 0.32, 0.86, 0.70, 0.05);
			view->SetHighColor(bottom);
			for (int row = 0; row < 2; row++) {
				for (int column = 0; column < 6; column++) {
					float x = 0.20 + column * 0.105;
					float y = 0.38 + row * 0.10;
					view->FillRect(pen.R(x, y, x + 0.065, y + 0.06));
				}
			}
			view->FillRect(pen.R(0.31, 0.59, 0.69, 0.64));
			break;
		}

		case DEVICE_KIND_MOUSE:
			view->FillEllipse(pen.R(0.33, 0.20, 0.67, 0.82));
			view->SetHighColor(bottom);
			view->SetPenSize(pen.S(0.035));
			view->StrokeLine(pen.P(0.5, 0.22), pen.P(0.5, 0.44));
			view->StrokeLine(pen.P(0.34, 0.44), pen.P(0.66, 0.44));
			break;

		case DEVICE_KIND_AUDIO:
			view->SetPenSize(pen.S(0.07));
			view->StrokeArc(pen.R(0.24, 0.24, 0.76, 0.76), 0, 180);
			pen.FillRound(0.19, 0.50, 0.33, 0.76, 0.05);
			pen.FillRound(0.67, 0.50, 0.81, 0.76, 0.05);
			break;

		case DEVICE_KIND_PHONE:
			pen.FillRound(0.33, 0.16, 0.67, 0.84, 0.07);
			view->SetHighColor(bottom);
			view->FillRect(pen.R(0.38, 0.25, 0.62, 0.70));
			pen.FillCircle(0.5, 0.77, 0.025);
			break;

		case DEVICE_KIND_COMPUTER:
			pen.FillRound(0.16, 0.22, 0.84, 0.62, 0.04);
			view->FillRect(pen.R(0.44, 0.62, 0.56, 0.72));
			pen.FillRound(0.32, 0.71, 0.68, 0.78, 0.02);
			view->SetHighColor(bottom);
			view->FillRect(pen.R(0.21, 0.27, 0.79, 0.57));
			break;

		default:
		{
			// the Bluetooth rune
			BPoint rune[] = {
				pen.P(0.35, 0.35), pen.P(0.65, 0.63), pen.P(0.5, 0.77),
				pen.P(0.5, 0.23), pen.P(0.65, 0.37), pen.P(0.35, 0.65)
			};
			view->SetPenSize(pen.S(0.065));
			view->SetLineMode(B_ROUND_CAP, B_ROUND_JOIN);
			view->StrokePolygon(rune, 6, false);
			break;
		}
	}

	view->PopState();
}
