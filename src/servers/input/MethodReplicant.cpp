/*
 * Copyright 2004-2009, Haiku. All rights reserved.
 * Distributed under the terms of the MIT license.
 *
 * Authors:
 *		Jérôme Duval
 */


#include "MethodReplicant.h"

#include <new>
#include <string.h>

#include <Alert.h>
#include <AppDefs.h>
#include <Debug.h>
#include <Dragger.h>
#include <Bitmap.h>
#include <IconUtils.h>
#include <MenuItem.h>
#include <Message.h>
#include <Messenger.h>
#include <PopUpMenu.h>

#include "remote_icon.h"

#include "InputServerTypes.h"

#ifdef DEBUG
#	define CALLED() PRINT(("CALLED %s \n", __PRETTY_FUNCTION__));
#else
#	define CALLED()
#endif


/*!	BMenu::SetTargetForItems() only reaches the items of the menu itself, so
	the items of a method's submenus would be left without a target and their
	messages would never reach the method.
*/
static void
set_targets_recursively(BMenu* menu, const BMessenger& messenger)
{
	for (int32 i = 0; i < menu->CountItems(); i++) {
		BMenuItem* item = menu->ItemAt(i);
		if (item->Submenu() != NULL)
			set_targets_recursively(item->Submenu(), messenger);
		else
			item->SetTarget(messenger);
	}
}


//!	The vector icon a method may have besides its 16x16 one
static const uint8*
find_vector_icon(BMessage* message, size_t& size)
{
	const void* data;
	ssize_t numBytes;
	if (message->FindData("vector icon", B_VECTOR_ICON_TYPE, &data, &numBytes)
			!= B_OK || numBytes <= 0) {
		size = 0;
		return NULL;
	}

	size = numBytes;
	return (const uint8*)data;
}


MethodReplicant::MethodReplicant(const char* signature, float size)
	:
	BView(BRect(0, 0, size - 1, size - 1), REPLICANT_CTL_NAME, B_FOLLOW_ALL,
		B_WILL_DRAW),
	fMenu("", false, false)
{
	// Background Bitmap, as large as the view
	fSegments = new BBitmap(Bounds(), B_RGBA32);
	BIconUtils::ConvertFromCMAP8(kRemoteBits, kRemoteWidth, kRemoteHeight,
		kRemoteWidth, fSegments);

	ASSERT(signature != NULL);
	fSignature = strdup(signature);

	fMenu.SetFont(be_plain_font);
	fMenu.SetRadioMode(true);
}


MethodReplicant::MethodReplicant(BMessage* message)
	:
	BView(message),
	fMenu("", false, false)
{
	// Background Bitmap, as large as the view
	fSegments = new BBitmap(Bounds(), B_RGBA32);
	BIconUtils::ConvertFromCMAP8(kRemoteBits, kRemoteWidth, kRemoteHeight,
		kRemoteWidth, fSegments);

	const char* signature = NULL;
	message->FindString("add_on", &signature);
	ASSERT(signature != NULL);
	fSignature = strdup(signature);

	fMenu.SetFont(be_plain_font);
	fMenu.SetRadioMode(true);
}


MethodReplicant::~MethodReplicant()
{
	delete fSegments;
	free(fSignature);
}


// archiving overrides
MethodReplicant*
MethodReplicant::Instantiate(BMessage* data)
{
	CALLED();
	if (!validate_instantiation(data, REPLICANT_CTL_NAME))
		return NULL;
	return new(std::nothrow) MethodReplicant(data);
}


status_t
MethodReplicant::Archive(BMessage* data, bool deep) const
{
	BView::Archive(data, deep);

	data->AddString("add_on", fSignature);
	return B_NO_ERROR;
}


void
MethodReplicant::AttachedToWindow()
{
	CALLED();

	SetViewColor(Parent()->ViewColor());

	BMessenger messenger(this);
	BMessage msg(IS_METHOD_REGISTER);
	msg.AddMessenger("address", messenger);

	BMessenger inputMessenger(fSignature);
	if (inputMessenger.SendMessage(&msg) != B_OK)
		printf("error when contacting input_server\n");
}


void
MethodReplicant::MessageReceived(BMessage* message)
{
	PRINT(("%s what:%c%c%c%c\n", __PRETTY_FUNCTION__, (char)(message->what >> 24),
		(char)(message->what >> 16), (char)(message->what >> 8), (char)message->what));
	PRINT_OBJECT(*message);

	switch (message->what) {
		case B_ABOUT_REQUESTED:
		{
			BAlert* alert = new BAlert("About Method Replicant",
				"Method Replicant (Replicant)\n"
				"  Brought to you by Jérôme DUVAL.\n\n"
				"Haiku, 2004-2009", "OK");
			alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
			alert->Go();
			break;
		}
		case IS_UPDATE_NAME:
			UpdateMethodName(message);
			break;
		case IS_UPDATE_ICON:
			UpdateMethodIcon(message);
			break;
		case IS_UPDATE_MENU:
			UpdateMethodMenu(message);
			break;
		case IS_UPDATE_METHOD:
			UpdateMethod(message);
			break;
		case IS_ADD_METHOD:
			AddMethod(message);
			break;
		case IS_REMOVE_METHOD:
			RemoveMethod(message);
			break;

		default:
			BView::MessageReceived(message);
			break;
	}
}


void
MethodReplicant::Draw(BRect rect)
{
	BView::Draw(rect);

	SetDrawingMode(B_OP_ALPHA);
	SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	DrawBitmap(fSegments);
}


void
MethodReplicant::MouseDown(BPoint point)
{
	CALLED();
	uint32 mouseButtons;
	BPoint where;
	GetMouse(&where, &mouseButtons, true);

	where = ConvertToScreen(point);

	fMenu.SetTargetForItems(this);
	BMenuItem* item = fMenu.Go(where, true, true,
		BRect(where - BPoint(4, 4), where + BPoint(4, 4)));

	if (dynamic_cast<MethodMenuItem*>(item) != NULL) {
		BMessage msg(IS_SET_METHOD);
		msg.AddInt32("cookie", ((MethodMenuItem*)item)->Cookie());
		BMessenger messenger(fSignature);
		messenger.SendMessage(&msg);
	}
}


void
MethodReplicant::MouseUp(BPoint point)
{
	/* don't Quit() ! thanks for FFM users */
}


void
MethodReplicant::UpdateMethod(BMessage* message)
{
	CALLED();
	int32 cookie;
	if (message->FindInt32("cookie", &cookie) != B_OK) {
		fprintf(stderr, "can't find cookie in message\n");
		return;
	}

	MethodMenuItem* item = FindItemByCookie(cookie);
	if (item == NULL) {
		fprintf(stderr, "can't find item with cookie %" B_PRIx32 "\n", cookie);
		return;
	}
	item->SetMarked(true);

	item->GetIcon(fSegments);
	Invalidate();
}


void
MethodReplicant::UpdateMethodIcon(BMessage* message)
{
	CALLED();
	int32 cookie;
	if (message->FindInt32("cookie", &cookie) != B_OK) {
		fprintf(stderr, "can't find cookie in message\n");
		return;
	}

	const uchar* data;
	ssize_t numBytes;
	if (message->FindData("icon", B_ANY_TYPE, (const void**)&data, &numBytes)
			!= B_OK) {
		fprintf(stderr, "can't find icon in message\n");
		return;
	}

	MethodMenuItem* item = FindItemByCookie(cookie);
	if (item == NULL) {
		fprintf(stderr, "can't find item with cookie %" B_PRIx32 "\n", cookie);
		return;
	}

	size_t vectorIconSize;
	const uint8* vectorIcon = find_vector_icon(message, vectorIconSize);
	item->SetIcon(data, vectorIcon, vectorIconSize);

	// The replicant shows the icon of the active method, so a method that
	// changes its icon (to show its input mode, for example) must be redrawn.
	if (item->IsMarked()) {
		item->GetIcon(fSegments);
		Invalidate();
	}
}


void
MethodReplicant::UpdateMethodMenu(BMessage* message)
{
	CALLED();
	int32 cookie;
	if (message->FindInt32("cookie", &cookie) != B_OK) {
		fprintf(stderr, "can't find cookie in message\n");
		return;
	}

	BMessage msg;
	if (message->FindMessage("menu", &msg) != B_OK) {
		fprintf(stderr, "can't find menu in message\n");
		return;
	}
	PRINT_OBJECT(msg);

	BMessenger messenger;
	if (message->FindMessenger("target", &messenger) != B_OK) {
		fprintf(stderr, "can't find target in message\n");
		return;
	}

	BMenu* menu = (BMenu*)BMenu::Instantiate(&msg);
	if (menu == NULL) {
		PRINT(("can't instantiate menu\n"));
	} else
		set_targets_recursively(menu, messenger);

	MethodMenuItem* item = FindItemByCookie(cookie);
	if (item == NULL) {
		fprintf(stderr, "can't find item with cookie %" B_PRIx32 "\n", cookie);
		return;
	}
	int32 index = fMenu.IndexOf(item);

	MethodMenuItem* item2 = NULL;
	if (menu) {
		item2 = new MethodMenuItem(cookie, item->Label(), item->Icon(),
			item->VectorIcon(), item->VectorIconSize(), menu, messenger);
	} else {
		item2 = new MethodMenuItem(cookie, item->Label(), item->Icon(),
			item->VectorIcon(), item->VectorIconSize());
	}

	item = (MethodMenuItem*)fMenu.RemoveItem(index);
	fMenu.AddItem(item2, index);
	item2->SetMarked(item->IsMarked());
	delete item;
}


void
MethodReplicant::UpdateMethodName(BMessage* message)
{
	CALLED();
	int32 cookie;
	if (message->FindInt32("cookie", &cookie) != B_OK) {
		fprintf(stderr, "can't find cookie in message\n");
		return;
	}

	const char* name;
	if (message->FindString("name", &name) != B_OK) {
		fprintf(stderr, "can't find name in message\n");
		return;
	}

	MethodMenuItem* item = FindItemByCookie(cookie);
	if (item == NULL) {
		fprintf(stderr, "can't find item with cookie %" B_PRIx32 "\n", cookie);
		return;
	}

	item->SetName(name);
}


MethodMenuItem*
MethodReplicant::FindItemByCookie(int32 cookie)
{
	for (int32 i = 0; i < fMenu.CountItems(); i++) {
		MethodMenuItem* item = (MethodMenuItem*)fMenu.ItemAt(i);
		PRINT(("cookie : 0x%" B_PRIx32 "\n", item->Cookie()));
		if (item->Cookie() == cookie)
			return item;
	}

	return NULL;
}


void
MethodReplicant::AddMethod(BMessage* message)
{
	CALLED();
	int32 cookie;
	if (message->FindInt32("cookie", &cookie) != B_OK) {
		fprintf(stderr, "can't find cookie in message\n");
		return;
	}

	const char* name;
	if (message->FindString("name", &name) != B_OK) {
		fprintf(stderr, "can't find name in message\n");
		return;
	}

	const uchar* icon;
	ssize_t numBytes;
	if (message->FindData("icon", B_ANY_TYPE, (const void**)&icon, &numBytes)
			!= B_OK) {
		fprintf(stderr, "can't find icon in message\n");
		return;
	}

	BMessage menuMsg;
	if (message->FindMessage("menu", &menuMsg) != B_OK) {
		fprintf(stderr, "can't find menu in message\n");
		return;
	}
	PRINT_OBJECT(menuMsg);

	BMessenger messenger;
	if (message->FindMessenger("target", &messenger) != B_OK) {
		fprintf(stderr, "can't find target in message\n");
		return;
	}

	BMenu* menu = static_cast<BMenu*>(BMenu::Instantiate(&menuMsg));
	if (menu == NULL) {
		PRINT(("can't instantiate menu\n"));
	} else
		set_targets_recursively(menu, messenger);

	MethodMenuItem* item = FindItemByCookie(cookie);
	if (item != NULL) {
		fprintf(stderr, "item with cookie %" B_PRIx32 " already exists\n", cookie);
		return;
	}

	size_t vectorIconSize;
	const uint8* vectorIcon = find_vector_icon(message, vectorIconSize);
	if (menu != NULL) {
		item = new MethodMenuItem(cookie, name, icon, vectorIcon,
			vectorIconSize, menu, messenger);
	} else {
		item = new MethodMenuItem(cookie, name, icon, vectorIcon,
			vectorIconSize);
	}
	fMenu.AddItem(item);
	item->SetTarget(this);

	if (fMenu.CountItems() == 1) {
		item->SetMarked(true);
		item->GetIcon(fSegments);
		Invalidate();
	}
}


void
MethodReplicant::RemoveMethod(BMessage* message)
{
	CALLED();
	int32 cookie;
	if (message->FindInt32("cookie", &cookie) != B_OK) {
		fprintf(stderr, "can't find cookie in message\n");
		return;
	}

	MethodMenuItem* item = FindItemByCookie(cookie);
	if (item == NULL) {
		fprintf(stderr, "can't find item with cookie %" B_PRIx32 "\n", cookie);
		return;
	}
	fMenu.RemoveItem(item);
	delete item;
}
