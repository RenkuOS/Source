/*
 * Copyright 2021, Haiku, Inc. All rights reserved.
 * Released under the terms of the MIT License
 */


#include <asm_defs.h>

.text

/* float __swap_float(float value) */
FUNCTION(__swap_float):
		fmov w0, s0	// Bitcopy float to general register
		rev w0, w0	// GCC8.3 does this for __builtin_bswap32
		fmov s0, w0	// and back
		ret
FUNCTION_END(__swap_float)


/* double __swap_double(double value) */
FUNCTION(__swap_double):
		fmov x0, d0	// Bitcopy double to general register
		rev x0, x0	// GCC8.3 does this for __builtin_bswap64
		fmov d0, x0	// and back
		ret
FUNCTION_END(__swap_double)
