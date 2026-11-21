/*
 * Copyright 2018-2021 Intel Corporation. All rights reserved.
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the BSD 3-Clause license.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The register layout, the firmware image format and the boot sequence follow
 * the Linux Sound Open Firmware driver (sound/soc/sof/intel/atom.h, shim.h,
 * byt.c, atom.c and include/uapi/sound/sof/fw.h), which Intel provides under
 * either the GPL 2.0 or the BSD 3-Clause license.
 */
#ifndef INTEL_LPE_SOF_DSP_H
#define INTEL_LPE_SOF_DSP_H


#include <OS.h>


// DSP memory map in the 2 MB LPE BAR (atom.h)
#define LPE_BAR_SIZE				0x200000
#define LPE_IRAM_OFFSET				0x0c0000
#define LPE_IRAM_SIZE				(80 * 1024)
#define LPE_DRAM_OFFSET				0x100000
#define LPE_DRAM_SIZE				(160 * 1024)
#define LPE_SHIM_OFFSET				0x140000
#define LPE_MBOX_OFFSET				0x144000
#define LPE_MBOX_SIZE				0x1000

// shim registers, 64 bit (shim.h)
#define SHIM_CSR					0x00
#define SHIM_IMRX					0x28
#define SHIM_IMRD					0x30
#define SHIM_IPCX					0x38
#define SHIM_IPCD					0x40

#define SHIM_BYT_CSR_RST			(1 << 0)
#define SHIM_BYT_CSR_VECTOR_SEL		(1 << 1)
#define SHIM_BYT_CSR_STALL			(1 << 2)
#define SHIM_BYT_CSR_PWAITMODE		(1 << 3)

#define SHIM_IMRX_DONE				(1 << 0)
#define SHIM_IMRX_BUSY				(1 << 1)

#define SHIM_BYT_IPC_DONE			(1ULL << 62)
#define SHIM_BYT_IPC_BUSY			(1ULL << 63)

// PCI configuration shadow (the second ACPI memory resource): power state
#define LPE_PCI_PMCSR				0x84

// IPC3 (include/sound/sof/header.h, info.h, topology.h, stream.h, dai.h,
// dai-intel.h)
#define SOF_GLB_TYPE_MASK			(0xfU << 28)
#define SOF_CMD_TYPE_MASK			(0xfffU << 16)
#define SOF_IPC_GLB_REPLY			(0x1U << 28)
#define SOF_IPC_GLB_TPLG_MSG		(0x3U << 28)
#define SOF_IPC_GLB_STREAM_MSG		(0x6U << 28)
#define SOF_IPC_FW_READY			(0x7U << 28)
#define SOF_IPC_GLB_DAI_MSG			(0x8U << 28)
#define SOF_IPC_GLB_TRACE_MSG		(0x9U << 28)

#define SOF_IPC_TPLG_COMP_NEW		(0x001U << 16)
#define SOF_IPC_TPLG_COMP_CONNECT	(0x003U << 16)
#define SOF_IPC_TPLG_PIPE_NEW		(0x010U << 16)
#define SOF_IPC_TPLG_PIPE_FREE		(0x011U << 16)
#define SOF_IPC_TPLG_PIPE_COMPLETE	(0x013U << 16)
#define SOF_IPC_TPLG_BUFFER_NEW		(0x020U << 16)
#define SOF_IPC_DAI_CONFIG			(0x001U << 16)
#define SOF_IPC_STREAM_PCM_PARAMS	(0x001U << 16)
#define SOF_IPC_STREAM_PCM_FREE		(0x003U << 16)
#define SOF_IPC_STREAM_TRIG_START	(0x004U << 16)
#define SOF_IPC_STREAM_TRIG_STOP	(0x005U << 16)
#define SOF_IPC_STREAM_POSITION		(0x00aU << 16)

#define SOF_IPC_PANIC_MAGIC			0x0dead000
#define SOF_IPC_PANIC_MAGIC_MASK	0x0ffff000

#define SOF_IPC_EXT_WINDOW			1
#define SOF_IPC_REGION_DOWNBOX		0
#define SOF_IPC_REGION_UPBOX		1
#define SOF_IPC_REGION_STREAM		4
#define SOF_IPC_REGION_EXCEPTION	6

#define SOF_COMP_HOST				1
#define SOF_COMP_DAI				2
#define SOF_COMP_BUFFER				12

#define SOF_MEM_CAPS_RAM			(1 << 0)
#define SOF_MEM_CAPS_DMA			(1 << 5)
#define SOF_MEM_CAPS_CACHE			(1 << 6)

#define SOF_TIME_DOMAIN_DMA			0
#define SOF_IPC_STREAM_PLAYBACK		0
#define SOF_IPC_FRAME_S16_LE		0
#define SOF_IPC_FRAME_S24_4LE		1
#define SOF_IPC_BUFFER_INTERLEAVED	0

#define SOF_DAI_INTEL_SSP			1
#define SOF_DAI_FMT_I2S				1
#define SOF_DAI_FMT_CONT			(1 << 4)
#define SOF_DAI_FMT_NB_NF			(0 << 8)
#define SOF_DAI_FMT_CBC_CFC			(4 << 12)

#define SOF_IPC_MAX_CHANNELS		8


struct lpe_dsp {
	phys_addr_t		physicalBase;
	size_t			length;
	phys_addr_t		pciBase;
	uint32			irq;

	area_id			area;
	uint8*			registers;
	area_id			pciArea;
	uint8*			pci;

	bool			ready;
	uint32			hostBoxOffset;
	uint32			hostBoxSize;
	uint32			dspBoxOffset;
	uint32			dspBoxSize;
	uint32			streamBoxOffset;
	uint32			exceptionOffset;
};


status_t lpe_dsp_init(lpe_dsp* dsp);
void lpe_dsp_uninit(lpe_dsp* dsp);
status_t lpe_dsp_boot(lpe_dsp* dsp, const uint8* image, size_t size);
void lpe_dsp_poll_messages(lpe_dsp* dsp);


#endif	// INTEL_LPE_SOF_DSP_H
