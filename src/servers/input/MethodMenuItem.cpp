// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
//
//      Copyright (c) 2004, Haiku
//
//  This software is part of the Haiku distribution and is covered
//  by the Haiku license.
//
//
//  File:                       MethodMenuItem.cpp
//  Authors:            Jérôme Duval,
//
//  Description:        Input Server
//  Created:            October 19, 2004
//
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

#include <stdlib.h>
#include <string.h>

#include <ControlLook.h>
#include <IconUtils.h>

#include "MethodMenuItem.h"

MethodMenuItem::MethodMenuItem(int32 cookie, const char* name,
	const uchar* icon, const uint8* vectorIcon, size_t vectorIconSize,
	BMenu* subMenu, BMessenger& messenger)
	: BMenuItem(subMenu),
	fVectorIcon(NULL),
	fVectorIconSize(0),
	fIcon(BRect(B_ORIGIN, be_control_look->ComposeIconSize(B_MINI_ICON)),
		B_RGBA32),
	fCookie(cookie)
{
	SetLabel(name);
	SetIcon(icon, vectorIcon, vectorIconSize);
	fMessenger = messenger;
}


MethodMenuItem::MethodMenuItem(int32 cookie, const char* name,
	const uchar* icon, const uint8* vectorIcon, size_t vectorIconSize)
	: BMenuItem(name, NULL),
	fVectorIcon(NULL),
	fVectorIconSize(0),
	fIcon(BRect(B_ORIGIN, be_control_look->ComposeIconSize(B_MINI_ICON)),
		B_RGBA32),
	fCookie(cookie)
{
	SetIcon(icon, vectorIcon, vectorIconSize);
}


MethodMenuItem::~MethodMenuItem()
{
	free(fVectorIcon);
}


void
MethodMenuItem::SetName(const char *name)
{
	SetLabel(name);
}

void
MethodMenuItem::SetIcon(const uchar *icon, const uint8 *vectorIcon,
	size_t vectorIconSize)
{
	memcpy(fIconBits, icon, sizeof(fIconBits));

	free(fVectorIcon);
	fVectorIcon = NULL;
	fVectorIconSize = 0;
	if (vectorIcon != NULL && vectorIconSize > 0) {
		fVectorIcon = (uint8*)malloc(vectorIconSize);
		if (fVectorIcon != NULL) {
			memcpy(fVectorIcon, vectorIcon, vectorIconSize);
			fVectorIconSize = vectorIconSize;
		}
	}

	GetIcon(&fIcon);
}


status_t
MethodMenuItem::GetIcon(BBitmap *bitmap)
{
	if (fVectorIcon != NULL
		&& BIconUtils::GetVectorIcon(fVectorIcon, fVectorIconSize, bitmap)
			== B_OK) {
		return B_OK;
	}

	return BIconUtils::ConvertFromCMAP8(fIconBits, MENUITEM_ICON_SIZE,
		MENUITEM_ICON_SIZE, MENUITEM_ICON_SIZE, bitmap);
}


void
MethodMenuItem::GetContentSize(float *width, float *height)
{
	float iconSize = fIcon.Bounds().Width() + 1;
	*width = be_plain_font->StringWidth(Label()) + iconSize + 3;

	font_height fheight;
	be_plain_font->GetHeight(&fheight);

	*height = fheight.ascent + fheight.descent + fheight.leading - 2;
	if (*height < iconSize)
		*height = iconSize;
}


void
MethodMenuItem::DrawContent()
{
	BMenu *menu = Menu();
	BPoint contLoc = ContentLocation();

	menu->SetDrawingMode(B_OP_ALPHA);
	menu->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	menu->MovePenTo(contLoc);
	menu->DrawBitmapAsync(&fIcon);
	menu->SetDrawingMode(B_OP_COPY);
	menu->MovePenBy(fIcon.Bounds().Width() + 1 + 3, 2);
	BMenuItem::DrawContent();
}

