/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


/*!	Tests for PlaybackBuffer, the part of SoundRecorder that fills the
	buffers of the sound player from the decoder.

	Build and run (on RenkuOS, from the top of the source tree):
		S=src/apps/soundrecorder
		T=src/tests/apps/soundrecorder
		g++ -I $S $S/PlaybackBuffer.cpp $T/PlaybackBufferTest.cpp \
			-o PlaybackBufferTest
		./PlaybackBufferTest
*/


#include "PlaybackBuffer.h"

#include <stdio.h>
#include <string.h>


static const size_t kChannels = 2;
static const size_t kFrameSize = kChannels * sizeof(int32);
static const uint8 kGuard = 0xa5;
static const size_t kGuardSize = 64 * 1024;

static int sFailures = 0;


#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			sFailures++; \
		} \
	} while (false)


/*!	Stands in for BMediaTrack::ReadFrames(): hands out \c fChunk frames per
	call until \c fTotal frames are read. Each sample holds its frame number.
*/
struct FakeTrack {
	int64	fChunk;
	int64	fTotal;
	int64	fNext;
	int32	fCalls;
	bool	fFail;
};


static status_t
ReadFake(void* cookie, void* buffer, int64* _frameCount)
{
	FakeTrack* track = (FakeTrack*)cookie;
	track->fCalls++;
	if (track->fFail || track->fNext >= track->fTotal) {
		*_frameCount = 0;
		return B_ERROR;
	}

	int64 frames = track->fTotal - track->fNext;
	if (frames > track->fChunk)
		frames = track->fChunk;

	int32* samples = (int32*)buffer;
	for (int64 i = 0; i < frames * (int64)kChannels; i++)
		samples[i] = (int32)(track->fNext + i / kChannels);

	track->fNext += frames;
	*_frameCount = frames;
	return B_OK;
}


static FakeTrack
MakeTrack(int64 chunk, int64 total)
{
	FakeTrack track = { chunk, total, 0, 0, false };
	return track;
}


//!	Output buffer with room for a whole decoder buffer behind it.
static uint8*
NewOutput(size_t size)
{
	uint8* output = new uint8[size + kGuardSize];
	memset(output, kGuard, size + kGuardSize);
	return output;
}


static bool
GuardIntact(const uint8* output, size_t size)
{
	for (size_t i = size; i < size + kGuardSize; i++) {
		if (output[i] != kGuard)
			return false;
	}
	return true;
}


static bool
HoldsFrames(const uint8* output, int64 first, int64 count)
{
	const int32* samples = (const int32*)output;
	for (int64 i = 0; i < count * (int64)kChannels; i++) {
		if (samples[i] != (int32)(first + i / kChannels))
			return false;
	}
	return true;
}


static bool
IsSilence(const uint8* data, size_t size, uint8 silence)
{
	for (size_t i = 0; i < size; i++) {
		if (data[i] != silence)
			return false;
	}
	return true;
}


static void
TestSmallDecoderChunksFillTheWholeBuffer()
{
	printf("TestSmallDecoderChunksFillTheWholeBuffer\n");
	const size_t size = 512 * kFrameSize;
	FakeTrack track = MakeTrack(100, 10000);
	PlaybackBuffer buffer;
	CHECK(buffer.SetTo(100 * kFrameSize, kFrameSize, 0) == B_OK);
	uint8* output = NewOutput(size);

	CHECK(buffer.Fill(output, size, ReadFake, &track) == 512);
	CHECK(HoldsFrames(output, 0, 512));
	CHECK(buffer.Fill(output, size, ReadFake, &track) == 512);
	CHECK(HoldsFrames(output, 512, 512));
	CHECK(GuardIntact(output, size));
	delete[] output;
}


static void
TestLargeDecoderChunksStayInsideTheBuffer()
{
	printf("TestLargeDecoderChunksStayInsideTheBuffer\n");
	const size_t size = 512 * kFrameSize;
	FakeTrack track = MakeTrack(4096, 10000);
	PlaybackBuffer buffer;
	CHECK(buffer.SetTo(4096 * kFrameSize, kFrameSize, 0) == B_OK);
	uint8* output = NewOutput(size);

	CHECK(buffer.Fill(output, size, ReadFake, &track) == 512);
	CHECK(GuardIntact(output, size));
	CHECK(HoldsFrames(output, 0, 512));
	CHECK(buffer.Fill(output, size, ReadFake, &track) == 512);
	CHECK(HoldsFrames(output, 512, 512));
	CHECK(track.fCalls == 1);
	delete[] output;
}


static void
TestEndOfTrackIsFollowedBySilence()
{
	printf("TestEndOfTrackIsFollowedBySilence\n");
	const size_t size = 512 * kFrameSize;
	FakeTrack track = MakeTrack(512, 700);
	PlaybackBuffer buffer;
	CHECK(buffer.SetTo(512 * kFrameSize, kFrameSize, 0) == B_OK);
	uint8* output = NewOutput(size);

	CHECK(buffer.Fill(output, size, ReadFake, &track) == 512);
	CHECK(buffer.Fill(output, size, ReadFake, &track) == 188);
	CHECK(HoldsFrames(output, 512, 188));
	CHECK(IsSilence(output + 188 * kFrameSize, size - 188 * kFrameSize, 0));
	CHECK(buffer.Fill(output, size, ReadFake, &track) == 0);
	CHECK(IsSilence(output, size, 0));
	CHECK(GuardIntact(output, size));
	delete[] output;
}


static void
TestUnsignedSilenceIsTheMiddleValue()
{
	printf("TestUnsignedSilenceIsTheMiddleValue\n");
	const size_t size = 64 * kFrameSize;
	FakeTrack track = MakeTrack(64, 10);
	PlaybackBuffer buffer;
	CHECK(buffer.SetTo(64 * kFrameSize, kFrameSize, 0x80) == B_OK);
	uint8* output = NewOutput(size);

	CHECK(buffer.Fill(output, size, ReadFake, &track) == 10);
	CHECK(IsSilence(output + 10 * kFrameSize, size - 10 * kFrameSize, 0x80));
	buffer.FillSilence(output, size);
	CHECK(IsSilence(output, size, 0x80));
	CHECK(GuardIntact(output, size));
	delete[] output;
}


static void
TestClearDropsDecodedFrames()
{
	printf("TestClearDropsDecodedFrames\n");
	const size_t size = 512 * kFrameSize;
	FakeTrack track = MakeTrack(1024, 10000);
	PlaybackBuffer buffer;
	CHECK(buffer.SetTo(1024 * kFrameSize, kFrameSize, 0) == B_OK);
	uint8* output = NewOutput(size);

	CHECK(buffer.Fill(output, size, ReadFake, &track) == 512);
	buffer.Clear();
	track.fNext = 5000;
	CHECK(buffer.Fill(output, size, ReadFake, &track) == 512);
	CHECK(HoldsFrames(output, 5000, 512));
	delete[] output;
}


static void
TestReadErrorGivesSilence()
{
	printf("TestReadErrorGivesSilence\n");
	const size_t size = 512 * kFrameSize;
	FakeTrack track = MakeTrack(512, 10000);
	track.fFail = true;
	PlaybackBuffer buffer;
	CHECK(buffer.SetTo(512 * kFrameSize, kFrameSize, 0) == B_OK);
	uint8* output = NewOutput(size);

	CHECK(buffer.Fill(output, size, ReadFake, &track) == 0);
	CHECK(IsSilence(output, size, 0));
	CHECK(GuardIntact(output, size));
	delete[] output;
}


static void
TestPartialFrameAtTheEndOfTheOutputIsSilence()
{
	printf("TestPartialFrameAtTheEndOfTheOutputIsSilence\n");
	const size_t size = 10 * kFrameSize + 3;
	FakeTrack track = MakeTrack(512, 10000);
	PlaybackBuffer buffer;
	CHECK(buffer.SetTo(512 * kFrameSize, kFrameSize, 0) == B_OK);
	uint8* output = NewOutput(size);

	CHECK(buffer.Fill(output, size, ReadFake, &track) == 10);
	CHECK(IsSilence(output + 10 * kFrameSize, 3, 0));
	CHECK(GuardIntact(output, size));
	delete[] output;
}


static void
TestInvalidSizesAreRejected()
{
	printf("TestInvalidSizesAreRejected\n");
	PlaybackBuffer buffer;
	CHECK(buffer.SetTo(0, kFrameSize, 0) == B_BAD_VALUE);
	CHECK(buffer.SetTo(kFrameSize - 1, kFrameSize, 0) == B_BAD_VALUE);
	CHECK(buffer.SetTo(512 * kFrameSize, 0, 0) == B_BAD_VALUE);

	const size_t size = 16 * kFrameSize;
	FakeTrack track = MakeTrack(512, 10000);
	uint8* output = NewOutput(size);
	CHECK(buffer.Fill(output, size, ReadFake, &track) == 0);
	CHECK(IsSilence(output, size, 0));
	CHECK(track.fCalls == 0);
	delete[] output;
}


int
main()
{
	TestSmallDecoderChunksFillTheWholeBuffer();
	TestLargeDecoderChunksStayInsideTheBuffer();
	TestEndOfTrackIsFollowedBySilence();
	TestUnsignedSilenceIsTheMiddleValue();
	TestClearDropsDecodedFrames();
	TestReadErrorGivesSilence();
	TestPartialFrameAtTheEndOfTheOutputIsSilence();
	TestInvalidSizesAreRejected();

	if (sFailures != 0) {
		printf("%d check(s) failed\n", sFailures);
		return 1;
	}
	printf("all tests passed\n");
	return 0;
}
