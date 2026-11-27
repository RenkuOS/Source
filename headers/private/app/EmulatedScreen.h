/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef _EMULATED_SCREEN_H
#define _EMULATED_SCREEN_H


#include <GraphicsDefs.h>


/*!	The start of the area that app_server shares with a BWindowScreen whose
	screen it emulates: on a rotated screen, or in a mode the screen doesn't
	have. The application draws to the frame buffer, which follows at
	\c offset, and app_server shows it on the screen, scaled to fit.
*/
struct emulated_screen {
	uint32			offset;
		// of the frame buffer from the start of the area
	uint32			width;
	uint32			height;
	uint32			bytes_per_row;
	uint32			space;
	uint32			display_width;
	uint32			display_height;

	// written by the application
	int32			display_x;
	int32			display_y;
	rgb_color		palette[256];
		// for B_CMAP8
};


#endif	// _EMULATED_SCREEN_H
