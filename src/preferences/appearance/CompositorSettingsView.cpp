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
static const int32 kMsgShadows = 'cmpw';
static const int32 kMsgRoundedCorners = 'cmpr';
static const int32 kMsgDragTransparency = 'cmpt';

// What app_server uses until it is told otherwise
static const bool kDefaultComposited = false;
static const bool kDefaultShadows = true;
static const bool kDefaultRoundedCorners = true;
static const bool kDefaultDragTransparency = false;


CompositorSettingsView::CompositorSettingsView(const char* name)
	:
	BView(name, 0)
{
	fSavedComposited = _Get(AS_GET_COMPOSITED_DESKTOP, kDefaultComposited);
	fSavedShadows = _Get(AS_GET_COMPOSITED_SHADOWS, kDefaultShadows);
	fSavedRoundedCorners = _Get(AS_GET_COMPOSITED_ROUNDED_CORNERS,
		kDefaultRoundedCorners);
	fSavedDragTransparency = _Get(AS_GET_COMPOSITED_DRAG_TRANSPARENCY,
		kDefaultDragTransparency);

	fCompositedCheckBox = new BCheckBox("composited",
		B_TRANSLATE("Compositor (after a restart)"),
		new BMessage(kMsgComposited));
	fShadowsCheckBox = new BCheckBox("shadows",
		B_TRANSLATE("Window shadows"), new BMessage(kMsgShadows));
	fRoundedCornersCheckBox = new BCheckBox("rounded corners",
		B_TRANSLATE("Rounded window corners"),
		new BMessage(kMsgRoundedCorners));
	fDragTransparencyCheckBox = new BCheckBox("drag transparency",
		B_TRANSLATE("See-through windows while dragging"),
		new BMessage(kMsgDragTransparency));

	fShadowsCheckBox->SetValue(fSavedShadows);
	fRoundedCornersCheckBox->SetValue(fSavedRoundedCorners);
	fDragTransparencyCheckBox->SetValue(fSavedDragTransparency);
	_SetComposited(fSavedComposited);

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.Add(fCompositedCheckBox)
		.AddGroup(B_VERTICAL, B_USE_SMALL_SPACING)
			.Add(fShadowsCheckBox)
			.Add(fRoundedCornersCheckBox)
			.Add(fDragTransparencyCheckBox)
			.SetInsets(B_USE_WINDOW_SPACING, 0, 0, 0)
			.End()
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
	fShadowsCheckBox->SetTarget(this);
	fRoundedCornersCheckBox->SetTarget(this);
	fDragTransparencyCheckBox->SetTarget(this);
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

		case kMsgShadows:
			_Set(AS_SET_COMPOSITED_SHADOWS,
				fShadowsCheckBox->Value() == B_CONTROL_ON);
			Window()->PostMessage(kMsgUpdate);
			break;

		case kMsgRoundedCorners:
			_Set(AS_SET_COMPOSITED_ROUNDED_CORNERS,
				fRoundedCornersCheckBox->Value() == B_CONTROL_ON);
			Window()->PostMessage(kMsgUpdate);
			break;

		case kMsgDragTransparency:
			_Set(AS_SET_COMPOSITED_DRAG_TRANSPARENCY,
				fDragTransparencyCheckBox->Value() == B_CONTROL_ON);
			Window()->PostMessage(kMsgUpdate);
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
			!= kDefaultComposited
		|| (fShadowsCheckBox->Value() == B_CONTROL_ON) != kDefaultShadows
		|| (fRoundedCornersCheckBox->Value() == B_CONTROL_ON)
			!= kDefaultRoundedCorners
		|| (fDragTransparencyCheckBox->Value() == B_CONTROL_ON)
			!= kDefaultDragTransparency;
}


void
CompositorSettingsView::SetDefaults()
{
	_Set(AS_SET_COMPOSITED_DESKTOP, kDefaultComposited);
	_SetComposited(kDefaultComposited);
	_SetOption(fShadowsCheckBox, AS_SET_COMPOSITED_SHADOWS, kDefaultShadows);
	_SetOption(fRoundedCornersCheckBox, AS_SET_COMPOSITED_ROUNDED_CORNERS,
		kDefaultRoundedCorners);
	_SetOption(fDragTransparencyCheckBox,
		AS_SET_COMPOSITED_DRAG_TRANSPARENCY, kDefaultDragTransparency);
}


bool
CompositorSettingsView::IsRevertable()
{
	return (fCompositedCheckBox->Value() == B_CONTROL_ON) != fSavedComposited
		|| (fShadowsCheckBox->Value() == B_CONTROL_ON) != fSavedShadows
		|| (fRoundedCornersCheckBox->Value() == B_CONTROL_ON)
			!= fSavedRoundedCorners
		|| (fDragTransparencyCheckBox->Value() == B_CONTROL_ON)
			!= fSavedDragTransparency;
}


void
CompositorSettingsView::Revert()
{
	if (!IsRevertable())
		return;

	_Set(AS_SET_COMPOSITED_DESKTOP, fSavedComposited);
	_SetComposited(fSavedComposited);
	_SetOption(fShadowsCheckBox, AS_SET_COMPOSITED_SHADOWS, fSavedShadows);
	_SetOption(fRoundedCornersCheckBox, AS_SET_COMPOSITED_ROUNDED_CORNERS,
		fSavedRoundedCorners);
	_SetOption(fDragTransparencyCheckBox,
		AS_SET_COMPOSITED_DRAG_TRANSPARENCY, fSavedDragTransparency);
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


/*!	The options only apply to a composited desktop, so they are greyed out
	without it, and keep their settings.
*/
void
CompositorSettingsView::_SetComposited(bool composited)
{
	fCompositedCheckBox->SetValue(composited);
	fShadowsCheckBox->SetEnabled(composited);
	fRoundedCornersCheckBox->SetEnabled(composited);
	fDragTransparencyCheckBox->SetEnabled(composited);
}


void
CompositorSettingsView::_SetOption(BCheckBox* checkBox, int32 code,
	bool value)
{
	_Set(code, value);
	checkBox->SetValue(value);
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
