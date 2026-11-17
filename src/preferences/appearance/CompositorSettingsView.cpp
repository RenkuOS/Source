/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


#include "CompositorSettingsView.h"

#include <Alert.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <Locale.h>
#include <Window.h>

#include <AppServerLink.h>
#include <RosterPrivate.h>
#include <ServerProtocol.h>

#include "AppearanceWindow.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "CompositorSettingsView"


static const int32 kMsgComposited = 'cmps';

// What app_server uses until it is told otherwise
static const bool kDefaultComposited = false;


CompositorSettingsView::CompositorSettingsView(const char* name)
	:
	BView(name, 0)
{
	fSavedComposited = _Get(AS_GET_COMPOSITED_DESKTOP, kDefaultComposited);

	fCompositedCheckBox = new BCheckBox("composited",
		B_TRANSLATE("Compositor (after a restart)"),
		new BMessage(kMsgComposited));

	_SetComposited(fSavedComposited);

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.Add(fCompositedCheckBox)
		.AddGlue()
		.SetInsets(B_USE_WINDOW_SPACING);
}


CompositorSettingsView::~CompositorSettingsView()
{
}


void
CompositorSettingsView::AttachedToWindow()
{
	AdoptParentColors();

	fCompositedCheckBox->SetTarget(this);
}


void
CompositorSettingsView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgComposited:
			_Set(AS_SET_COMPOSITED_DESKTOP,
				fCompositedCheckBox->Value() == B_CONTROL_ON);
			_SetComposited(fCompositedCheckBox->Value() == B_CONTROL_ON);
			Window()->PostMessage(kMsgUpdate);
			_OfferRestart();
			break;

		default:
			BView::MessageReceived(message);
			break;
	}
}


bool
CompositorSettingsView::IsDefaultable()
{
	return (fCompositedCheckBox->Value() == B_CONTROL_ON)
		!= kDefaultComposited;
}


void
CompositorSettingsView::SetDefaults()
{
	_Set(AS_SET_COMPOSITED_DESKTOP, kDefaultComposited);
	_SetComposited(kDefaultComposited);
}


bool
CompositorSettingsView::IsRevertable()
{
	return (fCompositedCheckBox->Value() == B_CONTROL_ON) != fSavedComposited;
}


void
CompositorSettingsView::Revert()
{
	if (!IsRevertable())
		return;

	_Set(AS_SET_COMPOSITED_DESKTOP, fSavedComposited);
	_SetComposited(fSavedComposited);
}


void
CompositorSettingsView::_OfferRestart()
{
	BAlert* alert = new BAlert(B_TRANSLATE("Restart"),
		B_TRANSLATE("The compositor is switched on or off when the system "
			"starts. Do you want to restart now?"),
		B_TRANSLATE("I don't wanna"), B_TRANSLATE("Let's go!"));
	alert->SetShortcut(0, B_ESCAPE);

	if (alert->Go() == 1)
		BRoster::Private().ShutDown(true, false, false);
}


void
CompositorSettingsView::_SetComposited(bool composited)
{
	fCompositedCheckBox->SetValue(composited);
}


/*static*/ bool
CompositorSettingsView::_Get(int32 code, bool defaultValue)
{
	BPrivate::AppServerLink link;
	link.StartMessage(code);

	int32 status;
	bool value = defaultValue;
	if (link.FlushWithReply(status) == B_OK && status == B_OK)
		link.Read<bool>(&value);
	return value;
}


/*static*/ void
CompositorSettingsView::_Set(int32 code, bool value)
{
	BPrivate::AppServerLink link;
	link.StartMessage(code);
	link.Attach<bool>(value);
	link.Flush();
}
