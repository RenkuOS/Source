/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


/*!	Tests BPrivate::get_window_preview(): a covered window's preview shows its
	own contents, previews follow changes, bad tokens and bitmaps fail
	cleanly, and windows closing during calls don't hurt. Prints PASS/FAIL
	lines to stdout and to /dev/dprintf.

	On a desktop that isn't composited, it only checks that the call fails
	with B_NOT_SUPPORTED. With --toggle-composited, it then switches the
	composited desktop setting (it applies at the next start of app_server)
	and restarts the computer after a composited run, or shuts it down after
	a non-composited one, for unattended runs in a VM.
*/


#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Application.h>
#include <Bitmap.h>
#include <InterfaceDefs.h>
#include <Roster.h>
#include <View.h>
#include <Window.h>

#include <AppServerLink.h>
#include <ServerProtocol.h>
#include <WindowInfo.h>
#include <WindowPrivate.h>


static int sFailures = 0;
static int sDprintf = -1;


static void
report(const char* format, ...)
{
	char line[512];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);

	printf("WPT: %s\n", line);
	fflush(stdout);
	if (sDprintf >= 0) {
		char buffer[600];
		int length = snprintf(buffer, sizeof(buffer), "WPT: %s\n", line);
		write(sDprintf, buffer, length);
	}
}


static void
check(bool ok, const char* format, ...)
{
	char line[480];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);

	if (!ok)
		sFailures++;
	report("%s %s", ok ? "PASS" : "FAIL", line);
}


// #pragma mark -


class ColorView : public BView {
public:
	ColorView(BRect frame, rgb_color color)
		:
		BView(frame, "color", B_FOLLOW_ALL, B_WILL_DRAW),
		fColor(color)
	{
	}

	virtual void Draw(BRect updateRect)
	{
		SetHighColor(fColor);
		FillRect(Bounds());
		// a marker at the left top, to see where things are
		SetHighColor(0, 0, 255);
		FillRect(BRect(20, 20, 59, 59));
		// and some text, to see how it scales
		SetHighColor(0, 0, 0);
		SetFontSize(24);
		DrawString(Window()->Title(), BPoint(80, 220));
	}

	void SetColor(rgb_color color)
	{
		fColor = color;
		Invalidate();
	}

private:
	rgb_color fColor;
};


class PreviewView : public BView {
public:
	PreviewView(BRect frame)
		:
		BView(frame, "previews", B_FOLLOW_ALL, B_WILL_DRAW),
		fCount(0)
	{
		SetViewColor(80, 80, 80);
	}

	void AddBitmap(BBitmap* bitmap, BPoint where)
	{
		fBitmaps[fCount] = bitmap;
		fPoints[fCount] = where;
		fCount++;
	}

	virtual void Draw(BRect updateRect)
	{
		for (int i = 0; i < fCount; i++)
			DrawBitmap(fBitmaps[i], fPoints[i]);
	}

private:
	BBitmap*	fBitmaps[4];
	BPoint		fPoints[4];
	int			fCount;
};


struct TestWindow {
	BWindow*	window;
	ColorView*	view;
	int32		token;
};


static int32
find_token(const char* title)
{
	int32 count = 0;
	int32* tokens = get_token_list(be_app->Team(), &count);
	int32 found = -1;
	for (int32 i = 0; tokens != NULL && i < count && found < 0; i++) {
		client_window_info* info = get_window_info(tokens[i]);
		if (info == NULL)
			continue;
		if (strcmp(info->name, title) == 0)
			found = tokens[i];
		free(info);
	}
	free(tokens);
	return found;
}


static void
sync_window(BWindow* window)
{
	if (window->Lock()) {
		window->ChildAt(0)->Sync();
		window->Unlock();
	}
	snooze(200000);
}


static TestWindow
open_window(const char* title, BRect frame, rgb_color color)
{
	TestWindow test;
	test.window = new BWindow(frame, title, B_TITLED_WINDOW,
		B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS);
	test.view = new ColorView(test.window->Bounds(), color);
	test.window->AddChild(test.view);
	test.window->Show();
	sync_window(test.window);
	test.token = find_token(title);
	return test;
}


static void
close_window(TestWindow& test)
{
	if (test.window->Lock())
		test.window->Quit();
	test.window = NULL;
}


static bool
pixel_is(BBitmap* bitmap, int32 x, int32 y, rgb_color color,
	rgb_color* _actual = NULL)
{
	const uint8* pixel = (const uint8*)bitmap->Bits()
		+ y * bitmap->BytesPerRow() + x * 4;
	rgb_color actual = make_color(pixel[2], pixel[1], pixel[0]);
	if (_actual != NULL)
		*_actual = actual;
	return abs(actual.red - color.red) <= 8
		&& abs(actual.green - color.green) <= 8
		&& abs(actual.blue - color.blue) <= 8;
}


/*!	Checks the preview pixel that shows the window's content pixel \a x, \a y.
*/
static void
check_content(BBitmap* bitmap, BRect content, int32 x, int32 y,
	rgb_color color, const char* what)
{
	int32 width = bitmap->Bounds().IntegerWidth() + 1;
	int32 height = bitmap->Bounds().IntegerHeight() + 1;
	int32 bitmapX = (int32)((int64)x * width / (content.IntegerWidth() + 1));
	int32 bitmapY = (int32)((int64)y * height / (content.IntegerHeight() + 1));
	rgb_color actual;
	bool ok = pixel_is(bitmap, bitmapX, bitmapY, color, &actual);
	check(ok, "%" B_PRId32 "x%" B_PRId32 ": %s at content %" B_PRId32 ",%"
		B_PRId32 " (bitmap %" B_PRId32 ",%" B_PRId32 ") is %d,%d,%d,"
		" expected %d,%d,%d", width, height, what, x, y, bitmapX, bitmapY,
		actual.red, actual.green, actual.blue, color.red, color.green,
		color.blue);
}


/*!	The desktop window, Tracker's, of any team.
*/
static int32
find_desktop_token()
{
	int32 count = 0;
	int32* tokens = get_token_list(-1, &count);
	int32 found = -1;
	for (int32 i = 0; tokens != NULL && i < count && found < 0; i++) {
		client_window_info* info = get_window_info(tokens[i]);
		if (info == NULL)
			continue;
		if (info->feel == (uint32)kDesktopWindowFeel)
			found = tokens[i];
		free(info);
	}
	free(tokens);
	return found;
}


static uint8
alpha_at(BBitmap* bitmap, int32 x, int32 y)
{
	return ((const uint8*)bitmap->Bits())[y * bitmap->BytesPerRow() + x * 4
		+ 3];
}


/*!	WINDOW_PREVIEW_FIT and WINDOW_PREVIEW_DECORATOR, on the window \a token,
	whose 400x300 content is \a color with a blue marker at 20,20 - 59,59.
*/
static void
test_flags(int32 token, rgb_color color)
{
	const rgb_color blue = make_color(0, 0, 255);
	BRect content(0, 0, 399, 299);

	// Without flags, all of the bitmap is used.
	{
		BBitmap bitmap(BRect(0, 0, 132, 299), B_RGB32);
		BRect used;
		status_t status = BPrivate::get_window_preview(token, &bitmap, 0,
			&used);
		check(status == B_OK && used == bitmap.Bounds(),
			"no flags: all of the bitmap is used (%s, %gx%g)",
			strerror(status), used.Width() + 1, used.Height() + 1);
	}

	// Fit: 4:3 in a square, at the left top, the rest untouched
	{
		BBitmap bitmap(BRect(0, 0, 199, 199), B_RGBA32);
		memset(bitmap.Bits(), 0x11, bitmap.BitsLength());
		BRect used;
		status_t status = BPrivate::get_window_preview(token, &bitmap,
			BPrivate::WINDOW_PREVIEW_FIT, &used);
		check(status == B_OK && used == BRect(0, 0, 199, 149),
			"fit 200x200: uses 200x150 (%s, %gx%g)", strerror(status),
			used.Width() + 1, used.Height() + 1);
		check(pixel_is(&bitmap, 20, 20, blue)
			&& pixel_is(&bitmap, 100, 100, color),
			"fit 200x200: the marker and the content are in place");
		const uint8* below = (const uint8*)bitmap.Bits()
			+ 150 * bitmap.BytesPerRow();
		bool untouched = true;
		for (int32 i = 0; i < 50 * bitmap.BytesPerRow(); i++)
			untouched &= below[i] == 0x11;
		check(untouched, "fit 200x200: the rest of the bitmap is untouched");
	}

	// Fit never scales up: 1:1 in a bigger bitmap
	{
		BBitmap bitmap(BRect(0, 0, 799, 599), B_RGB32);
		BRect used;
		status_t status = BPrivate::get_window_preview(token, &bitmap,
			BPrivate::WINDOW_PREVIEW_FIT, &used);
		check(status == B_OK && used == content,
			"fit 800x600: uses 400x300 (%s, %gx%g)", strerror(status),
			used.Width() + 1, used.Height() + 1);
		check(pixel_is(&bitmap, 19, 19, color)
			&& pixel_is(&bitmap, 20, 20, blue)
			&& pixel_is(&bitmap, 59, 59, blue)
			&& pixel_is(&bitmap, 60, 60, color),
			"fit 800x600: 1:1, the marker's edges are exact");
	}

	// The decorator, 1:1: the content is inside the border, below the tab,
	// and what is beside the tab is transparent.
	{
		BBitmap bitmap(BRect(0, 0, 999, 999), B_RGBA32);
		BRect used;
		status_t status = BPrivate::get_window_preview(token, &bitmap,
			BPrivate::WINDOW_PREVIEW_FIT | BPrivate::WINDOW_PREVIEW_DECORATOR,
			&used);
		int32 width = used.IntegerWidth() + 1;
		int32 height = used.IntegerHeight() + 1;
		check(status == B_OK && width > 400 && height > 300,
			"decorator: bigger than the content (%s, %" B_PRId32 "x%" B_PRId32
			")", strerror(status), width, height);
		if (status == B_OK && width > 400 && height > 300) {
			int32 border = (width - 400) / 2;
			int32 top = height - 300 - border;
			check(pixel_is(&bitmap, border + 20, top + 20, blue)
				&& pixel_is(&bitmap, border + 19, top + 19, color)
				&& alpha_at(&bitmap, border + 200, top + 150) == 255,
				"decorator: the content is at %" B_PRId32 ",%" B_PRId32,
				border, top);
			check(alpha_at(&bitmap, width / 2, height - 1) == 255
				&& alpha_at(&bitmap, 0, height / 2) == 255,
				"decorator: the border is opaque");
			check(alpha_at(&bitmap, width - 2, 1) == 0,
				"decorator: beside the tab is transparent (%d)",
				alpha_at(&bitmap, width - 2, 1));
		}
	}

	// The decorator, scaled to a thumbnail
	{
		BBitmap bitmap(BRect(0, 0, 159, 159), B_RGBA32);
		BRect used;
		status_t status = BPrivate::get_window_preview(token, &bitmap,
			BPrivate::WINDOW_PREVIEW_FIT | BPrivate::WINDOW_PREVIEW_DECORATOR,
			&used);
		check(status == B_OK && used.IntegerWidth() == 159
			&& used.IntegerHeight() < 159,
			"decorator, fit 160x160: %s, %gx%g", strerror(status),
			used.Width() + 1, used.Height() + 1);
	}
}


static int64
app_server_memory()
{
	app_info info;
	if (be_roster->GetAppInfo("application/x-vnd.Haiku-app_server", &info)
			!= B_OK) {
		return -1;
	}
	int64 total = 0;
	ssize_t cookie = 0;
	area_info area;
	while (get_next_area_info(info.team, &cookie, &area) == B_OK)
		total += area.ram_size;
	return total;
}


// #pragma mark - closing windows during calls


struct RaceState {
	volatile int32	token;
	volatile bool	quit;
	int32			ok;
	int32			badValue;
	int32			notAllowed;
	int32			other;
	status_t		otherCode;
};


static status_t
race_thread(void* data)
{
	RaceState* state = (RaceState*)data;
	BBitmap bitmap(BRect(0, 0, 99, 74), B_RGB32);
	while (!state->quit) {
		status_t status = BPrivate::get_window_preview(state->token, &bitmap);
		if (status == B_OK)
			state->ok++;
		else if (status == B_BAD_VALUE)
			state->badValue++;
		else if (status == B_NOT_ALLOWED)
			state->notAllowed++;
		else {
			state->other++;
			state->otherCode = status;
		}
	}
	return B_OK;
}


static void
test_closing_windows()
{
	RaceState state;
	memset(&state, 0, sizeof(state));
	state.token = -1;
	thread_id thread = spawn_thread(race_thread, "preview race",
		B_NORMAL_PRIORITY, &state);
	resume_thread(thread);

	const int kWindows = 100;
	for (int i = 0; i < kWindows; i++) {
		char title[32];
		snprintf(title, sizeof(title), "race %d", i);
		BWindow* window = new BWindow(BRect(500, 400, 699, 549), title,
			B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
		window->AddChild(new ColorView(window->Bounds(),
			make_color(i * 2, 128, 255 - i * 2)));
		window->Show();
		if (window->Lock()) {
			window->ChildAt(0)->Sync();
			window->Unlock();
		}
		state.token = find_token(title);
		snooze((i % 5) * 4000);
		if (window->Lock())
			window->Quit();
		snooze((i % 3) * 3000);
	}

	state.quit = true;
	status_t result;
	wait_for_thread(thread, &result);

	report("closing windows: %d windows, %" B_PRId32 " B_OK, %" B_PRId32
		" B_BAD_VALUE, %" B_PRId32 " B_NOT_ALLOWED, %" B_PRId32 " other",
		kWindows, state.ok, state.badValue, state.notAllowed, state.other);
	check(state.other == 0, "closing windows: no unexpected results (last "
		"other: 0x%" B_PRIx32 ")", state.other > 0 ? state.otherCode : 0);
	check(state.ok > 0 && state.badValue > 0,
		"closing windows: saw both live and gone windows");
}


// #pragma mark -


static bigtime_t
time_calls(int32 token, BBitmap* bitmap, int count)
{
	bigtime_t start = system_time();
	for (int i = 0; i < count; i++)
		BPrivate::get_window_preview(token, bitmap);
	return (system_time() - start) / count;
}


static void
test_composited()
{
	const rgb_color red = make_color(255, 0, 0);
	const rgb_color green = make_color(0, 255, 0);
	const rgb_color blue = make_color(0, 0, 255);
	const rgb_color yellow = make_color(255, 255, 0);

	// A is mostly covered by B: its right bottom part is under B.
	BRect frameA(100, 100, 499, 399);
	TestWindow a = open_window("preview A", frameA, red);
	TestWindow b = open_window("preview B", BRect(200, 180, 599, 479), green);
	check(a.token >= 0 && b.token >= 0, "found tokens: A %" B_PRId32
		", B %" B_PRId32, a.token, b.token);

	BRect content(0, 0, 399, 299);
	const int32 sizes[][2] = {
		{ 400, 300 }, { 200, 150 }, { 100, 75 }, { 37, 23 }, { 800, 600 },
		{ 133, 300 }
	};
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		BBitmap bitmap(BRect(0, 0, sizes[i][0] - 1, sizes[i][1] - 1),
			B_RGB32);
		status_t status = BPrivate::get_window_preview(a.token, &bitmap);
		check(status == B_OK, "%" B_PRId32 "x%" B_PRId32 ": get_window_preview"
			" returns %s", sizes[i][0], sizes[i][1], strerror(status));
		if (status != B_OK)
			continue;
		check_content(&bitmap, content, 40, 40, blue, "marker");
		check_content(&bitmap, content, 5, 290, red, "visible part");
		check_content(&bitmap, content, 200, 150, red, "covered centre");
		check_content(&bitmap, content, 350, 250, red, "covered corner");
	}

	// At 1:1, the pixels are exact, up to the marker's edges.
	{
		BBitmap bitmap(BRect(0, 0, 399, 299), B_RGBA32);
		status_t status = BPrivate::get_window_preview(a.token, &bitmap);
		check(status == B_OK, "B_RGBA32 400x300: %s", strerror(status));
		check(pixel_is(&bitmap, 19, 19, red) && pixel_is(&bitmap, 20, 20, blue)
			&& pixel_is(&bitmap, 59, 59, blue)
			&& pixel_is(&bitmap, 60, 60, red),
			"1:1: the marker's edges are exact");
		const uint8* pixel = (const uint8*)bitmap.Bits();
		check(pixel[3] == 255, "B_RGBA32: an opaque window is opaque (%d)",
			pixel[3]);
	}

	// B's preview is all its own, too
	{
		BBitmap bitmap(BRect(0, 0, 199, 149), B_RGB32);
		status_t status = BPrivate::get_window_preview(b.token, &bitmap);
		check(status == B_OK, "B: %s", strerror(status));
		check_content(&bitmap, content, 200, 150, green, "B centre");
		check_content(&bitmap, content, 40, 40, blue, "B marker");
	}

	// It follows changes
	{
		if (a.window->Lock()) {
			a.view->SetColor(yellow);
			a.window->Unlock();
		}
		sync_window(a.window);
		BBitmap bitmap(BRect(0, 0, 199, 149), B_RGB32);
		status_t status = BPrivate::get_window_preview(a.token, &bitmap);
		check(status == B_OK, "after a change: %s", strerror(status));
		check_content(&bitmap, content, 200, 150, yellow, "changed, covered");
		check_content(&bitmap, content, 5, 290, yellow, "changed, visible");
	}

	// A is yellow now
	test_flags(a.token, yellow);

	// The desktop window, for a wallpaper preview
	{
		int32 desktop = find_desktop_token();
		BBitmap bitmap(BRect(0, 0, 159, 119), B_RGB32);
		BRect used;
		status_t status = desktop < 0 ? B_NAME_NOT_FOUND
			: BPrivate::get_window_preview(desktop, &bitmap,
				BPrivate::WINDOW_PREVIEW_FIT, &used);
		check(status == B_OK && used.IsValid(),
			"desktop window %" B_PRId32 ": %s", desktop, strerror(status));
	}

	// Errors
	{
		BBitmap bitmap(BRect(0, 0, 99, 74), B_RGB32);
		status_t status = BPrivate::get_window_preview(0x7ffffff0, &bitmap);
		check(status == B_BAD_VALUE, "unknown token: %s", strerror(status));
		status = BPrivate::get_window_preview(-1, &bitmap);
		check(status == B_BAD_VALUE, "token -1: %s", strerror(status));
		status = BPrivate::get_window_preview(a.token, NULL);
		check(status == B_BAD_VALUE, "NULL bitmap: %s", strerror(status));

		BBitmap cmap(BRect(0, 0, 99, 74), B_CMAP8);
		status = BPrivate::get_window_preview(a.token, &cmap);
		check(status == B_BAD_VALUE, "B_CMAP8 bitmap: %s", strerror(status));

		BBitmap local(BRect(0, 0, 99, 74), B_BITMAP_NO_SERVER_LINK, B_RGB32);
		status = BPrivate::get_window_preview(a.token, &local);
		check(status == B_BAD_VALUE, "bitmap without server link: %s",
			strerror(status));

		a.window->Lock();
		a.window->Hide();
		a.window->Unlock();
		snooze(200000);
		status = BPrivate::get_window_preview(a.token, &bitmap);
		check(status == B_NOT_ALLOWED, "hidden window: %s", strerror(status));
		a.window->Lock();
		a.window->Show();
		a.window->Unlock();
		sync_window(a.window);
		status = BPrivate::get_window_preview(a.token, &bitmap);
		check(status == B_OK, "shown again: %s", strerror(status));

		a.window->Lock();
		a.window->Minimize(true);
		a.window->Unlock();
		snooze(300000);
		status = BPrivate::get_window_preview(a.token, &bitmap);
		check(status == B_NOT_ALLOWED, "minimized window: %s",
			strerror(status));
		a.window->Lock();
		a.window->Minimize(false);
		a.window->Unlock();
		sync_window(a.window);
	}

	// Every token there could be, of every app and offscreen windows too
	{
		BBitmap bitmap(BRect(0, 0, 63, 47), B_RGB32);
		int32 ok = 0;
		int32 bad = 0;
		int32 notAllowed = 0;
		int32 other = 0;
		for (int32 token = 0; token < 4096; token++) {
			status_t status = BPrivate::get_window_preview(token, &bitmap);
			if (status == B_OK)
				ok++;
			else if (status == B_BAD_VALUE)
				bad++;
			else if (status == B_NOT_ALLOWED)
				notAllowed++;
			else
				other++;
		}
		report("tokens 0-4095: %" B_PRId32 " B_OK, %" B_PRId32 " B_BAD_VALUE, %"
			B_PRId32 " B_NOT_ALLOWED, %" B_PRId32 " other", ok, bad,
			notAllowed, other);
		check(other == 0 && ok >= 2, "tokens 0-4095: only expected results");
	}

	// Timing
	{
		TestWindow big = open_window("preview big", BRect(40, 40, 1023, 727),
			make_color(30, 60, 90));
		struct {
			int32 token;
			int32 width;
			int32 height;
			const char* what;
		} cases[] = {
			{ a.token, 200, 150, "400x300 window" },
			{ a.token, 400, 300, "400x300 window" },
			{ big.token, 330, 231, "984x688 window" },
			{ big.token, 492, 344, "984x688 window" },
			{ big.token, 984, 688, "984x688 window" },
		};
		for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			BBitmap bitmap(BRect(0, 0, cases[i].width - 1,
				cases[i].height - 1), B_RGB32);
			time_calls(cases[i].token, &bitmap, 5);
			bigtime_t perCall = time_calls(cases[i].token, &bitmap, 100);
			report("timing: %s into %" B_PRId32 "x%" B_PRId32 ": %" B_PRId64
				" us per call", cases[i].what, cases[i].width,
				cases[i].height, perCall);
		}
		close_window(big);
	}

	// Leaks: app_server's memory before and after many calls
	{
		BBitmap bitmap(BRect(0, 0, 199, 149), B_RGB32);
		time_calls(a.token, &bitmap, 200);
		int64 before = app_server_memory();
		time_calls(a.token, &bitmap, 5000);
		report("timing: unknown token (the round trip alone): %" B_PRId64
			" us per call", time_calls(0x7ffffff0, &bitmap, 5000));
		int64 after = app_server_memory();
		report("app_server memory: %" B_PRId64 " KiB before, %" B_PRId64
			" KiB after 10000 calls", before / 1024, after / 1024);
		check(after - before < 256 * 1024, "app_server memory didn't grow "
			"by more than 256 KiB");
	}

	test_closing_windows();

	// Show what the previews look like, for a screenshot: A's, covered by B
	// again, at three sizes, and B's.
	{
		b.window->Lock();
		b.window->Activate();
		b.window->Unlock();
		if (a.window->Lock()) {
			a.view->SetColor(red);
			a.window->Unlock();
		}
		sync_window(a.window);

		PreviewView* view = new PreviewView(BRect(0, 0, 659, 299));
		const BRect frames[] = {
			BRect(10, 10, 109, 84), BRect(120, 10, 279, 129),
			BRect(290, 10, 649, 279), BRect(10, 140, 109, 214)
		};
		const int32 tokens[] = { a.token, a.token, a.token, b.token };
		status_t status = B_OK;
		for (int i = 0; i < 4; i++) {
			BBitmap* bitmap = new BBitmap(frames[i].OffsetToCopy(0, 0),
				B_RGB32);
			status_t result = BPrivate::get_window_preview(tokens[i], bitmap);
			if (result != B_OK)
				status = result;
			view->AddBitmap(bitmap, frames[i].LeftTop());
		}
		check(status == B_OK, "app_server is still fine after all that");

		BWindow* window = new BWindow(BRect(610, 500, 1269, 799), "previews",
			B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
		window->AddChild(view);
		window->Show();
	}
}


static void
set_composited(bool composited)
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_SET_COMPOSITED_DESKTOP);
	link.Attach<bool>(composited);
	link.Flush();
}


int
main(int argc, char** argv)
{
	bool toggle = argc > 1 && strcmp(argv[1], "--toggle-composited") == 0;
	sDprintf = open("/dev/dprintf", O_WRONLY);

	BApplication app("application/x-vnd.Renku-WindowPreviewTest");

	// give the desktop time to settle after booting
	if (toggle)
		snooze(15000000);

	bool composited = is_composited_desktop();
	report("start, composited desktop: %s", composited ? "yes" : "no");

	if (composited)
		test_composited();
	else {
		TestWindow a = open_window("preview A", BRect(100, 100, 499, 399),
			make_color(255, 0, 0));
		BBitmap bitmap(BRect(0, 0, 199, 149), B_RGB32);
		status_t status = BPrivate::get_window_preview(a.token, &bitmap);
		check(status == B_NOT_SUPPORTED, "not composited: %s",
			strerror(status));
		status = BPrivate::get_window_preview(0x7ffffff0, &bitmap);
		check(status == B_NOT_SUPPORTED, "not composited, unknown token: %s",
			strerror(status));
	}

	report("END %d failures", sFailures);

	if (toggle) {
		// time for a screenshot
		snooze(20000000);
		set_composited(!composited);
		snooze(1000000);
		system(composited ? "shutdown -r" : "shutdown -q");
	}
	return sFailures == 0 ? 0 : 1;
}
