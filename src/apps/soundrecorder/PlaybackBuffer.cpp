/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "PlaybackBuffer.h"

#include <new>
#include <string.h>


PlaybackBuffer::PlaybackBuffer()
	:
	fBuffer(NULL),
	fReadSize(0),
	fFrameSize(0),
	fOffset(0),
	fUsed(0),
	fSilence(0)
{
}


PlaybackBuffer::~PlaybackBuffer()
{
	delete[] fBuffer;
}


/*!	\a readSize is the buffer size of the decoded format, the most one
	read can deliver. \a silence is the byte value of a silent sample.
*/
status_t
PlaybackBuffer::SetTo(size_t readSize, size_t frameSize, uint8 silence)
{
	delete[] fBuffer;
	fBuffer = NULL;
	fReadSize = 0;
	fFrameSize = 0;
	Clear();

	if (frameSize == 0 || readSize < frameSize)
		return B_BAD_VALUE;

	fBuffer = new(std::nothrow) uint8[readSize];
	if (fBuffer == NULL)
		return B_NO_MEMORY;

	fReadSize = readSize;
	fFrameSize = frameSize;
	fSilence = silence;
	return B_OK;
}


//!	Drops the decoded frames that were not handed out yet, after a seek.
void
PlaybackBuffer::Clear()
{
	fOffset = 0;
	fUsed = 0;
}


/*!	Fills \a data with whole frames until \a size bytes are used or the
	track ends, the rest is silence. Never writes more than \a size bytes.
	Returns the number of frames taken from the track.
*/
int64
PlaybackBuffer::Fill(void* data, size_t size, playback_read_func read,
	void* cookie)
{
	uint8* output = (uint8*)data;
	size_t wanted = fFrameSize == 0 ? 0 : size - size % fFrameSize;
	size_t filled = 0;

	while (filled < wanted) {
		if (fOffset == fUsed && !_ReadMore(read, cookie))
			break;

		size_t bytes = fUsed - fOffset;
		if (bytes > wanted - filled)
			bytes = wanted - filled;

		memcpy(output + filled, fBuffer + fOffset, bytes);
		fOffset += bytes;
		filled += bytes;
	}

	FillSilence(output + filled, size - filled);
	return filled == 0 ? 0 : filled / fFrameSize;
}


void
PlaybackBuffer::FillSilence(void* data, size_t size) const
{
	memset(data, fSilence, size);
}


bool
PlaybackBuffer::_ReadMore(playback_read_func read, void* cookie)
{
	Clear();

	int64 frames = 0;
	if (read(cookie, fBuffer, &frames) != B_OK || frames <= 0)
		return false;

	size_t bytes = (size_t)frames * fFrameSize;
	fUsed = bytes < fReadSize ? bytes : fReadSize;
	return true;
}
