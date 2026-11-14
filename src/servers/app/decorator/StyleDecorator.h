/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef STYLE_DECORATOR_H
#define STYLE_DECORATOR_H


#include <vector>

#include <decorator/DecorStyle.h>

#include "DecorManager.h"
#include "TabDecorator.h"


/*!	Loads decorator styles: add-ons exporting instantiate_decor_style(),
	whose decorators app_server makes itself, as StyleDecorators.
*/
class StyleDecorAddOn : public DecorAddOn {
public:
								StyleDecorAddOn(image_id id, const char* name,
									const decor_style* style);

	virtual	status_t			InitCheck() const;

	static	const decor_style*	StyleOf(image_id image);
		// the add-on's style, if it is one whose version app_server knows

protected:
	virtual	Decorator*			_AllocateDecorator(DesktopSettings& settings,
									BRect rect, Desktop* desktop);

private:
			const decor_style*	fStyle;
};


/*!	A decorator that a style describes: the style lays the frame out and
	draws it, through the plain structures of DecorStyle.h, and this does
	the rest, as app_server's own decorators do. What doesn't depend on how
	the frame is laid out comes from TabDecorator.
*/
class StyleDecorator : public TabDecorator {
public:
								StyleDecorator(DesktopSettings& settings,
									BRect frame, Desktop* desktop,
									const decor_style* style);
	virtual						~StyleDecorator();

	virtual	void				Draw(BRect updateRect);
	virtual	void				Draw();

	virtual	void				DrawMinimize(int32 tab);

	virtual	Region				RegionAt(BPoint where, int32& tab) const;

	virtual	void				UpdateColors(DesktopSettings& settings);

	virtual	void				GetSizeLimits(int32* minWidth,
									int32* minHeight, int32* maxWidth,
									int32* maxHeight) const;

protected:
	struct Tab : public Decorator::Tab {
								Tab();

		void*					cookie;
	};

	virtual	Decorator::Tab*		_AllocateNewTab();

	virtual	void				_DoLayout();

	virtual	void				_DrawFrame(BRect rect);
	virtual	void				_DrawOutlineFrame(BRect rect);
	virtual	void				_DrawTab(Decorator::Tab* tab, BRect rect);
	virtual	void				_DrawTitle(Decorator::Tab* tab, BRect rect);
	virtual	void				_DrawButtons(Decorator::Tab* tab,
									const BRect& invalid);
	virtual	void				_DrawClose(Decorator::Tab* tab, bool direct,
									BRect rect);
	virtual	void				_DrawMinimize(Decorator::Tab* tab,
									bool direct, BRect rect);
	virtual	void				_DrawZoom(Decorator::Tab* tab, bool direct,
									BRect rect);

	virtual	void				_SetTitle(Decorator::Tab* tab,
									const char* string,
									BRegion* updateRegion = NULL);
	virtual	void				_SetFocus(Decorator::Tab* tab);
	virtual	bool				_SetTabLocation(Decorator::Tab* tab,
									float location, bool isShifting,
									BRegion* updateRegion = NULL);
	virtual	bool				_SetSettings(const BMessage& settings,
									BRegion* updateRegion = NULL);

	virtual	void				_UpdateFont(DesktopSettings& settings);

	virtual	void				_MoveBy(BPoint offset);
	virtual	void				_ResizeBy(BPoint offset, BRegion* dirty);

	virtual	bool				_AddTab(DesktopSettings& settings,
									int32 index = -1,
									BRegion* updateRegion = NULL);
	virtual	bool				_RemoveTab(int32 index,
									BRegion* updateRegion = NULL);
	virtual	bool				_MoveTab(int32 from, int32 to, bool isMoving,
									BRegion* updateRegion = NULL);

	virtual	void				_GetFootprint(BRegion* region);

private:
			void				_SyncIn() const;
			void				_SyncOut();
			void				_DrawButton(Decorator::Tab* tab, int32 button,
									bool direct, BRect rect);
			bool				_HasStyle() const;
			bool				_StyleHas(size_t end) const;

			const decor_style*	fStyle;
	mutable	decor_window		fWindow;
	mutable	std::vector<decor_tab> fTabs;
	mutable	std::vector<decor_tab*> fTabPointers;
		// what fWindow.tabs points to
			decor_canvas*		fCanvas;
			bool				fWindowCreated;
};


#endif	// STYLE_DECORATOR_H
