/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	Being told rather than asking.

	The ring has been read on a timer, fifty times a second, which is
	enough to scan with and is not enough to associate with: an access
	point expects an answer to its authentication within a few tens of
	milliseconds, and a frame that arrives just after a sweep waits for the
	next one. The card can say when it has something; this is that.

	What it says is in one register. The microcode raises a bit per kind of
	event -- a frame arrived, a frame was sent, the sequencer stopped -- and
	the chip asserts its line while any of the bits the mask allows is set.
	Clearing them is writing them back.
*/


#include "linux/types.h"

#include <KernelExport.h>

#include "net/mac80211.h"
#include "linux/bcma/bcma.h"
#include "d11.h"
#include "debug_haiku.h"
#include "glue.h"
#include "glue_net.h"
#include "main.h"
#include "pub.h"
#include "types.h"


#define TRACE(x...)	dprintf("broadcom43xxn: " x)

/*!	What this driver wants to hear about, which is less than it might.

	A frame arriving, and nothing else.

	Not MI_MACSSPNDD: that bit is how suspending the MAC reports itself,
	and brcms_c_suspend_mac_and_wait() watches for it by reading the
	register. An interrupt that acknowledged it first would leave that wait
	watching for something already taken away.

	MI_TFS is asked for: the card raises it while there is anything in the
	transmit status queue, and what answers it empties that queue and takes
	the buffers back. Without it nothing would ever be given back and a
	pool of sixty four would last sixty four frames.
*/
#define WANTED_INTERRUPTS	(MI_DMAINT | MI_TFS)


static uint32 sMask;
static int32 sPending;
static uint32 sInterrupts;
static uint32 sNotOurs;


/*!	Whether the line is being asserted by this card, and if so, quiet it.

	Runs in the interrupt itself, so it does as little as it can: read the
	status, decide, and stop the card asking again until the frames have
	been taken off the ring.
*/
bool
brcms_glue_interrupt_claim(void)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	if (wlc_hw->d11core == NULL || !wlc_hw->clk)
		return false;

	uint32 status = bcma_read32(wlc_hw->d11core, D11REGOFFS(macintstatus));

	// All ones is a card that has stopped answering -- pulled out, or
	// powered down -- rather than a card with everything to say.
	if (status == 0xffffffff)
		return false;

	status &= sMask;
	if (status == 0) {
		sNotOurs++;
		return false;
	}

	// Acknowledged by writing back, and nothing more asked for until what
	// has arrived has been dealt with.
	bcma_write32(wlc_hw->d11core, D11REGOFFS(macintstatus), status);

	// MI_DMAINT is not an event of its own. It summarises the receive
	// engine's own interrupt register, and writing the summary back does
	// not touch what it summarises. Left alone, it is set again the moment
	// the mask is reopened, and the card asks a hundred thousand times a
	// second: the counters read fifty six million interrupts for eighty
	// thousand frames, and a processor was spending its day answering.
	//
	// The only receive interrupt asked for is I_RI on the receive queue,
	// so that is what is cleared, which is what brcmsmac clears.
	if ((status & MI_DMAINT) != 0) {
		bcma_write32(wlc_hw->d11core,
			D11REGOFFS(intctrlregs[RX_FIFO].intstatus), DEF_RXINTMASK);
	}

	bcma_write32(wlc_hw->d11core, D11REGOFFS(macintmask), 0);

	atomic_or(&sPending, status);
	sInterrupts++;

	return true;
}


/*!	Deals with what the card said, with the interrupt no longer held. */
void
brcms_glue_interrupt_handle(void)
{
	// Claim and consume can run on different CPUs on a shared IRQ.
	uint32 status = atomic_get_and_set(&sPending, 0);

	if ((status & MI_TFS) != 0)
		brcms_glue_reclaim_sent();

	if ((status & MI_DMAINT) != 0 || brcms_glue_rx_pending())
		brcms_glue_poll_frames_bounded();
}


/*!	Lets the card ask again. */
void
brcms_glue_interrupt_enable(void)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	if (wlc_hw->d11core == NULL || !wlc_hw->clk)
		return;

	bcma_write32(wlc_hw->d11core, D11REGOFFS(macintmask), sMask);
}


/*!	Opens the mask, so the card has something it may ask about. */
void
brcms_glue_interrupts_on(void)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();
	if (wlc_hw->d11core == NULL || !wlc_hw->clk)
		return;

	sMask = WANTED_INTERRUPTS;
	sInterrupts = 0;
	sNotOurs = 0;
	atomic_get_and_set(&sPending, 0);

	// Anything outstanding is from before this was asked for, in the
	// summary and in the receive engine that one of its bits summarises.
	bcma_write32(wlc_hw->d11core,
		D11REGOFFS(intctrlregs[RX_FIFO].intstatus), DEF_RXINTMASK);
	bcma_write32(wlc_hw->d11core, D11REGOFFS(macintstatus), -1);
	bcma_write32(wlc_hw->d11core, D11REGOFFS(macintmask), sMask);
}


/*!	Closes it again, and leaves nothing outstanding. */
void
brcms_glue_interrupts_off(void)
{
	struct brcms_hardware* wlc_hw = brcms_glue_hardware();

	sMask = 0;

	if (wlc_hw->d11core == NULL || !wlc_hw->clk)
		return;

	bcma_write32(wlc_hw->d11core, D11REGOFFS(macintmask), 0);
	bcma_write32(wlc_hw->d11core, D11REGOFFS(macintstatus), -1);
}


/*!	How many times the card has asked so far. */
uint32
brcms_glue_interrupt_count(void)
{
	return sInterrupts;
}


void
brcms_glue_interrupt_report(void)
{
	TRACE("interrupts: %" B_PRIu32 " taken, %" B_PRIu32 " not ours\n",
		sInterrupts, sNotOurs);
}
