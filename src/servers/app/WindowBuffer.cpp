/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


#include "WindowBuffer.h"

#include <string.h>

#include <Accelerant.h>
#include <OS.h>
#include <Region.h>

#include "Compositor.h"
#include "Window.h"


// Windows are resized in many small steps. Memory is allocated in larger ones,
// so that most of these steps don't need to reallocate and copy the window.
static const int32 kCapacityStep = 128;


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
	fHeight(0)
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
	return max_c(fLeft + fWidth, 1);
}


uint32
WindowBuffer::Height() const
{
	return max_c(fTop + fHeight, 1);
}


bool
WindowBuffer::SetFrame(const BRect& frame)
{
	if (!frame.IsValid())
		return false;

	int32 left = (int32)frame.left;
	int32 top = (int32)frame.top;
	int32 width = frame.IntegerWidth() + 1;
	int32 height = frame.IntegerHeight() + 1;

	if (left == fLeft && top == fTop && width == fWidth && height == fHeight)
		return false;

	if (width > fCapacityWidth || height > fCapacityHeight) {
		if (!_Reallocate(width, height, false))
			return false;
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
	if (fArea >= 0)
		delete_area(fArea);

	fArea = -1;
	fPixels = NULL;
	fCapacityWidth = 0;
	fCapacityHeight = 0;
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
	return fPixels + (intptr_t)(y - fTop) * BytesPerRow()
		+ (intptr_t)(x - fLeft) * 4;
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
	uint8* pixels;
	size_t size = (size_t)capacityHeight * capacityWidth * 4;
	area_id area = create_area("window buffer", (void**)&pixels,
		B_ANY_ADDRESS, (size + B_PAGE_SIZE - 1) & ~(size_t)(B_PAGE_SIZE - 1),
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
void
WindowBufferHWInterface::SetFrame(const BRect& frame)
{
	if (fBuffer.SetFrame(frame))
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

	// An application with direct access draws to the screen on its own. What
	// the window draws must get there first, as it isn't redrawn on top.
	if (fWindow != NULL && fWindow->ServerWindow()->IsDirectlyAccessing())
		fCompositor->FlushLocked();
}
