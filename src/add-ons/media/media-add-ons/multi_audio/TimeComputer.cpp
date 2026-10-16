/*
 * Copyright 2009, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */
#include "TimeComputer.h"

#include <OS.h>


TimeComputer::TimeComputer()
	:
	fRealTime(0),
	fPerformanceTime(0),
	fDrift(1),
	fFrameRate(1000000),
	fUsecsPerFrame(1),
	fPerformanceTimeBase(0),
	fFrameBase(0),
	fResetTimeBase(true),
	fFirstEntry(0),
	fLastEntry(0)
{
}


void
TimeComputer::Init(float frameRate, bigtime_t realBaseTime)
{
	fRealTime = realBaseTime;
	fPerformanceTime = 0;
	fDrift = 1;
	fResetTimeBase = true;
	SetFrameRate(frameRate);
}


void
TimeComputer::SetFrameRate(float frameRate)
{
	if (frameRate == fFrameRate)
		return;

	fFrameRate = frameRate;
	fUsecsPerFrame = (double)1000000 / fFrameRate;
	fResetTimeBase = true;
	fFirstEntry = 0;
	fLastEntry = 0;
}


/*!	Continues the performance timeline across a gap in which no time stamps
	arrived (the device was unplugged and has come back). Performance time is
	carried forward to \a realTime at a drift of exactly 1 -- the drift the
	node publishes, so this matches what consumers extrapolated during the gap
	and the timeline neither jumps nor restarts at 0 (which Init() would do).
	The next time stamp re-bases the frame count, since the device's own count
	started over.
*/
void
TimeComputer::Resync(bigtime_t realTime)
{
	fPerformanceTime += realTime - fRealTime;
	fRealTime = realTime;
	fDrift = 1;
	fResetTimeBase = true;
	fFirstEntry = 0;
	fLastEntry = 0;
}


void
TimeComputer::AddTimeStamp(bigtime_t realTime, uint64 frames)
{
	bigtime_t estimatedPerformanceTime = fPerformanceTime
		+ bigtime_t((realTime - fRealTime) * fDrift);

	fRealTime = realTime;

	if (fResetTimeBase) {
		// use the extrapolated performance time at the given real time
		fPerformanceTime = estimatedPerformanceTime;
		fPerformanceTimeBase = estimatedPerformanceTime;
		fFrameBase = frames;
		fResetTimeBase = false;
		_AddEntry(fRealTime, fPerformanceTime);
		fFirstEntry = fLastEntry;
		return;
	}

	// add entry
	bigtime_t performanceTime = fPerformanceTimeBase
		+ bigtime_t((frames - fFrameBase) * fUsecsPerFrame);
	_AddEntry(realTime, performanceTime);

	// Update performance time and drift. We don't use the given
	// performance time directly, but average it with the estimated
	// performance time.
	fPerformanceTime = (performanceTime + estimatedPerformanceTime) / 2;

	Entry& entry = fEntries[fFirstEntry];
	fDrift = double(fPerformanceTime - entry.performanceTime)
		/ double(fRealTime - entry.realTime);
}


void
TimeComputer::_AddEntry(bigtime_t realTime, bigtime_t performanceTime)
{
	fLastEntry = (fLastEntry + 1) % kEntryCount;
	Entry& entry = fEntries[fLastEntry];
	entry.realTime = realTime;
	entry.performanceTime = performanceTime;

	if (fLastEntry == fFirstEntry)
		fFirstEntry = (fFirstEntry + 1) % kEntryCount;
}
