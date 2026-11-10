/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef STYLE_CANVAS_H
#define STYLE_CANVAS_H


#include <Region.h>

#include <decorator/DecorStyle.h>


class Decorator;
class DrawState;


/*!	The canvas a decorator style draws on and measures with
	(DecorStyle.h): app_server's drawing engine and a decorator's title
	font, behind the interface's function table.
*/
decor_canvas*	create_style_canvas(Decorator* decorator, DrawState& state);
void			delete_style_canvas(decor_canvas* canvas);


/*!	Collects a style's footprint into a BRegion. */
class StyleRegion : public decor_region {
public:
	StyleRegion(BRegion* region)
		:
		fRegion(region)
	{
		memset(static_cast<decor_region*>(this), 0, sizeof(decor_region));
		size = sizeof(decor_region);
		cookie = this;
		include = &_Include;
		exclude = &_Exclude;
	}

private:
	static void _Include(decor_region* region, decor_rect rect)
	{
		BRect r(rect.left, rect.top, rect.right, rect.bottom);
		if (r.IsValid())
			static_cast<StyleRegion*>(region->cookie)->fRegion->Include(r);
	}

	static void _Exclude(decor_region* region, decor_rect rect)
	{
		BRect r(rect.left, rect.top, rect.right, rect.bottom);
		if (r.IsValid())
			static_cast<StyleRegion*>(region->cookie)->fRegion->Exclude(r);
	}

			BRegion*		fRegion;
};


#endif	// STYLE_CANVAS_H
