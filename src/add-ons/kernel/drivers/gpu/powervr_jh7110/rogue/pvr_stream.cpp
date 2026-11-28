/*
 * Copyright 2026, The RenkuOS authors.
 * Copyright (c) 2023 Imagination Technologies Ltd.
 * SPDX-License-Identifier: GPL-2.0-only OR MIT
 *
 * Port of pvr_stream.c and pvr_stream_defs.c of the Linux drm/imagination
 * driver, plus pvr_device_has_feature() and pvr_device_has_uapi_quirk()
 * (pvr_device.c) on a pvr_device_desc.
 */


#include "pvr_stream.h"

#include <string.h>


//	#pragma mark - device description


void
pvr_device_desc_init_bxe_4_32(pvr_device_desc* desc)
{
	memset(desc, 0, sizeof(*desc));
	desc->gpu_multicore_support = true;
	desc->tpu_dm_global_registers = true;
	desc->has_cdm_control_stream_format = true;
	desc->cdm_control_stream_format = 1;
	desc->has_fbcdc_algorithm = true;
	desc->fbcdc_algorithm = 50;

	desc->roguexe = true;
	desc->xe_memory_hierarchy = true;
	desc->has_simple_parameter_format_version = true;
	desc->simple_parameter_format_version = 2;
	desc->num_raster_pipes = 1;
	desc->xpu_max_slaves = 3;
	desc->num_isp_ipp_pipes = 4;
	desc->isp_samples_per_pixel = 1;
	desc->tile_size_x = 16;
	desc->tile_size_y = 16;
}


bool
pvr_device_has_feature(const pvr_device_desc& desc, uint32 feature)
{
	switch (feature) {
		case PVR_FEATURE_CLUSTER_GROUPING:
			return desc.cluster_grouping;
		case PVR_FEATURE_COMPUTE_MORTON_CAPABLE:
			return desc.compute_morton_capable;
		case PVR_FEATURE_FB_CDC_V4:
			return desc.fb_cdc_v4;
		case PVR_FEATURE_GPU_MULTICORE_SUPPORT:
			return desc.gpu_multicore_support;
		case PVR_FEATURE_ISP_ZLS_D24_S8_PACKING_OGL_MODE:
			return desc.isp_zls_d24_s8_packing_ogl_mode;
		case PVR_FEATURE_S7_TOP_INFRASTRUCTURE:
			return desc.s7_top_infrastructure;
		case PVR_FEATURE_TESSELLATION:
			return desc.tessellation;
		case PVR_FEATURE_TPU_DM_GLOBAL_REGISTERS:
			return desc.tpu_dm_global_registers;
		case PVR_FEATURE_VDM_DRAWINDIRECT:
			return desc.vdm_drawindirect;
		case PVR_FEATURE_VDM_OBJECT_LEVEL_LLS:
			return desc.vdm_object_level_lls;
		case PVR_FEATURE_ZLS_SUBTILE:
			return desc.zls_subtile;

		// derived features
		case PVR_FEATURE_CDM_USER_MODE_QUEUE:
		{
			// PVR_FEATURE_VALUE() leaves the value 0 if the feature is absent
			uint32 format = desc.has_cdm_control_stream_format
				? desc.cdm_control_stream_format : 0;
			return format >= 2 && format <= 4;
		}
		case PVR_FEATURE_REQUIRES_FB_CDC_ZLS_SETUP:
			if (desc.has_fbcdc_algorithm)
				return desc.fbcdc_algorithm < 3 || desc.fb_cdc_v4;
			return false;

		default:
			return false;
	}
}


bool
pvr_device_has_uapi_quirk(const pvr_device_desc& desc, uint32 quirk)
{
	switch (quirk) {
		case 47217:
			return desc.brn47217;
		case 48545:
			return desc.brn48545;
		case 49927:
			return desc.brn49927;
		case 51764:
			return desc.brn51764;
		case 62269:
			return desc.brn62269;
		default:
			return false;
	}
}


//	#pragma mark - stream definitions (pvr_stream_defs.c)


#define PVR_STREAM_DEF_SET(owner, member, _size, _array_size, _feature) \
	{ offsetof(struct owner, member), (_size), (_array_size), (_feature) }

#define PVR_STREAM_DEF(owner, member, member_size) \
	PVR_STREAM_DEF_SET(owner, member, PVR_STREAM_SIZE_ ## member_size, 0, \
		PVR_FEATURE_NONE)

#define PVR_STREAM_DEF_FEATURE(owner, member, member_size, feature) \
	PVR_STREAM_DEF_SET(owner, member, PVR_STREAM_SIZE_ ## member_size, 0, \
		feature)

#define PVR_STREAM_DEF_NOT_FEATURE(owner, member, member_size, feature) \
	PVR_STREAM_DEF_SET(owner, member, PVR_STREAM_SIZE_ ## member_size, 0, \
		(feature) | PVR_FEATURE_NOT)

#define PVR_STREAM_DEF_ARRAY(owner, member) \
	PVR_STREAM_DEF_SET(owner, member, PVR_STREAM_SIZE_ARRAY, \
		sizeof(((struct owner*)0)->member), PVR_FEATURE_NONE)


/*
 * The order of the entries is the stream layout: it must not change.
 */
static const pvr_stream_def rogue_fwif_cmd_geom_stream[] = {
	PVR_STREAM_DEF(rogue_fwif_cmd_geom, regs.vdm_ctrl_stream_base, 64),
	PVR_STREAM_DEF(rogue_fwif_cmd_geom, regs.tpu_border_colour_table, 64),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_geom, regs.vdm_draw_indirect0, 64,
		PVR_FEATURE_VDM_DRAWINDIRECT),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_geom, regs.vdm_draw_indirect1, 32,
		PVR_FEATURE_VDM_DRAWINDIRECT),
	PVR_STREAM_DEF(rogue_fwif_cmd_geom, regs.ppp_ctrl, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_geom, regs.te_psg, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_geom, regs.vdm_context_resume_task0_size,
		32),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_geom,
		regs.vdm_context_resume_task3_size, 32,
		PVR_FEATURE_VDM_OBJECT_LEVEL_LLS),
	PVR_STREAM_DEF(rogue_fwif_cmd_geom, regs.view_idx, 32),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_geom, regs.pds_coeff_free_prog, 32,
		PVR_FEATURE_TESSELLATION),
};

static const pvr_stream_def rogue_fwif_cmd_geom_stream_brn49927[] = {
	PVR_STREAM_DEF(rogue_fwif_cmd_geom, regs.tpu, 32),
};

static const pvr_stream_ext_def cmd_geom_ext_streams_0[] = {
	{
		rogue_fwif_cmd_geom_stream_brn49927,
		ARRAY_SIZE(rogue_fwif_cmd_geom_stream_brn49927),
		PVR_STREAM_EXTHDR_GEOM0_BRN49927,
		49927,
	},
};

static const pvr_stream_ext_header cmd_geom_ext_headers[] = {
	{
		cmd_geom_ext_streams_0,
		ARRAY_SIZE(cmd_geom_ext_streams_0),
		PVR_STREAM_EXTHDR_GEOM0_VALID,
	},
};

const pvr_stream_cmd_defs pvr_cmd_geom_stream = {
	PVR_STREAM_TYPE_GEOM,
	rogue_fwif_cmd_geom_stream,
	ARRAY_SIZE(rogue_fwif_cmd_geom_stream),
	ARRAY_SIZE(cmd_geom_ext_headers),
	cmd_geom_ext_headers,
	sizeof(struct rogue_fwif_cmd_geom),
};


static const pvr_stream_def rogue_fwif_cmd_frag_stream[] = {
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, regs.isp_scissor_base, 64),
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, regs.isp_dbias_base, 64),
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, regs.isp_oclqry_base, 64),
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, regs.isp_zlsctl, 64),
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, regs.isp_zload_store_base, 64),
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, regs.isp_stencil_load_store_base, 64),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_frag, regs.fb_cdc_zls, 64,
		PVR_FEATURE_REQUIRES_FB_CDC_ZLS_SETUP),
	PVR_STREAM_DEF_ARRAY(rogue_fwif_cmd_frag, regs.pbe_word),
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, regs.tpu_border_colour_table, 64),
	PVR_STREAM_DEF_ARRAY(rogue_fwif_cmd_frag, regs.pds_bgnd),
	PVR_STREAM_DEF_ARRAY(rogue_fwif_cmd_frag, regs.pds_pr_bgnd),
	PVR_STREAM_DEF_ARRAY(rogue_fwif_cmd_frag, regs.usc_clear_register),
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, regs.usc_pixel_output_ctrl, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, regs.isp_bgobjdepth, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, regs.isp_bgobjvals, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, regs.isp_aa, 32),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_frag, regs.isp_xtp_pipe_enable, 32,
		PVR_FEATURE_S7_TOP_INFRASTRUCTURE),
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, regs.isp_ctl, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, regs.event_pixel_pds_info, 32),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_frag, regs.pixel_phantom, 32,
		PVR_FEATURE_CLUSTER_GROUPING),
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, regs.view_idx, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, regs.event_pixel_pds_data, 32),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_frag, regs.isp_oclqry_stride, 32,
		PVR_FEATURE_GPU_MULTICORE_SUPPORT),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_frag, regs.isp_zls_pixels, 32,
		PVR_FEATURE_ZLS_SUBTILE),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_frag, regs.rgx_cr_blackpearl_fix, 32,
		PVR_FEATURE_ISP_ZLS_D24_S8_PACKING_OGL_MODE),
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, zls_stride, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, sls_stride, 32),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_frag, execute_count, 32,
		PVR_FEATURE_GPU_MULTICORE_SUPPORT),
};

static const pvr_stream_def rogue_fwif_cmd_frag_stream_brn47217[] = {
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, regs.isp_oclqry_stride, 32),
};

static const pvr_stream_def rogue_fwif_cmd_frag_stream_brn49927[] = {
	PVR_STREAM_DEF(rogue_fwif_cmd_frag, regs.tpu, 32),
};

static const pvr_stream_ext_def cmd_frag_ext_streams_0[] = {
	{
		rogue_fwif_cmd_frag_stream_brn47217,
		ARRAY_SIZE(rogue_fwif_cmd_frag_stream_brn47217),
		PVR_STREAM_EXTHDR_FRAG0_BRN47217,
		47217,
	},
	{
		rogue_fwif_cmd_frag_stream_brn49927,
		ARRAY_SIZE(rogue_fwif_cmd_frag_stream_brn49927),
		PVR_STREAM_EXTHDR_FRAG0_BRN49927,
		49927,
	},
};

static const pvr_stream_ext_header cmd_frag_ext_headers[] = {
	{
		cmd_frag_ext_streams_0,
		ARRAY_SIZE(cmd_frag_ext_streams_0),
		PVR_STREAM_EXTHDR_FRAG0_VALID,
	},
};

const pvr_stream_cmd_defs pvr_cmd_frag_stream = {
	PVR_STREAM_TYPE_FRAG,
	rogue_fwif_cmd_frag_stream,
	ARRAY_SIZE(rogue_fwif_cmd_frag_stream),
	ARRAY_SIZE(cmd_frag_ext_headers),
	cmd_frag_ext_headers,
	sizeof(struct rogue_fwif_cmd_frag),
};


static const pvr_stream_def rogue_fwif_cmd_compute_stream[] = {
	PVR_STREAM_DEF(rogue_fwif_cmd_compute, regs.tpu_border_colour_table, 64),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_compute, regs.cdm_cb_queue, 64,
		PVR_FEATURE_CDM_USER_MODE_QUEUE),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_compute, regs.cdm_cb_base, 64,
		PVR_FEATURE_CDM_USER_MODE_QUEUE),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_compute, regs.cdm_cb, 64,
		PVR_FEATURE_CDM_USER_MODE_QUEUE),
	PVR_STREAM_DEF_NOT_FEATURE(rogue_fwif_cmd_compute,
		regs.cdm_ctrl_stream_base, 64, PVR_FEATURE_CDM_USER_MODE_QUEUE),
	PVR_STREAM_DEF(rogue_fwif_cmd_compute, regs.cdm_context_state_base_addr,
		64),
	PVR_STREAM_DEF(rogue_fwif_cmd_compute, regs.cdm_resume_pds1, 32),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_compute, regs.cdm_item, 32,
		PVR_FEATURE_COMPUTE_MORTON_CAPABLE),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_compute, regs.compute_cluster, 32,
		PVR_FEATURE_CLUSTER_GROUPING),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_compute, regs.tpu_tag_cdm_ctrl, 32,
		PVR_FEATURE_TPU_DM_GLOBAL_REGISTERS),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_compute, stream_start_offset, 32,
		PVR_FEATURE_CDM_USER_MODE_QUEUE),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_compute, execute_count, 32,
		PVR_FEATURE_GPU_MULTICORE_SUPPORT),
};

static const pvr_stream_def rogue_fwif_cmd_compute_stream_brn49927[] = {
	PVR_STREAM_DEF(rogue_fwif_cmd_compute, regs.tpu, 32),
};

static const pvr_stream_ext_def cmd_compute_ext_streams_0[] = {
	{
		rogue_fwif_cmd_compute_stream_brn49927,
		ARRAY_SIZE(rogue_fwif_cmd_compute_stream_brn49927),
		PVR_STREAM_EXTHDR_COMPUTE0_BRN49927,
		49927,
	},
};

static const pvr_stream_ext_header cmd_compute_ext_headers[] = {
	{
		cmd_compute_ext_streams_0,
		ARRAY_SIZE(cmd_compute_ext_streams_0),
		PVR_STREAM_EXTHDR_COMPUTE0_VALID,
	},
};

const pvr_stream_cmd_defs pvr_cmd_compute_stream = {
	PVR_STREAM_TYPE_COMPUTE,
	rogue_fwif_cmd_compute_stream,
	ARRAY_SIZE(rogue_fwif_cmd_compute_stream),
	ARRAY_SIZE(cmd_compute_ext_headers),
	cmd_compute_ext_headers,
	sizeof(struct rogue_fwif_cmd_compute),
};


static const pvr_stream_def rogue_fwif_cmd_transfer_stream[] = {
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.pds_bgnd0_base, 64),
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.pds_bgnd1_base, 64),
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.pds_bgnd3_sizeinfo, 64),
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.isp_mtile_base, 64),
	PVR_STREAM_DEF_ARRAY(rogue_fwif_cmd_transfer, regs.pbe_wordx_mrty),
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.isp_bgobjvals, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.usc_pixel_output_ctrl, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.usc_clear_register0, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.usc_clear_register1, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.usc_clear_register2, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.usc_clear_register3, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.isp_mtile_size, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.isp_render_origin, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.isp_ctl, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.isp_aa, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.event_pixel_pds_info, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.event_pixel_pds_code, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.event_pixel_pds_data, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.isp_render, 32),
	PVR_STREAM_DEF(rogue_fwif_cmd_transfer, regs.isp_rgn, 32),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_transfer, regs.isp_xtp_pipe_enable,
		32, PVR_FEATURE_S7_TOP_INFRASTRUCTURE),
	PVR_STREAM_DEF_FEATURE(rogue_fwif_cmd_transfer, regs.frag_screen, 32,
		PVR_FEATURE_GPU_MULTICORE_SUPPORT),
};

const pvr_stream_cmd_defs pvr_cmd_transfer_stream = {
	PVR_STREAM_TYPE_TRANSFER,
	rogue_fwif_cmd_transfer_stream,
	ARRAY_SIZE(rogue_fwif_cmd_transfer_stream),
	0,
	NULL,
	sizeof(struct rogue_fwif_cmd_transfer),
};


#define GEOM_CASWITCH(member) \
	PVR_STREAM_DEF(rogue_fwif_geom_registers_caswitch, member, 64)

static const pvr_stream_def
	rogue_fwif_static_render_context_state_stream[] = {
	GEOM_CASWITCH(geom_reg_vdm_context_state_base_addr),
	GEOM_CASWITCH(geom_reg_vdm_context_state_resume_addr),
	GEOM_CASWITCH(geom_reg_ta_context_state_base_addr),
	GEOM_CASWITCH(geom_state[0].geom_reg_vdm_context_store_task0),
	GEOM_CASWITCH(geom_state[0].geom_reg_vdm_context_store_task1),
	GEOM_CASWITCH(geom_state[0].geom_reg_vdm_context_store_task2),
	GEOM_CASWITCH(geom_state[0].geom_reg_vdm_context_store_task3),
	GEOM_CASWITCH(geom_state[0].geom_reg_vdm_context_store_task4),
	GEOM_CASWITCH(geom_state[0].geom_reg_vdm_context_resume_task0),
	GEOM_CASWITCH(geom_state[0].geom_reg_vdm_context_resume_task1),
	GEOM_CASWITCH(geom_state[0].geom_reg_vdm_context_resume_task2),
	GEOM_CASWITCH(geom_state[0].geom_reg_vdm_context_resume_task3),
	GEOM_CASWITCH(geom_state[0].geom_reg_vdm_context_resume_task4),
	GEOM_CASWITCH(geom_state[1].geom_reg_vdm_context_store_task0),
	GEOM_CASWITCH(geom_state[1].geom_reg_vdm_context_store_task1),
	GEOM_CASWITCH(geom_state[1].geom_reg_vdm_context_store_task2),
	GEOM_CASWITCH(geom_state[1].geom_reg_vdm_context_store_task3),
	GEOM_CASWITCH(geom_state[1].geom_reg_vdm_context_store_task4),
	GEOM_CASWITCH(geom_state[1].geom_reg_vdm_context_resume_task0),
	GEOM_CASWITCH(geom_state[1].geom_reg_vdm_context_resume_task1),
	GEOM_CASWITCH(geom_state[1].geom_reg_vdm_context_resume_task2),
	GEOM_CASWITCH(geom_state[1].geom_reg_vdm_context_resume_task3),
	GEOM_CASWITCH(geom_state[1].geom_reg_vdm_context_resume_task4),
};

const pvr_stream_cmd_defs pvr_static_render_context_state_stream = {
	PVR_STREAM_TYPE_STATIC_RENDER_CONTEXT,
	rogue_fwif_static_render_context_state_stream,
	ARRAY_SIZE(rogue_fwif_static_render_context_state_stream),
	0,
	NULL,
	sizeof(struct rogue_fwif_geom_registers_caswitch),
};


#define CDM_CSWITCH(member) \
	PVR_STREAM_DEF(rogue_fwif_cdm_registers_cswitch, member, 64)

static const pvr_stream_def
	rogue_fwif_static_compute_context_state_stream[] = {
	CDM_CSWITCH(cdmreg_cdm_context_pds0),
	CDM_CSWITCH(cdmreg_cdm_context_pds1),
	CDM_CSWITCH(cdmreg_cdm_terminate_pds),
	CDM_CSWITCH(cdmreg_cdm_terminate_pds1),
	CDM_CSWITCH(cdmreg_cdm_resume_pds0),
	CDM_CSWITCH(cdmreg_cdm_context_pds0_b),
	CDM_CSWITCH(cdmreg_cdm_resume_pds0_b),
};

const pvr_stream_cmd_defs pvr_static_compute_context_state_stream = {
	PVR_STREAM_TYPE_STATIC_COMPUTE_CONTEXT,
	rogue_fwif_static_compute_context_state_stream,
	ARRAY_SIZE(rogue_fwif_static_compute_context_state_stream),
	0,
	NULL,
	sizeof(struct rogue_fwif_cdm_registers_cswitch),
};


//	#pragma mark - parser (pvr_stream.c)


static bool
stream_def_is_supported(const pvr_device_desc& desc,
	const pvr_stream_def* streamDef)
{
	if (streamDef->feature == PVR_FEATURE_NONE)
		return true;

	if ((streamDef->feature & PVR_FEATURE_NOT) == 0
		&& pvr_device_has_feature(desc, streamDef->feature)) {
		return true;
	}

	if ((streamDef->feature & PVR_FEATURE_NOT) != 0
		&& !pvr_device_has_feature(desc,
			streamDef->feature & ~PVR_FEATURE_NOT)) {
		return true;
	}

	return false;
}


/*!	pvr_stream_get_data(), with the additions and the alignment checked for
	overflow (Linux computes them in u32 unchecked).
*/
static status_t
pvr_stream_get_data(const uint8* stream, uint32* streamOffset,
	uint32 streamSize, uint32 dataSize, uint32 alignSize, void* dest)
{
	uint64 offset = ((uint64)*streamOffset + alignSize - 1)
		& ~(uint64)(alignSize - 1);

	if (offset + dataSize > streamSize)
		return B_BAD_VALUE;

	memcpy(dest, stream + offset, dataSize);
	*streamOffset = (uint32)(offset + dataSize);
	return B_OK;
}


/*!	pvr_stream_process_1(): one stream (main or one extension). */
static status_t
pvr_stream_process_1(const pvr_device_desc& desc,
	const pvr_stream_def* streamDef, uint32 entryCount, const uint8* stream,
	uint32 streamOffset, uint32 streamSize, uint8* dest, uint32 destSize,
	uint32* _streamOffset)
{
	status_t status = B_OK;

	for (uint32 i = 0; i < entryCount; i++) {
		if (streamDef[i].offset >= destSize) {
			status = B_BAD_VALUE;
			break;
		}

		if (!stream_def_is_supported(desc, &streamDef[i]))
			continue;

		uint32 size;
		uint32 align;
		switch (streamDef[i].size) {
			case PVR_STREAM_SIZE_8:
				size = align = sizeof(uint8);
				break;
			case PVR_STREAM_SIZE_16:
				size = align = sizeof(uint16);
				break;
			case PVR_STREAM_SIZE_32:
				size = align = sizeof(uint32);
				break;
			case PVR_STREAM_SIZE_64:
				size = align = sizeof(uint64);
				break;
			case PVR_STREAM_SIZE_ARRAY:
				size = streamDef[i].array_size;
				align = sizeof(uint64);
				break;
			default:
				return B_BAD_VALUE;
		}

		// not in Linux (the tables guarantee it): the field must fit
		if ((uint64)streamDef[i].offset + size > destSize)
			return B_BAD_VALUE;

		status = pvr_stream_get_data(stream, &streamOffset, streamSize, size,
			align, dest + streamDef[i].offset);
		if (status != B_OK)
			return status;
	}

	if (_streamOffset != NULL)
		*_streamOffset = streamOffset;

	return status;
}


/*!	pvr_stream_create_musthave_masks(), for one stream type. */
static void
musthave_masks(const pvr_device_desc& desc, pvr_stream_type type,
	uint32 masks[PVR_STREAM_EXTHDR_TYPE_MAX])
{
	memset(masks, 0, sizeof(uint32) * PVR_STREAM_EXTHDR_TYPE_MAX);

	if (pvr_device_has_uapi_quirk(desc, 47217)
		&& type == PVR_STREAM_TYPE_FRAG) {
		masks[0] |= PVR_STREAM_EXTHDR_FRAG0_BRN47217;
	}

	if (pvr_device_has_uapi_quirk(desc, 49927)) {
		if (type == PVR_STREAM_TYPE_GEOM)
			masks[0] |= PVR_STREAM_EXTHDR_GEOM0_BRN49927;
		else if (type == PVR_STREAM_TYPE_FRAG)
			masks[0] |= PVR_STREAM_EXTHDR_FRAG0_BRN49927;
		else if (type == PVR_STREAM_TYPE_COMPUTE)
			masks[0] |= PVR_STREAM_EXTHDR_COMPUTE0_BRN49927;
	}
}


static status_t
pvr_stream_process_ext_stream(const pvr_device_desc& desc,
	const pvr_stream_cmd_defs* cmdDefs, const uint8* extStream,
	uint32 streamOffset, uint32 extStreamSize, void* dest)
{
	uint32 masks[PVR_STREAM_EXTHDR_TYPE_MAX];
	uint32 extHeader;

	// "must have" masks of the device, cleared while processing the stream
	musthave_masks(desc, cmdDefs->type, masks);

	do {
		status_t status = pvr_stream_get_data(extStream, &streamOffset,
			extStreamSize, sizeof(uint32), sizeof(extHeader), &extHeader);
		if (status != B_OK)
			return status;

		uint32 type = (extHeader & PVR_STREAM_EXTHDR_TYPE_MASK)
			>> PVR_STREAM_EXTHDR_TYPE_SHIFT;
		uint32 data = extHeader & PVR_STREAM_EXTHDR_DATA_MASK;

		if (type >= cmdDefs->ext_nr_headers)
			return B_BAD_VALUE;

		const pvr_stream_ext_header* header = &cmdDefs->ext_headers[type];
		if ((data & ~header->valid_mask) != 0)
			return B_BAD_VALUE;

		masks[type] &= ~data;

		for (uint32 i = 0; i < header->ext_streams_num; i++) {
			const pvr_stream_ext_def* extDef = &header->ext_streams[i];

			if ((extHeader & extDef->header_mask) == 0)
				continue;

			if (!pvr_device_has_uapi_quirk(desc, extDef->quirk))
				return B_BAD_VALUE;

			status = pvr_stream_process_1(desc, extDef->stream,
				extDef->stream_len, extStream, streamOffset, extStreamSize,
				(uint8*)dest, cmdDefs->dest_size, &streamOffset);
			if (status != B_OK)
				return status;
		}
	} while ((extHeader & PVR_STREAM_EXTHDR_CONTINUATION) != 0);

	// one of the "must have" quirks of this command was not present
	for (uint32 i = 0; i < cmdDefs->ext_nr_headers; i++) {
		if (masks[i] != 0)
			return B_BAD_VALUE;
	}

	return B_OK;
}


status_t
pvr_stream_process(const pvr_device_desc& desc,
	const pvr_stream_cmd_defs* cmdDefs, const void* _stream,
	uint32 streamSize, void* dest)
{
	const uint8* stream = (const uint8*)_stream;
	uint32 streamOffset = 0;
	uint32 mainStreamLen;
	uint32 padding;

	if (stream == NULL || streamSize == 0 || cmdDefs == NULL || dest == NULL
		|| cmdDefs->ext_nr_headers > PVR_STREAM_EXTHDR_TYPE_MAX)
		return B_BAD_VALUE;

	status_t status = pvr_stream_get_data(stream, &streamOffset, streamSize,
		sizeof(uint32), sizeof(uint32), &mainStreamLen);
	if (status != B_OK)
		return status;

	// the u32 after the length is padding (reserved): it must be zero
	status = pvr_stream_get_data(stream, &streamOffset, streamSize,
		sizeof(uint32), sizeof(uint32), &padding);
	if (status != B_OK)
		return status;

	if (mainStreamLen < streamOffset || mainStreamLen > streamSize
		|| padding != 0)
		return B_BAD_VALUE;

	status = pvr_stream_process_1(desc, cmdDefs->main_stream,
		cmdDefs->main_stream_len, stream, streamOffset, mainStreamLen,
		(uint8*)dest, cmdDefs->dest_size, &streamOffset);
	if (status != B_OK)
		return status;

	// as Linux: the extension stream starts where the parsed main stream
	// data ends
	if (streamOffset < streamSize) {
		return pvr_stream_process_ext_stream(desc, cmdDefs, stream,
			streamOffset, streamSize, dest);
	}

	// without an extension stream there must not be any "must have" quirk
	uint32 masks[PVR_STREAM_EXTHDR_TYPE_MAX];
	musthave_masks(desc, cmdDefs->type, masks);
	for (uint32 i = 0; i < cmdDefs->ext_nr_headers; i++) {
		if (masks[i] != 0)
			return B_BAD_VALUE;
	}

	return B_OK;
}


//	#pragma mark - typed wrappers


static status_t
parse_zeroed(const pvr_device_desc& desc, const pvr_stream_cmd_defs* defs,
	const void* stream, uint32 streamSize, void* dest)
{
	memset(dest, 0, defs->dest_size);
	return pvr_stream_process(desc, defs, stream, streamSize, dest);
}


status_t
pvr_stream_parse_geom(const pvr_device_desc& desc, const void* stream,
	uint32 streamSize, rogue_fwif_cmd_geom* cmd)
{
	return parse_zeroed(desc, &pvr_cmd_geom_stream, stream, streamSize, cmd);
}


status_t
pvr_stream_parse_frag(const pvr_device_desc& desc, const void* stream,
	uint32 streamSize, rogue_fwif_cmd_frag* cmd)
{
	return parse_zeroed(desc, &pvr_cmd_frag_stream, stream, streamSize, cmd);
}


status_t
pvr_stream_parse_compute(const pvr_device_desc& desc, const void* stream,
	uint32 streamSize, rogue_fwif_cmd_compute* cmd)
{
	return parse_zeroed(desc, &pvr_cmd_compute_stream, stream, streamSize,
		cmd);
}


status_t
pvr_stream_parse_transfer(const pvr_device_desc& desc, const void* stream,
	uint32 streamSize, rogue_fwif_cmd_transfer* cmd)
{
	return parse_zeroed(desc, &pvr_cmd_transfer_stream, stream, streamSize,
		cmd);
}


status_t
pvr_stream_parse_static_render_state(const pvr_device_desc& desc,
	const void* stream, uint32 streamSize,
	rogue_fwif_geom_registers_caswitch* regs)
{
	return parse_zeroed(desc, &pvr_static_render_context_state_stream, stream,
		streamSize, regs);
}


status_t
pvr_stream_parse_static_compute_state(const pvr_device_desc& desc,
	const void* stream, uint32 streamSize,
	rogue_fwif_cdm_registers_cswitch* regs)
{
	return parse_zeroed(desc, &pvr_static_compute_context_state_stream,
		stream, streamSize, regs);
}
