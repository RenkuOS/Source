/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


#include "drawing/ScreenRotation.h"

#include <string.h>

#include <TestSuiteAddon.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>


static const int32 kRotations[] = { 90, 180, 270 };
static const int32 kRotationCount = 3;


class ScreenRotationTest : public CppUnit::TestFixture {
	CPPUNIT_TEST_SUITE(ScreenRotationTest);
	CPPUNIT_TEST(RotateModeTwice);
	CPPUNIT_TEST(RotateModeFourTimes);
	CPPUNIT_TEST(RotateModeSwaps);
	CPPUNIT_TEST(RotatePointIsBijective);
	CPPUNIT_TEST(UnrotatePoint);
	CPPUNIT_TEST(RotateRect);
	CPPUNIT_TEST_SUITE_END();

public:
	static display_mode Mode()
	{
		display_mode mode;
		memset(&mode, 0, sizeof(mode));
		mode.timing.pixel_clock = 148500;
		mode.timing.h_display = 1920;
		mode.timing.h_sync_start = 2008;
		mode.timing.h_sync_end = 2052;
		mode.timing.h_total = 2200;
		mode.timing.v_display = 1080;
		mode.timing.v_sync_start = 1084;
		mode.timing.v_sync_end = 1089;
		mode.timing.v_total = 1125;
		mode.timing.flags = B_POSITIVE_HSYNC | B_BLANK_PEDESTAL;
		mode.space = B_RGB32;
		mode.virtual_width = 1920;
		mode.virtual_height = 2160;
		mode.h_display_start = 3;
		mode.v_display_start = 7;
		mode.flags = B_SCROLL;
		return mode;
	}

	void RotateModeTwice()
	{
		const display_mode original = Mode();
		for (int32 i = 0; i < kRotationCount; i++) {
			display_mode mode = original;
			rotate_display_mode(mode, kRotations[i]);
			rotate_display_mode(mode, kRotations[i]);
			CPPUNIT_ASSERT(memcmp(&mode, &original, sizeof(mode)) == 0);
		}

		display_mode mode = original;
		rotate_display_mode(mode, 0);
		CPPUNIT_ASSERT(memcmp(&mode, &original, sizeof(mode)) == 0);
		rotate_display_mode(mode, 180);
		CPPUNIT_ASSERT(memcmp(&mode, &original, sizeof(mode)) == 0);
	}

	void RotateModeFourTimes()
	{
		const display_mode original = Mode();
		display_mode mode = original;
		for (int32 i = 0; i < 4; i++)
			rotate_display_mode(mode, 90);
		CPPUNIT_ASSERT(memcmp(&mode, &original, sizeof(mode)) == 0);
	}

	void RotateModeSwaps()
	{
		display_mode mode = Mode();
		rotate_display_mode(mode, 270);

		CPPUNIT_ASSERT(mode.virtual_width == 2160);
		CPPUNIT_ASSERT(mode.virtual_height == 1920);
		CPPUNIT_ASSERT(mode.h_display_start == 7);
		CPPUNIT_ASSERT(mode.v_display_start == 3);
		CPPUNIT_ASSERT(mode.timing.h_display == 1080);
		CPPUNIT_ASSERT(mode.timing.v_display == 1920);
		CPPUNIT_ASSERT(mode.timing.h_sync_start == 1084);
		CPPUNIT_ASSERT(mode.timing.v_sync_start == 2008);
		CPPUNIT_ASSERT(mode.timing.h_sync_end == 1089);
		CPPUNIT_ASSERT(mode.timing.v_sync_end == 2052);
		CPPUNIT_ASSERT(mode.timing.h_total == 1125);
		CPPUNIT_ASSERT(mode.timing.v_total == 2200);
		CPPUNIT_ASSERT(mode.timing.pixel_clock == 148500);

		// the sync polarities swap, other flags stay
		CPPUNIT_ASSERT(mode.timing.flags
			== (B_POSITIVE_VSYNC | B_BLANK_PEDESTAL));
		CPPUNIT_ASSERT(mode.space == B_RGB32);
		CPPUNIT_ASSERT(mode.flags == B_SCROLL);
	}

	/*!	Every pixel of an odd sized drawing buffer lands on its own pixel
		inside the front buffer.
	*/
	void RotatePointIsBijective()
	{
		const int32 kWidth = 5;
		const int32 kHeight = 3;

		for (int32 i = 0; i < kRotationCount; i++) {
			int32 degrees = kRotations[i];
			int32 frontWidth = degrees == 180 ? kWidth : kHeight;
			int32 frontHeight = degrees == 180 ? kHeight : kWidth;

			bool used[kWidth * kHeight];
			memset(used, 0, sizeof(used));

			for (int32 y = 0; y < kHeight; y++) {
				for (int32 x = 0; x < kWidth; x++) {
					int32 frontX = x;
					int32 frontY = y;
					rotate_point(degrees, frontWidth, frontHeight, frontX,
						frontY);

					CPPUNIT_ASSERT(frontX >= 0 && frontX < frontWidth);
					CPPUNIT_ASSERT(frontY >= 0 && frontY < frontHeight);
					CPPUNIT_ASSERT(!used[frontY * frontWidth + frontX]);
					used[frontY * frontWidth + frontX] = true;
				}
			}
		}
	}

	void UnrotatePoint()
	{
		const int32 kWidth = 5;
		const int32 kHeight = 3;

		for (int32 i = 0; i < kRotationCount; i++) {
			int32 degrees = kRotations[i];
			int32 frontWidth = degrees == 180 ? kWidth : kHeight;
			int32 frontHeight = degrees == 180 ? kHeight : kWidth;

			for (int32 y = 0; y < kHeight; y++) {
				for (int32 x = 0; x < kWidth; x++) {
					int32 frontX = x;
					int32 frontY = y;
					rotate_point(degrees, frontWidth, frontHeight, frontX,
						frontY);
					unrotate_point(degrees, frontWidth, frontHeight, frontX,
						frontY);
					CPPUNIT_ASSERT(frontX == x && frontY == y);
				}
			}
		}

		// clockwise: the top left corner goes to the top right for 90
		int32 x = 0;
		int32 y = 0;
		rotate_point(90, 3, 5, x, y);
		CPPUNIT_ASSERT(x == 2 && y == 0);
	}

	/*!	A rectangle lands on the rectangle that holds exactly its pixels. */
	void RotateRect()
	{
		const int32 kWidth = 7;
		const int32 kHeight = 5;

		for (int32 i = 0; i < kRotationCount; i++) {
			int32 degrees = kRotations[i];
			int32 frontWidth = degrees == 180 ? kWidth : kHeight;
			int32 frontHeight = degrees == 180 ? kHeight : kWidth;

			int32 left = 1;
			int32 top = 2;
			int32 right = 5;
			int32 bottom = 3;
			rotate_rect(degrees, frontWidth, frontHeight, left, top, right,
				bottom);

			CPPUNIT_ASSERT(left >= 0 && right < frontWidth);
			CPPUNIT_ASSERT(top >= 0 && bottom < frontHeight);
			CPPUNIT_ASSERT((right - left + 1) * (bottom - top + 1) == 5 * 2);

			for (int32 y = 2; y <= 3; y++) {
				for (int32 x = 1; x <= 5; x++) {
					int32 frontX = x;
					int32 frontY = y;
					rotate_point(degrees, frontWidth, frontHeight, frontX,
						frontY);
					CPPUNIT_ASSERT(frontX >= left && frontX <= right);
					CPPUNIT_ASSERT(frontY >= top && frontY <= bottom);
				}
			}
		}
	}
};


CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(ScreenRotationTest, getTestSuiteName());
