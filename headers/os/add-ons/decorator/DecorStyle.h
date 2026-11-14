/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef _DECOR_STYLE_H
#define _DECOR_STYLE_H


/*!	Window decorator styles.

	A style describes how windows are framed: where the borders, tabs and
	buttons go, and how they're drawn. app_server's own decorator engine does
	everything else, such as tabs and stacking, moving and resizing, hit
	testing and highlights.

	Unlike a decorator add-on, which subclasses app_server's private classes
	and so only fits the build it was compiled for, a style only sees the
	plain structures and function tables below. It keeps working on later
	versions of app_server:
	- each structure with a \c size starts with it, and new fields are only
	  ever added at the end, so a style reads what it knows and checks
	  \c size before reading anything newer;
	- a window's tabs are reached through pointers, never by indexing an
	  array of decor_tab, so that decor_tab can grow;
	- the arrays indexed by DECOR_BUTTON_* and DECOR_REGION_* have room for
	  more than this version uses, so that new buttons and regions don't
	  move the fields after them;
	- a style names the version of this interface it was built for, and
	  app_server refuses one newer than its own.

	A style is an add-on in a "decorators" add-ons folder that exports

		extern "C" const decor_style* instantiate_decor_style(
			uint32 apiVersion);

	\c apiVersion is app_server's DECOR_STYLE_API_VERSION. Return NULL to
	decline, and app_server uses its own decorator.

	A style built for a later version than \c apiVersion is refused unless
	it returns a table whose \c api_version is \c apiVersion or lower. To
	run on an older app_server as well, check \c apiVersion, return a table
	for it, and use only what that version has: the hooks it calls, and the
	fields of the structures it passes, whose \c size says how far they go.

	A style runs inside app_server, which exports its own classes. Link it
	with -Wl,--no-undefined: a symbol the style lacks would otherwise bind
	to app_server's private code when it loads, and tie the style to that
	build after all.

	The header is C so that its names aren't mangled, not so that any
	compiler will do: the structures hold \c bool and some are passed by
	value, as the architecture's C ABI has them. Build a style for each
	architecture app_server runs on, with the compiler app_server is built
	with there (gcc 2.95 on x86_gcc2h).

	Coordinates are app_server's: screen pixels, with rectangles including
	their right and bottom edges, as BRect.
*/


#include <GraphicsDefs.h>
#include <InterfaceDefs.h>
#include <SupportDefs.h>


#define DECOR_STYLE_API_VERSION		1


#ifdef __cplusplus
extern "C" {
#endif


typedef struct decor_rect {
	float					left;
	float					top;
	float					right;
	float					bottom;
} decor_rect;

typedef struct decor_point {
	float					x;
	float					y;
} decor_point;


/*!	The room in the arrays indexed by DECOR_BUTTON_* and DECOR_REGION_*.
	These never change; a later version may use more of them, up to these.
*/
#define DECOR_BUTTON_CAPACITY	8
#define DECOR_REGION_CAPACITY	32


/*!	A tab's buttons. DECOR_BUTTON_COUNT is how many this version has, and
	may grow in a later one, up to DECOR_BUTTON_CAPACITY: a style lays out
	and draws those it knows, and the engine treats one left invalid as
	missing.
*/
enum {
	DECOR_BUTTON_CLOSE = 0,
	DECOR_BUTTON_ZOOM,
	DECOR_BUTTON_MINIMIZE,

	DECOR_BUTTON_COUNT
};

/*!	What a point on the frame does; the values of app_server's
	Decorator::Region. DECOR_REGION_COUNT may grow in a later version, up to
	DECOR_REGION_CAPACITY.
*/
enum {
	DECOR_REGION_NONE = 0,
	DECOR_REGION_TAB,
	DECOR_REGION_CLOSE_BUTTON,
	DECOR_REGION_ZOOM_BUTTON,
	DECOR_REGION_MINIMIZE_BUTTON,
	DECOR_REGION_LEFT_BORDER,
	DECOR_REGION_RIGHT_BORDER,
	DECOR_REGION_TOP_BORDER,
	DECOR_REGION_BOTTOM_BORDER,
	DECOR_REGION_LEFT_TOP_CORNER,
	DECOR_REGION_LEFT_BOTTOM_CORNER,
	DECOR_REGION_RIGHT_TOP_CORNER,
	DECOR_REGION_RIGHT_BOTTOM_CORNER,

	DECOR_REGION_COUNT,

	// from region_at(): decide as app_server's decorators do
	DECOR_REGION_DEFAULT = -1
};

// Region highlights, as app_server's Decorator
enum {
	DECOR_HIGHLIGHT_NONE = 0,
	DECOR_HIGHLIGHT_RESIZE_BORDER
};

// How a title is shortened to fit
enum {
	DECOR_TRUNCATE_END = 0,
	DECOR_TRUNCATE_MIDDLE,
	DECOR_TRUNCATE_BEGINNING
};

// The system fonts a style can base its title font on
enum {
	DECOR_FONT_PLAIN = 0,
	DECOR_FONT_BOLD,
	DECOR_FONT_FIXED,
	DECOR_FONT_MENU
		// the menus' font, at their font size
};

/*!	A font to measure text in: one of the system's, at \c size (or its own
	size if 0), with B_FORCE_ANTIALIASING and the like in \c flags.
*/
typedef struct decor_font {
	int32					which;
		// DECOR_FONT_*
	float					size;
	uint32					flags;
	float					rotation;
		// in degrees, counterclockwise: 90 for a title running up the
		// side of a window
} decor_font;


/*!	A window's tab: one per window in a stack. The engine fills in the
	"in" fields; layout() fills in the rest.
*/
typedef struct decor_tab {
	uint32					size;

	// in
	const char*				title;
	uint32					look;
		// window_look
	uint32					flags;
	bool					focused;
		// whether the window is active
	bool					button_focus;
		// whether its buttons are drawn as active: a floating window's
		// that avoids focus are, too
	bool					pressed[DECOR_BUTTON_CAPACITY];
		// by DECOR_BUTTON_*
	bool					highlighted;
		// whether the tab is highlighted, as when another window is
		// dragged over it to stack
	float					location;
		// where a tab that can slide was put, from the frame's left edge

	// out: layout()
	decor_rect				frame;
		// the whole tab, or title bar; invalid if it has none
	decor_rect				buttons[DECOR_BUTTON_CAPACITY];
		// by DECOR_BUTTON_*; invalid for those it hasn't
	float					text_offset;
		// from the tab's left edge to where its title starts
	float					title_room;
		// the width the title is shortened to
	uint32					truncate;
		// DECOR_TRUNCATE_*
	float					min_size;
	float					max_size;
		// widths a stack's tabs can shrink and grow to; the first tab's
		// min_size keeps the window at least that wide

	// out: the engine, from title_room
	const char*				shown_title;
		// the title as shortened, to draw
	int32					shown_title_length;
		// in bytes

	// the style's own: free to use from layout() on
	void*					cookie;
} decor_tab;


/*!	A window's frame. The engine fills in the "in" fields; layout() fills in
	the rest.
*/
typedef struct decor_window {
	uint32					size;

	// in
	decor_rect				frame;
		// the window's content
	int32					tab_count;
	decor_tab**				tabs;
		// tab_count pointers: use tabs[i]->title, never (*tabs)[i], since
		// decor_tab can grow
	int32					top_tab;
		// the one in front, whose look and flags the frame follows. There is
		// always at least one tab, and top_tab is one of them, except in
		// create_window(), delete_window() and update_colors(), which may
		// be called with none.
	uint8					highlights[DECOR_REGION_CAPACITY];
		// DECOR_HIGHLIGHT_* for each region, by DECOR_REGION_*
	bool					outline_resizing;
	decor_rect				outline_frame;
		// while the window is resized as an outline: the new content frame

	// out: layout()
	float					border_width;
	decor_rect				left_border;
	decor_rect				top_border;
	decor_rect				right_border;
	decor_rect				bottom_border;
		// invalid when there are none
	decor_rect				title_bar;
		// the tabs together; invalid without a tab
	decor_rect				resize;
		// the resize corner, invalid without one
	bool					tabs_slide;
		// whether a tab can be moved along the top, as on BeOS; the
		// engine keeps its location between 0 and the frame's width less
		// the tab's

	// the style's own: free to use from create_window() on
	void*					cookie;
} decor_window;


/*!	A gradient fill: linear, from \c start to \c end, through up to
	\c DECOR_GRADIENT_STOPS colours at offsets from 0 to 255.

	This structure is frozen: it has no \c size, and neither it nor
	DECOR_GRADIENT_STOPS will ever change. A later version that needs more
	adds a structure and a canvas call of its own.
*/
#define DECOR_GRADIENT_STOPS	8

typedef struct decor_gradient {
	decor_point				start;
	decor_point				end;
	int32					stop_count;
	rgb_color				colors[DECOR_GRADIENT_STOPS];
	uint8					offsets[DECOR_GRADIENT_STOPS];
} decor_gradient;


/*!	What a style draws with, and how it measures text. app_server provides
	it, clipped to the window's frame and what needs drawing. Not every call
	is available everywhere: layout() and its helpers may only measure.
*/
typedef struct decor_canvas decor_canvas;

struct decor_canvas {
	uint32					size;
	void*					cookie;
		// app_server's own

	// colours and modes
	rgb_color				(*ui_color)(decor_canvas* canvas,
								color_which which);
	void					(*set_drawing_mode)(decor_canvas* canvas,
								drawing_mode mode);
	void					(*set_blending_mode)(decor_canvas* canvas,
								source_alpha srcAlpha,
								alpha_function alphaFunction);
	void					(*set_high_color)(decor_canvas* canvas,
								rgb_color color);
	void					(*set_low_color)(decor_canvas* canvas,
								rgb_color color);
	void					(*set_pattern)(decor_canvas* canvas,
								const pattern* pattern);

	// shapes, in the colour given
	void					(*stroke_line)(decor_canvas* canvas,
								decor_point start, decor_point end,
								rgb_color color);
	void					(*stroke_point)(decor_canvas* canvas,
								decor_point point, rgb_color color);
	void					(*stroke_rect)(decor_canvas* canvas,
								decor_rect rect, rgb_color color);
	void					(*fill_rect)(decor_canvas* canvas,
								decor_rect rect, rgb_color color);
	void					(*fill_rect_gradient)(decor_canvas* canvas,
								decor_rect rect,
								const decor_gradient* gradient);
		// in the high colour and pattern set
	void					(*stroke_rect_pattern)(decor_canvas* canvas,
								decor_rect rect);
	void					(*fill_rect_pattern)(decor_canvas* canvas,
								decor_rect rect);

	// bitmaps: B_RGBA32 pixels, \a bytesPerRow apart, drawn from \a source
	// into \a destination in the drawing mode set
	void					(*draw_bitmap)(decor_canvas* canvas,
								const void* bits, int32 width, int32 height,
								int32 bytesPerRow, decor_rect source,
								decor_rect destination);

	// The title font, that draw_string() draws in: set it as a decor_font
	// says. The measuring calls measure in \a font, or in the title font if
	// it's NULL.
	void					(*set_font)(decor_canvas* canvas,
								const decor_font* font);
	float					(*font_size)(decor_canvas* canvas, int32 which);
		// the system font's own size
	void					(*font_height)(decor_canvas* canvas,
								const decor_font* font, float* ascent,
								float* descent, float* leading);
	float					(*string_width)(decor_canvas* canvas,
								const decor_font* font, const char* string,
								int32 length);
	int32					(*truncate_string)(decor_canvas* canvas,
								const decor_font* font, const char* string,
								uint32 mode, float width, char* result,
								int32 resultSize);
		// shortens \a string to \a width as DECOR_TRUNCATE_* \a mode says,
		// into \a result; returns the result's length in bytes
	void					(*draw_string)(decor_canvas* canvas,
								const char* string, int32 length,
								decor_point baseline, rgb_color color);
		// in the title font and the drawing mode set

	// the drawing goes straight to the screen rather than through the back
	// buffer, as for a button pressed down
	bool					(*copy_to_front)(decor_canvas* canvas);
	void					(*set_copy_to_front)(decor_canvas* canvas,
								bool enabled);
};


/*!	Collects a frame's footprint: what of the screen it covers. */
typedef struct decor_region decor_region;

struct decor_region {
	uint32					size;
	void*					cookie;
	void					(*include)(decor_region* region,
								decor_rect rect);
	void					(*exclude)(decor_region* region,
								decor_rect rect);
};


/*!	A style. Only \c size, \c api_version, \c name, \c layout, \c draw_frame,
	\c draw_tab and \c draw_button are required; the rest may be NULL, and
	the engine then does as app_server's own decorators do.
*/
typedef struct decor_style {
	uint32					size;
	uint32					api_version;
		// DECOR_STYLE_API_VERSION it was built for
	const char*				name;

	// a window's own state: set window->cookie, and free it again
	status_t				(*create_window)(decor_window* window,
								decor_canvas* canvas);
	void					(*delete_window)(decor_window* window);

	// chooses the title font with canvas->set_font(); called when a window
	// is created and when the system fonts change
	void					(*update_font)(decor_window* window,
								decor_canvas* canvas);

	// where everything goes, from window->frame and the tabs; may measure
	// text, not draw
	void					(*layout)(decor_window* window,
								decor_canvas* canvas);

	// drawing: \a invalid is what needs it
	void					(*draw_frame)(decor_window* window,
								decor_canvas* canvas, decor_rect invalid);
	void					(*draw_tab)(decor_window* window,
								decor_canvas* canvas, int32 tab,
								decor_rect invalid);
		// the whole tab: its background, title and buttons
	void					(*draw_title)(decor_window* window,
								decor_canvas* canvas, int32 tab);
		// only the title, as when it changes; NULL draws the whole tab
	void					(*draw_button)(decor_window* window,
								decor_canvas* canvas, int32 tab,
								int32 button, decor_rect invalid);
		// DECOR_BUTTON_*, pressed or not as tab->pressed says
	void					(*draw_outline)(decor_window* window,
								decor_canvas* canvas, decor_rect invalid);
		// the outline while resizing; NULL draws a dotted line

	// what is at \a where on the frame, and in which tab;
	// DECOR_REGION_DEFAULT leaves it to the engine
	int32					(*region_at)(decor_window* window,
								decor_point where, int32* tab);

	// what of the screen the frame covers; NULL covers the borders, the
	// title bar and the resize corner
	void					(*footprint)(decor_window* window,
								decor_region* region);

	// draws the whole frame within \a invalid, instead of draw_frame()
	// and draw_tab() for each tab
	void					(*draw)(decor_window* window,
								decor_canvas* canvas, decor_rect invalid);

	// keeps the window's content within these sizes, as it may already
	// have to be, in pixels
	void					(*size_limits)(decor_window* window,
								int32* minWidth, int32* minHeight,
								int32* maxWidth, int32* maxHeight);

	// the window moved by \a x and \a y; the engine has moved what it
	// knows of, from window->frame and the layout
	void					(*moved)(decor_window* window, float x, float y);

	// the system colours changed; called before the frame is drawn again
	void					(*update_colors)(decor_window* window,
								decor_canvas* canvas);
} decor_style;


#ifdef __cplusplus
}
#endif


#endif	// _DECOR_STYLE_H
