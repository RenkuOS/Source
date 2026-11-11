/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 *
 * Checks that running applications follow a change of the system fonts, and
 * that views keep the fonts their application chose.
 *
 *   SystemFontSwitchTest watch       windows that log each view's font
 *   SystemFontSwitchTest quit        quits a running watcher
 *   SystemFontSwitchTest report      has a running watcher log again
 *   SystemFontSwitchTest set WHICH FAMILY STYLE SIZE
 *                                    WHICH is plain, bold, fixed or menu
 *   SystemFontSwitchTest get         prints the system fonts
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Messenger.h>
#include <OS.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <Slider.h>
#include <String.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>


extern void _set_system_font_(const char* which, font_family family,
	font_style style, float size);


static const char* kWatcherSignature
	= "application/x-vnd.Renku-SystemFontSwitchTest";
static const char* kDriverSignature
	= "application/x-vnd.Renku-SystemFontSwitchTest-driver";
static const uint32 kReport = 'rprt';


static BString
describe(const BFont& font)
{
	font_family family;
	font_style style;
	font.GetFamilyAndStyle(&family, &style);
	BString text;
	text.SetToFormat("%s/%s/%g", family, style, font.Size());
	return text;
}


class FontWindow : public BWindow {
public:
	FontWindow()
		:
		BWindow(BRect(40, 40, 520, 600), "Fonts", B_TITLED_WINDOW,
			B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS)
	{
		BMenuBar* menuBar = new BMenuBar("menubar");
		BMenu* menu = new BMenu("File");
		menu->AddItem(new BMenuItem("Open" B_UTF8_ELLIPSIS, NULL, 'O'));
		BMenu* subMenu = new BMenu("Submenu");
		subMenu->AddItem(new BMenuItem("Item", NULL));
		menu->AddItem(subMenu);
		menuBar->AddItem(menu);
		fMenu = menu;
		fSubMenu = subMenu;

		BStringView* plainView = new BStringView("default", "Never set a font");

		BStringView* boldView = new BStringView("bold", "be_bold_font");
		boldView->SetFont(be_bold_font);

		BStringView* fixedView = new BStringView("fixed", "be_fixed_font");
		fixedView->SetFont(be_fixed_font);

		// The application's own choices: these must stay as they are.
		BStringView* ownView = new BStringView("own", "A font of its own");
		BFont ownFont(be_fixed_font);
		ownFont.SetSize(15);
		ownView->SetFont(&ownFont);

		BStringView* copyView = new BStringView("copy", "A copy of be_plain_font");
		BFont copyFont(be_plain_font);
		copyView->SetFont(&copyFont);

		// Only the size is the application's.
		BStringView* sizeView = new BStringView("size", "Its own size");
		sizeView->SetFontSize(20);

		// Only the face is the application's.
		BStringView* faceView = new BStringView("face", "Its own face");
		BFont faceFont;
		faceFont.SetFace(B_ITALIC_FACE);
		faceView->SetFont(&faceFont, B_FONT_FACE);

		BPopUpMenu* popUp = new BPopUpMenu("One");
		popUp->AddItem(new BMenuItem("One", NULL));
		popUp->AddItem(new BMenuItem("Two", NULL));

		BListView* list = new BListView("list");
		for (int32 i = 0; i < 20; i++) {
			BString label;
			label.SetToFormat("Item %" B_PRId32, i);
			list->AddItem(new BStringItem(label));
		}
		BScrollView* listScroll = new BScrollView("Second", list, 0, false,
			true);

		BTextView* text = new BTextView("text");
		text->SetText("Text view");

		BSlider* slider = new BSlider("slider", "Slider", NULL, 0, 100,
			B_HORIZONTAL);
		slider->SetHashMarks(B_HASH_MARKS_BOTTOM);
		slider->SetHashMarkCount(5);
		slider->SetLimitLabels("Min", "Max");

		BTabView* tabs = new BTabView("tabs");
		BView* first = BLayoutBuilder::Group<>(B_VERTICAL)
			.Add(new BCheckBox("Check box"))
			.Add(new BTextControl("Text", "text", NULL))
			.Add(new BMenuField("Menu", popUp))
			.Add(slider)
			.Add(new BButton("Button", NULL))
			.View();
		first->SetName("First");
		tabs->AddTab(first);
		tabs->AddTab(listScroll);

		BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
			.Add(menuBar)
			.AddGroup(B_VERTICAL)
				.SetInsets(B_USE_WINDOW_SPACING)
				.Add(plainView)
				.Add(boldView)
				.Add(fixedView)
				.Add(ownView)
				.Add(copyView)
				.Add(sizeView)
				.Add(faceView)
				.Add(text)
				.Add(tabs)
				.End();
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kReport:
				_Report();
				break;
			case B_FONTS_UPDATED:
				printf("update\n");
				_Report();
				BWindow::MessageReceived(message);
				break;
			default:
				BWindow::MessageReceived(message);
				break;
		}
	}

	virtual bool QuitRequested()
	{
		be_app->PostMessage(B_QUIT_REQUESTED);
		return true;
	}

private:
	void _Report()
	{
		printf("system plain=%s bold=%s fixed=%s\n",
			describe(*be_plain_font).String(),
			describe(*be_bold_font).String(),
			describe(*be_fixed_font).String());
		_Report(ChildAt(0));

		BFont font;
		fMenu->GetFont(&font);
		printf("view menu %s\n", describe(font).String());
		fSubMenu->GetFont(&font);
		printf("view submenu %s\n", describe(font).String());
	}

	void _Report(BView* view)
	{
		for (; view != NULL; view = view->NextSibling()) {
			if (dynamic_cast<BStringView*>(view) != NULL) {
				BFont font;
				view->GetFont(&font);
				printf("view %s %s\n", view->Name(),
					describe(font).String());
			}
			_Report(view->ChildAt(0));
		}
	}

	BMenu*	fMenu;
	BMenu*	fSubMenu;
};


class Watcher : public BApplication {
public:
	Watcher()
		:
		BApplication(kWatcherSignature)
	{
	}

	virtual void ReadyToRun()
	{
		fWindow = new FontWindow();
		fWindow->Show();
		BMessenger(fWindow).SendMessage(kReport);
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == kReport)
			BMessenger(fWindow).SendMessage(kReport);
		else
			BApplication::MessageReceived(message);
	}

private:
	BWindow*	fWindow;
};


static int
usage()
{
	fprintf(stderr, "usage: SystemFontSwitchTest watch | quit | report | get | "
		"set WHICH FAMILY STYLE SIZE\n");
	return 1;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (argc < 2)
		return usage();

	if (strcmp(argv[1], "watch") == 0) {
		Watcher watcher;
		watcher.Run();
		return 0;
	}

	if (strcmp(argv[1], "quit") == 0)
		return BMessenger(kWatcherSignature).SendMessage(B_QUIT_REQUESTED);

	if (strcmp(argv[1], "report") == 0)
		return BMessenger(kWatcherSignature).SendMessage(kReport);

	BApplication driver(kDriverSignature);

	if (strcmp(argv[1], "get") == 0) {
		menu_info info;
		get_menu_info(&info);
		printf("plain=%s bold=%s fixed=%s menu=%s/%s/%g\n",
			describe(*be_plain_font).String(),
			describe(*be_bold_font).String(),
			describe(*be_fixed_font).String(),
			info.f_family, info.f_style, info.font_size);
		return 0;
	}

	if (strcmp(argv[1], "set") == 0 && argc == 6) {
		font_family family;
		font_style style;
		strlcpy(family, argv[3], sizeof(family));
		strlcpy(style, argv[4], sizeof(style));
		float size = atof(argv[5]);

		if (strcmp(argv[2], "menu") == 0) {
			menu_info info;
			get_menu_info(&info);
			strlcpy(info.f_family, family, sizeof(info.f_family));
			strlcpy(info.f_style, style, sizeof(info.f_style));
			info.font_size = size;
			return set_menu_info(&info) == B_OK ? 0 : 1;
		}
		_set_system_font_(argv[2], family, style, size);
		// There's no reply; make sure it's been handled before quitting.
		snooze(100000);
		return 0;
	}

	return usage();
}
