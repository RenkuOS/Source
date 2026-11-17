/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


/*!	Transmit path of rtwn over SDIO.

	A frame is written with one CMD53 to the transmit FIFO of its queue; the
	low bits of that address carry its length in dwords. The chip charges
	pages of the FIFO (or of the public pool) for it, so a frame waits until
	REG_SDIO_FREE_TXPG shows enough of them. The write is synchronous: the
	frame is done when the command completes, and is handed back to net80211
	from a task so that no callback runs under the driver lock.

	The queue to FIFO mapping follows the RQPN table of the rtw88 driver for
	SDIO: VO, management and high to the high FIFO, VI to the normal one, BE
	and BK to the low one. The queue is taken from the descriptor, not from
	the mbuf.
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
#include <sys/taskqueue.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/ethernet.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>

#include <dev/sdio/sdio_haiku.h>

#include <dev/rtwn/if_rtwnreg.h>
#include <dev/rtwn/if_rtwnvar.h>
#include <dev/rtwn/if_rtwn_debug.h>
#include <dev/rtwn/if_rtwn_tx.h>

#include <dev/rtwn/sdio/rtwn_sdio_reg.h>
#include <dev/rtwn/sdio/rtwn_sdio_var.h>


/* How long a frame waits for free pages before it is dropped. */
#define RTWN_SDIO_TX_WAIT_US	20000


/*!	Picks the FIFO from the queue select field of the descriptor, the way
	the chip maps it (R92C_TRXDMA_CTRL_QMAP_3EP). The access category of the
	mbuf can differ: net80211 sends EAPOL frames as VO, while their QoS
	header and so their queue select say TID 0. A frame written to a FIFO
	that does not match its queue select is refused (TXDMA_STATUS 0x400).
*/
static int
rtwn_sdio_tx_queue(const struct rtwn_tx_desc_common *txd)
{
	/* Bits 12:8 of the second dword, the same on all chips. */
	switch ((le32toh(txd->txdw1) >> 8) & 0x1f) {
	case 0x00:	/* BE */
	case 0x01:	/* BK */
	case 0x02:	/* BK */
	case 0x03:	/* BE */
		return (RTWN_SDIO_TXQ_LOW);
	case 0x04:	/* VI */
	case 0x05:	/* VI */
		return (RTWN_SDIO_TXQ_NORMAL);
	default:	/* VO, beacon, high, management, command */
		return (RTWN_SDIO_TXQ_HIGH);
	}
}


static int
rtwn_sdio_free_pages(uint32_t freePages, int queue)
{
	int pages = RTWN_SDIO_FREE_TXPG_PUB(freePages);

	switch (queue) {
	case RTWN_SDIO_TXQ_HIGH:
		return (pages + RTWN_SDIO_FREE_TXPG_HIGH(freePages));
	case RTWN_SDIO_TXQ_NORMAL:
		return (pages + RTWN_SDIO_FREE_TXPG_NORMAL(freePages));
	default:
		return (pages + RTWN_SDIO_FREE_TXPG_LOW(freePages));
	}
}


static int
rtwn_sdio_wait_pages(struct rtwn_sdio_softc *uc, int queue, int pages)
{
	uint32_t freePages = 0;
	int tries;

	for (tries = 0; tries < RTWN_SDIO_TX_WAIT_US / 100; tries++) {
		freePages = rtwn_sdio_local_read_4(uc, RTWN_SDIO_FREE_TXPG);
		if (freePages != 0xffffffff
		    && rtwn_sdio_free_pages(freePages, queue) >= pages
		    && rtwn_sdio_local_read_1(uc, RTWN_SDIO_OQT_FREE_PG) != 0)
			return (0);
		DELAY(100);
	}

	return (ENOBUFS);
}


/*!	Checksum of the descriptor as the RTL8723B family expects it: the XOR
	of its first 32 bytes as 16 bit words, not inverted (rtw88, rtw8723x.c;
	the same as rtwn_usb).
*/
static void
rtwn_sdio_tx_checksum(struct rtwn_tx_desc_common *txd)
{
	uint16_t sum = 0;
	int i;

	txd->txdw7.usb_checksum = 0;
	for (i = 0; i < 32 / 2; i++)
		sum ^= ((uint16_t *)txd)[i];
	txd->txdw7.usb_checksum = sum;
}


int
rtwn_sdio_tx_start(struct rtwn_softc *sc, struct ieee80211_node *ni,
    struct mbuf *m, uint8_t *tx_desc, uint8_t type, int id)
{
	struct rtwn_sdio_softc *uc = RTWN_SDIO_SOFTC(sc);
	struct rtwn_tx_desc_common *txd;
	size_t length, size;
	uint32_t addr;
	int queue, pages, error;

	RTWN_ASSERT_LOCKED(sc);

	length = sc->txdesc_len + m->m_pkthdr.len;
	size = rtwn_sdio_align_size(length);
	if (size > RTWN_SDIO_TXBUFSZ)
		return (EINVAL);

	queue = rtwn_sdio_tx_queue(
	    (const struct rtwn_tx_desc_common *)tx_desc);
	pages = howmany(roundup2(length, 4), sc->page_size);
	error = rtwn_sdio_wait_pages(uc, queue, pages);
	if (error != 0) {
		RTWN_DPRINTF(sc, RTWN_DEBUG_XMIT,
		    "%s: no free pages in queue %d\n", __func__, queue);
		return (error);
	}

	txd = (struct rtwn_tx_desc_common *)tx_desc;
	txd->pktlen = htole16(m->m_pkthdr.len);
	txd->offset = sc->txdesc_len;
	txd->flags0 |= RTWN_FLAGS0_OWN;
	rtwn_sdio_tx_checksum(txd);

	rtwn_dump_tx_desc(sc, tx_desc);

	memcpy(uc->sc_tx_buf, tx_desc, sc->txdesc_len);
	m_copydata(m, 0, m->m_pkthdr.len,
	    (caddr_t)(uc->sc_tx_buf + sc->txdesc_len));
	memset(uc->sc_tx_buf + length, 0, size - length);

	addr = RTWN_SDIO_ADDR(RTWN_SDIO_ADDR_TXFF_HIGH + queue,
	    howmany(length, 4));
	error = sdio_write_bytes(uc->sc_func, addr, uc->sc_tx_buf, size, true);
	if (error != 0) {
		device_printf(sc->sc_dev, "%s: writing %zu bytes to queue %d "
		    "failed: %d\n", __func__, size, queue, error);
		return (EIO);
	}

	/* Beacons (no node) stay owned by the caller. */
	if (ni != NULL) {
		m->m_pkthdr.PH_loc.ptr = ni;
		if (mbufq_enqueue(&uc->sc_tx_done, m) != 0)
			ieee80211_tx_complete(ni, m, 0);
		else
			taskqueue_enqueue(taskqueue_thread, &uc->sc_tx_task);
	}

	return (0);
}


/*!	Hands written frames back to net80211 with \a status. */
void
rtwn_sdio_tx_drain(struct rtwn_sdio_softc *uc, int status)
{
	struct rtwn_softc *sc = &uc->sc_sc;
	struct mbufq done;
	struct mbuf *m;

	RTWN_LOCK(sc);
	mbufq_init(&done, IFQ_MAXLEN);
	while ((m = mbufq_dequeue(&uc->sc_tx_done)) != NULL)
		mbufq_enqueue(&done, m);
	RTWN_UNLOCK(sc);

	while ((m = mbufq_dequeue(&done)) != NULL) {
		struct ieee80211_node *ni = m->m_pkthdr.PH_loc.ptr;

		m->m_pkthdr.PH_loc.ptr = NULL;
		ieee80211_tx_complete(ni, m, status);
	}
}


void
rtwn_sdio_tx_done_task(void *arg, int pending)
{
	struct rtwn_sdio_softc *uc = arg;
	struct rtwn_softc *sc = &uc->sc_sc;

	rtwn_sdio_tx_drain(uc, 0);

	RTWN_LOCK(sc);
	rtwn_start(sc);
	RTWN_UNLOCK(sc);
}
