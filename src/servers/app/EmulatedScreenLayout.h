/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef EMULATED_SCREEN_LAYOUT_H
#define EMULATED_SCREEN_LAYOUT_H


#include <math.h>

#include <SupportDefs.h>


/*!	Where an emulated screen is shown on the real one: as large as it fits
	without changing its shape, in the middle.
*/
struct emulated_screen_layout {
	int32	left;
	int32	top;
	int32	width;
	int32	height;
};


/*!	The layout of a frame buffer that shows \a displayWidth by
	\a displayHeight pixels on a screen of \a screenWidth by \a screenHeight.
*/
static inline emulated_screen_layout
layout_emulated_screen(int32 screenWidth, int32 screenHeight,
	uint32 displayWidth, uint32 displayHeight)
{
	emulated_screen_layout layout;
	layout.width = screenWidth;
	layout.height = (int32)((int64)screenWidth * displayHeight / displayWidth);
	if (layout.height > screenHeight) {
		layout.height = screenHeight;
		layout.width = (int32)((int64)screenHeight * displayWidth
			/ displayHeight);
	}
	layout.width = max_c(layout.width, 1);
	layout.height = max_c(layout.height, 1);
	layout.left = (screenWidth - layout.width) / 2;
	layout.top = (screenHeight - layout.height) / 2;
	return layout;
}


/*!	The pixel of the frame buffer, along one axis of \a displaySize pixels,
	that is shown at \a position of the \a shownSize pixels it is scaled to.
*/
static inline int32
emulated_screen_source(int32 position, int32 shownSize, uint32 displaySize)
{
	return (int32)((int64)position * displaySize / shownSize);
}


/*!	Where \a position on the screen is on the frame buffer, along the axis
	on which it is shown from \a start for \a shownSize pixels: the pixel
	shown there, or the nearest edge outside of it.
*/
static inline float
emulated_screen_position(float position, int32 start, int32 shownSize,
	uint32 displaySize)
{
	int32 offset = (int32)floorf(position) - start;
	if (offset < 0)
		return 0;
	if (offset >= shownSize)
		return displaySize - 1;
	return emulated_screen_source(offset, shownSize, displaySize);
}


#endif	// EMULATED_SCREEN_LAYOUT_H
