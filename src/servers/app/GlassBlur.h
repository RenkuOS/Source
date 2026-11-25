/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef GLASS_BLUR_H
#define GLASS_BLUR_H


#include <Region.h>
#include <SupportDefs.h>

#include <vector>


/*!	The blur of what is behind glass, on B_RGBA32 pixels.

	What is behind is blurred at half the resolution of the screen, with a
	box blur done three times in each direction, which comes close to a
	Gaussian blur, and scaled back up smoothly. A pixel then takes in what is
	up to kReach pixels away. It comes out the same whatever else is blurred
	with it: only the pixels within its reach, and the edges of the screen,
	make a difference.
*/
class GlassBlur {
public:
	static	const int32			kReach = 20;

			// Blurs the \a count rects in the \a width by \a height pixels at
			// \a bits, in place. The pixels around them, within the reach,
			// are taken in as well, but left as they are.
			void				Blur(uint8* bits, uint32 bytesPerRow,
									int32 width, int32 height,
									const clipping_rect* rects, int32 count);

private:
			void				_BlurArea(uint8* bits, uint32 bytesPerRow,
									const clipping_rect& area,
									const clipping_rect* rects, int32 count);

private:
			std::vector<clipping_rect>
								fAreas;
			std::vector<int32>	fAreaOf;
			std::vector<clipping_rect>
								fRects;
			std::vector<uint32>	fPixels;
			std::vector<uint32>	fSpare;
			std::vector<uint32>	fSums;
};


#endif	// GLASS_BLUR_H
