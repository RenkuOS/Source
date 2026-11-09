/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


#include "Compositor.h"

#include <new>
#include <string.h>

#include <Autolock.h>
#include <ByteOrder.h>

#include "Desktop.h"
#include "HWInterface.h"
#include "RenderingBuffer.h"
#include "Window.h"
#include "WindowBuffer.h"
#include "WindowShape.h"


// The pixel code reads B_RGBA32 as blue, green, red and alpha bytes in memory,
// as on every architecture RenkuOS builds for. A big-endian one would need it
// changed.
#if B_HOST_IS_BENDIAN
#	error "the compositor expects B_RGBA32 pixels in little-endian order"
#endif


Compositor::Compositor(Desktop* desktop)
	:
	fDesktop(desktop),
	fLock("compositor"),
	fSceneBits(NULL),
	fSceneWidth(0),
	fSceneHeight(0)
{
}


Compositor::~Compositor()
{
	for (size_t i = 0; i < fLayers.size(); i++)
		delete fLayers[i];
}


void
Compositor::Composite(const BRegion& damage)
{
	if (damage.CountRects() == 0)
		return;

	// Other window threads may composite at the same time. Serializing them
	// also makes sure the screen ends up correct: a window's drawing is done
	// when it gets here, so whoever comes next sees its new pixels.
	BAutolock _(fLock);

	::HWInterface* interface = fDesktop->HWInterface();
	if (!interface->LockParallelAccess())
		return;

	RenderingBuffer* target = interface->DrawingBuffer();

	BRegion remaining(damage);
	BRegion bounds((BRect)target->Bounds());
	remaining.IntersectWith(&bounds);

	BRegion screenDamage(remaining);

	// Going from the front to the back, find out which part of which window
	// can be seen.
	WindowList& windows = fDesktop->CurrentWindows();
	int32 workspace = fDesktop->CurrentWorkspace();
	size_t layerCount = 0;
	for (Window* window = windows.LastWindow();
			window != NULL && remaining.CountRects() > 0;
			window = window->PreviousWindow(workspace)) {
		if (window->IsHidden() || !window->IsBufferReady())
			continue;

		// The others in a stack are behind it, with the same outline. They
		// would only make its shadow and corners darker.
		if (window->TopLayerStackWindow() != window)
			continue;

		const WindowShape& shape = window->Shape();
		if (!remaining.Intersects(shape.Bounds()))
			continue;

		if (layerCount == fLayers.size()) {
			Layer* layer = new(std::nothrow) Layer;
			if (layer == NULL)
				break;
			fLayers.push_back(layer);
		}

		Layer* layer = fLayers[layerCount];
		layer->region = remaining;
		layer->region.IntersectWith(&window->CompositeRegion());

		layer->shadow.MakeEmpty();
		if (shape.HasShadow() && remaining.Intersects(shape.ShadowFrame())) {
			layer->shadow = remaining;
			BRegion shadowFrame(shape.ShadowFrame());
			layer->shadow.IntersectWith(&shadowFrame);
			layer->shadow.Exclude(&shape.OpaqueRegion());
		}

		if (layer->region.CountRects() == 0
			&& layer->shadow.CountRects() == 0) {
			continue;
		}

		layer->window = window;
		layerCount++;

		// Only what hides everything behind it ends the search here.
		remaining.Exclude(&shape.OpaqueRegion());
	}

	// Then draw them from the back to the front.
	_Fill(target, remaining, fDesktop->CurrentBackgroundColor());

	for (size_t i = layerCount; i-- > 0;) {
		Layer* layer = fLayers[i];
		const WindowShape& shape = layer->window->Shape();

		if (layer->shadow.CountRects() > 0)
			_DrawShadow(target, layer->window, layer->shadow);

		BRegion opaque(layer->region);
		if (shape.EdgeRegion().CountRects() > 0) {
			opaque.Exclude(&shape.EdgeRegion());

			BRegion edge(layer->region);
			edge.IntersectWith(&shape.EdgeRegion());
			_BlendEdges(target, layer->window, edge);
		}
		_Copy(target, layer->window, opaque);
	}

	interface->InvalidateRegion(screenDamage);

	interface->UnlockParallelAccess();
}


void
Compositor::SceneChanged()
{
	std::vector<SceneEntry> scene;

	WindowList& windows = fDesktop->CurrentWindows();
	int32 workspace = fDesktop->CurrentWorkspace();
	for (Window* window = windows.LastWindow(); window != NULL;
			window = window->PreviousWindow(workspace)) {
		if (window->IsHidden())
			continue;

		SceneEntry entry = { window, window->Shape().Bounds() };
		scene.push_back(entry);
	}

	BRegion damage;

	// The screen mode has changed, nothing can be kept.
	RenderingBuffer* target = fDesktop->HWInterface()->DrawingBuffer();
	if (target->Bits() != fSceneBits || target->Width() != fSceneWidth
		|| target->Height() != fSceneHeight) {
		fSceneBits = target->Bits();
		fSceneWidth = target->Width();
		fSceneHeight = target->Height();
		damage.Include((BRect)target->Bounds());
	}

	// Find the windows that have appeared, have moved or changed size, or
	// have gone away. The index of each window in the previous scene is kept
	// to compare the order of the windows.
	std::vector<int32> previousIndex(scene.size(), -1);
	std::vector<bool> stillThere(fScene.size(), false);

	for (size_t i = 0; i < scene.size(); i++) {
		for (size_t k = 0; k < fScene.size(); k++) {
			if (fScene[k].window == scene[i].window) {
				previousIndex[i] = k;
				stillThere[k] = true;
				break;
			}
		}

		if (previousIndex[i] < 0) {
			damage.Include(scene[i].frame);
		} else if (fScene[previousIndex[i]].frame != scene[i].frame) {
			damage.Include(fScene[previousIndex[i]].frame);
			damage.Include(scene[i].frame);
		}
	}

	for (size_t k = 0; k < fScene.size(); k++) {
		if (!stillThere[k])
			damage.Include(fScene[k].frame);
	}

	// Two windows have changed places: only what they have in common can look
	// different.
	for (size_t i = 0; i < scene.size(); i++) {
		if (previousIndex[i] < 0)
			continue;

		for (size_t k = i + 1; k < scene.size(); k++) {
			if (previousIndex[k] >= 0 && previousIndex[i] > previousIndex[k]
				&& scene[i].frame.Intersects(scene[k].frame)) {
				damage.Include(scene[i].frame & scene[k].frame);
			}
		}
	}

	fScene.swap(scene);

	Composite(damage);
}


void
Compositor::_Fill(RenderingBuffer* target, const BRegion& region,
	const rgb_color& color)
{
	uint32 pixel = (uint32)color.blue | ((uint32)color.green << 8)
		| ((uint32)color.red << 16) | (255UL << 24);

	uint8* bits = (uint8*)target->Bits();
	uint32 bytesPerRow = target->BytesPerRow();

	int32 count = region.CountRects();
	for (int32 i = 0; i < count; i++) {
		clipping_rect rect = region.RectAtInt(i);
		for (int32 y = rect.top; y <= rect.bottom; y++) {
			uint32* dst = (uint32*)(bits + y * bytesPerRow) + rect.left;
			for (int32 x = rect.left; x <= rect.right; x++)
				*dst++ = pixel;
		}
	}
}


void
Compositor::_Copy(RenderingBuffer* target, Window* window,
	const BRegion& region)
{
	const WindowBuffer& buffer = window->BufferInterface()->Buffer();
	if (buffer.InitCheck() != B_OK)
		return;

	BRect bufferFrame = buffer.Frame();

	uint8* bits = (uint8*)target->Bits();
	uint32 bytesPerRow = target->BytesPerRow();

	int32 count = region.CountRects();
	for (int32 i = 0; i < count; i++) {
		clipping_rect rect = region.RectAtInt(i);
		rect.left = max_c(rect.left, (int32)bufferFrame.left);
		rect.top = max_c(rect.top, (int32)bufferFrame.top);
		rect.right = min_c(rect.right, (int32)bufferFrame.right);
		rect.bottom = min_c(rect.bottom, (int32)bufferFrame.bottom);
		if (rect.left > rect.right || rect.top > rect.bottom)
			continue;

		size_t bytes = (size_t)(rect.right - rect.left + 1) * 4;
		const uint8* src = buffer.PixelAt(rect.left, rect.top);
		uint8* dst = bits + rect.top * bytesPerRow + rect.left * 4;
		for (int32 y = rect.top; y <= rect.bottom; y++) {
			memcpy(dst, src, bytes);
			src += buffer.BytesPerRow();
			dst += bytesPerRow;
		}
	}
}


/*!	Mixes a window's corner pixels with what has been drawn behind them.
*/
void
Compositor::_BlendEdges(RenderingBuffer* target, Window* window,
	const BRegion& region)
{
	const WindowBuffer& buffer = window->BufferInterface()->Buffer();
	if (buffer.InitCheck() != B_OK)
		return;

	BRect bufferFrame = buffer.Frame();
	uint8* bits = (uint8*)target->Bits();
	uint32 bytesPerRow = target->BytesPerRow();

	const std::vector<WindowShape::Corner>& corners = window->Shape().Corners();
	for (size_t c = 0; c < corners.size(); c++) {
		const WindowShape::Corner& corner = corners[c];
		const uint8* coverage = WindowShape::CoverageFor(corner.radius);
		if (coverage == NULL)
			continue;

		BRegion clip(corner.frame & bufferFrame);
		clip.IntersectWith(&region);

		int32 count = clip.CountRects();
		for (int32 i = 0; i < count; i++) {
			clipping_rect rect = clip.RectAtInt(i);
			for (int32 y = rect.top; y <= rect.bottom; y++) {
				int32 row = y - (int32)corner.frame.top;
				if (corner.kind == WindowShape::LEFT_BOTTOM
					|| corner.kind == WindowShape::RIGHT_BOTTOM) {
					row = corner.radius - 1 - row;
				}

				const uint8* src = buffer.PixelAt(rect.left, y);
				uint8* dst = bits + y * bytesPerRow + rect.left * 4;
				for (int32 x = rect.left; x <= rect.right; x++) {
					int32 column = x - (int32)corner.frame.left;
					if (corner.kind == WindowShape::RIGHT_TOP
						|| corner.kind == WindowShape::RIGHT_BOTTOM) {
						column = corner.radius - 1 - column;
					}

					uint32 alpha = coverage[row * corner.radius + column];
					uint32 inverse = 255 - alpha;
					dst[0] = (src[0] * alpha + dst[0] * inverse + 127) / 255;
					dst[1] = (src[1] * alpha + dst[1] * inverse + 127) / 255;
					dst[2] = (src[2] * alpha + dst[2] * inverse + 127) / 255;
					src += 4;
					dst += 4;
				}
			}
		}
	}
}


/*!	Darkens what has been drawn behind a window. Each rectangle of the window's
	shape casts a shadow, and each pixel gets the darkest one of them.
*/
void
Compositor::_DrawShadow(RenderingBuffer* target, Window* window,
	const BRegion& region)
{
	const WindowShape& shape = window->Shape();
	const std::vector<BRect>& rects = shape.ShadowRects();
	const uint8* profile = WindowShape::ShadowProfile();
	const int32 margin = WindowShape::ShadowMargin();
	const uint32 strength = WindowShape::ShadowAlpha();

	uint8* bits = (uint8*)target->Bits();
	uint32 bytesPerRow = target->BytesPerRow();

	// The blurred edge at a distance from a rectangle's side
	#define EDGE(distance) ((distance) < -margin ? 0 : (distance) > margin \
		? 255 : profile[(distance) + margin])

	int32 count = region.CountRects();
	for (int32 i = 0; i < count; i++) {
		clipping_rect rect = region.RectAtInt(i);
		int32 width = rect.right - rect.left + 1;

		// Columns only need to be worked out once for the whole rectangle
		fShadowColumns.resize((size_t)width * rects.size());
		for (size_t k = 0; k < rects.size(); k++) {
			int32 left = (int32)rects[k].left;
			int32 right = (int32)rects[k].right;
			uint8* column = &fShadowColumns[k * width];
			for (int32 x = 0; x < width; x++) {
				int32 position = rect.left + x;
				column[x] = EDGE(position - left) * EDGE(right - position) / 255;
			}
		}

		for (int32 y = rect.top; y <= rect.bottom; y++) {
			uint8* dst = bits + y * bytesPerRow + rect.left * 4;
			for (int32 x = 0; x < width; x++) {
				uint32 alpha = 0;
				for (size_t k = 0; k < rects.size(); k++) {
					int32 top = (int32)rects[k].top;
					int32 bottom = (int32)rects[k].bottom;
					uint32 rowValue = EDGE(y - top) * EDGE(bottom - y) / 255;
					uint32 value = fShadowColumns[k * width + x] * rowValue;
					if (value > alpha)
						alpha = value;
				}

				uint32 keep = 255 - (alpha / 255) * strength / 255;
				dst[0] = dst[0] * keep / 255;
				dst[1] = dst[1] * keep / 255;
				dst[2] = dst[2] * keep / 255;
				dst += 4;
			}
		}
	}

	#undef EDGE
}
