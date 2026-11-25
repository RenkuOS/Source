/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


#include "GlassBlur.h"

#include <algorithm>

#include <ByteOrder.h>
#include <Debug.h>


// The pixel code reads B_RGBA32 as blue, green, red and alpha bytes in memory.
#if B_HOST_IS_BENDIAN
#	error "the glass blur expects B_RGBA32 pixels in little-endian order"
#endif


static const int32 kRadius = 3;
static const int32 kPasses = 3;



/*!	One pass of a box blur across a row of \a count pixels. The pixels
	beyond the ends are taken to be the same as the ones at the ends.
*/
static void
box_blur_row(const uint32* source, uint32* dest, int32 count)
{
	const int32 size = 2 * kRadius + 1;
	const uint32 scale = (65536 + size / 2) / size;

	uint32 sum0 = 0;
	uint32 sum1 = 0;
	uint32 sum2 = 0;
	for (int32 i = -kRadius; i <= kRadius; i++) {
		uint32 pixel = source[min_c(max_c(i, 0), count - 1)];
		sum0 += pixel & 0xff;
		sum1 += (pixel >> 8) & 0xff;
		sum2 += (pixel >> 16) & 0xff;
	}

	// Rounded to the nearest: cut off, each pass would make it darker.
	for (int32 i = 0; i < count; i++) {
		dest[i] = ((sum0 * scale + 0x8000) >> 16)
			| (((sum1 * scale + 0x8000) >> 16) << 8)
			| (((sum2 * scale + 0x8000) >> 16) << 16) | 0xff000000;

		uint32 in = source[min_c(i + kRadius + 1, count - 1)];
		uint32 out = source[max_c(i - kRadius, 0)];
		sum0 += (in & 0xff) - (out & 0xff);
		sum1 += ((in >> 8) & 0xff) - ((out >> 8) & 0xff);
		sum2 += ((in >> 16) & 0xff) - ((out >> 16) & 0xff);
	}
}


/*!	One pass of a box blur down the columns of \a width by \a height pixels.
	It goes a row at a time, with a sum for each column, which is much kinder
	to the cache than going down each column.
*/
static void
box_blur_columns(const uint32* source, uint32* dest, int32 width,
	int32 height, std::vector<uint32>& sumBuffer)
{
	const int32 size = 2 * kRadius + 1;
	const uint32 scale = (65536 + size / 2) / size;

	sumBuffer.assign((size_t)width * 3, 0);
	uint32* sums = &sumBuffer[0];
	for (int32 i = -kRadius; i <= kRadius; i++) {
		const uint32* row = source
			+ (size_t)min_c(max_c(i, 0), height - 1) * width;
		for (int32 x = 0; x < width; x++) {
			uint32 pixel = row[x];
			sums[3 * x] += pixel & 0xff;
			sums[3 * x + 1] += (pixel >> 8) & 0xff;
			sums[3 * x + 2] += (pixel >> 16) & 0xff;
		}
	}

	for (int32 y = 0; y < height; y++) {
		uint32* row = dest + (size_t)y * width;
		const uint32* in = source
			+ (size_t)min_c(y + kRadius + 1, height - 1) * width;
		const uint32* out = source + (size_t)max_c(y - kRadius, 0) * width;
		for (int32 x = 0; x < width; x++) {
			uint32* sum = sums + 3 * x;
			row[x] = ((sum[0] * scale + 0x8000) >> 16)
				| (((sum[1] * scale + 0x8000) >> 16) << 8)
				| (((sum[2] * scale + 0x8000) >> 16) << 16) | 0xff000000;

			uint32 add = in[x];
			uint32 remove = out[x];
			sum[0] += (add & 0xff) - (remove & 0xff);
			sum[1] += ((add >> 8) & 0xff) - ((remove >> 8) & 0xff);
			sum[2] += ((add >> 16) & 0xff) - ((remove >> 16) & 0xff);
		}
	}
}


//!	Mixes \a a and \a b, with \a weight of 4 parts for \a b, rounded.
static inline uint32
mix_pixels(uint32 a, uint32 b, uint32 weight)
{
	uint32 keep = 4 - weight;
	return ((((a & 0xff00ff) * keep + (b & 0xff00ff) * weight + 0x020002) >> 2)
			& 0xff00ff)
		| ((((a & 0x00ff00) * keep + (b & 0x00ff00) * weight + 0x000200) >> 2)
			& 0x00ff00)
		| 0xff000000;
}


static inline bool
rects_overlap(const clipping_rect& a, const clipping_rect& b)
{
	return a.left <= b.right && b.left <= a.right && a.top <= b.bottom
		&& b.top <= a.bottom;
}


void
GlassBlur::Blur(uint8* bits, uint32 bytesPerRow, int32 width, int32 height,
	const clipping_rect* rects, int32 count)
{
	// A pixel takes in what is up to twice the reach of the box passes away
	// at half resolution, and one more each for halving and for scaling back
	// up.
	STATIC_ASSERT(kReach == 2 * kRadius * kPasses + 2);

	// Rects whose reach overlaps are blurred together, so that nothing is
	// blurred twice, and the others each on their own, so that what is far
	// from all of them isn't blurred for nothing. In the end, no area reads
	// what another one writes.
	fAreas.clear();
	fAreaOf.resize(count);
	for (int32 i = 0; i < count; i++) {
		clipping_rect area = rects[i];
		area.left = max_c(area.left - kReach, 0);
		area.top = max_c(area.top - kReach, 0);
		area.right = min_c(area.right + kReach, width - 1);
		area.bottom = min_c(area.bottom + kReach, height - 1);
		if (area.left > area.right || area.top > area.bottom) {
			fAreaOf[i] = -1;
			continue;
		}

		// Swallow every area this one overlaps, as long as there are any.
		bool merged = true;
		while (merged) {
			merged = false;
			for (size_t k = 0; k < fAreas.size(); k++) {
				if (fAreas[k].left > fAreas[k].right
					|| !rects_overlap(fAreas[k], area)) {
					continue;
				}

				area.left = min_c(area.left, fAreas[k].left);
				area.top = min_c(area.top, fAreas[k].top);
				area.right = max_c(area.right, fAreas[k].right);
				area.bottom = max_c(area.bottom, fAreas[k].bottom);
				for (int32 j = 0; j < i; j++) {
					if (fAreaOf[j] == (int32)k)
						fAreaOf[j] = (int32)fAreas.size();
				}
				// It is left empty.
				fAreas[k].left = 0;
				fAreas[k].right = -1;
				merged = true;
			}
		}

		fAreaOf[i] = (int32)fAreas.size();
		fAreas.push_back(area);
	}

	for (size_t k = 0; k < fAreas.size(); k++) {
		const clipping_rect& area = fAreas[k];
		if (area.left > area.right)
			continue;

		fRects.clear();
		for (int32 i = 0; i < count; i++) {
			if (fAreaOf[i] != (int32)k)
				continue;

			clipping_rect rect = rects[i];
			rect.left = max_c(rect.left, area.left);
			rect.top = max_c(rect.top, area.top);
			rect.right = min_c(rect.right, area.right);
			rect.bottom = min_c(rect.bottom, area.bottom);
			fRects.push_back(rect);
		}
		_BlurArea(bits, bytesPerRow, area, &fRects[0], (int32)fRects.size());
	}
}


/*!	Blurs the \a count rects, which lie in \a area, from the pixels in it.
	\a area reaches as far as the blur around them, or to the screen's edge.
*/
void
GlassBlur::_BlurArea(uint8* bits, uint32 bytesPerRow, const clipping_rect& area,
	const clipping_rect* rects, int32 count)
{
	// Halving starts on an even pixel, so that each pixel of the screen keeps
	// its place in the half as big picture, whatever the area is.
	const int32 left = area.left & ~1;
	const int32 top = area.top & ~1;
	const int32 right = area.right;
	const int32 bottom = area.bottom;
	const int32 width = (right - left + 2) / 2;
	const int32 height = (bottom - top + 2) / 2;

	fPixels.resize((size_t)width * height);
	fSpare.resize((size_t)width * height);
	uint32* pixels = &fPixels[0];
	uint32* spare = &fSpare[0];

	// Each pixel of the small picture is the average of four of the screen.
	for (int32 y = 0; y < height; y++) {
		const int32 y0 = top + 2 * y;
		const int32 y1 = min_c(y0 + 1, bottom);
		const uint32* row0 = (const uint32*)(bits + y0 * bytesPerRow);
		const uint32* row1 = (const uint32*)(bits + y1 * bytesPerRow);
		uint32* small = pixels + (size_t)y * width;
		for (int32 x = 0; x < width; x++) {
			const int32 x0 = left + 2 * x;
			const int32 x1 = min_c(x0 + 1, right);
			uint32 a = row0[x0];
			uint32 b = row0[x1];
			uint32 c = row1[x0];
			uint32 d = row1[x1];
			uint32 redBlue = ((a & 0xff00ff) + (b & 0xff00ff) + (c & 0xff00ff)
				+ (d & 0xff00ff) + 0x020002) >> 2;
			uint32 green = ((a & 0x00ff00) + (b & 0x00ff00) + (c & 0x00ff00)
				+ (d & 0x00ff00) + 0x000200) >> 2;
			small[x] = (redBlue & 0xff00ff) | (green & 0x00ff00) | 0xff000000;
		}
	}

	// Across, then down; each pass from one buffer to the other.
	for (int32 pass = 0; pass < kPasses; pass++) {
		for (int32 y = 0; y < height; y++) {
			box_blur_row(pixels + (size_t)y * width,
				spare + (size_t)y * width, width);
		}
		std::swap(pixels, spare);
	}
	for (int32 pass = 0; pass < kPasses; pass++) {
		box_blur_columns(pixels, spare, width, height, fSums);
		std::swap(pixels, spare);
	}

	// Back to the screen, scaled up smoothly. A pixel of the screen lies a
	// quarter of the way between two of the small picture, in each direction.
	for (int32 i = 0; i < count; i++) {
		const clipping_rect& rect = rects[i];
		for (int32 y = rect.top; y <= rect.bottom; y++) {
			int32 smallY = y - top;
			int32 near = smallY >> 1;
			int32 far = (smallY & 1) != 0 ? near + 1 : near - 1;
			far = min_c(max_c(far, 0), height - 1);
			const uint32* nearRow = pixels + (size_t)near * width;
			const uint32* farRow = pixels + (size_t)far * width;

			uint32* dst = (uint32*)(bits + y * bytesPerRow) + rect.left;
			for (int32 x = rect.left; x <= rect.right; x++) {
				int32 smallX = x - left;
				int32 nearX = smallX >> 1;
				int32 farX = (smallX & 1) != 0 ? nearX + 1 : nearX - 1;
				farX = min_c(max_c(farX, 0), width - 1);

				uint32 nearMix = mix_pixels(nearRow[nearX], nearRow[farX], 1);
				uint32 farMix = mix_pixels(farRow[nearX], farRow[farX], 1);
				*dst++ = mix_pixels(nearMix, farMix, 1);
			}
		}
	}
}
