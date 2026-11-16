/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


#include "WindowShape.h"

#include <math.h>
#include <stdlib.h>


static const int32 kMaxCornerRadius = 16;
static const int32 kCornerSamples = 8;

// The shadow is a gaussian blur of the window's outline.
static const float kShadowSigma = 6.0f;
static const int32 kShadowMargin = 15;
static const int32 kShadowOffset = 4;
static const uint8 kShadowAlpha = 96;


static uint8* sCoverage[kMaxCornerRadius + 1];
static uint8 sShadowProfile[2 * kShadowMargin + 1];
static bool sShadowProfileValid = false;


WindowShape::WindowShape()
	:
	fHasShadow(false)
{
}


bool
WindowShape::SetTo(const std::vector<Decorator::ShapeRect>& shape,
	const BRegion& fullRegion, bool hasShadow)
{
	if (shape == fShape && hasShadow == fHasShadow && fullRegion == fFullRegion)
		return false;

	fShape = shape;
	fFullRegion = fullRegion;
	fHasShadow = hasShadow;

	fCorners.clear();
	fShadowRects.clear();

	// What is cut off at the corners is what doesn't hide anything behind it.
	BRegion cutOff;
	for (size_t i = 0; i < shape.size(); i++) {
		const Decorator::ShapeRect& rect = shape[i];
		const float radii[4] = { rect.leftTop, rect.rightTop, rect.leftBottom,
			rect.rightBottom };

		for (int32 kind = LEFT_TOP; kind <= RIGHT_BOTTOM; kind++) {
			int32 radius = min_c((int32)radii[kind], kMaxCornerRadius);
			if (radius <= 0)
				continue;

			float left = kind == LEFT_TOP || kind == LEFT_BOTTOM
				? rect.rect.left : rect.rect.right - radius + 1;
			float top = kind == LEFT_TOP || kind == RIGHT_TOP
				? rect.rect.top : rect.rect.bottom - radius + 1;

			Corner corner;
			corner.frame.Set(left, top, left + radius - 1, top + radius - 1);
			corner.radius = radius;
			corner.kind = (corner_kind)kind;
			fCorners.push_back(corner);

			cutOff.Include(corner.frame);
		}
	}

	// A corner can be covered by another rectangle of the shape, like the
	// corner of a frame where it meets the window's tab.
	BRegion covered;
	for (size_t i = 0; i < shape.size(); i++) {
		BRegion opaque(shape[i].rect);
		for (size_t k = 0; k < fCorners.size(); k++) {
			if (shape[i].rect.Contains(fCorners[k].frame))
				opaque.Exclude(fCorners[k].frame);
		}
		covered.Include(&opaque);
	}
	cutOff.Exclude(&covered);

	fEdgeRegion = cutOff;
	fEdgeRegion.IntersectWith(&fullRegion);

	fOpaqueRegion = fullRegion;
	fOpaqueRegion.Exclude(&cutOff);

	fBounds = fullRegion.Frame();
	if (hasShadow && !shape.empty()) {
		fShadowFrame = BRect();
		for (size_t i = 0; i < shape.size(); i++) {
			BRect rect = shape[i].rect;
			rect.OffsetBy(0, kShadowOffset);
			fShadowRects.push_back(rect);
			fShadowFrame = fShadowFrame.IsValid() ? fShadowFrame | rect : rect;
		}
		fShadowFrame.InsetBy(-kShadowMargin, -kShadowMargin);
		fBounds = fBounds | fShadowFrame;
	}

	return true;
}


/*static*/ const uint8*
WindowShape::CoverageFor(int32 radius)
{
	if (radius <= 0 || radius > kMaxCornerRadius)
		return NULL;

	if (sCoverage[radius] != NULL)
		return sCoverage[radius];

	uint8* coverage = (uint8*)malloc(radius * radius);
	if (coverage == NULL)
		return NULL;

	for (int32 y = 0; y < radius; y++) {
		for (int32 x = 0; x < radius; x++) {
			int32 inside = 0;
			for (int32 sampleY = 0; sampleY < kCornerSamples; sampleY++) {
				for (int32 sampleX = 0; sampleX < kCornerSamples; sampleX++) {
					float dx = radius - (x + (sampleX + 0.5f) / kCornerSamples);
					float dy = radius - (y + (sampleY + 0.5f) / kCornerSamples);
					if (dx * dx + dy * dy <= (float)(radius * radius))
						inside++;
				}
			}
			coverage[y * radius + x]
				= inside * 255 / (kCornerSamples * kCornerSamples);
		}
	}

	sCoverage[radius] = coverage;
	return coverage;
}


/*static*/ const uint8*
WindowShape::ShadowProfile()
{
	if (!sShadowProfileValid) {
		for (int32 i = 0; i <= 2 * kShadowMargin; i++) {
			float distance = (float)(i - kShadowMargin);
			float value = 0.5f * (1.0f
				+ erff(distance / (kShadowSigma * (float)M_SQRT2)));
			sShadowProfile[i] = (uint8)(value * 255.0f + 0.5f);
		}
		sShadowProfileValid = true;
	}
	return sShadowProfile;
}


/*static*/ int32
WindowShape::ShadowMargin()
{
	return kShadowMargin;
}


/*static*/ int32
WindowShape::ShadowOffset()
{
	return kShadowOffset;
}


/*static*/ uint8
WindowShape::ShadowAlpha()
{
	return kShadowAlpha;
}
