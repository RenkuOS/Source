/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


#include "ScreenEmulation.h"

#include <string.h>

#include <Autolock.h>
#include <ByteOrder.h>
#include <Point.h>

#include <EmulatedScreen.h>

#include "HWInterface.h"
#include "IntRect.h"
#include "RenderingBuffer.h"


// for screens where the driver can't tell when the retrace is
static const bigtime_t kFrameInterval = 16667;


static uint32
bytes_per_pixel(color_space space)
{
	switch (space) {
		case B_CMAP8:
			return 1;
		case B_RGB15:
		case B_RGBA15:
		case B_RGB16:
			return 2;
		case B_RGB32:
		case B_RGBA32:
			return 4;
		default:
			return 0;
	}
}


ScreenEmulation::ScreenEmulation(team_id team)
	:
	fTeam(team),
	fLock("screen emulation"),
	fArea(-1),
	fScreen(NULL),
	fBits(NULL),
	fWidth(0),
	fHeight(0),
	fBytesPerRow(0),
	fSpace(B_NO_COLOR_SPACE),
	fDisplayWidth(0),
	fDisplayHeight(0),
	fInterface(NULL),
	fThread(-1),
	fQuit(false),
	fLayoutLock("screen emulation layout"),
	fFrameWidth(0),
	fFrameHeight(0),
	fShownWidth(0),
	fShownHeight(0)
{
}


ScreenEmulation::~ScreenEmulation()
{
	Stop();
	if (fArea >= 0)
		delete_area(fArea);
}


/*!	Gives the application a new frame buffer of \a width by \a height pixels
	in \a space, of which \a displayWidth by \a displayHeight are shown. The
	application clones Area() to draw to it.
*/
status_t
ScreenEmulation::SetMode(uint32 width, uint32 height, uint32 displayWidth,
	uint32 displayHeight, color_space space)
{
	uint32 bytesPerPixel = bytes_per_pixel(space);
	if (bytesPerPixel == 0)
		return B_BAD_VALUE;
	if (width == 0 || height == 0 || width > 8192 || height > 8192
		|| displayWidth == 0 || displayWidth > width
		|| displayHeight == 0 || displayHeight > height) {
		return B_BAD_VALUE;
	}

	uint32 bytesPerRow = (width * bytesPerPixel + 15) & ~15;
	uint32 offset = (sizeof(emulated_screen) + B_PAGE_SIZE - 1)
		& ~(B_PAGE_SIZE - 1);
	size_t size = (offset + (size_t)bytesPerRow * height + B_PAGE_SIZE - 1)
		& ~(B_PAGE_SIZE - 1);

	emulated_screen* screen;
	area_id area = create_area("emulated screen", (void**)&screen,
		B_ANY_ADDRESS, size, B_NO_LOCK,
		B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
	if (area < 0)
		return area;

	// The palette starts black.
	memset((void*)screen, 0, offset);
	screen->offset = offset;
	screen->width = width;
	screen->height = height;
	screen->bytes_per_row = bytesPerRow;
	screen->space = space;
	screen->display_width = displayWidth;
	screen->display_height = displayHeight;

	BAutolock _(fLock);

	// The application keeps its own clone of the old one until it has
	// cloned the new one.
	if (fArea >= 0)
		delete_area(fArea);
	fArea = area;
	fScreen = screen;
	fBits = (const uint8*)screen + offset;
	fWidth = width;
	fHeight = height;
	fBytesPerRow = bytesPerRow;
	fSpace = space;
	fDisplayWidth = displayWidth;
	fDisplayHeight = displayHeight;

	return B_OK;
}


void
ScreenEmulation::Start(::HWInterface* interface)
{
	if (fThread >= 0)
		return;

	fInterface = interface;
	fQuit = false;
	{
		BAutolock _(fLayoutLock);
		fFrameWidth = 0;
		fFrameHeight = 0;
	}

	fThread = spawn_thread(&_ThreadEntry, "screen emulation",
		B_DISPLAY_PRIORITY, this);
	if (fThread >= 0)
		resume_thread(fThread);
}


void
ScreenEmulation::Stop()
{
	if (fThread < 0)
		return;

	fQuit = true;
	status_t result;
	wait_for_thread(fThread, &result);
	fThread = -1;
}


/*!	Moves \a point on the screen to where it is on the frame buffer, as that
	is shown. The frame buffer's edges go on to the screen's. Returns false
	before anything has been shown.
*/
bool
ScreenEmulation::MapPoint(BPoint& point)
{
	BAutolock _(fLayoutLock);
	if (fFrameWidth == 0)
		return false;

	point.x = emulated_screen_position(point.x, fLayout.left, fLayout.width,
		fShownWidth);
	point.y = emulated_screen_position(point.y, fLayout.top, fLayout.height,
		fShownHeight);
	return true;
}


/*static*/ status_t
ScreenEmulation::_ThreadEntry(void* data)
{
	((ScreenEmulation*)data)->_Run();
	return B_OK;
}


void
ScreenEmulation::_Run()
{
	bigtime_t nextFrame = system_time();

	while (!fQuit) {
		status_t status = fInterface->WaitForRetrace(kFrameInterval);
		if (status != B_OK && status != B_TIMED_OUT) {
			nextFrame += kFrameInterval;
			if (nextFrame < system_time())
				nextFrame = system_time();
			snooze_until(nextFrame, B_SYSTEM_TIMEBASE);
		}

		if (!fQuit)
			_Present();
	}
}


/*!	Puts the shown part of the frame buffer on the screen, as large as it
	fits without changing its shape, with black around it.
*/
void
ScreenEmulation::_Present()
{
	BAutolock _(fLock);
	if (fScreen == NULL || !fInterface->LockParallelAccess())
		return;

	RenderingBuffer* drawing = fInterface->DrawingBuffer();
	const int32 screenWidth = drawing->Width();
	const int32 screenHeight = drawing->Height();

	// work out where it goes when the screen or the mode has changed
	bool relaidOut = screenWidth != fFrameWidth
		|| screenHeight != fFrameHeight || fDisplayWidth != fShownWidth
		|| fDisplayHeight != fShownHeight;
	if (relaidOut) {
		emulated_screen_layout layout = layout_emulated_screen(screenWidth,
			screenHeight, fDisplayWidth, fDisplayHeight);

		fFrame.assign((size_t)screenWidth * screenHeight, 0xff000000);
		fColumns.resize(layout.width);
		for (int32 x = 0; x < layout.width; x++) {
			fColumns[x] = emulated_screen_source(x, layout.width,
				fDisplayWidth);
		}

		BAutolock layoutLocker(fLayoutLock);
		fFrameWidth = screenWidth;
		fFrameHeight = screenHeight;
		fShownWidth = fDisplayWidth;
		fShownHeight = fDisplayHeight;
		fLayout = layout;
	}

	const int32 left = fLayout.left;
	const int32 top = fLayout.top;
	const int32 width = fLayout.width;
	const int32 height = fLayout.height;

	// The application can change what it shares with us at any time, so
	// each of it is read once.
	if (fSpace == B_CMAP8) {
		for (int32 i = 0; i < 256; i++) {
			const rgb_color color = fScreen->palette[i];
			fPalette[i] = 0xff000000 | ((uint32)color.red << 16)
				| ((uint32)color.green << 8) | color.blue;
		}
	}

	// The application may have moved what is shown anywhere.
	int32 displayX = *(volatile int32*)&fScreen->display_x;
	int32 displayY = *(volatile int32*)&fScreen->display_y;
	displayX = max_c(0, min_c(displayX, (int32)(fWidth - fDisplayWidth)));
	displayY = max_c(0, min_c(displayY, (int32)(fHeight - fDisplayHeight)));

	const uint32 bytesPerPixel = bytes_per_pixel(fSpace);
	for (int32 y = 0; y < height; y++) {
		int32 sourceY = displayY
			+ emulated_screen_source(y, height, fDisplayHeight);
		const uint8* source = fBits + (size_t)sourceY * fBytesPerRow
			+ displayX * bytesPerPixel;
		_ConvertRow(source, &fFrame[(size_t)(top + y) * screenWidth + left],
			width);
	}

	IntRect area = relaidOut ? IntRect(0, 0, screenWidth - 1, screenHeight - 1)
		: IntRect(left, top, left + width - 1, top + height - 1);
	fInterface->CopyToScreen((const uint8*)&fFrame[0], screenWidth * 4,
		area);

	fInterface->UnlockParallelAccess();
}


/*!	Converts \a count pixels for the screen, from those of the row at
	\a source that fColumns picks.
*/
void
ScreenEmulation::_ConvertRow(const uint8* source, uint32* target,
	int32 count) const
{
	switch (fSpace) {
		case B_CMAP8:
			for (int32 x = 0; x < count; x++)
				target[x] = fPalette[source[fColumns[x]]];
			break;

		case B_RGB16:
			for (int32 x = 0; x < count; x++) {
				uint32 pixel = B_LENDIAN_TO_HOST_INT16(
					((const uint16*)source)[fColumns[x]]);
				uint32 red = (pixel >> 11) & 0x1f;
				uint32 green = (pixel >> 5) & 0x3f;
				uint32 blue = pixel & 0x1f;
				target[x] = 0xff000000 | (((red << 3) | (red >> 2)) << 16)
					| (((green << 2) | (green >> 4)) << 8)
					| ((blue << 3) | (blue >> 2));
			}
			break;

		case B_RGB15:
		case B_RGBA15:
			for (int32 x = 0; x < count; x++) {
				uint32 pixel = B_LENDIAN_TO_HOST_INT16(
					((const uint16*)source)[fColumns[x]]);
				uint32 red = (pixel >> 10) & 0x1f;
				uint32 green = (pixel >> 5) & 0x1f;
				uint32 blue = pixel & 0x1f;
				target[x] = 0xff000000 | (((red << 3) | (red >> 2)) << 16)
					| (((green << 3) | (green >> 2)) << 8)
					| ((blue << 3) | (blue >> 2));
			}
			break;

		default:
			for (int32 x = 0; x < count; x++)
				target[x] = ((const uint32*)source)[fColumns[x]] | 0xff000000;
			break;
	}
}
