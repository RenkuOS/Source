/*
 * Copyright (c) 2002, Jerome Duval (jerome.duval@free.fr)
 * Distributed under the terms of the MIT License.
 */
#ifndef MULTI_AUDIO_ADDON_H
#define MULTI_AUDIO_ADDON_H


#include <List.h>
#include <Locker.h>
#include <Looper.h>
#include <MediaDefs.h>
#include <MediaAddOn.h>
#include <Message.h>
#include <Node.h>
#include <String.h>

class BEntry;
class MultiAudioAddOn;
class MultiAudioDevice;


/*!	One entry of the add-on's flavor list.

	Slots are append-only for the lifetime of the add-on: a flavor's index is
	its \c internal_id, and the media server hands that index back to
	InstantiateNodeFor() long after the flavor was published. Compacting the
	list on unplug would silently repoint a stale id at the wrong device, so
	vanished devices merely clear \a present and keep their slot.

	\a device is likewise never freed before the add-on itself goes away - a
	MultiAudioNode instantiated from this slot may still be running when the
	hardware disappears.
*/
struct MultiAudioSlot {
	MultiAudioDevice*	device;
	BString				path;
	bool				present;
};


/*!	Watches /dev/audio/hmulti/ so that hot-plugged sound cards show up without
	a media services restart. devfs posts entry-created/removed notifications
	for published devices, which is what makes this work at all.
*/
class MultiAudioDeviceWatcher : public BLooper {
public:
							MultiAudioDeviceWatcher(MultiAudioAddOn* addOn,
								const char* rootPath);
	virtual					~MultiAudioDeviceWatcher();

	virtual	void			MessageReceived(BMessage* message);

			//! Arms the watches, then reconciles against what is there now.
			void			Rescan();

			/*!	Tells the watcher that the add-on is going away, so it stops
				calling back into it. Safe to call from any thread.
			*/
			void			PrepareToQuit();

private:
			void			_ArmRoot();
			bool			_Rearm(const char* rootPath, BEntry* rootEntry,
								uint32 depth, BList& paths);
			bool			_IsWatched(const node_ref& nodeRef) const;
			void			_Watch(const node_ref& nodeRef);
			void			_DropWatches();

private:
	MultiAudioAddOn*		fAddOn;
	BString					fRootPath;
	BList					fWatched;
		// node_ref*, the directories we currently hold a watch on
	int32					fQuitting;
		// set once the add-on starts tearing down
};


class MultiAudioAddOn : public BMediaAddOn {
public:
						MultiAudioAddOn(image_id image);
	virtual				~MultiAudioAddOn();

	virtual	status_t	InitCheck(const char** _failureText);
	virtual	int32		CountFlavors();
	virtual	status_t	GetFlavorAt(int32 i, const flavor_info** _info);
	virtual	BMediaNode*	InstantiateNodeFor(const flavor_info* info,
							BMessage* config, status_t* _error);
	virtual	status_t	GetConfigurationFor(BMediaNode* node,
							BMessage* message);
	virtual	bool		WantsAutoStart();
	virtual	status_t	AutoStart(int count, BMediaNode** _node,
							int32* _internalID, bool* _hasMore);

			/*!	Reconciles the flavor list against \a paths, the full device
				paths found by the most recent scan. Called on the watcher
				thread; notifies the media server if anything changed.
			*/
			void		DevicesChanged(const BList& paths,
							const char* rootPath);

private:
			status_t	_RecursiveScan(const char* path, BEntry* rootEntry = NULL,
							uint32 depth = 0);
			MultiAudioSlot*	_SlotForPath(const char* path) const;
			bool		_AddDevice(const char* name, const char* path);
			void		_SaveSettings();
			void		_LoadSettings();

private:
	status_t 		fInitStatus;
	BLocker			fLock;
		// guards fDevices; the watcher thread mutates it behind the media
		// server's back
	BList			fDevices;
		// MultiAudioSlot*, indexed by flavor internal_id

	MultiAudioDeviceWatcher* fWatcher;

	BMessage		fSettings;
		// loaded from settings directory
};

extern "C" BMediaAddOn* make_media_addon(image_id you);

#endif	// MULTI_AUDIO_ADDON_H
