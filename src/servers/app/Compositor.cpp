/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


#include "Compositor.h"

#include <math.h>
#include <new>
#include <string.h>

#include <Autolock.h>
#include <ByteOrder.h>

#include "Desktop.h"
#include "HWInterface.h"
#include "RenderingBuffer.h"
#include "ServerWindow.h"
#include "Window.h"
#include "WindowBuffer.h"
#include "WindowShape.h"
#include "WindowWobble.h"


// The pixel code reads B_RGBA32 as blue, green, red and alpha bytes in memory,
// as on every architecture RenkuOS builds for. A big-endian one would need it
// changed.
#if B_HOST_IS_BENDIAN
#	error "the compositor expects B_RGBA32 pixels in little-endian order"
#endif


// 60 Hz, for screens where the driver can't tell
static const bigtime_t kFrameInterval = 16667;

// Drawing is shared out between at most this many threads, in about twice as
// many bands, so that one that takes longer doesn't hold the others up. Waking
// them up costs more than drawing less than kMinBandedPixels.
static const int32 kMaxDrawThreads = 8;
static const int32 kBandsPerThread = 2;
static const int32 kMinBandHeight = 16;
static const int64 kMinBandedPixels = 256 * 256;

// How long to wait for an application to zoom its window
static const bigtime_t kZoomTimeout = 500000;

// A wobbling window is drawn as a mesh of cells about this many pixels wide.
static const int32 kWobbleCell = 24;
static const int32 kMaxWobbleCells = 48;


/*!	A window that wobbles. Its pixels are drawn along the springs as they are
	at rest, from the left top of its shape's bounds: so is the shadow.
*/
struct Compositor::Wobble {
	Window*				window;
	WindowWobble		model;
	bool				ready;
	bool				grabPending;
	bool				released;
	BPoint				grab;
	uint8				wobbliness;

	// Waiting for the application to zoom the window, which was here
	bool				zooming;
	BRect				zoomFrom;
	bigtime_t			zoomDeadline;

	// What was drawn last, and its frame. While the window is as good as at
	// rest, it is drawn as it is.
	BRegion				area;
	BRect				bounds;
	bool				flat;

	int32				width;
	int32				height;
	// How much of each pixel the window covers, and how much of what is
	// behind it its shadow takes away
	std::vector<uint8>	coverage;
	std::vector<uint8>	shade;
	// The pixels of each row from which on, and up to which, the window
	// covers everything, and isn't transparent
	std::vector<int32>	solidLeft;
	std::vector<int32>	solidRight;
	bool				isGlow;
	rgb_color			glowColor;

	// Where the buffer is, and its size
	int32				bufferLeft;
	int32				bufferTop;
	int32				bufferWidth;
	int32				bufferHeight;
	// The contents of a transparent window, or an invalid rect
	clipping_rect		transparent;
};


Compositor::Compositor(Desktop* desktop)
	:
	fDesktop(desktop),
	fLock("compositor"),
	fDamageLock("compositor damage"),
	fWakePending(false),
	fWakeSem(create_sem(0, "compositor wake")),
	fThread(-1),
	fQuit(false),
	fNextFrame(0),
	fUsesRetrace(false),
	fReportedPacing(false),
	fHelpSem(create_sem(0, "compositor help")),
	fHelpDoneSem(create_sem(0, "compositor help done")),
	fBandTarget(NULL),
	fBandBackground(NULL),
	fBandLayerCount(0),
	fBandCount(0),
	fNextBand(0),
	fWobbleLock("compositor wobble"),
	fSceneBits(NULL),
	fSceneWidth(0),
	fSceneHeight(0)
{
	// The helpers can't build this at the same time.
	WindowShape::ShadowProfile();

	system_info info;
	if (fHelpSem >= 0 && fHelpDoneSem >= 0 && get_system_info(&info) == B_OK) {
		int32 count = min_c((int32)info.cpu_count, kMaxDrawThreads) - 1;
		for (int32 i = 0; i < count; i++) {
			thread_id helper = spawn_thread(&_HelperEntry, "compositor helper",
				B_DISPLAY_PRIORITY, this);
			if (helper < 0)
				break;
			fHelpers.push_back(helper);
			resume_thread(helper);
		}
	}

	fThread = spawn_thread(&_ThreadEntry, "compositor", B_DISPLAY_PRIORITY, this);
	if (fThread >= 0)
		resume_thread(fThread);
}


Compositor::~Compositor()
{
	fQuit = true;
	delete_sem(fWakeSem);
	if (fThread >= 0) {
		status_t result;
		wait_for_thread(fThread, &result);
	}

	// Without their semaphore, the helpers quit.
	delete_sem(fHelpSem);
	delete_sem(fHelpDoneSem);
	for (size_t i = 0; i < fHelpers.size(); i++) {
		status_t result;
		wait_for_thread(fHelpers[i], &result);
	}

	for (size_t i = 0; i < fLayers.size(); i++)
		delete fLayers[i];
	for (size_t i = 0; i < fWobbles.size(); i++)
		delete fWobbles[i];
}


status_t
Compositor::InitCheck() const
{
	if (fWakeSem < 0)
		return fWakeSem;
	return fThread >= 0 ? B_OK : fThread;
}


void
Compositor::Composite(const BRegion& damage)
{
	if (damage.CountRects() == 0)
		return;

	BAutolock _(fDamageLock);
	fDamage.Include(&damage);
	if (!fWakePending) {
		fWakePending = true;
		release_sem(fWakeSem);
	}
}


void
Compositor::Flush()
{
	if (fDesktop->LockSingleWindow()) {
		_CompositeDamage();
		fDesktop->UnlockSingleWindow();
	}
}


void
Compositor::ScreenTaken()
{
	{
		// _Composite() looks at the owner of the screen with this held.
		BAutolock _(fLock);
	}

	// The application draws to the first page.
	fDesktop->HWInterface()->SetPageFlipping(false);
}


void
Compositor::ScreenReleased()
{
	fDesktop->HWInterface()->SetPageFlipping(true);

	BRegion all((BRect)fDesktop->HWInterface()->DrawingBuffer()->Bounds());
	Composite(all);
}


/*static*/ status_t
Compositor::_ThreadEntry(void* data)
{
	((Compositor*)data)->_Run();
	return B_OK;
}


void
Compositor::_Run()
{
	while (!fQuit) {
		status_t status = acquire_sem(fWakeSem);
		if (status == B_INTERRUPTED)
			continue;
		if (status != B_OK)
			break;

		if (!fUsesRetrace) {
			_WaitForFrame();

			bool wobbling = false;
			if (fDesktop->LockSingleWindow()) {
				wobbling = _StepWobbles();
				_CompositeDamage();
				fDesktop->UnlockSingleWindow();
			}

			// The springs move on in the next frame.
			if (wobbling)
				_ScheduleFrame();
			continue;
		}

		// With a retrace, the frame is built first and only copied to the
		// screen in the vertical blank. Building it takes longer than the
		// blank lasts, so building it after the retrace left the top of the
		// screen to tear; the copy alone runs ahead of the scanout.
		BRegion present;
		bool wobbling = false;
		if (fDesktop->LockSingleWindow()) {
			wobbling = _StepWobbles();
			_CompositeDamage(&present);
			fDesktop->UnlockSingleWindow();
		}
		_WaitForFrame();
		_Present(present);

		if (wobbling)
			_ScheduleFrame();
	}
}


/*!	Paces the updates of the screen. When the driver knows about the vertical
	retrace, the screen is updated right after it, which keeps a window from
	tearing while the update is done. Otherwise this just limits the rate, and
	what comes after an idle moment goes straight to the screen.
*/
void
Compositor::_WaitForFrame()
{
	status_t status = fDesktop->HWInterface()->WaitForRetrace(kFrameInterval);
	fUsesRetrace = status == B_OK || status == B_TIMED_OUT;

	if (!fReportedPacing) {
		debug_printf("app_server: compositor paced by %s\n",
			fUsesRetrace ? "the vertical retrace" : "a timer");
		fReportedPacing = true;
	}

	if (!fUsesRetrace) {
		if (system_time() < fNextFrame)
			snooze_until(fNextFrame, B_SYSTEM_TIMEBASE);
		fNextFrame = system_time() + kFrameInterval;
	}
}


void
Compositor::_CompositeDamage(BRegion* present)
{
	BRegion damage;
	{
		BAutolock _(fDamageLock);
		damage = fDamage;
		fDamage.MakeEmpty();
		fWakePending = false;
	}

	// A window with direct access draws to its buffer on its own, and doesn't
	// say when: while it may, its contents are put on the screen every frame.
	BRegion direct;
	WindowList& windows = fDesktop->CurrentWindows();
	int32 workspace = fDesktop->CurrentWorkspace();
	for (Window* window = windows.FirstWindow(); window != NULL;
			window = window->NextWindow(workspace)) {
		if (!window->IsHidden()
			&& window->ServerWindow()->IsDirectlyAccessing()) {
			BRegion content;
			window->GetContentRegion(&content);
			direct.Include(&content);
		}
	}
	damage.Include(&direct);

	_Composite(damage, present);

	// and again in the next one
	Composite(direct);
}


/*!	Copies \a region, as built by _Composite(), from the back buffer to the
	screen.
*/
void
Compositor::_Present(const BRegion& region)
{
	if (region.CountRects() == 0)
		return;

	BAutolock _(fLock);

	::HWInterface* interface = fDesktop->HWInterface();
	if (!interface->LockParallelAccess())
		return;

	if (fDesktop->IsDirectScreenLocked())
		interface->CancelPendingFrame();
	else
		interface->PresentRegion(region);

	interface->UnlockParallelAccess();
}


/*!	The window lock must be held at least for reading. With \a present, what
	changed on screen is added to it, to be copied there by _Present();
	otherwise it is copied at once.
*/
void
Compositor::_Composite(const BRegion& damage, BRegion* present)
{
	if (damage.CountRects() == 0)
		return;

	// Flush() can get here at the same time.
	BAutolock _(fLock);

	if (fDesktop->IsDirectScreenLocked())
		return;

	::HWInterface* interface = fDesktop->HWInterface();
	if (!interface->LockParallelAccess())
		return;

	RenderingBuffer* target = interface->DrawingBuffer();

	BRegion remaining(damage);
	BRegion bounds((BRect)target->Bounds());
	remaining.IntersectWith(&bounds);

	BRegion screenDamage(remaining);

	BAutolock wobbleLocker(fWobbleLock);

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
		Wobble* wobble = _WobbleFor(window);
		if (wobble != NULL && (!wobble->ready || wobble->flat))
			wobble = NULL;

		BRect bounds = shape.Bounds();
		if (wobble != NULL)
			bounds = bounds | wobble->bounds;
		if (!remaining.Intersects(bounds))
			continue;

		if (layerCount == fLayers.size()) {
			Layer* layer = new(std::nothrow) Layer;
			if (layer == NULL)
				break;
			fLayers.push_back(layer);
		}

		Layer* layer = fLayers[layerCount];
		layer->region = remaining;
		layer->shadow.MakeEmpty();

		if (wobble != NULL) {
			// It is drawn along the springs, with its shadow.
			layer->region.IntersectWith(&wobble->area);
		} else
			layer->region.IntersectWith(&window->CompositeRegion());

		if (wobble == NULL && shape.HasShadow()
			&& remaining.Intersects(shape.ShadowFrame())) {
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
		layer->alpha = window->CompositeAlpha();
		layer->wobble = wobble;
		layerCount++;

		// Only what hides everything behind it ends the search here.
		if (layer->alpha == 255 && wobble == NULL) {
			if (window->IsTransparent()) {
				BRegion opaque(shape.OpaqueRegion());
				opaque.Exclude(&window->TransparentRegion());
				remaining.Exclude(&opaque);
			} else
				remaining.Exclude(&shape.OpaqueRegion());
		}
	}

	// What each window that isn't wobbling is drawn with
	for (size_t i = 0; i < layerCount; i++) {
		Layer* layer = fLayers[i];
		layer->opaque = layer->region;
		layer->clear.MakeEmpty();
		layer->edge.MakeEmpty();
		if (layer->wobble != NULL)
			continue;

		Window* window = layer->window;
		const WindowShape& shape = window->Shape();
		if (window->IsTransparent()) {
			layer->clear = layer->region;
			layer->clear.IntersectWith(&window->TransparentRegion());
			layer->opaque.Exclude(&layer->clear);
		}
		if (shape.EdgeRegion().CountRects() > 0) {
			layer->opaque.Exclude(&shape.EdgeRegion());
			layer->edge = layer->region;
			layer->edge.IntersectWith(&shape.EdgeRegion());

			// The helpers can't build these at the same time.
			const std::vector<WindowShape::Corner>& corners = shape.Corners();
			for (size_t c = 0; c < corners.size(); c++)
				WindowShape::CoverageFor(corners[c].radius);
		}
	}

	// Then draw them from the back to the front.
	fBandTarget = target;
	fBandBackground = &remaining;
	fBandColor = fDesktop->CurrentBackgroundColor();
	fBandLayerCount = layerCount;

	// Under a wobbling or see-through window, a pixel is written more than
	// once on its way to the frame: the cursor, drawn from here, would show
	// what is in between, like the desktop or a shadow.
	bool cursorLocked = interface->LockFloatingOverlays();
	_DrawBands(screenDamage);

	if (present != NULL) {
		present->Include(&screenDamage);
		if (screenDamage.CountRects() > 0)
			interface->SetFramePending();
	} else
		interface->InvalidateRegion(screenDamage);
	if (cursorLocked)
		interface->UnlockFloatingOverlays();

	interface->UnlockParallelAccess();
}


/*static*/ status_t
Compositor::_HelperEntry(void* data)
{
	((Compositor*)data)->_Help();
	return B_OK;
}


void
Compositor::_Help()
{
	std::vector<uint8> shadowColumns;

	while (true) {
		status_t status = acquire_sem(fHelpSem);
		if (status == B_INTERRUPTED)
			continue;
		if (status != B_OK)
			break;

		_DrawNextBands(shadowColumns);
		release_sem(fHelpDoneSem);
	}
}


/*!	Draws the layers found by _Composite() where they are damaged: in bands,
	together with the helpers, where it is worth it.
*/
void
Compositor::_DrawBands(const BRegion& damage)
{
	BRect bounds = damage.Frame();
	int32 height = bounds.IntegerHeight() + 1;

	int64 pixels = 0;
	int32 count = damage.CountRects();
	for (int32 i = 0; i < count; i++) {
		clipping_rect rect = damage.RectAtInt(i);
		pixels += (int64)(rect.right - rect.left + 1)
			* (rect.bottom - rect.top + 1);
	}

	int32 bandCount = min_c((int32)(fHelpers.size() + 1) * kBandsPerThread,
		height / kMinBandHeight);
	if (bandCount < 2 || pixels < kMinBandedPixels) {
		_DrawBand(bounds, fShadowColumns);
		return;
	}

	fBandBounds = bounds;
	fBandCount = bandCount;
	fNextBand = 0;

	int32 helperCount = (int32)fHelpers.size();
	release_sem_etc(fHelpSem, helperCount, 0);
	_DrawNextBands(fShadowColumns);
	acquire_sem_etc(fHelpDoneSem, helperCount, 0, 0);
}


void
Compositor::_DrawNextBands(std::vector<uint8>& shadowColumns)
{
	const int32 top = (int32)fBandBounds.top;
	const int32 height = fBandBounds.IntegerHeight() + 1;

	while (true) {
		int32 band = atomic_add(&fNextBand, 1);
		if (band >= fBandCount)
			return;

		BRect rect(fBandBounds.left, top + height * band / fBandCount,
			fBandBounds.right, top + height * (band + 1) / fBandCount - 1);
		_DrawBand(rect, shadowColumns);
	}
}


/*!	Draws the layers from the back to the front, within \a band. Each thread
	only writes to the pixels of its own band.
*/
void
Compositor::_DrawBand(const BRect& band, std::vector<uint8>& shadowColumns)
{
	RenderingBuffer* target = fBandTarget;
	const BRegion clip(band);

	BRegion part(clip);
	part.IntersectWith(fBandBackground);
	_Fill(target, part, fBandColor);

	for (size_t i = fBandLayerCount; i-- > 0;) {
		const Layer* layer = fLayers[i];
		Window* window = layer->window;

		part = clip;
		if (layer->wobble != NULL) {
			part.IntersectWith(&layer->region);
			if (part.CountRects() > 0) {
				_DrawWobble(target, window, layer->wobble, part,
					layer->alpha);
			}
			continue;
		}

		part.IntersectWith(&layer->shadow);
		if (part.CountRects() > 0)
			_DrawShadow(target, window, part, shadowColumns);

		part = clip;
		part.IntersectWith(&layer->clear);
		if (part.CountRects() > 0)
			_BlendPixels(target, window, part, layer->alpha);

		part = clip;
		part.IntersectWith(&layer->edge);
		if (part.CountRects() > 0)
			_BlendEdges(target, window, part, layer->alpha);

		part = clip;
		part.IntersectWith(&layer->opaque);
		if (part.CountRects() == 0)
			continue;
		if (layer->alpha == 255)
			_Copy(target, window, part);
		else
			_Blend(target, window, part, layer->alpha);
	}
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


/*!	Mixes a window that can be seen through with what has been drawn behind
	it.
*/
void
Compositor::_Blend(RenderingBuffer* target, Window* window,
	const BRegion& region, uint8 alpha)
{
	const WindowBuffer& buffer = window->BufferInterface()->Buffer();
	if (buffer.InitCheck() != B_OK)
		return;

	BRect bufferFrame = buffer.Frame();
	uint8* bits = (uint8*)target->Bits();
	uint32 bytesPerRow = target->BytesPerRow();
	const uint32 inverse = 255 - alpha;

	int32 count = region.CountRects();
	for (int32 i = 0; i < count; i++) {
		clipping_rect rect = region.RectAtInt(i);
		rect.left = max_c(rect.left, (int32)bufferFrame.left);
		rect.top = max_c(rect.top, (int32)bufferFrame.top);
		rect.right = min_c(rect.right, (int32)bufferFrame.right);
		rect.bottom = min_c(rect.bottom, (int32)bufferFrame.bottom);

		for (int32 y = rect.top; y <= rect.bottom; y++) {
			const uint8* src = buffer.PixelAt(rect.left, y);
			uint8* dst = bits + y * bytesPerRow + rect.left * 4;
			for (int32 x = rect.left; x <= rect.right; x++) {
				dst[0] = (src[0] * alpha + dst[0] * inverse + 127) / 255;
				dst[1] = (src[1] * alpha + dst[1] * inverse + 127) / 255;
				dst[2] = (src[2] * alpha + dst[2] * inverse + 127) / 255;
				src += 4;
				dst += 4;
			}
		}
	}
}


/*!	Mixes a transparent window's contents with what has been drawn behind it,
	by the alpha of each pixel, and the window's own \a alpha on top.
*/
void
Compositor::_BlendPixels(RenderingBuffer* target, Window* window,
	const BRegion& region, uint8 alpha)
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

		for (int32 y = rect.top; y <= rect.bottom; y++) {
			const uint8* src = buffer.PixelAt(rect.left, y);
			uint8* dst = bits + y * bytesPerRow + rect.left * 4;
			for (int32 x = rect.left; x <= rect.right; x++) {
				uint32 cover = alpha == 255 ? src[3] : src[3] * alpha / 255;
				if (cover == 255) {
					dst[0] = src[0];
					dst[1] = src[1];
					dst[2] = src[2];
				} else if (cover != 0) {
					uint32 inverse = 255 - cover;
					dst[0] = (src[0] * cover + dst[0] * inverse + 127) / 255;
					dst[1] = (src[1] * cover + dst[1] * inverse + 127) / 255;
					dst[2] = (src[2] * cover + dst[2] * inverse + 127) / 255;
				}
				src += 4;
				dst += 4;
			}
		}
	}
}


/*!	Mixes a window's corner pixels with what has been drawn behind them. The
	window's own \a alpha is applied on top of the coverage of the corners.
*/
void
Compositor::_BlendEdges(RenderingBuffer* target, Window* window,
	const BRegion& region, uint8 alpha)
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

					uint32 cover = coverage[row * corner.radius + column]
						* alpha / 255;
					uint32 inverse = 255 - cover;
					dst[0] = (src[0] * cover + dst[0] * inverse + 127) / 255;
					dst[1] = (src[1] * cover + dst[1] * inverse + 127) / 255;
					dst[2] = (src[2] * cover + dst[2] * inverse + 127) / 255;
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
	const BRegion& region, std::vector<uint8>& columns)
{
	const WindowShape& shape = window->Shape();
	const std::vector<BRect>& rects = shape.ShadowRects();
	const uint8* profile = WindowShape::ShadowProfile();
	const int32 margin = WindowShape::ShadowMargin();
	const bool isGlow = shape.IsGlow();
	const rgb_color glow = shape.GlowColor();
	const uint32 strength = isGlow
		? WindowShape::GlowAlpha() : WindowShape::ShadowAlpha();

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
		columns.resize((size_t)width * rects.size());
		for (size_t k = 0; k < rects.size(); k++) {
			int32 left = (int32)rects[k].left;
			int32 right = (int32)rects[k].right;
			uint8* column = &columns[k * width];
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
					uint32 value = columns[k * width + x] * rowValue;
					if (value > alpha)
						alpha = value;
				}

				uint32 keep = 255 - (alpha / 255) * strength / 255;
				if (isGlow) {
					// Rounded, or the faint end of it comes out darker than
					// what is behind it.
					const uint32 give = 255 - keep;
					dst[0] = (dst[0] * keep + glow.blue * give + 127) / 255;
					dst[1] = (dst[1] * keep + glow.green * give + 127) / 255;
					dst[2] = (dst[2] * keep + glow.red * give + 127) / 255;
				} else {
					dst[0] = dst[0] * keep / 255;
					dst[1] = dst[1] * keep / 255;
					dst[2] = dst[2] * keep / 255;
				}
				dst += 4;
			}
		}
	}

	#undef EDGE
}


// #pragma mark - wobbling windows


void
Compositor::StartWobble(Window* window, BPoint grab, uint8 wobbliness)
{
	{
		BAutolock _(fWobbleLock);

		Wobble* wobble = _WobbleFor(window);
		if (wobble == NULL) {
			wobble = new(std::nothrow) Wobble;
			if (wobble == NULL)
				return;

			wobble->window = window;
			wobble->ready = false;
			wobble->zooming = false;
			fWobbles.push_back(wobble);
		}

		wobble->grab = grab;
		wobble->wobbliness = wobbliness;
		wobble->grabPending = true;
		wobble->released = false;
	}

	_ScheduleFrame();
}


void
Compositor::StartZoomWobble(Window* window, uint8 wobbliness)
{
	{
		BAutolock _(fWobbleLock);

		// One that still wobbles goes on into the new frame by itself.
		if (_WobbleFor(window) != NULL)
			return;

		Wobble* wobble = new(std::nothrow) Wobble;
		if (wobble == NULL)
			return;

		wobble->window = window;
		wobble->ready = false;
		wobble->grabPending = false;
		wobble->released = true;
		wobble->wobbliness = wobbliness;
		wobble->zooming = true;
		wobble->zoomFrom = window->Shape().Bounds();
		wobble->zoomDeadline = system_time() + kZoomTimeout;
		fWobbles.push_back(wobble);
	}

	_ScheduleFrame();
}


void
Compositor::ReleaseWobble(Window* window)
{
	BAutolock _(fWobbleLock);

	Wobble* wobble = _WobbleFor(window);
	if (wobble != NULL)
		wobble->released = true;
}


void
Compositor::ForgetWobble(Window* window)
{
	BAutolock _(fWobbleLock);

	for (size_t i = 0; i < fWobbles.size(); i++) {
		if (fWobbles[i]->window == window) {
			delete fWobbles[i];
			fWobbles.erase(fWobbles.begin() + i);
			return;
		}
	}
}


void
Compositor::_ScheduleFrame()
{
	BAutolock _(fDamageLock);
	if (!fWakePending) {
		fWakePending = true;
		release_sem(fWakeSem);
	}
}


/*!	Moves the springs of the wobbling windows on, and notes what has to be
	drawn again. Returns whether any window still wobbles. The window lock must
	be held at least for reading.
*/
bool
Compositor::_StepWobbles()
{
	BAutolock _(fWobbleLock);
	if (fWobbles.empty())
		return false;

	bigtime_t now = system_time();
	BRegion damage;

	WindowList& windows = fDesktop->CurrentWindows();
	int32 workspace = fDesktop->CurrentWorkspace();

	for (size_t i = 0; i < fWobbles.size();) {
		Wobble* wobble = fWobbles[i];

		Window* window = windows.FirstWindow();
		while (window != NULL && window != wobble->window)
			window = window->NextWindow(workspace);

		bool alive = window != NULL && !window->IsHidden()
			&& window->IsBufferReady();
		if (alive && !wobble->ready && wobble->zooming
			&& window->Shape().Bounds() == wobble->zoomFrom) {
			// The application hasn't zoomed the window yet.
			if (now < wobble->zoomDeadline) {
				i++;
				continue;
			}
			alive = false;
		}

		if (alive) {
			if (!wobble->ready)
				_InitWobble(wobble);
			else {
				if (wobble->grabPending) {
					wobble->model.Grab(window->Frame().LeftTop() + wobble->grab,
						wobble->wobbliness);
				}

				const BRect frame = window->Shape().Bounds();
				if (!wobble->model.SetFrame(frame)) {
					_InitWobbleMasks(wobble);
					wobble->model.Resize(frame);
				} else if (_WobbleBufferMoved(wobble, window)) {
					// A window with direct access only has the part of it
					// on the screen in its buffer, which changes as it moves
					// over the screen's edge.
					_InitWobbleMasks(wobble);
				}
			}
			wobble->grabPending = false;
		}

		if (alive && wobble->released) {
			wobble->model.Release();
			wobble->released = false;
		}

		if (alive)
			alive = wobble->model.Step(now);

		if (!alive) {
			// From now on, the window is drawn as it is.
			if (wobble->ready) {
				damage.Include(&wobble->area);
				damage.Include(wobble->model.Frame());
			}
			delete wobble;
			fWobbles.erase(fWobbles.begin() + i);
			continue;
		}

		// Nothing changes on the screen while it stays as good as at rest.
		bool wasFlat = wobble->flat;
		wobble->flat = wobble->model.IsFlat();
		if (!wasFlat || !wobble->flat) {
			damage.Include(&wobble->area);
			_UpdateWobbleArea(wobble);
			damage.Include(&wobble->area);
		}
		i++;
	}

	Composite(damage);
	return !fWobbles.empty();
}


Compositor::Wobble*
Compositor::_WobbleFor(const Window* window) const
{
	for (size_t i = 0; i < fWobbles.size(); i++) {
		if (fWobbles[i]->window == window)
			return fWobbles[i];
	}
	return NULL;
}


/*!	Starts the springs of a window that has just been grabbed or zoomed.
*/
void
Compositor::_InitWobble(Wobble* wobble)
{
	Window* window = wobble->window;
	const BRect frame = window->Shape().Bounds();
	_InitWobbleMasks(wobble);

	if (wobble->zooming) {
		// It starts out where it was before, and is pulled into its new
		// frame.
		wobble->model.SetTo(wobble->zoomFrom, wobble->zoomFrom.LeftTop(),
			wobble->wobbliness, system_time());
		wobble->model.Release();
		wobble->model.Zoomed();
		wobble->model.Resize(frame);
	} else {
		wobble->model.SetTo(frame, window->Frame().LeftTop() + wobble->grab,
			wobble->wobbliness, system_time());
	}
	wobble->flat = false;
	_UpdateWobbleArea(wobble);
	wobble->ready = true;
}


/*!	The corners of the cells a wobbling window is drawn with, on the screen.
*/
static void
wobble_mesh(const WindowWobble& model, int32 width, int32 height,
	BPoint* points, int32& columns, int32& rows)
{
	columns = max_c(4, min_c(kMaxWobbleCells, width / kWobbleCell));
	rows = max_c(4, min_c(kMaxWobbleCells, height / kWobbleCell));

	for (int32 row = 0; row <= rows; row++) {
		for (int32 column = 0; column <= columns; column++) {
			points[row * (columns + 1) + column] = model.PointAt(
				(float)column / columns, (float)row / rows);
		}
	}
}


/*!	Finds out which part of the screen a wobbling window covers: the frames
	of blocks of its cells, so that a window that is bent doesn't take the
	whole rectangle round it.
*/
void
Compositor::_UpdateWobbleArea(Wobble* wobble)
{
	wobble->area.MakeEmpty();

	if (wobble->flat) {
		wobble->bounds = wobble->model.Frame();
		wobble->area.Include(wobble->bounds);
		return;
	}

	BPoint points[(kMaxWobbleCells + 1) * (kMaxWobbleCells + 1)];
	int32 columns, rows;
	wobble_mesh(wobble->model, wobble->width, wobble->height, points, columns,
		rows);

	const int32 blockColumns = (columns + 7) / 8;
	const int32 blockRows = (rows + 7) / 8;
	for (int32 top = 0; top < rows; top += blockRows) {
		int32 bottom = min_c(top + blockRows, rows);
		for (int32 left = 0; left < columns; left += blockColumns) {
			int32 right = min_c(left + blockColumns, columns);

			BPoint first = points[top * (columns + 1) + left];
			BRect block(first, first);
			for (int32 row = top; row <= bottom; row++) {
				for (int32 column = left; column <= right; column++) {
					const BPoint& point = points[row * (columns + 1) + column];
					block.left = min_c(block.left, point.x);
					block.top = min_c(block.top, point.y);
					block.right = max_c(block.right, point.x);
					block.bottom = max_c(block.bottom, point.y);
				}
			}
			wobble->area.Include(BRect(floorf(block.left), floorf(block.top),
				ceilf(block.right), ceilf(block.bottom)));
		}
	}

	wobble->bounds = wobble->area.Frame();
}


/*!	Notes how much of each pixel of the window, as it is now, its outline
	and its shadow cover.
*/
void
Compositor::_InitWobbleMasks(Wobble* wobble)
{
	Window* window = wobble->window;
	const WindowShape& shape = window->Shape();
	const BRect frame = shape.Bounds();
	const int32 left = (int32)frame.left;
	const int32 top = (int32)frame.top;
	const int32 width = frame.IntegerWidth() + 1;
	const int32 height = frame.IntegerHeight() + 1;

	wobble->width = width;
	wobble->height = height;
	wobble->coverage.assign((size_t)width * height, 0);
	wobble->shade.assign((size_t)width * height, 0);

	const WindowBuffer& buffer = window->BufferInterface()->Buffer();
	BRect bufferFrame = buffer.Frame();
	wobble->bufferLeft = (int32)bufferFrame.left - left;
	wobble->bufferTop = (int32)bufferFrame.top - top;
	wobble->bufferWidth = bufferFrame.IntegerWidth() + 1;
	wobble->bufferHeight = bufferFrame.IntegerHeight() + 1;

	// Only what is in the buffer can be drawn.
	BRegion opaque(shape.OpaqueRegion());
	BRegion inBuffer(bufferFrame);
	opaque.IntersectWith(&inBuffer);
	int32 count = opaque.CountRects();
	for (int32 i = 0; i < count; i++) {
		clipping_rect rect = opaque.RectAtInt(i);
		for (int32 y = rect.top; y <= rect.bottom; y++) {
			memset(&wobble->coverage[(size_t)(y - top) * width + rect.left
				- left], 255, rect.right - rect.left + 1);
		}
	}

	const std::vector<WindowShape::Corner>& corners = shape.Corners();
	for (size_t c = 0; c < corners.size(); c++) {
		const WindowShape::Corner& corner = corners[c];
		const uint8* coverage = WindowShape::CoverageFor(corner.radius);
		if (coverage == NULL)
			continue;

		BRegion edge(corner.frame & bufferFrame);
		edge.IntersectWith(&shape.EdgeRegion());
		count = edge.CountRects();
		for (int32 i = 0; i < count; i++) {
			clipping_rect rect = edge.RectAtInt(i);
			for (int32 y = rect.top; y <= rect.bottom; y++) {
				int32 row = y - (int32)corner.frame.top;
				if (corner.kind == WindowShape::LEFT_BOTTOM
					|| corner.kind == WindowShape::RIGHT_BOTTOM) {
					row = corner.radius - 1 - row;
				}
				for (int32 x = rect.left; x <= rect.right; x++) {
					int32 column = x - (int32)corner.frame.left;
					if (corner.kind == WindowShape::RIGHT_TOP
						|| corner.kind == WindowShape::RIGHT_BOTTOM) {
						column = corner.radius - 1 - column;
					}
					wobble->coverage[(size_t)(y - top) * width + x - left]
						= coverage[row * corner.radius + column];
				}
			}
		}
	}

	// The shadow, worked out as in _DrawShadow(), where the window doesn't
	// hide it
	wobble->isGlow = shape.IsGlow();
	wobble->glowColor = shape.GlowColor();
	if (shape.HasShadow()) {
		const std::vector<BRect>& rects = shape.ShadowRects();
		const uint8* profile = WindowShape::ShadowProfile();
		const int32 margin = WindowShape::ShadowMargin();
		const uint32 strength = shape.IsGlow()
			? WindowShape::GlowAlpha() : WindowShape::ShadowAlpha();

		#define EDGE(distance) ((distance) < -margin ? 0 \
			: (distance) > margin ? 255 : profile[(distance) + margin])

		BRect shadowFrame = shape.ShadowFrame() & frame;
		for (int32 y = (int32)shadowFrame.top; y <= (int32)shadowFrame.bottom;
				y++) {
			for (int32 x = (int32)shadowFrame.left;
					x <= (int32)shadowFrame.right; x++) {
				size_t index = (size_t)(y - top) * width + x - left;
				if (wobble->coverage[index] == 255)
					continue;

				uint32 alpha = 0;
				for (size_t k = 0; k < rects.size(); k++) {
					uint32 value = EDGE(x - (int32)rects[k].left)
						* EDGE((int32)rects[k].right - x) / 255
						* (EDGE(y - (int32)rects[k].top)
							* EDGE((int32)rects[k].bottom - y) / 255);
					if (value > alpha)
						alpha = value;
				}
				wobble->shade[index] = (alpha / 255) * strength / 255;
			}
		}

		#undef EDGE
	}

	wobble->transparent.left = 0;
	wobble->transparent.top = 0;
	wobble->transparent.right = -1;
	wobble->transparent.bottom = -1;
	if (window->IsTransparent()) {
		BRect contents = window->TransparentRegion().Frame();
		if (contents.IsValid()) {
			wobble->transparent.left = (int32)contents.left - left;
			wobble->transparent.top = (int32)contents.top - top;
			wobble->transparent.right = (int32)contents.right - left;
			wobble->transparent.bottom = (int32)contents.bottom - top;
		}
	}

	// Inside these, a pixel is mixed from the window alone.
	wobble->solidLeft.assign(height, 1);
	wobble->solidRight.assign(height, 0);
	for (int32 y = 0; y < height; y++) {
		const uint8* row = &wobble->coverage[(size_t)y * width];
		bool inTransparent = y >= wobble->transparent.top
			&& y <= wobble->transparent.bottom;
		int32 x = 0;
		while (x < width && row[x] != 255)
			x++;
		int32 left = x;
		while (x < width && row[x] == 255
			&& (!inTransparent || x < wobble->transparent.left
				|| x > wobble->transparent.right)) {
			x++;
		}
		if (x > left) {
			wobble->solidLeft[y] = left;
			wobble->solidRight[y] = x - 1;
		}
	}
}


/*!	Whether the part of the window that its buffer has is no longer the one
	the wobble's masks were made for.
*/
bool
Compositor::_WobbleBufferMoved(const Wobble* wobble, Window* window) const
{
	const BRect frame = window->Shape().Bounds();
	const BRect bufferFrame = window->BufferInterface()->Buffer().Frame();
	return (int32)bufferFrame.left - (int32)frame.left != wobble->bufferLeft
		|| (int32)bufferFrame.top - (int32)frame.top != wobble->bufferTop
		|| bufferFrame.IntegerWidth() + 1 != wobble->bufferWidth
		|| bufferFrame.IntegerHeight() + 1 != wobble->bufferHeight;
}


/*!	Draws a wobbling window along its springs, as a mesh of triangles that
	each show a part of the window.
*/
void
Compositor::_DrawWobble(RenderingBuffer* target, Window* window,
	const Wobble* wobble, const BRegion& region, uint8 alpha)
{
	const WindowBuffer& buffer = window->BufferInterface()->Buffer();
	if (buffer.InitCheck() != B_OK)
		return;

	// The window has changed size, and the springs stop in the next frame.
	BRect bufferFrame = buffer.Frame();
	if (bufferFrame.IntegerWidth() + 1 != wobble->bufferWidth
		|| bufferFrame.IntegerHeight() + 1 != wobble->bufferHeight) {
		return;
	}

	const uint8* pixels = buffer.PixelAt((int32)bufferFrame.left,
		(int32)bufferFrame.top);
	const uint32 bytesPerRow = buffer.BytesPerRow();

	BPoint points[(kMaxWobbleCells + 1) * (kMaxWobbleCells + 1)];
	int32 columns, rows;
	wobble_mesh(wobble->model, wobble->width, wobble->height, points, columns,
		rows);

	int32 count = region.CountRects();
	for (int32 i = 0; i < count; i++) {
		clipping_rect clip = region.RectAtInt(i);

		for (int32 row = 0; row < rows; row++) {
			for (int32 column = 0; column < columns; column++) {
				const BPoint* corner = &points[row * (columns + 1) + column];
				const BPoint& leftTop = corner[0];
				const BPoint& rightTop = corner[1];
				const BPoint& leftBottom = corner[columns + 1];
				const BPoint& rightBottom = corner[columns + 2];

				float left = min_c(min_c(leftTop.x, rightTop.x),
					min_c(leftBottom.x, rightBottom.x));
				float right = max_c(max_c(leftTop.x, rightTop.x),
					max_c(leftBottom.x, rightBottom.x));
				float top = min_c(min_c(leftTop.y, rightTop.y),
					min_c(leftBottom.y, rightBottom.y));
				float bottom = max_c(max_c(leftTop.y, rightTop.y),
					max_c(leftBottom.y, rightBottom.y));
				if (right < clip.left || left > clip.right + 1
					|| bottom < clip.top || top > clip.bottom + 1) {
					continue;
				}

				float sourceLeft = (float)wobble->width * column / columns;
				float sourceRight = (float)wobble->width * (column + 1)
					/ columns;
				float sourceTop = (float)wobble->height * row / rows;
				float sourceBottom = (float)wobble->height * (row + 1) / rows;

				BPoint triangle[3] = { leftTop, rightTop, rightBottom };
				BPoint sources[3] = { BPoint(sourceLeft, sourceTop),
					BPoint(sourceRight, sourceTop),
					BPoint(sourceRight, sourceBottom) };
				_FillTriangle(target, wobble, pixels, bytesPerRow, clip,
					triangle, sources, alpha);

				triangle[1] = rightBottom;
				triangle[2] = leftBottom;
				sources[1] = BPoint(sourceRight, sourceBottom);
				sources[2] = BPoint(sourceLeft, sourceBottom);
				_FillTriangle(target, wobble, pixels, bytesPerRow, clip,
					triangle, sources, alpha);
			}
		}
	}
}


/*!	Where an edge from \a a to \a b, with \a a above it, crosses the row at
	\a y. Neighbouring triangles get the same result for the edge they share,
	so that no pixel is drawn twice, or left out.
*/
static inline float
edge_x(const BPoint& a, const BPoint& b, float y)
{
	if (b.y == a.y)
		return a.x;
	return a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y);
}


/*!	Draws one triangle of a wobbling window: the pixels whose centres are in
	it, each mixed from the four pixels of the window closest to where it
	comes from, with their coverage, and the shadow behind them.
*/
void
Compositor::_FillTriangle(RenderingBuffer* target, const Wobble* wobble,
	const uint8* pixels, uint32 bytesPerRow, const clipping_rect& clip,
	const BPoint* points, const BPoint* sources, uint8 alpha)
{
	const float x0 = points[0].x;
	const float y0 = points[0].y;
	const float determinant = (points[1].x - x0) * (points[2].y - y0)
		- (points[2].x - x0) * (points[1].y - y0);
	if (fabsf(determinant) < 0.001f)
		return;

	// How the source changes along the screen
	const float u1 = sources[1].x - sources[0].x;
	const float u2 = sources[2].x - sources[0].x;
	const float v1 = sources[1].y - sources[0].y;
	const float v2 = sources[2].y - sources[0].y;
	const float uPerX = (u1 * (points[2].y - y0) - u2 * (points[1].y - y0))
		/ determinant;
	const float uPerY = (u2 * (points[1].x - x0) - u1 * (points[2].x - x0))
		/ determinant;
	const float vPerX = (v1 * (points[2].y - y0) - v2 * (points[1].y - y0))
		/ determinant;
	const float vPerY = (v2 * (points[1].x - x0) - v1 * (points[2].x - x0))
		/ determinant;

	// From the top to the bottom
	int32 a = 0, b = 1, c = 2;
	if (points[b].y < points[a].y) {
		int32 swap = a; a = b; b = swap;
	}
	if (points[c].y < points[b].y) {
		int32 swap = b; b = c; c = swap;
	}
	if (points[b].y < points[a].y) {
		int32 swap = a; a = b; b = swap;
	}
	const BPoint& top = points[a];
	const BPoint& middle = points[b];
	const BPoint& bottom = points[c];

	int32 firstRow = max_c((int32)ceilf(top.y - 0.5f), clip.top);
	int32 lastRow = min_c((int32)ceilf(bottom.y - 0.5f) - 1, clip.bottom);

	uint8* bits = (uint8*)target->Bits();
	const uint32 targetBytesPerRow = target->BytesPerRow();
	const int32 width = wobble->width;
	const int32 height = wobble->height;
	const uint8* coverage = &wobble->coverage[0];
	const uint8* shade = &wobble->shade[0];
	const clipping_rect& transparent = wobble->transparent;
	const rgb_color glow = wobble->glowColor;
	const int32* solidLeft = &wobble->solidLeft[0];
	const int32* solidRight = &wobble->solidRight[0];

	for (int32 y = firstRow; y <= lastRow; y++) {
		float centerY = y + 0.5f;
		float longX = edge_x(top, bottom, centerY);
		float shortX = centerY < middle.y
			? edge_x(top, middle, centerY) : edge_x(middle, bottom, centerY);
		float leftX = min_c(longX, shortX);
		float rightX = max_c(longX, shortX);

		int32 first = max_c((int32)ceilf(leftX - 0.5f), clip.left);
		int32 last = min_c((int32)ceilf(rightX - 0.5f) - 1, clip.right);
		if (first > last)
			continue;

		// The source of the first pixel, less half a pixel to find the four
		// around it, in 1/65536 of a pixel
		float u = sources[0].x + (first + 0.5f - x0) * uPerX
			+ (centerY - y0) * uPerY - 0.5f;
		float v = sources[0].y + (first + 0.5f - x0) * vPerX
			+ (centerY - y0) * vPerY - 0.5f;
		int32 fixedU = (int32)(u * 65536);
		int32 fixedV = (int32)(v * 65536);
		const int32 stepU = (int32)(uPerX * 65536);
		const int32 stepV = (int32)(vPerX * 65536);

		uint8* dst = bits + y * targetBytesPerRow + first * 4;
		for (int32 x = first; x <= last; x++, dst += 4, fixedU += stepU,
				fixedV += stepV) {
			// The pixel closest to the source carries the shadow.
			int32 nearestX = (fixedU + 32768) >> 16;
			int32 nearestY = (fixedV + 32768) >> 16;
			if (nearestX < -1 || nearestX > width || nearestY < -1
				|| nearestY > height) {
				continue;
			}

			uint32 give = 0;
			if (nearestX >= 0 && nearestX < width && nearestY >= 0
				&& nearestY < height) {
				give = shade[nearestY * width + nearestX];
			}

			const int32 sourceX = fixedU >> 16;
			const int32 sourceY = fixedV >> 16;
			const uint32 fractionX = (fixedU >> 10) & 63;
			const uint32 fractionY = (fixedV >> 10) & 63;
			const uint32 weights[4] = {
				(64 - fractionX) * (64 - fractionY),
				fractionX * (64 - fractionY),
				(64 - fractionX) * fractionY,
				fractionX * fractionY
			};

			// Most of the window: four pixels it covers fully, and no shadow
			if (sourceY >= 0 && sourceY + 1 < height
				&& sourceX >= solidLeft[sourceY]
				&& sourceX >= solidLeft[sourceY + 1]
				&& sourceX + 1 <= solidRight[sourceY]
				&& sourceX + 1 <= solidRight[sourceY + 1]) {
				const uint8* src = pixels
					+ (intptr_t)(sourceY - wobble->bufferTop) * bytesPerRow
					+ (intptr_t)(sourceX - wobble->bufferLeft) * 4;
				const uint8* below = src + bytesPerRow;
				uint32 values[3];
				for (int32 c = 0; c < 3; c++) {
					values[c] = (src[c] * weights[0] + src[c + 4] * weights[1]
						+ below[c] * weights[2] + below[c + 4] * weights[3]
						+ 2048) >> 12;
				}
				if (alpha == 255) {
					dst[0] = values[0];
					dst[1] = values[1];
					dst[2] = values[2];
				} else {
					const uint32 inverse = 255 - alpha;
					for (int32 c = 0; c < 3; c++) {
						dst[c] = (values[c] * alpha + dst[c] * inverse + 127)
							/ 255;
					}
				}
				continue;
			}

			uint32 total = 0;
			uint32 blue = 0, green = 0, red = 0;
			for (int32 k = 0; k < 4; k++) {
				int32 tapX = sourceX + (k & 1);
				int32 tapY = sourceY + (k >> 1);
				if (weights[k] == 0 || tapX < 0 || tapX >= width || tapY < 0
					|| tapY >= height) {
					continue;
				}

				uint32 cover = coverage[tapY * width + tapX];
				if (cover == 0)
					continue;

				const uint8* src = pixels
					+ (intptr_t)(tapY - wobble->bufferTop) * bytesPerRow
					+ (intptr_t)(tapX - wobble->bufferLeft) * 4;
				if (tapX >= transparent.left && tapX <= transparent.right
					&& tapY >= transparent.top && tapY <= transparent.bottom) {
					cover = cover * src[3] / 255;
					if (cover == 0)
						continue;
				}

				uint32 weight = weights[k] * cover;
				total += weight;
				blue += weight * src[0];
				green += weight * src[1];
				red += weight * src[2];
			}

			if (give != 0) {
				const uint32 keep = 255 - give;
				if (wobble->isGlow) {
					dst[0] = (dst[0] * keep + glow.blue * give + 127) / 255;
					dst[1] = (dst[1] * keep + glow.green * give + 127) / 255;
					dst[2] = (dst[2] * keep + glow.red * give + 127) / 255;
				} else {
					dst[0] = dst[0] * keep / 255;
					dst[1] = dst[1] * keep / 255;
					dst[2] = dst[2] * keep / 255;
				}
			}

			if (total == 0)
				continue;

			uint32 cover = (total + 2048) >> 12;
			if (alpha != 255)
				cover = cover * alpha / 255;
			const uint32 half = total / 2;
			const uint32 srcBlue = (blue + half) / total;
			const uint32 srcGreen = (green + half) / total;
			const uint32 srcRed = (red + half) / total;
			if (cover >= 255) {
				dst[0] = srcBlue;
				dst[1] = srcGreen;
				dst[2] = srcRed;
			} else if (cover != 0) {
				const uint32 inverse = 255 - cover;
				dst[0] = (srcBlue * cover + dst[0] * inverse + 127) / 255;
				dst[1] = (srcGreen * cover + dst[1] * inverse + 127) / 255;
				dst[2] = (srcRed * cover + dst[2] * inverse + 127) / 255;
			}
		}
	}
}
