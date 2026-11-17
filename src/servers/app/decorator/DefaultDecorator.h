/*
 * Copyright 2001-2020 Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Stephan Aßmus, superstippi@gmx.de
 *		DarkWyrm, bpmagic@columbus.rr.com
 *		Ryan Leavengood, leavengood@gmail.com
 *		Philippe Saint-Pierre, stpere@gmail.com
 *		John Scipione, jscipione@gmail.com
 *		Ingo Weinhold, ingo_weinhold@gmx.de
 *		Clemens Zeidler, haiku@clemens-zeidler.de
 *		Joseph Groover, looncraz@looncraz.net
 *		Tri-Edge AI
 *		Jacob Secunda, secundja@gmail.com
 */
#ifndef DEFAULT_DECORATOR_H
#define DEFAULT_DECORATOR_H


#include "TabDecorator.h"
#include "WindowShape.h"


class Desktop;
class ServerBitmap;


class DefaultDecorator: public TabDecorator {
public:
								DefaultDecorator(DesktopSettings& settings,
									BRect frame, Desktop* desktop);
	virtual						~DefaultDecorator();

	virtual	void				GetComponentColors(Component component,
									uint8 highlight, ComponentColors _colors,
									Decorator::Tab* tab = NULL);

	virtual void				UpdateColors(DesktopSettings& settings);

			void				GetRoundedShape(
									std::vector<ShapeRect>& shape) const;
			// Decorators built on this one have their own idea of the corners,
			// except for the one that adds stack and tile to it, which is the
			// one the system normally uses.
			bool				HasRoundedCorners() const;

protected:
	virtual	void				_DrawFrame(BRect rect);

	virtual	void				_DrawTab(Decorator::Tab* tab, BRect r);
	virtual	void				_DrawTitle(Decorator::Tab* tab, BRect r);
	virtual	void				_DrawClose(Decorator::Tab* tab, bool direct,
									BRect rect);
	virtual	void				_DrawZoom(Decorator::Tab* tab, bool direct,
									BRect rect);
	virtual	void				_DrawMinimize(Decorator::Tab* tab, bool direct,
									BRect rect);
	virtual	void				_DrawResizeKnob(BRect r, bool full,
									const ComponentColors& color);

private:
			void				_StrokeCorner(BPoint leftTop, int32 radius,
									WindowShape::corner_kind kind,
									rgb_color color);
			void				_DrawFrameCorners(BRect rect);
 			void				_DrawButtonBitmap(ServerBitmap* bitmap,
 									bool direct, BRect rect);
			void				_DrawBlendedRect(DrawingEngine *engine,
									const BRect rect, bool down,
									const ComponentColors& colors);
			ServerBitmap*		_GetBitmapForButton(Decorator::Tab* tab,
									Component item, bool down, int32 width,
									int32 height);

			void				_GetComponentColors(Component component,
									ComponentColors _colors,
									Decorator::Tab* tab = NULL);
};


#endif	// DEFAULT_DECORATOR_H
