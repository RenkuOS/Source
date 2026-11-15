/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef SCREEN_ROTATION_H
#define SCREEN_ROTATION_H


#include <Accelerant.h>

#include <algorithm>


/*!	Moves the pixel at \a x, \a y of the drawing buffer to where it is on a
	front buffer of \a width by \a height pixels that shows it turned by
	\a degrees clockwise.
*/
static inline void
rotate_point(int32 degrees, int32 width, int32 height, int32& x, int32& y)
{
	int32 oldX = x;
	switch (degrees) {
		case 90:
			x = width - 1 - y;
			y = oldX;
			break;
		case 180:
			x = width - 1 - x;
			y = height - 1 - y;
			break;
		case 270:
			x = y;
			y = height - 1 - oldX;
			break;
	}
}


/*!	The reverse of rotate_point(): moves the pixel at \a x, \a y of the front
	buffer to where it comes from on the drawing buffer.
*/
static inline void
unrotate_point(int32 degrees, int32 width, int32 height, int32& x, int32& y)
{
	int32 oldX = x;
	switch (degrees) {
		case 90:
			x = y;
			y = width - 1 - oldX;
			break;
		case 180:
			x = width - 1 - x;
			y = height - 1 - y;
			break;
		case 270:
			x = height - 1 - y;
			y = oldX;
			break;
	}
}


/*!	Moves the rectangle from \a left, \a top to \a right, \a bottom (all
	included) of the drawing buffer to where it is on the front buffer.
*/
static inline void
rotate_rect(int32 degrees, int32 width, int32 height, int32& left,
	int32& top, int32& right, int32& bottom)
{
	rotate_point(degrees, width, height, left, top);
	rotate_point(degrees, width, height, right, bottom);
	if (left > right)
		std::swap(left, right);
	if (top > bottom)
		std::swap(top, bottom);
}


/*!	Turns \a mode between how the screen is driven and how it is shown when
	it is rotated by \a degrees: for 90 and 270, what is horizontal becomes
	vertical. Doing it twice gives the same mode back.
*/
static inline void
rotate_display_mode(display_mode& mode, int32 degrees)
{
	if (degrees != 90 && degrees != 270)
		return;

	std::swap(mode.virtual_width, mode.virtual_height);
	std::swap(mode.h_display_start, mode.v_display_start);
	std::swap(mode.timing.h_display, mode.timing.v_display);
	std::swap(mode.timing.h_sync_start, mode.timing.v_sync_start);
	std::swap(mode.timing.h_sync_end, mode.timing.v_sync_end);
	std::swap(mode.timing.h_total, mode.timing.v_total);

	uint32 polarity = mode.timing.flags & (B_POSITIVE_HSYNC | B_POSITIVE_VSYNC);
	mode.timing.flags &= ~(B_POSITIVE_HSYNC | B_POSITIVE_VSYNC);
	if ((polarity & B_POSITIVE_HSYNC) != 0)
		mode.timing.flags |= B_POSITIVE_VSYNC;
	if ((polarity & B_POSITIVE_VSYNC) != 0)
		mode.timing.flags |= B_POSITIVE_HSYNC;
}


#endif	// SCREEN_ROTATION_H
