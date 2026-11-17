/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


/*!	Interrupt and receive path of rtwn over SDIO.

	rtwn_sdio_handle() does the work of a chip interrupt: the card
	interrupt reaches it through rtwn_sdio_service(), and while the card
	interrupt is off (see rtwn_sdio_attach()) the poll thread calls it
	directly. REG_SDIO_HISR says why; for received data
	REG_SDIO_RX0_REQ_LEN gives the number of bytes waiting in the receive
	FIFO, which are read with one CMD53 from the RX0 FIFO address.
	The buffer holds one or more frames, each with a receive descriptor and
	aligned to 8 bytes, like an aggregated USB transfer. The order of the
	steps follows the rtw88 driver (sdio.c).
*/


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/bus.h>
#include <sys/endian.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/ethernet.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>

#include <dev/sdio/sdio_haiku.h>

#include <dev/rtwn/if_rtwnreg.h>
#include <dev/rtwn/if_rtwnvar.h>
#include <dev/rtwn/if_rtwn_debug.h>
#include <dev/rtwn/if_rtwn_rx.h>
#include <dev/rtwn/if_rtwn_tx.h>

#include <dev/rtwn/rtl8192c/r92c_reg.h>

#include <dev/rtwn/sdio/rtwn_sdio_reg.h>
#include <dev/rtwn/sdio/rtwn_sdio_var.h>


/* Stop reading after this many bytes to let other work in. */
#define RTWN_SDIO_RX_BUDGET	(64 * 1024)
/* How often rtwn_sdio_poll_thread() looks for a receive request. */
#define RTWN_SDIO_POLL_INTERVAL	5000


void
rtwn_sdio_enable_intr(struct rtwn_sdio_softc *uc, bool enable)
{
	uc->sc_intr_enabled = enable;
	rtwn_sdio_local_write_4(uc, RTWN_SDIO_HIMR, enable ? uc->sc_himr : 0);
}


/*!	Reads \a size bytes of the pending receive request from the FIFO.

	The whole request goes in one CMD53, as in the vendor driver. A second
	CMD53 for the rest of a request reads nothing, whatever address it uses,
	and leaves the receive FIFO stuck until the chip is reset.
*/
static int
rtwn_sdio_read_port(struct rtwn_sdio_softc *uc, size_t size)
{
	int error;

	error = sdio_read_bytes(uc->sc_func, RTWN_SDIO_RX0FF(uc->sc_rx_id),
	    uc->sc_rx_buf, size, true);
	uc->sc_rx_id++;

	return (error);
}


/*!	Reads the request announced as \a len bytes, of which \a size bytes are
	transferred, and says why a read failed.
*/
static int
rtwn_sdio_rx_read(struct rtwn_sdio_softc *uc, uint32_t len, size_t size)
{
	int error;

	error = rtwn_sdio_read_port(uc, size);
	if (error != 0)
		rtwn_sdio_io_fault(uc);
	if (error != 0
	    && uc->sc_io_errors_logged++ < RTWN_SDIO_MAX_IO_LOGS) {
		device_printf(uc->sc_sc.sc_dev, "reading %u bytes failed: %d, "
		    "RX0_REQ_LEN is %u\n", len, error,
		    rtwn_sdio_local_read_2(uc, RTWN_SDIO_RX0_REQ_LEN));
	}

	return (error);
}


static struct mbuf *
rtwn_sdio_rx_copy(struct rtwn_softc *sc, struct rtwn_rx_stat_common *stat,
    int totlen)
{
	struct ieee80211com *ic = &sc->sc_ic;
	uint32_t rxdw0 = le32toh(stat->rxdw0);
	struct mbuf *m;

	if (!(sc->sc_flags & RTWN_RUNNING))
		return (NULL);

	if (rxdw0 & (RTWN_RXDW0_CRCERR | RTWN_RXDW0_ICVERR)
	    || MS(rxdw0, RTWN_RXDW0_PKTLEN)
		< sizeof(struct ieee80211_frame_ack)) {
		counter_u64_add(ic->ic_ierrors, 1);
		return (NULL);
	}

	m = m_get3(totlen, M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL) {
		counter_u64_add(ic->ic_ierrors, 1);
		return (NULL);
	}

	memcpy(mtod(m, uint8_t *), stat, totlen);
	m->m_pkthdr.len = m->m_len = totlen;

	if (rtwn_check_frame(sc, m) != 0) {
		m_freem(m);
		counter_u64_add(ic->ic_ierrors, 1);
		return (NULL);
	}

	return (m);
}


/*!	Splits the buffer into frames. Reports of the firmware are handled
	here, received frames are returned as a list.
*/
static struct mbuf *
rtwn_sdio_rx_parse(struct rtwn_softc *sc, uint8_t *buf, int len)
{
	struct mbuf *m0 = NULL, *last = NULL;

	while (len >= (int)sizeof(struct rtwn_rx_stat_common)) {
		struct rtwn_rx_stat_common *stat
		    = (struct rtwn_rx_stat_common *)buf;
		uint32_t rxdw0 = le32toh(stat->rxdw0);
		int pktlen = MS(rxdw0, RTWN_RXDW0_PKTLEN);
		int infosz = MS(rxdw0, RTWN_RXDW0_INFOSZ) * 8;
		int shift = MS(rxdw0, RTWN_RXDW0_SHIFT);
		int totlen = sizeof(*stat) + infosz + shift + pktlen;
		struct mbuf *m;

		if (pktlen == 0 || totlen > len)
			break;

		switch (rtwn_classify_intr(sc, buf, totlen)) {
		case RTWN_RX_DATA:
			m = rtwn_sdio_rx_copy(sc, stat, totlen);
			sc->sc_rx_transfers++;
			if (m != NULL) {
				if (last == NULL)
					m0 = m;
				else
					last->m_nextpkt = m;
				last = m;
			}
			break;
		case RTWN_RX_TX_REPORT:
			if (sc->sc_ratectl == RTWN_RATECTL_NET80211) {
				RTWN_NT_LOCK(sc);
				rtwn_handle_tx_report(sc, buf, totlen);
				RTWN_NT_UNLOCK(sc);
			}
			break;
		case RTWN_RX_TX_REPORT2:
			if (sc->sc_ratectl == RTWN_RATECTL_NET80211) {
				RTWN_NT_LOCK(sc);
				rtwn_handle_tx_report2(sc, buf, totlen);
				RTWN_NT_UNLOCK(sc);
			}
			break;
		default:
			rtwn_handle_c2h_report(sc, buf, totlen);
			break;
		}

		totlen = roundup2(totlen, RTWN_SDIO_DATA_ALIGN);
		buf += totlen;
		len -= totlen;
	}

	return (m0);
}


static void
rtwn_sdio_rx_input(struct rtwn_softc *sc, struct mbuf *m)
{
	struct ieee80211com *ic = &sc->sc_ic;
	struct mbuf *m0 = m, *next;

	/* rtwn_rx_common() wants the PCIe layout: descriptor apart. */
	for (; m != NULL; m = m->m_nextpkt) {
		struct rtwn_rx_stat_common stat;

		m_copydata(m, 0, sizeof(stat), (caddr_t)&stat);
		m_adj(m, sizeof(stat));
		m->m_pkthdr.PH_loc.ptr = rtwn_rx_common(sc, m, &stat);
	}

	RTWN_UNLOCK(sc);
	for (m = m0; m != NULL; m = next) {
		struct ieee80211_node *ni = m->m_pkthdr.PH_loc.ptr;

		next = m->m_nextpkt;
		m->m_nextpkt = NULL;
		m->m_pkthdr.PH_loc.ptr = NULL;
		if (ni != NULL) {
			(void)ieee80211_input_mimo(ni, m);
			ieee80211_free_node(ni);
		} else
			(void)ieee80211_input_mimo_all(ic, m);
	}
	RTWN_LOCK(sc);
}


static void
rtwn_sdio_rx(struct rtwn_sdio_softc *uc)
{
	struct rtwn_softc *sc = &uc->sc_sc;
	uint32_t total = 0, hisr;

	do {
		uint32_t len = rtwn_sdio_local_read_2(uc, RTWN_SDIO_RX0_REQ_LEN);
		size_t size;
		struct mbuf *m;

		if (len == 0 || len == 0xffff)
			break;

		size = rtwn_sdio_align_size(len);
		if (size > RTWN_SDIO_RXBUFSZ) {
			/*
			 * Cannot happen with a receive FIFO that is smaller than
			 * the buffer (checked in the chip's attach). Part of a
			 * request cannot be read either (see
			 * rtwn_sdio_read_port()): leave it, the watchdog restarts
			 * a chip that stops receiving.
			 */
			device_printf(sc->sc_dev, "%s: %u bytes do not fit\n",
			    __func__, len);
			counter_u64_add(sc->sc_ic.ic_ierrors, 1);
			break;
		}
		if (rtwn_sdio_rx_read(uc, len, size) != 0) {
			counter_u64_add(sc->sc_ic.ic_ierrors, 1);
			break;
		}
		total += len;

		m = rtwn_sdio_rx_parse(sc, uc->sc_rx_buf, MIN(len, size));
		if (m != NULL) {
			rtwn_sdio_rx_input(sc, m);
			/* The lock was dropped: the device may have stopped. */
			if (!uc->sc_intr_enabled
			    || !(sc->sc_flags & RTWN_RUNNING))
				break;
		}

		/*
		 * The 8051 chips can show a length while the FIFO is still
		 * being filled; only go on while a request is pending.
		 */
		hisr = rtwn_sdio_local_read_4(uc, RTWN_SDIO_HISR);
	} while (total < RTWN_SDIO_RX_BUDGET && hisr != 0xffffffff
	    && (hisr & RTWN_SDIO_INT_RX_REQUEST) != 0);
}


/*!	Does the work of a chip interrupt: reads REG_SDIO_HISR, handles a
	transmit error, reads the frames of a receive request and acknowledges
	the interrupt. \a kickTx restarts the transmit queue; the poll thread
	passes false, because rtwn_start() can wait 20 ms for free pages and
	that would keep the only thread that reads the receive FIFO away from
	it.
*/
static void
rtwn_sdio_handle(struct rtwn_sdio_softc *uc, bool kickTx)
{
	struct rtwn_softc *sc = &uc->sc_sc;
	uint32_t hisr;

	RTWN_LOCK(sc);
	if (!uc->sc_intr_enabled) {
		RTWN_UNLOCK(sc);
		return;
	}

	hisr = rtwn_sdio_local_read_4(uc, RTWN_SDIO_HISR);
	if (hisr == 0xffffffff) {
		RTWN_UNLOCK(sc);
		return;
	}

	if (hisr & RTWN_SDIO_INT_TXERR) {
		/* The cause is in REG_TXDMA_STATUS; write 1 to clear. */
		uint32_t status = rtwn_read_4(sc, R92C_TXDMA_STATUS);
		rtwn_write_4(sc, R92C_TXDMA_STATUS, status);
		device_printf(sc->sc_dev, "transmit error (HISR %#x, "
		    "TXDMA_STATUS %#x)\n", hisr, status);
	}
	if (hisr & RTWN_SDIO_INT_RX_REQUEST)
		rtwn_sdio_rx(uc);

	/* Only defined bits: others keep the interrupt asserted. */
	hisr &= RTWN_SDIO_HISR_CLEAR_MASK;
	if (hisr != 0)
		rtwn_sdio_local_write_4(uc, RTWN_SDIO_HISR, hisr);

	if (kickTx)
		rtwn_start(sc);
	RTWN_UNLOCK(sc);
}


/*!	Entry point for the card interrupt; see rtwn_sdio_handle(). */
void
rtwn_sdio_service(void *arg)
{
	rtwn_sdio_handle(arg, true);
}


/*!	Polls the chip for work. The card interrupt is off (see
	rtwn_sdio_attach()), so this thread does what it would have done.
*/
static status_t
rtwn_sdio_poll_thread(void *arg)
{
	struct rtwn_sdio_softc *uc = arg;

	while (!uc->sc_poll_stop) {
		snooze(RTWN_SDIO_POLL_INTERVAL);
		rtwn_sdio_handle(uc, false);
	}

	return (B_OK);
}


void
rtwn_sdio_poll_start(struct rtwn_sdio_softc *uc)
{
	uc->sc_poll_stop = false;
	uc->sc_poll_thread = spawn_kernel_thread(rtwn_sdio_poll_thread,
	    "rtwn sdio poll", B_LOW_PRIORITY, uc);
	if (uc->sc_poll_thread >= 0)
		resume_thread(uc->sc_poll_thread);
}


void
rtwn_sdio_poll_stop(struct rtwn_sdio_softc *uc)
{
	status_t result;

	if (uc->sc_poll_thread < 0)
		return;
	uc->sc_poll_stop = true;
	wait_for_thread(uc->sc_poll_thread, &result);
	uc->sc_poll_thread = -1;
}
