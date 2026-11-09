/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef ROUNDED_DECORATOR_H
#define ROUNDED_DECORATOR_H


#include <vector>

#include "Decorator.h"
#include "WindowShape.h"


class Desktop;
class DrawingEngine;


/*!	To be inherited by a decorator that rounds its corners on a composited
	desktop, next to the class it is built on. The compositor cuts off the corners
	of the shape it gets from GetRoundedShape(), and the decorator draws the line
	of its border along them with StrokeCorner().

	Decorators that don't inherit this keep their corners, and this class being
	separate leaves the layout of Decorator alone for those.
*/
class RoundedDecorator {
public:
	virtual						~RoundedDecorator();

	virtual	void				GetRoundedShape(
									std::vector<Decorator::ShapeRect>& shape)
										const = 0;

	// Whether there is a compositor to cut off corners, and the user wants that.
	static	bool				IsActive(Desktop* desktop);

	// Gives a shape from Decorator::GetShape() the radii that go with a look.
	static	void				RoundShape(
									std::vector<Decorator::ShapeRect>& shape,
									window_look look, int32 frameRadius,
									int32 tabRadius);

	static	void				StrokeCorners(DrawingEngine* engine,
									const Decorator::ShapeRect& shape,
									rgb_color leftTop, rgb_color rightTop,
									rgb_color leftBottom,
									rgb_color rightBottom);

	static	void				StrokeCorner(DrawingEngine* engine,
									BPoint leftTop, int32 radius,
									WindowShape::corner_kind kind,
									rgb_color color);
};


#endif	// ROUNDED_DECORATOR_H
