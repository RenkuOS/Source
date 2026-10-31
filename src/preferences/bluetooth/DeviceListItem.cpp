/*
 * Copyright 2009, Oliver Ruiz Dorantes, <oliver.ruiz.dorantes_at_gmail.com>
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <Bitmap.h>
#include <Catalog.h>
#include <ControlLook.h>
#include <View.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/BluetoothDevice.h>

#include "DeviceIcon.h"
#include "DeviceListItem.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Device list"

namespace Bluetooth {


DeviceListItem::DeviceListItem(RemoteDevice* bDevice)
	:
	BListItem(),
	fDevice(bDevice),
	fConnectionState(RemoteDevice::DISCONNECTED),
	fShowAddress(false)
{
	fAddress = bDevice->GetBluetoothAddress();
	fClass = bDevice->GetDeviceClass();
	// we always use the cached name here as we dont want to fire a query in the middle of an
	// INQUIRY
	fName = bDevice->GetCachedFriendlyName();
}


void
DeviceListItem::SetDevice(RemoteDevice* bDevice)
{
	fAddress = bDevice->GetBluetoothAddress();
	fClass = bDevice->GetDeviceClass();
	fName = bDevice->GetFriendlyName();
	// without a name the kit gives the address, which is shown anyway
	if (fName == bdaddrUtils::ToString(fAddress))
		fName = "";
	// AKAIR rssi we can just have it @ inquiry time...
}


DeviceListItem::~DeviceListItem()
{

}


void
DeviceListItem::DrawItem(BView* owner, BRect itemRect, bool complete)
{
	rgb_color background = ui_color(IsSelected()
		? B_LIST_SELECTED_BACKGROUND_COLOR : B_LIST_BACKGROUND_COLOR);
	rgb_color text = ui_color(IsSelected()
		? B_LIST_SELECTED_ITEM_TEXT_COLOR : B_LIST_ITEM_TEXT_COLOR);
	rgb_color dimmed = tint_color(text, IsSelected() ? B_DARKEN_1_TINT
		: B_LIGHTEN_1_TINT);

	owner->SetLowColor(background);
	if (IsSelected() || complete) {
		owner->SetHighColor(background);
		owner->FillRect(itemRect);
	}

	float inset = be_control_look->DefaultLabelSpacing();
	float iconSize = IconSize();
	BRect iconFrame(itemRect.left + inset, 0, itemRect.left + inset + iconSize
		- 1, iconSize - 1);
	iconFrame.OffsetTo(iconFrame.left,
		itemRect.top + (itemRect.Height() - iconSize) / 2);
	DrawDeviceIcon(owner, iconFrame,
		DeviceKind(fClass.MajorDeviceClass(), fClass.MinorDeviceClass()));

	float textLeft = iconFrame.right + inset * 1.5;

	// the name
	BFont font(be_bold_font);
	font_height boldHeight;
	font.GetHeight(&boldHeight);
	owner->SetFont(&font);
	owner->SetHighColor(text);

	BFont plain(be_plain_font);
	font_height plainHeight;
	plain.GetHeight(&plainHeight);

	float lines = boldHeight.ascent + boldHeight.descent + inset / 2
		+ plainHeight.ascent + plainHeight.descent;
	float top = itemRect.top + (itemRect.Height() - lines) / 2;

	// a device that did not tell its name yet goes by its kind
	BString name(fName);
	if (name.IsEmpty()) {
		name = DeviceKindName(DeviceKind(fClass.MajorDeviceClass(),
			fClass.MinorDeviceClass()));
	}
	font.TruncateString(&name, B_TRUNCATE_END, itemRect.right - textLeft - inset);
	owner->DrawString(name.String(), BPoint(textLeft, top + boldHeight.ascent));

	// the state, or the address while looking for devices
	owner->SetFont(&plain);
	float baseline = top + boldHeight.ascent + boldHeight.descent + inset / 2
		+ plainHeight.ascent;
	float x = textLeft;
	BString line;

	if (fShowAddress) {
		line << DeviceKindName(DeviceKind(fClass.MajorDeviceClass(),
			fClass.MinorDeviceClass()))
			<< " · " << bdaddrUtils::ToString(fAddress);
	} else {
		rgb_color dot;
		const char* state;
		switch (fConnectionState) {
			case RemoteDevice::CONNECTED:
				dot = (rgb_color){ 52, 168, 83, 255 };
				state = B_TRANSLATE("Connected");
				break;
			case RemoteDevice::CONNECTING:
				dot = (rgb_color){ 240, 160, 30, 255 };
				state = B_TRANSLATE("Connecting" B_UTF8_ELLIPSIS);
				break;
			default:
				dot = dimmed;
				state = B_TRANSLATE("Not connected");
				break;
		}

		float radius = ceilf(plainHeight.ascent / 3);
		BPoint center(x + radius, baseline - plainHeight.ascent / 2 + 1);
		owner->SetHighColor(dot);
		if (fConnectionState == RemoteDevice::DISCONNECTED)
			owner->StrokeEllipse(center, radius, radius);
		else
			owner->FillEllipse(center, radius, radius);
		x += 2 * radius + inset;

		line << state << " · " << DeviceKindName(DeviceKind(
			fClass.MajorDeviceClass(), fClass.MinorDeviceClass()));
	}

	owner->SetHighColor(dimmed);
	plain.TruncateString(&line, B_TRUNCATE_END, itemRect.right - x - inset);
	owner->DrawString(line.String(), BPoint(x, baseline));

	owner->SetHighColor(text);
}


float
DeviceListItem::IconSize()
{
	return be_control_look->ComposeIconSize(32).Width() + 1;
}


void
DeviceListItem::Update(BView* owner, const BFont* font)
{
	BListItem::Update(owner, font);

	font_height bold;
	be_bold_font->GetHeight(&bold);
	font_height plain;
	be_plain_font->GetHeight(&plain);

	float inset = be_control_look->DefaultLabelSpacing();
	float text = bold.ascent + bold.descent + inset / 2 + plain.ascent
		+ plain.descent;
	SetHeight(ceilf(MAX(text, IconSize()) + 2 * inset));
}


int
DeviceListItem::Compare(const void	*firstArg, const void	*secondArg)
{
	const DeviceListItem* item1 = *static_cast<const DeviceListItem* const *>
		(firstArg);
	const DeviceListItem* item2 = *static_cast<const DeviceListItem* const *>
		(secondArg);

	return (int)bdaddrUtils::Compare(item1->fAddress, item2->fAddress);
}


RemoteDevice*
DeviceListItem::Device() const
{
	return fDevice;
}


void
DeviceListItem::SetConnectionState(RemoteDevice::ConnectionState connectionState)
{
	fConnectionState = connectionState;
}


RemoteDevice::ConnectionState
DeviceListItem::GetConnectionState()
{
	return fConnectionState;
}


}
