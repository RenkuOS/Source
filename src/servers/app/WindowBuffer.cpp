/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


#include "WindowBuffer.h"

#include <string.h>
#include <syslog.h>

#include <Accelerant.h>
#include <OS.h>
#include <Region.h>
#include <vm_defs.h>

#include "Compositor.h"
#include "Window.h"


// Windows are resized in many small steps. Memory is allocated in larger ones,
// so that most of these steps don't need to reallocate and copy the window.
static const int32 kCapacityStep = 128;

// A window can be as large as 32768 by 32768 pixels, which would need 4 GB.
// This allows 16384 by 8192, more than two 8K screens side by side.
static const uint64 kMaxBufferSize = 512 * 1024 * 1024;


static inline int32
round_up_capacity(int32 size)
{
	return (size + kCapacityStep - 1) / kCapacityStep * kCapacityStep;
}


WindowBuffer::WindowBuffer()
	:
	fArea(-1),
	fPixels(NULL),
	fCapacityWidth(0),
	fCapacityHeight(0),
	fLeft(0),
	fTop(0),
	fWidth(0),
	fHeight(0),
	fScreenWidth(0),
	fScreenHeight(0),
	fFrameLeft(0),
	fFrameTop(0)
{
}


WindowBuffer::~WindowBuffer()
{
	if (fArea >= 0)
		delete_area(fArea);
}


status_t
WindowBuffer::InitCheck() const
{
	return fPixels != NULL ? B_OK : B_NO_INIT;
}


color_space
WindowBuffer::ColorSpace() const
{
	return B_RGBA32;
}


void*
WindowBuffer::Bits() const
{
	if (HasScreenLayout())
		return fPixels;

	// This address is outside of the allocation, but nothing ever accesses a
	// pixel outside of Frame(): all drawing is clipped to the window.
	return fPixels - (intptr_t)fTop * BytesPerRow() - (intptr_t)fLeft * 4;
}


uint32
WindowBuffer::BytesPerRow() const
{
	return fCapacityWidth * 4;
}


uint32
WindowBuffer::Width() const
{
	// Without memory, there is nothing to draw to.
	if (fPixels == NULL)
		return 0;
	if (HasScreenLayout())
		return fScreenWidth;
	return max_c(fLeft + fWidth, 1);
}


uint32
WindowBuffer::Height() const
{
	if (fPixels == NULL)
		return 0;
	if (HasScreenLayout())
		return fScreenHeight;
	return max_c(fTop + fHeight, 1);
}


bool
WindowBuffer::SetFrame(const BRect& frame)
{
	if (!frame.IsValid())
		return false;

	if (HasScreenLayout()) {
		BRect onScreen = frame & BRect(0, 0, fScreenWidth - 1,
			fScreenHeight - 1);
		if (!onScreen.IsValid())
			onScreen.Set(0, 0, -1, -1);

		bool changed = false;
		if (fPixels == NULL) {
			if (!_AllocateScreen())
				return false;
			changed = true;
		} else if (fWidth > 0 && fHeight > 0) {
			// The window moved: its pixels go along.
			int32 x = (int32)frame.left - fFrameLeft;
			int32 y = (int32)frame.top - fFrameTop;
			if (x != 0 || y != 0)
				_MovePixels(Frame(), x, y);
		}

		fFrameLeft = (int32)frame.left;
		fFrameTop = (int32)frame.top;
		fLeft = (int32)onScreen.left;
		fTop = (int32)onScreen.top;
		fWidth = onScreen.IsValid() ? onScreen.IntegerWidth() + 1 : 0;
		fHeight = onScreen.IsValid() ? onScreen.IntegerHeight() + 1 : 0;
		return changed;
	}

	int32 left = (int32)frame.left;
	int32 top = (int32)frame.top;
	int32 width = frame.IntegerWidth() + 1;
	int32 height = frame.IntegerHeight() + 1;

	if (left == fLeft && top == fTop && width == fWidth && height == fHeight
		&& fPixels != NULL) {
		return false;
	}

	if (width > fCapacityWidth || height > fCapacityHeight) {
		// The old buffer is too small for the window: drawing to it would go
		// past its end. Without a buffer, the window isn't drawn, until it
		// gets one at its next frame change.
		if (!_Reallocate(width, height, false))
			_Free();
	} else if (fPixels != NULL
		&& (int64)round_up_capacity(width) * round_up_capacity(height) * 2
			<= (int64)fCapacityWidth * fCapacityHeight) {
		// A window that has become much smaller gives back what it no longer
		// needs. If that fails, the larger buffer still does.
		_Reallocate(width, height, true);
	}

	fLeft = left;
	fTop = top;
	fWidth = width;
	fHeight = height;
	return true;
}


void
WindowBuffer::Release()
{
	_Free();
	fWidth = 0;
	fHeight = 0;
}


BRect
WindowBuffer::Frame() const
{
	return BRect(fLeft, fTop, fLeft + fWidth - 1, fTop + fHeight - 1);
}


const uint8*
WindowBuffer::PixelAt(int32 x, int32 y) const
{
	if (HasScreenLayout())
		return fPixels + (intptr_t)y * BytesPerRow() + (intptr_t)x * 4;

	return fPixels + (intptr_t)(y - fTop) * BytesPerRow()
		+ (intptr_t)(x - fLeft) * 4;
}


bool
WindowBuffer::SetScreenLayout(int32 width, int32 height)
{
	if (width == fScreenWidth && height == fScreenHeight)
		return false;

	// What was drawn isn't kept: the window draws again.
	Release();
	fScreenWidth = width;
	fScreenHeight = height;
	return true;
}


/*!	Allocates the memory for the screen layout. Only the pages that are
	drawn to are used, so the memory isn't committed up front.
*/
bool
WindowBuffer::_AllocateScreen()
{
	uint8* pixels;
	size_t size = (size_t)fScreenWidth * fScreenHeight * 4;
	area_id area = create_area("window buffer", (void**)&pixels,
		B_ANY_ADDRESS, (size + B_PAGE_SIZE - 1) & ~(size_t)(B_PAGE_SIZE - 1),
		B_NO_LOCK, B_READ_AREA | B_WRITE_AREA | B_OVERCOMMITTING_AREA);
	if (area < 0)
		return false;

	fArea = area;
	fPixels = pixels;
	fCapacityWidth = fScreenWidth;
	fCapacityHeight = fScreenHeight;
	return true;
}


/*!	In the screen layout, moves the pixels of \a from by \a x and \a y, as far
	as they stay on the screen.
*/
void
WindowBuffer::_MovePixels(const BRect& from, int32 x, int32 y)
{
	BRect screen(0, 0, fScreenWidth - 1, fScreenHeight - 1);
	BRect to = from.OffsetByCopy(x, y) & screen;
	if (!to.IsValid())
		return;

	int32 left = (int32)to.left;
	int32 top = (int32)to.top;
	int32 bottom = (int32)to.bottom;
	size_t bytes = (size_t)(to.IntegerWidth() + 1) * 4;
	uint32 bytesPerRow = BytesPerRow();

	// Rows are moved in the order that doesn't overwrite any still to move.
	for (int32 i = 0; i <= bottom - top; i++) {
		int32 row = y > 0 ? bottom - i : top + i;
		memmove(fPixels + (size_t)row * bytesPerRow + left * 4,
			fPixels + (size_t)(row - y) * bytesPerRow + (left - x) * 4, bytes);
	}
}


/*!	Makes room for \a width by \a height pixels, keeping what fits of the
	window. With \a shrink, the buffer can become smaller, otherwise it only
	grows.
*/
bool
WindowBuffer::_Reallocate(int32 width, int32 height, bool shrink)
{
	int32 capacityWidth = round_up_capacity(
		shrink ? width : max_c(width, fCapacityWidth));
	int32 capacityHeight = round_up_capacity(
		shrink ? height : max_c(height, fCapacityHeight));

	// Windows come and go, and can be large. An area of its own gives the
	// memory back to the system when it is no longer needed; the heap would
	// keep it. It also doesn't use any memory until it is drawn to.
	uint64 size = (uint64)capacityHeight * capacityWidth * 4;
	if (size > kMaxBufferSize)
		return false;

	uint8* pixels;
	area_id area = create_area("window buffer", (void**)&pixels,
		B_ANY_ADDRESS, ((size_t)size + B_PAGE_SIZE - 1)
			& ~(size_t)(B_PAGE_SIZE - 1),
		B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (area < 0)
		return false;

	if (fPixels != NULL) {
		size_t bytes = min_c(fWidth, capacityWidth) * 4;
		int32 rows = min_c(fHeight, capacityHeight);
		for (int32 y = 0; y < rows; y++) {
			memcpy(pixels + (size_t)y * capacityWidth * 4,
				fPixels + (size_t)y * fCapacityWidth * 4, bytes);
		}
		delete_area(fArea);
	}

	fArea = area;
	fPixels = pixels;
	fCapacityWidth = capacityWidth;
	fCapacityHeight = capacityHeight;
	return true;
}


void
WindowBuffer::_Free()
{
	if (fArea >= 0)
		delete_area(fArea);
	fArea = -1;
	fPixels = NULL;
	fCapacityWidth = 0;
	fCapacityHeight = 0;
}


// #pragma mark - WindowBufferHWInterface


WindowBufferHWInterface::WindowBufferHWInterface(Compositor* compositor)
	:
	HWInterface(),
	fCompositor(compositor),
	fWindow(NULL)
{
}


WindowBufferHWInterface::~WindowBufferHWInterface()
{
}


/*!	The window lock must be held for writing, so nothing is drawing to the
	buffer, or compositing it.
*/
status_t
WindowBufferHWInterface::SetFrame(const BRect& frame)
{
	if (fBuffer.SetFrame(frame))
		_NotifyFrameBufferChanged();

	status_t status = fBuffer.InitCheck();
	if (status != B_OK && frame.IsValid()) {
		syslog(LOG_ERR, "app_server: no memory for a window buffer of "
			"%" B_PRId32 " x %" B_PRId32 " pixels\n",
			frame.IntegerWidth() + 1, frame.IntegerHeight() + 1);
	}
	return status;
}


void
WindowBufferHWInterface::SetScreenLayout(int32 width, int32 height)
{
	if (fBuffer.SetScreenLayout(width, height))
		_NotifyFrameBufferChanged();
}


status_t
WindowBufferHWInterface::Shutdown()
{
	return B_OK;
}


status_t
WindowBufferHWInterface::SetMode(const display_mode& mode)
{
	return B_UNSUPPORTED;
}


void
WindowBufferHWInterface::GetMode(display_mode* mode)
{
	if (mode != NULL)
		memset(mode, 0, sizeof(display_mode));
}


status_t
WindowBufferHWInterface::GetDeviceInfo(accelerant_device_info* info)
{
	return B_UNSUPPORTED;
}


status_t
WindowBufferHWInterface::GetModeList(display_mode** _modeList, uint32* _count)
{
	return B_UNSUPPORTED;
}


status_t
WindowBufferHWInterface::GetPixelClockLimits(display_mode* mode, uint32* _low,
	uint32* _high)
{
	return B_UNSUPPORTED;
}


status_t
WindowBufferHWInterface::GetTimingConstraints(
	display_timing_constraints* constraints)
{
	return B_UNSUPPORTED;
}


status_t
WindowBufferHWInterface::ProposeMode(display_mode* candidate,
	const display_mode* low, const display_mode* high)
{
	return B_UNSUPPORTED;
}


sem_id
WindowBufferHWInterface::RetraceSemaphore()
{
	return -1;
}


status_t
WindowBufferHWInterface::WaitForRetrace(bigtime_t timeout)
{
	return B_UNSUPPORTED;
}


status_t
WindowBufferHWInterface::SetDPMSMode(uint32 state)
{
	return B_UNSUPPORTED;
}


uint32
WindowBufferHWInterface::DPMSMode()
{
	return 0;
}


uint32
WindowBufferHWInterface::DPMSCapabilities()
{
	return 0;
}


status_t
WindowBufferHWInterface::SetBrightness(float)
{
	return B_UNSUPPORTED;
}


status_t
WindowBufferHWInterface::GetBrightness(float*)
{
	return B_UNSUPPORTED;
}


RenderingBuffer*
WindowBufferHWInterface::FrontBuffer() const
{
	return &fBuffer;
}


RenderingBuffer*
WindowBufferHWInterface::BackBuffer() const
{
	return NULL;
}


bool
WindowBufferHWInterface::IsDoubleBuffered() const
{
	return false;
}


status_t
WindowBufferHWInterface::InvalidateRegion(const BRegion& region)
{
	_Damaged(region);
	return B_OK;
}


status_t
WindowBufferHWInterface::Invalidate(const BRect& frame)
{
	BRegion region(frame);
	_Damaged(region);
	return B_OK;
}


void
WindowBufferHWInterface::_Damaged(const BRegion& region)
{
	fCompositor->Composite(region);
}
