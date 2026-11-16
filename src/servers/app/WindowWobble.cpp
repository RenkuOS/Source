/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */


#include "WindowWobble.h"

#include <math.h>


// The springs are moved on in steps of this many seconds, and never more
// than kMaxElapsed at once, so a slow frame doesn't throw them off.
static const float kStep = 0.002f;
static const float kMaxElapsed = 0.05f;

// How stiff the springs are, and how much they are slowed down, from the
// least wobbly window to the most wobbly one
static const float kStiffSpring = 3000.0f;
static const float kLooseSpring = 400.0f;
static const float kStiffFriction = 25.0f;
static const float kLooseFriction = 6.0f;
// A zoomed window is slowed down more.
static const float kZoomFrictionScale = 3.0f;
// The pull of each point back to its place on the window, against that
// of its neighbours
static const float kHomeShare = 0.2f;

// No point lags further behind than this
static const float kMaxOffset = 400.0f;

// Below this the window is at rest.
static const float kRestOffset = 0.3f;
static const float kRestVelocity = 4.0f;


static inline float
bernstein(int32 index, float t)
{
	float s = 1.0f - t;
	switch (index) {
		case 0:
			return s * s * s;
		case 1:
			return 3.0f * t * s * s;
		case 2:
			return 3.0f * t * t * s;
		default:
			return t * t * t;
	}
}


WindowWobble::WindowWobble()
	:
	fWidth(0),
	fHeight(0),
	fAnchor(-1),
	fTime(0),
	fSpring(0),
	fHome(0),
	fFriction(0),
	fFrictionScale(1)
{
}


void
WindowWobble::SetTo(const BRect& frame, BPoint grab, uint8 wobbliness,
	bigtime_t time)
{
	fFrame = frame;
	fWidth = frame.Width() + 1;
	fHeight = frame.Height() + 1;
	fTime = time;

	for (int32 i = 0; i < kPointCount; i++) {
		fOffsetX[i] = fOffsetY[i] = 0;
		fVelocityX[i] = fVelocityY[i] = 0;
	}

	Grab(grab, wobbliness);
}


void
WindowWobble::Grab(BPoint where, uint8 wobbliness)
{
	_SetWobbliness(wobbliness);
	fFrictionScale = 1;

	// It stays where it is, and moves with the window from now on.
	fAnchor = _PointClosestTo(where);
	fVelocityX[fAnchor] = fVelocityY[fAnchor] = 0;
}


void
WindowWobble::Release()
{
	fAnchor = -1;
}


bool
WindowWobble::SetFrame(const BRect& frame)
{
	if (frame.Width() + 1 != fWidth || frame.Height() + 1 != fHeight)
		return false;

	// The points stay where they were on the screen, but the one that has
	// been grabbed.
	float deltaX = frame.left - fFrame.left;
	float deltaY = frame.top - fFrame.top;
	if (deltaX == 0 && deltaY == 0)
		return true;

	fFrame = frame;
	for (int32 i = 0; i < kPointCount; i++) {
		if (i == fAnchor)
			continue;

		fOffsetX[i] -= deltaX;
		fOffsetY[i] -= deltaY;
		if (fOffsetX[i] > kMaxOffset)
			fOffsetX[i] = kMaxOffset;
		else if (fOffsetX[i] < -kMaxOffset)
			fOffsetX[i] = -kMaxOffset;
		if (fOffsetY[i] > kMaxOffset)
			fOffsetY[i] = kMaxOffset;
		else if (fOffsetY[i] < -kMaxOffset)
			fOffsetY[i] = -kMaxOffset;
	}
	return true;
}


void
WindowWobble::Resize(const BRect& frame)
{
	float width = frame.Width() + 1;
	float height = frame.Height() + 1;

	for (int32 row = 0; row < kGridSize; row++) {
		for (int32 column = 0; column < kGridSize; column++) {
			int32 i = row * kGridSize + column;
			float x = fFrame.left + fWidth * column / (kGridSize - 1)
				+ fOffsetX[i];
			float y = fFrame.top + fHeight * row / (kGridSize - 1)
				+ fOffsetY[i];
			fOffsetX[i] = x - (frame.left + width * column / (kGridSize - 1));
			fOffsetY[i] = y - (frame.top + height * row / (kGridSize - 1));
		}
	}

	fFrame = frame;
	fWidth = width;
	fHeight = height;
}


void
WindowWobble::Zoomed()
{
	fFrictionScale = kZoomFrictionScale;
}


bool
WindowWobble::Step(bigtime_t time)
{
	float elapsed = (time - fTime) / 1000000.0f;
	fTime = time;
	if (elapsed > kMaxElapsed)
		elapsed = kMaxElapsed;

	while (elapsed > 0) {
		_StepOnce(elapsed < kStep ? elapsed : kStep);
		elapsed -= kStep;
	}

	if (fAnchor >= 0)
		return true;

	for (int32 i = 0; i < kPointCount; i++) {
		if (fabsf(fOffsetX[i]) > kRestOffset || fabsf(fOffsetY[i]) > kRestOffset
			|| fabsf(fVelocityX[i]) > kRestVelocity
			|| fabsf(fVelocityY[i]) > kRestVelocity) {
			return true;
		}
	}
	return false;
}


BRect
WindowWobble::Bounds() const
{
	// The patch lies within its control points.
	float left = fFrame.left;
	float top = fFrame.top;
	float right = fFrame.left + fWidth;
	float bottom = fFrame.top + fHeight;

	for (int32 row = 0; row < kGridSize; row++) {
		for (int32 column = 0; column < kGridSize; column++) {
			int32 i = row * kGridSize + column;
			float x = fFrame.left + fWidth * column / (kGridSize - 1)
				+ fOffsetX[i];
			float y = fFrame.top + fHeight * row / (kGridSize - 1)
				+ fOffsetY[i];
			if (x < left)
				left = x;
			if (x > right)
				right = x;
			if (y < top)
				top = y;
			if (y > bottom)
				bottom = y;
		}
	}

	return BRect(floorf(left), floorf(top), ceilf(right), ceilf(bottom));
}


bool
WindowWobble::IsFlat() const
{
	// A point of the patch is never further away than its control points.
	for (int32 i = 0; i < kPointCount; i++) {
		if (fabsf(fOffsetX[i]) >= 0.5f || fabsf(fOffsetY[i]) >= 0.5f)
			return false;
	}
	return true;
}


BPoint
WindowWobble::PointAt(float u, float v) const
{
	// Where the points are at rest, the patch is the window itself, so only
	// their offsets need to be added up.
	float x = fFrame.left + u * fWidth;
	float y = fFrame.top + v * fHeight;

	for (int32 row = 0; row < kGridSize; row++) {
		float rowWeight = bernstein(row, v);
		for (int32 column = 0; column < kGridSize; column++) {
			float weight = rowWeight * bernstein(column, u);
			int32 i = row * kGridSize + column;
			x += weight * fOffsetX[i];
			y += weight * fOffsetY[i];
		}
	}

	return BPoint(x, y);
}


void
WindowWobble::_SetWobbliness(uint8 wobbliness)
{
	float share = (wobbliness > 100 ? 100 : wobbliness) / 100.0f;
	// Each step of the slider changes them by the same factor.
	fSpring = kStiffSpring * powf(kLooseSpring / kStiffSpring, share);
	fFriction = kStiffFriction * powf(kLooseFriction / kStiffFriction, share);
	fHome = fSpring * kHomeShare;
}


int32
WindowWobble::_PointClosestTo(BPoint where) const
{
	int32 column = (int32)roundf((where.x - fFrame.left) / fWidth
		* (kGridSize - 1));
	int32 row = (int32)roundf((where.y - fFrame.top) / fHeight
		* (kGridSize - 1));
	column = column < 0 ? 0 : column >= kGridSize ? kGridSize - 1 : column;
	row = row < 0 ? 0 : row >= kGridSize ? kGridSize - 1 : row;
	return row * kGridSize + column;
}


void
WindowWobble::_StepOnce(float seconds)
{
	float forceX[kPointCount];
	float forceY[kPointCount];
	const float friction = fFriction * fFrictionScale;

	for (int32 row = 0; row < kGridSize; row++) {
		for (int32 column = 0; column < kGridSize; column++) {
			int32 i = row * kGridSize + column;
			float x = -fHome * fOffsetX[i] - friction * fVelocityX[i];
			float y = -fHome * fOffsetY[i] - friction * fVelocityY[i];

			const int32 neighbours[4][2] = {
				{ row - 1, column }, { row + 1, column },
				{ row, column - 1 }, { row, column + 1 }
			};
			for (int32 k = 0; k < 4; k++) {
				int32 otherRow = neighbours[k][0];
				int32 otherColumn = neighbours[k][1];
				if (otherRow < 0 || otherRow >= kGridSize || otherColumn < 0
					|| otherColumn >= kGridSize) {
					continue;
				}
				int32 other = otherRow * kGridSize + otherColumn;
				x += fSpring * (fOffsetX[other] - fOffsetX[i]);
				y += fSpring * (fOffsetY[other] - fOffsetY[i]);
			}

			forceX[i] = x;
			forceY[i] = y;
		}
	}

	for (int32 i = 0; i < kPointCount; i++) {
		if (i == fAnchor)
			continue;

		fVelocityX[i] += forceX[i] * seconds;
		fVelocityY[i] += forceY[i] * seconds;
		fOffsetX[i] += fVelocityX[i] * seconds;
		fOffsetY[i] += fVelocityY[i] * seconds;
	}
}
