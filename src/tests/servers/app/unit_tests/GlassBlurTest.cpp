/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


#include "GlassBlur.h"

#include <vector>

#include <TestSuiteAddon.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>


static const int32 kWidth = 157;
static const int32 kHeight = 123;


class GlassBlurTest : public CppUnit::TestFixture {
	CPPUNIT_TEST_SUITE(GlassBlurTest);
	CPPUNIT_TEST(FlatColorStaysFlat);
	CPPUNIT_TEST(SameWhateverIsBlurredWithIt);
	CPPUNIT_TEST_SUITE_END();

public:
	void FlatColorStaysFlat()
	{
		const uint32 colors[] = { 0xffffffff, 0xff000000, 0xff808080,
			0xff7f7f7f, 0xff123456, 0xffff00ff };
		GlassBlur blur;
		for (size_t c = 0; c < sizeof(colors) / sizeof(colors[0]); c++) {
			std::vector<uint32> pixels(kWidth * kHeight, colors[c]);
			clipping_rect all = { 0, 0, kWidth - 1, kHeight - 1 };
			blur.Blur((uint8*)&pixels[0], kWidth * 4, kWidth, kHeight, &all,
				1);
			for (size_t i = 0; i < pixels.size(); i++)
				CPPUNIT_ASSERT_EQUAL(colors[c], pixels[i]);
		}
	}

	void SameWhateverIsBlurredWithIt()
	{
		std::vector<uint32> source(kWidth * kHeight);
		for (size_t i = 0; i < source.size(); i++)
			source[i] = _Random() | 0xff000000;

		GlassBlur blur;
		std::vector<uint32> whole(source);
		clipping_rect all = { 0, 0, kWidth - 1, kHeight - 1 };
		blur.Blur((uint8*)&whole[0], kWidth * 4, kWidth, kHeight, &all, 1);

		for (int32 trial = 0; trial < 500; trial++) {
			// Rects that don't overlap, as in a region, some close enough
			// to be blurred together
			clipping_rect rects[5];
			int32 count = 0;
			for (int32 i = 0; i < 5; i++) {
				clipping_rect rect;
				rect.left = _Random() % kWidth;
				rect.top = _Random() % kHeight;
				rect.right = min_c(rect.left + (int32)(_Random() % 40),
					kWidth - 1);
				rect.bottom = min_c(rect.top + (int32)(_Random() % 40),
					kHeight - 1);
				bool overlaps = false;
				for (int32 k = 0; k < count; k++)
					overlaps |= _Overlap(rect, rects[k]);
				if (!overlaps)
					rects[count++] = rect;
			}

			std::vector<uint32> part(source);
			blur.Blur((uint8*)&part[0], kWidth * 4, kWidth, kHeight, rects,
				count);

			for (int32 y = 0; y < kHeight; y++) {
				for (int32 x = 0; x < kWidth; x++) {
					bool inside = false;
					for (int32 k = 0; k < count; k++) {
						inside |= x >= rects[k].left && x <= rects[k].right
							&& y >= rects[k].top && y <= rects[k].bottom;
					}
					size_t index = y * kWidth + x;
					CPPUNIT_ASSERT_EQUAL(inside ? whole[index] : source[index],
						part[index]);
				}
			}
		}
	}

private:
	static bool _Overlap(const clipping_rect& a, const clipping_rect& b)
	{
		return a.left <= b.right && b.left <= a.right && a.top <= b.bottom
			&& b.top <= a.bottom;
	}

	uint32 _Random()
	{
		fSeed = fSeed * 1103515245 + 12345;
		return fSeed >> 8;
	}

	uint32 fSeed = 12345;
};


CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(GlassBlurTest, getTestSuiteName());
