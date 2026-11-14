/*
 * Copyright (c) 2002, Jerome Duval (jerome.duval@free.fr)
 * Distributed under the terms of the MIT License.
 */


#include "MultiAudioAddOn.h"

#include <limits.h>
#include <new>
#include <stdio.h>
#include <string.h>

#include <Autolock.h>
#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <File.h>
#include <NodeMonitor.h>
#include <Path.h>

#include "debug.h"
#include "MultiAudioNode.h"
#include "MultiAudioDevice.h"


#define MULTI_SAVE

const char* kSettingsName = "Media/multi_audio_settings";

static const char* const kDevicesRoot = "/dev/audio/hmulti/";

//! Asks the watcher thread to reconcile; see MultiAudioDeviceWatcher::Rescan().
static const uint32 kMsgRescan = 'maRS';


//! instantiation function
extern "C" BMediaAddOn*
make_media_addon(image_id image)
{
	CALLED();
	return new MultiAudioAddOn(image);
}


//	#pragma mark - MultiAudioDeviceWatcher


MultiAudioDeviceWatcher::MultiAudioDeviceWatcher(MultiAudioAddOn* addOn,
	const char* rootPath)
	:
	BLooper("multi_audio device watcher"),
	fAddOn(addOn),
	fRootPath(rootPath),
	fWatched(),
	fQuitting(0)
{
}


MultiAudioDeviceWatcher::~MultiAudioDeviceWatcher()
{
	_DropWatches();
}


void
MultiAudioDeviceWatcher::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgRescan:
			Rescan();
			break;

		case B_NODE_MONITOR:
		{
			int32 opcode;
			if (message->FindInt32("opcode", &opcode) != B_OK)
				break;

			// Reconcile wholesale instead of deriving the affected device from
			// the message. It is idempotent, and it also picks up whole
			// subdirectories (/dev/audio/hmulti/usb) appearing at once.
			if (opcode == B_ENTRY_CREATED || opcode == B_ENTRY_REMOVED
					|| opcode == B_ENTRY_MOVED) {
				Rescan();
			}
			break;
		}

		default:
			BLooper::MessageReceived(message);
			break;
	}
}


void
MultiAudioDeviceWatcher::PrepareToQuit()
{
	atomic_set(&fQuitting, 1);
}


void
MultiAudioDeviceWatcher::Rescan()
{
	if (atomic_get(&fQuitting) != 0)
		return;

	BList paths;

	_ArmRoot();
	bool scanned = _Rearm(fRootPath.String(), NULL, 0, paths);

	// A root we could not even open is not evidence that every device went
	// away, so leave the flavor list alone in that case.
	if (scanned && atomic_get(&fQuitting) == 0)
		fAddOn->DevicesChanged(paths, fRootPath.String());

	for (int32 i = 0; i < paths.CountItems(); i++)
		delete (BString*)paths.ItemAt(i);
}


/*!	Watches the shallowest existing directory on the way down to the root path.

	On a machine whose only sound card is on USB, /dev/audio/hmulti does not
	exist until that card is plugged in, so watching the root alone would never
	fire. Watching its nearest existing ancestor gets us the notification that
	the root itself was created.
*/
void
MultiAudioDeviceWatcher::_ArmRoot()
{
	BPath path;
	if (path.SetTo(fRootPath.String()) != B_OK)
		return;

	while (true) {
		BDirectory directory(path.Path());
		if (directory.InitCheck() == B_OK) {
			node_ref nodeRef;
			if (directory.GetNodeRef(&nodeRef) == B_OK && !_IsWatched(nodeRef))
				_Watch(nodeRef);
			return;
		}

		BPath parent;
		if (path.GetParent(&parent) != B_OK)
			return;
		if (strcmp(parent.Path(), path.Path()) == 0)
			return;

		path = parent;
	}
}


/*!	Arms a watch on \a rootEntry (or \a rootPath) and every directory below it,
	appending the full path of each device found to \a paths.

	Returns whether the directory could be read at all.
*/
bool
MultiAudioDeviceWatcher::_Rearm(const char* rootPath, BEntry* rootEntry,
	uint32 depth, BList& paths)
{
	if (depth > 16)
		return false;

	BDirectory root;
	if (rootEntry != NULL)
		root.SetTo(rootEntry);
	else if (rootPath != NULL)
		root.SetTo(rootPath);
	else
		return false;

	if (root.InitCheck() != B_OK)
		return false;

	// Watch before listing: a device published while we walk then arrives as a
	// notification rather than falling into the gap between the two steps.
	node_ref nodeRef;
	if (root.GetNodeRef(&nodeRef) == B_OK && !_IsWatched(nodeRef))
		_Watch(nodeRef);

	BEntry entry;
	while (root.GetNextEntry(&entry) == B_OK) {
		if (entry.IsDirectory()) {
			_Rearm(rootPath, &entry, depth + 1, paths);
		} else {
			BPath path;
			if (entry.GetPath(&path) != B_OK)
				continue;

			BString* item = new (std::nothrow) BString(path.Path());
			if (item == NULL)
				continue;
			if (!paths.AddItem(item))
				delete item;
		}
	}

	return true;
}


bool
MultiAudioDeviceWatcher::_IsWatched(const node_ref& nodeRef) const
{
	for (int32 i = 0; i < fWatched.CountItems(); i++) {
		const node_ref* watched = (const node_ref*)fWatched.ItemAt(i);
		if (watched != NULL && watched->device == nodeRef.device
				&& watched->node == nodeRef.node) {
			return true;
		}
	}

	return false;
}


void
MultiAudioDeviceWatcher::_Watch(const node_ref& nodeRef)
{
	if (watch_node(&nodeRef, B_WATCH_DIRECTORY, BMessenger(this)) != B_OK)
		return;

	node_ref* copy = new (std::nothrow) node_ref(nodeRef);
	if (copy != NULL && fWatched.AddItem(copy))
		return;

	// Without a record of it we could not drop it later, so do not keep it.
	delete copy;
	watch_node(&nodeRef, B_STOP_WATCHING, BMessenger(this));
}


void
MultiAudioDeviceWatcher::_DropWatches()
{
	stop_watching(BMessenger(this));

	for (int32 i = 0; i < fWatched.CountItems(); i++)
		delete (node_ref*)fWatched.ItemAt(i);

	fWatched.MakeEmpty();
}


//	#pragma mark - MultiAudioAddOn


MultiAudioAddOn::MultiAudioAddOn(image_id image)
	:
	BMediaAddOn(image),
	fInitStatus(B_OK),
	fLock("multi_audio add-on"),
	fDevices(),
	fWatcher(NULL)
{
	CALLED();
	fInitStatus = _RecursiveScan(kDevicesRoot);
	if (fInitStatus != B_OK)
		return;

	_LoadSettings();

	fWatcher = new (std::nothrow) MultiAudioDeviceWatcher(this, kDevicesRoot);
	if (fWatcher != NULL) {
		fWatcher->Run();
		// Arm on the watcher thread so that every scan runs there and cannot
		// race this one. It also catches devices published while we scanned.
		fWatcher->PostMessage(kMsgRescan);
	} else
		PRINT(("MultiAudioAddOn: no watcher, hot-plug will not be noticed\n"));

	fInitStatus = B_OK;
}


MultiAudioAddOn::~MultiAudioAddOn()
{
	CALLED();

	if (fWatcher != NULL) {
		// Reconciling opens device nodes, so the watcher can be parked inside
		// a driver by the time we get here. Tell it to stop calling back, then
		// take the looper lock with a bound: a wedged device must not hold up
		// the media server's shutdown, which is a visible, whole-desktop hang.
		// If the lock never comes the team is on its way out regardless, and
		// the looper dies with it.
		fWatcher->PrepareToQuit();
		if (fWatcher->LockWithTimeout(2000000) == B_OK)
			fWatcher->Quit();
		fWatcher = NULL;
	}

	for (int32 i = 0; i < fDevices.CountItems(); i++) {
		MultiAudioSlot* slot = (MultiAudioSlot*)fDevices.ItemAt(i);
		if (slot == NULL)
			continue;

		delete slot->device;
		delete slot;
	}
	fDevices.MakeEmpty();

	_SaveSettings();
}


status_t
MultiAudioAddOn::InitCheck(const char** _failureText)
{
	CALLED();
	return fInitStatus;
}


int32
MultiAudioAddOn::CountFlavors()
{
	CALLED();

	BAutolock locker(fLock);
		// Slots of departed devices are counted too - see MultiAudioSlot.
	return fDevices.CountItems();
}


status_t
MultiAudioAddOn::GetFlavorAt(int32 index, const flavor_info** _info)
{
	CALLED();

	BAutolock locker(fLock);

	MultiAudioSlot* slot = (MultiAudioSlot*)fDevices.ItemAt(index);
	if (slot == NULL || !slot->present || slot->device == NULL)
		return B_BAD_INDEX;

	flavor_info* info = new (std::nothrow) flavor_info;
	if (info == NULL)
		return B_NO_MEMORY;

	MultiAudioNode::GetFlavor(info, index);

	const multi_description& description = slot->device->Description();
	info->name = description.friendly_name;

	// GetFlavor() claims both kinds for every device. Advertise only what the
	// hardware really has, or an output-only card turns up in Media
	// preferences as a recording source that can never produce a frame.
	if (description.input_channel_count <= 0)
		info->kinds &= ~(uint64)B_PHYSICAL_INPUT;
	if (description.output_channel_count <= 0)
		info->kinds &= ~(uint64)B_PHYSICAL_OUTPUT;

	*_info = info;
	return B_OK;
}


BMediaNode*
MultiAudioAddOn::InstantiateNodeFor(const flavor_info* info, BMessage* config,
	status_t* _error)
{
	CALLED();

	fLock.Lock();
	MultiAudioSlot* slot = (MultiAudioSlot*)fDevices.ItemAt(info->internal_id);
	MultiAudioDevice* device = slot != NULL && slot->present
		? slot->device : NULL;
	fLock.Unlock();
		// The node constructor is heavy and may call back into us; the device
		// itself outlives the slot's presence, so it stays safe to use here.

	if (device == NULL) {
		*_error = B_ERROR;
		return NULL;
	}

#ifdef MULTI_SAVE
	if (fSettings.FindMessage(device->Description().friendly_name, config)
			== B_OK) {
		fSettings.RemoveData(device->Description().friendly_name);
	}
#endif

	MultiAudioNode* node = new (std::nothrow) MultiAudioNode(this,
		device->Description().friendly_name, device, info->internal_id, config);
	if (node == NULL)
		*_error = B_NO_MEMORY;
	else
		*_error = node->InitCheck();

	return node;
}


status_t
MultiAudioAddOn::GetConfigurationFor(BMediaNode* _node, BMessage* message)
{
	CALLED();
	MultiAudioNode* node = dynamic_cast<MultiAudioNode*>(_node);
	if (node == NULL)
		return B_BAD_TYPE;

#ifdef MULTI_SAVE
	if (message == NULL) {
		BMessage settings;
		if (node->GetConfigurationFor(&settings) == B_OK) {
			fSettings.AddMessage(node->Name(), &settings);
		}
		return B_OK;
	}
#endif

	// currently never called by the media kit. Seems it is not implemented.

	return node->GetConfigurationFor(message);
}


bool
MultiAudioAddOn::WantsAutoStart()
{
	CALLED();
	return false;
}


status_t
MultiAudioAddOn::AutoStart(int count, BMediaNode** _node, int32* _internalID,
	bool* _hasMore)
{
	CALLED();
	return B_OK;
}


void
MultiAudioAddOn::DevicesChanged(const BList& paths, const char* rootPath)
{
	bool changed = false;
	size_t rootLength = strlen(rootPath);

	fLock.Lock();

	// Devices we have no live slot for are new.
	for (int32 i = 0; i < paths.CountItems(); i++) {
		const BString* path = (const BString*)paths.ItemAt(i);
		if (path == NULL || _SlotForPath(path->String()) != NULL)
			continue;

		const char* name = path->String();
		if ((size_t)path->Length() > rootLength)
			name += rootLength;

		if (_AddDevice(name, path->String())) {
			PRINT(("MultiAudioAddOn: added device %s\n", path->String()));
			changed = true;
		}
	}

	// Live slots with no device behind them any more are gone.
	for (int32 i = 0; i < fDevices.CountItems(); i++) {
		MultiAudioSlot* slot = (MultiAudioSlot*)fDevices.ItemAt(i);
		if (slot == NULL || !slot->present)
			continue;

		bool found = false;
		for (int32 j = 0; j < paths.CountItems() && !found; j++) {
			const BString* path = (const BString*)paths.ItemAt(j);
			found = path != NULL && *path == slot->path;
		}

		if (!found) {
			PRINT(("MultiAudioAddOn: device %s went away\n",
				slot->path.String()));
			slot->present = false;
			changed = true;
		}
	}

	fLock.Unlock();

	// Never notify under the lock: the server answers by calling straight back
	// into CountFlavors()/GetFlavorAt().
	if (changed)
		NotifyFlavorChange();
}


/*!	Returns the live slot for \a path, or NULL.

	Departed slots are deliberately ignored, so a device re-plugged under a
	devfs name we have seen before gets a fresh slot instead of reviving a
	handle that a still-running node may be holding.
*/
MultiAudioSlot*
MultiAudioAddOn::_SlotForPath(const char* path) const
{
	for (int32 i = 0; i < fDevices.CountItems(); i++) {
		MultiAudioSlot* slot = (MultiAudioSlot*)fDevices.ItemAt(i);
		if (slot != NULL && slot->present && slot->path == path)
			return slot;
	}

	return NULL;
}


//!	Caller must hold fLock (or be the constructor, before the watcher exists).
bool
MultiAudioAddOn::_AddDevice(const char* name, const char* path)
{
	MultiAudioDevice* device = new (std::nothrow) MultiAudioDevice(name, path);
	if (device == NULL)
		return false;

	if (device->InitCheck() != B_OK) {
		delete device;
		return false;
	}

	MultiAudioSlot* slot = new (std::nothrow) MultiAudioSlot;
	if (slot == NULL) {
		delete device;
		return false;
	}

	slot->device = device;
	slot->path = path;
	slot->present = true;

	if (!fDevices.AddItem(slot)) {
		delete device;
		delete slot;
		return false;
	}

	return true;
}


status_t
MultiAudioAddOn::_RecursiveScan(const char* rootPath, BEntry* rootEntry, uint32 depth)
{
	CALLED();
	if (depth > 16)
		return B_ERROR;

	BDirectory root;
	if (rootEntry != NULL)
		root.SetTo(rootEntry);
	else if (rootPath != NULL)
		root.SetTo(rootPath);
	else {
		PRINT(("Error in MultiAudioAddOn::RecursiveScan() null params\n"));
		return B_ERROR;
	}

	BEntry entry;
	while (root.GetNextEntry(&entry) == B_OK) {
		if (entry.IsDirectory()) {
			_RecursiveScan(rootPath, &entry, depth + 1);
		} else {
			BPath path;
			if (entry.GetPath(&path) != B_OK)
				continue;

			_AddDevice(path.Path() + strlen(rootPath), path.Path());
		}
	}

	return B_OK;
}


void
MultiAudioAddOn::_SaveSettings()
{
	CALLED();
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return;

	path.Append(kSettingsName);

	BFile file(path.Path(), B_READ_WRITE | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() == B_OK)
		fSettings.Flatten(&file);
}


void
MultiAudioAddOn::_LoadSettings()
{
	CALLED();
	fSettings.MakeEmpty();

	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return;

	path.Append(kSettingsName);

	BFile file(path.Path(), B_READ_ONLY);
	if (file.InitCheck() == B_OK && fSettings.Unflatten(&file) == B_OK) {
		PRINT_OBJECT(fSettings);
	} else {
		PRINT(("Error unflattening settings file %s\n", path.Path()));
	}
}
