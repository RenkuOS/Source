/*
 * Copyright 2010, Haiku. All rights reserved.
 * Distributed under the terms of the MIT license.
 *
 * Authors:
 *		Marcus Overhagen
 *		Jérôme Duval
 */
/*
 * Copyright 2002, Marcus Overhagen. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _DEFAULT_MANAGER_H
#define _DEFAULT_MANAGER_H


/*!	Manager for defaults (audio and video, input and output)
*/


#include "DataExchange.h"

#include <Autolock.h>
#include <MediaRoster.h>
#include <Message.h>

class NodeManager;


class DefaultManager {
public:
								DefaultManager();
								~DefaultManager();

			status_t 			LoadState();
			status_t			SaveState(NodeManager *node_manager);

			status_t			Set(media_node_id nodeid,
									const char *input_name, int32 input_id,
									node_type type);
			status_t			Get(media_node_id *nodeid, char *input_name,
									int32 *input_id, node_type type);
			status_t			Rescan();

			void				Dump();

			void 				CleanupTeam(team_id team);

private:
			static int32		rescan_thread(void *arg);
			void				_RescanThread();

			// savedOnly: only look for the saved default, and leave *id
			// alone if it is not there.
			void				_FindPhysical(volatile media_node_id *id,
									uint32 default_type, bool isInput,
									media_type type, bool savedOnly = false);
			void				_FindAudioMixer();
			void				_FindTimeSource();

			status_t			_ConnectMixerToOutput();

			// See _FindPhysical().
			static	int32		_TypeIndex(uint32 defaultType);
			BMessage*			_SavedDefault(uint32 defaultType) const;

private:
			volatile bool 		fMixerConnected;
			volatile media_node_id fPhysicalVideoOut;
			volatile media_node_id fPhysicalVideoIn;
			volatile media_node_id fPhysicalAudioOut;
			volatile media_node_id fPhysicalAudioIn;
			volatile media_node_id fSystemTimeSource;
			volatile media_node_id fTimeSource;
			volatile media_node_id fAudioMixer;
			volatile int32 		fPhysicalAudioOutInputID;
			char fPhysicalAudioOutInputName[B_MEDIA_NAME_LENGTH];

			media_output 		fMixerToPhysicalAudioOut;
			media_input 		fPhysicalAudioOutFromMixer;

			BList				fMsgList;

			// Per default type (_TypeIndex()), whether the current
			// default is only standing in because the saved one was absent at
			// boot. SaveState() then keeps the saved preference.
			bool				fUsingFallback[4];

			uint32				fBeginHeader[3];
			uint32				fEndHeader[3];
			thread_id			fRescanThread;
			int32 				fRescanRequested;
			BLocker				fRescanLock;
			BMediaRoster*		fRoster;
};

#endif // _DEFAULT_MANAGER_H
