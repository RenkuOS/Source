/*
 * Copyright 2000-2004 Ingo Weinhold <ingo_weinhold@gmx.de>
 * Copyright 2006-2008 Stephan Aßmus <superstippi@gmx.de>
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef MEDIA_TRACK_AUDIO_SUPPLIER_H
#define MEDIA_TRACK_AUDIO_SUPPLIER_H


#include <List.h>
#include <Locker.h>
#include <OS.h>

#include "AudioTrackSupplier.h"


class BMediaTrack;
struct media_codec_info;
struct media_format;


class MediaTrackAudioSupplier : public AudioTrackSupplier {
public:
								MediaTrackAudioSupplier(BMediaTrack* track,
									int32 trackIndex);
	virtual						~MediaTrackAudioSupplier();

	virtual	const media_format&	Format() const;
	virtual	status_t			GetEncodedFormat(media_format* format) const;
	virtual	status_t			GetCodecInfo(media_codec_info* info) const;
	virtual	bigtime_t			Duration() const;

	// AudioReader interface
	// (needed to reuse the class as AudioResampler input)
	virtual bigtime_t			InitialLatency() const;
	virtual	status_t			Read(void* buffer, int64 pos, int64 frames);

	virtual	status_t			InitCheck() const;

	virtual	int32				TrackIndex() const
									{ return fTrackIndex; }

private:
			struct Buffer;
			void				_InitFromTrack();

			int64				_FramesPerBuffer() const;

			void				_CopyFrames(void* source, int64 sourceOffset,
									void* target, int64 targetOffset,
									int64 position, int64 frames) const;
			void				_CopyFrames(Buffer* buffer, void* target,
									int64 targetOffset, int64 position,
									int64 frames) const;

			void				_AllocateBuffers();
			void				_FreeBuffers();
			Buffer*				_BufferAt(int32 index) const;
			Buffer*				_FindBufferAtFrame(int64 frame) const;
			Buffer*				_FindUnusedBuffer() const;
			Buffer*				_FindUsableBuffer() const;
			Buffer*				_FindUsableBufferFor(int64 position) const;
			void				_GetBuffersFor(BList& buffers, int64 position,
									int64 frames) const;
			void				_TouchBuffer(Buffer* buffer);

			status_t			_ReadBuffer(Buffer* buffer, int64 position);
			status_t			_ReadBuffer(Buffer* buffer, int64 position,
									bigtime_t time);

			void				_ReadCachedFrames(void*& buffer,
									int64& position, int64& frames,
									bigtime_t time);

			status_t			_ReadUncachedFrames(void* buffer,
									int64 position, int64 frames,
									bigtime_t time);

			int32				_BufferCount() const;
			Buffer*				_FindReadAheadBuffer(int64 playPosition)
									const;
	static	status_t			_ReadAheadEntry(void* cookie);
			void				_ReadAheadLoop();

			status_t			_FindKeyFrameForward(int64& position);
			status_t			_FindKeyFrameBackward(int64& position);
// NOTE: unused
//			status_t			_SeekToKeyFrameForward(int64& position);
			status_t			_SeekToKeyFrameBackward(int64& position);

 private:
			BMediaTrack*		fMediaTrack;
			char*				fBuffer;
			int64				fBufferOffset;
			int64				fBufferSize;
			BList				fBuffers;
			bool				fHasKeyFrames;
			int64				fCountFrames;
			bool				fReportSeekError;
			int32				fTrackIndex;

			// Read-ahead: a thread decodes the frames following the last
			// Read() into the cache, so that Read(), which the audio
			// producer calls in its real-time thread, rarely has to wait for
			// the disk. fTrackLock serializes every use of fMediaTrack,
			// fCacheLock the cache buffers and fPlayPosition. Lock order:
			// fTrackLock before fCacheLock.
			BLocker				fTrackLock;
			BLocker				fCacheLock;
			thread_id			fReadAheadThread;
			sem_id				fReadAheadSem;
			volatile bool		fQuitReadAhead;
			int64				fPlayPosition;
				// first frame after the last Read()
			char*				fReadAheadData;
				// decoded by the read-ahead thread, before it is cached
};

#endif	// MEDIA_TRACK_AUDIO_SUPPLIER_H
