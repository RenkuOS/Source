/*
 * Copyright 1999 Precision Insight, Inc., Cedar Park, Texas.
 * Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Copyright 2003 Tungsten Graphics, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 *
 * Linux DRM/i915 UAPI subset used by the intel_render_9x render node.
 *
 * This header is shared between the kernel driver
 * (src/add-ons/kernel/drivers/graphics/intel_extreme/render_node.cpp) and
 * userspace (the libdrm_haiku shim and test tools). Mesa's iris winsys
 * speaks the standard i915 UAPI through libdrm, so the ioctl numbers and
 * the argument struct layouts below MUST match the canonical Linux
 * definitions byte for byte: the ioctl number encodes sizeof(struct),
 * and Mesa reads/writes these fields directly.
 *
 * Definitions transcribed from Linux UAPI
 * include/uapi/drm/drm.h and include/uapi/drm/i915_drm.h (matching
 * libdrm 2.4.125), re-typed with fixed-width integer types and Linux
 * __user annotations removed.
 */
#ifndef INTEL_RENDER_DRM_UAPI_H
#define INTEL_RENDER_DRM_UAPI_H


#include <stddef.h>
#include <stdint.h>


/*	ioctl encoding.

	The bit layout is <linux/ioctl.h>'s, but the direction values must be
	Haiku's: <sys/ioccom.h> defines _IOW = 0x80000000 and _IOR = 0x40000000,
	the opposite of Linux. Mesa's libdrm builds every ioctl number with the
	host _IOW/_IOR macros, so the kernel side must agree -- otherwise all
	IOR/IOW ops (GEM_CLOSE, GET_APERTURE, ...) arrive encoded with a
	different direction and are not recognised. IOWR (0xc0000000) is the
	same in both.	*/

#define DRM_IOCTL_BASE		'd'
#define DRM_COMMAND_BASE	0x40

#define DRM_IOC_NONE		0u
#define DRM_IOC_WRITE		2u	/* matches Haiku _IOW */
#define DRM_IOC_READ		1u	/* matches Haiku _IOR */
#define DRM_IOC_READ_WRITE	3u

#define DRM_IOC(dir, type, nr, size) \
	(((dir) << 30) | ((type) << 8) | (nr) | ((size) << 16))

#define DRM_IO(nr)			DRM_IOC(DRM_IOC_NONE, DRM_IOCTL_BASE, nr, 0)
#define DRM_IOR(nr, type)	DRM_IOC(DRM_IOC_READ, DRM_IOCTL_BASE, nr, \
								sizeof(type))
#define DRM_IOW(nr, type)	DRM_IOC(DRM_IOC_WRITE, DRM_IOCTL_BASE, nr, \
								sizeof(type))
#define DRM_IOWR(nr, type)	DRM_IOC(DRM_IOC_READ_WRITE, DRM_IOCTL_BASE, nr, \
								sizeof(type))


/*	generic DRM structs	*/

struct drm_version {
	int32_t		version_major;
	int32_t		version_minor;
	int32_t		version_patchlevel;
	size_t		name_len;
	char*		name;
	size_t		date_len;
	char*		date;
	size_t		desc_len;
	char*		desc;
};

struct drm_gem_close {
	uint32_t	handle;
	uint32_t	pad;
};

struct drm_gem_flink {
	uint32_t	handle;
	uint32_t	name;
};

struct drm_gem_open {
	uint32_t	name;
	uint32_t	handle;
	uint64_t	size;
};

struct drm_prime_handle {
	uint32_t	handle;
	uint32_t	flags;
	int32_t		fd;
};

struct drm_syncobj_create {
	uint32_t	handle;
	uint32_t	flags;		/* DRM_SYNCOBJ_CREATE_SIGNALED */
};

struct drm_syncobj_destroy {
	uint32_t	handle;
	uint32_t	pad;
};

struct drm_syncobj_handle {
	uint32_t	handle;
	uint32_t	flags;
	int32_t		fd;
	uint32_t	pad;
	uint64_t	point;
};

struct drm_syncobj_wait {
	uint64_t	handles;
	int64_t		timeout_nsec;
	uint32_t	count_handles;
	uint32_t	flags;
	uint32_t	first_signaled;
	uint32_t	pad;
	uint64_t	deadline_nsec;
};

struct drm_syncobj_timeline_wait {
	uint64_t	handles;
	uint64_t	points;
	int64_t		timeout_nsec;
	uint32_t	count_handles;
	uint32_t	flags;
	uint32_t	first_signaled;
	uint32_t	pad;
	uint64_t	deadline_nsec;
};

#define DRM_SYNCOBJ_CREATE_SIGNALED			(1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL		(1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT	(1 << 1)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE	(1 << 2)

#define DRM_IOCTL_VERSION			DRM_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_GEM_CLOSE			DRM_IOW(0x09, struct drm_gem_close)
#define DRM_IOCTL_GEM_FLINK			DRM_IOWR(0x0a, struct drm_gem_flink)
#define DRM_IOCTL_GEM_OPEN			DRM_IOWR(0x0b, struct drm_gem_open)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD	DRM_IOWR(0x2d, struct drm_prime_handle)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE	DRM_IOWR(0x2e, struct drm_prime_handle)
#define DRM_IOCTL_SYNCOBJ_CREATE	DRM_IOWR(0xBF, struct drm_syncobj_create)
#define DRM_IOCTL_SYNCOBJ_DESTROY	DRM_IOWR(0xC0, struct drm_syncobj_destroy)
#define DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD	DRM_IOWR(0xC1, struct drm_syncobj_handle)
#define DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE	DRM_IOWR(0xC2, struct drm_syncobj_handle)
#define DRM_IOCTL_SYNCOBJ_WAIT		DRM_IOWR(0xC3, struct drm_syncobj_wait)
#define DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT	DRM_IOWR(0xCA, \
									struct drm_syncobj_timeline_wait)


/*	i915 ioctl opcodes (must be kept compact -- no holes)	*/

#define DRM_I915_GETPARAM			0x06
#define DRM_I915_SETPARAM			0x07
#define DRM_I915_GEM_GET_APERTURE	0x23
#define DRM_I915_GEM_MMAP			0x1e
#define DRM_I915_GEM_MMAP_GTT		0x24
#define DRM_I915_GEM_SET_DOMAIN		0x1f
#define DRM_I915_GEM_SET_TILING		0x21
#define DRM_I915_GEM_GET_TILING		0x22
#define DRM_I915_GEM_MADVISE		0x26
#define DRM_I915_GET_RESET_STATS	0x32
#define DRM_I915_GEM_BUSY			0x17
#define DRM_I915_GEM_WAIT			0x2c
#define DRM_I915_GEM_CONTEXT_CREATE	0x2d
#define DRM_I915_GEM_CONTEXT_DESTROY	0x2e
#define DRM_I915_GEM_SET_CACHING	0x2f
#define DRM_I915_REG_READ			0x31
#define DRM_I915_GEM_CREATE			0x1b
#define DRM_I915_GEM_USERPTR		0x33
#define DRM_I915_GEM_CONTEXT_GETPARAM	0x34
#define DRM_I915_GEM_CONTEXT_SETPARAM	0x35
#define DRM_I915_QUERY				0x39
#define DRM_I915_GEM_EXECBUFFER2	0x29
#define DRM_I915_GEM_CREATE_EXT		0x3c


/*	i915 structs	*/

struct drm_i915_getparam {
	int32_t		param;
	int32_t*	value;
};

/*	i915 query item ids (DRM_IOCTL_I915_QUERY)	*/

#define DRM_I915_QUERY_TOPOLOGY_INFO			1
#define DRM_I915_QUERY_ENGINE_INFO			2
#define DRM_I915_QUERY_PERF_CONFIG			3
#define DRM_I915_QUERY_MEMORY_REGIONS			4
#define DRM_I915_QUERY_HWCONFIG_BLOB			5
#define DRM_I915_QUERY_GEOMETRY_SUBSLICES		6
#define DRM_I915_QUERY_GUC_SUBMISSION_VERSION	7

#define I915_ENGINE_CLASS_RENDER		0
#define I915_ENGINE_CLASS_COPY			1
#define I915_ENGINE_CLASS_VIDEO			2
#define I915_ENGINE_CLASS_VIDEO_ENHANCE	3
#define I915_ENGINE_CLASS_COMPUTE		4

struct i915_engine_class_instance {
	uint16_t	engine_class;
	uint16_t	engine_instance;
};

struct drm_i915_engine_info {
	struct i915_engine_class_instance	engine;
	uint32_t	rsvd0;
	uint64_t	flags;
	uint64_t	capabilities;
	uint16_t	logical_instance;
	uint16_t	rsvd1[3];
	uint64_t	rsvd2[3];
};

struct drm_i915_query_engine_info {
	uint32_t	num_engines;
	uint32_t	rsvd[3];
	struct drm_i915_engine_info	engines[];
};

struct drm_i915_gem_create {
	uint64_t	size;
	uint32_t	handle;
	uint32_t	pad;
};

struct drm_i915_gem_create_ext {
	uint64_t	size;
	uint32_t	handle;
	uint32_t	flags;
	uint64_t	extensions;
};

struct drm_i915_gem_mmap {
	uint32_t	handle;
	uint32_t	pad;
	uint64_t	offset;
	uint64_t	size;
	uint64_t	addr_ptr;
	uint64_t	flags;
};

struct drm_i915_gem_mmap_gtt {
	uint32_t	handle;
	uint32_t	pad;
	uint64_t	offset;
};

struct drm_i915_gem_mmap_offset {
	uint32_t	handle;
	uint32_t	pad;
	uint64_t	offset;
	uint64_t	flags;
	uint64_t	extensions;
};

struct drm_i915_gem_set_domain {
	uint32_t	handle;
	uint32_t	read_domains;
	uint32_t	write_domain;
};

struct drm_i915_gem_set_tiling {
	uint32_t	handle;
	uint32_t	tiling_mode;
	uint32_t	stride;
	uint32_t	swizzle_mode;
};

struct drm_i915_gem_get_tiling {
	uint32_t	handle;
	uint32_t	tiling_mode;
	uint32_t	swizzle_mode;
	uint32_t	phys_swizzle_mode;
};

struct drm_i915_gem_get_aperture {
	uint64_t	aper_size;
	uint64_t	aper_available_size;
};

struct drm_i915_gem_madvise {
	uint32_t	handle;
	uint32_t	madv;
	uint32_t	retained;
};

struct drm_i915_gem_busy {
	uint32_t	handle;
	uint32_t	busy;
};

struct drm_i915_gem_wait {
	uint32_t	bo_handle;
	uint32_t	flags;
	int64_t		timeout_ns;
};

struct drm_i915_gem_caching {
	uint32_t	handle;
	uint32_t	caching;
};

struct drm_i915_gem_context_create {
	uint32_t	ctx_id;
	uint32_t	pad;
};

struct i915_user_extension {
	uint64_t	next_extension;
	uint32_t	name;
	uint32_t	flags;
	uint32_t	rsvd[4];
};

struct drm_i915_gem_context_create_ext {
	uint32_t	ctx_id;
	uint32_t	flags;
	uint64_t	extensions;
};

struct drm_i915_gem_context_param {
	uint32_t	ctx_id;
	uint32_t	size;
	uint64_t	param;
	uint64_t	value;
};

struct drm_i915_gem_context_create_ext_setparam {
	struct i915_user_extension base;
	struct drm_i915_gem_context_param param;
};

struct drm_i915_gem_context_destroy {
	uint32_t	ctx_id;
	uint32_t	pad;
};

struct drm_i915_reg_read {
	uint64_t	offset;
	uint64_t	val;
};

#define I915_REG_READ_8B_WA	(1ul << 0)

struct drm_i915_gem_userptr {
	uint64_t	user_ptr;
	uint64_t	user_size;
	uint32_t	flags;
	uint32_t	handle;
};

#define I915_USERPTR_READ_ONLY		0x1
#define I915_USERPTR_UNSYNCHRONIZED	0x80000000

struct drm_i915_query_item {
	uint64_t	query_id;
	int32_t		length;
	uint32_t	flags;
	uint64_t	data_ptr;
};

struct drm_i915_query {
	uint32_t	num_items;
	uint32_t	flags;
	uint64_t	items_ptr;
};

struct drm_i915_reset_stats {
	uint32_t	ctx_id;
	uint32_t	flags;
	uint32_t	reset_count;
	uint32_t	batch_active;
	uint32_t	batch_pending;
	uint32_t	pad;
};

struct drm_i915_gem_exec_fence {
	uint32_t	handle;
	uint32_t	flags;
};

struct drm_i915_gem_exec_object2 {
	uint32_t	handle;
	uint32_t	relocation_count;
	uint64_t	relocs_ptr;
	uint64_t	alignment;
	uint64_t	offset;
	uint64_t	flags;
	uint64_t	rsvd1;
	uint64_t	rsvd2;
};

struct drm_i915_gem_execbuffer2 {
	uint64_t	buffers_ptr;
	uint32_t	buffer_count;
	uint32_t	batch_start_offset;
	uint32_t	batch_len;
	uint32_t	DR1;
	uint32_t	DR4;
	uint32_t	num_cliprects;
	uint64_t	cliprects_ptr;
	uint64_t	flags;
	uint64_t	rsvd1;		/* context id */
	uint64_t	rsvd2;
};


#define DRM_IOCTL_I915_GETPARAM		DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_GETPARAM, \
										struct drm_i915_getparam)
#define DRM_IOCTL_I915_GEM_GET_APERTURE	DRM_IOR(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_GET_APERTURE, \
										struct drm_i915_gem_get_aperture)
#define DRM_IOCTL_I915_GEM_MMAP		DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_MMAP, \
										struct drm_i915_gem_mmap)
#define DRM_IOCTL_I915_GEM_MMAP_GTT	DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_MMAP_GTT, \
										struct drm_i915_gem_mmap_gtt)
#define DRM_IOCTL_I915_GEM_MMAP_OFFSET	DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_MMAP_GTT, \
										struct drm_i915_gem_mmap_offset)
#define DRM_IOCTL_I915_GEM_SET_DOMAIN	DRM_IOW(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_SET_DOMAIN, \
										struct drm_i915_gem_set_domain)
#define DRM_IOCTL_I915_GEM_SET_TILING	DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_SET_TILING, \
										struct drm_i915_gem_set_tiling)
#define DRM_IOCTL_I915_GEM_GET_TILING	DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_GET_TILING, \
										struct drm_i915_gem_get_tiling)
#define DRM_IOCTL_I915_GEM_MADVISE	DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_MADVISE, \
										struct drm_i915_gem_madvise)
#define DRM_IOCTL_I915_GET_RESET_STATS	DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_GET_RESET_STATS, \
										struct drm_i915_reset_stats)
#define DRM_IOCTL_I915_GEM_BUSY		DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_BUSY, \
										struct drm_i915_gem_busy)
#define DRM_IOCTL_I915_GEM_WAIT		DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_WAIT, \
										struct drm_i915_gem_wait)
#define DRM_IOCTL_I915_GEM_CONTEXT_CREATE	DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_CONTEXT_CREATE, \
										struct drm_i915_gem_context_create)
#define DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT	DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_CONTEXT_CREATE, \
										struct \
										drm_i915_gem_context_create_ext)
#define DRM_IOCTL_I915_GEM_CONTEXT_DESTROY	DRM_IOW(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_CONTEXT_DESTROY, \
										struct \
										drm_i915_gem_context_destroy)
#define DRM_IOCTL_I915_GEM_SET_CACHING	DRM_IOW(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_SET_CACHING, \
										struct drm_i915_gem_caching)
#define DRM_IOCTL_I915_REG_READ		DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_REG_READ, \
										struct drm_i915_reg_read)
#define DRM_IOCTL_I915_GEM_CREATE	DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_CREATE, \
										struct drm_i915_gem_create)
#define DRM_IOCTL_I915_GEM_USERPTR	DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_USERPTR, \
										struct drm_i915_gem_userptr)
#define DRM_IOCTL_I915_GEM_CREATE_EXT	DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_CREATE_EXT, \
										struct drm_i915_gem_create_ext)
#define DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM	DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_CONTEXT_GETPARAM, \
										struct drm_i915_gem_context_param)
#define DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM	DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_CONTEXT_SETPARAM, \
										struct drm_i915_gem_context_param)
#define DRM_IOCTL_I915_GEM_EXECBUFFER2	DRM_IOW(DRM_COMMAND_BASE \
										+ DRM_I915_GEM_EXECBUFFER2, \
										struct drm_i915_gem_execbuffer2)
#define DRM_IOCTL_I915_QUERY		DRM_IOWR(DRM_COMMAND_BASE \
										+ DRM_I915_QUERY, \
										struct drm_i915_query)


/*	I915 GETPARAM ids	*/

#define I915_PARAM_CHIPSET_ID			4
#define I915_PARAM_HAS_GEM				5
#define I915_PARAM_HAS_EXECBUF2			9
#define I915_PARAM_HAS_BSD				10
#define I915_PARAM_HAS_BLT				11
#define I915_PARAM_HAS_RELAXED_FENCING	12
#define I915_PARAM_HAS_RELAXED_DELTA	15
#define I915_PARAM_HAS_LLC				17
#define I915_PARAM_HAS_ALIASING_PPGTT	18
#define I915_PARAM_HAS_WAIT_TIMEOUT		19
#define I915_PARAM_HAS_VEBOX			22
#define I915_PARAM_HAS_EXEC_NO_RELOC	25
#define I915_PARAM_HAS_EXEC_HANDLE_LUT	26
#define I915_PARAM_MMAP_VERSION			30
#define I915_PARAM_REVISION				32
#define I915_PARAM_SUBSLICE_TOTAL		33
#define I915_PARAM_EU_TOTAL				34
#define I915_PARAM_HAS_GPU_RESET		35
#define I915_PARAM_HAS_RESOURCE_STREAMER 36
#define I915_PARAM_HAS_EXEC_SOFTPIN		37
#define I915_PARAM_MMAP_GTT_VERSION		40
#define I915_PARAM_SLICE_MASK			46
#define I915_PARAM_SUBSLICE_MASK		47
#define I915_PARAM_HAS_EXEC_BATCH_FIRST	48
#define I915_PARAM_HAS_EXEC_FENCE_ARRAY	49
#define I915_PARAM_HAS_CONTEXT_ISOLATION 50
#define I915_PARAM_CS_TIMESTAMP_FREQUENCY 51
#define I915_PARAM_HAS_EXEC_SUBMIT_FENCE 53
#define I915_PARAM_PERF_REVISION		54
#define I915_PARAM_HAS_EXEC_TIMELINE_FENCES 55
#define I915_PARAM_HAS_USERPTR_PROBE	56
#define I915_PARAM_PXP_STATUS			58


/*	GEM flags and enums	*/

#define I915_GEM_DOMAIN_CPU			0x00000001
#define I915_GEM_DOMAIN_RENDER		0x00000002
#define I915_GEM_DOMAIN_SAMPLER		0x00000004
#define I915_GEM_DOMAIN_COMMAND		0x00000008
#define I915_GEM_DOMAIN_INSTRUCTION	0x00000010
#define I915_GEM_DOMAIN_VERTEX		0x00000020

#define I915_CACHING_NONE			0
#define I915_CACHING_CACHED			1

#define I915_TILING_NONE			0
#define I915_TILING_X				1
#define I915_TILING_Y				2

#define I915_MADV_WILLNEED			0
#define I915_MADV_DONTNEED			1

#define I915_MMAP_OFFSET_GTT		0
#define I915_MMAP_OFFSET_WC			1
#define I915_MMAP_OFFSET_WB			2
#define I915_MMAP_OFFSET_UC			3
#define I915_MMAP_OFFSET_FIXED		4

#define I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS	(1 << 0)
#define I915_GEM_CREATE_EXT_MEMORY_REGIONS			1
#define I915_GEM_CREATE_EXT_PROTECTED_CONTENT		2
#define I915_GEM_CREATE_EXT_SET_PAT					3


/*	context params	*/

#define I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS	(1 << 0)
#define I915_CONTEXT_PRIVATE_PARAM_BOOST			0x80000000
#define I915_CONTEXT_PARAM_BAN_PERIOD				0x1
#define I915_CONTEXT_PARAM_NO_ZEROMAP				0x2
#define I915_CONTEXT_PARAM_GTT_SIZE					0x3
#define I915_CONTEXT_PARAM_NO_ERROR_CAPTURE			0x4
#define I915_CONTEXT_PARAM_BANNABLE					0x5
#define I915_CONTEXT_PARAM_PRIORITY					0x6
#define I915_CONTEXT_PARAM_SSEU						0x7
#define I915_CONTEXT_PARAM_RECOVERABLE				0x8
#define I915_CONTEXT_PARAM_VM						0x9
#define I915_CONTEXT_PARAM_ENGINES					0xa
#define I915_CONTEXT_PARAM_PERSISTENCE				0xb
#define I915_CONTEXT_PARAM_SCHED_PRIORITY			0xc
#define I915_CONTEXT_PARAM_PROTECTED_CONTENT		0xd
#define I915_CONTEXT_PARAM_LOW_LATENCY				0xe
#define I915_CONTEXT_DEFAULT_PRIORITY				0
#define I915_CONTEXT_MIN_USER_PRIORITY				(-1023)
#define I915_CONTEXT_MAX_USER_PRIORITY				1023

#define I915_CONTEXT_CREATE_EXT_SETPARAM			0
#define I915_CONTEXT_CREATE_EXT_FLAGS_RELAXED		1


/*	execbuffer2 flags	*/

#define I915_EXEC_RENDER				(1 << 0)
#define I915_EXEC_BSD					(2 << 0)
#define I915_EXEC_BLT					(3 << 0)
#define I915_EXEC_VEBOX					(4 << 0)
#define I915_EXEC_RING_MASK				0x3f
#define I915_EXEC_DEFAULT				(0 << 0)
#define I915_EXEC_FENCE_IN				(1 << 16)
#define I915_EXEC_FENCE_SUBMIT			(1 << 20)
#define I915_EXEC_FENCE_OUT				(1 << 17)
#define I915_EXEC_FENCE_ARRAY			(1 << 19)
#define I915_EXEC_NO_RELOC				(1 << 11)
#define I915_EXEC_HANDLE_LUT			(1 << 12)
#define I915_EXEC_IS_PINNED				(1 << 10)
#define I915_EXEC_BATCH_FIRST			(1 << 18)
#define I915_EXEC_CONTEXT_ID_MASK		((1llu << 12) - 1)

#define I915_EXEC_FENCE_WAIT			(1 << 0)
#define I915_EXEC_FENCE_SIGNAL			(1 << 1)

#define I915_GEM_EXEC_OBJECT_NEEDS_FENCE	(1 << 0)
#define I915_GEM_EXEC_OBJECT_NEEDS_GTT		(1 << 1)
#define I915_GEM_EXEC_OBJECT_WRITE			(1 << 2)
#define I915_GEM_EXEC_OBJECT_SUPPORTS_48B_ADDRESS (1 << 3)
#define I915_GEM_EXEC_OBJECT_PINNED			(1 << 4)
#define I915_GEM_EXEC_OBJECT_PAD_TO_SIZE	(1 << 5)


#endif	/* INTEL_RENDER_DRM_UAPI_H */
