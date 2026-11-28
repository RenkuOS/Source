/*
 * Copyright 2026, Dru Satori, dru@druware.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	Regression test: app_server must remove the EventTarget of an offscreen
	window (a BBitmap that accepts views) from its EventDispatcher when the
	window is deleted. A BScrollBar registers an event listener by itself,
	so before the fix every deleted bitmap holding one left a dangling
	target that the event loop read on the next input event.

	The test leaks such targets, churns windows to reuse the freed memory,
	then moves the pointer itself and checks that a pointer listener in an
	on-screen window still gets B_MOUSE_MOVED, i.e. that the event loop is
	alive. It exits with 0 on success and 1 on failure.

	It needs a running desktop session and it moves the pointer. It is not
	fully deterministic: the bug is a use after free, so an unfixed
	app_server crashes only once the freed memory has been reused. Several
	rounds make that very likely, but not certain. When app_server does
	crash, the test fails and app_server stays stopped in the debugger.
*/


#include <stdio.h>
#include <unistd.h>

#include <Application.h>
#include <Bitmap.h>
#include <Button.h>
#include <ScrollBar.h>
#include <View.h>
#include <Window.h>
#include <WindowScreen.h>


static const int32 kRounds = 5;
static const int32 kBitmaps = 20;
static const int32 kChurnWindows = 40;
static const bigtime_t kTimeout = 120000000;

static int32 sMouseMoves = 0;


class PointerListener : public BView {
public:
	PointerListener(BRect frame)
		:
		BView(frame, "listener", B_FOLLOW_ALL, 0)
	{
	}

	virtual void AttachedToWindow()
	{
		SetEventMask(B_POINTER_EVENTS);
	}

	virtual void MouseMoved(BPoint, uint32, const BMessage*)
	{
		atomic_add(&sMouseMoves, 1);
	}
};


static void
leak_offscreen_targets()
{
	BBitmap* bitmaps[kBitmaps];
	for (int32 i = 0; i < kBitmaps; i++) {
		bitmaps[i] = new BBitmap(BRect(0, 0, 99, 99), B_RGB32, true);
		if (bitmaps[i]->Lock()) {
			bitmaps[i]->AddChild(new BScrollBar(BRect(80, 0, 99, 79), "sb",
				NULL, 0, 100, B_VERTICAL));
			bitmaps[i]->Unlock();
		}
	}
	for (int32 i = 0; i < kBitmaps; i++)
		delete bitmaps[i];
}


static void
churn_windows()
{
	for (int32 i = 0; i < kChurnWindows; i++) {
		BWindow* window = new BWindow(BRect(50, 50, 400, 300), "churn",
			B_TITLED_WINDOW, 0);
		for (int32 j = 0; j < 60; j++) {
			window->AddChild(new BButton(BRect(0, j, 80, j + 20), "b", "b",
				NULL));
		}
		window->Show();
		window->Lock();
		window->Quit();
	}
}


static void
fail(const char* reason)
{
	// Do not run any destructor: they would wait for a dead app_server.
	printf("FAIL: %s\n", reason);
	fflush(stdout);
	_exit(1);
}


static bool
event_loop_alive()
{
	int32 before = atomic_get(&sMouseMoves);
	for (int32 i = 0; i < 50; i++) {
		set_mouse_position(100 + (i % 2) * 200, 100 + (i % 5) * 40);
		snooze(10000);
	}
	for (int32 i = 0; i < 200; i++) {
		if (atomic_get(&sMouseMoves) != before)
			return true;
		snooze(10000);
	}
	return false;
}


static status_t
watchdog(void*)
{
	// app_server blocks every client once its event loop died.
	snooze(kTimeout);
	fail("app_server did not answer in time");
	return B_OK;
}


int
main()
{
	BApplication app("application/x-vnd.Haiku-OffscreenEventTarget");
	resume_thread(spawn_thread(watchdog, "watchdog", B_NORMAL_PRIORITY,
		NULL));

	BWindow* window = new BWindow(BRect(500, 500, 599, 599), "listener",
		B_TITLED_WINDOW, B_NOT_ZOOMABLE);
	window->AddChild(new PointerListener(window->Bounds()));
	window->Show();

	if (!event_loop_alive())
		fail("no pointer events before the test started");

	for (int32 round = 0; round < kRounds; round++) {
		leak_offscreen_targets();
		churn_windows();
		if (!event_loop_alive())
			fail("app_server stopped delivering pointer events");
	}

	window->Lock();
	window->Quit();
	printf("PASS\n");
	return 0;
}
