// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~
//
//	Copyright (c) 2004, Haiku
//
//  This software is part of the Haiku distribution and is covered
//  by the Haiku license.
//
//
//  File:			MethodMenuItem.h
//  Authors:		Jérôme Duval,
//
//  Description:	Input Server
//  Created:		October 19, 2004
//
// ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~ ~

#ifndef METHOD_MENUITEM_H_
#define METHOD_MENUITEM_H_

#include <Bitmap.h>
#include <MenuItem.h>

#define MENUITEM_ICON_SIZE 16

class MethodMenuItem : public BMenuItem {
	public:
		MethodMenuItem(int32 cookie, const char *label, const uchar *icon,
			const uint8 *vectorIcon, size_t vectorIconSize, BMenu *subMenu,
			BMessenger &messenger);
		MethodMenuItem(int32 cookie, const char *label, const uchar *icon,
			const uint8 *vectorIcon, size_t vectorIconSize);

		virtual ~MethodMenuItem();

		virtual void DrawContent();
		virtual void GetContentSize(float *width, float *height);

		void SetName(const char *name);
		const char *Name() { return Label(); };

		void SetIcon(const uchar *icon, const uint8 *vectorIcon,
			size_t vectorIconSize);
		const uchar *Icon() { return fIconBits; };
		const uint8 *VectorIcon() { return fVectorIcon; };
		size_t VectorIconSize() { return fVectorIconSize; };
		// draws the icon into a bitmap of any size
		status_t GetIcon(BBitmap *bitmap);

		int32 Cookie() { return fCookie; };
	private:
		// what the method gave: 16x16 B_CMAP8, and a vector icon if it has one
		uchar fIconBits[MENUITEM_ICON_SIZE * MENUITEM_ICON_SIZE];
		uint8 *fVectorIcon;
		size_t fVectorIconSize;
		// as drawn in the menu
		BBitmap fIcon;
		int32 fCookie;
		BMessenger fMessenger;
};

#endif
