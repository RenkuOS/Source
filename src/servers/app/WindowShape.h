/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */
#ifndef WINDOW_SHAPE_H
#define WINDOW_SHAPE_H


#include <vector>

#include <Rect.h>
#include <Region.h>

#include "Decorator.h"


/*!	What the compositor needs to know about the outline of a window: which of
	its pixels hide what is behind them, which ones are blended with it, and
	where its shadow falls.
*/
class WindowShape {
public:
			enum corner_kind {
				LEFT_TOP,
				RIGHT_TOP,
				LEFT_BOTTOM,
				RIGHT_BOTTOM
			};

			struct Corner {
				BRect			frame;
				int32			radius;
				corner_kind		kind;
			};

								WindowShape();

			// Returns false if there was nothing to do.
			bool				SetTo(
									const std::vector<Decorator::ShapeRect>& shape,
									const BRegion& fullRegion, bool hasShadow);

			const BRegion&		OpaqueRegion() const { return fOpaqueRegion; }
			const BRegion&		EdgeRegion() const { return fEdgeRegion; }
			const std::vector<Corner>&
								Corners() const { return fCorners; }

			bool				HasShadow() const { return fHasShadow; }
			const std::vector<BRect>&
								ShadowRects() const { return fShadowRects; }
			const BRect&		ShadowFrame() const { return fShadowFrame; }

			// Everything the window has an effect on.
			const BRect&		Bounds() const { return fBounds; }

	// The share of each pixel of a corner that belongs to the window, 0 - 255.
	// The radius * radius pixels are for the left top corner.
	static	const uint8*		CoverageFor(int32 radius);

	// How dark the shadow of one edge is, from the outside of the shadow to
	// the inside, in 2 * ShadowMargin() + 1 steps.
	static	const uint8*		ShadowProfile();
	static	int32				ShadowMargin();
	static	int32				ShadowOffset();
	static	uint8				ShadowAlpha();

private:
			std::vector<Decorator::ShapeRect>
								fShape;
			BRegion				fFullRegion;
			bool				fHasShadow;

			BRegion				fOpaqueRegion;
			BRegion				fEdgeRegion;
			std::vector<Corner>	fCorners;
			std::vector<BRect>	fShadowRects;
			BRect				fShadowFrame;
			BRect				fBounds;
};


#endif	// WINDOW_SHAPE_H
