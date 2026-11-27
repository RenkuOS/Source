/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */


#include "EmulatedScreenLayout.h"

#include <TestSuiteAddon.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>


class EmulatedScreenLayoutTest : public CppUnit::TestFixture {
	CPPUNIT_TEST_SUITE(EmulatedScreenLayoutTest);
	CPPUNIT_TEST(Letterbox);
	CPPUNIT_TEST(FitsTheScreen);
	CPPUNIT_TEST(Columns);
	CPPUNIT_TEST(MousePosition);
	CPPUNIT_TEST_SUITE_END();

public:
	void Letterbox()
	{
		// black on the left and right
		emulated_screen_layout layout
			= layout_emulated_screen(1920, 1080, 640, 480);
		CPPUNIT_ASSERT(layout.left == 240 && layout.top == 0);
		CPPUNIT_ASSERT(layout.width == 1440 && layout.height == 1080);

		// above and below, on a portrait screen
		layout = layout_emulated_screen(1080, 1920, 640, 400);
		CPPUNIT_ASSERT(layout.left == 0 && layout.top == 622);
		CPPUNIT_ASSERT(layout.width == 1080 && layout.height == 675);

		// none
		layout = layout_emulated_screen(1024, 768, 640, 480);
		CPPUNIT_ASSERT(layout.left == 0 && layout.top == 0);
		CPPUNIT_ASSERT(layout.width == 1024 && layout.height == 768);
	}

	void FitsTheScreen()
	{
		const int32 kScreens[][2] = {
			{ 1920, 1080 }, { 1080, 1920 }, { 801, 599 }, { 1, 1 }, { 7, 3 }
		};
		const uint32 kDisplays[][2] = {
			{ 640, 480 }, { 320, 200 }, { 8192, 1 }, { 1, 8192 }, { 3, 7 }
		};

		for (int32 i = 0; i < 5; i++) {
			for (int32 j = 0; j < 5; j++) {
				int32 screenWidth = kScreens[i][0];
				int32 screenHeight = kScreens[i][1];
				emulated_screen_layout layout = layout_emulated_screen(
					screenWidth, screenHeight, kDisplays[j][0],
					kDisplays[j][1]);

				CPPUNIT_ASSERT(layout.width >= 1 && layout.height >= 1);
				CPPUNIT_ASSERT(layout.left >= 0 && layout.top >= 0);
				CPPUNIT_ASSERT(layout.left + layout.width <= screenWidth);
				CPPUNIT_ASSERT(layout.top + layout.height <= screenHeight);
				// one side always fills the screen
				CPPUNIT_ASSERT(layout.width == screenWidth
					|| layout.height == screenHeight);
			}
		}
	}

	/*!	Every pixel shown comes from the frame buffer, in order, and when
		it is scaled up, all of it is shown.
	*/
	void Columns()
	{
		const int32 kShown[] = { 1, 7, 640, 1440, 1441 };
		const uint32 kDisplay[] = { 1, 3, 320, 640 };

		for (int32 i = 0; i < 5; i++) {
			for (int32 j = 0; j < 4; j++) {
				int32 previous = -1;
				for (int32 x = 0; x < kShown[i]; x++) {
					int32 source = emulated_screen_source(x, kShown[i],
						kDisplay[j]);
					CPPUNIT_ASSERT(source >= 0
						&& source < (int32)kDisplay[j]);
					CPPUNIT_ASSERT(source >= previous);
					if (kShown[i] >= (int32)kDisplay[j])
						CPPUNIT_ASSERT(source <= previous + 1);
					previous = source;
				}
				CPPUNIT_ASSERT(emulated_screen_source(0, kShown[i],
					kDisplay[j]) == 0);
				if (kShown[i] >= (int32)kDisplay[j])
					CPPUNIT_ASSERT(previous == (int32)kDisplay[j] - 1);
			}
		}
	}

	/*!	The mouse is on the pixel of the frame buffer that is shown under it.
	*/
	void MousePosition()
	{
		emulated_screen_layout layout
			= layout_emulated_screen(1920, 1080, 640, 480);

		for (int32 x = 0; x < 1920; x++) {
			float position = emulated_screen_position(x + 0.5f, layout.left,
				layout.width, 640);
			CPPUNIT_ASSERT(position >= 0 && position <= 639);
			if (x >= layout.left && x < layout.left + layout.width) {
				CPPUNIT_ASSERT(position == emulated_screen_source(
					x - layout.left, layout.width, 640));
			}
		}

		// the edges go on outside of it
		CPPUNIT_ASSERT(emulated_screen_position(0, 240, 1440, 640) == 0);
		CPPUNIT_ASSERT(emulated_screen_position(239, 240, 1440, 640) == 0);
		CPPUNIT_ASSERT(emulated_screen_position(240, 240, 1440, 640) == 0);
		CPPUNIT_ASSERT(emulated_screen_position(9, 10, 3, 640) == 0);
		CPPUNIT_ASSERT(emulated_screen_position(1679, 240, 1440, 640) == 639);
		CPPUNIT_ASSERT(emulated_screen_position(1919, 240, 1440, 640) == 639);
		CPPUNIT_ASSERT(emulated_screen_position(960, 240, 1440, 640) == 320);
		CPPUNIT_ASSERT(emulated_screen_position(540, 0, 1080, 480) == 240);
	}
};


CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(EmulatedScreenLayoutTest,
	getTestSuiteName());
