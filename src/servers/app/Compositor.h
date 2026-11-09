/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */
#ifndef COMPOSITOR_H
#define COMPOSITOR_H


#include <vector>

#include <GraphicsDefs.h>
#include <Locker.h>
#include <OS.h>
#include <Rect.h>
#include <Region.h>


class Desktop;
class RenderingBuffer;
class Window;


/*!	Builds the screen from the windows' off-screen buffers.

	Windows draw into a WindowBuffer. Whatever they have drawn, and whatever
	changes on the desktop, is passed here as damage: the compositor draws these
	parts of the screen again from the buffers, and no application is involved.

	It does so on a thread of its own, at most once for every refresh of the
	screen, and after the vertical retrace where the driver can wait for it. So
	what is drawn many times per frame only reaches the screen once.
*/
class Compositor {
public:
								Compositor(Desktop* desktop);
								~Compositor();

			status_t			InitCheck() const;

			// Notes what has to be drawn again. This can be called from
			// anywhere, and doesn't wait for anything.
			void				Composite(const BRegion& damage);

			// Draws what is due right now, for reading back what is on the
			// screen. No window lock may be held.
			void				Flush();
			// The same, for those who hold the window lock already.
			void				FlushLocked();

			// An application has taken over the screen, and this returns when
			// nothing will touch it anymore. What happens from now on is drawn
			// when it gives the screen back with ScreenReleased().
			void				ScreenTaken();
			void				ScreenReleased();

			// To be called with the window lock held for writing, when the
			// clipping of the windows has been rebuilt: finds out what has
			// changed since the last time, and composites these parts.
			void				SceneChanged();

private:
			struct SceneEntry {
				const Window*	window;
				BRect			frame;
			};
			struct Layer {
				Window*			window;
				BRegion			region;
				BRegion			shadow;
			};

	static	status_t			_ThreadEntry(void* data);
			void				_Run();
			void				_WaitForFrame();
			void				_CompositeDamage();
			void				_Composite(const BRegion& damage);

			void				_Fill(RenderingBuffer* target,
									const BRegion& region, const rgb_color& color);
			void				_Copy(RenderingBuffer* target, Window* window,
									const BRegion& region);
			void				_BlendEdges(RenderingBuffer* target,
									Window* window, const BRegion& region);
			void				_DrawShadow(RenderingBuffer* target,
									Window* window, const BRegion& region);

private:
			Desktop*			fDesktop;
			BLocker				fLock;

			BLocker				fDamageLock;
			BRegion				fDamage;
			bool				fWakePending;
			sem_id				fWakeSem;
			thread_id			fThread;
			volatile bool		fQuit;
			bigtime_t			fNextFrame;
			bool				fUsesRetrace;
			bool				fReportedPacing;

			std::vector<Layer*>	fLayers;
			std::vector<SceneEntry>
								fScene;
			std::vector<uint8>	fShadowColumns;
			const void*			fSceneBits;
			uint32				fSceneWidth;
			uint32				fSceneHeight;
};


#endif	// COMPOSITOR_H
