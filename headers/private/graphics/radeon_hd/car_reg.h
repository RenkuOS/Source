/*
 * DCE_11_0 Register documentation
 *
 * Copyright (C) 2014  Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 * AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* This header has been modified from its original version:
 * it has been formatted to contain only the constants Haiku needs. */

#ifndef DCE_11_0_D_H
#define DCE_11_0_D_H

#define CAR_mmDC_GPIO_HPD_A                                                         0x488d

/* Line buffer, DCP0 (dword offsets; the same in DCE 8). At 0x1ac1, where
 * Evergreen had DESKTOP_HEIGHT, DCE 8 and later have LB_MEMORY_CTRL. */
#define CAR_mmLB_MEMORY_CTRL                                                        0x1ac1
#define CAR_mmLB_DESKTOP_HEIGHT                                                     0x1ac3

/* GRPH_CONTROL fields (the same in DCE 8) */
#define CAR_GRPH_CONTROL__GRPH_DEPTH_MASK                                          0x3
#define CAR_GRPH_CONTROL__GRPH_FORMAT_MASK                                         0x700
#define CAR_GRPH_CONTROL__GRPH_ARRAY_MODE_MASK                                     0xf00000
#define CAR_GRPH_CONTROL__GRPH_ARRAY_MODE__SHIFT                                   0x14
#define CAR_ARRAY_LINEAR_ALIGNED                                                   0x1

#endif /* DCE_11_0_D_H */
