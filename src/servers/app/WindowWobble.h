/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDOW_WOBBLE_H
#define WINDOW_WOBBLE_H


#include <OS.h>
#include <Rect.h>


/*!	The springs that make a window wobble while it is dragged.

	A grid of 4 x 4 points is spread over the window. Each is tied by springs
	to its neighbours and to its place on the window. The point closest to
	where the window was grabbed moves with it, and the others follow it.
	The points are the control points of a Bézier patch, which the window is
	drawn along.
*/
class WindowWobble {
public:
								WindowWobble();

			// Starts over, with the window at rest. The wobbliness goes from
			// 0 to 100.
			void				SetTo(const BRect& frame, BPoint grab,
									uint8 wobbliness, bigtime_t time);

			// The window is grabbed again, before it has come to rest.
			void				Grab(BPoint where, uint8 wobbliness);
			void				Release();
			bool				IsGrabbed() const { return fAnchor >= 0; }

			// Where the window is now. Returns false if it has changed size:
			// then Resize() has to be used.
			bool				SetFrame(const BRect& frame);
			// The window has a new size. The points stay where they are on
			// the screen, and the springs pull them into the new frame.
			void				Resize(const BRect& frame);
			// It has been zoomed: it settles with less of a bounce, as the
			// whole window moves a long way.
			void				Zoomed();
			const BRect&		Frame() const { return fFrame; }

			// Moves the springs on to the given time. Returns false once the
			// window has been let go and has come to rest.
			bool				Step(bigtime_t time);

			// The part of the screen the window covers.
			BRect				Bounds() const;
			// Whether it is less than half a pixel away from its frame, so
			// that it can be drawn as it is.
			bool				IsFlat() const;

			// Where the point of the window at the share u, v of its width
			// and height is drawn.
			BPoint				PointAt(float u, float v) const;

	static	const int32			kGridSize = 4;
	static	const int32			kPointCount = kGridSize * kGridSize;

private:
			void				_SetWobbliness(uint8 wobbliness);
			int32				_PointClosestTo(BPoint where) const;
			void				_StepOnce(float seconds);

			BRect				fFrame;
			float				fWidth;
			float				fHeight;

			// How far each point is from its place on the window, and how
			// fast it moves, in pixels and pixels per second
			float				fOffsetX[kPointCount];
			float				fOffsetY[kPointCount];
			float				fVelocityX[kPointCount];
			float				fVelocityY[kPointCount];

			// The point that moves with the window, or -1 once it is let go
			int32				fAnchor;
			bigtime_t			fTime;

			float				fSpring;
			float				fHome;
			float				fFriction;
			float				fFrictionScale;
};


#endif	// WINDOW_WOBBLE_H
