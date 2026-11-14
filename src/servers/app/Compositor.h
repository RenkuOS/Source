/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef COMPOSITOR_H
#define COMPOSITOR_H


#include <vector>

#include <GraphicsDefs.h>
#include <Locker.h>
#include <Rect.h>
#include <Region.h>


class Desktop;
class RenderingBuffer;
class Window;


/*!	Builds the screen from the windows' off-screen buffers.

	Windows draw into a WindowBuffer. Whatever they have drawn, and whatever
	changes on the desktop, is passed here as damage: the compositor draws these
	parts of the screen again from the buffers, and no application is involved.
*/
class Compositor {
public:
								Compositor(Desktop* desktop);
								~Compositor();

			// The window lock must be held at least for reading.
			void				Composite(const BRegion& damage);

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

			std::vector<Layer*>	fLayers;
			std::vector<SceneEntry>
								fScene;
			std::vector<uint8>	fShadowColumns;
			const void*			fSceneBits;
			uint32				fSceneWidth;
			uint32				fSceneHeight;
};


#endif	// COMPOSITOR_H
