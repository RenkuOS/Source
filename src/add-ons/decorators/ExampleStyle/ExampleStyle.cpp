/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 *
 * An example of a decorator style (DecorStyle.h): a title bar across the
 * top of the window, split between a stack's tabs, with a close button on
 * the left and a zoom button on the right, over a thin frame. It uses only
 * the interface, not app_server's or libbe's classes, so a build of it keeps
 * working on later app_servers. Everything it leaves out, such as hit
 * testing and the footprint, app_server's engine does.
 */


#include <math.h>
#include <string.h>

#include <Window.h>

#include <decorator/DecorStyle.h>


static const float kBorderWidth = 3;
static const float kButtonInset = 4;
static const float kTitleGap = 6;


static rgb_color
shade(rgb_color color, float factor)
{
	// darker below 1, lighter above
	rgb_color result = color;
	if (factor < 1) {
		result.red = (uint8)(color.red * factor);
		result.green = (uint8)(color.green * factor);
		result.blue = (uint8)(color.blue * factor);
	} else {
		factor -= 1;
		result.red = (uint8)(color.red + (255 - color.red) * factor);
		result.green = (uint8)(color.green + (255 - color.green) * factor);
		result.blue = (uint8)(color.blue + (255 - color.blue) * factor);
	}
	return result;
}


static decor_rect
make_rect(float left, float top, float right, float bottom)
{
	decor_rect rect = { left, top, right, bottom };
	return rect;
}


static bool
is_valid(const decor_rect& rect)
{
	return rect.left <= rect.right && rect.top <= rect.bottom;
}


static bool
intersects(const decor_rect& a, const decor_rect& b)
{
	return is_valid(a) && is_valid(b) && a.left <= b.right
		&& b.left <= a.right && a.top <= b.bottom && b.top <= a.bottom;
}


static uint32
top_look(const decor_window* window)
{
	return window->tabs[window->top_tab]->look;
}


static void
example_update_font(decor_window* window, decor_canvas* canvas)
{
	decor_font font;
	memset(&font, 0, sizeof(font));
	font.which = top_look(window) == B_FLOATING_WINDOW_LOOK
		? DECOR_FONT_PLAIN : DECOR_FONT_BOLD;
	font.flags = B_FORCE_ANTIALIASING;
	canvas->set_font(canvas, &font);
}


static void
example_layout(decor_window* window, decor_canvas* canvas)
{
	const uint32 look = top_look(window);
	if (look == B_NO_BORDER_WINDOW_LOOK)
		return;

	const decor_rect& f = window->frame;
	const float b = kBorderWidth;
	window->border_width = b;
	window->left_border = make_rect(f.left - b, f.top, f.left - 1, f.bottom);
	window->right_border = make_rect(f.right + 1, f.top, f.right + b,
		f.bottom);
	window->top_border = make_rect(f.left - b, f.top - b, f.right + b,
		f.top - 1);
	window->bottom_border = make_rect(f.left - b, f.bottom + 1, f.right + b,
		f.bottom + b);

	if (look == B_BORDERED_WINDOW_LOOK)
		return;

	float ascent, descent, leading;
	canvas->font_height(canvas, NULL, &ascent, &descent, &leading);
	const float height = ceilf(ascent + descent)
		+ (look == B_FLOATING_WINDOW_LOOK ? 4 : 8);
	const float buttonSize = height - 2 * kButtonInset;

	decor_rect bar = make_rect(f.left - b, f.top - b - height, f.right + b,
		f.top - b - 1);
	window->title_bar = bar;

	// a stack's tabs share the title bar evenly
	const float width = bar.right - bar.left + 1;
	for (int32 i = 0; i < window->tab_count; i++) {
		decor_tab* tab = window->tabs[i];
		tab->frame = make_rect(bar.left + floorf(width * i / window->tab_count),
			bar.top, bar.left + floorf(width * (i + 1) / window->tab_count) - 1,
			bar.bottom);

		const float top = tab->frame.top + kButtonInset;
		float textLeft = tab->frame.left + kTitleGap;
		float textRight = tab->frame.right - kTitleGap;
		if ((tab->flags & B_NOT_CLOSABLE) == 0) {
			const float left = tab->frame.left + kButtonInset;
			tab->buttons[DECOR_BUTTON_CLOSE] = make_rect(left, top,
				left + buttonSize - 1, top + buttonSize - 1);
			textLeft = left + buttonSize + kTitleGap;
		}
		if ((tab->flags & B_NOT_ZOOMABLE) == 0) {
			const float right = tab->frame.right - kButtonInset;
			tab->buttons[DECOR_BUTTON_ZOOM] = make_rect(right - buttonSize + 1,
				top, right, top + buttonSize - 1);
			textRight = right - buttonSize - kTitleGap;
		}

		tab->text_offset = textLeft - tab->frame.left;
		tab->title_room = textRight - textLeft;
		tab->truncate = DECOR_TRUNCATE_END;
		tab->min_size = 2 * (buttonSize + kButtonInset) + 3 * kTitleGap;
		tab->max_size = 1e6;
	}
}


static void
example_draw_frame(decor_window* window, decor_canvas* canvas,
	decor_rect invalid)
{
	const decor_tab* tab = window->tabs[window->top_tab];
	const rgb_color color = canvas->ui_color(canvas, tab->focused
		? B_WINDOW_BORDER_COLOR : B_WINDOW_INACTIVE_BORDER_COLOR);

	const decor_rect* borders[] = { &window->left_border,
		&window->top_border, &window->right_border, &window->bottom_border };
	for (int32 i = 0; i < 4; i++) {
		if (intersects(*borders[i], invalid))
			canvas->fill_rect(canvas, *borders[i], color);
	}

	// an outline around the frame and the title bar
	decor_rect outline = make_rect(window->left_border.left,
		window->top_border.top, window->right_border.right,
		window->bottom_border.bottom);
	if (is_valid(window->title_bar))
		outline.top = window->title_bar.top;
	canvas->stroke_rect(canvas, outline, shade(color, 0.6f));
}


static void
example_draw_button(decor_window* window, decor_canvas* canvas, int32 index,
	int32 button, decor_rect invalid)
{
	const decor_tab* tab = window->tabs[index];
	const decor_rect& r = tab->buttons[button];
	if (!is_valid(r) || button > DECOR_BUTTON_ZOOM)
		return;

	const rgb_color base = canvas->ui_color(canvas, tab->button_focus
		? B_WINDOW_TAB_COLOR : B_WINDOW_INACTIVE_TAB_COLOR);
	const rgb_color mark = shade(base, 0.45f);
	canvas->fill_rect(canvas, r, shade(base, tab->pressed[button]
		? 0.8f : 1.3f));
	canvas->stroke_rect(canvas, r, mark);

	const float inset = floorf((r.right - r.left) / 4);
	const decor_rect in = make_rect(r.left + inset, r.top + inset,
		r.right - inset, r.bottom - inset);
	if (button == DECOR_BUTTON_CLOSE) {
		decor_point a = { in.left, in.top }, b = { in.right, in.bottom };
		decor_point c = { in.left, in.bottom }, d = { in.right, in.top };
		canvas->stroke_line(canvas, a, b, mark);
		canvas->stroke_line(canvas, c, d, mark);
	} else
		canvas->stroke_rect(canvas, in, mark);
}


static void
example_draw_title(decor_window* window, decor_canvas* canvas, int32 index)
{
	const decor_tab* tab = window->tabs[index];
	float ascent, descent, leading;
	canvas->font_height(canvas, NULL, &ascent, &descent, &leading);

	const float height = tab->frame.bottom - tab->frame.top + 1;
	decor_point baseline = { tab->frame.left + tab->text_offset,
		floorf(tab->frame.top + (height - ascent - descent) / 2 + ascent) };
	canvas->set_drawing_mode(canvas, B_OP_OVER);
	canvas->draw_string(canvas, tab->shown_title, tab->shown_title_length,
		baseline, canvas->ui_color(canvas, tab->focused
			? B_WINDOW_TEXT_COLOR : B_WINDOW_INACTIVE_TEXT_COLOR));
	canvas->set_drawing_mode(canvas, B_OP_COPY);
}


static void
example_draw_tab(decor_window* window, decor_canvas* canvas, int32 index,
	decor_rect invalid)
{
	const decor_tab* tab = window->tabs[index];
	const rgb_color base = canvas->ui_color(canvas, tab->focused
		? B_WINDOW_TAB_COLOR : B_WINDOW_INACTIVE_TAB_COLOR);

	decor_gradient gradient;
	memset((void*)&gradient, 0, sizeof(gradient));
	gradient.start.x = gradient.end.x = tab->frame.left;
	gradient.start.y = tab->frame.top;
	gradient.end.y = tab->frame.bottom;
	gradient.stop_count = 2;
	gradient.colors[0] = shade(base, tab->highlighted ? 1.6f : 1.25f);
	gradient.colors[1] = base;
	gradient.offsets[1] = 255;
	canvas->fill_rect_gradient(canvas, tab->frame, &gradient);

	// the outline over the title bar, and lines between a stack's tabs
	const decor_rect& r = tab->frame;
	const rgb_color outline = shade(canvas->ui_color(canvas, tab->focused
		? B_WINDOW_BORDER_COLOR : B_WINDOW_INACTIVE_BORDER_COLOR), 0.6f);
	decor_point leftTop = { r.left, r.top }, rightTop = { r.right, r.top };
	decor_point leftBottom = { r.left, r.bottom };
	decor_point rightBottom = { r.right, r.bottom };
	canvas->stroke_line(canvas, leftTop, rightTop, outline);
	canvas->stroke_line(canvas, leftTop, leftBottom, outline);
	if (index == window->tab_count - 1)
		canvas->stroke_line(canvas, rightTop, rightBottom, outline);

	example_draw_title(window, canvas, index);
	for (int32 button = 0; button < DECOR_BUTTON_COUNT; button++) {
		if (intersects(tab->buttons[button], invalid))
			example_draw_button(window, canvas, index, button, invalid);
	}
}


static decor_style sStyle;


extern "C" const decor_style*
instantiate_decor_style(uint32 apiVersion)
{
	// Built for version 1, it runs on any app_server with styles. A style
	// built for a later version that should run on older ones too looks at
	// apiVersion here and fills in only what that version has.
	if (apiVersion < 1)
		return NULL;

	memset(&sStyle, 0, sizeof(sStyle));
	sStyle.size = sizeof(sStyle);
	sStyle.api_version = 1;
	sStyle.name = "Example";
	sStyle.update_font = &example_update_font;
	sStyle.layout = &example_layout;
	sStyle.draw_frame = &example_draw_frame;
	sStyle.draw_tab = &example_draw_tab;
	sStyle.draw_button = &example_draw_button;
	return &sStyle;
}
