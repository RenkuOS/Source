/*
 * Copyright 2004-2009, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2008, Stephan Aßmus <superstippi@gmx.de>
 * Copyright 2008, Philippe Saint-Pierre <stpere@gmail.com>
 * Distributed under the terms of the MIT License.
 */


#include <arch/cpu.h>
#include <boot/stage2.h>
#include <boot/platform.h>
#include <boot/menu.h>
#include <boot/kernel_args.h>
#include <boot/platform/generic/video.h>
#include <boot/platform/generic/video_blitter.h>

#include <boot/images.h>
#include "splash_icons_borderless.h"
#include <boot/platform/generic/video_splash.h>

#include <stdio.h>
#include <stdlib.h>

#include <zlib.h>


//#define TRACE_VIDEO
#ifdef TRACE_VIDEO
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


static uint8 sSplashBackground;


extern "C" void
video_set_splash_background(uint8 shade)
{
	sSplashBackground = shade;
}


static void
fill_gray_background(uint8* buffer, size_t available, uint32 width,
	uint32 height, uint32 stride, uint8 shade)
{
	// Bound each row by both its pitch and the actual framebuffer mapping.
	uint64 rowBytes = (uint64)width * 4;
	if (rowBytes > stride)
		return;
	for (uint32 y = 0; y < height; y++) {
		uint64 offset = (uint64)y * stride;
		if (offset >= available)
			break;
		size_t bytes = min_c(rowBytes, available - offset);
		uint8* row = buffer + offset;
		for (size_t x = 0; x + 4 <= bytes; x += 4) {
			row[x] = row[x + 1] = row[x + 2] = shade;
			row[x + 3] = 0;
		}
	}
}


static void
replace_black_background(uint8* image, size_t size, uint8 shade)
{
	if (shade == 0)
		return;
	// Splash assets are RGB images with a black backdrop, not alpha images.
	// Keep every foreground color intact, and remap only the backdrop.
	for (size_t i = 0; i + 3 <= size; i += 3) {
		if (image[i] == 0 && image[i + 1] == 0 && image[i + 2] == 0)
			image[i] = image[i + 1] = image[i + 2] = shade;
	}
}


static status_t
uncompress(const uint8 compressed[], unsigned int compressedSize,
	uint8* uncompressed, unsigned int uncompressedSize)
{
	if (compressedSize == 0 || uncompressedSize == 0)
		return B_BAD_VALUE;

	// prepare zlib stream
	z_stream zStream = {
		(Bytef*)compressed,		// next_in
		compressedSize,			// avail_in
		0,						// total_in
		(Bytef*)uncompressed,	// next_out
		uncompressedSize,		// avail_out
		0,						// total_out
		0,						// msg
		0,						// state
		Z_NULL,					// zalloc (kernel_args_malloc?)
		Z_NULL,					// zfree (kernel_args_free?)
		Z_NULL,					// opaque
		0,						// data_type
		0,						// adler
		0						// reserved
	};

	int zlibError = inflateInit(&zStream);
	if (zlibError != Z_OK)
		return B_ERROR;	// TODO: translate zlibError

	// inflate
	status_t status = B_OK;
	zlibError = inflate(&zStream, Z_FINISH);
	if (zlibError != Z_STREAM_END) {
		if (zlibError == Z_OK)
			status = B_BUFFER_OVERFLOW;
		else
			status = B_ERROR;	// TODO: translate zlibError
	}

	// clean up
	zlibError = inflateEnd(&zStream);
	if (zlibError != Z_OK && status == B_OK)
		status = B_ERROR;	// TODO: translate zlibError

	if (status == B_OK && zStream.total_out != uncompressedSize)
		status = B_ERROR;

	return status;
}


static bool
prepare_borderless_icons(uint8* icons, size_t size, uint8 shade)
{
	if (shade == 0 || kSplashIconsWidth != 308 || kSplashIconsHeight != 80
		|| size != 308 * 80 * 3) {
		return false;
	}

	const unsigned int alphaSize = 308 * 80 * 4;
	uint8* alpha = (uint8*)malloc(alphaSize);
	if (alpha == NULL)
		return false;
	status_t status = uncompress(kSplashBorderlessIconsBGRA,
		sizeof(kSplashBorderlessIconsBGRA), alpha, alphaSize);
	if (status != B_OK) {
		free(alpha);
		return false;
	}

	// The native icon layers contain straight BGRA, without the button layer.
	// Composite both states here: the kernel reuses the active upper half.
	for (size_t i = 0, j = 0; i < size; i += 3, j += 4) {
		uint32 opacity = alpha[j + 3];
		for (size_t channel = 0; channel < 3; channel++) {
			icons[i + channel] = (alpha[j + channel] * opacity
				+ shade * (255 - opacity) + 127) / 255;
		}
	}
	free(alpha);
	return true;
}


extern "C" void
video_blit_image(addr_t frameBuffer, const uint8 *data,
	uint16 width, uint16 height, uint16 imageWidth, uint16 left, uint16 top)
{
	if (gKernelArgs.frame_buffer.depth == 4) {
		// call platform specific code since it's really platform-specific.
		platform_blit4(frameBuffer, data, width, height,
			imageWidth, left, top);
	} else {
		BlitParameters params;
		params.from = data;
		params.fromWidth = imageWidth;
		params.fromLeft = params.fromTop = 0;
		params.fromRight = width;
		params.fromBottom = height;
		params.to = (uint8*)frameBuffer;
		params.toBytesPerRow = gKernelArgs.frame_buffer.bytes_per_row;
		params.toLeft = left;
		params.toTop = top;
		blit(params, gKernelArgs.frame_buffer.depth);
	}
}


extern "C" status_t
video_display_splash(addr_t frameBuffer)
{
	if (!gKernelArgs.frame_buffer.enabled)
		return B_NO_INIT;

	uint8 shade = gKernelArgs.frame_buffer.depth == 32 ? sSplashBackground : 0;
	if (shade != 0) {
		// Fill directly: clearing to black first produces a visible flash.
		fill_gray_background((uint8*)frameBuffer,
			gKernelArgs.frame_buffer.physical_buffer.size,
			gKernelArgs.frame_buffer.width, gKernelArgs.frame_buffer.height,
			gKernelArgs.frame_buffer.bytes_per_row, shade);
	} else {
		addr_t pos = 0;
		// Limit area to clear to estimated screen area
		// UEFI can happily report a >256M framebuffer
		addr_t size = min_c(gKernelArgs.frame_buffer.width
				* gKernelArgs.frame_buffer.height * 4u,
			gKernelArgs.frame_buffer.physical_buffer.size);

		if (size >= 64) {
			// Align writes
			for (addr_t align = (8 - (frameBuffer & 7)) & 7; pos < align; pos++)
				*(char*)(frameBuffer + pos) = 0;
			// Write eight bytes, many many times, but not too many
			for (addr_t alignSize = size - 8; pos < alignSize; pos +=8) {
				*(uint32*)(frameBuffer + pos) = 0;
				*(uint32*)(frameBuffer + pos + 4) = 0;
			}
		}
		// Write a few bytes more
		for (; pos < size; pos++)
			*(char*)(frameBuffer + pos) = 0;
	}

	int width, height, x, y;
	if (gKernelArgs.frame_buffer.depth == 8)
		platform_set_palette(k8BitPalette);

	uint8* uncompressedLogo = NULL;
	unsigned int uncompressedSize = kSplashLogoWidth * kSplashLogoHeight;
	switch (gKernelArgs.frame_buffer.depth) {
		case 8:
			uncompressedLogo = (uint8*)malloc(uncompressedSize);
			if (uncompressedLogo == NULL)
				return B_NO_MEMORY;

			uncompress(kSplashLogo8BitCompressedImage,
				sizeof(kSplashLogo8BitCompressedImage), uncompressedLogo,
				uncompressedSize);
		break;
		default: // 24 bits is assumed here
			uncompressedSize *= 3;
			uncompressedLogo = (uint8*)malloc(uncompressedSize);
			if (uncompressedLogo == NULL)
				return B_NO_MEMORY;

			uncompress(kSplashLogo24BitCompressedImage,
				sizeof(kSplashLogo24BitCompressedImage), uncompressedLogo,
				uncompressedSize);
		break;
	}

	replace_black_background(uncompressedLogo, uncompressedSize, shade);

	// TODO: support 4-bit indexed version of the images!

	// render splash logo
	compute_splash_logo_placement(gKernelArgs.frame_buffer.width, gKernelArgs.frame_buffer.height,
		width, height, x, y);
	video_blit_image(frameBuffer, uncompressedLogo, width, height,
		kSplashLogoWidth, x, y);

	free(uncompressedLogo);

	const uint8* lowerHalfIconImage;
	unsigned int iconsSize = kSplashIconsWidth * kSplashIconsHeight;
	const uint16 iconsHalfHeight = kSplashIconsHeight / 2;
	switch (gKernelArgs.frame_buffer.depth) {
		case 8:
			// pointer into the lower half of the icons image data
			gKernelArgs.boot_splash
				= (uint8*)kernel_args_malloc(iconsSize);
			if (gKernelArgs.boot_splash == NULL)
				return B_NO_MEMORY;
			uncompress(kSplashIcons8BitCompressedImage,
				sizeof(kSplashIcons8BitCompressedImage),
				gKernelArgs.boot_splash, iconsSize);
			lowerHalfIconImage = (uint8 *)gKernelArgs.boot_splash
				+ (kSplashIconsWidth * iconsHalfHeight);
		break;
		default:	// 24bits is assumed here
			iconsSize *= 3;
			// pointer into the lower half of the icons image data
			gKernelArgs.boot_splash
				= (uint8*)kernel_args_malloc(iconsSize);
			if (gKernelArgs.boot_splash == NULL)
				return B_NO_MEMORY;
			uncompress(kSplashIcons24BitCompressedImage,
				sizeof(kSplashIcons24BitCompressedImage),
				gKernelArgs.boot_splash, iconsSize);
			lowerHalfIconImage = (uint8 *)gKernelArgs.boot_splash
				+ (kSplashIconsWidth * iconsHalfHeight) * 3;
		break;
	}

	// Keep the background consistent when the kernel lights later icons.
	if (!prepare_borderless_icons((uint8*)gKernelArgs.boot_splash, iconsSize, shade)) {
		replace_black_background((uint8*)gKernelArgs.boot_splash, iconsSize, shade);
	}

	// An unlit icon is the lit one at quarter opacity over the backdrop.
	// The active half is already composited over that backdrop, so a 1:3
	// mix with the backdrop is exactly a 25% alpha. Palette images cannot
	// blend.
	if (gKernelArgs.frame_buffer.depth != 8) {
		const uint8* active = (const uint8*)gKernelArgs.boot_splash;
		uint8* inactive = (uint8*)lowerHalfIconImage;
		size_t halfBytes = (size_t)kSplashIconsWidth * iconsHalfHeight * 3;
		for (size_t i = 0; i < halfBytes; i++)
			inactive[i] = (uint8)((active[i] + 3 * shade + 2) / 4);
	}

	// render initial (grayed out) icons
	// the grayed out version is the lower half of the icons image
	compute_splash_icons_placement(gKernelArgs.frame_buffer.width, gKernelArgs.frame_buffer.height,
		width, height, x, y);

	video_blit_image(frameBuffer, lowerHalfIconImage, width, height,
		kSplashIconsWidth, x, y);
	return B_OK;
}
