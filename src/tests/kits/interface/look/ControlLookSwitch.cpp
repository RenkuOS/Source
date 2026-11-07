/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 *
 * Checks that running applications follow a change of the control look.
 *
 *   ControlLookSwitchTest watch [stress]   windows that log each update; with
 *                                          "stress", they also redraw all the
 *                                          time and threads call the look
 *                                          without holding any lock
 *   ControlLookSwitchTest quit             quits a running watcher
 *   ControlLookSwitchTest set PATH         sets the look ("" for the default)
 *   ControlLookSwitchTest get              prints what the system and this
 *                                          team use
 *   ControlLookSwitchTest sweep N MS PATH...
 *                                          sets each PATH in turn, N rounds,
 *                                          MS apart
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <typeinfo>

#include <Application.h>
#include <Button.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <OS.h>
#include <PopUpMenu.h>
#include <RadioButton.h>
#include <ScrollView.h>
#include <Slider.h>
#include <String.h>
#include <TabView.h>
#include <TextControl.h>
#include <Window.h>

#include <InterfacePrivate.h>


static const char* kWatcherSignature
	= "application/x-vnd.Renku-ControlLookSwitchTest";
static const char* kDriverSignature
	= "application/x-vnd.Renku-ControlLookSwitchTest-driver";
static const uint32 kRedraw = 'rdrw';
static const int32 kStressThreads = 4;

static int32 sQuitting = 0;


class LogView : public BView {
public:
	LogView(const char* name, BButton* button)
		:
		BView(name, B_WILL_DRAW),
		fButton(button)
	{
		// A plain view's minimum size would otherwise be its current size.
		SetExplicitMinSize(BSize(10, 10));
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == B_CONTROL_LOOK_UPDATED) {
			BSize size = fButton->PreferredSize();
			printf("update view=%s look=%s spacing=%g button=%gx%g\n", Name(),
				typeid(*be_control_look).name(),
				be_control_look->DefaultItemSpacing(), size.width,
				size.height);
		}
		BView::MessageReceived(message);
	}

private:
	BButton*	fButton;
};


class LayoutWindow : public BWindow {
public:
	LayoutWindow(bool stress)
		:
		BWindow(BRect(40, 40, 480, 440), "Layout", B_TITLED_WINDOW,
			B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS),
		fRedraw(NULL)
	{
		BMenuBar* menuBar = new BMenuBar("menu");
		BMenu* menu = new BMenu("File");
		menu->AddItem(new BMenuItem("Open" B_UTF8_ELLIPSIS, NULL, 'O'));
		menu->AddItem(new BMenuItem("Quit", NULL, 'Q'));
		menuBar->AddItem(menu);

		BPopUpMenu* popUp = new BPopUpMenu("One");
		popUp->AddItem(new BMenuItem("One", NULL));
		popUp->AddItem(new BMenuItem("Two", NULL));

		BListView* list = new BListView("list");
		for (int32 i = 0; i < 40; i++) {
			BString label;
			label.SetToFormat("Item %" B_PRId32, i);
			list->AddItem(new BStringItem(label));
		}

		BSlider* slider = new BSlider("slider", "Slider", NULL, 0, 100,
			B_HORIZONTAL);
		slider->SetHashMarks(B_HASH_MARKS_BOTTOM);
		slider->SetHashMarkCount(5);

		BButton* button = new BButton("button", "Button", NULL);
		LogView* inner = new LogView("inner", button);
		LogView* outer = new LogView("outer", button);

		BTabView* tabs = new BTabView("tabs");
		BView* first = BLayoutBuilder::Group<>(B_VERTICAL)
			.Add(new BCheckBox("Check box"))
			.Add(new BRadioButton("Radio button", NULL))
			.Add(new BTextControl("Text", "text", NULL))
			.Add(new BMenuField("Menu", popUp))
			.Add(slider)
			.View();
		first->SetName("First");
		tabs->AddTab(first);
		BView* second = new BScrollView("Second", list, 0, false, true);
		tabs->AddTab(second);

		BLayoutBuilder::Group<>(outer, B_VERTICAL)
			.Add(inner)
			.End();

		BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
			.Add(menuBar)
			.AddGroup(B_VERTICAL)
				.SetInsets(B_USE_WINDOW_SPACING)
				.Add(tabs)
				.Add(button)
				.Add(outer)
				.End();

		if (stress) {
			BMessage redraw(kRedraw);
			fRedraw = new BMessageRunner(BMessenger(this), &redraw, 16000);
		}
	}

	virtual ~LayoutWindow()
	{
		delete fRedraw;
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == kRedraw)
			_Invalidate(ChildAt(0));
		else
			BWindow::MessageReceived(message);
	}

	virtual bool QuitRequested()
	{
		be_app->PostMessage(B_QUIT_REQUESTED);
		return true;
	}

private:
	void _Invalidate(BView* view)
	{
		for (; view != NULL; view = view->NextSibling()) {
			view->Invalidate();
			_Invalidate(view->ChildAt(0));
		}
	}

	BMessageRunner*	fRedraw;
};


class FrameWindow : public BWindow {
public:
	FrameWindow()
		:
		BWindow(BRect(520, 40, 800, 200), "No layout", B_TITLED_WINDOW, 0)
	{
		BView* background = new BView(Bounds(), "background", B_FOLLOW_ALL, 0);
		background->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		AddChild(background);
		background->AddChild(new BButton(BRect(10, 10, 130, 40), "button",
			"Button", NULL));
		background->AddChild(new BCheckBox(BRect(10, 50, 200, 70), "check",
			"Check box", NULL));
		background->AddChild(new BSlider(BRect(10, 80, 270, 130), "slider",
			"Slider", NULL, 0, 100));
	}
};


static status_t
call_look(void*)
{
	while (atomic_get(&sQuitting) == 0) {
		BControlLook* look = be_control_look;
		look->DefaultItemSpacing();
		look->DefaultLabelSpacing();
		look->ComposeIconSize(16);
		look->GetScrollBarWidth(B_VERTICAL);
		snooze(1);
	}
	return B_OK;
}


class Watcher : public BApplication {
public:
	Watcher(bool stress)
		:
		BApplication(kWatcherSignature),
		fStress(stress)
	{
	}

	virtual void ReadyToRun()
	{
		printf("start look=%s\n", typeid(*be_control_look).name());
		(new LayoutWindow(fStress))->Show();
		(new FrameWindow())->Show();

		if (!fStress)
			return;

		for (int32 i = 0; i < kStressThreads; i++) {
			fThreads[i] = spawn_thread(call_look, "call look",
				B_NORMAL_PRIORITY, NULL);
			resume_thread(fThreads[i]);
		}
	}

	virtual bool QuitRequested()
	{
		if (fStress) {
			atomic_set(&sQuitting, 1);
			for (int32 i = 0; i < kStressThreads; i++) {
				status_t result;
				wait_for_thread(fThreads[i], &result);
			}
		}
		printf("quit look=%s\n", typeid(*be_control_look).name());
		return BApplication::QuitRequested();
	}

private:
	bool		fStress;
	thread_id	fThreads[kStressThreads];
};


static int
usage()
{
	fprintf(stderr, "usage: ControlLookSwitchTest watch [stress] | quit | "
		"get | set PATH | sweep ROUNDS MS PATH...\n");
	return 1;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (argc < 2)
		return usage();

	if (strcmp(argv[1], "watch") == 0) {
		Watcher watcher(argc > 2 && strcmp(argv[2], "stress") == 0);
		watcher.Run();
		return 0;
	}

	if (strcmp(argv[1], "quit") == 0)
		return BMessenger(kWatcherSignature).SendMessage(B_QUIT_REQUESTED);

	BApplication driver(kDriverSignature);

	if (strcmp(argv[1], "get") == 0) {
		BString path;
		BPrivate::get_control_look(path);
		printf("system=\"%s\" team=%s\n", path.String(),
			typeid(*be_control_look).name());
		return 0;
	}

	if (strcmp(argv[1], "set") == 0 && argc == 3) {
		status_t status = BPrivate::set_control_look(argv[2]);
		if (status != B_OK)
			fprintf(stderr, "set: %s\n", strerror(status));
		return status == B_OK ? 0 : 1;
	}

	if (strcmp(argv[1], "sweep") == 0 && argc > 4) {
		int32 rounds = atoi(argv[2]);
		bigtime_t delay = atoi(argv[3]) * 1000LL;
		for (int32 round = 0; round < rounds; round++) {
			for (int32 i = 4; i < argc; i++) {
				status_t status = BPrivate::set_control_look(argv[i]);
				if (status != B_OK) {
					fprintf(stderr, "set: %s\n", strerror(status));
					return 1;
				}
				snooze(delay);
			}
		}
		return 0;
	}

	return usage();
}
