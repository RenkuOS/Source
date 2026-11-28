/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright (c) 2023 Imagination Technologies Ltd. */
/*
 * Subset of the firmware interface (FWIF) of the PowerVR Rogue firmware,
 * ported from the Linux drm/imagination driver: pvr_rogue_fwif.h,
 * pvr_rogue_fwif_common.h, pvr_rogue_fwif_shared.h, pvr_rogue_defs.h,
 * pvr_rogue_heap_config.h and pvr_fw.h. Only what is needed to build the
 * structures the firmware reads at boot (sysinit, osinit and what they point
 * to) is kept; the layouts are unchanged and checked by
 * pvr_rogue_fwif_check.h.
 *
 * Changes from Linux: the host pointer rogue_fwif_tracebuf_space::trace_buffer
 * is an aligned_u64 (same size and offset on a 64 bit host), the
 * SUPPORT_ROGUE_FW_STATS_FRAMEWORK block of rogue_fwif_sysdata (not enabled in
 * Linux either) is dropped, and the KCCB/FWCCB command unions are opaque
 * blocks of their checked size (the payloads used by pvr_mmu/ are declared
 * separately and written into that block).
 *
 * The context, client CCB and compute command definitions at the end come
 * from pvr_rogue_fwif.h, pvr_rogue_fwif_shared.h and pvr_rogue_fwif_client.h,
 * followed by the render context, free list, HWRT data, FWCCB request and
 * geometry/fragment/transfer command definitions used by pvr_render.cpp and
 * pvr_stream.cpp.
 */
#ifndef PVR_ROGUE_FWIF_H
#define PVR_ROGUE_FWIF_H


#include "linux_compat.h"


/* pvr_rogue_defs.h */
#define ROGUE_FW_MAX_NUM_OS (8U)

/* pvr_rogue_heap_config.h */
#define ROGUE_PDSCODEDATA_HEAP_BASE 0xDA00000000ull
#define ROGUE_USCCODE_HEAP_BASE 0xE000000000ull
#define ROGUE_FW_HEAP_BASE 0xE1C0000000ull

/* pvr_fw.h */
#define ROGUE_FWIF_FWCCB_NUMCMDS_LOG2 5
#define ROGUE_FWIF_KCCB_NUMCMDS_LOG2_DEFAULT 7


/*
 * pvr_rogue_fwif_common.h
 */
#define PVR_FW_ALIGNMENT_LSB 7U

#define PVR_FW_STRUCT_SIZE_ASSERT(_a) \
	static_assert((sizeof(_a) & PVR_FW_ALIGNMENT_LSB) == 0U, \
		"Size of " #_a " is not properly aligned")

#define PVR_FWIF_DM_GP (0)
#define PVR_FWIF_DM_2D (1)
#define PVR_FWIF_DM_TDM (1)
#define PVR_FWIF_DM_GEOM (2)
#define PVR_FWIF_DM_FRAG (3)
#define PVR_FWIF_DM_CDM (4)
#define PVR_FWIF_DM_RAY (5)
#define PVR_FWIF_DM_GEOM2 (6)
#define PVR_FWIF_DM_GEOM3 (7)
#define PVR_FWIF_DM_GEOM4 (8)
#define PVR_FWIF_DM_LAST PVR_FWIF_DM_GEOM4
#define PVR_FWIF_DM_MAX (PVR_FWIF_DM_LAST + 1U)

#define PVR_FWIF_GPU_UTIL_STATE_IDLE 0U
#define PVR_FWIF_GPU_UTIL_STATE_ACTIVE 1U
#define PVR_FWIF_GPU_UTIL_STATE_BLOCKED 2U
#define PVR_FWIF_GPU_UTIL_STATE_NUM 3U
#define PVR_FWIF_GPU_UTIL_STATE_MASK 0x3ULL


/*
 * pvr_rogue_fwif_shared.h
 */
struct rogue_fwif_dma_addr {
	aligned_u64 dev_addr;
	u32 fw_addr;
	u32 padding;
} __aligned(8);


/*
 * pvr_rogue_fwif.h: logging type
 */
#define ROGUE_FWIF_LOG_TYPE_NONE 0x00000000U
#define ROGUE_FWIF_LOG_TYPE_TRACE 0x00000001U
#define ROGUE_FWIF_LOG_TYPE_GROUP_MAIN 0x00000002U
#define ROGUE_FWIF_LOG_TYPE_GROUP_MTS 0x00000004U
#define ROGUE_FWIF_LOG_TYPE_GROUP_CLEANUP 0x00000008U
#define ROGUE_FWIF_LOG_TYPE_GROUP_CSW 0x00000010U
#define ROGUE_FWIF_LOG_TYPE_GROUP_BIF 0x00000020U
#define ROGUE_FWIF_LOG_TYPE_GROUP_PM 0x00000040U
#define ROGUE_FWIF_LOG_TYPE_GROUP_RTD 0x00000080U
#define ROGUE_FWIF_LOG_TYPE_GROUP_SPM 0x00000100U
#define ROGUE_FWIF_LOG_TYPE_GROUP_POW 0x00000200U
#define ROGUE_FWIF_LOG_TYPE_GROUP_HWR 0x00000400U
#define ROGUE_FWIF_LOG_TYPE_GROUP_HWP 0x00000800U
#define ROGUE_FWIF_LOG_TYPE_GROUP_RPM 0x00001000U
#define ROGUE_FWIF_LOG_TYPE_GROUP_DMA 0x00002000U
#define ROGUE_FWIF_LOG_TYPE_GROUP_MISC 0x00004000U
#define ROGUE_FWIF_LOG_TYPE_GROUP_DEBUG 0x80000000U
#define ROGUE_FWIF_LOG_TYPE_GROUP_MASK 0x80007FFEU
#define ROGUE_FWIF_LOG_TYPE_MASK 0x80007FFFU


/*
 * Trace buffer
 */
#define ROGUE_FW_TRACE_BUF_DEFAULT_SIZE_IN_DWORDS 12000U
#define ROGUE_FW_TRACE_BUFFER_ASSERT_SIZE 200U
#define ROGUE_FW_THREAD_NUM 1U
#define ROGUE_FW_THREAD_MAX 2U

struct rogue_fwif_file_info_buf {
	char path[ROGUE_FW_TRACE_BUFFER_ASSERT_SIZE];
	char info[ROGUE_FW_TRACE_BUFFER_ASSERT_SIZE];
	u32 line_num;
	u32 padding;
} __aligned(8);

struct rogue_fwif_tracebuf_space {
	u32 trace_pointer;

	u32 trace_buffer_fw_addr;

	/*
	 * To be used by host when reading from trace buffer (a host pointer,
	 * u32 * in Linux; kept as a 64 bit field so the layout does not depend
	 * on the host pointer size)
	 */
	aligned_u64 trace_buffer;

	struct rogue_fwif_file_info_buf assert_buf;
} __aligned(8);

/* Total number of FW fault logs stored */
#define ROGUE_FWIF_FWFAULTINFO_MAX (8U)

struct rogue_fw_fault_info {
	aligned_u64 cr_timer;
	aligned_u64 os_timer;

	u32 data __aligned(8);
	u32 reserved;
	struct rogue_fwif_file_info_buf fault_buf;
} __aligned(8);

enum rogue_fwif_pow_state {
	ROGUE_FWIF_POW_OFF, /* idle and ready to full power down */
	ROGUE_FWIF_POW_ON, /* running HW commands */
	ROGUE_FWIF_POW_FORCED_IDLE, /* forced idle */
	ROGUE_FWIF_POW_IDLE, /* idle waiting for host handshake */
};

/* Firmware's connection state */
enum rogue_fwif_connection_fw_state {
	ROGUE_FW_CONNECTION_FW_OFFLINE = 0,
	ROGUE_FW_CONNECTION_FW_READY,
	ROGUE_FW_CONNECTION_FW_ACTIVE,
	ROGUE_FW_CONNECTION_FW_OFFLOADING,
	ROGUE_FW_CONNECTION_FW_STATE_COUNT
};

/* OS' connection state */
enum rogue_fwif_connection_os_state {
	ROGUE_FW_CONNECTION_OS_OFFLINE = 0,
	ROGUE_FW_CONNECTION_OS_READY,
	ROGUE_FW_CONNECTION_OS_ACTIVE,
	ROGUE_FW_CONNECTION_OS_STATE_COUNT
};

struct rogue_fwif_os_runtime_flags {
	unsigned int os_state : 3;
	unsigned int fl_ok : 1;
	unsigned int fl_grow_pending : 1;
	unsigned int isolated_os : 1;
	unsigned int reserved : 26;
};

#define PVR_SLR_LOG_ENTRIES 10
#define PVR_SLR_LOG_STRLEN 30

struct rogue_fwif_slr_entry {
	aligned_u64 timestamp;
	u32 fw_ctx_addr;
	u32 num_ufos;
	char ccb_name[PVR_SLR_LOG_STRLEN];
	char padding[2];
} __aligned(8);

#define MAX_THREAD_NUM 2

/* firmware trace control data */
struct rogue_fwif_tracebuf {
	u32 log_type;
	struct rogue_fwif_tracebuf_space tracebuf[MAX_THREAD_NUM];
	u32 tracebuf_size_in_dwords;
	/* Compatibility and other flags */
	u32 tracebuf_flags;
} __aligned(8);

/* firmware system data shared with the Host driver */
struct rogue_fwif_sysdata {
	/* Configuration flags from host */
	u32 config_flags;
	/* Extended configuration flags from host */
	u32 config_flags_ext;
	enum rogue_fwif_pow_state pow_state;
	u32 hw_perf_ridx;
	u32 hw_perf_widx;
	u32 hw_perf_wrap_count;
	u32 hw_perf_size;
	u32 hw_perf_drop_count;
	u32 hw_perf_ut;
	u32 first_drop_ordinal;
	u32 last_drop_ordinal;
	struct rogue_fwif_os_runtime_flags
		os_runtime_flags_mirror[ROGUE_FW_MAX_NUM_OS];

	struct rogue_fw_fault_info fault_info[ROGUE_FWIF_FWFAULTINFO_MAX];
	u32 fw_faults;
	u32 cr_poll_addr[MAX_THREAD_NUM];
	u32 cr_poll_mask[MAX_THREAD_NUM];
	u32 cr_poll_count[MAX_THREAD_NUM];
	aligned_u64 start_idle_time;

	/* (SUPPORT_ROGUE_FW_STATS_FRAMEWORK block not supported, as in Linux) */
	u32 hwr_state_flags;
	u32 hwr_recovery_flags[PVR_FWIF_DM_MAX];
	/* Compatibility and other flags */
	u32 fw_sys_data_flags;
	/* Identify whether MC config is P-P or P-S */
	u32 mc_config;
} __aligned(8);

/* per-os firmware shared data */
struct rogue_fwif_osdata {
	u32 fw_os_config_flags;
	u32 fw_sync_check_mark;
	u32 host_sync_check_mark;

	u32 forced_updates_requested;
	u8 slr_log_wp;
	struct rogue_fwif_slr_entry slr_log_first;
	struct rogue_fwif_slr_entry slr_log[PVR_SLR_LOG_ENTRIES];
	aligned_u64 last_forced_update_time;

	u32 interrupt_count[MAX_THREAD_NUM];
	u32 kccb_cmds_executed;
	u32 power_sync_fw_addr;
	u32 fw_os_data_flags;
	u32 padding;
} __aligned(8);


/*
 * HWR info
 */
enum rogue_hwrtype {
	ROGUE_HWRTYPE_UNKNOWNFAILURE = 0,
	ROGUE_HWRTYPE_OVERRUN = 1,
	ROGUE_HWRTYPE_POLLFAILURE = 2,
	ROGUE_HWRTYPE_BIF0FAULT = 3,
	ROGUE_HWRTYPE_BIF1FAULT = 4,
	ROGUE_HWRTYPE_TEXASBIF0FAULT = 5,
	ROGUE_HWRTYPE_MMUFAULT = 6,
	ROGUE_HWRTYPE_MMUMETAFAULT = 7,
	ROGUE_HWRTYPE_MIPSTLBFAULT = 8,
	ROGUE_HWRTYPE_ECCFAULT = 9,
	ROGUE_HWRTYPE_MMURISCVFAULT = 10,
};

struct rogue_bifinfo {
	aligned_u64 bif_req_status;
	aligned_u64 bif_mmu_status;
	aligned_u64 pc_address;
	aligned_u64 reserved;
};

struct rogue_eccinfo {
	u32 fault_gpu;
};

struct rogue_mmuinfo {
	aligned_u64 mmu_status[2];
	aligned_u64 pc_address;
	aligned_u64 reserved;
};

struct rogue_pollinfo {
	u32 thread_num;
	u32 cr_poll_addr;
	u32 cr_poll_mask;
	u32 cr_poll_last_value;
	aligned_u64 reserved;
} __aligned(8);

struct rogue_tlbinfo {
	u32 bad_addr;
	u32 entry_lo;
};

struct rogue_hwrinfo {
	union {
		struct rogue_bifinfo bif_info;
		struct rogue_mmuinfo mmu_info;
		struct rogue_pollinfo poll_info;
		struct rogue_tlbinfo tlb_info;
		struct rogue_eccinfo ecc_info;
	} hwr_data;

	aligned_u64 cr_timer;
	aligned_u64 os_timer;
	u32 frame_num;
	u32 pid;
	u32 active_hwrt_data;
	u32 hwr_number;
	u32 event_status;
	u32 hwr_recovery_flags;
	enum rogue_hwrtype hwr_type;
	u32 dm;
	u32 core_id;
	aligned_u64 cr_time_of_kick;
	aligned_u64 cr_time_hw_reset_start;
	aligned_u64 cr_time_hw_reset_finish;
	aligned_u64 cr_time_freelist_ready;
	aligned_u64 reserved[2];
} __aligned(8);

#define ROGUE_FWIF_HWINFO_MAX_FIRST 8U
#define ROGUE_FWIF_HWINFO_MAX_LAST 8U
#define ROGUE_FWIF_HWINFO_MAX \
	(ROGUE_FWIF_HWINFO_MAX_FIRST + ROGUE_FWIF_HWINFO_MAX_LAST)
#define ROGUE_FWIF_HWINFO_LAST_INDEX (ROGUE_FWIF_HWINFO_MAX - 1U)

struct rogue_fwif_hwrinfobuf {
	struct rogue_hwrinfo hwr_info[ROGUE_FWIF_HWINFO_MAX];
	u32 hwr_counter;
	u32 write_index;
	u32 dd_req_count;
	u32 hwr_info_buf_flags;
	u32 hwr_dm_locked_up_count[PVR_FWIF_DM_MAX];
	u32 hwr_dm_overran_count[PVR_FWIF_DM_MAX];
	u32 hwr_dm_recovered_count[PVR_FWIF_DM_MAX];
	u32 hwr_dm_false_detect_count[PVR_FWIF_DM_MAX];
} __aligned(8);


/*
 * Init configuration flags (rogue_fwif_sysdata::config_flags)
 */
#define ROGUE_FWIF_INICFG_CTXSWITCH_MODE_RAND BIT(0)
#define ROGUE_FWIF_INICFG_CTXSWITCH_SRESET_EN BIT(1)
#define ROGUE_FWIF_INICFG_HWPERF_EN BIT(2)
#define ROGUE_FWIF_INICFG_DM_KILL_MODE_RAND_EN BIT(3)
#define ROGUE_FWIF_INICFG_POW_RASCALDUST BIT(4)
#define ROGUE_FWIF_INICFG_FBCDC_V3_1_EN BIT(6)
#define ROGUE_FWIF_INICFG_CHECK_MLIST_EN BIT(7)
#define ROGUE_FWIF_INICFG_DISABLE_CLKGATING_EN BIT(8)
#define ROGUE_FWIF_INICFG_REGCONFIG_EN BIT(12)
#define ROGUE_FWIF_INICFG_ASSERT_ON_OUTOFMEMORY BIT(13)
#define ROGUE_FWIF_INICFG_HWP_DISABLE_FILTER BIT(14)
#define ROGUE_FWIF_INICFG_DISABLE_DM_OVERLAP BIT(19)
#define ROGUE_FWIF_INICFG_ASSERT_ON_HWR_TRIGGER BIT(20)
#define ROGUE_FWIF_INICFG_FABRIC_COHERENCY_ENABLED BIT(21)
#define ROGUE_FWIF_INICFG_VALIDATE_IRQ BIT(22)
#define ROGUE_FWIF_INICFG_DISABLE_PDP_EN BIT(23)
#define ROGUE_FWIF_INICFG_SPU_POWER_STATE_MASK_CHANGE_EN BIT(24)
#define ROGUE_FWIF_INICFG_WORKEST BIT(25)
#define ROGUE_FWIF_INICFG_PDVFS BIT(26)
#define ROGUE_FWIF_INICFG_VALIDATE_SOCUSC_TIMER BIT(31)


/*
 * Proactive DVFS structures
 */
#define NUM_OPP_VALUES 16

struct pdvfs_opp {
	u32 volt; /* V  */
	u32 freq; /* Hz */
} __aligned(8);

struct rogue_fwif_pdvfs_opp {
	struct pdvfs_opp opp_values[NUM_OPP_VALUES];
	u32 min_opp_point;
	u32 max_opp_point;
} __aligned(8);


/*
 * Kernel CCB control
 */
#define ROGUE_CMD_MAGIC_DWORD (0x2ABC)
#define ROGUE_CMD_MAGIC_DWORD_MASK (0xFFFF0000U)
#define ROGUE_CMD_MAGIC_DWORD_SHIFT (16U)
#define ROGUE_CMD_MAGIC_DWORD_SHIFTED \
	(ROGUE_CMD_MAGIC_DWORD << ROGUE_CMD_MAGIC_DWORD_SHIFT)

struct rogue_fwif_ccb_ctl {
	/* write offset into array of commands (MUST be aligned to 16 bytes!) */
	u32 write_offset;
	/* Padding to ensure read and write offsets are in separate cache lines. */
	u8 padding[128 - sizeof(u32)];
	/* read offset into array of commands */
	u32 read_offset;
	/* Offset wrapping mask (Total capacity of the CCB - 1) */
	u32 wrap_mask;
	/* size of each command in bytes */
	u32 cmd_size;
	u32 padding2;
} __aligned(8);

/*
 * KCCB and FWCCB commands. Only their size matters to build the rings; the
 * command payload unions of Linux are kept opaque here. The sizes (88 bytes,
 * payload at offset 8) are those asserted by pvr_rogue_fwif_check.h.
 */
#define PVR_FWIF_CCB_CMD_DATA_SIZE 80

struct rogue_fwif_kccb_cmd {
	/* Command type (enum rogue_fwif_kccb_cmd_type) */
	u32 cmd_type;
	/* Compatibility and other flags */
	u32 kccb_flags;
	/* Command data (union in Linux) */
	union {
		u8 raw[PVR_FWIF_CCB_CMD_DATA_SIZE];
		aligned_u64 align;
	} cmd_data;
} __aligned(8);

struct rogue_fwif_fwccb_cmd {
	/* Command type (enum rogue_fwif_fwccb_cmd_type) */
	u32 cmd_type;
	/* Compatibility and other flags */
	u32 fwccb_flags;
	/* Command data (union in Linux) */
	union {
		u8 raw[PVR_FWIF_CCB_CMD_DATA_SIZE];
		aligned_u64 align;
	} cmd_data;
} __aligned(8);


/*
 * Signature and checksums buffer, counter dump
 */
struct rogue_fwif_sigbuf_ctl {
	u32 buffer_fw_addr;
	u32 left_size_in_regs;
} __aligned(8);

struct rogue_fwif_counter_dump_ctl {
	u32 buffer_fw_addr;
	u32 size_in_dwords;
} __aligned(8);


/*
 * Compatibility checks
 */
#define ROGUE_FWIF_COMPCHECKS_LAYOUT_VERSION 3

struct rogue_fwif_compchecks_bvnc {
	/* WARNING: This field must be defined as first one in this structure */
	u32 layout_version;
	aligned_u64 bvnc;
} __aligned(8);

struct rogue_fwif_init_options {
	u8 os_count_support;
	u8 padding[7];
} __aligned(8);

static inline void
rogue_fwif_compchecks_bvnc_init(struct rogue_fwif_compchecks_bvnc* compchecks)
{
	compchecks->layout_version = ROGUE_FWIF_COMPCHECKS_LAYOUT_VERSION;
	compchecks->bvnc = 0;
}

struct rogue_fwif_compchecks {
	/* hardware BVNC (from the ROGUE registers) */
	struct rogue_fwif_compchecks_bvnc hw_bvnc;
	/* firmware BVNC */
	struct rogue_fwif_compchecks_bvnc fw_bvnc;
	/* identifier of the FW processor version */
	u32 fw_processor_version;
	/* software DDK version */
	u32 ddk_version;
	/* software DDK build no. */
	u32 ddk_build;
	/* build options bit-field */
	u32 build_options;
	/* initialisation options bit-field */
	struct rogue_fwif_init_options init_options;
	/* Information is valid */
	bool updated __aligned(4);
	u32 padding;
} __aligned(8);


/*
 * Updated configuration post FW data init
 */
struct rogue_fwif_runtime_cfg {
	/* APM latency in ms before signalling IDLE to the host */
	u32 active_pm_latency_ms;
	/* Compatibility and other flags */
	u32 runtime_cfg_flags;
	/*
	 * If set, APM latency does not reset to system default each GPU power
	 * transition
	 */
	bool active_pm_latency_persistant __aligned(4);
	/* Core clock speed, currently only used to calculate timer ticks */
	u32 core_clock_speed;
	/* Last number of dusts change requested by the host */
	u32 default_dusts_num_init;
	/* Periodic Hardware Reset configuration values */
	u32 phr_mode;
	/* New number of milliseconds C/S is allowed to last */
	u32 hcs_deadline_ms;
	/* The watchdog period in microseconds */
	u32 wdg_period_us;
	/* Array of priorities per OS */
	u32 osid_priority[ROGUE_FW_MAX_NUM_OS];
	/* On-demand allocated HWPerf buffer address, to be passed to the FW */
	u32 hwperf_buf_fw_addr;

	bool padding __aligned(4);
};


/*
 * Control data
 */
#define ROGUE_FWIF_HWR_DEBUG_DUMP_ALL (99999U)

enum rogue_fwif_tpu_dm {
	ROGUE_FWIF_TPU_DM_PDM = 0,
	ROGUE_FWIF_TPU_DM_VDM = 1,
	ROGUE_FWIF_TPU_DM_CDM = 2,
	ROGUE_FWIF_TPU_DM_TDM = 3,
	ROGUE_FWIF_TPU_DM_LAST
};

enum rogue_fwif_gpio_val_mode {
	ROGUE_FWIF_GPIO_VAL_OFF = 0,
	ROGUE_FWIF_GPIO_VAL_GENERAL = 1,
	ROGUE_FWIF_GPIO_VAL_AP = 2,
	ROGUE_FWIF_GPIO_VAL_TESTBENCH = 5,
	ROGUE_FWIF_GPIO_VAL_LOOPBACK = 6,
	ROGUE_FWIF_GPIO_VAL_LOOPBACK_LITE = 7,
	ROGUE_FWIF_GPIO_VAL_LAST
};

enum fw_perf_conf {
	FW_PERF_CONF_NONE = 0,
	FW_PERF_CONF_ICACHE = 1,
	FW_PERF_CONF_DCACHE = 2,
	FW_PERF_CONF_JTLB_INSTR = 5,
	FW_PERF_CONF_INSTRUCTIONS = 6
};

enum fw_boot_stage {
	FW_BOOT_STAGE_TLB_INIT_FAILURE = -2,
	FW_BOOT_STAGE_NOT_AVAILABLE = -1,
	FW_BOOT_NOT_STARTED = 0,
	FW_BOOT_BLDR_STARTED = 1,
	FW_BOOT_CACHE_DONE,
	FW_BOOT_TLB_DONE,
	FW_BOOT_MAIN_STARTED,
	FW_BOOT_ALIGNCHECKS_DONE,
	FW_BOOT_INIT_DONE,
};

/* Kernel CCB return slot responses */
#define ROGUE_FWIF_KCCB_RTN_SLOT_CMD_EXECUTED BIT(0)
#define ROGUE_FWIF_KCCB_RTN_SLOT_CLEANUP_BUSY BIT(1)
#define ROGUE_FWIF_KCCB_RTN_SLOT_POLL_FAILURE BIT(2)
#define ROGUE_FWIF_KCCB_RTN_SLOT_NO_RESPONSE 0x0U

struct rogue_fwif_connection_ctl {
	/* Fw-Os connection states */
	enum rogue_fwif_connection_fw_state connection_fw_state;
	enum rogue_fwif_connection_os_state connection_os_state;
	u32 alive_fw_token;
	u32 alive_os_token;
} __aligned(8);

struct rogue_fwif_osinit {
	/* Kernel CCB */
	u32 kernel_ccbctl_fw_addr;
	u32 kernel_ccb_fw_addr;
	u32 kernel_ccb_rtn_slots_fw_addr;

	/* Firmware CCB */
	u32 firmware_ccbctl_fw_addr;
	u32 firmware_ccb_fw_addr;

	/* Workload Estimation Firmware CCB */
	u32 work_est_firmware_ccbctl_fw_addr;
	u32 work_est_firmware_ccb_fw_addr;

	u32 rogue_fwif_hwr_info_buf_ctl_fw_addr;

	u32 hwr_debug_dump_limit;

	u32 fw_os_data_fw_addr;

	/* Compatibility checks to be populated by the Firmware */
	struct rogue_fwif_compchecks rogue_comp_checks;
} __aligned(8);

/* BVNC Features */
struct rogue_hwperf_bvnc_block {
	u16 block_id;
	u16 num_counters;
	u16 num_blocks;
	u16 reserved;
};

#define ROGUE_HWPERF_MAX_BVNC_LEN (24)
#define ROGUE_HWPERF_MAX_BVNC_BLOCK_LEN (16U)

struct rogue_hwperf_bvnc {
	char bvnc_string[ROGUE_HWPERF_MAX_BVNC_LEN];
	u32 bvnc_km_feature_flags;
	u16 num_bvnc_blocks;
	u16 bvnc_gpu_cores;
	struct rogue_hwperf_bvnc_block
		bvnc_blocks[ROGUE_HWPERF_MAX_BVNC_BLOCK_LEN];
};

PVR_FW_STRUCT_SIZE_ASSERT(struct rogue_hwperf_bvnc);

struct rogue_fwif_sysinit {
	/* Fault read address */
	aligned_u64 fault_phys_addr;

	/* PDS execution base */
	aligned_u64 pds_exec_base;
	/* UCS execution base */
	aligned_u64 usc_exec_base;
	/* FBCDC bindless texture state table base */
	aligned_u64 fbcdc_state_table_base;
	aligned_u64 fbcdc_large_state_table_base;
	/* Texture state base */
	aligned_u64 texture_heap_base;

	/* Event filter for Firmware events */
	aligned_u64 hw_perf_filter;

	aligned_u64 slc3_fence_dev_addr;

	u32 tpu_trilinear_frac_mask[ROGUE_FWIF_TPU_DM_LAST] __aligned(8);

	/* Signature and Checksum Buffers for DMs */
	struct rogue_fwif_sigbuf_ctl sigbuf_ctl[PVR_FWIF_DM_MAX];

	struct rogue_fwif_pdvfs_opp pdvfs_opp_info;

	struct rogue_fwif_dma_addr coremem_data_store;

	struct rogue_fwif_counter_dump_ctl counter_dump_ctl;

	u32 filter_flags;

	u32 runtime_cfg_fw_addr;

	u32 trace_buf_ctl_fw_addr;
	u32 fw_sys_data_fw_addr;

	u32 gpu_util_fw_cb_ctl_fw_addr;
	u32 reg_cfg_fw_addr;
	u32 hwperf_ctl_fw_addr;

	u32 align_checks;

	/* Core clock speed at FW boot time */
	u32 initial_core_clock_speed;

	/* APM latency in ms before signalling IDLE to the host */
	u32 active_pm_latency_ms;

	/* Flag to be set by the Firmware after successful start */
	bool firmware_started __aligned(4);

	/* Host/FW Trace synchronisation Partition Marker */
	u32 marker_val;

	/* Firmware initialization complete time */
	u32 firmware_started_timestamp;

	u32 jones_disable_mask;

	/* Firmware performance counter config */
	enum fw_perf_conf firmware_perf;

	/* FW Pointer to memory containing core clock rate in Hz */
	u32 core_clock_rate_fw_addr;

	enum rogue_fwif_gpio_val_mode gpio_validation_mode;

	/* Used in HWPerf for decoding BVNC Features */
	struct rogue_hwperf_bvnc bvnc_km_feature_flags;

	/* Value to write into ROGUE_CR_TFBC_COMPRESSION_CONTROL */
	u32 tfbc_compression_control;
} __aligned(8);


/*
 * Timer correlation and GPU utilisation
 */
struct rogue_fwif_time_corr {
	aligned_u64 os_timestamp;
	aligned_u64 os_mono_timestamp;
	aligned_u64 cr_timestamp;
	aligned_u64 cr_delta_to_os_delta_kns;

	u32 core_clock_speed;
	u32 reserved;
} __aligned(8);

#define ROGUE_FWIF_TIME_CORR_ARRAY_SIZE 256U

static_assert((ROGUE_FWIF_TIME_CORR_ARRAY_SIZE
		& (ROGUE_FWIF_TIME_CORR_ARRAY_SIZE - 1U)) == 0U,
	"ROGUE_FWIF_TIME_CORR_ARRAY_SIZE must be a power of two");

struct rogue_fwif_gpu_util_fwcb {
	struct rogue_fwif_time_corr time_corr[ROGUE_FWIF_TIME_CORR_ARRAY_SIZE];
	u32 time_corr_seq_count;

	/* Compatibility and other flags */
	u32 gpu_util_flags;

	/* Last GPU state + OS time of the last state update */
	aligned_u64 last_word;

	/* Counters for the amount of time the GPU was active/idle/blocked */
	aligned_u64 stats_counters[PVR_FWIF_GPU_UTIL_STATE_NUM];
} __aligned(8);


/*
 * Contexts and client CCBs. Added for pvr_mmu/pvr_context.cpp, from
 * pvr_rogue_fwif_shared.h, pvr_rogue_fwif.h and pvr_rogue_fwif_client.h.
 * Enum typed members of Linux are u32 here (same size and offset).
 */

/* pvr_rogue_fwif_shared.h */
#define ROGUE_FWIF_CCB_CMD_MAX_UFOS (32U + 32U)
#define ROGUE_FWIF_PRBUFFER_MAXSUPPORTED (2)

struct rogue_fwif_ufo {
	u32 addr;
	u32 value;
};

#define ROGUE_FWIF_UFO_ADDR_IS_SYNC_CHECKPOINT (1)

struct rogue_fwif_cmd_common {
	/* associated frame number */
	u32 frame_num;
};

/*
 * Client Circular Command Buffer (CCCB) control structure, shared between the
 * host and the firmware. Byte offsets into the CCCB.
 */
struct rogue_fwif_cccb_ctl {
	/* Host write offset into CCB. This must be aligned to 16 bytes. */
	u32 write_offset;
	/* Firmware read offset into CCB. */
	u32 read_offset;
	/* Firmware fence dependency offset. */
	u32 dep_offset;
	/* Offset wrapping mask, total capacity in bytes of the CCB-1 */
	u32 wrap_mask;
	/* Only used if SUPPORT_AGP is present. */
	u32 read_offset2;
	/* Only used if SUPPORT_AGP4 is present. */
	u32 read_offset3;
	/* Only used if SUPPORT_AGP4 is present. */
	u32 read_offset4;
	u32 padding;
} __aligned(8);

struct rogue_fwif_cdm_registers_cswitch {
	u64 cdmreg_cdm_context_pds0;
	u64 cdmreg_cdm_context_pds1;
	u64 cdmreg_cdm_terminate_pds;
	u64 cdmreg_cdm_terminate_pds1;
	/* CDM resume controls */
	u64 cdmreg_cdm_resume_pds0;
	u64 cdmreg_cdm_context_pds0_b;
	u64 cdmreg_cdm_resume_pds0_b;
};

struct rogue_fwif_static_computecontext_state {
	/* CDM registers for ctx switch */
	struct rogue_fwif_cdm_registers_cswitch ctxswitch_regs __aligned(8);
};

/* pvr_rogue_fwif.h: memory and common contexts */
#define ROGUE_FWIF_INVALID_PC_PHYADDR 0xFFFFFFFFFFFFFFFFLLU
#define ROGUE_FW_BIF_INVALID_PCSET 0xFFFFFFFFU

struct rogue_fwif_dllist_node {
	u32 p;
	u32 n;
};

/* Firmware memory context. */
struct rogue_fwif_fwmemcontext {
	/* device physical address of context's page catalogue */
	aligned_u64 pc_dev_paddr;
	/*
	 * associated page catalog base register (ROGUE_FW_BIF_INVALID_PCSET ==
	 * unallocated)
	 */
	u32 page_cat_base_reg_set;
	/* breakpoint address */
	u32 breakpoint_addr;
	/* breakpoint handler address */
	u32 bp_handler_addr;
	/* DM and enable control for BP */
	u32 breakpoint_ctl;
	/* Compatibility and other flags */
	u32 fw_mem_ctx_flags;
	u32 padding;
} __aligned(8);

/*
 * FW-accessible ISP state which must be written out to memory on context
 * store
 */
struct rogue_fwif_frag_ctx_state {
	u32 frag_reg_pm_deallocated_mask_status;
	u32 frag_reg_dm_pds_mtilefree_status;
	/* Compatibility and other flags */
	u32 ctx_state_flags;
	/* sized at runtime: number of ISP store registers of the core */
	u32 frag_reg_isp_store[];
} __aligned(8);

struct rogue_fwif_compute_ctx_state {
	u32 ctx_state_flags; /* Target buffer and other flags */
};

struct rogue_fwif_fwcommoncontext {
	/* CCB details for this firmware context */
	u32 ccbctl_fw_addr; /* CCB control */
	u32 ccb_fw_addr; /* CCB base */
	struct rogue_fwif_dma_addr ccb_meta_dma_addr;

	/* geom/frag context suspend state, read/written by FW */
	u32 context_state_addr __aligned(8);

	/* Flags e.g. for context switching */
	u32 fw_com_ctx_flags;
	u32 priority;
	u32 priority_seq_num;

	/* Register updates for Framework */
	u32 rf_cmd_addr __aligned(8);

	/* True when some stats are pending */
	bool stats_pending __aligned(4);
	/* Number of stores on this context since last update */
	s32 stats_num_stores;
	/* Number of OOMs on this context since last update */
	s32 stats_num_out_of_memory;
	/* Number of PRs on this context since last update */
	s32 stats_num_partial_renders;
	/* Data Master type */
	u32 dm;
	/* Device Virtual Address of the signal the context is waiting on */
	aligned_u64 wait_signal_address;
	/* List entry for the wait-signal list */
	struct rogue_fwif_dllist_node wait_signal_node __aligned(8);
	/* List entry for the buffer stalled list */
	struct rogue_fwif_dllist_node buf_stalled_node __aligned(8);
	/* Address of the circular buffer queue pointers */
	aligned_u64 cbuf_queue_ctrl_addr;

	aligned_u64 robustness_address;
	/* Max HWR deadline limit in ms */
	u32 max_deadline_ms;
	/* Following HWR circular buffer read-offset needs resetting */
	bool read_offset_needs_reset;

	/* List entry for the waiting list */
	struct rogue_fwif_dllist_node waiting_node __aligned(8);
	/* List entry for the run list */
	struct rogue_fwif_dllist_node run_node __aligned(8);
	/* UFO that last failed (or NULL) */
	struct rogue_fwif_ufo last_failed_ufo;

	/* Memory context */
	u32 fw_mem_context_fw_addr;

	/* the Server Common Context */
	u32 server_common_context_id;
	/* associated process ID */
	u32 pid;

	/* True when Geom DM OOM is not allowed */
	bool geom_oom_disabled __aligned(4);
} __aligned(8);

/* Firmware compute context. */
struct rogue_fwif_fwcomputecontext {
	/* Firmware context for the CDM */
	struct rogue_fwif_fwcommoncontext cdm_context;

	struct rogue_fwif_static_computecontext_state
		static_compute_context_state;

	/* Number of commands submitted to the WorkEst FW CCB */
	u32 work_est_ccb_submitted;

	/* Compatibility and other flags */
	u32 compute_ctx_flags;

	u32 wgp_state;
	u32 wgp_checksum;
	u32 core_mask_a;
	u32 core_mask_b;
} __aligned(8);

/* Firmware TQ3D context. */
struct rogue_fwif_fwtransfercontext {
	/* Firmware context for TQ3D. */
	struct rogue_fwif_fwcommoncontext tq_context;
} __aligned(8);

/* pvr_rogue_fwif.h: KCCB command types and payloads (cmd_data) */
enum rogue_fwif_kccb_cmd_type {
	ROGUE_FWIF_KCCB_CMD_KICK = 101U | ROGUE_CMD_MAGIC_DWORD_SHIFTED,
	ROGUE_FWIF_KCCB_CMD_MMUCACHE = 102U | ROGUE_CMD_MAGIC_DWORD_SHIFTED,
	ROGUE_FWIF_KCCB_CMD_CLEANUP = 106U | ROGUE_CMD_MAGIC_DWORD_SHIFTED,
	/* Freelist Grow done */
	ROGUE_FWIF_KCCB_CMD_FREELIST_GROW_UPDATE
		= 110U | ROGUE_CMD_MAGIC_DWORD_SHIFTED,
	/* Freelists Reconstruction done */
	ROGUE_FWIF_KCCB_CMD_FREELISTS_RECONSTRUCTION_UPDATE
		= 112U | ROGUE_CMD_MAGIC_DWORD_SHIFTED,
	ROGUE_FWIF_KCCB_CMD_HEALTH_CHECK = 115U | ROGUE_CMD_MAGIC_DWORD_SHIFTED,
	/* There is a geometry and a fragment command in this single kick */
	ROGUE_FWIF_KCCB_CMD_COMBINED_GEOM_FRAG_KICK
		= 117U | ROGUE_CMD_MAGIC_DWORD_SHIFTED,
};

#define ROGUE_FWIF_MMUCACHEDATA_FLAGS_PT (0x1U) /* MMU_CTRL_INVAL_PT_EN */
#define ROGUE_FWIF_MMUCACHEDATA_FLAGS_PD (0x2U) /* MMU_CTRL_INVAL_PD_EN */
#define ROGUE_FWIF_MMUCACHEDATA_FLAGS_PC (0x4U) /* MMU_CTRL_INVAL_PC_EN */
#define ROGUE_FWIF_MMUCACHEDATA_FLAGS_PMTLB (0x10)
#define ROGUE_FWIF_MMUCACHEDATA_FLAGS_TLB \
	(ROGUE_FWIF_MMUCACHEDATA_FLAGS_PMTLB | 0x8)
#define ROGUE_FWIF_MMUCACHEDATA_FLAGS_CTX_ALL (0x800)
#define ROGUE_FWIF_MMUCACHEDATA_FLAGS_INTERRUPT (0x4000000U)

struct rogue_fwif_mmucachedata {
	u32 cache_flags;
	u32 mmu_cache_sync_fw_addr;
	u32 mmu_cache_sync_update_value;
};

#define ROGUE_FWIF_KCCB_CMD_KICK_DATA_MAX_NUM_CLEANUP_CTLS \
	(ROGUE_FWIF_PRBUFFER_MAXSUPPORTED + 1U) /* +1 is RTDATASET cleanup */

struct rogue_fwif_kccb_cmd_kick_data {
	/* address of the firmware context */
	u32 context_fw_addr;
	/* Client CCB woff update */
	u32 client_woff_update;
	/* Client CCB wrap mask update after CCCB growth */
	u32 client_wrap_mask_update;
	/* number of CleanupCtl pointers attached */
	u32 num_cleanup_ctl;
	/* CleanupCtl structures associated with command */
	u32 cleanup_ctl_fw_addr
		[ROGUE_FWIF_KCCB_CMD_KICK_DATA_MAX_NUM_CLEANUP_CTLS];
	/* offset to the CmdHeader with the workload estimation kick data */
	u32 work_est_cmd_header_offset;
};

enum rogue_fwif_cleanup_type {
	/* FW common context cleanup */
	ROGUE_FWIF_CLEANUP_FWCOMMONCONTEXT,
	/* FW HW RT data cleanup */
	ROGUE_FWIF_CLEANUP_HWRTDATA,
	/* FW freelist cleanup */
	ROGUE_FWIF_CLEANUP_FREELIST,
	/* FW ZS Buffer cleanup */
	ROGUE_FWIF_CLEANUP_ZSBUFFER,
};

struct rogue_fwif_cleanup_request {
	/* Cleanup type (enum rogue_fwif_cleanup_type) */
	u32 cleanup_type;
	union {
		/* FW common context to cleanup */
		u32 context_fw_addr;
		/* HW RT to cleanup */
		u32 hwrt_data_fw_addr;
		/* Freelist to cleanup */
		u32 freelist_fw_addr;
		/* ZS Buffer to cleanup */
		u32 zs_buffer_fw_addr;
	} cleanup_data;
};

static_assert(sizeof(struct rogue_fwif_mmucachedata)
		<= PVR_FWIF_CCB_CMD_DATA_SIZE
	&& sizeof(struct rogue_fwif_kccb_cmd_kick_data)
		<= PVR_FWIF_CCB_CMD_DATA_SIZE
	&& sizeof(struct rogue_fwif_cleanup_request)
		<= PVR_FWIF_CCB_CMD_DATA_SIZE,
	"KCCB command payload larger than the opaque cmd_data block");

/* pvr_rogue_fwif.h: client CCB commands */
#define ROGUE_FWIF_FWALLOC_ALIGN sizeof(u64)

#define ROGUE_CCB_TYPE_TASK BIT(15)
#define ROGUE_CCB_FWALLOC_ALIGN(size) \
	(((size) + (ROGUE_FWIF_FWALLOC_ALIGN - 1)) \
		& ~(ROGUE_FWIF_FWALLOC_ALIGN - 1))

#define ROGUE_FWIF_CCB_CMD_TYPE_GEOM \
	(201U | ROGUE_CMD_MAGIC_DWORD_SHIFTED | ROGUE_CCB_TYPE_TASK)
#define ROGUE_FWIF_CCB_CMD_TYPE_TQ_3D \
	(202U | ROGUE_CMD_MAGIC_DWORD_SHIFTED | ROGUE_CCB_TYPE_TASK)
#define ROGUE_FWIF_CCB_CMD_TYPE_FRAG \
	(203U | ROGUE_CMD_MAGIC_DWORD_SHIFTED | ROGUE_CCB_TYPE_TASK)
#define ROGUE_FWIF_CCB_CMD_TYPE_FRAG_PR \
	(204U | ROGUE_CMD_MAGIC_DWORD_SHIFTED | ROGUE_CCB_TYPE_TASK)
#define ROGUE_FWIF_CCB_CMD_TYPE_CDM \
	(205U | ROGUE_CMD_MAGIC_DWORD_SHIFTED | ROGUE_CCB_TYPE_TASK)
#define ROGUE_FWIF_CCB_CMD_TYPE_TQ_TDM \
	(206U | ROGUE_CMD_MAGIC_DWORD_SHIFTED | ROGUE_CCB_TYPE_TASK)
#define ROGUE_FWIF_CCB_CMD_TYPE_FBSC_INVALIDATE \
	(207U | ROGUE_CMD_MAGIC_DWORD_SHIFTED | ROGUE_CCB_TYPE_TASK)
#define ROGUE_FWIF_CCB_CMD_TYPE_TQ_2D \
	(208U | ROGUE_CMD_MAGIC_DWORD_SHIFTED | ROGUE_CCB_TYPE_TASK)
#define ROGUE_FWIF_CCB_CMD_TYPE_PRE_TIMESTAMP \
	(209U | ROGUE_CMD_MAGIC_DWORD_SHIFTED | ROGUE_CCB_TYPE_TASK)
#define ROGUE_FWIF_CCB_CMD_TYPE_NULL \
	(210U | ROGUE_CMD_MAGIC_DWORD_SHIFTED | ROGUE_CCB_TYPE_TASK)
#define ROGUE_FWIF_CCB_CMD_TYPE_ABORT \
	(211U | ROGUE_CMD_MAGIC_DWORD_SHIFTED | ROGUE_CCB_TYPE_TASK)

/* Leave a gap between CCB specific commands and generic commands */
#define ROGUE_FWIF_CCB_CMD_TYPE_FENCE (212U | ROGUE_CMD_MAGIC_DWORD_SHIFTED)
#define ROGUE_FWIF_CCB_CMD_TYPE_UPDATE (213U | ROGUE_CMD_MAGIC_DWORD_SHIFTED)
#define ROGUE_FWIF_CCB_CMD_TYPE_RMW_UPDATE \
	(214U | ROGUE_CMD_MAGIC_DWORD_SHIFTED)
#define ROGUE_FWIF_CCB_CMD_TYPE_FENCE_PR (215U | ROGUE_CMD_MAGIC_DWORD_SHIFTED)
#define ROGUE_FWIF_CCB_CMD_TYPE_PRIORITY \
	(216U | ROGUE_CMD_MAGIC_DWORD_SHIFTED)
#define ROGUE_FWIF_CCB_CMD_TYPE_POST_TIMESTAMP \
	(217U | ROGUE_CMD_MAGIC_DWORD_SHIFTED)
#define ROGUE_FWIF_CCB_CMD_TYPE_UNFENCED_UPDATE \
	(218U | ROGUE_CMD_MAGIC_DWORD_SHIFTED)
#define ROGUE_FWIF_CCB_CMD_TYPE_UNFENCED_RMW_UPDATE \
	(219U | ROGUE_CMD_MAGIC_DWORD_SHIFTED)

#define ROGUE_FWIF_CCB_CMD_TYPE_PADDING (221U | ROGUE_CMD_MAGIC_DWORD_SHIFTED)

struct rogue_fwif_workest_kick_data {
	/* Index for the KM Workload estimation return data array */
	u16 return_data_index __aligned(8);
	/* Predicted time taken to do the work in cycles */
	u32 cycles_prediction __aligned(8);
	/* Deadline for the workload */
	aligned_u64 deadline;
};

struct rogue_fwif_ccb_cmd_header {
	u32 cmd_type;
	u32 cmd_size;
	/* external job reference (debug) */
	u32 ext_job_ref;
	/* internal job reference (debug) */
	u32 int_job_ref;
	/* Workload Estimation - Workload Estimation Data */
	struct rogue_fwif_workest_kick_data work_est_kick_data __aligned(8);
};

/* pvr_rogue_fwif_client.h: compute command */
#define ROGUE_COMPUTE_FLAG_PREVENT_ALL_OVERLAP BIT(2)
/* Use single core in a multi core setup. */
#define ROGUE_COMPUTE_FLAG_SINGLE_CORE BIT(5)

struct rogue_fwif_compute_regs {
	u64 tpu_border_colour_table;
	/* Only used when feature CDM_USER_MODE_QUEUE present. */
	u64 cdm_cb_queue;
	/* Only used when feature CDM_USER_MODE_QUEUE present. */
	u64 cdm_cb_base;
	/* Only used when feature CDM_USER_MODE_QUEUE present. */
	u64 cdm_cb;
	/* Only used when feature CDM_USER_MODE_QUEUE is not present. */
	u64 cdm_ctrl_stream_base;
	u64 cdm_context_state_base_addr;
	/* Only used when BRN 49927 is present. */
	u32 tpu;
	u32 cdm_resume_pds1;
	/* Only used when feature COMPUTE_MORTON_CAPABLE present. */
	u32 cdm_item;
	/* Only used when feature CLUSTER_GROUPING present. */
	u32 compute_cluster;
	/* Only used when feature TPU_DM_GLOBAL_REGISTERS present. */
	u32 tpu_tag_cdm_ctrl;
	u32 padding;
};

struct rogue_fwif_cmd_compute {
	/* Common command attributes */
	struct rogue_fwif_cmd_common common __aligned(8);
	/* CDM registers */
	struct rogue_fwif_compute_regs regs;
	/* Control flags */
	u32 flags __aligned(8);
	/* Only used when feature UNIFIED_STORE_VIRTUAL_PARTITIONING present. */
	u32 num_temp_regions;
	/* Only used when feature CDM_USER_MODE_QUEUE present. */
	u32 stream_start_offset;
	/* Only used when feature GPU_MULTICORE_SUPPORT present. */
	u32 execute_count;
};


/*
 * Render contexts, free lists, HWRT data and the geometry, fragment and
 * transfer commands. Added for pvr_render.cpp/pvr_stream.cpp, from
 * pvr_rogue_defs.h, pvr_rogue_fwif_shared.h, pvr_rogue_fwif.h and
 * pvr_rogue_fwif_client.h. Enum typed members of Linux are u32 here.
 */

/* pvr_rogue_defs.h */
#define ROGUE_BIF_PM_PHYSICAL_PAGE_ALIGNSHIFT (12U)
#define ROGUE_BIF_PM_PHYSICAL_PAGE_SIZE \
	BIT(ROGUE_BIF_PM_PHYSICAL_PAGE_ALIGNSHIFT)
#define ROGUE_BIF_PM_FREELIST_BASE_ADDR_ALIGNSIZE (16U)

/* pvr_rogue_fwif_shared.h */
#define ROGUE_FWIF_NUM_RTDATAS 2U
#define ROGUE_FWIF_NUM_GEOMDATAS 1U
#define ROGUE_FWIF_NUM_RTDATA_FREELISTS 2U
#define ROGUE_NUM_GEOM_CORES 1U
#define ROGUE_NUM_GEOM_CORES_SIZE 2U

#define ROGUE_FWIF_PRBUFFER_ZSBUFFER (0)
#define ROGUE_FWIF_PRBUFFER_MSAABUFFER (1)

#define ROGUE_FW_LOCAL_FREELIST (0)
#define ROGUE_FW_GLOBAL_FREELIST (1)
#define ROGUE_FW_FREELIST_TYPE_LAST ROGUE_FW_GLOBAL_FREELIST
#define ROGUE_FW_MAX_FREELISTS (ROGUE_FW_FREELIST_TYPE_LAST + 1U)

struct rogue_fwif_cleanup_ctl {
	/* Number of commands received by the FW */
	u32 submitted_commands;
	/* Number of commands executed by the FW */
	u32 executed_commands;
} __aligned(8);

/*
 * Geometry and fragment commands: block of FW addresses written by the
 * kernel, shared between UM, KM and FW across all BVNCs.
 */
struct rogue_fwif_cmd_geom_frag_shared {
	/* Common command attributes */
	struct rogue_fwif_cmd_common cmn;
	/* RTData associated with this command */
	u32 hwrt_data_fw_addr;
	/* Supported PR Buffers like Z/S/MSAA Scratch */
	u32 pr_buffer_fw_addr[ROGUE_FWIF_PRBUFFER_MAXSUPPORTED];
};

struct rogue_fwif_geom_registers_caswitch {
	u64 geom_reg_vdm_context_state_base_addr;
	u64 geom_reg_vdm_context_state_resume_addr;
	u64 geom_reg_ta_context_state_base_addr;

	struct {
		u64 geom_reg_vdm_context_store_task0;
		u64 geom_reg_vdm_context_store_task1;
		u64 geom_reg_vdm_context_store_task2;

		/* VDM resume state update controls */
		u64 geom_reg_vdm_context_resume_task0;
		u64 geom_reg_vdm_context_resume_task1;
		u64 geom_reg_vdm_context_resume_task2;

		u64 geom_reg_vdm_context_store_task3;
		u64 geom_reg_vdm_context_store_task4;

		u64 geom_reg_vdm_context_resume_task3;
		u64 geom_reg_vdm_context_resume_task4;
	} geom_state[2];
};

struct rogue_fwif_static_rendercontext_state {
	/* Geom registers for ctx switch */
	struct rogue_fwif_geom_registers_caswitch
		ctxswitch_regs[ROGUE_NUM_GEOM_CORES_SIZE] __aligned(8);
};

/* pvr_rogue_fwif.h: geometry context state and render context */
#define ROGUE_NUM_GEOM_CORES_MAX 4

struct rogue_fwif_geom_ctx_state_per_geom {
	/* To store in mid-TA */
	aligned_u64 geom_reg_vdm_call_stack_pointer;
	/* Initial value (in case is 'lost' due to a lock-up */
	aligned_u64 geom_reg_vdm_call_stack_pointer_init;
	u32 geom_reg_vbs_so_prim[4];
	u16 geom_current_idx;
	u16 padding[3];
} __aligned(8);

struct rogue_fwif_geom_ctx_state {
	/* FW-accessible TA state which must be written out on context store */
	struct rogue_fwif_geom_ctx_state_per_geom
		geom_core[ROGUE_NUM_GEOM_CORES_MAX];
} __aligned(8);

/* Firmware render context. */
struct rogue_fwif_fwrendercontext {
	/* Geometry firmware context. */
	struct rogue_fwif_fwcommoncontext geom_context;
	/* Fragment firmware context. */
	struct rogue_fwif_fwcommoncontext frag_context;

	struct rogue_fwif_static_rendercontext_state static_render_context_state;

	/* Number of commands submitted to the WorkEst FW CCB */
	u32 work_est_ccb_submitted;

	/* Compatibility and other flags */
	u32 fw_render_ctx_flags;
} __aligned(8);

/* pvr_rogue_fwif.h: KCCB payloads (written into rogue_fwif_kccb_cmd) */
struct rogue_fwif_kccb_cmd_combined_geom_frag_kick_data {
	struct rogue_fwif_kccb_cmd_kick_data geom_cmd_kick_data;
	struct rogue_fwif_kccb_cmd_kick_data frag_cmd_kick_data;
};

struct rogue_fwif_freelist_gs_data {
	/* Freelist FW address */
	u32 freelist_fw_addr;
	/* Amount of the Freelist change */
	u32 delta_pages;
	/* New amount of pages on the freelist (including ready pages) */
	u32 new_pages;
	/* Number of ready pages to be held in reserve until OOM */
	u32 ready_pages;
};

#define MAX_FREELISTS_SIZE 3
#define MAX_HW_GEOM_FRAG_CONTEXTS_SIZE 3

#define ROGUE_FWIF_MAX_FREELISTS_TO_RECONSTRUCT \
	(MAX_HW_GEOM_FRAG_CONTEXTS_SIZE * MAX_FREELISTS_SIZE * 2U)
#define ROGUE_FWIF_FREELISTS_RECONSTRUCTION_FAILED_FLAG 0x80000000U

struct rogue_fwif_freelists_reconstruction_data {
	u32 freelist_count;
	u32 freelist_ids[ROGUE_FWIF_MAX_FREELISTS_TO_RECONSTRUCT];
};

static_assert(sizeof(struct rogue_fwif_kccb_cmd_combined_geom_frag_kick_data)
			<= PVR_FWIF_CCB_CMD_DATA_SIZE
		&& sizeof(struct rogue_fwif_freelist_gs_data)
			<= PVR_FWIF_CCB_CMD_DATA_SIZE
		&& sizeof(struct rogue_fwif_freelists_reconstruction_data)
			<= PVR_FWIF_CCB_CMD_DATA_SIZE,
	"KCCB command payload larger than the opaque cmd_data block");

/* pvr_rogue_fwif.h: FWCCB command types and payloads (cmd_data) */
enum rogue_fwif_fwccb_cmd_type {
	/* Requests ZSBuffer to be backed with physical pages */
	ROGUE_FWIF_FWCCB_CMD_ZSBUFFER_BACKING
		= 101U | ROGUE_CMD_MAGIC_DWORD_SHIFTED,
	/* Requests ZSBuffer to be unbacked */
	ROGUE_FWIF_FWCCB_CMD_ZSBUFFER_UNBACKING
		= 102U | ROGUE_CMD_MAGIC_DWORD_SHIFTED,
	/* Requests an on-demand freelist grow/shrink */
	ROGUE_FWIF_FWCCB_CMD_FREELIST_GROW = 103U | ROGUE_CMD_MAGIC_DWORD_SHIFTED,
	/* Requests freelists reconstruction */
	ROGUE_FWIF_FWCCB_CMD_FREELISTS_RECONSTRUCTION
		= 104U | ROGUE_CMD_MAGIC_DWORD_SHIFTED,
	/* Notifies host of a HWR event on a context */
	ROGUE_FWIF_FWCCB_CMD_CONTEXT_RESET_NOTIFICATION
		= 105U | ROGUE_CMD_MAGIC_DWORD_SHIFTED,
	/* Requests an on-demand debug dump */
	ROGUE_FWIF_FWCCB_CMD_DEBUG_DUMP = 106U | ROGUE_CMD_MAGIC_DWORD_SHIFTED,
	/* Requests an on-demand update on process stats */
	ROGUE_FWIF_FWCCB_CMD_UPDATE_STATS = 107U | ROGUE_CMD_MAGIC_DWORD_SHIFTED,
	ROGUE_FWIF_FWCCB_CMD_CORE_CLK_RATE_CHANGE
		= 108U | ROGUE_CMD_MAGIC_DWORD_SHIFTED,
	ROGUE_FWIF_FWCCB_CMD_REQUEST_GPU_RESTART
		= 109U | ROGUE_CMD_MAGIC_DWORD_SHIFTED,
	/* Notifies host of a FW pagefault */
	ROGUE_FWIF_FWCCB_CMD_CONTEXT_FW_PF_NOTIFICATION
		= 112U | ROGUE_CMD_MAGIC_DWORD_SHIFTED,
};

struct rogue_fwif_fwccb_cmd_freelist_gs_data {
	u32 freelist_id;
};

struct rogue_fwif_fwccb_cmd_freelists_reconstruction_data {
	u32 freelist_count;
	u32 hwr_counter;
	u32 freelist_ids[ROGUE_FWIF_MAX_FREELISTS_TO_RECONSTRUCT];
};

/* pvr_rogue_fwif.h: render target array control, free list, HWRT data */
struct rogue_fwif_rta_ctl {
	/* Render number */
	u32 render_target_index;
	/* index in RTA */
	u32 current_render_target;
	/* total active RTs */
	u32 active_render_targets;
	/* total active RTs from the first TA kick, for OOM */
	u32 cumul_active_render_targets;
	/* Array of valid RT indices */
	u32 valid_render_targets_fw_addr;
	/* Array of number of occurred partial renders per render target */
	u32 rta_num_partial_renders_fw_addr;
	/* Number of render targets in the array */
	u32 max_rts;
	/* Compatibility and other flags */
	u32 rta_ctl_flags;
} __aligned(8);

struct rogue_fwif_freelist {
	aligned_u64 freelist_dev_addr;
	aligned_u64 current_dev_addr;
	u32 current_stack_top;
	u32 max_pages;
	u32 grow_pages;
	/* HW pages */
	u32 current_pages;
	u32 allocated_page_count;
	u32 allocated_mmu_page_count;
	u32 freelist_id;

	bool grow_pending __aligned(4);
	/* Pages that should be used only when OOM is reached */
	u32 ready_pages;
	/* Compatibility and other flags */
	u32 freelist_flags;
	/* PM Global PB on which Freelist is loaded */
	u32 pm_global_pb;
	u32 padding;
} __aligned(8);

/* HWRTData flags */
#define HWRTDATA_HAS_LAST_GEOM BIT(2)
#define HWRTDATA_PARTIAL_RENDERED BIT(3)
#define HWRTDATA_DISABLE_TILE_REORDERING BIT(4)
#define HWRTDATA_NEED_BRN65101_BLIT BIT(5)
#define HWRTDATA_FIRST_BRN65101_STRIP BIT(6)
#define HWRTDATA_NEED_BRN67182_2ND_RENDER BIT(7)

enum rogue_fwif_rtdata_state {
	ROGUE_FWIF_RTDATA_STATE_NONE = 0,
	ROGUE_FWIF_RTDATA_STATE_KICK_GEOM,
	ROGUE_FWIF_RTDATA_STATE_KICK_GEOM_FIRST,
	ROGUE_FWIF_RTDATA_STATE_GEOM_FINISHED,
	ROGUE_FWIF_RTDATA_STATE_KICK_FRAG,
	ROGUE_FWIF_RTDATA_STATE_FRAG_FINISHED,
	ROGUE_FWIF_RTDATA_STATE_FRAG_CONTEXT_STORED,
	ROGUE_FWIF_RTDATA_STATE_GEOM_OUTOFMEM,
	ROGUE_FWIF_RTDATA_STATE_PARTIALRENDERFINISHED,
	ROGUE_FWIF_RTDATA_STATE_HWR,
	ROGUE_FWIF_RTDATA_STATE_UNKNOWN = 0x7FFFFFFFU
};

struct rogue_fwif_hwrtdata_common {
	bool geom_caches_need_zeroing __aligned(4);

	u32 screen_pixel_max;
	aligned_u64 multi_sample_ctl;
	u64 flipped_multi_sample_ctl;
	u32 tpc_stride;
	u32 tpc_size;
	u32 te_screen;
	u32 mtile_stride;
	u32 teaa;
	u32 te_mtile1;
	u32 te_mtile2;
	u32 isp_merge_lower_x;
	u32 isp_merge_lower_y;
	u32 isp_merge_upper_x;
	u32 isp_merge_upper_y;
	u32 isp_merge_scale_x;
	u32 isp_merge_scale_y;
	u32 rgn_header_size;
	u32 isp_mtile_size;
	u32 padding;
} __aligned(8);

struct rogue_fwif_hwrtdata {
	/* MList Data Store */
	aligned_u64 pm_mlist_dev_addr;

	aligned_u64 vce_cat_base[4];
	aligned_u64 vce_last_cat_base[4];
	aligned_u64 te_cat_base[4];
	aligned_u64 te_last_cat_base[4];
	aligned_u64 alist_cat_base;
	aligned_u64 alist_last_cat_base;

	aligned_u64 pm_alist_stack_pointer;
	u32 pm_mlist_stack_pointer;

	u32 hwrt_data_common_fw_addr;

	u32 hwrt_data_flags;
	u32 state;	/* enum rogue_fwif_rtdata_state */

	u32 freelists_fw_addr[MAX_FREELISTS_SIZE] __aligned(8);
	u32 freelist_hwr_snapshot[MAX_FREELISTS_SIZE];

	aligned_u64 vheap_table_dev_addr;

	struct rogue_fwif_rta_ctl rta_ctl;

	aligned_u64 tail_ptrs_dev_addr;
	aligned_u64 macrotile_array_dev_addr;
	aligned_u64 rgn_header_dev_addr;
	aligned_u64 rtc_dev_addr;

	u32 owner_geom_not_used_by_host __aligned(8);

	bool geom_caches_need_zeroing __aligned(4);

	struct rogue_fwif_cleanup_ctl cleanup_state __aligned(64);
} __aligned(64);

/* pvr_rogue_fwif_client.h */
#define ROGUE_PM_PAGE_SIZE SZ_4K
#define ROGUE_PM_MAX_FREELIST_SIZE 0x80000000U	/* SZ_2G */

/* Flags of the geometry command (rogue_fwif_cmd_geom::flags) */
#define ROGUE_GEOM_FLAGS_FIRSTKICK BIT(0)
#define ROGUE_GEOM_FLAGS_LASTKICK BIT(1)
/* Use single core in a multi core setup. */
#define ROGUE_GEOM_FLAGS_SINGLE_CORE BIT(3)

/* Flags of the fragment command (rogue_fwif_cmd_frag::flags) */
#define ROGUE_FRAG_FLAGS_SINGLE_CORE BIT(3)
#define ROGUE_FRAG_FLAGS_GET_VIS_RESULTS BIT(5)
#define ROGUE_FRAG_FLAGS_DEPTHBUFFER BIT(7)
#define ROGUE_FRAG_FLAGS_STENCILBUFFER BIT(8)
#define ROGUE_FRAG_FLAGS_DISABLE_PIXELMERGE BIT(15)
#define ROGUE_FRAG_FLAGS_SCRATCHBUFFER BIT(19)
#define ROGUE_FRAG_FLAGS_PREVENT_CDM_OVERLAP BIT(26)

/* Flags of the transfer command (rogue_fwif_cmd_transfer::flags) */
#define ROGUE_TRANSFER_FLAGS_SINGLE_CORE BIT(1)

struct rogue_fwif_geom_regs {
	u64 vdm_ctrl_stream_base;
	u64 tpu_border_colour_table;

	/* Only used when feature VDM_DRAWINDIRECT present. */
	u64 vdm_draw_indirect0;
	/* Only used when feature VDM_DRAWINDIRECT present. */
	u32 vdm_draw_indirect1;

	u32 ppp_ctrl;
	u32 te_psg;
	/* Only used when BRN 49927 present. */
	u32 tpu;

	u32 vdm_context_resume_task0_size;
	/* Only used when feature VDM_OBJECT_LEVEL_LLS present. */
	u32 vdm_context_resume_task3_size;

	/* Only used when BRN 56279 or BRN 67381 present. */
	u32 pds_ctrl;

	u32 view_idx;

	/* Only used when feature TESSELLATION present */
	u32 pds_coeff_free_prog;

	u32 padding;
};

/* Only used when BRN 44455 or BRN 63027 present. */
struct rogue_fwif_dummy_rgnhdr_init_geom_regs {
	u64 te_psgregion_addr;
};

struct rogue_fwif_cmd_geom {
	/* must stay the first member: written by the kernel */
	struct rogue_fwif_cmd_geom_frag_shared cmd_shared;

	struct rogue_fwif_geom_regs regs __aligned(8);
	u32 flags __aligned(8);

	/*
	 * Holds the geometry/fragment fence value to allow the fragment partial
	 * render command to go through.
	 */
	struct rogue_fwif_ufo partial_render_geom_frag_fence;

	/* Only used when BRN 44455 or BRN 63027 present. */
	struct rogue_fwif_dummy_rgnhdr_init_geom_regs
		dummy_rgnhdr_init_geom_regs __aligned(8);

	/* Only used when BRN 61484 or BRN 66333 present. */
	u32 brn61484_66333_live_rt;

	u32 padding;
};

#define ROGUE_MAXIMUM_OUTPUT_REGISTERS_PER_PIXEL 8U
#define ROGUE_PBE_WORDS_REQUIRED_FOR_RENDERS 3U

struct rogue_fwif_frag_regs {
	u32 usc_pixel_output_ctrl;

	u32 usc_clear_register[ROGUE_MAXIMUM_OUTPUT_REGISTERS_PER_PIXEL];

	u32 isp_bgobjdepth;
	u32 isp_bgobjvals;
	u32 isp_aa;
	/* Only used when feature S7_TOP_INFRASTRUCTURE present. */
	u32 isp_xtp_pipe_enable;

	u32 isp_ctl;

	/* Only used when BRN 49927 present. */
	u32 tpu;

	u32 event_pixel_pds_info;

	/* Only used when feature CLUSTER_GROUPING present. */
	u32 pixel_phantom;

	u32 view_idx;

	u32 event_pixel_pds_data;

	/* Only used when BRN 65101 present. */
	u32 brn65101_event_pixel_pds_data;

	/* Only used when feature GPU_MULTICORE_SUPPORT or BRN 47217 present. */
	u32 isp_oclqry_stride;

	/* Only used when feature ZLS_SUBTILE present. */
	u32 isp_zls_pixels;

	/* Only used when feature ISP_ZLS_D24_S8_PACKING_OGL_MODE present. */
	u32 rgx_cr_blackpearl_fix;

	/* All values below the ALIGN(8) must be 64 bit. */
	aligned_u64 isp_scissor_base;
	u64 isp_dbias_base;
	u64 isp_oclqry_base;
	u64 isp_zlsctl;
	u64 isp_zload_store_base;
	u64 isp_stencil_load_store_base;

	/* FBCDC_ALGORITHM < 3 or FB_CDC_V4 (REQUIRES_FB_CDC_ZLS_SETUP) */
	u64 fb_cdc_zls;

	u64 pbe_word[8U][ROGUE_PBE_WORDS_REQUIRED_FOR_RENDERS];
	u64 tpu_border_colour_table;
	u64 pds_bgnd[3U];

	/* Only used when BRN 65101 present. */
	u64 pds_bgnd_brn65101[3U];

	u64 pds_pr_bgnd[3U];

	/* Only used when BRN 62850 or 62865 present. */
	u64 isp_dummy_stencil_store_base;

	/* Only used when BRN 66193 present. */
	u64 isp_dummy_depth_store_base;

	/* Only used when BRN 67182 present. */
	u32 rgnhdr_single_rt_size;
	/* Only used when BRN 67182 present. */
	u32 rgnhdr_scratch_offset;
};

struct rogue_fwif_cmd_frag {
	struct rogue_fwif_cmd_geom_frag_shared cmd_shared __aligned(8);

	struct rogue_fwif_frag_regs regs __aligned(8);
	/* command control flags. */
	u32 flags;
	/* Stride IN BYTES for Z-Buffer in case of RTAs. */
	u32 zls_stride;
	/* Stride IN BYTES for S-Buffer in case of RTAs. */
	u32 sls_stride;

	/* Only used if feature GPU_MULTICORE_SUPPORT present. */
	u32 execute_count;
};

#define ROGUE_PBE_WORDS_REQUIRED_FOR_TQS 3

struct rogue_fwif_transfer_regs {
	u32 isp_bgobjvals;

	u32 usc_pixel_output_ctrl;
	u32 usc_clear_register0;
	u32 usc_clear_register1;
	u32 usc_clear_register2;
	u32 usc_clear_register3;

	u32 isp_mtile_size;
	u32 isp_render_origin;
	u32 isp_ctl;

	/* Only used when feature S7_TOP_INFRASTRUCTURE present. */
	u32 isp_xtp_pipe_enable;
	u32 isp_aa;

	u32 event_pixel_pds_info;

	u32 event_pixel_pds_code;
	u32 event_pixel_pds_data;

	u32 isp_render;
	u32 isp_rgn;

	/* Only used when feature GPU_MULTICORE_SUPPORT present. */
	u32 frag_screen;

	/* All values below the aligned_u64 must be 64 bit. */
	aligned_u64 pds_bgnd0_base;
	u64 pds_bgnd1_base;
	u64 pds_bgnd3_sizeinfo;

	u64 isp_mtile_base;
	/* TQ_MAX_RENDER_TARGETS * PBE_STATE_SIZE */
	u64 pbe_wordx_mrty[3U * ROGUE_PBE_WORDS_REQUIRED_FOR_TQS];
};

struct rogue_fwif_cmd_transfer {
	/* Common command attributes */
	struct rogue_fwif_cmd_common common __aligned(8);

	struct rogue_fwif_transfer_regs regs __aligned(8);

	u32 flags;

	u32 padding;
};


#include "pvr_rogue_fwif_check.h"


#endif /* PVR_ROGUE_FWIF_H */
