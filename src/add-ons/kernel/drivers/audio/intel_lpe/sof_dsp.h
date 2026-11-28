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


#include <KernelExport.h>
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

#define SOF_ABI_MAJOR				3

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


struct sof_ipc_cmd_hdr {
	uint32	size;
	uint32	cmd;
};

struct sof_ipc_reply {
	sof_ipc_cmd_hdr	hdr;
	int32	error;
};

struct sof_ipc_comp {
	sof_ipc_cmd_hdr	hdr;
	uint32	id;
	uint32	type;
	uint32	pipeline_id;
	uint32	core;
	uint32	ext_data_length;
};

struct sof_ipc_comp_config {
	sof_ipc_cmd_hdr	hdr;
	uint32	periods_sink;
	uint32	periods_source;
	uint32	reserved1;
	uint32	frame_fmt;
	uint32	xrun_action;
	uint32	reserved[2];
};

struct sof_ipc_buffer {
	sof_ipc_comp comp;
	uint32	size;
	uint32	caps;
	uint32	flags;
	uint32	reserved;
};

struct sof_ipc_comp_host {
	sof_ipc_comp comp;
	sof_ipc_comp_config config;
	uint32	direction;
	uint32	no_irq;
	uint32	dmac_config;
};

struct sof_ipc_comp_dai {
	sof_ipc_comp comp;
	sof_ipc_comp_config config;
	uint32	direction;
	uint32	dai_index;
	uint32	type;
	uint32	reserved;
};

struct sof_ipc_pipe_new {
	sof_ipc_cmd_hdr	hdr;
	uint32	comp_id;
	uint32	pipeline_id;
	uint32	sched_id;
	uint32	core;
	uint32	period;
	uint32	priority;
	uint32	period_mips;
	uint32	frames_per_sched;
	uint32	xrun_limit_usecs;
	uint32	time_domain;
};

struct sof_ipc_pipe_comp_connect {
	sof_ipc_cmd_hdr	hdr;
	uint32	source_id;
	uint32	sink_id;
};

struct sof_ipc_pipe_ready {
	sof_ipc_cmd_hdr	hdr;
	uint32	comp_id;
};

struct sof_ipc_dai_ssp_params {
	uint32	hdr_size;
	uint16	reserved1;
	uint16	mclk_id;
	uint32	mclk_rate;
	uint32	fsync_rate;
	uint32	bclk_rate;
	uint32	tdm_slots;
	uint32	rx_slots;
	uint32	tx_slots;
	uint32	sample_valid_bits;
	uint16	tdm_slot_width;
	uint16	reserved2;
	uint32	mclk_direction;
	uint16	frame_pulse_width;
	uint16	tdm_per_slot_padding_flag;
	uint32	clks_control;
	uint32	quirks;
	uint32	bclk_delay;
};

struct sof_ipc_dai_config {
	sof_ipc_cmd_hdr	hdr;
	uint32	type;
	uint32	dai_index;
	uint16	format;
	uint8	group_id;
	uint8	flags;
	uint32	reserved[8];
	sof_ipc_dai_ssp_params ssp;
	uint8	union_padding[104];
		// the union of all DAI types is 164 bytes
};

struct sof_ipc_host_buffer {
	uint32	hdr_size;
	uint32	phy_addr;
	uint32	pages;
	uint32	size;
	uint32	reserved[3];
};

struct sof_ipc_stream_params {
	uint32	hdr_size;
	sof_ipc_host_buffer buffer;
	uint32	direction;
	uint32	frame_fmt;
	uint32	buffer_fmt;
	uint32	rate;
	uint16	stream_tag;
	uint16	channels;
	uint16	sample_valid_bytes;
	uint16	sample_container_bytes;
	uint32	host_period_bytes;
	uint16	no_stream_position;
	uint8	cont_update_posn;
	uint8	reserved0;
	int16	ext_data_length;
	uint8	reserved[2];
	uint16	chmap[SOF_IPC_MAX_CHANNELS];
};

struct sof_ipc_pcm_params {
	sof_ipc_cmd_hdr	hdr;
	uint32	comp_id;
	uint32	flags;
	uint32	reserved[2];
	sof_ipc_stream_params params;
};

struct sof_ipc_pcm_params_reply {
	sof_ipc_reply	rhdr;
	uint32	comp_id;
	uint32	posn_offset;
};

struct sof_ipc_stream {
	sof_ipc_cmd_hdr	hdr;
	uint32	comp_id;
};

struct sof_ipc_stream_posn {
	sof_ipc_reply	rhdr;
	uint32	comp_id;
	uint32	flags;
	uint32	wallclock_hz;
	uint32	timestamp_ns;
	uint64	host_posn;
	uint64	dai_posn;
	uint64	comp_posn;
	uint64	wallclock;
	uint64	timestamp;
	uint32	xrun_comp_id;
	int32	xrun_size;
} _PACKED;


// stream directions, as SOF numbers them
#define LPE_PLAYBACK				0
#define LPE_CAPTURE					1
#define LPE_STREAM_COUNT			2


// Called from the interrupt handler when the DSP reports that a stream
// finished a period.
typedef void (*lpe_position_hook)(void* cookie, uint32 direction);

struct lpe_dsp {
	phys_addr_t		physicalBase;
	size_t			length;
	phys_addr_t		pciBase;
	uint32			irq;
	bool			irqLevel;
	bool			irqActiveLow;

	area_id			area;
	uint8*			registers;
	area_id			pciArea;
	uint8*			pci;

	bool			ready;
	bool			unsupported;
	uint32			hostBoxOffset;
	uint32			hostBoxSize;
	uint32			dspBoxOffset;
	uint32			dspBoxSize;
	uint32			streamBoxOffset;
	uint32			exceptionOffset;

	bool			pipelineCreated[LPE_STREAM_COUNT];
	uint32			positionOffset[LPE_STREAM_COUNT];

	// after the boot, the doorbells raise interrupts instead of being polled
	bool			interruptsEnabled;
	spinlock		lock;
	sem_id			replySem;
	lpe_position_hook positionHook;
	void*			hookCookie;
};

// the host buffer of a stream: a page table (one page) followed by
// the audio pages, all below 4 GB
struct lpe_stream_buffer {
	phys_addr_t		pageTable;
	uint32			pageCount;
	uint32			size;
	uint32			periodBytes;
};


status_t lpe_dsp_init(lpe_dsp* dsp);
void lpe_dsp_uninit(lpe_dsp* dsp);
status_t lpe_dsp_boot(lpe_dsp* dsp, const uint8* image, size_t size);
void lpe_dsp_poll_messages(lpe_dsp* dsp);

status_t lpe_dsp_start_stream(lpe_dsp* dsp, uint32 direction,
	const lpe_stream_buffer* buffer);
status_t lpe_dsp_stop_stream(lpe_dsp* dsp, uint32 direction);
status_t lpe_dsp_read_position(lpe_dsp* dsp, uint32 direction,
	sof_ipc_stream_posn* position);
void lpe_fill_page_table(uint8* table, const phys_addr_t* pages,
	uint32 count);


#endif	// INTEL_LPE_SOF_DSP_H
