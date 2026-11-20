/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


/*!	SDIO bus glue of rtwn, for the RTL8723BS.

	The structure follows the USB glue of rtwn (rtwn_usb_attach.c). The SDIO
	specifics, the register windows, the transmit FIFOs and the receive FIFO,
	are described by the rtw88 driver (sdio.c and sdio.h, GPL-2.0 OR
	BSD-3-Clause).
*/


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/mbuf.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
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
#include <dev/rtwn/if_rtwn_nop.h>

#include <dev/rtwn/sdio/rtwn_sdio_reg.h>
#include <dev/rtwn/sdio/rtwn_sdio_var.h>

#include <dev/rtwn/rtl8192c/r92c_reg.h>


#define RTWN_SDIO_VENDOR_REALTEK	0x024c
#define RTWN_SDIO_DEVICE_RTL8723BS	0xb723


static device_probe_t	rtwn_sdio_probe;
static device_attach_t	rtwn_sdio_attach;
static device_detach_t	rtwn_sdio_detach;


static void
rtwn_sdio_start_xfers(struct rtwn_softc *sc)
{
	struct rtwn_sdio_softc *uc = RTWN_SDIO_SOFTC(sc);
	uint32_t hisr;

	/* Acknowledge what came in while the interrupt was off. */
	hisr = rtwn_sdio_local_read_4(uc, RTWN_SDIO_HISR);
	if (hisr != 0xffffffff && (hisr & RTWN_SDIO_HISR_CLEAR_MASK) != 0) {
		rtwn_sdio_local_write_4(uc, RTWN_SDIO_HISR,
		    hisr & RTWN_SDIO_HISR_CLEAR_MASK);
	}

	rtwn_sdio_enable_intr(uc, true);
}


static void
rtwn_sdio_abort_xfers(struct rtwn_softc *sc)
{
	rtwn_sdio_enable_intr(RTWN_SDIO_SOFTC(sc), false);
}


static void
rtwn_sdio_reset_lists(struct rtwn_softc *sc, struct ieee80211vap *vap)
{
	struct rtwn_sdio_softc *uc = RTWN_SDIO_SOFTC(sc);
	struct mbufq keep;
	struct mbuf *m;

	RTWN_ASSERT_LOCKED(sc);

	mbufq_init(&keep, IFQ_MAXLEN);
	while ((m = mbufq_dequeue(&uc->sc_tx_done)) != NULL) {
		struct ieee80211_node *ni = m->m_pkthdr.PH_loc.ptr;

		if (vap != NULL && ni->ni_vap != vap) {
			mbufq_enqueue(&keep, m);
			continue;
		}
		m->m_pkthdr.PH_loc.ptr = NULL;
		ieee80211_free_node(ni);
		m_freem(m);
	}
	while ((m = mbufq_dequeue(&keep)) != NULL)
		mbufq_enqueue(&uc->sc_tx_done, m);
}


/*!	Queue to FIFO mapping of the RQPN table of rtw88 for SDIO: VO, MGT and
	HI to the high queue, VI to the normal one, BE and BK to the low one.
*/
static uint16_t
rtwn_sdio_get_qmap(struct rtwn_softc *sc)
{
	return (R92C_TRXDMA_CTRL_QMAP_3EP);
}


static void
rtwn_sdio_attach_methods(struct rtwn_softc *sc)
{
	sc->sc_write_1		= rtwn_sdio_write_1;
	sc->sc_write_2		= rtwn_sdio_write_2;
	sc->sc_write_4		= rtwn_sdio_write_4;
	sc->sc_read_1		= rtwn_sdio_read_1;
	sc->sc_read_2		= rtwn_sdio_read_2;
	sc->sc_read_4		= rtwn_sdio_read_4;
	sc->sc_delay		= rtwn_sdio_delay;
	sc->sc_tx_start		= rtwn_sdio_tx_start;
	sc->sc_start_xfers	= rtwn_sdio_start_xfers;
	sc->sc_reset_lists	= rtwn_sdio_reset_lists;
	sc->sc_abort_xfers	= rtwn_sdio_abort_xfers;
	sc->sc_fw_write_block	= rtwn_sdio_fw_write_block;
	sc->sc_get_qmap		= rtwn_sdio_get_qmap;
	sc->sc_set_desc_addr	= rtwn_nop_softc;
	sc->sc_drop_incorrect_tx = rtwn_nop_softc;
	sc->sc_beacon_update_begin = rtwn_nop_softc_vap;
	sc->sc_beacon_update_end = rtwn_nop_softc_vap;
	sc->sc_beacon_unload	= rtwn_nop_softc_int;

	sc->bcn_check_interval	= 100;
}


static int
rtwn_sdio_probe(device_t dev)
{
	struct sdio_attach_args *args = device_get_ivars(dev);

	if (args == NULL || args->vendor != RTWN_SDIO_VENDOR_REALTEK
	    || args->device != RTWN_SDIO_DEVICE_RTL8723BS
	    || args->function != 1)
		return (ENXIO);

	device_set_desc(dev, "Realtek RTL8723BS");
	return (BUS_PROBE_DEFAULT);
}


static int
rtwn_sdio_attach(device_t dev)
{
	struct rtwn_sdio_softc *uc = device_get_softc(dev);
	struct rtwn_softc *sc = &uc->sc_sc;
	struct ieee80211com *ic = &sc->sc_ic;
	int error;

	sc->sc_dev = dev;
	ic->ic_name = device_get_nameunit(dev);
	uc->sc_func = sdio_get_function(dev);
	uc->sc_poll_thread = -1;

	/* In the compat layer, malloc() can return NULL even with M_WAITOK. */
	uc->sc_rx_buf = malloc(RTWN_SDIO_RXBUFSZ, M_DEVBUF, M_WAITOK);
	uc->sc_tx_buf = malloc(RTWN_SDIO_TXBUFSZ, M_DEVBUF, M_WAITOK);
	if (uc->sc_rx_buf == NULL || uc->sc_tx_buf == NULL) {
		free(uc->sc_tx_buf, M_DEVBUF);
		free(uc->sc_rx_buf, M_DEVBUF);
		return (ENOMEM);
	}
	mbufq_init(&uc->sc_tx_done, IFQ_MAXLEN);
	TASK_INIT(&uc->sc_tx_task, 0, rtwn_sdio_tx_done_task, uc);

	rtwn_sysctlattach(sc);
	mtx_init(&sc->sc_mtx, ic->ic_name, MTX_NETWORK_LOCK, MTX_DEF);

	error = sdio_enable_func(uc->sc_func);
	if (error != 0) {
		device_printf(dev, "cannot enable SDIO function 1: %d\n",
		    error);
		goto fail;
	}
	error = sdio_set_block_size(uc->sc_func, RTWN_SDIO_BLOCK_SIZE);
	if (error != 0) {
		device_printf(dev, "cannot set the block size: %d\n", error);
		goto fail;
	}

	rtwn_sdio_attach_methods(sc);
	error = r8723bs_attach(uc);
	if (error != 0) {
		device_printf(dev, "cannot allocate the chip state\n");
		goto fail;
	}

	/*
	 * The efuse is only readable with the chip on; rtwn_attach() reads
	 * it, rtwn_init() switches the chip on again.
	 */
	RTWN_LOCK(sc);
	error = rtwn_power_on(sc);
	RTWN_UNLOCK(sc);
	if (error != 0) {
		device_printf(dev, "power on failed: %d\n", error);
		goto fail;
	}

	/*
	 * The card interrupt stays off: it uses DAT1 in 4 bit mode and can
	 * corrupt a block that is being received. The poll thread does its
	 * work instead; sdio_setup_intr() would turn the interrupt on.
	 */
	error = rtwn_attach(sc);

	/*
	 * The efuse is read: off again until the interface comes up, also
	 * when the attach failed.
	 */
	RTWN_LOCK(sc);
	rtwn_power_off(sc);
	RTWN_UNLOCK(sc);

	if (error != 0)
		goto fail;

	rtwn_sdio_poll_start(uc);
	return (0);

fail:
	if (sc->sc_detach_private != NULL)
		rtwn_detach_private(sc);
	sdio_disable_func(uc->sc_func);
	mtx_destroy(&sc->sc_mtx);
	free(uc->sc_tx_buf, M_DEVBUF);
	free(uc->sc_rx_buf, M_DEVBUF);
	return (ENXIO);
}


static int
rtwn_sdio_detach(device_t dev)
{
	struct rtwn_sdio_softc *uc = device_get_softc(dev);
	struct rtwn_softc *sc = &uc->sc_sc;

	rtwn_sdio_poll_stop(uc);
	rtwn_detach(sc);

	taskqueue_drain(taskqueue_thread, &uc->sc_tx_task);
	rtwn_sdio_tx_drain(uc, 1);

	rtwn_detach_private(sc);
	sdio_disable_func(uc->sc_func);
	mtx_destroy(&sc->sc_mtx);

	free(uc->sc_tx_buf, M_DEVBUF);
	free(uc->sc_rx_buf, M_DEVBUF);
	return (0);
}


static device_method_t rtwn_sdio_methods[] = {
	DEVMETHOD(device_probe,		rtwn_sdio_probe),
	DEVMETHOD(device_attach,	rtwn_sdio_attach),
	DEVMETHOD(device_detach,	rtwn_sdio_detach),

	DEVMETHOD_END
};

static driver_t rtwn_sdio_driver = {
	"rtwn",
	rtwn_sdio_methods,
	sizeof(struct rtwn_sdio_softc)
};

DRIVER_MODULE(rtwn_sdio, sdio, rtwn_sdio_driver, NULL, NULL);
