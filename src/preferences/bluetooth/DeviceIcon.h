/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef DEVICE_ICON_H
#define DEVICE_ICON_H


#include <Rect.h>
#include <SupportDefs.h>


class BView;


enum device_kind {
	DEVICE_KIND_OTHER,
	DEVICE_KIND_COMPUTER,
	DEVICE_KIND_PHONE,
	DEVICE_KIND_AUDIO,
	DEVICE_KIND_KEYBOARD,
	DEVICE_KIND_MOUSE,
	DEVICE_KIND_GAMEPAD
};


device_kind		DeviceKind(uint8 majorClass, uint8 minorClass);
const char*		DeviceKindName(device_kind kind);
void			DrawDeviceIcon(BView* view, BRect frame, device_kind kind);


#endif	// DEVICE_ICON_H
