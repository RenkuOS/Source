/*
 * Copyright 2026, The RenkuOS authors.
 * Copyright (c) 2023 Imagination Technologies Ltd.
 * SPDX-License-Identifier: GPL-2.0-only OR MIT
 *
 * Command stream parser of the PowerVR Rogue driver, ported from the Linux
 * drm/imagination driver (pvr_stream.c, pvr_stream.h, pvr_stream_defs.c,
 * pvr_rogue_fwif_stream.h, plus the feature and quirk lookups of
 * pvr_device.c: pvr_device_has_feature(), pvr_device_has_uapi_quirk()).
 *
 * The input is the byte stream Mesa's pvr driver builds in
 * pvr_winsys_*_submit_info::fw_stream (and the static context state
 * streams), the cmd_stream of the Linux UAPI:
 *  - u32 length of the main stream (including this 8 byte header),
 *    u32 padding (must be 0),
 *  - the main stream: the fields of the command, naturally aligned, in the
 *    order of the stream definition, the ones of absent features skipped,
 *  - an optional extension stream: u32 headers (type, quirk mask,
 *    continuation bit), each followed by the fields of its quirks.
 *
 * Malformed input (truncated, misaligned beyond the end, unknown header
 * type or bits, missing "must have" quirk data) gives B_BAD_VALUE. The
 * functions only read the stream and write the destination; the caller
 * copies the stream into kernel memory first (Linux: memdup_user()).
 */
#ifndef PVR_STREAM_H
#define PVR_STREAM_H


#include <SupportDefs.h>

#include "pvr_rogue_fwif.h"


/*
 * Device description used by the parser and by pvr_render.cpp. Linux reads
 * these from the firmware image header (pvr_device_info.c); here the driver
 * fills them, e.g. with pvr_device_desc_init_bxe_4_32().
 */
struct pvr_device_desc {
	// features tested by the stream definitions (PVR_FEATURE_*)
	bool	cluster_grouping;
	bool	compute_morton_capable;
	bool	fb_cdc_v4;
	bool	gpu_multicore_support;
	bool	isp_zls_d24_s8_packing_ogl_mode;
	bool	s7_top_infrastructure;
	bool	tessellation;
	bool	tpu_dm_global_registers;
	bool	vdm_drawindirect;
	bool	vdm_object_level_lls;
	bool	zls_subtile;
	// inputs of the derived features CDM_USER_MODE_QUEUE and
	// REQUIRES_FB_CDC_ZLS_SETUP
	bool	has_cdm_control_stream_format;
	uint32	cdm_control_stream_format;
	bool	has_fbcdc_algorithm;
	uint32	fbcdc_algorithm;

	// features used by the free list, HWRT and context code
	bool	roguexe;
	bool	xe_memory_hierarchy;
	bool	has_simple_parameter_format_version;
	uint32	simple_parameter_format_version;
	uint32	num_raster_pipes;
	uint32	xpu_max_slaves;		// with gpu_multicore_support
	uint32	num_isp_ipp_pipes;	// without xe_memory_hierarchy
	uint32	isp_samples_per_pixel;
	uint32	tile_size_x;
	uint32	tile_size_y;

	// hardware quirks (BRNs): the UAPI ones of pvr_device_has_uapi_quirk()
	// and 66011 (free list minimum size)
	bool	brn47217;
	bool	brn48545;
	bool	brn49927;
	bool	brn51764;
	bool	brn62269;
	bool	brn66011;
};

/*!	Fills desc for BXE-4-32 MC1, BVNC 36.50.54.182 (DDK rgxconfig_km
	36.V.54.182 + rgxcore_km 36.50.54.182, Mesa bxe-4-32.h): roguexe,
	xe_memory_hierarchy, gpu_multicore_support (xpu_max_slaves 3),
	tpu_dm_global_registers, CDM control stream format 1, FBCDC algorithm
	50, 1 raster pipe, 4 ISP IPP pipes, 16x16 tiles, 1 ISP sample per
	pixel, simple parameter format 2, none of the quirks above.
*/
void pvr_device_desc_init_bxe_4_32(pvr_device_desc* desc);


/* pvr_device_info.h */
enum {
	PVR_FEATURE_CDM_USER_MODE_QUEUE,
	PVR_FEATURE_CLUSTER_GROUPING,
	PVR_FEATURE_COMPUTE_MORTON_CAPABLE,
	PVR_FEATURE_FB_CDC_V4,
	PVR_FEATURE_GPU_MULTICORE_SUPPORT,
	PVR_FEATURE_ISP_ZLS_D24_S8_PACKING_OGL_MODE,
	PVR_FEATURE_REQUIRES_FB_CDC_ZLS_SETUP,
	PVR_FEATURE_S7_TOP_INFRASTRUCTURE,
	PVR_FEATURE_TESSELLATION,
	PVR_FEATURE_TPU_DM_GLOBAL_REGISTERS,
	PVR_FEATURE_VDM_DRAWINDIRECT,
	PVR_FEATURE_VDM_OBJECT_LEVEL_LLS,
	PVR_FEATURE_ZLS_SUBTILE,
};

bool pvr_device_has_feature(const pvr_device_desc& desc, uint32 feature);
bool pvr_device_has_uapi_quirk(const pvr_device_desc& desc, uint32 quirk);


/* pvr_rogue_fwif_stream.h */
#define PVR_STREAM_EXTHDR_TYPE_SHIFT		29U
#define PVR_STREAM_EXTHDR_TYPE_MASK			(7U << PVR_STREAM_EXTHDR_TYPE_SHIFT)
#define PVR_STREAM_EXTHDR_TYPE_MAX			8U
#define PVR_STREAM_EXTHDR_CONTINUATION		(1U << 28)
#define PVR_STREAM_EXTHDR_DATA_MASK \
	(~(PVR_STREAM_EXTHDR_TYPE_MASK | PVR_STREAM_EXTHDR_CONTINUATION))

#define PVR_STREAM_EXTHDR_TYPE_GEOM0		0U
#define PVR_STREAM_EXTHDR_GEOM0_BRN49927	(1U << 0)
#define PVR_STREAM_EXTHDR_GEOM0_VALID		PVR_STREAM_EXTHDR_GEOM0_BRN49927

#define PVR_STREAM_EXTHDR_TYPE_FRAG0		0U
#define PVR_STREAM_EXTHDR_FRAG0_BRN47217	(1U << 0)
#define PVR_STREAM_EXTHDR_FRAG0_BRN49927	(1U << 1)
/* as Linux: the 47217 bit is not in the valid mask */
#define PVR_STREAM_EXTHDR_FRAG0_VALID		PVR_STREAM_EXTHDR_FRAG0_BRN49927

#define PVR_STREAM_EXTHDR_TYPE_COMPUTE0		0U
#define PVR_STREAM_EXTHDR_COMPUTE0_BRN49927	(1U << 0)
#define PVR_STREAM_EXTHDR_COMPUTE0_VALID	PVR_STREAM_EXTHDR_COMPUTE0_BRN49927


/* pvr_stream.h */
enum pvr_stream_type {
	PVR_STREAM_TYPE_GEOM = 0,
	PVR_STREAM_TYPE_FRAG,
	PVR_STREAM_TYPE_COMPUTE,
	PVR_STREAM_TYPE_TRANSFER,
	PVR_STREAM_TYPE_STATIC_RENDER_CONTEXT,
	PVR_STREAM_TYPE_STATIC_COMPUTE_CONTEXT,

	PVR_STREAM_TYPE_MAX
};

enum pvr_stream_size {
	PVR_STREAM_SIZE_8 = 0,
	PVR_STREAM_SIZE_16,
	PVR_STREAM_SIZE_32,
	PVR_STREAM_SIZE_64,
	PVR_STREAM_SIZE_ARRAY,
};

#define PVR_FEATURE_NOT		(1U << 31)
#define PVR_FEATURE_NONE	0xFFFFFFFFU

struct pvr_stream_def {
	uint32			offset;
	pvr_stream_size	size;
	uint32			array_size;
	uint32			feature;
};

struct pvr_stream_ext_def {
	const pvr_stream_def*	stream;
	uint32					stream_len;
	uint32					header_mask;
	uint32					quirk;
};

struct pvr_stream_ext_header {
	const pvr_stream_ext_def*	ext_streams;
	uint32						ext_streams_num;
	uint32						valid_mask;
};

struct pvr_stream_cmd_defs {
	pvr_stream_type					type;

	const pvr_stream_def*			main_stream;
	uint32							main_stream_len;

	uint32							ext_nr_headers;
	const pvr_stream_ext_header*	ext_headers;

	size_t							dest_size;
};

/* pvr_stream_defs.c */
extern const pvr_stream_cmd_defs pvr_cmd_geom_stream;
extern const pvr_stream_cmd_defs pvr_cmd_frag_stream;
extern const pvr_stream_cmd_defs pvr_cmd_compute_stream;
extern const pvr_stream_cmd_defs pvr_cmd_transfer_stream;
extern const pvr_stream_cmd_defs pvr_static_render_context_state_stream;
extern const pvr_stream_cmd_defs pvr_static_compute_context_state_stream;


/*!	pvr_stream_process(): fills dest (cmdDefs->dest_size bytes) from the
	stream. Fields that are not in the stream are left untouched, as in
	Linux; the caller zeroes dest first (Linux allocates it with kzalloc()).
	The "must have" quirk masks of pvr_stream_create_musthave_masks() are
	derived from desc on every call.
*/
status_t pvr_stream_process(const pvr_device_desc& desc,
	const pvr_stream_cmd_defs* cmdDefs, const void* stream,
	uint32 streamSize, void* dest);

/*	Convenience wrappers: zero the destination, then pvr_stream_process()
	with the matching definition. The kernel owned fields (frame number,
	flags, HWRT and fence addresses) are set by pvr_render.cpp afterwards.
*/
status_t pvr_stream_parse_geom(const pvr_device_desc& desc,
	const void* stream, uint32 streamSize, rogue_fwif_cmd_geom* cmd);
status_t pvr_stream_parse_frag(const pvr_device_desc& desc,
	const void* stream, uint32 streamSize, rogue_fwif_cmd_frag* cmd);
status_t pvr_stream_parse_compute(const pvr_device_desc& desc,
	const void* stream, uint32 streamSize, rogue_fwif_cmd_compute* cmd);
status_t pvr_stream_parse_transfer(const pvr_device_desc& desc,
	const void* stream, uint32 streamSize, rogue_fwif_cmd_transfer* cmd);
/*!	Static render context state (Linux init_render_fw_objs(): only
	ctxswitch_regs[0] is filled). An empty stream is B_BAD_VALUE, as Linux.
*/
status_t pvr_stream_parse_static_render_state(const pvr_device_desc& desc,
	const void* stream, uint32 streamSize,
	rogue_fwif_geom_registers_caswitch* regs);
/*!	Static compute context state (Linux init_compute_fw_objs()). An empty
	stream is B_BAD_VALUE, as Linux.
*/
status_t pvr_stream_parse_static_compute_state(const pvr_device_desc& desc,
	const void* stream, uint32 streamSize,
	rogue_fwif_cdm_registers_cswitch* regs);


#endif	// PVR_STREAM_H
