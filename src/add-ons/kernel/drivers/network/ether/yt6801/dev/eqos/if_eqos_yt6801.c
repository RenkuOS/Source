/*
 * Copyright 2026, The RenkuOS authors.
 * SPDX-License-Identifier: MIT
 */

/*
 * PCI front end for the Motorcomm YT6801: brings the chip out of reset,
 * finds the station address and hands a filled softc to the eqos core.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/rman.h>
#include <sys/socket.h>
#include <sys/mutex.h>
#include <sys/callout.h>
#include <machine/bus.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_media.h>

#include <dev/mii/mii.h>
#include <dev/mii/miivar.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include "miibus_if.h"

#include <dev/eqos/if_eqos_var.h>
#include <dev/eqos/yt6801_mgmt.h>

#define	YT_RD(sc, o)		bus_read_4((sc)->res[EQOS_RES_MEM], (o))
#define	YT_WR(sc, o, v)		bus_write_4((sc)->res[EQOS_RES_MEM], (o), (v))

#define	YT_BAR_RID		PCIR_BAR(0)
#define	YT_MSI_RID		1

/* Settle times; see AUTHOR-NOTES.md (O2, O3, O5) */
#define	YT_RESET_SETTLE_US	10000
#define	YT_PHY_SETTLE_US	5000
#define	YT_FUSE_POLL_US		20
#define	YT_FUSE_POLL_TRIES	1000

static int
yt6801_probe(device_t dev)
{

	if (pci_get_vendor(dev) != YTM_PCI_VENDOR ||
	    pci_get_device(dev) != YTM_PCI_DEVICE)
		return (ENXIO);
	device_set_desc(dev, "Motorcomm YT6801 Gigabit Ethernet");
	return (BUS_PROBE_DEFAULT);
}

static void
yt6801_aspm_off(device_t dev)
{
	int cap;
	uint16_t before, after;

	if (pci_find_cap(dev, PCIY_EXPRESS, &cap) != 0) {
		device_printf(dev, "no PCIe capability, ASPM left alone\n");
		return;
	}
	before = pci_read_config(dev, cap + PCIER_LINK_CTL, 2);
	if ((before & PCIEM_LINK_CTL_ASPMC) != 0)
		pci_write_config(dev, cap + PCIER_LINK_CTL,
		    before & ~PCIEM_LINK_CTL_ASPMC, 2);
	after = pci_read_config(dev, cap + PCIER_LINK_CTL, 2);
	device_printf(dev, "PCIe link control 0x%04x -> 0x%04x (ASPM off)\n",
	    before, after);
}

static void
yt6801_busmaster_off(device_t dev)
{
	uint16_t cmd;

	cmd = pci_read_config(dev, PCIR_COMMAND, 2);
	pci_write_config(dev, PCIR_COMMAND, cmd & ~PCIM_CMD_BUSMASTEREN, 2);
}

/*
 * The reset bit reads back as set afterwards, so completion cannot be
 * polled: drive a clean 0 -> 1 edge and wait a fixed time.
 */
static void
yt6801_chip_reset(struct eqos_softc *sc)
{
	uint32_t v;

	v = YT_RD(sc, YTM_CHIP_RESET);
	YT_WR(sc, YTM_CHIP_RESET, v & ~YTM_CHIP_RESET_GO);
	YT_WR(sc, YTM_CHIP_RESET, v | YTM_CHIP_RESET_GO);
	DELAY(YT_RESET_SETTLE_US);
}

/* The chip reset puts the PHY and MDIO back into reset. */
static void
yt6801_phy_run(struct eqos_softc *sc)
{

	YT_WR(sc, YTM_PHY_CTL, YT_RD(sc, YTM_PHY_CTL) | YTM_PHY_CTL_RUN);
	DELAY(YT_PHY_SETTLE_US);
}

static int
yt6801_fuse_byte(struct eqos_softc *sc, u_int addr, uint8_t *out)
{
	uint32_t st;
	int i;

	YT_WR(sc, YTM_FUSE_CMD, (addr << YTM_FUSE_CMD_ADDR_SHIFT) |
	    YTM_FUSE_CMD_GO | YTM_FUSE_CMD_READ);
	for (i = 0; i < YT_FUSE_POLL_TRIES; i++) {
		DELAY(YT_FUSE_POLL_US);
		st = YT_RD(sc, YTM_FUSE_STAT);
		if ((st & YTM_FUSE_STAT_DONE) != 0) {
			*out = st >> YTM_FUSE_STAT_BYTE_SHIFT;
			return (0);
		}
	}
	return (ETIMEDOUT);
}

/*
 * Walk the eFuse preset records for the two MAC-address register
 * values. Returns NULL on success, else a reason for the log.
 */
static const char *
yt6801_fuse_macaddr(struct eqos_softc *sc)
{
	uint8_t b[YTM_FUSE_REC_SIZE];
	uint32_t lo = 0, hi = 0, val;
	uint16_t target;
	bool have_lo = false, have_hi = false;
	u_int rec, i, base;

	for (rec = 0; rec < YTM_FUSE_REC_MAX; rec++) {
		base = YTM_FUSE_REC_FIRST + rec * YTM_FUSE_REC_SIZE;
		for (i = 0; i < YTM_FUSE_REC_SIZE; i++) {
			if (yt6801_fuse_byte(sc, base + i, &b[i]) != 0)
				return ("eFuse read timeout");
		}
		target = b[0] | (b[1] << 8);
		if (target == 0)
			break;
		val = b[2] | (b[3] << 8) | (b[4] << 16) | ((uint32_t)b[5] << 24);
		if (target == YTM_MACADDR_LO) {
			lo = val;
			have_lo = true;
		} else if (target == YTM_MACADDR_HI) {
			hi = val;
			have_hi = true;
		}
	}
	if (!have_lo || !have_hi)
		return ("no address records in eFuse");

	sc->eaddr[0] = hi >> 8;
	sc->eaddr[1] = hi;
	sc->eaddr[2] = lo >> 24;
	sc->eaddr[3] = lo >> 16;
	sc->eaddr[4] = lo >> 8;
	sc->eaddr[5] = lo;

	if ((sc->eaddr[0] & 0x01) != 0)
		return ("eFuse address is multicast");
	for (i = 0; i < ETHER_ADDR_LEN; i++) {
		if (sc->eaddr[i] != 0)
			return (NULL);
	}
	return ("eFuse address is zero");
}

static void
yt6801_set_macaddr(struct eqos_softc *sc)
{
	const char *why;

	why = yt6801_fuse_macaddr(sc);
	if (why == NULL) {
		device_printf(sc->dev, "MAC address from eFuse\n");
		return;
	}
	arc4rand(sc->eaddr, ETHER_ADDR_LEN, 0);
	sc->eaddr[0] = (sc->eaddr[0] & ~0x01) | 0x02;
	device_printf(sc->dev, "%s, using random MAC address\n", why);
}

static void
yt6801_irq_setup(struct eqos_softc *sc)
{

	/* With wake logic on, DMA interrupts never reach the host */
	YT_WR(sc, YTM_WAKE_CTL, YT_RD(sc, YTM_WAKE_CTL) | YTM_WAKE_CTL_OFF);
	YT_WR(sc, YTM_IRQ_PACE,
	    ((YTM_IRQ_PACE_USEC << YTM_IRQ_PACE_TX_SHIFT) &
	    YTM_IRQ_PACE_TX_MASK) | (YTM_IRQ_PACE_USEC & YTM_IRQ_PACE_RX_MASK));
	YT_WR(sc, YTM_IRQ_CTL, YTM_IRQ_CTL_ALL_OPEN);
}

static int
yt6801_attach(device_t dev)
{
	struct eqos_softc *sc = device_get_softc(dev);
	bus_size_t barsize;
	int error, rid, nmsi;

	sc->dev = dev;
	pci_enable_busmaster(dev);

	rid = YT_BAR_RID;
	sc->res[EQOS_RES_MEM] = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
	    &rid, RF_ACTIVE);
	if (sc->res[EQOS_RES_MEM] == NULL) {
		device_printf(dev, "cannot map BAR0\n");
		error = ENXIO;
		goto fail_busmaster;
	}
	barsize = rman_get_size(sc->res[EQOS_RES_MEM]);
	device_printf(dev, "BAR0 size 0x%lx, PCI revision 0x%02x\n",
	    (u_long)barsize, pci_get_revid(dev));
	if (barsize < YTM_BAR_MIN_SIZE) {
		device_printf(dev, "BAR0 too small\n");
		error = ENXIO;
		goto fail_bar;
	}

	yt6801_aspm_off(dev);

	/* The chip has no INTx */
	if (pci_msi_count(dev) < 1) {
		device_printf(dev, "MSI not available\n");
		error = ENXIO;
		goto fail_bar;
	}
	nmsi = 1;
	if (pci_alloc_msi(dev, &nmsi) != 0) {
		device_printf(dev, "cannot allocate MSI\n");
		error = ENXIO;
		goto fail_bar;
	}
	rid = YT_MSI_RID;
	sc->res[EQOS_RES_IRQ0] = bus_alloc_resource_any(dev, SYS_RES_IRQ,
	    &rid, RF_ACTIVE);
	if (sc->res[EQOS_RES_IRQ0] == NULL) {
		device_printf(dev, "cannot allocate MSI resource\n");
		error = ENXIO;
		goto fail_msi;
	}

	yt6801_chip_reset(sc);
	YT_WR(sc, YTM_IRQ_CTL, YTM_IRQ_CTL_ALL_MASKED);
	/* Before eqos_attach(): the DMA soft reset may need PHY clocks */
	yt6801_phy_run(sc);
	yt6801_set_macaddr(sc);
	yt6801_irq_setup(sc);

	sc->mac_base = YTM_MAC_BASE;
	sc->phy_addr = 0;
	sc->csr_clock = 25000000;
	sc->csr_clock_range = GMAC_MAC_MDIO_ADDRESS_CR_20_35;
	sc->pblx8 = true;
	sc->txpbl = 32;
	sc->rxpbl = 32;
	sc->thresh_dma_mode = false;
	sc->ttc = 0x10;
	sc->rtc = 0;

	device_printf(dev, "PHY ID at MDIO 0: 0x%04x 0x%04x\n",
	    eqos_miibus_readreg(dev, 0, MII_PHYIDR1),
	    eqos_miibus_readreg(dev, 0, MII_PHYIDR2));

	error = eqos_attach(dev);
	if (error != 0) {
		eqos_detach(dev);
		goto fail_irq;
	}
	return (0);

fail_irq:
	YT_WR(sc, YTM_IRQ_CTL, YTM_IRQ_CTL_ALL_MASKED);
	bus_release_resource(dev, SYS_RES_IRQ, YT_MSI_RID,
	    sc->res[EQOS_RES_IRQ0]);
	sc->res[EQOS_RES_IRQ0] = NULL;
fail_msi:
	pci_release_msi(dev);
fail_bar:
	bus_release_resource(dev, SYS_RES_MEMORY, YT_BAR_RID,
	    sc->res[EQOS_RES_MEM]);
	sc->res[EQOS_RES_MEM] = NULL;
fail_busmaster:
	yt6801_busmaster_off(dev);
	return (error);
}

static int
yt6801_detach(device_t dev)
{
	struct eqos_softc *sc = device_get_softc(dev);

	eqos_detach(dev);

	/* Leave the chip silent for whatever runs next */
	YT_WR(sc, YTM_IRQ_CTL, YTM_IRQ_CTL_ALL_MASKED);
	yt6801_busmaster_off(dev);

	bus_release_resource(dev, SYS_RES_IRQ, YT_MSI_RID,
	    sc->res[EQOS_RES_IRQ0]);
	sc->res[EQOS_RES_IRQ0] = NULL;
	pci_release_msi(dev);
	bus_release_resource(dev, SYS_RES_MEMORY, YT_BAR_RID,
	    sc->res[EQOS_RES_MEM]);
	sc->res[EQOS_RES_MEM] = NULL;
	return (0);
}

static device_method_t yt6801_methods[] = {
	DEVMETHOD(device_probe,		yt6801_probe),
	DEVMETHOD(device_attach,	yt6801_attach),
	DEVMETHOD(device_detach,	yt6801_detach),

	DEVMETHOD(miibus_readreg,	eqos_miibus_readreg),
	DEVMETHOD(miibus_writereg,	eqos_miibus_writereg),
	DEVMETHOD(miibus_statchg,	eqos_miibus_statchg),

	DEVMETHOD_END
};

/* The core and miibus look the driver up by the name "eqos" */
static driver_t yt6801_driver = {
	"eqos",
	yt6801_methods,
	sizeof(struct eqos_softc)
};

DRIVER_MODULE(eqos, pci, yt6801_driver, 0, 0);
DRIVER_MODULE(miibus, eqos, miibus_driver, 0, 0);
