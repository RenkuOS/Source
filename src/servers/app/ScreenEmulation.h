/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef SCREEN_EMULATION_H
#define SCREEN_EMULATION_H


#include <GraphicsDefs.h>
#include <Locker.h>
#include <OS.h>

#include <vector>

#include "EmulatedScreenLayout.h"


class BPoint;
class HWInterface;
struct emulated_screen;


/*!	A screen for a BWindowScreen that can't have the real one: on a rotated
	screen, or in a mode the screen doesn't have. The application draws to a
	frame buffer in an area shared with it, and while it has the screen, that
	is shown on the screen once a frame, scaled to fit and turned with the
	screen.
*/
class ScreenEmulation {
public:
								ScreenEmulation(team_id team);
								~ScreenEmulation();

			team_id				Team() const { return fTeam; }
			area_id				Area() const { return fArea; }

			status_t			SetMode(uint32 width, uint32 height,
									uint32 displayWidth, uint32 displayHeight,
									color_space space);

			void				Start(::HWInterface* interface);
			void				Stop();
			bool				IsRunning() const { return fThread >= 0; }

			bool				MapPoint(BPoint& point);

private:
	static	status_t			_ThreadEntry(void* data);
			void				_Run();
			void				_Present();
			void				_ConvertRow(const uint8* source,
									uint32* target, int32 count) const;

private:
			team_id				fTeam;
			BLocker				fLock;
			area_id				fArea;
			emulated_screen*	fScreen;

			// the frame buffer's geometry: the application can change the
			// copy in fScreen, which is never trusted
			const uint8*		fBits;
			uint32				fWidth;
			uint32				fHeight;
			uint32				fBytesPerRow;
			color_space			fSpace;
			uint32				fDisplayWidth;
			uint32				fDisplayHeight;

			::HWInterface*		fInterface;
			thread_id			fThread;
	volatile bool				fQuit;

			// what is shown: the screen as large as it is drawn, with the
			// frame buffer scaled into it; fLayoutLock guards the layout
			// for MapPoint()
			std::vector<uint32>	fFrame;
			BLocker				fLayoutLock;
			int32				fFrameWidth;
			int32				fFrameHeight;
			uint32				fShownWidth;
			uint32				fShownHeight;
			emulated_screen_layout fLayout;
			std::vector<int32>	fColumns;
			uint32				fPalette[256];
};


#endif	// SCREEN_EMULATION_H
