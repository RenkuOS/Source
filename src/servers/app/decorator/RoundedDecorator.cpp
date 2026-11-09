/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


#include "RoundedDecorator.h"

#include <AffineTransform.h>

#include <WindowPrivate.h>

#include "Desktop.h"
#include "DesktopSettings.h"
#include "DrawingEngine.h"


RoundedDecorator::~RoundedDecorator()
{
}


/*static*/ bool
RoundedDecorator::IsActive(Desktop* desktop)
{
	return desktop != NULL && desktop->GetCompositor() != NULL
		&& DesktopSettings(desktop).CompositedRoundedCorners();
}


/*static*/ void
RoundedDecorator::RoundShape(std::vector<Decorator::ShapeRect>& shape,
	window_look look, int32 frameRadius, int32 tabRadius)
{
	if (look != B_TITLED_WINDOW_LOOK && look != B_DOCUMENT_WINDOW_LOOK
		&& look != B_MODAL_WINDOW_LOOK && look != B_FLOATING_WINDOW_LOOK
		&& look != kLeftTitledWindowLook) {
		return;
	}

	Decorator::ShapeRect& frame = shape[0];
	const BRect border = frame.rect;
	frame.leftTop = frame.rightTop = frame.leftBottom = frame.rightBottom
		= frameRadius;

	for (size_t i = 1; i < shape.size(); i++) {
		Decorator::ShapeRect& tab = shape[i];

		// A title bar inside the frame has no corners of its own to round.
		if (border.Contains(tab.rect))
			continue;

		tab.leftTop = tabRadius;

		// The frame keeps its corner where a tab grows out of it.
		if (look == kLeftTitledWindowLook) {
			tab.leftBottom = tabRadius;
			if (tab.rect.top <= border.top + frameRadius)
				frame.leftTop = 0;
			if (tab.rect.bottom >= border.bottom - frameRadius)
				frame.leftBottom = 0;
		} else {
			tab.rightTop = tabRadius;
			if (tab.rect.left <= border.left + frameRadius)
				frame.leftTop = 0;
			if (tab.rect.right >= border.right - frameRadius)
				frame.rightTop = 0;
		}
	}
}


/*static*/ void
RoundedDecorator::StrokeCorners(DrawingEngine* engine,
	const Decorator::ShapeRect& shape, rgb_color leftTop, rgb_color rightTop,
	rgb_color leftBottom, rgb_color rightBottom)
{
	const BRect& rect = shape.rect;
	const int32 lt = (int32)shape.leftTop;
	const int32 rt = (int32)shape.rightTop;
	const int32 lb = (int32)shape.leftBottom;
	const int32 rb = (int32)shape.rightBottom;

	if (lt > 0) {
		StrokeCorner(engine, rect.LeftTop(), lt, WindowShape::LEFT_TOP, leftTop);
	}
	if (rt > 0) {
		StrokeCorner(engine, BPoint(rect.right - rt + 1, rect.top), rt,
			WindowShape::RIGHT_TOP, rightTop);
	}
	if (lb > 0) {
		StrokeCorner(engine, BPoint(rect.left, rect.bottom - lb + 1), lb,
			WindowShape::LEFT_BOTTOM, leftBottom);
	}
	if (rb > 0) {
		StrokeCorner(engine, BPoint(rect.right - rb + 1, rect.bottom - rb + 1),
			rb, WindowShape::RIGHT_BOTTOM, rightBottom);
	}
}


/*!	Draws the line of the border along a rounded corner. What a line through
	the corner would do to the color fringes of subpixel antialiasing is avoided
	by drawing it from what the compositor cuts off: a pixel is mixed with the
	line color by how much of the pixel's visible part belongs to the line.
*/
/*static*/ void
RoundedDecorator::StrokeCorner(DrawingEngine* engine, BPoint leftTop,
	int32 radius, WindowShape::corner_kind kind, rgb_color color)
{
	const uint8* outer = WindowShape::CoverageFor(radius);
	if (outer == NULL)
		return;
	const uint8* inner = WindowShape::CoverageFor(radius - 1);

	// Other than lines, this depends on the state the engine has been left in.
	engine->SetTransform(BAffineTransform(), 0, 0);
	engine->SetDrawingMode(B_OP_ALPHA);
	engine->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	engine->SetPattern(B_SOLID_HIGH);

	const bool right = kind == WindowShape::RIGHT_TOP
		|| kind == WindowShape::RIGHT_BOTTOM;
	const bool bottom = kind == WindowShape::LEFT_BOTTOM
		|| kind == WindowShape::RIGHT_BOTTOM;

	for (int32 row = 0; row < radius; row++) {
		for (int32 column = 0; column < radius; column++) {
			int32 visible = outer[row * radius + column];
			if (visible == 0)
				continue;

			int32 hidden = 0;
			if (inner != NULL && row > 0 && column > 0)
				hidden = inner[(row - 1) * (radius - 1) + column - 1];
			if (hidden >= visible)
				continue;

			color.alpha = (visible - hidden) * 255 / visible;
			engine->SetHighColor(color);

			BPoint where(leftTop.x + (right ? radius - 1 - column : column),
				leftTop.y + (bottom ? radius - 1 - row : row));
			engine->FillRect(BRect(where, where));
		}
	}

	// Decorators draw on as if nothing had happened, gradients in particular
	// come out wrong in any other mode.
	engine->SetDrawingMode(B_OP_COPY);
}
