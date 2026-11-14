/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 *
 * Checks that applications can follow the user interface between light and
 * dark with ui_is_dark() and B_COLORS_UPDATED, swapping their artwork as they
 * would swap icons.
 *
 *   DarkModeTest watch          windows that follow it, and log each change
 *   DarkModeTest report         has a running watcher log its views' state
 *   DarkModeTest quit           quits a running watcher
 *   DarkModeTest probe          prints what an application sees
 *   DarkModeTest probe-noapp    the same without a BApplication
 *   DarkModeTest sweep FROM TO MS
 *                               makes the panel each grey in turn, MS apart,
 *                               with white text on the darker half
 *   DarkModeTest set PANEL TEXT sets those two colours, as RRGGBB
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <LayoutBuilder.h>
#include <Messenger.h>
#include <OS.h>
#include <String.h>
#include <View.h>
#include <Window.h>


static const char* kWatcherSignature
	= "application/x-vnd.Renku-DarkModeTest";
static const char* kDriverSignature
	= "application/x-vnd.Renku-DarkModeTest-driver";
static const uint32 kReport = 'rprt';


// Follows the pattern documented with ui_is_dark(). Its artwork stands in for
// an application's icons: a disc that has to show up on the panel behind it.
class ArtworkView : public BView {
public:
	ArtworkView(const char* name)
		:
		BView(name, B_WILL_DRAW),
		fDark(false),
		fSwaps(0),
		fMessages(0)
	{
		SetExplicitMinSize(BSize(280, 80));
	}

	virtual void AttachedToWindow()
	{
		BView::AttachedToWindow();
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
		SetHighUIColor(B_PANEL_TEXT_COLOR);
		fDark = ui_is_dark();
		_LoadArtwork();
		printf("attach %s dark=%d\n", Name(), fDark);
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == B_COLORS_UPDATED) {
			fMessages++;
			if (ui_is_dark() != fDark) {
				fDark = !fDark;
				fSwaps++;
				_LoadArtwork();
				printf("swap %s dark=%d\n", Name(), fDark);
				Invalidate();
			}
		}
		BView::MessageReceived(message);
	}

	virtual void Draw(BRect)
	{
		SetHighColor(fArtwork);
		FillEllipse(BRect(10, 10, 60, 60));

		SetHighUIColor(B_PANEL_TEXT_COLOR);
		BString text;
		text << Name() << ": " << (fDark ? "dark" : "light") << " artwork";
		DrawString(text, BPoint(72, 30));
		text = "";
		text << fSwaps << " swaps, " << fMessages << " messages";
		DrawString(text, BPoint(72, 50));
	}

	void Report()
	{
		printf("report %s tracked=%d actual=%d %s swaps=%" B_PRId32
			" messages=%" B_PRId32 "\n", Name(), fDark, ui_is_dark(),
			fDark == ui_is_dark() ? "OK" : "WRONG", fSwaps, fMessages);
	}

private:
	void _LoadArtwork()
	{
		fArtwork = fDark ? make_color(250, 220, 80) : make_color(20, 40, 120);
	}

	bool		fDark;
	rgb_color	fArtwork;
	int32		fSwaps;
	int32		fMessages;
};


class WatchWindow : public BWindow {
public:
	WatchWindow()
		:
		BWindow(BRect(40, 60, 340, 260), "Dark mode test", B_TITLED_WINDOW,
			B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS | B_NOT_ZOOMABLE)
	{
		fTop = new ArtworkView("top");
		fNested = new ArtworkView("nested");

		// a level down, to see that children get the message
		BGroupView* group = new BGroupView(B_VERTICAL);
		BLayoutBuilder::Group<>(group, B_VERTICAL, 0)
			.Add(fNested);
		BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
			.Add(fTop)
			.Add(group);
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == kReport) {
			fTop->Report();
			fNested->Report();
		} else
			BWindow::MessageReceived(message);
	}

	virtual bool QuitRequested()
	{
		be_app->PostMessage(B_QUIT_REQUESTED);
		return true;
	}

private:
	ArtworkView*	fTop;
	ArtworkView*	fNested;
};


class WatchApp : public BApplication {
public:
	WatchApp()
		:
		BApplication(kWatcherSignature)
	{
	}

	virtual void ReadyToRun()
	{
		fWindow = new WatchWindow();
		fWindow->Show();
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == kReport)
			fWindow->PostMessage(message);
		else
			BApplication::MessageReceived(message);
	}

private:
	BWindow*	fWindow;
};


static void
probe(const char* label)
{
	rgb_color panel = ui_color(B_PANEL_BACKGROUND_COLOR);
	rgb_color text = ui_color(B_PANEL_TEXT_COLOR);
	printf("%s panel=#%02X%02X%02X text=#%02X%02X%02X ui_is_dark=%d\n", label,
		panel.red, panel.green, panel.blue, text.red, text.green, text.blue,
		ui_is_dark());
}


static int
sweep(int from, int to, int milliseconds)
{
	const int step = from <= to ? 1 : -1;
	for (int grey = from; ; grey += step) {
		BMessage colors;
		colors.AddColor(ui_color_name(B_PANEL_BACKGROUND_COLOR),
			make_color(grey, grey, grey));
		const uint8 text = grey < 128 ? 255 : 0;
		colors.AddColor(ui_color_name(B_PANEL_TEXT_COLOR),
			make_color(text, text, text));
		set_ui_colors(&colors);
		if (milliseconds > 0)
			snooze(milliseconds * 1000);
		if (grey == to)
			return 0;
	}
}


static rgb_color
parse_color(const char* text)
{
	const unsigned long value = strtoul(text, NULL, 16);
	return make_color((value >> 16) & 0xff, (value >> 8) & 0xff, value & 0xff);
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	const char* command = argc > 1 ? argv[1] : "";
	if (strcmp(command, "probe-noapp") == 0) {
		probe("no application:");
		return 0;
	}
	if (strcmp(command, "watch") == 0) {
		WatchApp app;
		app.Run();
		return 0;
	}

	BApplication app(kDriverSignature);
	if (strcmp(command, "report") == 0)
		return BMessenger(kWatcherSignature).SendMessage(kReport);
	if (strcmp(command, "quit") == 0)
		return BMessenger(kWatcherSignature).SendMessage(B_QUIT_REQUESTED);
	if (strcmp(command, "probe") == 0) {
		probe("application:");
		return 0;
	}
	if (strcmp(command, "sweep") == 0 && argc == 5)
		return sweep(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
	if (strcmp(command, "set") == 0 && argc == 4) {
		BMessage colors;
		colors.AddColor(ui_color_name(B_PANEL_BACKGROUND_COLOR),
			parse_color(argv[2]));
		colors.AddColor(ui_color_name(B_PANEL_TEXT_COLOR),
			parse_color(argv[3]));
		set_ui_colors(&colors);
		return 0;
	}

	fprintf(stderr, "Usage: see the top of DarkModeTest.cpp\n");
	return 2;
}
