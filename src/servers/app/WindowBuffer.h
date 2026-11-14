/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDOW_BUFFER_H
#define WINDOW_BUFFER_H


#include <OS.h>

#include "HWInterface.h"
#include "RenderingBuffer.h"


class Compositor;


/*!	The off-screen pixels of one window, used when the desktop is composited.

	All drawing in app_server uses screen coordinates, so that is what this
	buffer presents to the Painter and to the DrawingEngine: Bits() is where
	the screen's left top pixel would be, if the buffer extended that far.
	Only the window's own frame is backed by memory, and moving the window just
	moves that frame.

	The pixels left of or above the screen can't be addressed this way. Drawing
	must be clipped to positive coordinates, so these parts of a window are not
	drawn, and need a redraw when they are moved into view.
*/
class WindowBuffer : public RenderingBuffer {
public:
								WindowBuffer();
	virtual						~WindowBuffer();

	virtual	status_t			InitCheck() const;

	virtual	color_space			ColorSpace() const;
	virtual	void*				Bits() const;
	virtual	uint32				BytesPerRow() const;
	virtual	uint32				Width() const;
	virtual	uint32				Height() const;

			// Returns true if Bits(), Width() or Height() have changed.
			bool				SetFrame(const BRect& frame);
			BRect				Frame() const;

			void				Release();

			// Memory address of a pixel inside of Frame(), in screen
			// coordinates.
			const uint8*		PixelAt(int32 x, int32 y) const;

private:
			bool				_Reallocate(int32 width, int32 height,
									bool shrink);

private:
			area_id				fArea;
			uint8*				fPixels;
			int32				fCapacityWidth;
			int32				fCapacityHeight;
			int32				fLeft;
			int32				fTop;
			int32				fWidth;
			int32				fHeight;
};


/*!	Lets a window's DrawingEngine draw to its WindowBuffer, and tells the
	Compositor about what was drawn.
*/
class WindowBufferHWInterface : public HWInterface {
public:
								WindowBufferHWInterface(Compositor* compositor);
	virtual						~WindowBufferHWInterface();

			void				SetFrame(const BRect& frame);
			void				ReleaseBuffer() { fBuffer.Release(); }
			const WindowBuffer&	Buffer() const { return fBuffer; }

	virtual	status_t			Shutdown();

	virtual	status_t			SetMode(const display_mode& mode);
	virtual	void				GetMode(display_mode* mode);

	virtual status_t			GetDeviceInfo(accelerant_device_info* info);
	virtual status_t			GetModeList(display_mode** _modeList,
									uint32* _count);
	virtual status_t			GetPixelClockLimits(display_mode* mode,
									uint32* _low, uint32* _high);
	virtual status_t			GetTimingConstraints(display_timing_constraints*
									constraints);
	virtual status_t			ProposeMode(display_mode* candidate,
									const display_mode* low,
									const display_mode* high);

	virtual sem_id				RetraceSemaphore();
	virtual status_t			WaitForRetrace(
									bigtime_t timeout = B_INFINITE_TIMEOUT);

	virtual status_t			SetDPMSMode(uint32 state);
	virtual uint32				DPMSMode();
	virtual uint32				DPMSCapabilities();

	virtual status_t			SetBrightness(float);
	virtual status_t			GetBrightness(float*);

	virtual	RenderingBuffer*	FrontBuffer() const;
	virtual	RenderingBuffer*	BackBuffer() const;
	virtual	bool				IsDoubleBuffered() const;

	virtual	status_t			InvalidateRegion(const BRegion& region);
	virtual	status_t			Invalidate(const BRect& frame);

private:
			Compositor*			fCompositor;
	mutable	WindowBuffer		fBuffer;
};


#endif	// WINDOW_BUFFER_H
