/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
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
class WindowWobble;


/*!	Builds the screen from the windows' off-screen buffers.

	Windows draw into a WindowBuffer. Whatever they have drawn, and whatever
	changes on the desktop, is passed here as damage: the compositor draws these
	parts of the screen again from the buffers, and no application is involved.

	It does so on a thread of its own, at most once for every refresh of the
	screen, and after the vertical retrace where the driver can wait for it. So
	what is drawn many times per frame only reaches the screen once. Where
	much of the screen changes, it is cut into bands, which are drawn by one
	thread for each processor.
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

			// An application has taken over the screen, and this returns when
			// nothing will touch it anymore. What happens from now on is drawn
			// when it gives the screen back with ScreenReleased().
			void				ScreenTaken();
			void				ScreenReleased();

			// To be called with the window lock held for writing, when the
			// clipping of the windows has been rebuilt: finds out what has
			// changed since the last time, and composites these parts.
			void				SceneChanged();

			// A window is being dragged, grabbed at the given point of its
			// frame: it wobbles until it has been let go and has come to rest.
			// The wobbliness goes from 0 to 100.
			void				StartWobble(Window* window, BPoint grab,
									uint8 wobbliness);
			void				ReleaseWobble(Window* window);
			// The window's zoom button has been clicked: once the application
			// has zoomed it, it wobbles into its new frame.
			void				StartZoomWobble(Window* window,
									uint8 wobbliness);
			// The window is going away.
			void				ForgetWobble(Window* window);

private:
			struct Wobble;
			struct SceneEntry {
				const Window*	window;
				BRect			frame;
			};
			struct Layer {
				Window*			window;
				BRegion			region;
				BRegion			shadow;
				uint8			alpha;
				Wobble*			wobble;
				// The parts drawn by themselves, for a window that isn't
				// wobbling
				BRegion			opaque;
				BRegion			clear;
				BRegion			edge;
			};

	static	status_t			_ThreadEntry(void* data);
			void				_Run();
			void				_WaitForFrame();
			void				_CompositeDamage(BRegion* present = NULL);
			void				_Composite(const BRegion& damage,
									BRegion* present = NULL);
			void				_Present(const BRegion& region);

	static	status_t			_HelperEntry(void* data);
			void				_Help();
			void				_DrawBands(const BRegion& damage);
			void				_DrawNextBands(
									std::vector<uint8>& shadowColumns);
			void				_DrawBand(const BRect& band,
									std::vector<uint8>& shadowColumns);

			void				_Fill(RenderingBuffer* target,
									const BRegion& region, const rgb_color& color);
			void				_Copy(RenderingBuffer* target, Window* window,
									const BRegion& region);
			void				_Blend(RenderingBuffer* target, Window* window,
									const BRegion& region, uint8 alpha);
			void				_BlendPixels(RenderingBuffer* target,
									Window* window, const BRegion& region,
									uint8 alpha);
			void				_BlendEdges(RenderingBuffer* target,
									Window* window, const BRegion& region,
									uint8 alpha);
			void				_DrawShadow(RenderingBuffer* target,
									Window* window, const BRegion& region,
									std::vector<uint8>& columns);

			void				_ScheduleFrame();
			bool				_StepWobbles();
			Wobble*				_WobbleFor(const Window* window) const;
			void				_InitWobble(Wobble* wobble);
			void				_InitWobbleMasks(Wobble* wobble);
			bool				_WobbleBufferMoved(const Wobble* wobble,
									Window* window) const;
			void				_UpdateWobbleArea(Wobble* wobble);
			void				_DrawWobble(RenderingBuffer* target,
									Window* window, const Wobble* wobble,
									const BRegion& region, uint8 alpha);
			void				_FillTriangle(RenderingBuffer* target,
									const Wobble* wobble, const uint8* pixels,
									uint32 bytesPerRow,
									const clipping_rect& clip,
									const BPoint* points,
									const BPoint* sources, uint8 alpha);

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

			// Threads that draw bands of the screen together with the
			// compositor's own, and what they draw
			std::vector<thread_id>
								fHelpers;
			sem_id				fHelpSem;
			sem_id				fHelpDoneSem;
			RenderingBuffer*	fBandTarget;
			const BRegion*		fBandBackground;
			rgb_color			fBandColor;
			size_t				fBandLayerCount;
			BRect				fBandBounds;
			int32				fBandCount;
			int32				fNextBand;

			BLocker				fWobbleLock;
			std::vector<Wobble*>
								fWobbles;
			const void*			fSceneBits;
			uint32				fSceneWidth;
			uint32				fSceneHeight;
};


#endif	// COMPOSITOR_H
