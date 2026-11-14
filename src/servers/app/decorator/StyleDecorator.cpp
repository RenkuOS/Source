/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


#include "StyleDecorator.h"

#include <algorithm>
#include <math.h>
#include <new>
#include <stddef.h>
#include <string.h>
#include <syslog.h>

#include <Region.h>

#include <WindowPrivate.h>

#include "DesktopSettings.h"
#include "DrawingEngine.h"
#include "ServerFont.h"
#include "StyleCanvas.h"


static const float kBorderResizeLength = 22;
static const float kResizeKnobSize = 18;


static inline BRect
to_rect(const decor_rect& rect)
{
	return BRect(rect.left, rect.top, rect.right, rect.bottom);
}


static inline decor_rect
to_decor_rect(const BRect& rect)
{
	decor_rect result = { rect.left, rect.top, rect.right, rect.bottom };
	return result;
}


static inline decor_rect
invalid_rect()
{
	decor_rect rect = { 0, 0, -1, -1 };
	return rect;
}


//	#pragma mark - StyleDecorAddOn


StyleDecorAddOn::StyleDecorAddOn(image_id id, const char* name,
	const decor_style* style)
	:
	DecorAddOn(id, name),
	fStyle(style)
{
}


status_t
StyleDecorAddOn::InitCheck() const
{
	return fStyle != NULL ? B_OK : B_BAD_VALUE;
}


/*static*/ const decor_style*
StyleDecorAddOn::StyleOf(image_id image)
{
	typedef const decor_style* (*instantiate_function)(uint32 apiVersion);
	instantiate_function instantiate;
	if (get_image_symbol(image, "instantiate_decor_style", B_SYMBOL_TYPE_TEXT,
			(void**)&instantiate) != B_OK) {
		return NULL;
	}

	const decor_style* style = instantiate(DECOR_STYLE_API_VERSION);
	if (style == NULL)
		return NULL;

	// a style built for a later version may rely on what this one lacks
	if (style->api_version == 0 || style->api_version > DECOR_STYLE_API_VERSION
		|| style->size < offsetof(decor_style, footprint)
		|| style->layout == NULL || style->draw_frame == NULL
		|| style->draw_tab == NULL || style->draw_button == NULL) {
		syslog(LOG_WARNING, "app_server: decorator style \"%s\" isn't one "
			"this app_server can use", style->name != NULL ? style->name : "");
		return NULL;
	}
	return style;
}


Decorator*
StyleDecorAddOn::_AllocateDecorator(DesktopSettings& settings, BRect rect,
	Desktop* desktop)
{
	return new(std::nothrow) StyleDecorator(settings, rect, desktop, fStyle);
}


//	#pragma mark - StyleDecorator


StyleDecorator::Tab::Tab()
	:
	cookie(NULL)
{
}


StyleDecorator::StyleDecorator(DesktopSettings& settings, BRect frame,
	Desktop* desktop, const decor_style* style)
	:
	TabDecorator(settings, frame, desktop),
	fStyle(style),
	fCanvas(NULL),
	fWindowCreated(false)
{
	fBorderResizeLength = kBorderResizeLength;
	fResizeKnobSize = kResizeKnobSize;

	memset(&fWindow, 0, sizeof(fWindow));
	fWindow.size = sizeof(fWindow);

	fCanvas = create_style_canvas(this, fDrawState);
	if (fCanvas != NULL && fStyle->create_window != NULL)
		fWindowCreated = fStyle->create_window(&fWindow, fCanvas) == B_OK;
	else
		fWindowCreated = fCanvas != NULL;

	UpdateColors(settings);
}


StyleDecorator::~StyleDecorator()
{
	if (fWindowCreated && fStyle->delete_window != NULL) {
		_SyncIn();
		fStyle->delete_window(&fWindow);
	}
	delete_style_canvas(fCanvas);
}


void
StyleDecorator::Draw(BRect updateRect)
{
	if (!_HasStyle() || fTabList.CountItems() == 0)
		return;

	if (_StyleHas(offsetof(decor_style, draw) + sizeof(fStyle->draw))
		&& fStyle->draw != NULL) {
		fDrawingEngine->SetDrawState(&fDrawState);
		_SyncIn();
		fStyle->draw(&fWindow, fCanvas, to_decor_rect(updateRect));
		return;
	}

	TabDecorator::Draw(updateRect);
}


void
StyleDecorator::Draw()
{
	BRect all = fBorderRect;
	if (fTitleBarRect.IsValid())
		all = all | fTitleBarRect;
	if (IsOutlineResizing())
		all = all | fOutlineBorderRect;
	Draw(all);
}


void
StyleDecorator::DrawMinimize(int32 tab)
{
	AutoReadLocker _(fLocker);

	Decorator::Tab* decoratorTab = fTabList.ItemAt(tab);
	if (decoratorTab != NULL)
		_DrawMinimize(decoratorTab, true, decoratorTab->minimizeRect);
}


Decorator::Region
StyleDecorator::RegionAt(BPoint where, int32& tabIndex) const
{
	tabIndex = -1;
	if (fTopTab == NULL)
		return REGION_NONE;

	if (fStyle->region_at != NULL && fCanvas != NULL) {
		_SyncIn();
		int32 tab = -1;
		decor_point point = { where.x, where.y };
		const int32 region = fStyle->region_at(&fWindow, point, &tab);
		if (region != DECOR_REGION_DEFAULT) {
			tabIndex = tab;
			return region >= REGION_NONE && region < REGION_COUNT
				? (Region)region : REGION_NONE;
		}
	}

	for (int32 i = 0; i < fTabList.CountItems(); i++) {
		if (fTabList.ItemAt(i)->minimizeRect.Contains(where)) {
			tabIndex = i;
			return REGION_MINIMIZE_BUTTON;
		}
	}

	// close, zoom and the tabs
	Region region = Decorator::RegionAt(where, tabIndex);
	if (region != REGION_NONE)
		return region;

	// a style may give any window a resize corner
	if (fResizeRect.IsValid() && fResizeRect.Contains(where)
		&& (fTopTab->flags & B_NOT_RESIZABLE) == 0)
		return REGION_RIGHT_BOTTOM_CORNER;

	// the borders, and the end of the bottom one that resizes
	return TabDecorator::RegionAt(where, tabIndex);
}


void
StyleDecorator::UpdateColors(DesktopSettings& settings)
{
	// the style can also ask for colours as it draws
	if (_HasStyle() && _StyleHas(offsetof(decor_style, update_colors)
			+ sizeof(fStyle->update_colors))
		&& fStyle->update_colors != NULL) {
		_SyncIn();
		fStyle->update_colors(&fWindow, fCanvas);
	}
}


void
StyleDecorator::GetSizeLimits(int32* minWidth, int32* minHeight,
	int32* maxWidth, int32* maxHeight) const
{
	Decorator::GetSizeLimits(minWidth, minHeight, maxWidth, maxHeight);

	if (_HasStyle() && _StyleHas(offsetof(decor_style, size_limits)
			+ sizeof(fStyle->size_limits))
		&& fStyle->size_limits != NULL) {
		AutoReadLocker _(fLocker);
		_SyncIn();
		fStyle->size_limits(&fWindow, minWidth, minHeight, maxWidth,
			maxHeight);
	}
}


//	#pragma mark - Protected methods


Decorator::Tab*
StyleDecorator::_AllocateNewTab()
{
	Tab* tab = new(std::nothrow) Tab;
	if (tab == NULL)
		return NULL;

	_SetFocus(tab);
	return tab;
}


void
StyleDecorator::_DoLayout()
{
	if (!_HasStyle() || fTopTab == NULL || fTabList.CountItems() == 0)
		return;

	_SyncIn();

	fWindow.border_width = 0;
	fWindow.left_border = invalid_rect();
	fWindow.top_border = invalid_rect();
	fWindow.right_border = invalid_rect();
	fWindow.bottom_border = invalid_rect();
	fWindow.title_bar = invalid_rect();
	fWindow.resize = invalid_rect();
	fWindow.tabs_slide = false;
	for (size_t i = 0; i < fTabs.size(); i++) {
		decor_tab& tab = fTabs[i];
		tab.frame = invalid_rect();
		for (int32 button = 0; button < DECOR_BUTTON_CAPACITY; button++)
			tab.buttons[button] = invalid_rect();
		tab.text_offset = 0;
		tab.title_room = 0;
		tab.truncate = DECOR_TRUNCATE_END;
		tab.min_size = 0;
		tab.max_size = 0;
	}

	fStyle->layout(&fWindow, fCanvas);

	_SyncOut();
}


void
StyleDecorator::_DrawFrame(BRect rect)
{
	if (!_HasStyle() || !rect.IsValid())
		return;

	_SyncIn();
	fStyle->draw_frame(&fWindow, fCanvas, to_decor_rect(rect));
}


void
StyleDecorator::_DrawOutlineFrame(BRect rect)
{
	if (!_HasStyle() || !rect.IsValid())
		return;

	if (fStyle->draw_outline != NULL) {
		_SyncIn();
		fStyle->draw_outline(&fWindow, fCanvas, to_decor_rect(rect));
			return;
	}

	drawing_mode oldMode;
	fDrawingEngine->SetDrawingMode(B_OP_ALPHA, oldMode);
	fDrawingEngine->SetPattern(B_MIXED_COLORS);
	fDrawingEngine->StrokeRect(fOutlineBorderRect);
	fDrawingEngine->SetDrawingMode(oldMode);
}


void
StyleDecorator::_DrawTab(Decorator::Tab* tab, BRect rect)
{
	const int32 index = fTabList.IndexOf(tab);
	if (!_HasStyle() || index < 0 || !rect.IsValid()
		|| !rect.Intersects(tab->tabRect))
		return;

	_SyncIn();
	fStyle->draw_tab(&fWindow, fCanvas, index, to_decor_rect(rect));
}


void
StyleDecorator::_DrawTitle(Decorator::Tab* tab, BRect rect)
{
	const int32 index = fTabList.IndexOf(tab);
	if (!_HasStyle() || index < 0)
		return;

	if (fStyle->draw_title == NULL) {
		_DrawTab(tab, tab->tabRect);
		return;
	}

	_SyncIn();
	fStyle->draw_title(&fWindow, fCanvas, index);
}


void
StyleDecorator::_DrawButtons(Decorator::Tab* tab, const BRect& invalid)
{
	if (invalid.Intersects(tab->closeRect))
		_DrawClose(tab, false, tab->closeRect);
	if (invalid.Intersects(tab->minimizeRect))
		_DrawMinimize(tab, false, tab->minimizeRect);
	if (invalid.Intersects(tab->zoomRect))
		_DrawZoom(tab, false, tab->zoomRect);
}


void
StyleDecorator::_DrawClose(Decorator::Tab* tab, bool direct, BRect rect)
{
	_DrawButton(tab, DECOR_BUTTON_CLOSE, direct, rect);
}


void
StyleDecorator::_DrawMinimize(Decorator::Tab* tab, bool direct, BRect rect)
{
	_DrawButton(tab, DECOR_BUTTON_MINIMIZE, direct, rect);
}


void
StyleDecorator::_DrawZoom(Decorator::Tab* tab, bool direct, BRect rect)
{
	_DrawButton(tab, DECOR_BUTTON_ZOOM, direct, rect);
}


void
StyleDecorator::_SetTitle(Decorator::Tab* tab, const char* string,
	BRegion* updateRegion)
{
	if (updateRegion != NULL)
		updateRegion->Include(&GetFootprint());

	_DoLayout();
	_DoOutlineLayout();

	if (updateRegion == NULL)
		return;

	BRegion footprint;
	_GetFootprint(&footprint);
	updateRegion->Include(&footprint);
}


void
StyleDecorator::_SetFocus(Decorator::Tab* tab)
{
	tab->buttonFocus = IsFocus(tab)
		|| ((tab->look == B_FLOATING_WINDOW_LOOK
			|| tab->look == kLeftTitledWindowLook)
			&& (tab->flags & B_AVOID_FOCUS) != 0);
	// a stack's tabs may be laid out by which one is in front
	if (CountTabs() > 1 && fTopTab != NULL)
		_DoLayout();
}


bool
StyleDecorator::_SetTabLocation(Decorator::Tab* tab, float location,
	bool isShifting, BRegion* updateRegion)
{
	if (!fWindow.tabs_slide || fTabList.CountItems() != 1
		|| !tab->tabRect.IsValid())
		return false;

	const float maxLocation = std::max(0.0f,
		fBorderRect.Width() - tab->tabRect.Width());
	location = std::max(0.0f, std::min(floorf(location), maxLocation));
	if (location == tab->tabLocation)
		return false;

	if (updateRegion != NULL) {
		updateRegion->Include(tab->tabRect);
		updateRegion->Include(fTopBorder);
	}
	tab->tabLocation = location;
	_DoLayout();
	if (updateRegion != NULL)
		updateRegion->Include(tab->tabRect);
	return true;
}


bool
StyleDecorator::_SetSettings(const BMessage& settings, BRegion* updateRegion)
{
	bool modified = false;
	float location;
	for (int32 i = 0; i < fTabList.CountItems(); i++) {
		if (settings.FindFloat("tab location", i, &location) != B_OK)
			break;
		// a window without a sliding tab keeps the place of the tab it
		// may have again
		if (!fWindow.tabs_slide && fTabList.CountItems() == 1) {
			Decorator::Tab* tab = fTabList.ItemAt(i);
			tab->tabLocation = std::max(0.0f, floorf(location));
			tab->tabOffset = (uint32)tab->tabLocation;
			continue;
		}
		modified |= SetTabLocation(i, location, false, updateRegion);
	}
	return modified;
}


void
StyleDecorator::_UpdateFont(DesktopSettings& settings)
{
	if (!_HasStyle())
		return;

	if (fStyle->update_font != NULL) {
		_SyncIn();
		fStyle->update_font(&fWindow, fCanvas);
		return;
	}

	TabDecorator::_UpdateFont(settings);
}


void
StyleDecorator::_MoveBy(BPoint offset)
{
	TabDecorator::_MoveBy(offset);
	for (int32 i = 0; i < fTabList.CountItems(); i++)
		fTabList.ItemAt(i)->minimizeRect.OffsetBy(offset);

	if (_HasStyle() && _StyleHas(offsetof(decor_style, moved)
			+ sizeof(fStyle->moved))
		&& fStyle->moved != NULL) {
		_SyncIn();
		fStyle->moved(&fWindow, offset.x, offset.y);
	}
}


void
StyleDecorator::_ResizeBy(BPoint offset, BRegion* dirty)
{
	if (dirty != NULL) {
		BRegion before;
		_GetFootprint(&before);
		dirty->Include(&before);
	}

	fFrame.right += offset.x;
	fFrame.bottom += offset.y;
	_DoLayout();

	if (dirty != NULL) {
		BRegion after;
		_GetFootprint(&after);
		dirty->Include(&after);
	}
}


bool
StyleDecorator::_AddTab(DesktopSettings& settings, int32 index,
	BRegion* updateRegion)
{
	if (updateRegion != NULL && fTitleBarRect.IsValid())
		updateRegion->Include(fTitleBarRect);

	return TabDecorator::_AddTab(settings, index, updateRegion);
}


bool
StyleDecorator::_RemoveTab(int32 index, BRegion* updateRegion)
{
	if (updateRegion != NULL)
		updateRegion->Include(fTitleBarRect);

	return TabDecorator::_RemoveTab(index, updateRegion);
}


bool
StyleDecorator::_MoveTab(int32 from, int32 to, bool isMoving,
	BRegion* updateRegion)
{
	if (updateRegion != NULL)
		updateRegion->Include(fTitleBarRect);

	_DoLayout();

	if (updateRegion != NULL)
		updateRegion->Include(fTitleBarRect);
	return true;
}


void
StyleDecorator::_GetFootprint(BRegion* region)
{
	if (region == NULL || fTopTab == NULL
		|| fTopTab->look == B_NO_BORDER_WINDOW_LOOK)
		return;

	if (_HasStyle() && fStyle->footprint != NULL) {
		_SyncIn();
		StyleRegion styleRegion(region);
		fStyle->footprint(&fWindow, &styleRegion);
		return;
	}

	if (fBorderWidth > 0) {
		region->Include(fTopBorder);
		region->Include(fLeftBorder);
		region->Include(fRightBorder);
		region->Include(fBottomBorder);
	}

	if (fTopTab->look == B_BORDERED_WINDOW_LOOK)
		return;

	if (fTitleBarRect.IsValid())
		region->Include(fTitleBarRect);
	if (fResizeRect.IsValid())
		region->Include(fResizeRect);
}


//	#pragma mark - Private methods


/*!	Brings the structures the style sees up to date with the decorator's
	state: the frame, the tabs and their titles, focus and buttons.
*/
void
StyleDecorator::_SyncIn() const
{
	const int32 count = fTabList.CountItems();
	if ((int32)fTabs.size() != count) {
		decor_tab empty;
		memset(&empty, 0, sizeof(empty));
		empty.size = sizeof(empty);
		fTabs.resize(count, empty);
	}

	fWindow.frame = to_decor_rect(fFrame);
	fWindow.tab_count = count;
	fTabPointers.resize(count);
	for (int32 i = 0; i < count; i++)
		fTabPointers[i] = &fTabs[i];
	fWindow.tabs = count > 0 ? &fTabPointers[0] : NULL;
	// While a tab is removed, fTopTab can still be the one that's gone.
	fWindow.top_tab = std::max(fTabList.IndexOf(fTopTab), (int32)0);
	fWindow.highlights[DECOR_REGION_NONE] = DECOR_HIGHLIGHT_NONE;
	for (int32 region = 1; region < DECOR_REGION_COUNT; region++)
		fWindow.highlights[region] = RegionHighlight((Region)region);
	fWindow.outline_resizing = IsOutlineResizing();
	fWindow.outline_frame = to_decor_rect(BRect(fFrame.LeftTop(),
		fFrame.RightBottom() + fOutlinesDelta));

	for (int32 i = 0; i < count; i++) {
		Tab* tab = static_cast<Tab*>(fTabList.ItemAt(i));
		decor_tab& styleTab = fTabs[i];
		styleTab.size = sizeof(decor_tab);
		styleTab.title = tab->title.String();
		styleTab.look = tab->look;
		styleTab.flags = tab->flags;
		styleTab.focused = tab->isFocused;
		styleTab.button_focus = tab->buttonFocus;
		styleTab.pressed[DECOR_BUTTON_CLOSE] = tab->closePressed;
		styleTab.pressed[DECOR_BUTTON_ZOOM] = tab->zoomPressed;
		styleTab.pressed[DECOR_BUTTON_MINIMIZE] = tab->minimizePressed;
		styleTab.highlighted = tab->isHighlighted;
		styleTab.location = tab->tabLocation;
		styleTab.shown_title = tab->truncatedTitle.String();
		styleTab.shown_title_length = tab->truncatedTitleLength;
		styleTab.cookie = tab->cookie;

		// the layout, as the decorator has it now
		styleTab.frame = to_decor_rect(tab->tabRect);
		styleTab.buttons[DECOR_BUTTON_CLOSE] = to_decor_rect(tab->closeRect);
		styleTab.buttons[DECOR_BUTTON_ZOOM] = to_decor_rect(tab->zoomRect);
		styleTab.buttons[DECOR_BUTTON_MINIMIZE]
			= to_decor_rect(tab->minimizeRect);
		styleTab.text_offset = tab->textOffset;
		styleTab.min_size = tab->minTabSize;
		styleTab.max_size = tab->maxTabSize;
	}

	fWindow.border_width = fBorderWidth > 0 ? fBorderWidth : 0;
	fWindow.left_border = to_decor_rect(fLeftBorder);
	fWindow.top_border = to_decor_rect(fTopBorder);
	fWindow.right_border = to_decor_rect(fRightBorder);
	fWindow.bottom_border = to_decor_rect(fBottomBorder);
	fWindow.title_bar = to_decor_rect(fTitleBarRect);
	fWindow.resize = to_decor_rect(fResizeRect);
}


//!	Takes the layout the style made into the decorator.
void
StyleDecorator::_SyncOut()
{
	fBorderWidth = (int32)fWindow.border_width;
	fLeftBorder = to_rect(fWindow.left_border);
	fTopBorder = to_rect(fWindow.top_border);
	fRightBorder = to_rect(fWindow.right_border);
	fBottomBorder = to_rect(fWindow.bottom_border);
	if (fBorderWidth > 0 && fTopBorder.IsValid() && fBottomBorder.IsValid())
		fBorderRect = BRect(fTopBorder.LeftTop(), fBottomBorder.RightBottom());
	else
		fBorderRect = fFrame;
	fTitleBarRect = to_rect(fWindow.title_bar);
	fResizeRect = to_rect(fWindow.resize);

	const ServerFont& font = fDrawState.Font();
	const int32 count = std::min((int32)fTabs.size(), fTabList.CountItems());
	for (int32 i = 0; i < count; i++) {
		Tab* tab = static_cast<Tab*>(fTabList.ItemAt(i));
		const decor_tab& styleTab = fTabs[i];

		tab->cookie = styleTab.cookie;
		tab->tabRect = to_rect(styleTab.frame);
		tab->closeRect = to_rect(styleTab.buttons[DECOR_BUTTON_CLOSE]);
		tab->zoomRect = to_rect(styleTab.buttons[DECOR_BUTTON_ZOOM]);
		tab->minimizeRect = to_rect(styleTab.buttons[DECOR_BUTTON_MINIMIZE]);
		tab->textOffset = styleTab.text_offset;
		tab->minTabSize = styleTab.min_size;
		tab->maxTabSize = styleTab.max_size;
		// a sliding tab's place is the style's to keep in bounds
		if (fWindow.tabs_slide)
			tab->tabLocation = styleTab.location;
		// Saved with the window's settings: a lone tab that can't slide keeps
		// the place it may have again, when it can.
		if (!fWindow.tabs_slide && count == 1)
			tab->tabOffset = (uint32)std::max(0.0f, tab->tabLocation);
		else {
			tab->tabOffset = tab->tabRect.IsValid() ? (uint32)std::max(0.0f,
				tab->tabRect.left - fBorderRect.left) : 0;
		}

		uint32 mode;
		switch (styleTab.truncate) {
			case DECOR_TRUNCATE_MIDDLE:
				mode = B_TRUNCATE_MIDDLE;
				break;
			case DECOR_TRUNCATE_BEGINNING:
				mode = B_TRUNCATE_BEGINNING;
				break;
			default:
				mode = B_TRUNCATE_END;
				break;
		}
		tab->truncatedTitle = tab->title;
		font.TruncateString(&tab->truncatedTitle, mode,
			std::max(0.0f, styleTab.title_room));
		tab->truncatedTitleLength = tab->truncatedTitle.Length();
	}
}


void
StyleDecorator::_DrawButton(Decorator::Tab* tab, int32 button, bool direct,
	BRect rect)
{
	const int32 index = fTabList.IndexOf(tab);
	if (!_HasStyle() || index < 0 || !rect.IsValid())
		return;

	_SyncIn();
	const bool copyToFront = fDrawingEngine->CopyToFrontEnabled();
	fDrawingEngine->SetCopyToFrontEnabled(direct);
	fStyle->draw_button(&fWindow, fCanvas, index, button,
		to_decor_rect(rect));
	fDrawingEngine->SetCopyToFrontEnabled(copyToFront);
}


//!	Whether the style's structure is large enough to have what ends at \a end.
bool
StyleDecorator::_StyleHas(size_t end) const
{
	return fStyle != NULL && fStyle->size >= end;
}


bool
StyleDecorator::_HasStyle() const
{
	return fStyle != NULL && fCanvas != NULL && fWindowCreated;
}
