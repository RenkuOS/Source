/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PLAYBACK_BUFFER_H
#define PLAYBACK_BUFFER_H


#include <SupportDefs.h>


//!	Reads up to the decoder's buffer size into \a buffer, like BMediaTrack.
typedef status_t (*playback_read_func)(void* cookie, void* buffer,
	int64* _frameCount);


class PlaybackBuffer {
public:
								PlaybackBuffer();
								~PlaybackBuffer();

			status_t			SetTo(size_t readSize, size_t frameSize,
									uint8 silence);
			void				Clear();

			int64				Fill(void* data, size_t size,
									playback_read_func read, void* cookie);
			void				FillSilence(void* data, size_t size) const;

private:
			bool				_ReadMore(playback_read_func read,
									void* cookie);

private:
			uint8*				fBuffer;
			size_t				fReadSize;
			size_t				fFrameSize;
			size_t				fOffset;
			size_t				fUsed;
			uint8				fSilence;
};


#endif	// PLAYBACK_BUFFER_H
