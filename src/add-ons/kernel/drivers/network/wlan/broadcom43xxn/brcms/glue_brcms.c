/*
 * Copyright (c) 2010 Broadcom Corporation
 * Copyright 2026, rainygirl <rainygirl@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * SPDX-License-Identifier: ISC
 */

/*!	The half of brcmsmac the phy layer calls back into.

	phy_shim.c is a two way interface: the driver calls into the phy, and
	the phy calls back out through brcms_b_* and brcms_c_* for the things
	only the MAC side can do -- shared memory, object memory, the core
	reset, suspending the sequencer. Those functions are taken from
	brcmsmac's main.c unchanged, since getting the order of register writes
	right by hand is exactly what cost the most on this chip.

	What is written here rather than imported is the surroundings: Linux's
	timers, its interrupt handling and its logging, and the transmit and
	receive paths this driver does not have yet.
*/


#include "linux/types.h"

#include <KernelExport.h>

#include "net/mac80211.h"
#include "linux/ieee80211.h"

#include "linux/bcma/bcma.h"
#include "aiutils.h"
#include "brcmu_utils.h"
#include "chipcommon.h"
#include "chipcommon.h"
#include "chipcommon.h"
#include "d11.h"
#include "debug_haiku.h"
#include "glue.h"
#include "mac80211_if.h"
#include "main.h"
#include "phy/phy_hal.h"
#include "phy_shim.h"
#include "pub.h"
#include "rate.h"
#include "soc.h"
#include "soc.h"
#include "soc.h"
#include "types.h"


#define TRACE(x...)	dprintf("broadcom43xxn: " x)

// From brcmsmac's main.c, where the functions below come from.
#define WL_SPURAVOID_ON2		2
#define BRCMS_USE_COREFLAGS		0xffffffff
#define PHYPLL_WAIT_US			100000

// From brcmsmac's main.c, where the functions below come from.
#define WL_SPURAVOID_ON2		2
#define BRCMS_USE_COREFLAGS		0xffffffff
#define PHYPLL_WAIT_US			100000

// From brcmsmac's main.c, where the functions below come from.
#define WL_SPURAVOID_OFF		0
#define WL_SPURAVOID_ON1		1
#define WL_SPURAVOID_ON2		2
#define BRCMS_USE_COREFLAGS		0xffffffff
#define PHYPLL_WAIT_US			100000


/*!	What the imported sources report a problem to the stack through. */
void
wiphy_err(struct wiphy* wiphy, const char* format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	dprintf("broadcom43xxn: brcms: ");
	dvprintf(format, arguments);
	va_end(arguments);
}


/*!	Taking the device down, which happens here only if the chip has gone. */
void
brcms_down(struct brcms_info* wl)
{
	TRACE("the chip stopped answering\n");
}


// Interrupts are not taken yet: the phy layer is driven from the probe, with
// nothing else running against the card. Masking them off is therefore all
// these have to do.
void
brcms_intrson(struct brcms_info* wl)
{
}


u32
brcms_intrsoff(struct brcms_info* wl)
{
	return 0;
}


void
brcms_intrsrestore(struct brcms_info* wl, u32 macintmask)
{
}


/*!	The multiphase calibration timer.

	The phy layer asks for a timer, arms it for a few milliseconds at a time
	and expects its callback to run with the card to itself.

	A Haiku kernel timer fires in interrupt context, and the phy calibrates
	with udelay(), which is snooze() here and may not be called there. So
	the timer only writes the expiry down and wakes a thread, which is where
	the phy's own code runs.
*/
// Set once the driver is on its way out. The phy re-arms its calibration
// timer from inside the callback, so without this a timer could be put back
// on the list between the cancel and the free.
static bool sTimersStopped;

#define PENDING_MAX		16

static struct brcms_timer* sPending[PENDING_MAX];
static int32 sPendingHead;
static int32 sPendingTail;
static spinlock sPendingLock;
static sem_id sTimerWakeup = -1;
static thread_id sTimerThread = -1;


static int32
timer_fired(timer* which)
{
	struct brcms_timer* t = (struct brcms_timer*)which->user_data;

	t->set = false;

	acquire_spinlock(&sPendingLock);
	int32 next = (sPendingHead + 1) % PENDING_MAX;
	bool room = next != sPendingTail;
	if (room) {
		sPending[sPendingHead] = t;
		sPendingHead = next;
	}
	release_spinlock(&sPendingLock);

	if (!room) {
		// Nothing is lost that matters: the phy asks again next time
		// round. Say so rather than let it look like the card went quiet.
		TRACE("timer queue full, a calibration step was dropped\n");
		return B_HANDLED_INTERRUPT;
	}

	release_sem_etc(sTimerWakeup, 1, B_DO_NOT_RESCHEDULE);

	return B_INVOKE_SCHEDULER;
}


static int32
timer_thread(void* data)
{
	while (acquire_sem(sTimerWakeup) == B_OK) {
		cpu_status state = disable_interrupts();
		acquire_spinlock(&sPendingLock);

		struct brcms_timer* t = NULL;
		if (sPendingTail != sPendingHead) {
			t = sPending[sPendingTail];
			sPendingTail = (sPendingTail + 1) % PENDING_MAX;
		}

		release_spinlock(&sPendingLock);
		restore_interrupts(state);

		if (t != NULL && t->fn != NULL && !sTimersStopped)
			t->fn(t->arg);
	}

	return 0;
}


/*!	Starts the thread the phy's own callbacks run on. */
status_t
brcms_glue_start_timer_thread(void)
{
	if (sTimerThread >= 0)
		return B_OK;

	sTimersStopped = false;
	sPendingHead = 0;
	sPendingTail = 0;
	B_INITIALIZE_SPINLOCK(&sPendingLock);

	sTimerWakeup = create_sem(0, "broadcom43xxn phy timer");
	if (sTimerWakeup < B_OK)
		return sTimerWakeup;

	sTimerThread = spawn_kernel_thread(timer_thread, "broadcom43xxn phy timer",
		B_NORMAL_PRIORITY, NULL);
	if (sTimerThread < B_OK) {
		delete_sem(sTimerWakeup);
		sTimerWakeup = -1;
		return sTimerThread;
	}

	resume_thread(sTimerThread);

	return B_OK;
}


void
brcms_glue_stop_timer_thread(void)
{
	if (sTimerThread < 0)
		return;

	// Deleting the semaphore is what the thread is waiting on, so this is
	// what tells it to finish.
	delete_sem(sTimerWakeup);
	sTimerWakeup = -1;

	status_t result;
	wait_for_thread(sTimerThread, &result);
	sTimerThread = -1;
}


// Every timer handed out is kept on this list. A timer armed when the
// driver image goes away fires into memory that is no longer mapped, and
// the kernel takes a page fault inside the interrupt handler, so nothing
// may be left running when the probe finishes.
static struct brcms_timer* sTimers;




struct brcms_timer*
brcms_init_timer(struct brcms_info* wl, void (*fn)(void* arg), void* arg,
	const char* name)
{
	struct brcms_timer* t = kzalloc_obj(*t, GFP_ATOMIC);
	if (t == NULL)
		return NULL;

	t->wl = wl;
	t->fn = fn;
	t->arg = arg;
	t->dly_wrk.haiku_timer.user_data = t;

	t->next = sTimers;
	sTimers = t;

	return t;
}


void
brcms_add_timer(struct brcms_timer* t, uint ms, int periodic)
{
	if (sTimersStopped)
		return;

	if (t->set)
		cancel_timer(&t->dly_wrk.haiku_timer);

	t->ms = ms;
	t->periodic = periodic != 0;
	t->set = true;
	t->dly_wrk.haiku_timer.user_data = t;

	add_timer(&t->dly_wrk.haiku_timer, timer_fired, (bigtime_t)ms * 1000,
		t->periodic ? B_PERIODIC_TIMER : B_ONE_SHOT_RELATIVE_TIMER);
}


bool
brcms_del_timer(struct brcms_timer* t)
{
	if (!t->set)
		return true;

	t->set = false;
	cancel_timer(&t->dly_wrk.haiku_timer);

	return true;
}


void
brcms_free_timer(struct brcms_timer* t)
{
	brcms_del_timer(t);

	struct brcms_timer** link = &sTimers;
	while (*link != NULL) {
		if (*link == t) {
			*link = t->next;
			break;
		}

		link = &(*link)->next;
	}

	kfree(t);
}


/*!	Disarms and frees everything still outstanding.

	Called before the driver image is let go. Cancelling a Haiku timer waits
	for a callback that is already running, so after this nothing of this
	driver can be entered from an interrupt.
*/
/*!	Disarms every timer and refuses any further arming.

	Cancelling a Haiku timer waits for a callback that is already running,
	so once this returns nothing of this driver can be entered from an
	interrupt again. The timers themselves are left allocated: the phy frees
	its own in wlc_phy_detach(), which must still find them.
*/
void
brcms_glue_quiesce_timers(void)
{
	sTimersStopped = true;

	for (struct brcms_timer* t = sTimers; t != NULL; t = t->next) {
		cancel_timer(&t->dly_wrk.haiku_timer);
		t->set = false;
	}
}


/*!	Frees whatever is still on the list, for a path that gave up partway. */
void
brcms_glue_stop_timers(void)
{
	brcms_glue_quiesce_timers();

	while (sTimers != NULL) {
		struct brcms_timer* t = sTimers;
		sTimers = t->next;
		cancel_timer(&t->dly_wrk.haiku_timer);
		kfree(t);
	}
}


/*!	Puts the card back the way it was found.

	The microcode is running and the interrupt mask is open by the time the
	phy has been initialised. If the driver is unloaded with the card in
	that state it keeps asserting its interrupt line at a kernel that no
	longer has anything to service it, so the sequencer is stopped, the mask
	closed and the core left in reset.
*/
void
brcms_glue_quiesce_hardware(struct brcms_hardware* wlc_hw)
{
	if (wlc_hw->d11core == NULL || !wlc_hw->clk)
		return;

	bcma_write32(wlc_hw->d11core, D11REGOFFS(macintmask), 0);
	bcma_write32(wlc_hw->d11core, D11REGOFFS(macintstatus), -1);

	brcms_b_mctrl(wlc_hw, MCTL_PSM_RUN | MCTL_EN_MAC, 0);

	bcma_core_disable(wlc_hw->d11core, 0);
	wlc_hw->clk = false;
	wlc_hw->ucode_loaded = false;
}


// What the imported sources log through. Everything lands in the syslog at
// one level, which is what a bring up wants.
void
__brcms_err(struct device* dev, const char* format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	dprintf("broadcom43xxn: brcms: ");
	dvprintf(format, arguments);
	va_end(arguments);
}


void
__brcms_info(struct device* dev, const char* format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	dprintf("broadcom43xxn: brcms: ");
	dvprintf(format, arguments);
	va_end(arguments);
}


void
__brcms_warn(struct device* dev, const char* format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	dprintf("broadcom43xxn: brcms: ");
	dvprintf(format, arguments);
	va_end(arguments);
}


void
__brcms_crit(struct device* dev, const char* format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	dprintf("broadcom43xxn: brcms: ");
	dvprintf(format, arguments);
	va_end(arguments);
}


// The LCN phy belongs to the BCM4313 and is not built; these are what the
// common phy code calls when it decides it is looking at one.
bool wlc_phy_attach_lcnphy(struct brcms_phy* pi) { return false; }
void wlc_lcnphy_calib_modes(struct brcms_phy* pi, uint mode) {}
void wlc_lcnphy_deaf_mode(struct brcms_phy* pi, bool mode) {}
void wlc_lcnphy_get_tssi(struct brcms_phy* pi, s8* ofdm_pwr, s8* cck_pwr) {}
s32 wlc_lcnphy_rx_signal_power(struct brcms_phy* pi, s32 gain_index)
	{ return 0; }
s8 wlc_lcnphy_tempsense_degree(struct brcms_phy* pi, bool mode) { return 0; }
s8 wlc_lcnphy_vbatsense(struct brcms_phy* pi, bool mode) { return 0; }
bool wlc_phy_tpc_isenabled_lcnphy(struct brcms_phy* pi) { return false; }


/* --- what follows is brcmsmac's own main.c, unchanged --- */

/* main.c s own constants and tables, taken whole. */


static u16 get_sifs(struct brcms_band* band);

static void
brcms_c_mhfdef(struct brcms_c_info *wlc, u16 *mhfs, u16 mhf2_init)
{
	struct brcms_hardware *wlc_hw = wlc->hw;

	memset(mhfs, 0, MHFMAX * sizeof(u16));

	mhfs[MHF2] |= mhf2_init;

	/* prohibit use of slowclock on multifunction boards */
	if (wlc_hw->boardflags & BFL_NOPLLDOWN)
		mhfs[MHF1] |= MHF1_FORCEFASTCLK;

	if (BRCMS_ISNPHY(wlc_hw->band) && NREV_LT(wlc_hw->band->phyrev, 2)) {
		mhfs[MHF2] |= MHF2_NPHY40MHZ_WAR;
		mhfs[MHF1] |= MHF1_IQSWAP_WAR;
	}
}

static uint
dmareg(uint direction, uint fifonum)
{
	if (direction == DMA_TX)
		return offsetof(struct d11regs, fifo64regs[fifonum].dmaxmt);
	return offsetof(struct d11regs, fifo64regs[fifonum].dmarcv);
}

#define TIMER_INTERVAL_WATCHDOG		1000
/* radio monitor timer, in unit of ms */
#define TIMER_INTERVAL_RADIOCHK		800

/* beacon interval, in unit of 1024TU */
#define BEACON_INTERVAL_DEFAULT		100

/* n-mode support capability */
/* 2x2 includes both 1x1 & 2x2 devices
 * reserved #define 2 for future when we want to separate 1x1 & 2x2 and
 * control it independently
 */
#define WL_11N_2x2			1
#define WL_11N_3x3			3
#define WL_11N_4x4			4

#define EDCF_ACI_MASK			0x60
#define EDCF_ACI_SHIFT			5
#define EDCF_ECWMIN_MASK		0x0f
#define EDCF_ECWMAX_SHIFT		4
#define EDCF_AIFSN_MASK			0x0f
#define EDCF_AIFSN_MAX			15
#define EDCF_ECWMAX_MASK		0xf0

#define EDCF_AC_BE_TXOP_STA		0x0000
#define EDCF_AC_BK_TXOP_STA		0x0000
#define EDCF_AC_VO_ACI_STA		0x62
#define EDCF_AC_VO_ECW_STA		0x32
#define EDCF_AC_VI_ACI_STA		0x42
#define EDCF_AC_VI_ECW_STA		0x43
#define EDCF_AC_BK_ECW_STA		0xA4
#define EDCF_AC_VI_TXOP_STA		0x005e
#define EDCF_AC_VO_TXOP_STA		0x002f
#define EDCF_AC_BE_ACI_STA		0x03
#define EDCF_AC_BE_ECW_STA		0xA4
#define EDCF_AC_BK_ACI_STA		0x27
#define EDCF_AC_VO_TXOP_AP		0x002f

#define EDCF_TXOP2USEC(txop)		((txop) << 5)
#define EDCF_ECW2CW(exp)		((1 << (exp)) - 1)

#define APHY_SYMBOL_TIME		4
#define APHY_PREAMBLE_TIME		16
#define APHY_SIGNAL_TIME		4
#define APHY_SIFS_TIME			16
#define APHY_SERVICE_NBITS		16
#define APHY_TAIL_NBITS			6
#define BPHY_SIFS_TIME			10
#define BPHY_PLCP_SHORT_TIME		96

#define PREN_PREAMBLE			24
#define PREN_MM_EXT			12
#define PREN_PREAMBLE_EXT		4

#define DOT11_MAC_HDR_LEN		24
#define DOT11_ACK_LEN			10
#define DOT11_BA_LEN			4
#define DOT11_OFDM_SIGNAL_EXTENSION	6
#define DOT11_MIN_FRAG_LEN		256
#define DOT11_RTS_LEN			16
#define DOT11_CTS_LEN			10

// The check sequence every frame carries, which the times below count.
#ifndef FCS_LEN
#	define FCS_LEN			4
#endif
#define DOT11_BA_BITMAP_LEN		128
#define DOT11_MAXNUMFRAGS		16
#define DOT11_MAX_FRAG_LEN		2346

#define BPHY_PLCP_TIME			192
#define RIFS_11N_TIME			2

/* length of the BCN template area */
#define BCN_TMPL_LEN			512

/* brcms_bss_info flag bit values */
#define BRCMS_BSS_HT			0x0020	/* BSS is HT (MIMO) capable */

/* chip rx buffer offset */
#define BRCMS_HWRXOFF			38

/* rfdisable delay timer 500 ms, runs of ALP clock */
#define RFDISABLE_DEFAULT		10000000

#define BRCMS_TEMPSENSE_PERIOD		10	/* 10 second timeout */

/* synthpu_dly times in us */
#define SYNTHPU_DLY_APHY_US		3700
#define SYNTHPU_DLY_BPHY_US		1050
#define SYNTHPU_DLY_NPHY_US		2048
#define SYNTHPU_DLY_LPPHY_US		300

#define ANTCNT				10	/* vanilla M_MAX_ANTCNT val */

/* Per-AC retry limit register definitions; uses defs.h bitfield macros */
#define EDCF_SHORT_S			0
#define EDCF_SFB_S			4
#define EDCF_LONG_S			8
#define EDCF_LFB_S			12
#define EDCF_SHORT_M			BITFIELD_MASK(4)
#define EDCF_SFB_M			BITFIELD_MASK(4)
#define EDCF_LONG_M			BITFIELD_MASK(4)
#define EDCF_LFB_M			BITFIELD_MASK(4)

#define RETRY_SHORT_DEF			7	/* Default Short retry Limit */
#define RETRY_SHORT_MAX			255	/* Maximum Short retry Limit */
#define RETRY_LONG_DEF			4	/* Default Long retry count */
#define RETRY_SHORT_FB			3	/* Short count for fb rate */
#define RETRY_LONG_FB			2	/* Long count for fb rate */

#define APHY_CWMIN			15
#define PHY_CWMAX			1023

#define EDCF_AIFSN_MIN			1

#define FRAGNUM_MASK			0xF

#define APHY_SLOT_TIME			9
#define BPHY_SLOT_TIME			20

#define WL_SPURAVOID_OFF		0
#define WL_SPURAVOID_ON1		1
#define WL_SPURAVOID_ON2		2

/* invalid core flags, use the saved coreflags */
#define BRCMS_USE_COREFLAGS		0xffffffff

/* values for PLCPHdr_override */
#define BRCMS_PLCP_AUTO			-1
#define BRCMS_PLCP_SHORT		0
#define BRCMS_PLCP_LONG			1

/* values for g_protection_override and n_protection_override */
#define BRCMS_PROTECTION_AUTO		-1
#define BRCMS_PROTECTION_OFF		0
#define BRCMS_PROTECTION_ON		1
#define BRCMS_PROTECTION_MMHDR_ONLY	2
#define BRCMS_PROTECTION_CTS_ONLY	3

/* values for g_protection_control and n_protection_control */
#define BRCMS_PROTECTION_CTL_OFF	0
#define BRCMS_PROTECTION_CTL_LOCAL	1
#define BRCMS_PROTECTION_CTL_OVERLAP	2

/* values for n_protection */
#define BRCMS_N_PROTECTION_OFF		0
#define BRCMS_N_PROTECTION_OPTIONAL	1
#define BRCMS_N_PROTECTION_20IN40	2
#define BRCMS_N_PROTECTION_MIXEDMODE	3

/* values for band specific 40MHz capabilities */
#define BRCMS_N_BW_20ALL		0
#define BRCMS_N_BW_40ALL		1
#define BRCMS_N_BW_20IN2G_40IN5G	2

/* bitflags for SGI support (sgi_rx iovar) */
#define BRCMS_N_SGI_20			0x01
#define BRCMS_N_SGI_40			0x02

/* defines used by the nrate iovar */
/* MSC in use,indicates b0-6 holds an mcs */
#define NRATE_MCS_INUSE			0x00000080
/* rate/mcs value */
#define NRATE_RATE_MASK			0x0000007f
/* stf mode mask: siso, cdd, stbc, sdm */
#define NRATE_STF_MASK			0x0000ff00
/* stf mode shift */
#define NRATE_STF_SHIFT			8
/* bit indicate to override mcs only */
#define NRATE_OVERRIDE_MCS_ONLY		0x40000000
#define NRATE_SGI_MASK			0x00800000	/* sgi mode */
#define NRATE_SGI_SHIFT			23		/* sgi mode */
#define NRATE_LDPC_CODING		0x00400000	/* adv coding in use */
#define NRATE_LDPC_SHIFT		22		/* ldpc shift */

#define NRATE_STF_SISO			0		/* stf mode SISO */
#define NRATE_STF_CDD			1		/* stf mode CDD */
#define NRATE_STF_STBC			2		/* stf mode STBC */
#define NRATE_STF_SDM			3		/* stf mode SDM */

#define MAX_DMA_SEGS			4

/* # of entries in Tx FIFO */
#define NTXD				64
/* Max # of entries in Rx FIFO based on 4kb page size */
#define NRXD				512

/* Amount of headroom to leave in Tx FIFO */
#define TX_HEADROOM			4

/* try to keep this # rbufs posted to the chip */
#define NRXBUFPOST			511

/* max # frames to process in brcms_c_recv() */
#define RXBND				8
/* max # tx status to process in wlc_txstatus() */
#define TXSBND				8

/*
 * The following table lists the buffer memory allocated to xmt fifos in HW.
 * the size is in units of 256bytes(one block), total size is HW dependent
 * ucode has default fifo partition, sw can overwrite if necessary
 *
 * This is documented in twiki under the topic UcodeTxFifo. Please ensure
 * the twiki is updated before making changes.
 */

/* Starting corerev for the fifo size table */
#define XMTFIFOTBL_STARTREV	17

struct d11init {
	__le16 addr;
	__le16 size;
	__le32 value;
};

struct edcf_acparam {
	u8 ACI;
	u8 ECW;
	u16 TXOP;
} __packed;

/* debug/trace */
uint brcm_msg_level;

/* TX FIFO number to WME/802.1E Access Category */
static const u8 wme_fifo2ac[] = {
	IEEE80211_AC_BK,
	IEEE80211_AC_BE,
	IEEE80211_AC_VI,
	IEEE80211_AC_VO,
	IEEE80211_AC_BE,
	IEEE80211_AC_BE
};

/* ieee80211 Access Category to TX FIFO number */
static const u8 wme_ac2fifo[] = {
	TX_AC_VO_FIFO,
	TX_AC_VI_FIFO,
	TX_AC_BE_FIFO,
	TX_AC_BK_FIFO
};

static const u16 xmtfifo_sz[][NFIFO] = {
	/* corerev 17: 5120, 49152, 49152, 5376, 4352, 1280 */
	{20, 192, 192, 21, 17, 5},
	/* corerev 18: */
	{0, 0, 0, 0, 0, 0},
	/* corerev 19: */
	{0, 0, 0, 0, 0, 0},
	/* corerev 20: 5120, 49152, 49152, 5376, 4352, 1280 */
	{20, 192, 192, 21, 17, 5},
	/* corerev 21: 2304, 14848, 5632, 3584, 3584, 1280 */
	{9, 58, 22, 14, 14, 5},
	/* corerev 22: 5120, 49152, 49152, 5376, 4352, 1280 */
	{20, 192, 192, 21, 17, 5},
	/* corerev 23: 5120, 49152, 49152, 5376, 4352, 1280 */
	{20, 192, 192, 21, 17, 5},
	/* corerev 24: 2304, 14848, 5632, 3584, 3584, 1280 */
	{9, 58, 22, 14, 14, 5},
	/* corerev 25: */
	{0, 0, 0, 0, 0, 0},
	/* corerev 26: */
	{0, 0, 0, 0, 0, 0},
	/* corerev 27: */
	{0, 0, 0, 0, 0, 0},
	/* corerev 28: 2304, 14848, 5632, 3584, 3584, 1280 */
	{9, 58, 22, 14, 14, 5},
};

#ifdef DEBUG
static const char * const fifo_names[] = {
	"AC_BK", "AC_BE", "AC_VI", "AC_VO", "BCMC", "ATIM" };
#else
static const char fifo_names[6][1];
#endif

#ifdef DEBUG
/* pointer to most recently allocated wl/wlc */
static struct brcms_c_info *wlc_info_dbg = (struct brcms_c_info *) (NULL);
#endif

/* Mapping of ieee80211 AC numbers to tx fifos */
static const u8 ac_to_fifo_mapping[IEEE80211_NUM_ACS] = {
	[IEEE80211_AC_VO]	= TX_AC_VO_FIFO,
	[IEEE80211_AC_VI]	= TX_AC_VI_FIFO,
	[IEEE80211_AC_BE]	= TX_AC_BE_FIFO,
	[IEEE80211_AC_BK]	= TX_AC_BK_FIFO,
};

/* Mapping of tx fifos to ieee80211 AC numbers */
static const u8 fifo_to_ac_mapping[IEEE80211_NUM_ACS] = {
	[TX_AC_BK_FIFO]	= IEEE80211_AC_BK,
	[TX_AC_BE_FIFO]	= IEEE80211_AC_BE,
	[TX_AC_VI_FIFO]	= IEEE80211_AC_VI,
	[TX_AC_VO_FIFO]	= IEEE80211_AC_VO,
};


static u16
brcms_b_read_objmem(struct brcms_hardware *wlc_hw, uint offset, u32 sel)
{
	struct bcma_device *core = wlc_hw->d11core;
	u16 objoff = D11REGOFFS(objdata);
	u16 value;

	unsigned long held = brcms_glue_window_hold();

	bcma_write32(core, D11REGOFFS(objaddr), sel | (offset >> 2));
	(void)bcma_read32(core, D11REGOFFS(objaddr));
	if (offset & 2)
		objoff += 2;

	value = bcma_read16(core, objoff);

	brcms_glue_window_release(held);
	return value;
}

static void
brcms_b_write_objmem(struct brcms_hardware *wlc_hw, uint offset, u16 v,
		     u32 sel)
{
	struct bcma_device *core = wlc_hw->d11core;
	u16 objoff = D11REGOFFS(objdata);

	unsigned long held = brcms_glue_window_hold();

	bcma_write32(core, D11REGOFFS(objaddr), sel | (offset >> 2));
	(void)bcma_read32(core, D11REGOFFS(objaddr));
	if (offset & 2)
		objoff += 2;

	bcma_wflush16(core, objoff, v);

	brcms_glue_window_release(held);
}

void
brcms_b_mhf(struct brcms_hardware *wlc_hw, u8 idx, u16 mask, u16 val,
	     int bands)
{
	u16 save;
	u16 addr[MHFMAX] = {
		M_HOST_FLAGS1, M_HOST_FLAGS2, M_HOST_FLAGS3, M_HOST_FLAGS4,
		M_HOST_FLAGS5
	};
	struct brcms_hw_band *band;

	if ((val & ~mask) || idx >= MHFMAX)
		return; /* error condition */

	switch (bands) {
		/* Current band only or all bands,
		 * then set the band to current band
		 */
	case BRCM_BAND_AUTO:
	case BRCM_BAND_ALL:
		band = wlc_hw->band;
		break;
	case BRCM_BAND_5G:
		band = wlc_hw->bandstate[BAND_5G_INDEX];
		break;
	case BRCM_BAND_2G:
		band = wlc_hw->bandstate[BAND_2G_INDEX];
		break;
	default:
		band = NULL;	/* error condition */
	}

	if (band) {
		save = band->mhfs[idx];
		band->mhfs[idx] = (band->mhfs[idx] & ~mask) | val;

		/* optimization: only write through if changed, and
		 * changed band is the current band
		 */
		if (wlc_hw->clk && (band->mhfs[idx] != save)
		    && (band == wlc_hw->band))
			brcms_b_write_shm(wlc_hw, addr[idx],
					   (u16) band->mhfs[idx]);
	}

	if (bands == BRCM_BAND_ALL) {
		wlc_hw->bandstate[0]->mhfs[idx] =
		    (wlc_hw->bandstate[0]->mhfs[idx] & ~mask) | val;
		wlc_hw->bandstate[1]->mhfs[idx] =
		    (wlc_hw->bandstate[1]->mhfs[idx] & ~mask) | val;
	}
}

void
brcms_b_write_template_ram(struct brcms_hardware *wlc_hw, int offset, int len,
			    void *buf)
{
	struct bcma_device *core = wlc_hw->d11core;
	u32 word;
	__le32 word_le;
	__be32 word_be;
	bool be_bit;
	brcms_dbg_info(core, "wl%d\n", wlc_hw->unit);

	bcma_write32(core, D11REGOFFS(tplatewrptr), offset);

	/* if MCTL_BIGEND bit set in mac control register,
	 * the chip swaps data in fifo, as well as data in
	 * template ram
	 */
	be_bit = (bcma_read32(core, D11REGOFFS(maccontrol)) & MCTL_BIGEND) != 0;

	while (len > 0) {
		memcpy(&word, buf, sizeof(u32));

		if (be_bit) {
			word_be = cpu_to_be32(word);
			word = *(u32 *)&word_be;
		} else {
			word_le = cpu_to_le32(word);
			word = *(u32 *)&word_le;
		}

		bcma_write32(core, D11REGOFFS(tplatewrdata), word);

		buf = (u8 *) buf + sizeof(u32);
		len -= sizeof(u32);
	}
}

void
brcms_b_copyto_objmem(struct brcms_hardware *wlc_hw, uint offset,
		      const void *buf, int len, u32 sel)
{
	u16 v;
	const u8 *p = (const u8 *)buf;
	int i;

	if (len <= 0 || (offset & 1) || (len & 1))
		return;

	for (i = 0; i < len; i += 2) {
		v = p[i] | (p[i + 1] << 8);
		brcms_b_write_objmem(wlc_hw, offset + i, v, sel);
	}
}

void
brcms_b_copyfrom_objmem(struct brcms_hardware *wlc_hw, uint offset, void *buf,
			 int len, u32 sel)
{
	u16 v;
	u8 *p = (u8 *) buf;
	int i;

	if (len <= 0 || (offset & 1) || (len & 1))
		return;

	for (i = 0; i < len; i += 2) {
		v = brcms_b_read_objmem(wlc_hw, offset + i, sel);
		p[i] = v & 0xFF;
		p[i + 1] = (v >> 8) & 0xFF;
	}
}

bool
brcms_b_recv(struct brcms_hardware *wlc_hw, uint fifo, bool bound)
{
	struct sk_buff *p;
	struct sk_buff *next = NULL;
	struct sk_buff_head recv_frames;

	uint n = 0;
	uint bound_limit = bound ? RXBND : -1;
	bool morepending = false;

	skb_queue_head_init(&recv_frames);

	/* gather received frames */
	do {
		/* !give others some time to run! */
		if (n >= bound_limit)
			break;

		morepending = dma_rx(wlc_hw->di[fifo], &recv_frames);
		n++;
	} while (morepending);

	/* post more rbufs */
	dma_rxfill(wlc_hw->di[fifo]);

	/* process each frame */
	skb_queue_walk_safe(&recv_frames, p, next) {
		struct d11rxhdr_le *rxh_le;
		struct d11rxhdr *rxh;

		skb_unlink(p, &recv_frames);
		rxh_le = (struct d11rxhdr_le *)p->data;
		rxh = (struct d11rxhdr *)p->data;

		/* fixup rx header endianness */
		rxh->RxFrameSize = le16_to_cpu(rxh_le->RxFrameSize);
		rxh->PhyRxStatus_0 = le16_to_cpu(rxh_le->PhyRxStatus_0);
		rxh->PhyRxStatus_1 = le16_to_cpu(rxh_le->PhyRxStatus_1);
		rxh->PhyRxStatus_2 = le16_to_cpu(rxh_le->PhyRxStatus_2);
		rxh->PhyRxStatus_3 = le16_to_cpu(rxh_le->PhyRxStatus_3);
		rxh->PhyRxStatus_4 = le16_to_cpu(rxh_le->PhyRxStatus_4);
		rxh->PhyRxStatus_5 = le16_to_cpu(rxh_le->PhyRxStatus_5);
		rxh->RxStatus1 = le16_to_cpu(rxh_le->RxStatus1);
		rxh->RxStatus2 = le16_to_cpu(rxh_le->RxStatus2);
		rxh->RxTSFTime = le16_to_cpu(rxh_le->RxTSFTime);
		rxh->RxChan = le16_to_cpu(rxh_le->RxChan);

		brcms_c_recv(wlc_hw->wlc, p);
	}

	return morepending;
}

void
brcms_b_set_addrmatch(struct brcms_hardware *wlc_hw, int match_reg_offset,
		       const u8 *addr)
{
	struct bcma_device *core = wlc_hw->d11core;
	u16 mac_l;
	u16 mac_m;
	u16 mac_h;

	brcms_dbg_rx(core, "wl%d: brcms_b_set_addrmatch\n", wlc_hw->unit);

	mac_l = addr[0] | (addr[1] << 8);
	mac_m = addr[2] | (addr[3] << 8);
	mac_h = addr[4] | (addr[5] << 8);

	/* enter the MAC addr into the RXE match registers */
	bcma_write16(core, D11REGOFFS(rcm_ctl),
		     RCM_INC_DATA | match_reg_offset);
	bcma_write16(core, D11REGOFFS(rcm_mat_data), mac_l);
	bcma_write16(core, D11REGOFFS(rcm_mat_data), mac_m);
	bcma_write16(core, D11REGOFFS(rcm_mat_data), mac_h);
}

static u8 brcms_basic_rate(struct brcms_c_info *wlc, u32 rspec)
{
	if (is_mcs_rate(rspec))
		return wlc->band->basic_rate[mcs_table[rspec & RSPEC_RATE_MASK]
		       .leg_ofdm];
	return wlc->band->basic_rate[rspec & RSPEC_RATE_MASK];
}

static bool brcms_deviceremoved(struct brcms_c_info *wlc)
{
	u32 macctrl;

	if (!wlc->hw->clk)
		return ai_deviceremoved(wlc->hw->sih);
	macctrl = bcma_read32(wlc->hw->d11core,
			      D11REGOFFS(maccontrol));
	return (macctrl & (MCTL_PSM_JMP_0 | MCTL_IHR_EN)) != MCTL_IHR_EN;
}

static int brcms_chspec_bw(u16 chanspec)
{
	if (CHSPEC_IS40(chanspec))
		return BRCMS_40_MHZ;
	if (CHSPEC_IS20(chanspec))
		return BRCMS_20_MHZ;

	return BRCMS_10_MHZ;
}

void brcms_b_update_slot_timing(struct brcms_hardware *wlc_hw,
					bool shortslot)
{
	struct bcma_device *core = wlc_hw->d11core;

	if (shortslot) {
		/* 11g short slot: 11a timing */
		bcma_write16(core, D11REGOFFS(ifs_slot), 0x0207);
		brcms_b_write_shm(wlc_hw, M_DOT11_SLOT, APHY_SLOT_TIME);
	} else {
		/* 11g long slot: 11b timing */
		bcma_write16(core, D11REGOFFS(ifs_slot), 0x0212);
		brcms_b_write_shm(wlc_hw, M_DOT11_SLOT, BPHY_SLOT_TIME);
	}
}

static void brcms_c_write_inits(struct brcms_hardware *wlc_hw,
				const struct d11init *inits)
{
	struct bcma_device *core = wlc_hw->d11core;
	int i;
	uint offset;
	u16 size;
	u32 value;

	brcms_dbg_info(wlc_hw->d11core, "wl%d\n", wlc_hw->unit);

	// The table contains object-address/data pairs, not independent writes.
	unsigned long held = brcms_glue_window_hold();

	for (i = 0; inits[i].addr != cpu_to_le16(0xffff); i++) {
		size = le16_to_cpu(inits[i].size);
		offset = le16_to_cpu(inits[i].addr);
		value = le32_to_cpu(inits[i].value);
		if (size == 2)
			bcma_write16(core, offset, value);
		else if (size == 4)
			bcma_write32(core, offset, value);
		else
			break;
	}
	brcms_glue_window_release(held);
}

static void brcms_c_write_mhf(struct brcms_hardware *wlc_hw, u16 *mhfs)
{
	u8 idx;
	static const u16 addr[] = {
		M_HOST_FLAGS1, M_HOST_FLAGS2, M_HOST_FLAGS3, M_HOST_FLAGS4,
		M_HOST_FLAGS5
	};

	for (idx = 0; idx < MHFMAX; idx++)
		brcms_b_write_shm(wlc_hw, addr[idx], mhfs[idx]);
}

static void brcms_c_ucode_bsinit(struct brcms_hardware *wlc_hw)
{
	struct brcms_ucode *ucode = &wlc_hw->wlc->wl->ucode;

	/* init microcode host flags */
	brcms_c_write_mhf(wlc_hw, wlc_hw->band->mhfs);

	/* do band-specific ucode IHR, SHM, and SCR inits */
	if (D11REV_IS(wlc_hw->corerev, 17) || D11REV_IS(wlc_hw->corerev, 23)) {
		if (BRCMS_ISNPHY(wlc_hw->band))
			brcms_c_write_inits(wlc_hw, ucode->d11n0bsinitvals16);
		else
			brcms_err(wlc_hw->d11core,
				  "%s: wl%d: unsupported phy in corerev %d\n",
				  __func__, wlc_hw->unit,
				  wlc_hw->corerev);
	} else {
		if (D11REV_IS(wlc_hw->corerev, 24)) {
			if (BRCMS_ISLCNPHY(wlc_hw->band))
				brcms_c_write_inits(wlc_hw,
						    ucode->d11lcn0bsinitvals24);
			else
				brcms_err(wlc_hw->d11core,
					  "%s: wl%d: unsupported phy in core rev %d\n",
					  __func__, wlc_hw->unit,
					  wlc_hw->corerev);
		} else {
			brcms_err(wlc_hw->d11core,
				  "%s: wl%d: unsupported corerev %d\n",
				  __func__, wlc_hw->unit, wlc_hw->corerev);
		}
	}
}

static void brcms_b_core_ioctl(struct brcms_hardware *wlc_hw, u32 m, u32 v)
{
	struct bcma_device *core = wlc_hw->d11core;
	u32 ioctl = bcma_aread32(core, BCMA_IOCTL) & ~m;

	bcma_awrite32(core, BCMA_IOCTL, ioctl | v);
}

static void brcms_b_core_phy_clk(struct brcms_hardware *wlc_hw, bool clk)
{
	brcms_dbg_info(wlc_hw->d11core, "wl%d: clk %d\n", wlc_hw->unit, clk);

	wlc_hw->phyclk = clk;

	if (OFF == clk) {	/* clear gmode bit, put phy into reset */

		brcms_b_core_ioctl(wlc_hw, (SICF_PRST | SICF_FGC | SICF_GMODE),
				   (SICF_PRST | SICF_FGC));
		udelay(1);
		brcms_b_core_ioctl(wlc_hw, (SICF_PRST | SICF_FGC), SICF_PRST);
		udelay(1);

	} else {		/* take phy out of reset */

		brcms_b_core_ioctl(wlc_hw, (SICF_PRST | SICF_FGC), SICF_FGC);
		udelay(1);
		brcms_b_core_ioctl(wlc_hw, SICF_FGC, 0);
		udelay(1);

	}
}

void brcms_c_setxband(struct brcms_hardware *wlc_hw, uint bandunit)
{
	brcms_dbg_mac80211(wlc_hw->d11core, "wl%d: bandunit %d\n", wlc_hw->unit,
			   bandunit);

	wlc_hw->band = wlc_hw->bandstate[bandunit];

	/*
	 * BMAC_NOTE:
	 *   until we eliminate need for wlc->band refs in low level code
	 */
	wlc_hw->wlc->band = wlc_hw->wlc->bandstate[bandunit];

	/* set gmode core flag */
	if (wlc_hw->sbclk && !wlc_hw->noreset) {
		u32 gmode = 0;

		if (bandunit == 0)
			gmode = SICF_GMODE;

		brcms_b_core_ioctl(wlc_hw, SICF_GMODE, gmode);
	}
}

static void brcms_c_tbtt(struct brcms_c_info *wlc)
{
	if (wlc->bsscfg->type == BRCMS_TYPE_ADHOC)
		/*
		 * DirFrmQ is now valid...defer setting until end
		 * of ATIM window
		 */
		wlc->qvalid |= MCMD_DIRFRMQVAL;
}

bool brcms_b_attach_dmapio(struct brcms_c_info *wlc, uint j, bool wme)
{
	uint i;
	char name[8];
	/*
	 * ucode host flag 2 needed for pio mode, independent of band and fifo
	 */
	u16 pio_mhf2 = 0;
	struct brcms_hardware *wlc_hw = wlc->hw;
	uint unit = wlc_hw->unit;

	/* name and offsets for dma_attach */
	snprintf(name, sizeof(name), "wl%d", unit);

	if (wlc_hw->di[0] == NULL) {	/* Init FIFOs */
		int dma_attach_err = 0;

		/*
		 * FIFO 0
		 * TX: TX_AC_BK_FIFO (TX AC Background data packets)
		 * RX: RX_FIFO (RX data packets)
		 */
		wlc_hw->di[0] = dma_attach(name, wlc,
					   (wme ? dmareg(DMA_TX, 0) : 0),
					   dmareg(DMA_RX, 0),
					   (wme ? NTXD : 0), NRXD,
					   RXBUFSZ, -1, NRXBUFPOST,
					   BRCMS_HWRXOFF);
		dma_attach_err |= (NULL == wlc_hw->di[0]);

		/*
		 * FIFO 1
		 * TX: TX_AC_BE_FIFO (TX AC Best-Effort data packets)
		 *   (legacy) TX_DATA_FIFO (TX data packets)
		 * RX: UNUSED
		 */
		wlc_hw->di[1] = dma_attach(name, wlc,
					   dmareg(DMA_TX, 1), 0,
					   NTXD, 0, 0, -1, 0, 0);
		dma_attach_err |= (NULL == wlc_hw->di[1]);

		/*
		 * FIFO 2
		 * TX: TX_AC_VI_FIFO (TX AC Video data packets)
		 * RX: UNUSED
		 */
		wlc_hw->di[2] = dma_attach(name, wlc,
					   dmareg(DMA_TX, 2), 0,
					   NTXD, 0, 0, -1, 0, 0);
		dma_attach_err |= (NULL == wlc_hw->di[2]);
		/*
		 * FIFO 3
		 * TX: TX_AC_VO_FIFO (TX AC Voice data packets)
		 *   (legacy) TX_CTL_FIFO (TX control & mgmt packets)
		 */
		wlc_hw->di[3] = dma_attach(name, wlc,
					   dmareg(DMA_TX, 3),
					   0, NTXD, 0, 0, -1,
					   0, 0);
		dma_attach_err |= (NULL == wlc_hw->di[3]);
/* Cleaner to leave this as if with AP defined */

		if (dma_attach_err) {
			brcms_err(wlc_hw->d11core,
				  "wl%d: wlc_attach: dma_attach failed\n",
				  unit);
			return false;
		}

		/* get pointer to dma engine tx flow control variable */
		for (i = 0; i < NFIFO; i++)
			if (wlc_hw->di[i])
				wlc_hw->txavail[i] =
				    (uint *) dma_getvar(wlc_hw->di[i],
							"&txavail");
	}

	/* initial ucode host flags */
	brcms_c_mhfdef(wlc, wlc_hw->band->mhfs, pio_mhf2);

	return true;
}

void brcms_b_info_init(struct brcms_hardware *wlc_hw)
{
	struct brcms_c_info *wlc = wlc_hw->wlc;

	/* set default sw macintmask value */
	wlc->defmacintmask = DEF_MACINTMASK;

	/* various 802.11g modes */
	wlc_hw->shortslot = false;

	wlc_hw->SFBL = RETRY_SHORT_FB;
	wlc_hw->LFBL = RETRY_LONG_FB;

	/* default mac retry limits */
	wlc_hw->SRL = RETRY_SHORT_DEF;
	wlc_hw->LRL = RETRY_LONG_DEF;
	wlc_hw->chanspec = ch20mhz_chspec(1);
}

void brcms_b_wait_for_wake(struct brcms_hardware *wlc_hw)
{
	/* delay before first read of ucode state */
	udelay(40);

	/* wait until ucode is no longer asleep */
	SPINWAIT((brcms_b_read_shm(wlc_hw, M_UCODE_DBGST) ==
		  DBGST_ASLEEP), wlc_hw->wlc->fastpwrup_dly);
}

void brcms_b_clkctl_clk(struct brcms_hardware *wlc_hw, enum bcma_clkmode mode)
{
	if (ai_get_cccaps(wlc_hw->sih) & CC_CAP_PMU) {
		/* new chips with PMU, CCS_FORCEHT will distribute the HT clock
		 * on backplane, but mac core will still run on ALP(not HT) when
		 * it enters powersave mode, which means the FCA bit may not be
		 * set. Should wakeup mac if driver wants it to run on HT.
		 */

		if (wlc_hw->clk) {
			if (mode == BCMA_CLKMODE_FAST) {
				bcma_set32(wlc_hw->d11core,
					   D11REGOFFS(clk_ctl_st),
					   CCS_FORCEHT);

				udelay(64);

				SPINWAIT(
				    ((bcma_read32(wlc_hw->d11core,
				      D11REGOFFS(clk_ctl_st)) &
				      CCS_HTAVAIL) == 0),
				      PMU_MAX_TRANSITION_DLY);
				WARN_ON(!(bcma_read32(wlc_hw->d11core,
					D11REGOFFS(clk_ctl_st)) &
					CCS_HTAVAIL));
			} else {
				if ((ai_get_pmurev(wlc_hw->sih) == 0) &&
				    (bcma_read32(wlc_hw->d11core,
					D11REGOFFS(clk_ctl_st)) &
					(CCS_FORCEHT | CCS_HTAREQ)))
					SPINWAIT(
					    ((bcma_read32(wlc_hw->d11core,
					      offsetof(struct d11regs,
						       clk_ctl_st)) &
					      CCS_HTAVAIL) == 0),
					      PMU_MAX_TRANSITION_DLY);
				bcma_mask32(wlc_hw->d11core,
					D11REGOFFS(clk_ctl_st),
					~CCS_FORCEHT);
			}
		}
		wlc_hw->forcefastclk = (mode == BCMA_CLKMODE_FAST);
	} else {

		/* old chips w/o PMU, force HT through cc,
		 * then use FCA to verify mac is running fast clock
		 */

		wlc_hw->forcefastclk = ai_clkctl_cc(wlc_hw->sih, mode);

		/* check fast clock is available (if core is not in reset) */
		if (wlc_hw->forcefastclk && wlc_hw->clk)
			WARN_ON(!(bcma_aread32(wlc_hw->d11core, BCMA_IOST) &
				  SISF_FCLKA));

		/*
		 * keep the ucode wake bit on if forcefastclk is on since we
		 * do not want ucode to put us back to slow clock when it dozes
		 * for PM mode. Code below matches the wake override bit with
		 * current forcefastclk state. Only setting bit in wake_override
		 * instead of waking ucode immediately since old code had this
		 * behavior. Older code set wlc->forcefastclk but only had the
		 * wake happen if the wakup_ucode work (protected by an up
		 * check) was executed just below.
		 */
		if (wlc_hw->forcefastclk)
			mboolset(wlc_hw->wake_override,
				 BRCMS_WAKE_OVERRIDE_FORCEFAST);
		else
			mboolclr(wlc_hw->wake_override,
				 BRCMS_WAKE_OVERRIDE_FORCEFAST);
	}
}

static void brcms_c_mctrl_reset(struct brcms_hardware *wlc_hw)
{
	/* IHR accesses are always enabled, PSM disabled, HPS off and WAKE on */
	wlc_hw->maccontrol = 0;
	wlc_hw->suspended_fifos = 0;
	wlc_hw->wake_override = 0;
	wlc_hw->mute_override = 0;
	brcms_b_mctrl(wlc_hw, ~0, MCTL_IHR_EN | MCTL_WAKE);
}

static void brcms_c_mctrl_write(struct brcms_hardware *wlc_hw)
{
	unsigned long held = brcms_glue_window_hold();
	u32 maccontrol = wlc_hw->maccontrol;

	/* OR in the wake bit if overridden */
	if (wlc_hw->wake_override)
		maccontrol |= MCTL_WAKE;

	/* set AP and INFRA bits for mute if needed */
	if (wlc_hw->mute_override) {
		maccontrol &= ~(MCTL_AP);
		maccontrol |= MCTL_INFRA;
	}

	bcma_write32(wlc_hw->d11core, D11REGOFFS(maccontrol),
		     maccontrol);
	brcms_glue_window_release(held);
}

void brcms_b_mctrl(struct brcms_hardware *wlc_hw, u32 mask, u32 val)
{
	u32 maccontrol;
	u32 new_maccontrol;

	if (val & ~mask)
		return; /* error condition */
	// The cached control word and its MMIO write form one transaction.
	unsigned long held = brcms_glue_window_hold();
	maccontrol = wlc_hw->maccontrol;
	new_maccontrol = (maccontrol & ~mask) | val;

	/* if the new maccontrol value is the same as the old, nothing to do */
	if (new_maccontrol == maccontrol) {
		brcms_glue_window_release(held);
		return;
	}

	/* something changed, cache the new value */
	wlc_hw->maccontrol = new_maccontrol;

	/* write the new values with overrides applied */
	brcms_c_mctrl_write(wlc_hw);
	brcms_glue_window_release(held);
}

void brcms_c_ucode_wake_override_set(struct brcms_hardware *wlc_hw,
				 u32 override_bit)
{
	if (wlc_hw->wake_override || (wlc_hw->maccontrol & MCTL_WAKE)) {
		mboolset(wlc_hw->wake_override, override_bit);
		return;
	}

	mboolset(wlc_hw->wake_override, override_bit);

	brcms_c_mctrl_write(wlc_hw);
	brcms_b_wait_for_wake(wlc_hw);
}

void brcms_c_ucode_wake_override_clear(struct brcms_hardware *wlc_hw,
				   u32 override_bit)
{
	mboolclr(wlc_hw->wake_override, override_bit);

	if (wlc_hw->wake_override || (wlc_hw->maccontrol & MCTL_WAKE))
		return;

	brcms_c_mctrl_write(wlc_hw);
}

static void brcms_c_ucode_mute_override_set(struct brcms_hardware *wlc_hw)
{
	wlc_hw->mute_override = 1;

	/* if maccontrol already has AP == 0 and INFRA == 1 without this
	 * override, then there is no change to write
	 */
	if ((wlc_hw->maccontrol & (MCTL_AP | MCTL_INFRA)) == MCTL_INFRA)
		return;

	brcms_c_mctrl_write(wlc_hw);
}

static void brcms_c_ucode_mute_override_clear(struct brcms_hardware *wlc_hw)
{
	if (wlc_hw->mute_override == 0)
		return;

	wlc_hw->mute_override = 0;

	/* if maccontrol already has AP == 0 and INFRA == 1 without this
	 * override, then there is no change to write
	 */
	if ((wlc_hw->maccontrol & (MCTL_AP | MCTL_INFRA)) == MCTL_INFRA)
		return;

	brcms_c_mctrl_write(wlc_hw);
}

static void brcms_b_set_cwmin(struct brcms_hardware *wlc_hw, u16 newmin)
{
	wlc_hw->band->CWmin = newmin;

	// Keep object selection and data access in the same transaction.
	unsigned long held = brcms_glue_window_hold();

	bcma_write32(wlc_hw->d11core, D11REGOFFS(objaddr),
		     OBJADDR_SCR_SEL | S_DOT11_CWMIN);
	(void)bcma_read32(wlc_hw->d11core, D11REGOFFS(objaddr));
	bcma_write32(wlc_hw->d11core, D11REGOFFS(objdata), newmin);
	brcms_glue_window_release(held);
}

static void brcms_b_set_cwmax(struct brcms_hardware *wlc_hw, u16 newmax)
{
	wlc_hw->band->CWmax = newmax;

	// Keep object selection and data access in the same transaction.
	unsigned long held = brcms_glue_window_hold();

	bcma_write32(wlc_hw->d11core, D11REGOFFS(objaddr),
		     OBJADDR_SCR_SEL | S_DOT11_CWMAX);
	(void)bcma_read32(wlc_hw->d11core, D11REGOFFS(objaddr));
	bcma_write32(wlc_hw->d11core, D11REGOFFS(objdata), newmax);
	brcms_glue_window_release(held);
}

void brcms_b_bw_set(struct brcms_hardware *wlc_hw, u16 bw)
{
	bool fastclk;

	/* request FAST clock if not on */
	fastclk = wlc_hw->forcefastclk;
	if (!fastclk)
		brcms_b_clkctl_clk(wlc_hw, BCMA_CLKMODE_FAST);

	wlc_phy_bw_state_set(wlc_hw->band->pi, bw);

	brcms_b_phy_reset(wlc_hw);
	wlc_phy_init(wlc_hw->band->pi, wlc_phy_chanspec_get(wlc_hw->band->pi));

	/* restore the clk */
	if (!fastclk)
		brcms_b_clkctl_clk(wlc_hw, BCMA_CLKMODE_DYNAMIC);
}

static void brcms_b_upd_synthpu(struct brcms_hardware *wlc_hw)
{
	u16 v;
	struct brcms_c_info *wlc = wlc_hw->wlc;
	/* update SYNTHPU_DLY */

	if (BRCMS_ISLCNPHY(wlc->band))
		v = SYNTHPU_DLY_LPPHY_US;
	else if (BRCMS_ISNPHY(wlc->band) && (NREV_GE(wlc->band->phyrev, 3)))
		v = SYNTHPU_DLY_NPHY_US;
	else
		v = SYNTHPU_DLY_BPHY_US;

	brcms_b_write_shm(wlc_hw, M_SYNTHPU_DLY, v);
}

static void brcms_c_ucode_txant_set(struct brcms_hardware *wlc_hw)
{
	u16 phyctl;
	u16 phytxant = wlc_hw->bmac_phytxant;
	u16 mask = PHY_TXC_ANT_MASK;

	/* set the Probe Response frame phy control word */
	phyctl = brcms_b_read_shm(wlc_hw, M_CTXPRS_BLK + C_CTX_PCTLWD_POS);
	phyctl = (phyctl & ~mask) | phytxant;
	brcms_b_write_shm(wlc_hw, M_CTXPRS_BLK + C_CTX_PCTLWD_POS, phyctl);

	/* set the Response (ACK/CTS) frame phy control word */
	phyctl = brcms_b_read_shm(wlc_hw, M_RSP_PCTLWD);
	phyctl = (phyctl & ~mask) | phytxant;
	brcms_b_write_shm(wlc_hw, M_RSP_PCTLWD, phyctl);
}

static u16 brcms_b_ofdm_ratetable_offset(struct brcms_hardware *wlc_hw,
					 u8 rate)
{
	uint i;
	u8 plcp_rate = 0;
	struct plcp_signal_rate_lookup {
		u8 rate;
		u8 signal_rate;
	};
	/* OFDM RATE sub-field of PLCP SIGNAL field, per 802.11 sec 17.3.4.1 */
	const struct plcp_signal_rate_lookup rate_lookup[] = {
		{BRCM_RATE_6M, 0xB},
		{BRCM_RATE_9M, 0xF},
		{BRCM_RATE_12M, 0xA},
		{BRCM_RATE_18M, 0xE},
		{BRCM_RATE_24M, 0x9},
		{BRCM_RATE_36M, 0xD},
		{BRCM_RATE_48M, 0x8},
		{BRCM_RATE_54M, 0xC}
	};

	for (i = 0; i < ARRAY_SIZE(rate_lookup); i++) {
		if (rate == rate_lookup[i].rate) {
			plcp_rate = rate_lookup[i].signal_rate;
			break;
		}
	}

	/* Find the SHM pointer to the rate table entry by looking in the
	 * Direct-map Table
	 */
	return 2 * brcms_b_read_shm(wlc_hw, M_RT_DIRMAP_A + (plcp_rate * 2));
}

static void brcms_upd_ofdm_pctl1_table(struct brcms_hardware *wlc_hw)
{
	u8 rate;
	u8 rates[8] = {
		BRCM_RATE_6M, BRCM_RATE_9M, BRCM_RATE_12M, BRCM_RATE_18M,
		BRCM_RATE_24M, BRCM_RATE_36M, BRCM_RATE_48M, BRCM_RATE_54M
	};
	u16 entry_ptr;
	u16 pctl1;
	uint i;

	if (!BRCMS_PHY_11N_CAP(wlc_hw->band))
		return;

	/* walk the phy rate table and update the entries */
	for (i = 0; i < ARRAY_SIZE(rates); i++) {
		rate = rates[i];

		entry_ptr = brcms_b_ofdm_ratetable_offset(wlc_hw, rate);

		/* read the SHM Rate Table entry OFDM PCTL1 values */
		pctl1 =
		    brcms_b_read_shm(wlc_hw, entry_ptr + M_RT_OFDM_PCTL1_POS);

		/* modify the value */
		pctl1 &= ~PHY_TXC1_MODE_MASK;
		pctl1 |= (wlc_hw->hw_stf_ss_opmode << PHY_TXC1_MODE_SHIFT);

		/* Update the SHM Rate Table entry OFDM PCTL1 values */
		brcms_b_write_shm(wlc_hw, entry_ptr + M_RT_OFDM_PCTL1_POS,
				   pctl1);
	}
}

static void brcms_b_bsinit(struct brcms_c_info *wlc, u16 chanspec)
{
	struct brcms_hardware *wlc_hw = wlc->hw;

	brcms_dbg_mac80211(wlc_hw->d11core, "wl%d: bandunit %d\n", wlc_hw->unit,
			   wlc_hw->band->bandunit);

	brcms_c_ucode_bsinit(wlc_hw);

	wlc_phy_init(wlc_hw->band->pi, chanspec);

	brcms_c_ucode_txant_set(wlc_hw);

	/*
	 * cwmin is band-specific, update hardware
	 * with value for current band
	 */
	brcms_b_set_cwmin(wlc_hw, wlc_hw->band->CWmin);
	brcms_b_set_cwmax(wlc_hw, wlc_hw->band->CWmax);

	brcms_b_update_slot_timing(wlc_hw,
				   wlc_hw->band->bandtype == BRCM_BAND_5G ?
				   true : wlc_hw->shortslot);

	/* write phytype and phyvers */
	brcms_b_write_shm(wlc_hw, M_PHYTYPE, (u16) wlc_hw->band->phytype);
	brcms_b_write_shm(wlc_hw, M_PHYVER, (u16) wlc_hw->band->phyrev);

	/*
	 * initialize the txphyctl1 rate table since
	 * shmem is shared between bands
	 */
	brcms_upd_ofdm_pctl1_table(wlc_hw);

	brcms_b_upd_synthpu(wlc_hw);
}

void brcms_b_core_phypll_reset(struct brcms_hardware *wlc_hw)
{
	ai_cc_reg(wlc_hw->sih, offsetof(struct chipcregs, chipcontrol_addr),
		  ~0, 0);
	udelay(1);
	ai_cc_reg(wlc_hw->sih, offsetof(struct chipcregs, chipcontrol_data),
		  0x4, 0);
	udelay(1);
	ai_cc_reg(wlc_hw->sih, offsetof(struct chipcregs, chipcontrol_data),
		  0x4, 4);
	udelay(1);
	ai_cc_reg(wlc_hw->sih, offsetof(struct chipcregs, chipcontrol_data),
		  0x4, 0);
	udelay(1);
}

void brcms_b_phyclk_fgc(struct brcms_hardware *wlc_hw, bool clk)
{
	/* support(necessary for NPHY and HYPHY) only */
	if (!BRCMS_ISNPHY(wlc_hw->band))
		return;

	if (ON == clk)
		brcms_b_core_ioctl(wlc_hw, SICF_FGC, SICF_FGC);
	else
		brcms_b_core_ioctl(wlc_hw, SICF_FGC, 0);

}

void brcms_b_macphyclk_set(struct brcms_hardware *wlc_hw, bool clk)
{
	if (ON == clk)
		brcms_b_core_ioctl(wlc_hw, SICF_MPCLKE, SICF_MPCLKE);
	else
		brcms_b_core_ioctl(wlc_hw, SICF_MPCLKE, 0);
}

void brcms_b_phy_reset(struct brcms_hardware *wlc_hw)
{
	struct brcms_phy_pub *pih = wlc_hw->band->pi;
	u32 phy_bw_clkbits;

	brcms_dbg_info(wlc_hw->d11core, "wl%d: reset phy\n", wlc_hw->unit);

	if (pih == NULL)
		return;

	phy_bw_clkbits = wlc_phy_clk_bwbits(wlc_hw->band->pi);

	/* Specific reset sequence required for NPHY rev 3 and 4 */
	if (BRCMS_ISNPHY(wlc_hw->band) && NREV_GE(wlc_hw->band->phyrev, 3) &&
	    NREV_LE(wlc_hw->band->phyrev, 4)) {
		/* Set the PHY bandwidth */
		brcms_b_core_ioctl(wlc_hw, SICF_BWMASK, phy_bw_clkbits);

		udelay(1);

		/* Perform a soft reset of the PHY PLL */
		brcms_b_core_phypll_reset(wlc_hw);

		/* reset the PHY */
		brcms_b_core_ioctl(wlc_hw, (SICF_PRST | SICF_PCLKE),
				   (SICF_PRST | SICF_PCLKE));
	} else {
		brcms_b_core_ioctl(wlc_hw,
				   (SICF_PRST | SICF_PCLKE | SICF_BWMASK),
				   (SICF_PRST | SICF_PCLKE | phy_bw_clkbits));
	}

	udelay(2);
	brcms_b_core_phy_clk(wlc_hw, ON);

	wlc_phy_anacore(pih, ON);
}

static void brcms_b_xtal(struct brcms_hardware *wlc_hw, bool want)
{
	brcms_dbg_info(wlc_hw->d11core, "wl%d: want %d\n", wlc_hw->unit, want);

	/*
	 * dont power down if plldown is false or
	 * we must poll hw radio disable
	 */
	if (!want && wlc_hw->pllreq)
		return;

	wlc_hw->sbclk = want;
	if (!wlc_hw->sbclk) {
		wlc_hw->clk = false;
		if (wlc_hw->band && wlc_hw->band->pi)
			wlc_phy_hw_clk_state_upd(wlc_hw->band->pi, false);
	}
}

static bool brcms_b_radio_read_hwdisabled(struct brcms_hardware *wlc_hw)
{
	bool v, clk, xtal;
	u32 flags = 0;

	xtal = wlc_hw->sbclk;
	if (!xtal)
		brcms_b_xtal(wlc_hw, ON);

	/* may need to take core out of reset first */
	clk = wlc_hw->clk;
	if (!clk) {
		/*
		 * mac no longer enables phyclk automatically when driver
		 * accesses phyreg throughput mac. This can be skipped since
		 * only mac reg is accessed below
		 */
		if (D11REV_GE(wlc_hw->corerev, 18))
			flags |= SICF_PCLKE;

		/*
		 * TODO: test suspend/resume
		 *
		 * AI chip doesn't restore bar0win2 on
		 * hibernation/resume, need sw fixup
		 */

		bcma_core_enable(wlc_hw->d11core, flags);
		brcms_c_mctrl_reset(wlc_hw);
	}

	v = ((bcma_read32(wlc_hw->d11core,
			  D11REGOFFS(phydebug)) & PDBG_RFD) != 0);

	/* put core back into reset */
	if (!clk)
		bcma_core_disable(wlc_hw->d11core, 0);

	if (!xtal)
		brcms_b_xtal(wlc_hw, OFF);

	return v;
}

static bool wlc_dma_rxreset(struct brcms_hardware *wlc_hw, uint fifo)
{
	struct dma_pub *di = wlc_hw->di[fifo];
	return dma_rxreset(di);
}

void brcms_b_corereset(struct brcms_hardware *wlc_hw, u32 flags)
{
	uint i;
	bool fastclk;

	if (flags == BRCMS_USE_COREFLAGS)
		flags = (wlc_hw->band->pi ? wlc_hw->band->core_flags : 0);

	brcms_dbg_info(wlc_hw->d11core, "wl%d: core reset\n", wlc_hw->unit);

	/* request FAST clock if not on  */
	fastclk = wlc_hw->forcefastclk;
	if (!fastclk)
		brcms_b_clkctl_clk(wlc_hw, BCMA_CLKMODE_FAST);

	/* reset the dma engines except first time thru */
	if (bcma_core_is_enabled(wlc_hw->d11core)) {
		for (i = 0; i < NFIFO; i++)
			if ((wlc_hw->di[i]) && (!dma_txreset(wlc_hw->di[i])))
				brcms_err(wlc_hw->d11core, "wl%d: %s: "
					  "dma_txreset[%d]: cannot stop dma\n",
					   wlc_hw->unit, __func__, i);

		if ((wlc_hw->di[RX_FIFO])
		    && (!wlc_dma_rxreset(wlc_hw, RX_FIFO)))
			brcms_err(wlc_hw->d11core, "wl%d: %s: dma_rxreset"
				  "[%d]: cannot stop dma\n",
				  wlc_hw->unit, __func__, RX_FIFO);
	}
	/* if noreset, just stop the psm and return */
	if (wlc_hw->noreset) {
		wlc_hw->wlc->macintstatus = 0;	/* skip wl_dpc after down */
		brcms_b_mctrl(wlc_hw, MCTL_PSM_RUN | MCTL_EN_MAC, 0);
		return;
	}

	/*
	 * mac no longer enables phyclk automatically when driver accesses
	 * phyreg throughput mac, AND phy_reset is skipped at early stage when
	 * band->pi is invalid. need to enable PHY CLK
	 */
	if (D11REV_GE(wlc_hw->corerev, 18))
		flags |= SICF_PCLKE;

	/*
	 * reset the core
	 * In chips with PMU, the fastclk request goes through d11 core
	 * reg 0x1e0, which is cleared by the core_reset. have to re-request it.
	 *
	 * This adds some delay and we can optimize it by also requesting
	 * fastclk through chipcommon during this period if necessary. But
	 * that has to work coordinate with other driver like mips/arm since
	 * they may touch chipcommon as well.
	 */
	wlc_hw->clk = false;
	bcma_core_enable(wlc_hw->d11core, flags);
	wlc_hw->clk = true;
	if (wlc_hw->band && wlc_hw->band->pi)
		wlc_phy_hw_clk_state_upd(wlc_hw->band->pi, true);

	brcms_c_mctrl_reset(wlc_hw);

	if (ai_get_cccaps(wlc_hw->sih) & CC_CAP_PMU)
		brcms_b_clkctl_clk(wlc_hw, BCMA_CLKMODE_FAST);

	brcms_b_phy_reset(wlc_hw);

	/* turn on PHY_PLL */
	brcms_b_core_phypll_ctl(wlc_hw, true);

	/* clear sw intstatus */
	wlc_hw->wlc->macintstatus = 0;

	/* restore the clk setting */
	if (!fastclk)
		brcms_b_clkctl_clk(wlc_hw, BCMA_CLKMODE_DYNAMIC);
}

static void brcms_b_corerev_fifofixup(struct brcms_hardware *wlc_hw)
{
	struct bcma_device *core = wlc_hw->d11core;
	u16 fifo_nu;
	u16 txfifo_startblk = TXFIFO_START_BLK, txfifo_endblk;
	u16 txfifo_def, txfifo_def1;
	u16 txfifo_cmd;

	/* tx fifos start at TXFIFO_START_BLK from the Base address */
	txfifo_startblk = TXFIFO_START_BLK;

	/* sequence of operations:  reset fifo, set fifo size, reset fifo */
	for (fifo_nu = 0; fifo_nu < NFIFO; fifo_nu++) {

		txfifo_endblk = txfifo_startblk + wlc_hw->xmtfifo_sz[fifo_nu];
		txfifo_def = (txfifo_startblk & 0xff) |
		    (((txfifo_endblk - 1) & 0xff) << TXFIFO_FIFOTOP_SHIFT);
		txfifo_def1 = ((txfifo_startblk >> 8) & 0x1) |
		    ((((txfifo_endblk -
			1) >> 8) & 0x1) << TXFIFO_FIFOTOP_SHIFT);
		txfifo_cmd =
		    TXFIFOCMD_RESET_MASK | (fifo_nu << TXFIFOCMD_FIFOSEL_SHIFT);

		bcma_write16(core, D11REGOFFS(xmtfifocmd), txfifo_cmd);
		bcma_write16(core, D11REGOFFS(xmtfifodef), txfifo_def);
		bcma_write16(core, D11REGOFFS(xmtfifodef1), txfifo_def1);

		bcma_write16(core, D11REGOFFS(xmtfifocmd), txfifo_cmd);

		txfifo_startblk += wlc_hw->xmtfifo_sz[fifo_nu];
	}
	/*
	 * need to propagate to shm location to be in sync since ucode/hw won't
	 * do this
	 */
	brcms_b_write_shm(wlc_hw, M_FIFOSIZE0,
			   wlc_hw->xmtfifo_sz[TX_AC_BE_FIFO]);
	brcms_b_write_shm(wlc_hw, M_FIFOSIZE1,
			   wlc_hw->xmtfifo_sz[TX_AC_VI_FIFO]);
	brcms_b_write_shm(wlc_hw, M_FIFOSIZE2,
			   ((wlc_hw->xmtfifo_sz[TX_AC_VO_FIFO] << 8) | wlc_hw->
			    xmtfifo_sz[TX_AC_BK_FIFO]));
	brcms_b_write_shm(wlc_hw, M_FIFOSIZE3,
			   ((wlc_hw->xmtfifo_sz[TX_ATIM_FIFO] << 8) | wlc_hw->
			    xmtfifo_sz[TX_BCMC_FIFO]));
}

void brcms_b_switch_macfreq(struct brcms_hardware *wlc_hw, u8 spurmode)
{
	struct bcma_device *core = wlc_hw->d11core;

	if ((ai_get_chip_id(wlc_hw->sih) == BCMA_CHIP_ID_BCM43224) ||
	    (ai_get_chip_id(wlc_hw->sih) == BCMA_CHIP_ID_BCM43225)) {
		if (spurmode == WL_SPURAVOID_ON2) {	/* 126Mhz */
			bcma_write16(core, D11REGOFFS(tsf_clk_frac_l), 0x2082);
			bcma_write16(core, D11REGOFFS(tsf_clk_frac_h), 0x8);
		} else if (spurmode == WL_SPURAVOID_ON1) {	/* 123Mhz */
			bcma_write16(core, D11REGOFFS(tsf_clk_frac_l), 0x5341);
			bcma_write16(core, D11REGOFFS(tsf_clk_frac_h), 0x8);
		} else {	/* 120Mhz */
			bcma_write16(core, D11REGOFFS(tsf_clk_frac_l), 0x8889);
			bcma_write16(core, D11REGOFFS(tsf_clk_frac_h), 0x8);
		}
	} else if (BRCMS_ISLCNPHY(wlc_hw->band)) {
		if (spurmode == WL_SPURAVOID_ON1) {	/* 82Mhz */
			bcma_write16(core, D11REGOFFS(tsf_clk_frac_l), 0x7CE0);
			bcma_write16(core, D11REGOFFS(tsf_clk_frac_h), 0xC);
		} else {	/* 80Mhz */
			bcma_write16(core, D11REGOFFS(tsf_clk_frac_l), 0xCCCD);
			bcma_write16(core, D11REGOFFS(tsf_clk_frac_h), 0xC);
		}
	}
}

static void brcms_c_gpio_init(struct brcms_c_info *wlc)
{
	struct brcms_hardware *wlc_hw = wlc->hw;
	u32 gc, gm;

	/* use GPIO select 0 to get all gpio signals from the gpio out reg */
	brcms_b_mctrl(wlc_hw, MCTL_GPOUT_SEL_MASK, 0);

	/*
	 * Common GPIO setup:
	 *      G0 = LED 0 = WLAN Activity
	 *      G1 = LED 1 = WLAN 2.4 GHz Radio State
	 *      G2 = LED 2 = WLAN 5 GHz Radio State
	 *      G4 = radio disable input (HI enabled, LO disabled)
	 */

	gc = gm = 0;

	/* Allocate GPIOs for mimo antenna diversity feature */
	if (wlc_hw->antsel_type == ANTSEL_2x3) {
		/* Enable antenna diversity, use 2x3 mode */
		brcms_b_mhf(wlc_hw, MHF3, MHF3_ANTSEL_EN,
			     MHF3_ANTSEL_EN, BRCM_BAND_ALL);
		brcms_b_mhf(wlc_hw, MHF3, MHF3_ANTSEL_MODE,
			     MHF3_ANTSEL_MODE, BRCM_BAND_ALL);

		/* init superswitch control */
		wlc_phy_antsel_init(wlc_hw->band->pi, false);

	} else if (wlc_hw->antsel_type == ANTSEL_2x4) {
		gm |= gc |= (BOARD_GPIO_12 | BOARD_GPIO_13);
		/*
		 * The board itself is powered by these GPIOs
		 * (when not sending pattern) so set them high
		 */
		bcma_set16(wlc_hw->d11core, D11REGOFFS(psm_gpio_oe),
			   (BOARD_GPIO_12 | BOARD_GPIO_13));
		bcma_set16(wlc_hw->d11core, D11REGOFFS(psm_gpio_out),
			   (BOARD_GPIO_12 | BOARD_GPIO_13));

		/* Enable antenna diversity, use 2x4 mode */
		brcms_b_mhf(wlc_hw, MHF3, MHF3_ANTSEL_EN,
			     MHF3_ANTSEL_EN, BRCM_BAND_ALL);
		brcms_b_mhf(wlc_hw, MHF3, MHF3_ANTSEL_MODE, 0,
			     BRCM_BAND_ALL);

		/* Configure the desired clock to be 4Mhz */
		brcms_b_write_shm(wlc_hw, M_ANTSEL_CLKDIV,
				   ANTSEL_CLKDIV_4MHZ);
	}

	/*
	 * gpio 9 controls the PA. ucode is responsible
	 * for wiggling out and oe
	 */
	if (wlc_hw->boardflags & BFL_PACTRL)
		gm |= gc |= BOARD_GPIO_PACTRL;

	/* apply to gpiocontrol register */
	bcma_chipco_gpio_control(&wlc_hw->d11core->bus->drv_cc, gm, gc);
}

static void brcms_ucode_write(struct brcms_hardware *wlc_hw,
			      const __le32 ucode[], const size_t nbytes)
{
	struct bcma_device *core = wlc_hw->d11core;
	uint i;
	uint count;

	brcms_dbg_info(wlc_hw->d11core, "wl%d\n", wlc_hw->unit);

	count = (nbytes / sizeof(u32));

	// In runs, not one by one, and not all at once either: the window is
	// held with interrupts off, so a run is short enough not to be felt.
	unsigned long held = brcms_glue_window_hold();

	bcma_write32(core, D11REGOFFS(objaddr),
		     OBJADDR_AUTO_INC | OBJADDR_UCM_SEL);
	(void)bcma_read32(core, D11REGOFFS(objaddr));

	for (i = 0; i < count; i++) {
		if (i != 0 && (i % 256) == 0) {
			brcms_glue_window_release(held);
			held = brcms_glue_window_hold();
		}

		bcma_write32(core, D11REGOFFS(objdata), le32_to_cpu(ucode[i]));
	}

	brcms_glue_window_release(held);

}

static void brcms_ucode_download(struct brcms_hardware *wlc_hw)
{
	struct brcms_ucode *ucode = &wlc_hw->wlc->wl->ucode;

	if (wlc_hw->ucode_loaded)
		return;

	if (D11REV_IS(wlc_hw->corerev, 17) || D11REV_IS(wlc_hw->corerev, 23)) {
		if (BRCMS_ISNPHY(wlc_hw->band)) {
			brcms_ucode_write(wlc_hw, ucode->bcm43xx_16_mimo,
					  ucode->bcm43xx_16_mimosz);
			wlc_hw->ucode_loaded = true;
		} else
			brcms_err(wlc_hw->d11core,
				  "%s: wl%d: unsupported phy in corerev %d\n",
				  __func__, wlc_hw->unit, wlc_hw->corerev);
	} else if (D11REV_IS(wlc_hw->corerev, 24)) {
		if (BRCMS_ISLCNPHY(wlc_hw->band)) {
			brcms_ucode_write(wlc_hw, ucode->bcm43xx_24_lcn,
					  ucode->bcm43xx_24_lcnsz);
			wlc_hw->ucode_loaded = true;
		} else {
			brcms_err(wlc_hw->d11core,
				  "%s: wl%d: unsupported phy in corerev %d\n",
				  __func__, wlc_hw->unit, wlc_hw->corerev);
		}
	}
}

u16 brcms_b_get_txant(struct brcms_hardware *wlc_hw)
{
	return (u16) wlc_hw->wlc->stf->txant;
}

static void brcms_b_tx_fifo_suspend(struct brcms_hardware *wlc_hw,
				    uint tx_fifo)
{
	u8 fifo = 1 << tx_fifo;

	/* Two clients of this code, 11h Quiet period and scanning. */

	/* only suspend if not already suspended */
	if ((wlc_hw->suspended_fifos & fifo) == fifo)
		return;

	/* force the core awake only if not already */
	if (wlc_hw->suspended_fifos == 0)
		brcms_c_ucode_wake_override_set(wlc_hw,
						BRCMS_WAKE_OVERRIDE_TXFIFO);

	wlc_hw->suspended_fifos |= fifo;

	if (wlc_hw->di[tx_fifo]) {
		/*
		 * Suspending AMPDU transmissions in the middle can cause
		 * underflow which may result in mismatch between ucode and
		 * driver so suspend the mac before suspending the FIFO
		 */
		if (BRCMS_PHY_11N_CAP(wlc_hw->band))
			brcms_c_suspend_mac_and_wait(wlc_hw->wlc);

		dma_txsuspend(wlc_hw->di[tx_fifo]);

		if (BRCMS_PHY_11N_CAP(wlc_hw->band))
			brcms_c_enable_mac(wlc_hw->wlc);
	}
}

static void brcms_b_tx_fifo_resume(struct brcms_hardware *wlc_hw,
				   uint tx_fifo)
{
	/* BMAC_NOTE: BRCMS_TX_FIFO_ENAB is done in brcms_c_dpc() for DMA case
	 * but need to be done here for PIO otherwise the watchdog will catch
	 * the inconsistency and fire
	 */
	/* Two clients of this code, 11h Quiet period and scanning. */
	if (wlc_hw->di[tx_fifo])
		dma_txresume(wlc_hw->di[tx_fifo]);

	/* allow core to sleep again */
	if (wlc_hw->suspended_fifos == 0)
		return;
	else {
		wlc_hw->suspended_fifos &= ~(1 << tx_fifo);
		if (wlc_hw->suspended_fifos == 0)
			brcms_c_ucode_wake_override_clear(wlc_hw,
						BRCMS_WAKE_OVERRIDE_TXFIFO);
	}
}

static void brcms_b_mute(struct brcms_hardware *wlc_hw, bool mute_tx)
{
	static const u8 null_ether_addr[ETH_ALEN] = {0, 0, 0, 0, 0, 0};
	u8 *ethaddr = wlc_hw->wlc->pub->cur_etheraddr;

	if (mute_tx) {
		/* suspend tx fifos */
		brcms_b_tx_fifo_suspend(wlc_hw, TX_DATA_FIFO);
		brcms_b_tx_fifo_suspend(wlc_hw, TX_CTL_FIFO);
		brcms_b_tx_fifo_suspend(wlc_hw, TX_AC_BK_FIFO);
		brcms_b_tx_fifo_suspend(wlc_hw, TX_AC_VI_FIFO);

		/* zero the address match register so we do not send ACKs */
		brcms_b_set_addrmatch(wlc_hw, RCM_MAC_OFFSET, null_ether_addr);
	} else {
		/* resume tx fifos */
		brcms_b_tx_fifo_resume(wlc_hw, TX_DATA_FIFO);
		brcms_b_tx_fifo_resume(wlc_hw, TX_CTL_FIFO);
		brcms_b_tx_fifo_resume(wlc_hw, TX_AC_BK_FIFO);
		brcms_b_tx_fifo_resume(wlc_hw, TX_AC_VI_FIFO);

		/* Restore address */
		brcms_b_set_addrmatch(wlc_hw, RCM_MAC_OFFSET, ethaddr);
	}

	wlc_phy_mute_upd(wlc_hw->band->pi, mute_tx, 0);

	if (mute_tx)
		brcms_c_ucode_mute_override_set(wlc_hw);
	else
		brcms_c_ucode_mute_override_clear(wlc_hw);
}

void brcms_c_suspend_mac_and_wait(struct brcms_c_info *wlc)
{
	struct brcms_hardware *wlc_hw = wlc->hw;
	struct bcma_device *core = wlc_hw->d11core;
	u32 mc, mi;

	brcms_dbg_mac80211(core, "wl%d: bandunit %d\n", wlc_hw->unit,
			   wlc_hw->band->bandunit);

	/*
	 * Track overlapping suspend requests
	 */
	wlc_hw->mac_suspend_depth++;
	if (wlc_hw->mac_suspend_depth > 1)
		return;

	/* force the core awake */
	brcms_c_ucode_wake_override_set(wlc_hw, BRCMS_WAKE_OVERRIDE_MACSUSPEND);

	mc = bcma_read32(core, D11REGOFFS(maccontrol));

	if (mc == 0xffffffff) {
		brcms_err(core, "wl%d: %s: dead chip\n", wlc_hw->unit,
			  __func__);
		brcms_down(wlc->wl);
		return;
	}
	WARN_ON(mc & MCTL_PSM_JMP_0);
	WARN_ON(!(mc & MCTL_PSM_RUN));
	WARN_ON(!(mc & MCTL_EN_MAC));

	mi = bcma_read32(core, D11REGOFFS(macintstatus));
	if (mi == 0xffffffff) {
		brcms_err(core, "wl%d: %s: dead chip\n", wlc_hw->unit,
			  __func__);
		brcms_down(wlc->wl);
		return;
	}
	WARN_ON(mi & MI_MACSSPNDD);

	brcms_b_mctrl(wlc_hw, MCTL_EN_MAC, 0);

	SPINWAIT(!(bcma_read32(core, D11REGOFFS(macintstatus)) & MI_MACSSPNDD),
		 BRCMS_MAX_MAC_SUSPEND);

	if (!(bcma_read32(core, D11REGOFFS(macintstatus)) & MI_MACSSPNDD)) {
		brcms_err(core, "wl%d: wlc_suspend_mac_and_wait: waited %d uS"
			  " and MI_MACSSPNDD is still not on. depth %u, "
			  "maccontrol 0x%08x\n",
			  wlc_hw->unit, BRCMS_MAX_MAC_SUSPEND,
			  wlc_hw->mac_suspend_depth,
			  bcma_read32(core, D11REGOFFS(maccontrol)));
		brcms_err(core, "wl%d: psmdebug 0x%08x, phydebug 0x%08x, "
			  "psm_brc 0x%04x\n", wlc_hw->unit,
			  bcma_read32(core, D11REGOFFS(psmdebug)),
			  bcma_read32(core, D11REGOFFS(phydebug)),
			  bcma_read16(core, D11REGOFFS(psm_brc)));
	}

	mc = bcma_read32(core, D11REGOFFS(maccontrol));
	if (mc == 0xffffffff) {
		brcms_err(core, "wl%d: %s: dead chip\n", wlc_hw->unit,
			  __func__);
		brcms_down(wlc->wl);
		return;
	}
	WARN_ON(mc & MCTL_PSM_JMP_0);
	WARN_ON(!(mc & MCTL_PSM_RUN));
	WARN_ON(mc & MCTL_EN_MAC);
}

void brcms_c_enable_mac(struct brcms_c_info *wlc)
{
	struct brcms_hardware *wlc_hw = wlc->hw;
	struct bcma_device *core = wlc_hw->d11core;
	u32 mc, mi;

	brcms_dbg_mac80211(core, "wl%d: bandunit %d\n", wlc_hw->unit,
			   wlc->band->bandunit);

	/*
	 * Track overlapping suspend requests
	 */
	wlc_hw->mac_suspend_depth--;
	if (wlc_hw->mac_suspend_depth > 0)
		return;

	mc = bcma_read32(core, D11REGOFFS(maccontrol));
	WARN_ON(mc & MCTL_PSM_JMP_0);
	WARN_ON(mc & MCTL_EN_MAC);
	WARN_ON(!(mc & MCTL_PSM_RUN));

	brcms_b_mctrl(wlc_hw, MCTL_EN_MAC, MCTL_EN_MAC);
	bcma_write32(core, D11REGOFFS(macintstatus), MI_MACSSPNDD);

	mc = bcma_read32(core, D11REGOFFS(maccontrol));
	WARN_ON(mc & MCTL_PSM_JMP_0);
	WARN_ON(!(mc & MCTL_EN_MAC));
	WARN_ON(!(mc & MCTL_PSM_RUN));

	mi = bcma_read32(core, D11REGOFFS(macintstatus));
	WARN_ON(mi & MI_MACSSPNDD);

	brcms_c_ucode_wake_override_clear(wlc_hw,
					  BRCMS_WAKE_OVERRIDE_MACSUSPEND);
}

bool brcms_b_validate_chip_access(struct brcms_hardware *wlc_hw)
{
	struct bcma_device *core = wlc_hw->d11core;
	u32 w, val;
	struct wiphy *wiphy = wlc_hw->wlc->wiphy;

	/* Validate dchip register access */

	bcma_write32(core, D11REGOFFS(objaddr), OBJADDR_SHM_SEL | 0);
	(void)bcma_read32(core, D11REGOFFS(objaddr));
	w = bcma_read32(core, D11REGOFFS(objdata));

	/* Can we write and read back a 32bit register? */
	bcma_write32(core, D11REGOFFS(objaddr), OBJADDR_SHM_SEL | 0);
	(void)bcma_read32(core, D11REGOFFS(objaddr));
	bcma_write32(core, D11REGOFFS(objdata), (u32) 0xaa5555aa);

	bcma_write32(core, D11REGOFFS(objaddr), OBJADDR_SHM_SEL | 0);
	(void)bcma_read32(core, D11REGOFFS(objaddr));
	val = bcma_read32(core, D11REGOFFS(objdata));
	if (val != (u32) 0xaa5555aa) {
		wiphy_err(wiphy, "wl%d: validate_chip_access: SHM = 0x%x, "
			  "expected 0xaa5555aa\n", wlc_hw->unit, val);
		return false;
	}

	bcma_write32(core, D11REGOFFS(objaddr), OBJADDR_SHM_SEL | 0);
	(void)bcma_read32(core, D11REGOFFS(objaddr));
	bcma_write32(core, D11REGOFFS(objdata), (u32) 0x55aaaa55);

	bcma_write32(core, D11REGOFFS(objaddr), OBJADDR_SHM_SEL | 0);
	(void)bcma_read32(core, D11REGOFFS(objaddr));
	val = bcma_read32(core, D11REGOFFS(objdata));
	if (val != (u32) 0x55aaaa55) {
		wiphy_err(wiphy, "wl%d: validate_chip_access: SHM = 0x%x, "
			  "expected 0x55aaaa55\n", wlc_hw->unit, val);
		return false;
	}

	bcma_write32(core, D11REGOFFS(objaddr), OBJADDR_SHM_SEL | 0);
	(void)bcma_read32(core, D11REGOFFS(objaddr));
	bcma_write32(core, D11REGOFFS(objdata), w);

	/* clear CFPStart */
	bcma_write32(core, D11REGOFFS(tsf_cfpstart), 0);

	w = bcma_read32(core, D11REGOFFS(maccontrol));
	if ((w != (MCTL_IHR_EN | MCTL_WAKE)) &&
	    (w != (MCTL_IHR_EN | MCTL_GMODE | MCTL_WAKE))) {
		wiphy_err(wiphy, "wl%d: validate_chip_access: maccontrol = "
			  "0x%x, expected 0x%x or 0x%x\n", wlc_hw->unit, w,
			  (MCTL_IHR_EN | MCTL_WAKE),
			  (MCTL_IHR_EN | MCTL_GMODE | MCTL_WAKE));
		return false;
	}

	return true;
}

void brcms_b_core_phypll_ctl(struct brcms_hardware *wlc_hw, bool on)
{
	struct bcma_device *core = wlc_hw->d11core;
	u32 tmp;

	brcms_dbg_info(core, "wl%d\n", wlc_hw->unit);

	tmp = 0;

	if (on) {
		if ((ai_get_chip_id(wlc_hw->sih) == BCMA_CHIP_ID_BCM4313)) {
			bcma_set32(core, D11REGOFFS(clk_ctl_st),
				   CCS_ERSRC_REQ_HT |
				   CCS_ERSRC_REQ_D11PLL |
				   CCS_ERSRC_REQ_PHYPLL);
			SPINWAIT((bcma_read32(core, D11REGOFFS(clk_ctl_st)) &
				  CCS_ERSRC_AVAIL_HT) != CCS_ERSRC_AVAIL_HT,
				 PHYPLL_WAIT_US);

			tmp = bcma_read32(core, D11REGOFFS(clk_ctl_st));
			if ((tmp & CCS_ERSRC_AVAIL_HT) != CCS_ERSRC_AVAIL_HT)
				brcms_err(core, "%s: turn on PHY PLL failed\n",
					  __func__);
		} else {
			bcma_set32(core, D11REGOFFS(clk_ctl_st),
				   tmp | CCS_ERSRC_REQ_D11PLL |
				   CCS_ERSRC_REQ_PHYPLL);
			SPINWAIT((bcma_read32(core, D11REGOFFS(clk_ctl_st)) &
				  (CCS_ERSRC_AVAIL_D11PLL |
				   CCS_ERSRC_AVAIL_PHYPLL)) !=
				 (CCS_ERSRC_AVAIL_D11PLL |
				  CCS_ERSRC_AVAIL_PHYPLL), PHYPLL_WAIT_US);

			tmp = bcma_read32(core, D11REGOFFS(clk_ctl_st));
			if ((tmp &
			     (CCS_ERSRC_AVAIL_D11PLL | CCS_ERSRC_AVAIL_PHYPLL))
			    !=
			    (CCS_ERSRC_AVAIL_D11PLL | CCS_ERSRC_AVAIL_PHYPLL))
				brcms_err(core, "%s: turn on PHY PLL failed\n",
					  __func__);
		}
	} else {
		/*
		 * Since the PLL may be shared, other cores can still
		 * be requesting it; so we'll deassert the request but
		 * not wait for status to comply.
		 */
		bcma_mask32(core, D11REGOFFS(clk_ctl_st),
			    ~CCS_ERSRC_REQ_PHYPLL);
		(void)bcma_read32(core, D11REGOFFS(clk_ctl_st));
	}
}

u16 brcms_b_read_shm(struct brcms_hardware *wlc_hw, uint offset)
{
	return brcms_b_read_objmem(wlc_hw, offset, OBJADDR_SHM_SEL);
}

void brcms_b_write_shm(struct brcms_hardware *wlc_hw, uint offset, u16 v)
{
	brcms_b_write_objmem(wlc_hw, offset, v, OBJADDR_SHM_SEL);
}

static void brcms_b_pllreq(struct brcms_hardware *wlc_hw, bool set, u32 req_bit)
{
	if (set) {
		if (mboolisset(wlc_hw->pllreq, req_bit))
			return;

		mboolset(wlc_hw->pllreq, req_bit);

		if (mboolisset(wlc_hw->pllreq, BRCMS_PLLREQ_FLIP)) {
			if (!wlc_hw->sbclk)
				brcms_b_xtal(wlc_hw, ON);
		}
	} else {
		if (!mboolisset(wlc_hw->pllreq, req_bit))
			return;

		mboolclr(wlc_hw->pllreq, req_bit);

		if (mboolisset(wlc_hw->pllreq, BRCMS_PLLREQ_FLIP)) {
			if (wlc_hw->sbclk)
				brcms_b_xtal(wlc_hw, OFF);
		}
	}
}

static bool brcms_c_ps_allowed(struct brcms_c_info *wlc)
{
	/* not supporting PS so always return false for now */
	return false;
}

static void brcms_b_coreinit(struct brcms_c_info *wlc)
{
	struct brcms_hardware *wlc_hw = wlc->hw;
	struct bcma_device *core = wlc_hw->d11core;
	u32 bcnint_us;
	uint i = 0;
	bool fifosz_fixup = false;
	int err = 0;
	u16 buf[NFIFO];
	struct brcms_ucode *ucode = &wlc_hw->wlc->wl->ucode;

	brcms_dbg_info(core, "wl%d: core init\n", wlc_hw->unit);

	/* reset PSM */
	brcms_b_mctrl(wlc_hw, ~0, (MCTL_IHR_EN | MCTL_PSM_JMP_0 | MCTL_WAKE));

	brcms_ucode_download(wlc_hw);
	/*
	 * FIFOSZ fixup. driver wants to controls the fifo allocation.
	 */
	fifosz_fixup = true;

	/* let the PSM run to the suspended state, set mode to BSS STA */
	bcma_write32(core, D11REGOFFS(macintstatus), -1);
	brcms_b_mctrl(wlc_hw, ~0,
		       (MCTL_IHR_EN | MCTL_INFRA | MCTL_PSM_RUN | MCTL_WAKE));

	/* wait for ucode to self-suspend after auto-init */
	SPINWAIT(((bcma_read32(core, D11REGOFFS(macintstatus)) &
		   MI_MACSSPNDD) == 0), 1000 * 1000);
	if ((bcma_read32(core, D11REGOFFS(macintstatus)) & MI_MACSSPNDD) == 0)
		brcms_err(core, "wl%d: wlc_coreinit: ucode did not self-"
			  "suspend!\n", wlc_hw->unit);

	brcms_c_gpio_init(wlc);

	bcma_aread32(core, BCMA_IOST);

	if (D11REV_IS(wlc_hw->corerev, 17) || D11REV_IS(wlc_hw->corerev, 23)) {
		if (BRCMS_ISNPHY(wlc_hw->band))
			brcms_c_write_inits(wlc_hw, ucode->d11n0initvals16);
		else
			brcms_err(core, "%s: wl%d: unsupported phy in corerev"
				  " %d\n", __func__, wlc_hw->unit,
				  wlc_hw->corerev);
	} else if (D11REV_IS(wlc_hw->corerev, 24)) {
		if (BRCMS_ISLCNPHY(wlc_hw->band))
			brcms_c_write_inits(wlc_hw, ucode->d11lcn0initvals24);
		else
			brcms_err(core, "%s: wl%d: unsupported phy in corerev"
				  " %d\n", __func__, wlc_hw->unit,
				  wlc_hw->corerev);
	} else {
		brcms_err(core, "%s: wl%d: unsupported corerev %d\n",
			  __func__, wlc_hw->unit, wlc_hw->corerev);
	}

	/* For old ucode, txfifo sizes needs to be modified(increased) */
	if (fifosz_fixup)
		brcms_b_corerev_fifofixup(wlc_hw);

	/* check txfifo allocations match between ucode and driver */
	buf[TX_AC_BE_FIFO] = brcms_b_read_shm(wlc_hw, M_FIFOSIZE0);
	if (buf[TX_AC_BE_FIFO] != wlc_hw->xmtfifo_sz[TX_AC_BE_FIFO]) {
		i = TX_AC_BE_FIFO;
		err = -1;
	}
	buf[TX_AC_VI_FIFO] = brcms_b_read_shm(wlc_hw, M_FIFOSIZE1);
	if (buf[TX_AC_VI_FIFO] != wlc_hw->xmtfifo_sz[TX_AC_VI_FIFO]) {
		i = TX_AC_VI_FIFO;
		err = -1;
	}
	buf[TX_AC_BK_FIFO] = brcms_b_read_shm(wlc_hw, M_FIFOSIZE2);
	buf[TX_AC_VO_FIFO] = (buf[TX_AC_BK_FIFO] >> 8) & 0xff;
	buf[TX_AC_BK_FIFO] &= 0xff;
	if (buf[TX_AC_BK_FIFO] != wlc_hw->xmtfifo_sz[TX_AC_BK_FIFO]) {
		i = TX_AC_BK_FIFO;
		err = -1;
	}
	if (buf[TX_AC_VO_FIFO] != wlc_hw->xmtfifo_sz[TX_AC_VO_FIFO]) {
		i = TX_AC_VO_FIFO;
		err = -1;
	}
	buf[TX_BCMC_FIFO] = brcms_b_read_shm(wlc_hw, M_FIFOSIZE3);
	buf[TX_ATIM_FIFO] = (buf[TX_BCMC_FIFO] >> 8) & 0xff;
	buf[TX_BCMC_FIFO] &= 0xff;
	if (buf[TX_BCMC_FIFO] != wlc_hw->xmtfifo_sz[TX_BCMC_FIFO]) {
		i = TX_BCMC_FIFO;
		err = -1;
	}
	if (buf[TX_ATIM_FIFO] != wlc_hw->xmtfifo_sz[TX_ATIM_FIFO]) {
		i = TX_ATIM_FIFO;
		err = -1;
	}
	if (err != 0)
		brcms_err(core, "wlc_coreinit: txfifo mismatch: ucode size %d"
			  " driver size %d index %d\n", buf[i],
			  wlc_hw->xmtfifo_sz[i], i);

	/* make sure we can still talk to the mac */
	WARN_ON(bcma_read32(core, D11REGOFFS(maccontrol)) == 0xffffffff);

	/* band-specific inits done by wlc_bsinit() */

	/* Set up frame burst size and antenna swap threshold init values */
	brcms_b_write_shm(wlc_hw, M_MBURST_SIZE, MAXTXFRAMEBURST);
	brcms_b_write_shm(wlc_hw, M_MAX_ANTCNT, ANTCNT);

	/* enable one rx interrupt per received frame */
	bcma_write32(core, D11REGOFFS(intrcvlazy[0]), (1 << IRL_FC_SHIFT));

	/* set the station mode (BSS STA) */
	brcms_b_mctrl(wlc_hw,
		       (MCTL_INFRA | MCTL_DISCARD_PMQ | MCTL_AP),
		       (MCTL_INFRA | MCTL_DISCARD_PMQ));

	/* set up Beacon interval */
	bcnint_us = 0x8000 << 10;
	bcma_write32(core, D11REGOFFS(tsf_cfprep),
		     (bcnint_us << CFPREP_CBI_SHIFT));
	bcma_write32(core, D11REGOFFS(tsf_cfpstart), bcnint_us);
	bcma_write32(core, D11REGOFFS(macintstatus), MI_GP1);

	/* write interrupt mask */
	bcma_write32(core, D11REGOFFS(intctrlregs[RX_FIFO].intmask),
		     DEF_RXINTMASK);

	/* allow the MAC to control the PHY clock (dynamic on/off) */
	brcms_b_macphyclk_set(wlc_hw, ON);

	/* program dynamic clock control fast powerup delay register */
	wlc->fastpwrup_dly = ai_clkctl_fast_pwrup_delay(wlc_hw->sih);
	bcma_write16(core, D11REGOFFS(scc_fastpwrup_dly), wlc->fastpwrup_dly);

	/* tell the ucode the corerev */
	brcms_b_write_shm(wlc_hw, M_MACHW_VER, (u16) wlc_hw->corerev);

	/* tell the ucode MAC capabilities */
	brcms_b_write_shm(wlc_hw, M_MACHW_CAP_L,
			   (u16) (wlc_hw->machwcap & 0xffff));
	brcms_b_write_shm(wlc_hw, M_MACHW_CAP_H,
			   (u16) ((wlc_hw->
				      machwcap >> 16) & 0xffff));

	/* write retry limits to SCR, this done after PSM init */
	bcma_write32(core, D11REGOFFS(objaddr),
		     OBJADDR_SCR_SEL | S_DOT11_SRC_LMT);
	(void)bcma_read32(core, D11REGOFFS(objaddr));
	bcma_write32(core, D11REGOFFS(objdata), wlc_hw->SRL);
	bcma_write32(core, D11REGOFFS(objaddr),
		     OBJADDR_SCR_SEL | S_DOT11_LRC_LMT);
	(void)bcma_read32(core, D11REGOFFS(objaddr));
	bcma_write32(core, D11REGOFFS(objdata), wlc_hw->LRL);

	/* write rate fallback retry limits */
	brcms_b_write_shm(wlc_hw, M_SFRMTXCNTFBRTHSD, wlc_hw->SFBL);
	brcms_b_write_shm(wlc_hw, M_LFRMTXCNTFBRTHSD, wlc_hw->LFBL);

	bcma_mask16(core, D11REGOFFS(ifs_ctl), 0x0FFF);
	bcma_write16(core, D11REGOFFS(ifs_aifsn), EDCF_AIFSN_MIN);

	/* init the tx dma engines */
	for (i = 0; i < NFIFO; i++) {
		if (wlc_hw->di[i])
			dma_txinit(wlc_hw->di[i]);
	}

	/* init the rx dma engine(s) and post receive buffers */
	dma_rxinit(wlc_hw->di[RX_FIFO]);
	dma_rxfill(wlc_hw->di[RX_FIFO]);
}

void brcms_b_init(struct brcms_hardware *wlc_hw, u16 chanspec)
{
	u32 macintmask;
	bool fastclk;
	struct brcms_c_info *wlc = wlc_hw->wlc;

	/* request FAST clock if not on */
	fastclk = wlc_hw->forcefastclk;
	if (!fastclk)
		brcms_b_clkctl_clk(wlc_hw, BCMA_CLKMODE_FAST);

	/* disable interrupts */
	macintmask = brcms_intrsoff(wlc->wl);

	/* set up the specified band and chanspec */
	brcms_c_setxband(wlc_hw, chspec_bandunit(chanspec));
	wlc_phy_chanspec_radio_set(wlc_hw->band->pi, chanspec);

	/* do one-time phy inits and calibration */
	wlc_phy_cal_init(wlc_hw->band->pi);

	/* core-specific initialization */
	brcms_b_coreinit(wlc);

	/* band-specific inits */
	brcms_b_bsinit(wlc, chanspec);

	/* restore macintmask */
	brcms_intrsrestore(wlc->wl, macintmask);

	/* seed wake_override with BRCMS_WAKE_OVERRIDE_MACSUSPEND since the mac
	 * is suspended and brcms_c_enable_mac() will clear this override bit.
	 */
	mboolset(wlc_hw->wake_override, BRCMS_WAKE_OVERRIDE_MACSUSPEND);

	/*
	 * initialize mac_suspend_depth to 1 to match ucode
	 * initial suspended state
	 */
	wlc_hw->mac_suspend_depth = 1;

	/* restore the clk */
	if (!fastclk)
		brcms_b_clkctl_clk(wlc_hw, BCMA_CLKMODE_DYNAMIC);
}

static void brcms_c_set_phy_chanspec(struct brcms_c_info *wlc,
				     u16 chanspec)
{
	/* Save our copy of the chanspec */
	wlc->chanspec = chanspec;

	/* Set the chanspec and power limits for this locale */
	brcms_c_channel_set_chanspec(wlc->cmi, chanspec, BRCMS_TXPWR_MAX);

	if (wlc->stf->ss_algosel_auto)
		brcms_c_stf_ss_algo_channel_get(wlc, &wlc->stf->ss_algo_channel,
					    chanspec);

	brcms_c_stf_ss_update(wlc, wlc->band);
}

void brcms_c_mac_promisc(struct brcms_c_info *wlc, uint filter_flags)
{
	u32 promisc_bits = 0;

	wlc->filter_flags = filter_flags;

	if (filter_flags & FIF_OTHER_BSS)
		promisc_bits |= MCTL_PROMISC;

	if (filter_flags & FIF_BCN_PRBRESP_PROMISC)
		promisc_bits |= MCTL_BCNS_PROMISC;

	if (filter_flags & FIF_FCSFAIL)
		promisc_bits |= MCTL_KEEPBADFCS;

	if (filter_flags & (FIF_CONTROL | FIF_PSPOLL))
		promisc_bits |= MCTL_KEEPCONTROL;

	brcms_b_mctrl(wlc->hw,
		MCTL_PROMISC | MCTL_BCNS_PROMISC |
		MCTL_KEEPCONTROL | MCTL_KEEPBADFCS,
		promisc_bits);
}

static void brcms_c_ucode_mac_upd(struct brcms_c_info *wlc)
{
	/* enable or disable any active IBSSs depending on whether or not
	 * we are on the home channel
	 */
	if (wlc->home_chanspec == wlc_phy_chanspec_get(wlc->band->pi)) {
		if (wlc->pub->associated) {
			/*
			 * BMAC_NOTE: This is something that should be fixed
			 * in ucode inits. I think that the ucode inits set
			 * up the bcn templates and shm values with a bogus
			 * beacon. This should not be done in the inits. If
			 * ucode needs to set up a beacon for testing, the
			 * test routines should write it down, not expect the
			 * inits to populate a bogus beacon.
			 */
			if (BRCMS_PHY_11N_CAP(wlc->band))
				brcms_b_write_shm(wlc->hw,
						M_BCN_TXTSF_OFFSET, 0);
		}
	} else {
		/* disable an active IBSS if we are not on the home channel */
	}
}

static void brcms_c_write_rate_shm(struct brcms_c_info *wlc, u8 rate,
				   u8 basic_rate)
{
	u8 phy_rate, index;
	u8 basic_phy_rate, basic_index;
	u16 dir_table, basic_table;
	u16 basic_ptr;

	/* Shared memory address for the table we are reading */
	dir_table = is_ofdm_rate(basic_rate) ? M_RT_DIRMAP_A : M_RT_DIRMAP_B;

	/* Shared memory address for the table we are writing */
	basic_table = is_ofdm_rate(rate) ? M_RT_BBRSMAP_A : M_RT_BBRSMAP_B;

	/*
	 * for a given rate, the LS-nibble of the PLCP SIGNAL field is
	 * the index into the rate table.
	 */
	phy_rate = rate_info[rate] & BRCMS_RATE_MASK;
	basic_phy_rate = rate_info[basic_rate] & BRCMS_RATE_MASK;
	index = phy_rate & 0xf;
	basic_index = basic_phy_rate & 0xf;

	/* Find the SHM pointer to the ACK rate entry by looking in the
	 * Direct-map Table
	 */
	basic_ptr = brcms_b_read_shm(wlc->hw, (dir_table + basic_index * 2));

	/* Update the SHM BSS-basic-rate-set mapping table with the pointer
	 * to the correct basic rate for the given incoming rate
	 */
	brcms_b_write_shm(wlc->hw, (basic_table + index * 2), basic_ptr);
}

/* The rate sets this chip offers, and the choice between them. Taken from
 * brcmsmac's rate.c and main.c. Nothing in this driver reached them before,
 * so the compiler dropped every function that did -- which is why the ACK
 * and CTS rate table was never written, and why an access point that tried
 * to talk quickly heard nothing back and settled for 802.11b.
 */
const struct brcms_c_rateset cck_ofdm_mimo_rates = {
	12,
	/*  1b,   2b,   5.5b, 6,    9,    11b,  12,   18,   24,   36,   48, */
	{ 0x82, 0x84, 0x8b, 0x0c, 0x12, 0x96, 0x18, 0x24, 0x30, 0x48, 0x60,
	/* 54 Mbps */
	  0x6c},
	0x00,
	{ 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	  0x00, 0x00, 0x00, 0x00, 0x00}
};

const struct brcms_c_rateset ofdm_mimo_rates = {
	8,
	/*  6b,   9,    12b,  18,   24b,  36,   48,   54 Mbps */
	{ 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c},
	0x00,
	{ 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	  0x00, 0x00, 0x00, 0x00, 0x00}
};

const struct brcms_c_rateset cck_ofdm_rates = {
	12,
	/*  1b,   2b, 5.5b, 6,    9,    11b,  12,   18,   24,   36,   48,*/
	{ 0x82, 0x84, 0x8b, 0x0c, 0x12, 0x96, 0x18, 0x24, 0x30, 0x48, 0x60,
	/*54 Mbps */
	  0x6c},
	0x00,
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	  0x00, 0x00, 0x00, 0x00, 0x00}
};

const struct brcms_c_rateset cck_rates = {
	4,
	/*  1b,   2b,   5.5,  11 Mbps */
	{ 0x82, 0x84, 0x0b, 0x16},
	0x00,
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	  0x00, 0x00, 0x00, 0x00, 0x00}
};


void brcms_c_rateset_copy(const struct brcms_c_rateset *src,
			  struct brcms_c_rateset *dst)
{
	memcpy(dst, src, sizeof(struct brcms_c_rateset));
}


void brcms_c_rateset_mcs_upd(struct brcms_c_rateset *rs, u8 txstreams)
{
	int i;
	for (i = txstreams; i < MAX_STREAMS_SUPPORTED; i++)
		rs->mcs[i] = 0;
}


const struct brcms_c_rateset *brcms_c_rateset_get_hwrs(struct brcms_c_info *wlc);


/* Which rate to answer a frame at, for every rate a frame can arrive at.
 * Taken from brcmsmac's main.c. Nothing in this driver built it, so every
 * entry read zero -- and brcms_basic_rate() returning zero sends the
 * acknowledgement at the slowest rate the network has. An acknowledgement
 * at one megabit takes three hundred microseconds, which is longer than
 * the frame it is answering takes at any 802.11n scheme, so it arrives
 * after the access point has stopped listening for it. The access point
 * counts that as a frame lost and tries something slower, all the way down
 * to 802.11b.
 */
static void brcms_c_rate_lookup_init(struct brcms_c_info *wlc,
			      const struct brcms_c_rateset *rateset)
{
	u8 rate;
	u8 mandatory;
	u8 cck_basic = 0;
	u8 ofdm_basic = 0;
	u8 *br = wlc->band->basic_rate;
	uint i;

	memset(br, 0, BRCM_MAXRATE + 1);

	for (i = 0; i < rateset->count; i++) {
		if (!(rateset->rates[i] & BRCMS_RATE_FLAG))
			continue;

		rate = (rateset->rates[i] & BRCMS_RATE_MASK);

		if (rate > BRCM_MAXRATE)
			continue;

		br[rate] = rate;
	}

	for (i = 0; i < wlc->band->hw_rateset.count; i++) {
		rate = wlc->band->hw_rateset.rates[i];

		if (br[rate] != 0) {
			if (is_ofdm_rate(rate))
				ofdm_basic = rate;
			else
				cck_basic = rate;

			continue;
		}

		br[rate] = is_ofdm_rate(rate) ? ofdm_basic : cck_basic;

		if (br[rate] != 0)
			continue;

		if (is_ofdm_rate(rate)) {
			if (rate >= BRCM_RATE_24M)
				mandatory = BRCM_RATE_24M;
			else if (rate >= BRCM_RATE_12M)
				mandatory = BRCM_RATE_12M;
			else
				mandatory = BRCM_RATE_6M;
		} else
			mandatory = rate;

		br[rate] = mandatory;
	}
}


/*!	Crosses from one band to the other, the way brcmsmac does it.

	Not just a pointer swap. The radio of the band being left is switched
	off, the phy's clock is stopped across the change and started again,
	and only then is the band's own initialisation run. The clock has to
	stop because brcms_c_setxband() changes the core's G mode flag in
	between -- the core is told which band it is on, and it will not
	transmit on the other one until it has been.

	brcmsmac's brcms_b_setband(), without the interrupt save and restore:
	the caller here has already masked them.
*/
void
brcms_glue_cross_band(uint bandunit, u16 chanspec)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();

	// The radio being left stops first.
	wlc_phy_switch_radio(wlc_hw->band->pi, OFF);

	brcms_b_core_phy_clk(wlc_hw, OFF);

	brcms_c_setxband(wlc_hw, bandunit);

	brcms_b_core_phy_clk(wlc_hw, ON);

	brcms_glue_band_init(chanspec);
}


/*!	Everything the hardware has to be told when it changes band.

	brcmsmac's brcms_b_bsinit(), which this driver reached none of. Only
	the phy initialisation was being done, and most of the rest is not
	optional: the shared memory the microcode reads is shared between the
	two bands, so what it holds for one of them is wrong for the other
	until it is written again, and the slot timing differs between them --
	five gigahertz is always short slot, where 2.4 GHz depends on who else
	is in the room.
*/
void
brcms_glue_band_init(u16 chanspec)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();

	if (wlc_hw->d11core == NULL || !wlc_hw->clk)
		return;

	brcms_c_ucode_bsinit(wlc_hw);

	wlc_phy_init(wlc_hw->band->pi, chanspec);

	brcms_c_ucode_txant_set(wlc_hw);

	brcms_b_set_cwmin(wlc_hw, wlc_hw->band->CWmin);
	brcms_b_set_cwmax(wlc_hw, wlc_hw->band->CWmax);

	brcms_b_update_slot_timing(wlc_hw,
		wlc_hw->band->bandtype == BRCM_BAND_5G ? true
			: wlc_hw->shortslot);

	brcms_b_write_shm(wlc_hw, M_PHYTYPE, (u16)wlc_hw->band->phytype);
	brcms_b_write_shm(wlc_hw, M_PHYVER, (u16)wlc_hw->band->phyrev);

	// The rate table the phy control word is built from, which is in that
	// same shared memory and so is the other band's until this is run.
	brcms_upd_ofdm_pctl1_table(wlc_hw);

	brcms_b_upd_synthpu(wlc_hw);
}


/*!	Puts this card's address in the block acknowledgement template.

	The microcode builds the block acknowledgement it sends back for a
	received aggregate out of a template in the chip's memory, and the
	address it is from has to be written there by the driver. Nothing
	wrote it, so every block acknowledgement this card sent came from
	nobody and was ignored, and an access point whose aggregates are never
	answered works its way down until it is sending 802.11b -- which is
	answered by an ordinary acknowledgement the microcode builds itself,
	with no template involved.

	brcmsmac calls this from brcms_c_ampdu_attach() and again whenever the
	address changes.
*/
void
brcms_glue_write_ba_template(void)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	char template[T_RAM_ACCESS_SZ * 2];

	if (wlc_hw->d11core == NULL || !wlc_hw->clk)
		return;

	// The address sits sixteen bytes into the template.
	memset(template, 0, sizeof(template));
	memcpy(template, wlc_hw->etheraddr, 6);

	brcms_b_write_template_ram(wlc_hw, (T_BA_TPL_BASE + 16),
		(T_RAM_ACCESS_SZ * 2), template);
}


/*!	How much the microcode will wait for, now that frames arrive in runs. */
void
brcms_glue_write_ampdu_shm(void)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();

	if (wlc_hw->d11core == NULL || !wlc_hw->clk)
		return;

	// brcms_vap_create advertises IEEE80211_HTCAP_MAXRXAMPDU_64K.
	// Match brcms_c_ampdu_shm_upd's 64 KiB case: the default limits
	// are for the smaller receive capability.
	brcms_b_write_shm(wlc_hw, M_MIMO_MAXSYM, MIMO_MAXSYM_MAX);
	brcms_b_write_shm(wlc_hw, M_WATCHDOG_8TU, WATCHDOG_8TU_MAX);
}


/*!	Builds the rate sets the microcode and this driver look things up in. */
void
brcms_glue_init_rate_lookup(void)
{
	struct brcms_c_info *wlc = brcms_glue_common();
	const struct brcms_c_rateset *dflt = brcms_c_rateset_get_hwrs(wlc);
	struct brcms_c_rateset *hw = &wlc->band->hw_rateset;
	uint i;

	// What the hardware can do, which is the same rates without the marks
	// that say which of them the network insists on.
	brcms_c_rateset_copy(dflt, hw);
	for (i = 0; i < hw->count; i++)
		hw->rates[i] &= BRCMS_RATE_MASK;
	brcms_c_rateset_mcs_upd(hw, wlc->stf->txstreams);

	brcms_c_rate_lookup_init(wlc, dflt);
}


u16
brcms_c_calc_lsig_len(struct brcms_c_info *wlc, u32 ratespec, uint mac_len)
{
	uint nsyms, len = 0, kNdps;

	if (is_mcs_rate(ratespec)) {
		uint mcs = ratespec & RSPEC_RATE_MASK;
		int tot_streams = (mcs_2_txstreams(mcs) + 1) +
				  rspec_stc(ratespec);

		/* 1000Ndbps = kbps * 4 */
		kNdps = mcs_2_rate(mcs, rspec_is40mhz(ratespec),
				   rspec_issgi(ratespec)) * 4;

		if (rspec_stc(ratespec) == 0)
			nsyms =
			    CEIL((APHY_SERVICE_NBITS + 8 * mac_len +
				  APHY_TAIL_NBITS) * 1000, kNdps);
		else
			/* STBC needs to have even number of symbols */
			nsyms =
			    2 *
			    CEIL((APHY_SERVICE_NBITS + 8 * mac_len +
				  APHY_TAIL_NBITS) * 1000, 2 * kNdps);

		/* (+3) account for HT-SIG(2) and HT-STF(1) */
		nsyms += (tot_streams + 3);
		/*
		 * 3 bytes/symbol @ legacy 6Mbps rate
		 * (-3) excluding service bits and tail bits
		 */
		len = (3 * nsyms) - 3;
	}

	return (u16) len;
}


const struct brcms_c_rateset *
brcms_c_rateset_get_hwrs(struct brcms_c_info *wlc)
{
	const struct brcms_c_rateset *rs_dflt;

	if (BRCMS_PHY_11N_CAP(wlc->band)) {
		if (wlc->band->bandtype == BRCM_BAND_5G)
			rs_dflt = &ofdm_mimo_rates;
		else
			rs_dflt = &cck_ofdm_mimo_rates;
	} else if (wlc->band->gmode)
		rs_dflt = &cck_ofdm_rates;
	else
		rs_dflt = &cck_rates;

	return rs_dflt;
}


static void brcms_c_set_ratetable(struct brcms_c_info *wlc)
{
	const struct brcms_c_rateset *rs_dflt;
	struct brcms_c_rateset rs;
	u8 rate, basic_rate;
	uint i;

	rs_dflt = brcms_c_rateset_get_hwrs(wlc);

	brcms_c_rateset_copy(rs_dflt, &rs);
	brcms_c_rateset_mcs_upd(&rs, wlc->stf->txstreams);

	/* walk the phy rate table and update SHM basic rate lookup table */
	for (i = 0; i < rs.count; i++) {
		rate = rs.rates[i] & BRCMS_RATE_MASK;

		/* for a given rate brcms_basic_rate returns the rate at
		 * which a response ACK/CTS should be sent.
		 */
		basic_rate = brcms_basic_rate(wlc, rate);
		if (basic_rate == 0)
			/* This should only happen if we are using a
			 * restricted rateset.
			 */
			basic_rate = rs.rates[0] & BRCMS_RATE_MASK;

		brcms_c_write_rate_shm(wlc, rate, basic_rate);
	}
}

/*!	Writes the tables the microcode answers a frame from.

	Everything brcms_c_bsinit() does except the antenna selection, which
	this driver has no state for.
*/
void
brcms_glue_write_rate_tables(void)
{
	struct brcms_c_info *wlc = brcms_glue_common();

	brcms_c_set_ratetable(wlc);
	brcms_c_ucode_mac_upd(wlc);
}


static void brcms_c_bsinit(struct brcms_c_info *wlc)
{
	brcms_dbg_info(wlc->hw->d11core, "wl%d: bandunit %d\n",
		       wlc->pub->unit, wlc->band->bandunit);

	/* write ucode ACK/CTS rate table */
	brcms_c_set_ratetable(wlc);

	/* update some band specific mac configuration */
	brcms_c_ucode_mac_upd(wlc);

	/* init antenna selection */
	brcms_c_antsel_init(wlc->asi);

}

static void brcms_c_set_ps_ctrl(struct brcms_c_info *wlc)
{
	u32 v1, v2;
	bool hps;
	bool awake_before;

	hps = brcms_c_ps_allowed(wlc);

	brcms_dbg_mac80211(wlc->hw->d11core, "wl%d: hps %d\n", wlc->pub->unit,
			   hps);

	v1 = bcma_read32(wlc->hw->d11core, D11REGOFFS(maccontrol));
	v2 = MCTL_WAKE;
	if (hps)
		v2 |= MCTL_HPS;

	brcms_b_mctrl(wlc->hw, MCTL_WAKE | MCTL_HPS, v2);

	awake_before = ((v1 & MCTL_WAKE) || ((v1 & MCTL_HPS) == 0));

	if (!awake_before)
		brcms_b_wait_for_wake(wlc->hw);
}

static void brcms_c_setband(struct brcms_c_info *wlc,
					   uint bandunit)
{
	wlc->band = wlc->bandstate[bandunit];

	if (!wlc->pub->up)
		return;

	/* wait for at least one beacon before entering sleeping state */
	brcms_c_set_ps_ctrl(wlc);

	/* band-specific initializations */
	brcms_c_bsinit(wlc);
}

static void brcms_c_set_chanspec(struct brcms_c_info *wlc, u16 chanspec)
{
	uint bandunit;
	u16 old_chanspec = wlc->chanspec;

	if (!brcms_c_valid_chanspec_db(wlc->cmi, chanspec)) {
		brcms_err(wlc->hw->d11core, "wl%d: %s: Bad channel %d\n",
			  wlc->pub->unit, __func__, CHSPEC_CHANNEL(chanspec));
		return;
	}

	/* Switch bands if necessary */
	if (wlc->pub->_nbands > 1) {
		bandunit = chspec_bandunit(chanspec);
		if (wlc->band->bandunit != bandunit || wlc->bandinit_pending) {
			if (wlc->bandlocked) {
				brcms_err(wlc->hw->d11core,
					  "wl%d: %s: chspec %d band is locked!\n",
					  wlc->pub->unit, __func__,
					  CHSPEC_CHANNEL(chanspec));
				return;
			}
			/*
			 * should the setband call come after the
			 * brcms_b_chanspec() ? if the setband updates
			 * (brcms_c_bsinit) use low level calls to inspect and
			 * set state, the state inspected may be from the wrong
			 * band, or the following brcms_b_set_chanspec() may
			 * undo the work.
			 */
			brcms_c_setband(wlc, bandunit);
		}
	}

	/* sync up phy/radio chanspec */
	brcms_c_set_phy_chanspec(wlc, chanspec);

	/* init antenna selection */
	if (brcms_chspec_bw(old_chanspec) != brcms_chspec_bw(chanspec)) {
		brcms_c_antsel_init(wlc->asi);

		/* Fix the hardware rateset based on bw.
		 * Mainly add MCS32 for 40Mhz, remove MCS 32 for 20Mhz
		 */
		brcms_c_rateset_bw_mcs_filter(&wlc->band->hw_rateset,
			wlc->band->mimo_cap_40 ? brcms_chspec_bw(chanspec) : 0);
	}

	/* update some mac configuration since chanspec changed */
	brcms_c_ucode_mac_upd(wlc);
}

void brcms_c_beacon_phytxctl_txant_upd(struct brcms_c_info *wlc,
				       u32 bcn_rspec)
{
	u16 phyctl;
	u16 phytxant = wlc->stf->phytxant;
	u16 mask = PHY_TXC_ANT_MASK;

	/* for non-siso rates or default setting, use the available chains */
	if (BRCMS_PHY_11N_CAP(wlc->band))
		phytxant = brcms_c_stf_phytxchain_sel(wlc, bcn_rspec);

	phyctl = brcms_b_read_shm(wlc->hw, M_BCN_PCTLWD);
	phyctl = (phyctl & ~mask) | phytxant;
	brcms_b_write_shm(wlc->hw, M_BCN_PCTLWD, phyctl);
}

static void brcms_c_ht_update_ldpc(struct brcms_c_info *wlc, s8 val)
{
	wlc->stf->ldpc = val;

	if (wlc->pub->up) {
		brcms_c_update_beacon(wlc);
		brcms_c_update_probe_resp(wlc, true);
		wlc_phy_ldpc_override_set(wlc->band->pi, (val ? true : false));
	}
}

static void brcms_c_radio_monitor_start(struct brcms_c_info *wlc)
{
	/* Don't start the timer if HWRADIO feature is disabled */
	if (wlc->radio_monitor)
		return;

	wlc->radio_monitor = true;
	brcms_b_pllreq(wlc->hw, true, BRCMS_PLLREQ_RADIO_MON);
	brcms_add_timer(wlc->radio_timer, TIMER_INTERVAL_RADIOCHK, true);
}

static bool brcms_c_radio_monitor_stop(struct brcms_c_info *wlc)
{
	if (!wlc->radio_monitor)
		return true;

	wlc->radio_monitor = false;
	brcms_b_pllreq(wlc->hw, false, BRCMS_PLLREQ_RADIO_MON);
	return brcms_del_timer(wlc->radio_timer);
}

struct brcms_pub *brcms_c_pub(struct brcms_c_info *wlc)
{
	return wlc->pub;
}

void brcms_b_hw_up(struct brcms_hardware *wlc_hw)
{
	if (wlc_hw->wlc->pub->hw_up)
		return;

	brcms_dbg_info(wlc_hw->d11core, "wl%d\n", wlc_hw->unit);

	/*
	 * Enable pll and xtal, initialize the power control registers,
	 * and force fastclock for the remainder of brcms_c_up().
	 */
	brcms_b_xtal(wlc_hw, ON);
	ai_clkctl_init(wlc_hw->sih);
	brcms_b_clkctl_clk(wlc_hw, BCMA_CLKMODE_FAST);

	/*
	 * TODO: test suspend/resume
	 *
	 * AI chip doesn't restore bar0win2 on
	 * hibernation/resume, need sw fixup
	 */

	/*
	 * Inform phy that a POR reset has occurred so
	 * it does a complete phy init
	 */
	wlc_phy_por_inform(wlc_hw->band->pi);

	wlc_hw->ucode_loaded = false;
	wlc_hw->wlc->pub->hw_up = true;

	if ((wlc_hw->boardflags & BFL_FEM)
	    && (ai_get_chip_id(wlc_hw->sih) == BCMA_CHIP_ID_BCM4313)) {
		if (!
		    (wlc_hw->boardrev >= 0x1250
		     && (wlc_hw->boardflags & BFL_FEM_BT)))
			ai_epa_4313war(wlc_hw->sih);
	}
}

int brcms_b_up_prep(struct brcms_hardware *wlc_hw)
{
	brcms_dbg_info(wlc_hw->d11core, "wl%d\n", wlc_hw->unit);

	/*
	 * Enable pll and xtal, initialize the power control registers,
	 * and force fastclock for the remainder of brcms_c_up().
	 */
	brcms_b_xtal(wlc_hw, ON);
	ai_clkctl_init(wlc_hw->sih);
	brcms_b_clkctl_clk(wlc_hw, BCMA_CLKMODE_FAST);

	/*
	 * Configure pci/pcmcia here instead of in brcms_c_attach()
	 * to allow mfg hotswap:  down, hotswap (chip power cycle), up.
	 */
	bcma_host_pci_irq_ctl(wlc_hw->d11core->bus, wlc_hw->d11core,
			      true);

	/*
	 * Need to read the hwradio status here to cover the case where the
	 * system is loaded with the hw radio disabled. We do not want to
	 * bring the driver up in this case.
	 */
	if (brcms_b_radio_read_hwdisabled(wlc_hw)) {
		/* put SB PCI in down state again */
		bcma_host_pci_down(wlc_hw->d11core->bus);
		brcms_b_xtal(wlc_hw, OFF);
		return -ENOMEDIUM;
	}

	bcma_host_pci_up(wlc_hw->d11core->bus);

	/* reset the d11 core */
	brcms_b_corereset(wlc_hw, BRCMS_USE_COREFLAGS);

	return 0;
}

int brcms_b_up_finish(struct brcms_hardware *wlc_hw)
{
	wlc_hw->up = true;
	wlc_phy_hw_state_upd(wlc_hw->band->pi, true);

	/* FULLY enable dynamic power control and d11 core interrupt */
	brcms_b_clkctl_clk(wlc_hw, BCMA_CLKMODE_DYNAMIC);
	brcms_intrson(wlc_hw->wlc->wl);
	return 0;
}

u16 brcms_b_rate_shm_offset(struct brcms_hardware *wlc_hw, u8 rate)
{
	u16 table_ptr;
	u8 phy_rate, index;

	/* get the phy specific rate encoding for the PLCP SIGNAL field */
	if (is_ofdm_rate(rate))
		table_ptr = M_RT_DIRMAP_A;
	else
		table_ptr = M_RT_DIRMAP_B;

	/* for a given rate, the LS-nibble of the PLCP SIGNAL field is
	 * the index into the rate table.
	 */
	phy_rate = rate_info[rate] & BRCMS_RATE_MASK;
	index = phy_rate & 0xf;

	/* Find the SHM pointer to the rate table entry by looking in the
	 * Direct-map Table
	 */
	return 2 * brcms_b_read_shm(wlc_hw, table_ptr + (index * 2));
}

bool brcms_c_dpc(struct brcms_c_info *wlc, bool bounded)
{
	u32 macintstatus;
	struct brcms_hardware *wlc_hw = wlc->hw;
	struct bcma_device *core = wlc_hw->d11core;

	if (brcms_deviceremoved(wlc)) {
		brcms_err(core, "wl%d: %s: dead chip\n", wlc_hw->unit,
			  __func__);
		brcms_down(wlc->wl);
		return false;
	}

	/* grab and clear the saved software intstatus bits */
	macintstatus = wlc->macintstatus;
	wlc->macintstatus = 0;

	brcms_dbg_int(core, "wl%d: macintstatus 0x%x\n",
		      wlc_hw->unit, macintstatus);

	WARN_ON(macintstatus & MI_PRQ); /* PRQ Interrupt in non-MBSS */

	/* tx status */
	if (macintstatus & MI_TFS) {
		bool fatal;
		if (brcms_b_txstatus(wlc->hw, bounded, &fatal))
			wlc->macintstatus |= MI_TFS;
		if (fatal) {
			brcms_err(core, "MI_TFS: fatal\n");
			goto fatal;
		}
	}

	if (macintstatus & (MI_TBTT | MI_DTIM_TBTT))
		brcms_c_tbtt(wlc);

	/* ATIM window end */
	if (macintstatus & MI_ATIMWINEND) {
		brcms_dbg_info(core, "end of ATIM window\n");
		bcma_set32(core, D11REGOFFS(maccommand), wlc->qvalid);
		wlc->qvalid = 0;
	}

	/*
	 * received data or control frame, MI_DMAINT is
	 * indication of RX_FIFO interrupt
	 */
	if (macintstatus & MI_DMAINT)
		if (brcms_b_recv(wlc_hw, RX_FIFO, bounded))
			wlc->macintstatus |= MI_DMAINT;

	/* noise sample collected */
	if (macintstatus & MI_BG_NOISE)
		wlc_phy_noise_sample_intr(wlc_hw->band->pi);

	if (macintstatus & MI_GP0) {
		brcms_err(core, "wl%d: PSM microcode watchdog fired at %d "
			  "(seconds). Resetting.\n", wlc_hw->unit, wlc_hw->now);

		printk_once("%s : PSM Watchdog, chipid 0x%x, chiprev 0x%x\n",
			    __func__, ai_get_chip_id(wlc_hw->sih),
			    ai_get_chiprev(wlc_hw->sih));
		brcms_fatal_error(wlc_hw->wlc->wl);
	}

	/* gptimer timeout */
	if (macintstatus & MI_TO)
		bcma_write32(core, D11REGOFFS(gptimer), 0);

	if (macintstatus & MI_RFDISABLE) {
		brcms_dbg_info(core, "wl%d: BMAC Detected a change on the"
			       " RF Disable Input\n", wlc_hw->unit);
		brcms_rfkill_set_hw_state(wlc->wl);
	}

	/* BCN template is available */
	if (macintstatus & MI_BCNTPL)
		brcms_c_update_beacon(wlc);

	/* it isn't done and needs to be resched if macintstatus is non-zero */
	return wlc->macintstatus != 0;

 fatal:
	brcms_fatal_error(wlc_hw->wlc->wl);
	return wlc->macintstatus != 0;
}

/* brcmsmac s own rate table, which is_ofdm_rate() indexes. */
const u8 rate_info[BRCM_MAXRATE + 1] = {
	/*  0     1     2     3     4     5     6     7     8     9 */
/*   0 */ 0x00, 0x00, 0x0a, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00,
/*  10 */ 0x00, 0x37, 0x8b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8f, 0x00,
/*  20 */ 0x00, 0x00, 0x6e, 0x00, 0x8a, 0x00, 0x00, 0x00, 0x00, 0x00,
/*  30 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8e, 0x00, 0x00, 0x00,
/*  40 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x89, 0x00,
/*  50 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
/*  60 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
/*  70 */ 0x00, 0x00, 0x8d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
/*  80 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
/*  90 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88, 0x00, 0x00, 0x00,
/* 100 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8c
};



/*!	The transmit fifo sizes for one core revision.

	brcms_b_attach() picks these out of a table that is private to
	brcmsmac's main.c, and brcms_b_coreinit() then reads them back out of
	struct brcms_hardware, so the table has to be reachable from where that
	structure is filled.
*/
const u16*
brcms_glue_xmtfifo_sz(uint corerev)
{
	if (corerev < XMTFIFOTBL_STARTREV
		|| (corerev - XMTFIFOTBL_STARTREV) >= ARRAY_SIZE(xmtfifo_sz)) {
		return NULL;
	}

	return xmtfifo_sz[corerev - XMTFIFOTBL_STARTREV];
}

/* The transmit header and its PLCP, from brcmsmac main.c. */
static void brcms_c_cck_plcp_set(struct brcms_c_info *wlc, int rate_500,
			     uint length, u8 *plcp)
{
	u16 usec = 0;
	u8 le = 0;

	switch (rate_500) {
	case BRCM_RATE_1M:
		usec = length << 3;
		break;
	case BRCM_RATE_2M:
		usec = length << 2;
		break;
	case BRCM_RATE_5M5:
		usec = (length << 4) / 11;
		if ((length << 4) - (usec * 11) > 0)
			usec++;
		break;
	case BRCM_RATE_11M:
		usec = (length << 3) / 11;
		if ((length << 3) - (usec * 11) > 0) {
			usec++;
			if ((usec * 11) - (length << 3) >= 8)
				le = D11B_PLCP_SIGNAL_LE;
		}
		break;

	default:
		brcms_err(wlc->hw->d11core,
			  "brcms_c_cck_plcp_set: unsupported rate %d\n",
			  rate_500);
		rate_500 = BRCM_RATE_1M;
		usec = length << 3;
		break;
	}
	/* PLCP signal byte */
	plcp[0] = rate_500 * 5;	/* r (500kbps) * 5 == r (100kbps) */
	/* PLCP service byte */
	plcp[1] = (u8) (le | D11B_PLCP_SIGNAL_LOCKED);
	/* PLCP length u16, little endian */
	plcp[2] = usec & 0xff;
	plcp[3] = (usec >> 8) & 0xff;
	/* PLCP CRC16 */
	plcp[4] = 0;
	plcp[5] = 0;
}

/* Rate: 802.11 rate code, length: PSDU length in octets */
static void brcms_c_compute_cck_plcp(struct brcms_c_info *wlc, u32 rspec,
				 uint length, u8 *plcp)
{
	int rate = rspec2rate(rspec);

	brcms_c_cck_plcp_set(wlc, rate, length, plcp);
}

/* Rate: 802.11 rate code, length: PSDU length in octets */
static void brcms_c_compute_mimo_plcp(u32 rspec, uint length, u8 *plcp)
{
	u8 mcs = (u8) (rspec & RSPEC_RATE_MASK);
	plcp[0] = mcs;
	if (rspec_is40mhz(rspec) || (mcs == 32))
		plcp[0] |= MIMO_PLCP_40MHZ;
	BRCMS_SET_MIMO_PLCP_LEN(plcp, length);
	plcp[3] = rspec_mimoplcp3(rspec); /* rspec already holds this byte */
	plcp[3] |= 0x7; /* set smoothing, not sounding ppdu & reserved */
	plcp[4] = 0; /* number of extension spatial streams bit 0 & 1 */
	plcp[5] = 0;
}

static void
brcms_c_compute_ofdm_plcp(u32 rspec, u32 length, u8 *plcp)
{
	u8 rate_signal;
	u32 tmp = 0;
	int rate = rspec2rate(rspec);

	/*
	 * encode rate per 802.11a-1999 sec 17.3.4.1, with lsb
	 * transmitted first
	 */
	rate_signal = rate_info[rate] & BRCMS_RATE_MASK;
	memset(plcp, 0, D11_PHY_HDR_LEN);
	D11A_PHY_HDR_SRATE((struct ofdm_phy_hdr *) plcp, rate_signal);

	tmp = (length & 0xfff) << 5;
	plcp[2] |= (tmp >> 16) & 0xff;
	plcp[1] |= (tmp >> 8) & 0xff;
	plcp[0] |= tmp & 0xff;
}

void
brcms_c_compute_plcp(struct brcms_c_info *wlc, u32 rspec,
		     uint length, u8 *plcp)
{
	if (is_mcs_rate(rspec))
		brcms_c_compute_mimo_plcp(rspec, length, plcp);
	else if (is_ofdm_rate(rspec))
		brcms_c_compute_ofdm_plcp(rspec, length, plcp);
	else
		brcms_c_compute_cck_plcp(wlc, rspec, length, plcp);
}

int
brcms_c_txfifo(struct brcms_c_info *wlc, uint fifo, struct sk_buff *p)
{
	struct dma_pub *dma = wlc->hw->di[fifo];
	int ret;
	u16 queue;

	ret = dma_txfast(wlc, dma, p);
	// A failed submission frees p; do not inspect its queue afterward.
	if (ret != 0)
		return ret;

	/*
	 * Stop queue if DMA ring is full. Reserve some free descriptors,
	 * as we sometimes receive a frame from mac80211 after the queues
	 * are stopped.
	 */
	queue = skb_get_queue_mapping(p);
	if (dma->txavail <= TX_HEADROOM && fifo < TX_BCMC_FIFO &&
	    !ieee80211_queue_stopped(wlc->pub->ieee_hw, queue))
		ieee80211_stop_queue(wlc->pub->ieee_hw, queue);

	return ret;
}


/* What the transmit header is worked out from, from brcmsmac. */
struct legacy_phycfg {
	u32 rate_ofdm;	/* ofdm mac rate */
	/* phy ctl byte 3, code rate, modulation type, # of streams */
	u8 tx_phy_ctl3;
};

/* Number of legacy_rate_cfg entries in the table */
#define LEGACY_PHYCFG_TABLE_SIZE	12

/*
 * In CCK mode LPPHY overloads OFDM Modulation bits with CCK Data Rate
 * Eventually MIMOPHY would also be converted to this format
 * 0 = 1Mbps; 1 = 2Mbps; 2 = 5.5Mbps; 3 = 11Mbps
 */
static const struct
legacy_phycfg legacy_phycfg_table[LEGACY_PHYCFG_TABLE_SIZE] = {
	{BRCM_RATE_1M, 0x00},	/* CCK  1Mbps,  data rate  0 */
	{BRCM_RATE_2M, 0x08},	/* CCK  2Mbps,  data rate  1 */
	{BRCM_RATE_5M5, 0x10},	/* CCK  5.5Mbps,  data rate  2 */
	{BRCM_RATE_11M, 0x18},	/* CCK  11Mbps,  data rate   3 */
	/* OFDM  6Mbps,  code rate 1/2, BPSK,   1 spatial stream */
	{BRCM_RATE_6M, 0x00},
	/* OFDM  9Mbps,  code rate 3/4, BPSK,   1 spatial stream */
	{BRCM_RATE_9M, 0x02},
	/* OFDM  12Mbps, code rate 1/2, QPSK,   1 spatial stream */
	{BRCM_RATE_12M, 0x08},
	/* OFDM  18Mbps, code rate 3/4, QPSK,   1 spatial stream */
	{BRCM_RATE_18M, 0x0A},
	/* OFDM  24Mbps, code rate 1/2, 16-QAM, 1 spatial stream */
	{BRCM_RATE_24M, 0x10},
	/* OFDM  36Mbps, code rate 3/4, 16-QAM, 1 spatial stream */
	{BRCM_RATE_36M, 0x12},
	/* OFDM  48Mbps, code rate 2/3, 64-QAM, 1 spatial stream */
	{BRCM_RATE_48M, 0x19},
	/* OFDM  54Mbps, code rate 3/4, 64-QAM, 1 spatial stream */
	{BRCM_RATE_54M, 0x1A},
};

static u16 _brcms_c_stf_phytxchain_sel(struct brcms_c_info *wlc,
				       u32 rspec)
{
	u16 phytxant = wlc->stf->phytxant;

	if (rspec_stf(rspec) != PHY_TXC1_MODE_SISO)
		phytxant = wlc->stf->txchain << PHY_TXC_ANT_SHIFT;
	else if (wlc->stf->txant == ANT_TX_DEF)
		phytxant = wlc->stf->txchain << PHY_TXC_ANT_SHIFT;
	phytxant &= PHY_TXC_ANT_MASK;
	return phytxant;
}


s16 brcms_c_rate_legacy_phyctl(uint rate)
{
	uint i;
	for (i = 0; i < LEGACY_PHYCFG_TABLE_SIZE; i++)
		if (rate == legacy_phycfg_table[i].rate_ofdm)
			return legacy_phycfg_table[i].tx_phy_ctl3;

	return -1;
}


u16 frametype(u32 rspec, u8 mimoframe)
{
	if (is_mcs_rate(rspec))
		return mimoframe;
	return is_cck_rate(rspec) ? FT_CCK : FT_OFDM;
}


uint brcms_c_calc_frame_time(struct brcms_c_info *wlc, u32 ratespec,
				    u8 preamble_type, uint mac_len)
{
	uint nsyms, dur = 0, Ndps, kNdps;
	uint rate = rspec2rate(ratespec);

	if (rate == 0) {
		brcms_err(wlc->hw->d11core, "wl%d: WAR: using rate of 1 mbps\n",
			  wlc->pub->unit);
		rate = BRCM_RATE_1M;
	}

	if (is_mcs_rate(ratespec)) {
		uint mcs = ratespec & RSPEC_RATE_MASK;
		int tot_streams = mcs_2_txstreams(mcs) + rspec_stc(ratespec);

		dur = PREN_PREAMBLE + (tot_streams * PREN_PREAMBLE_EXT);
		if (preamble_type == BRCMS_MM_PREAMBLE)
			dur += PREN_MM_EXT;
		/* 1000Ndbps = kbps * 4 */
		kNdps = mcs_2_rate(mcs, rspec_is40mhz(ratespec),
				   rspec_issgi(ratespec)) * 4;

		if (rspec_stc(ratespec) == 0)
			nsyms =
			    CEIL((APHY_SERVICE_NBITS + 8 * mac_len +
				  APHY_TAIL_NBITS) * 1000, kNdps);
		else
			/* STBC needs to have even number of symbols */
			nsyms =
			    2 *
			    CEIL((APHY_SERVICE_NBITS + 8 * mac_len +
				  APHY_TAIL_NBITS) * 1000, 2 * kNdps);

		dur += APHY_SYMBOL_TIME * nsyms;
		if (wlc->band->bandtype == BRCM_BAND_2G)
			dur += DOT11_OFDM_SIGNAL_EXTENSION;
	} else if (is_ofdm_rate(rate)) {
		dur = APHY_PREAMBLE_TIME;
		dur += APHY_SIGNAL_TIME;
		/* Ndbps = Mbps * 4 = rate(500Kbps) * 2 */
		Ndps = rate * 2;
		/* NSyms = CEILING((SERVICE + 8*NBytes + TAIL) / Ndbps) */
		nsyms =
		    CEIL((APHY_SERVICE_NBITS + 8 * mac_len + APHY_TAIL_NBITS),
			 Ndps);
		dur += APHY_SYMBOL_TIME * nsyms;
		if (wlc->band->bandtype == BRCM_BAND_2G)
			dur += DOT11_OFDM_SIGNAL_EXTENSION;
	} else {
		/*
		 * calc # bits * 2 so factor of 2 in rate (1/2 mbps)
		 * will divide out
		 */
		mac_len = mac_len * 8 * 2;
		/* calc ceiling of bits/rate = microseconds of air time */
		dur = (mac_len + rate - 1) / rate;
		if (preamble_type & BRCMS_SHORT_PREAMBLE)
			dur += BPHY_PLCP_SHORT_TIME;
		else
			dur += BPHY_PLCP_TIME;
	}
	return dur;
}


u16 brcms_c_phytxctl1_calc(struct brcms_c_info *wlc, u32 rspec)
{
	u16 phyctl1 = 0;
	u16 bw;

	if (BRCMS_ISLCNPHY(wlc->band)) {
		bw = PHY_TXC1_BW_20MHZ;
	} else {
		bw = rspec_get_bw(rspec);
		/* 10Mhz is not supported yet */
		if (bw < PHY_TXC1_BW_20MHZ) {
			brcms_err(wlc->hw->d11core, "phytxctl1_calc: bw %d is "
				  "not supported yet, set to 20L\n", bw);
			bw = PHY_TXC1_BW_20MHZ;
		}
	}

	if (is_mcs_rate(rspec)) {
		uint mcs = rspec & RSPEC_RATE_MASK;

		/* bw, stf, coding-type is part of rspec_phytxbyte2 returns */
		phyctl1 = rspec_phytxbyte2(rspec);
		/* set the upper byte of phyctl1 */
		phyctl1 |= (mcs_table[mcs].tx_phy_ctl3 << 8);
	} else if (is_cck_rate(rspec) && !BRCMS_ISLCNPHY(wlc->band)
		   && !BRCMS_ISSSLPNPHY(wlc->band)) {
		/*
		 * In CCK mode LPPHY overloads OFDM Modulation bits with CCK
		 * Data Rate. Eventually MIMOPHY would also be converted to
		 * this format
		 */
		/* 0 = 1Mbps; 1 = 2Mbps; 2 = 5.5Mbps; 3 = 11Mbps */
		phyctl1 = (bw | (rspec_stf(rspec) << PHY_TXC1_MODE_SHIFT));
	} else {		/* legacy OFDM/CCK */
		s16 phycfg;
		/* get the phyctl byte from rate phycfg table */
		phycfg = brcms_c_rate_legacy_phyctl(rspec2rate(rspec));
		if (phycfg == -1) {
			brcms_err(wlc->hw->d11core, "phytxctl1_calc: wrong "
				  "legacy OFDM/CCK rate\n");
			phycfg = 0;
		}
		/* set the upper byte of phyctl1 */
		phyctl1 =
		    (bw | (phycfg << 8) |
		     (rspec_stf(rspec) << PHY_TXC1_MODE_SHIFT));
	}
	return phyctl1;
}


u16 brcms_c_stf_d11hdrs_phyctl_txant(struct brcms_c_info *wlc, u32 rspec)
{
	u16 phytxant = wlc->stf->phytxant;
	u16 mask = PHY_TXC_ANT_MASK;

	/* for non-siso rates or default setting, use the available chains */
	if (BRCMS_ISNPHY(wlc->band)) {
		phytxant = _brcms_c_stf_phytxchain_sel(wlc, rspec);
		mask = PHY_TXC_HTANT_MASK;
	}
	phytxant |= phytxant & mask;
	return phytxant;
}

/* brcmsmac s MCS table, which the rate helpers index. */
const struct brcms_mcs_info mcs_table[MCS_TABLE_SIZE] = {
	/* MCS  0: SS 1, MOD: BPSK,  CR 1/2 */
	{6500, 13500, CEIL(6500 * 10, 9), CEIL(13500 * 10, 9), 0x00,
	 BRCM_RATE_6M},
	/* MCS  1: SS 1, MOD: QPSK,  CR 1/2 */
	{13000, 27000, CEIL(13000 * 10, 9), CEIL(27000 * 10, 9), 0x08,
	 BRCM_RATE_12M},
	/* MCS  2: SS 1, MOD: QPSK,  CR 3/4 */
	{19500, 40500, CEIL(19500 * 10, 9), CEIL(40500 * 10, 9), 0x0A,
	 BRCM_RATE_18M},
	/* MCS  3: SS 1, MOD: 16QAM, CR 1/2 */
	{26000, 54000, CEIL(26000 * 10, 9), CEIL(54000 * 10, 9), 0x10,
	 BRCM_RATE_24M},
	/* MCS  4: SS 1, MOD: 16QAM, CR 3/4 */
	{39000, 81000, CEIL(39000 * 10, 9), CEIL(81000 * 10, 9), 0x12,
	 BRCM_RATE_36M},
	/* MCS  5: SS 1, MOD: 64QAM, CR 2/3 */
	{52000, 108000, CEIL(52000 * 10, 9), CEIL(108000 * 10, 9), 0x19,
	 BRCM_RATE_48M},
	/* MCS  6: SS 1, MOD: 64QAM, CR 3/4 */
	{58500, 121500, CEIL(58500 * 10, 9), CEIL(121500 * 10, 9), 0x1A,
	 BRCM_RATE_54M},
	/* MCS  7: SS 1, MOD: 64QAM, CR 5/6 */
	{65000, 135000, CEIL(65000 * 10, 9), CEIL(135000 * 10, 9), 0x1C,
	 BRCM_RATE_54M},
	/* MCS  8: SS 2, MOD: BPSK,  CR 1/2 */
	{13000, 27000, CEIL(13000 * 10, 9), CEIL(27000 * 10, 9), 0x40,
	 BRCM_RATE_6M},
	/* MCS  9: SS 2, MOD: QPSK,  CR 1/2 */
	{26000, 54000, CEIL(26000 * 10, 9), CEIL(54000 * 10, 9), 0x48,
	 BRCM_RATE_12M},
	/* MCS 10: SS 2, MOD: QPSK,  CR 3/4 */
	{39000, 81000, CEIL(39000 * 10, 9), CEIL(81000 * 10, 9), 0x4A,
	 BRCM_RATE_18M},
	/* MCS 11: SS 2, MOD: 16QAM, CR 1/2 */
	{52000, 108000, CEIL(52000 * 10, 9), CEIL(108000 * 10, 9), 0x50,
	 BRCM_RATE_24M},
	/* MCS 12: SS 2, MOD: 16QAM, CR 3/4 */
	{78000, 162000, CEIL(78000 * 10, 9), CEIL(162000 * 10, 9), 0x52,
	 BRCM_RATE_36M},
	/* MCS 13: SS 2, MOD: 64QAM, CR 2/3 */
	{104000, 216000, CEIL(104000 * 10, 9), CEIL(216000 * 10, 9), 0x59,
	 BRCM_RATE_48M},
	/* MCS 14: SS 2, MOD: 64QAM, CR 3/4 */
	{117000, 243000, CEIL(117000 * 10, 9), CEIL(243000 * 10, 9), 0x5A,
	 BRCM_RATE_54M},
	/* MCS 15: SS 2, MOD: 64QAM, CR 5/6 */
	{130000, 270000, CEIL(130000 * 10, 9), CEIL(270000 * 10, 9), 0x5C,
	 BRCM_RATE_54M},
	/* MCS 16: SS 3, MOD: BPSK,  CR 1/2 */
	{19500, 40500, CEIL(19500 * 10, 9), CEIL(40500 * 10, 9), 0x80,
	 BRCM_RATE_6M},
	/* MCS 17: SS 3, MOD: QPSK,  CR 1/2 */
	{39000, 81000, CEIL(39000 * 10, 9), CEIL(81000 * 10, 9), 0x88,
	 BRCM_RATE_12M},
	/* MCS 18: SS 3, MOD: QPSK,  CR 3/4 */
	{58500, 121500, CEIL(58500 * 10, 9), CEIL(121500 * 10, 9), 0x8A,
	 BRCM_RATE_18M},
	/* MCS 19: SS 3, MOD: 16QAM, CR 1/2 */
	{78000, 162000, CEIL(78000 * 10, 9), CEIL(162000 * 10, 9), 0x90,
	 BRCM_RATE_24M},
	/* MCS 20: SS 3, MOD: 16QAM, CR 3/4 */
	{117000, 243000, CEIL(117000 * 10, 9), CEIL(243000 * 10, 9), 0x92,
	 BRCM_RATE_36M},
	/* MCS 21: SS 3, MOD: 64QAM, CR 2/3 */
	{156000, 324000, CEIL(156000 * 10, 9), CEIL(324000 * 10, 9), 0x99,
	 BRCM_RATE_48M},
	/* MCS 22: SS 3, MOD: 64QAM, CR 3/4 */
	{175500, 364500, CEIL(175500 * 10, 9), CEIL(364500 * 10, 9), 0x9A,
	 BRCM_RATE_54M},
	/* MCS 23: SS 3, MOD: 64QAM, CR 5/6 */
	{195000, 405000, CEIL(195000 * 10, 9), CEIL(405000 * 10, 9), 0x9B,
	 BRCM_RATE_54M},
	/* MCS 24: SS 4, MOD: BPSK,  CR 1/2 */
	{26000, 54000, CEIL(26000 * 10, 9), CEIL(54000 * 10, 9), 0xC0,
	 BRCM_RATE_6M},
	/* MCS 25: SS 4, MOD: QPSK,  CR 1/2 */
	{52000, 108000, CEIL(52000 * 10, 9), CEIL(108000 * 10, 9), 0xC8,
	 BRCM_RATE_12M},
	/* MCS 26: SS 4, MOD: QPSK,  CR 3/4 */
	{78000, 162000, CEIL(78000 * 10, 9), CEIL(162000 * 10, 9), 0xCA,
	 BRCM_RATE_18M},
	/* MCS 27: SS 4, MOD: 16QAM, CR 1/2 */
	{104000, 216000, CEIL(104000 * 10, 9), CEIL(216000 * 10, 9), 0xD0,
	 BRCM_RATE_24M},
	/* MCS 28: SS 4, MOD: 16QAM, CR 3/4 */
	{156000, 324000, CEIL(156000 * 10, 9), CEIL(324000 * 10, 9), 0xD2,
	 BRCM_RATE_36M},
	/* MCS 29: SS 4, MOD: 64QAM, CR 2/3 */
	{208000, 432000, CEIL(208000 * 10, 9), CEIL(432000 * 10, 9), 0xD9,
	 BRCM_RATE_48M},
	/* MCS 30: SS 4, MOD: 64QAM, CR 3/4 */
	{234000, 486000, CEIL(234000 * 10, 9), CEIL(486000 * 10, 9), 0xDA,
	 BRCM_RATE_54M},
	/* MCS 31: SS 4, MOD: 64QAM, CR 5/6 */
	{260000, 540000, CEIL(260000 * 10, 9), CEIL(540000 * 10, 9), 0xDB,
	 BRCM_RATE_54M},
	/* MCS 32: SS 1, MOD: BPSK,  CR 1/2 */
	{0, 6000, 0, CEIL(6000 * 10, 9), 0x00, BRCM_RATE_6M},
};


/* The contention parameters, from brcmsmac main.c. */
void brcms_c_edcf_setparams(struct brcms_c_info *wlc, bool suspend)
{
	u16 aci;
	int i_ac;
	struct ieee80211_tx_queue_params txq_pars;
	static const struct edcf_acparam default_edcf_acparams[] = {
		 {EDCF_AC_BE_ACI_STA, EDCF_AC_BE_ECW_STA, EDCF_AC_BE_TXOP_STA},
		 {EDCF_AC_BK_ACI_STA, EDCF_AC_BK_ECW_STA, EDCF_AC_BK_TXOP_STA},
		 {EDCF_AC_VI_ACI_STA, EDCF_AC_VI_ECW_STA, EDCF_AC_VI_TXOP_STA},
		 {EDCF_AC_VO_ACI_STA, EDCF_AC_VO_ECW_STA, EDCF_AC_VO_TXOP_STA}
	}; /* ucode needs these parameters during its initialization */
	const struct edcf_acparam *edcf_acp = &default_edcf_acparams[0];

	for (i_ac = 0; i_ac < IEEE80211_NUM_ACS; i_ac++, edcf_acp++) {
		/* find out which ac this set of params applies to */
		// The on-air ACI order is BE, BK, VI, VO; mac80211 uses
		// VO, VI, BE, BK. Translate before selecting the hardware FIFO.
		static const u8 aci_to_queue[] = {
			IEEE80211_AC_BE, IEEE80211_AC_BK, IEEE80211_AC_VI, IEEE80211_AC_VO};
		aci = aci_to_queue[(edcf_acp->ACI & EDCF_ACI_MASK) >> EDCF_ACI_SHIFT];

		/* fill in shm ac params struct */
		txq_pars.txop = edcf_acp->TXOP;
		txq_pars.aifs = edcf_acp->ACI;

		/* CWmin = 2^(ECWmin) - 1 */
		txq_pars.cw_min = EDCF_ECW2CW(edcf_acp->ECW & EDCF_ECWMIN_MASK);
		/* CWmax = 2^(ECWmax) - 1 */
		txq_pars.cw_max = EDCF_ECW2CW((edcf_acp->ECW & EDCF_ECWMAX_MASK)
					    >> EDCF_ECWMAX_SHIFT);
		brcms_c_wme_setparams(wlc, aci, &txq_pars, suspend);
	}

	if (suspend) {
		brcms_c_suspend_mac_and_wait(wlc);
		brcms_c_enable_mac(wlc);
	}
}

/* Programming one of the four queues, from brcmsmac main.c. */
void brcms_c_wme_setparams(struct brcms_c_info *wlc, u16 aci,
		       const struct ieee80211_tx_queue_params *params,
		       bool suspend)
{
	int i;
	struct shm_acparams acp_shm;
	u16 *shm_entry;

	/* Only apply params if the core is out of reset and has clocks */
	if (!wlc->hw->clk) {
		brcms_err(wlc->hw->d11core, "wl%d: %s : no-clock\n",
			  wlc->pub->unit, __func__);
		return;
	}

	memset(&acp_shm, 0, sizeof(struct shm_acparams));
	/* fill in shm ac params struct */
	acp_shm.txop = params->txop;
	/* convert from units of 32us to us for ucode */
	wlc->edcf_txop[aci & 0x3] = acp_shm.txop =
	    EDCF_TXOP2USEC(acp_shm.txop);
	acp_shm.aifs = (params->aifs & EDCF_AIFSN_MASK);

	if (aci == IEEE80211_AC_VI && acp_shm.txop == 0
	    && acp_shm.aifs < EDCF_AIFSN_MAX)
		acp_shm.aifs++;

	if (acp_shm.aifs < EDCF_AIFSN_MIN
	    || acp_shm.aifs > EDCF_AIFSN_MAX) {
		brcms_err(wlc->hw->d11core, "wl%d: edcf_setparams: bad "
			  "aifs %d\n", wlc->pub->unit, acp_shm.aifs);
	} else {
		acp_shm.cwmin = params->cw_min;
		acp_shm.cwmax = params->cw_max;
		acp_shm.cwcur = acp_shm.cwmin;
		acp_shm.bslots =
			bcma_read16(wlc->hw->d11core, D11REGOFFS(tsf_random)) &
			acp_shm.cwcur;
		acp_shm.reggap = acp_shm.bslots + acp_shm.aifs;
		/* Indicate the new params to the ucode */
		acp_shm.status = brcms_b_read_shm(wlc->hw, (M_EDCF_QINFO +
						  wme_ac2fifo[aci] *
						  M_EDCF_QLEN +
						  M_EDCF_STATUS_OFF));
		acp_shm.status |= WME_STATUS_NEWAC;

		/* Fill in shm acparam table */
		shm_entry = (u16 *) &acp_shm;
		for (i = 0; i < (int)sizeof(struct shm_acparams); i += 2)
			brcms_b_write_shm(wlc->hw,
					  M_EDCF_QINFO +
					  wme_ac2fifo[aci] * M_EDCF_QLEN + i,
					  *shm_entry++);
	}

	if (suspend)
		brcms_c_suspend_mac_and_wait(wlc);

	brcms_c_update_beacon(wlc);
	brcms_c_update_probe_resp(wlc, false);

	if (suspend)
		brcms_c_enable_mac(wlc);
}



/* --- the beacon this driver does not send ----------------------------- */

/*!	Rewriting a beacon template, which there is none of.

	brcms_c_wme_setparams() rewrites the beacon and probe response when the
	contention parameters change, because an access point advertises them.
	This is a station; it sends neither.
*/
void
brcms_c_update_probe_resp(struct brcms_c_info* wlc, bool suspend)
{
}

/* Working out how long the air must be held, from main.c. */
brcms_c_calc_ack_time(struct brcms_c_info *wlc, u32 rspec,
		      u8 preamble_type)
{
	uint dur = 0;

	/*
	 * Spec 9.6: ack rate is the highest rate in BSSBasicRateSet that
	 * is less than or equal to the rate of the immediately previous
	 * frame in the FES
	 */
	rspec = brcms_basic_rate(wlc, rspec);
	/* ACK frame len == 14 == 2(fc) + 2(dur) + 6(ra) + 4(fcs) */
	dur =
	    brcms_c_calc_frame_time(wlc, rspec, preamble_type,
				(DOT11_ACK_LEN + FCS_LEN));
	return dur;
}

brcms_c_calc_ba_time(struct brcms_c_info *wlc, u32 rspec,
		     u8 preamble_type)
{
	/*
	 * Spec 9.6: ack rate is the highest rate in BSSBasicRateSet that
	 * is less than or equal to the rate of the immediately previous
	 * frame in the FES
	 */
	rspec = brcms_basic_rate(wlc, rspec);
	/* BA len == 32 == 16(ctl hdr) + 4(ba len) + 8(bitmap) + 4(fcs) */
	return brcms_c_calc_frame_time(wlc, rspec, preamble_type,
				   (DOT11_BA_LEN + DOT11_BA_BITMAP_LEN +
				    FCS_LEN));
}

brcms_c_calc_cts_time(struct brcms_c_info *wlc, u32 rspec,
		      u8 preamble_type)
{
	return brcms_c_calc_ack_time(wlc, rspec, preamble_type);
}

u16
brcms_c_compute_rtscts_dur(struct brcms_c_info *wlc, bool cts_only,
			   u32 rts_rate,
			   u32 frame_rate, u8 rts_preamble_type,
			   u8 frame_preamble_type, uint frame_len, bool ba)
{
	u16 dur, sifs;

	sifs = get_sifs(wlc->band);

	if (!cts_only) {
		/* RTS/CTS */
		dur = 3 * sifs;
		dur +=
		    (u16) brcms_c_calc_cts_time(wlc, rts_rate,
					       rts_preamble_type);
	} else {
		/* CTS-TO-SELF */
		dur = 2 * sifs;
	}

	dur +=
	    (u16) brcms_c_calc_frame_time(wlc, frame_rate, frame_preamble_type,
					 frame_len);
	if (ba)
		dur +=
		    (u16) brcms_c_calc_ba_time(wlc, frame_rate,
					      BRCMS_SHORT_PREAMBLE);
	else
		dur +=
		    (u16) brcms_c_calc_ack_time(wlc, frame_rate,
					       frame_preamble_type);
	return dur;
}


static u16 get_sifs(struct brcms_band *band)
{
	return band->bandtype == BRCM_BAND_5G ? APHY_SIFS_TIME :
				 BPHY_SIFS_TIME;
}


u16
brcms_glue_ack_duration(u32 rate, u8 preamble)
{
	struct brcms_c_info* wlc = brcms_glue_common();
	return get_sifs(wlc->band) + brcms_c_calc_ack_time(wlc, rate, preamble);
}


u16
brcms_glue_ba_duration(u32 rate)
{
	struct brcms_c_info* wlc = brcms_glue_common();
	// Compressed BA: 16-byte control header, control/sequence (4),
	// 64-bit bitmap (8), and FCS (4).
	return get_sifs(wlc->band) + brcms_c_calc_frame_time(wlc,
		brcms_basic_rate(wlc, rate), BRCMS_LONG_PREAMBLE, 32);
}
