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
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <Slider.h>
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
static const int32 kMsgWobbly = 'cmpo';
static const int32 kMsgWobbliness = 'cmpl';
static const int32 kMsgGlass = 'cmpg';
static const int32 kMsgGlassOpacity = 'cmpa';

// What app_server uses until it is told otherwise
static const bool kDefaultComposited = false;
static const bool kDefaultShadows = true;
static const bool kDefaultRoundedCorners = true;
static const bool kDefaultDragTransparency = false;
static const bool kDefaultWobbly = false;
static const int32 kDefaultWobbliness = 50;
static const int32 kDefaultGlass = 0;
static const int32 kDefaultGlassOpacity = 70;


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
	fSavedWobbly = _Get(AS_GET_COMPOSITED_WOBBLY_WINDOWS, kDefaultWobbly);
	fSavedWobbliness = _GetWobbliness();
	fSavedGlass = _GetValue(AS_GET_COMPOSITED_GLASS, kDefaultGlass);
	fSavedGlassOpacity = _GetValue(AS_GET_COMPOSITED_GLASS_OPACITY,
		kDefaultGlassOpacity);

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
	fWobblyCheckBox = new BCheckBox("wobbly",
		B_TRANSLATE("Wobbly windows"),
		new BMessage(kMsgWobbly));
	fWobblinessSlider = new BSlider("wobbliness", B_TRANSLATE("Wobbliness"),
		new BMessage(kMsgWobbliness), 0, 100, B_HORIZONTAL);
	fWobblinessSlider->SetLimitLabels(B_TRANSLATE("Less"),
		B_TRANSLATE("More"));
	fWobblinessSlider->SetHashMarks(B_HASH_MARKS_BOTTOM);
	fWobblinessSlider->SetHashMarkCount(5);

	// In the order of the values app_server takes
	BPopUpMenu* glassMenu = new BPopUpMenu("glass");
	const char* glassLabels[] = {
		B_TRANSLATE("Off"),
		B_TRANSLATE("Window borders"),
		B_TRANSLATE("Whole windows")
	};
	for (int32 i = 0; i < 3; i++) {
		BMessage* message = new BMessage(kMsgGlass);
		message->AddInt32("glass", i);
		glassMenu->AddItem(new BMenuItem(glassLabels[i], message));
	}
	fGlassMenuField = new BMenuField("glass", B_TRANSLATE("Glass:"),
		glassMenu);
	fGlassOpacitySlider = new BSlider("glass opacity",
		B_TRANSLATE("Glass opacity"), new BMessage(kMsgGlassOpacity), 10, 100,
		B_HORIZONTAL);
	fGlassOpacitySlider->SetLimitLabels(B_TRANSLATE("Clearer"),
		B_TRANSLATE("More solid"));
	fGlassOpacitySlider->SetHashMarks(B_HASH_MARKS_BOTTOM);
	fGlassOpacitySlider->SetHashMarkCount(10);

	fShadowsCheckBox->SetValue(fSavedShadows);
	fRoundedCornersCheckBox->SetValue(fSavedRoundedCorners);
	fDragTransparencyCheckBox->SetValue(fSavedDragTransparency);
	fWobblyCheckBox->SetValue(fSavedWobbly);
	fWobblinessSlider->SetValue(fSavedWobbliness);
	if (BMenuItem* item = glassMenu->ItemAt(fSavedGlass))
		item->SetMarked(true);
	fGlassOpacitySlider->SetValue(fSavedGlassOpacity);
	_SetComposited(fSavedComposited);

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.Add(fCompositedCheckBox)
		.AddGroup(B_VERTICAL, B_USE_SMALL_SPACING)
			.Add(fShadowsCheckBox)
			.Add(fRoundedCornersCheckBox)
			.Add(fDragTransparencyCheckBox)
			.Add(fWobblyCheckBox)
			.AddGroup(B_VERTICAL)
				.Add(fWobblinessSlider)
				.SetInsets(B_USE_WINDOW_SPACING, 0, 0, 0)
				.End()
			.AddGroup(B_HORIZONTAL)
				.Add(fGlassMenuField)
				.AddGlue()
				.End()
			.AddGroup(B_VERTICAL)
				.Add(fGlassOpacitySlider)
				.SetInsets(B_USE_WINDOW_SPACING, 0, 0, 0)
				.End()
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
	fWobblyCheckBox->SetTarget(this);
	fWobblinessSlider->SetTarget(this);
	fGlassMenuField->Menu()->SetTargetForItems(this);
	fGlassOpacitySlider->SetTarget(this);
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

		case kMsgWobbly:
			_Set(AS_SET_COMPOSITED_WOBBLY_WINDOWS,
				fWobblyCheckBox->Value() == B_CONTROL_ON);
			_UpdateWobbliness();
			Window()->PostMessage(kMsgUpdate);
			break;

		case kMsgWobbliness:
			_SetWobbliness(fWobblinessSlider->Value());
			Window()->PostMessage(kMsgUpdate);
			break;

		case kMsgGlass:
		{
			int32 glass;
			if (message->FindInt32("glass", &glass) == B_OK) {
				_SetGlass(glass);
				Window()->PostMessage(kMsgUpdate);
			}
			break;
		}

		case kMsgGlassOpacity:
			_SetGlassOpacity(fGlassOpacitySlider->Value());
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
			!= kDefaultDragTransparency
		|| (fWobblyCheckBox->Value() == B_CONTROL_ON) != kDefaultWobbly
		|| fWobblinessSlider->Value() != kDefaultWobbliness
		|| fGlassMenuField->Menu()->FindMarkedIndex() != kDefaultGlass
		|| fGlassOpacitySlider->Value() != kDefaultGlassOpacity;
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
	_SetOption(fWobblyCheckBox, AS_SET_COMPOSITED_WOBBLY_WINDOWS,
		kDefaultWobbly);
	_SetWobbliness(kDefaultWobbliness);
	_UpdateWobbliness();
	_SetGlass(kDefaultGlass);
	_SetGlassOpacity(kDefaultGlassOpacity);
}


bool
CompositorSettingsView::IsRevertable()
{
	return (fCompositedCheckBox->Value() == B_CONTROL_ON) != fSavedComposited
		|| (fShadowsCheckBox->Value() == B_CONTROL_ON) != fSavedShadows
		|| (fRoundedCornersCheckBox->Value() == B_CONTROL_ON)
			!= fSavedRoundedCorners
		|| (fDragTransparencyCheckBox->Value() == B_CONTROL_ON)
			!= fSavedDragTransparency
		|| (fWobblyCheckBox->Value() == B_CONTROL_ON) != fSavedWobbly
		|| fWobblinessSlider->Value() != fSavedWobbliness
		|| fGlassMenuField->Menu()->FindMarkedIndex() != fSavedGlass
		|| fGlassOpacitySlider->Value() != fSavedGlassOpacity;
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
	_SetOption(fWobblyCheckBox, AS_SET_COMPOSITED_WOBBLY_WINDOWS,
		fSavedWobbly);
	_SetWobbliness(fSavedWobbliness);
	_UpdateWobbliness();
	_SetGlass(fSavedGlass);
	_SetGlassOpacity(fSavedGlassOpacity);
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
	fWobblyCheckBox->SetEnabled(composited);
	fGlassMenuField->SetEnabled(composited);
	_UpdateWobbliness();
	_UpdateGlass();
}


void
CompositorSettingsView::_SetOption(BCheckBox* checkBox, int32 code,
	bool value)
{
	_Set(code, value);
	checkBox->SetValue(value);
}


void
CompositorSettingsView::_SetWobbliness(int32 wobbliness)
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_SET_COMPOSITED_WOBBLINESS);
	link.Attach<uint8>((uint8)wobbliness);
	link.Flush();

	fWobblinessSlider->SetValue(wobbliness);
}


//!	The slider only matters while windows wobble.
void
CompositorSettingsView::_UpdateWobbliness()
{
	fWobblinessSlider->SetEnabled(fCompositedCheckBox->Value() == B_CONTROL_ON
		&& fWobblyCheckBox->Value() == B_CONTROL_ON);
}


void
CompositorSettingsView::_SetGlass(int32 glass)
{
	_SetValue(AS_SET_COMPOSITED_GLASS, (uint8)glass);
	if (BMenuItem* item = fGlassMenuField->Menu()->ItemAt(glass))
		item->SetMarked(true);
	_UpdateGlass();
}


void
CompositorSettingsView::_SetGlassOpacity(int32 opacity)
{
	_SetValue(AS_SET_COMPOSITED_GLASS_OPACITY, (uint8)opacity);
	fGlassOpacitySlider->SetValue(opacity);
}


//!	The slider only matters while there is glass.
void
CompositorSettingsView::_UpdateGlass()
{
	fGlassOpacitySlider->SetEnabled(
		fCompositedCheckBox->Value() == B_CONTROL_ON
		&& fGlassMenuField->Menu()->FindMarkedIndex() > 0);
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


/*static*/ uint8
CompositorSettingsView::_GetWobbliness()
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_COMPOSITED_WOBBLINESS);

	int32 status;
	uint8 value = kDefaultWobbliness;
	if (link.FlushWithReply(status) == B_OK && status == B_OK)
		link.Read<uint8>(&value);
	return value;
}


/*static*/ uint8
CompositorSettingsView::_GetValue(int32 code, uint8 defaultValue)
{
	BPrivate::AppServerLink link;
	link.StartMessage(code);

	int32 status;
	uint8 value = defaultValue;
	if (link.FlushWithReply(status) == B_OK && status == B_OK)
		link.Read<uint8>(&value);
	return value;
}


/*static*/ void
CompositorSettingsView::_SetValue(int32 code, uint8 value)
{
	BPrivate::AppServerLink link;
	link.StartMessage(code);
	link.Attach<uint8>(value);
	link.Flush();
}


/*static*/ void
CompositorSettingsView::_Set(int32 code, bool value)
{
	BPrivate::AppServerLink link;
	link.StartMessage(code);
	link.Attach<bool>(value);
	link.Flush();
}
