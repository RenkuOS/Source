/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


#include "StyleCanvas.h"

#include <algorithm>
#include <new>
#include <string.h>

#include <GradientLinear.h>
#include <Menu.h>

#include "BitmapManager.h"
#include "Decorator.h"
#include "DesktopSettings.h"
#include "DrawingEngine.h"
#include "DrawState.h"
#include "GlobalFontManager.h"
#include "ServerBitmap.h"
#include "ServerFont.h"


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


static inline BPoint
to_point(const decor_point& point)
{
	return BPoint(point.x, point.y);
}


static inline decor_rect
invalid_rect()
{
	decor_rect rect = { 0, 0, -1, -1 };
	return rect;
}


//	#pragma mark - StyleCanvas


/*!	The canvas a style draws on and measures with: app_server's drawing
	engine and the decorator's title font, behind DecorStyle.h's function
	table.
*/
class StyleCanvas : public decor_canvas {
public:
	StyleCanvas(Decorator* decorator, DrawState& state)
		:
		fDecorator(decorator),
		fState(state)
	{
		memset(static_cast<decor_canvas*>(this), 0, sizeof(decor_canvas));
		size = sizeof(decor_canvas);
		cookie = this;

		ui_color = &_UIColor;
		set_drawing_mode = &_SetDrawingMode;
		set_blending_mode = &_SetBlendingMode;
		set_high_color = &_SetHighColor;
		set_low_color = &_SetLowColor;
		set_pattern = &_SetPattern;
		stroke_line = &_StrokeLine;
		stroke_point = &_StrokePoint;
		stroke_rect = &_StrokeRect;
		fill_rect = &_FillRect;
		fill_rect_gradient = &_FillRectGradient;
		stroke_rect_pattern = &_StrokeRectPattern;
		fill_rect_pattern = &_FillRectPattern;
		draw_bitmap = &_DrawBitmap;
		set_font = &_SetFont;
		font_size = &_FontSize;
		font_height = &_FontHeight;
		string_width = &_StringWidth;
		truncate_string = &_TruncateString;
		draw_string = &_DrawString;
		copy_to_front = &_CopyToFront;
		set_copy_to_front = &_SetCopyToFront;
	}

private:
	static StyleCanvas* _Self(decor_canvas* canvas)
	{
		return static_cast<StyleCanvas*>(canvas->cookie);
	}

	DrawingEngine* _Engine() const
	{
		return fDecorator->GetDrawingEngine();
	}

	static rgb_color _UIColor(decor_canvas* canvas, color_which which)
	{
		return _Self(canvas)->fDecorator->UIColor(which);
	}

	static void _SetDrawingMode(decor_canvas* canvas, drawing_mode mode)
	{
		if (DrawingEngine* engine = _Self(canvas)->_Engine())
			engine->SetDrawingMode(mode);
	}

	static void _SetBlendingMode(decor_canvas* canvas, source_alpha srcAlpha,
		alpha_function alphaFunction)
	{
		if (DrawingEngine* engine = _Self(canvas)->_Engine())
			engine->SetBlendingMode(srcAlpha, alphaFunction);
	}

	static void _SetHighColor(decor_canvas* canvas, rgb_color color)
	{
		if (DrawingEngine* engine = _Self(canvas)->_Engine())
			engine->SetHighColor(color);
	}

	static void _SetLowColor(decor_canvas* canvas, rgb_color color)
	{
		if (DrawingEngine* engine = _Self(canvas)->_Engine())
			engine->SetLowColor(color);
	}

	static void _SetPattern(decor_canvas* canvas, const pattern* pattern)
	{
		if (DrawingEngine* engine = _Self(canvas)->_Engine())
			engine->SetPattern(*pattern);
	}

	static void _StrokeLine(decor_canvas* canvas, decor_point start,
		decor_point end, rgb_color color)
	{
		if (DrawingEngine* engine = _Self(canvas)->_Engine())
			engine->StrokeLine(to_point(start), to_point(end), color);
	}

	static void _StrokePoint(decor_canvas* canvas, decor_point point,
		rgb_color color)
	{
		if (DrawingEngine* engine = _Self(canvas)->_Engine())
			engine->StrokePoint(to_point(point), color);
	}

	static void _StrokeRect(decor_canvas* canvas, decor_rect rect,
		rgb_color color)
	{
		if (DrawingEngine* engine = _Self(canvas)->_Engine())
			engine->StrokeRect(to_rect(rect), color);
	}

	static void _FillRect(decor_canvas* canvas, decor_rect rect,
		rgb_color color)
	{
		if (DrawingEngine* engine = _Self(canvas)->_Engine())
			engine->FillRect(to_rect(rect), color);
	}

	static void _FillRectGradient(decor_canvas* canvas, decor_rect rect,
		const decor_gradient* gradient)
	{
		DrawingEngine* engine = _Self(canvas)->_Engine();
		if (engine == NULL || gradient == NULL)
			return;

		BGradientLinear linear;
		linear.SetStart(to_point(gradient->start));
		linear.SetEnd(to_point(gradient->end));
		const int32 count = gradient->stop_count < DECOR_GRADIENT_STOPS
			? gradient->stop_count : DECOR_GRADIENT_STOPS;
		for (int32 i = 0; i < count; i++)
			linear.AddColor(gradient->colors[i], gradient->offsets[i]);
		engine->FillRect(to_rect(rect), linear);
	}

	static void _StrokeRectPattern(decor_canvas* canvas, decor_rect rect)
	{
		if (DrawingEngine* engine = _Self(canvas)->_Engine())
			engine->StrokeRect(to_rect(rect));
	}

	static void _FillRectPattern(decor_canvas* canvas, decor_rect rect)
	{
		if (DrawingEngine* engine = _Self(canvas)->_Engine())
			engine->FillRect(to_rect(rect));
	}

	static void _DrawBitmap(decor_canvas* canvas, const void* bits,
		int32 width, int32 height, int32 bytesPerRow, decor_rect source,
		decor_rect destination)
	{
		DrawingEngine* engine = _Self(canvas)->_Engine();
		if (engine == NULL || bits == NULL || width <= 0 || height <= 0)
			return;

		BReference<UtilityBitmap> bitmap(new(std::nothrow) UtilityBitmap(
			BRect(0, 0, width - 1, height - 1), B_RGBA32, 0), true);
		if (bitmap.Get() == NULL || bitmap->Bits() == NULL)
			return;

		const int32 rowBytes = bitmap->BytesPerRow();
		const int32 copy = rowBytes < bytesPerRow ? rowBytes : bytesPerRow;
		for (int32 y = 0; y < height; y++) {
			memcpy(bitmap->Bits() + y * rowBytes,
				(const uint8*)bits + y * bytesPerRow, copy);
		}
		engine->DrawBitmap(bitmap.Get(), to_rect(source), to_rect(destination));
	}

	static void _SetFont(decor_canvas* canvas, const decor_font* font)
	{
		StyleCanvas* self = _Self(canvas);
		ServerFont serverFont;
		_MakeFont(self, font, serverFont);
		self->fState.SetFont(serverFont);
		if (DrawingEngine* engine = self->_Engine())
			engine->SetFont(serverFont);
	}

	static float _FontSize(decor_canvas* canvas, int32 which)
	{
		ServerFont font;
		_SystemFont(_Self(canvas), which, font);
		return font.Size();
	}

	static void _FontHeight(decor_canvas* canvas, const decor_font* font,
		float* ascent, float* descent, float* leading)
	{
		::font_height height;
		_FontOf(_Self(canvas), font).GetHeight(height);
		if (ascent != NULL)
			*ascent = height.ascent;
		if (descent != NULL)
			*descent = height.descent;
		if (leading != NULL)
			*leading = height.leading;
	}

	static float _StringWidth(decor_canvas* canvas, const decor_font* font,
		const char* string, int32 length)
	{
		if (string == NULL)
			return 0;
		if (length < 0)
			length = strlen(string);
		return _FontOf(_Self(canvas), font).StringWidth(string, length);
	}

	static int32 _TruncateString(decor_canvas* canvas, const decor_font* font,
		const char* string, uint32 mode, float width, char* result,
		int32 resultSize)
	{
		if (string == NULL || result == NULL || resultSize <= 0)
			return 0;

		uint32 truncation;
		switch (mode) {
			case DECOR_TRUNCATE_MIDDLE:
				truncation = B_TRUNCATE_MIDDLE;
				break;
			case DECOR_TRUNCATE_BEGINNING:
				truncation = B_TRUNCATE_BEGINNING;
				break;
			default:
				truncation = B_TRUNCATE_END;
				break;
		}
		BString text(string);
		_FontOf(_Self(canvas), font).TruncateString(&text, truncation, width);
		const int32 length = std::min(text.Length(), resultSize - 1);
		memcpy(result, text.String(), length);
		result[length] = '\0';
		return length;
	}

	//! The title font for NULL, \a font otherwise.
	static ServerFont _FontOf(StyleCanvas* self, const decor_font* font)
	{
		if (font == NULL)
			return self->fState.Font();
		ServerFont serverFont;
		_MakeFont(self, font, serverFont);
		return serverFont;
	}

	static void _MakeFont(StyleCanvas* self, const decor_font* font,
		ServerFont& serverFont)
	{
		_SystemFont(self, font != NULL ? font->which : DECOR_FONT_PLAIN,
			serverFont);
		if (font != NULL && font->size > 0)
			serverFont.SetSize(font->size);
		serverFont.SetFlags(font != NULL ? font->flags : 0);
		serverFont.SetRotation(font != NULL ? font->rotation : 0);
		serverFont.SetSpacing(B_STRING_SPACING);
	}

	static void _DrawString(decor_canvas* canvas, const char* string,
		int32 length, decor_point baseline, rgb_color color)
	{
		DrawingEngine* engine = _Self(canvas)->_Engine();
		if (engine == NULL || string == NULL)
			return;
		if (length < 0)
			length = strlen(string);
		engine->SetHighColor(color);
		engine->DrawString(string, length, to_point(baseline));
	}

	static bool _CopyToFront(decor_canvas* canvas)
	{
		DrawingEngine* engine = _Self(canvas)->_Engine();
		return engine != NULL && engine->CopyToFrontEnabled();
	}

	static void _SetCopyToFront(decor_canvas* canvas, bool enabled)
	{
		if (DrawingEngine* engine = _Self(canvas)->_Engine())
			engine->SetCopyToFrontEnabled(enabled);
	}

	static void _SystemFont(StyleCanvas* self, int32 which, ServerFont& font)
	{
		DesktopSettings settings(self->fDecorator->GetDesktop());
		switch (which) {
			case DECOR_FONT_BOLD:
				settings.GetDefaultBoldFont(font);
				break;
			case DECOR_FONT_FIXED:
				settings.GetDefaultFixedFont(font);
				break;
			case DECOR_FONT_MENU:
			{
				menu_info info;
				settings.GetMenuInfo(info);
				settings.GetDefaultPlainFont(font);
				if (gFontManager->Lock()) {
					FontStyle* style = gFontManager->GetStyle(info.f_family,
						info.f_style);
					if (style != NULL)
						font.SetStyle(style);
					gFontManager->Unlock();
				}
				font.SetSize(info.font_size);
				break;
			}
			default:
				settings.GetDefaultPlainFont(font);
				break;
		}
	}

private:
			Decorator*		fDecorator;
			DrawState&		fState;
};


decor_canvas*
create_style_canvas(Decorator* decorator, DrawState& state)
{
	return new(std::nothrow) StyleCanvas(decorator, state);
}


void
delete_style_canvas(decor_canvas* canvas)
{
	delete static_cast<StyleCanvas*>(canvas);
}
