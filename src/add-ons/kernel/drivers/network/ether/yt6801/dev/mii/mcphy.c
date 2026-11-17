/*
 * Copyright 2026, The RenkuOS authors.
 * SPDX-License-Identifier: MIT
 */

/*
 * Motorcomm YT8531S (inside the YT6801) / YT8531 gigabit copper PHY.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/socket.h>
#include <sys/bus.h>

#include <net/if.h>
#include <net/if_media.h>

#include <dev/mii/mii.h>
#include <dev/mii/miivar.h>

#include "miibus_if.h"

/* IDR1 0x4f51, IDR2 0xe91a/0xe91b through mii_oui() / MII_MODEL() */
#define	MCPHY_OUI		0xc82b5e
#define	MCPHY_MODEL		0x11

#define	MCPHY_STAT		0x11
#define	 MCPHY_STAT_SPEED_MASK	0xc000
#define	 MCPHY_STAT_SPEED_10	0x0000
#define	 MCPHY_STAT_SPEED_100	0x4000
#define	 MCPHY_STAT_SPEED_1000	0x8000
#define	 MCPHY_STAT_FDX		0x2000
#define	MCPHY_IRQ_ENABLE	0x12
#define	MCPHY_IRQ_STATUS	0x13
#define	MCPHY_EXT_ADDR		0x1e
#define	MCPHY_EXT_DATA		0x1f

#define	MCPHY_X_SPACE		0xa000
#define	 MCPHY_X_SPACE_FIBER	0x0002
#define	MCPHY_X_AFE		0x0012
#define	 MCPHY_X_AFE_DAC_CLK_ON	0x2000

static int	mcphy_probe(device_t);
static int	mcphy_attach(device_t);
static int	mcphy_service(struct mii_softc *, struct mii_data *, int);
static void	mcphy_status(struct mii_softc *);
static void	mcphy_reset(struct mii_softc *);

static device_method_t mcphy_methods[] = {
	DEVMETHOD(device_probe,		mcphy_probe),
	DEVMETHOD(device_attach,	mcphy_attach),
	DEVMETHOD(device_detach,	mii_phy_detach),
	DEVMETHOD(device_shutdown,	bus_generic_shutdown),
	DEVMETHOD_END
};

static driver_t mcphy_driver = {
	"mcphy",
	mcphy_methods,
	sizeof(struct mii_softc)
};

DRIVER_MODULE(mcphy, miibus, mcphy_driver, 0, 0);

static const struct mii_phydesc mcphy_ids[] = {
	{ MCPHY_OUI, MCPHY_MODEL, "Motorcomm YT8531 gigabit PHY" },
	MII_PHY_END
};

static const struct mii_phy_funcs mcphy_funcs = {
	mcphy_service,
	mcphy_status,
	mcphy_reset
};

static int
mcphy_probe(device_t dev)
{

	return (mii_phy_dev_probe(dev, mcphy_ids, BUS_PROBE_DEFAULT));
}

static int
mcphy_attach(device_t dev)
{

	mii_phy_dev_attach(dev, MIIF_NOMANPAUSE, &mcphy_funcs, 1);
	return (0);
}

/* The address/data pair is not atomic; callers hold the MAC lock. */
static uint16_t
mcphy_ext_read(struct mii_softc *sc, uint16_t reg)
{

	PHY_WRITE(sc, MCPHY_EXT_ADDR, reg);
	return (PHY_READ(sc, MCPHY_EXT_DATA));
}

static void
mcphy_ext_write(struct mii_softc *sc, uint16_t reg, uint16_t val)
{

	PHY_WRITE(sc, MCPHY_EXT_ADDR, reg);
	PHY_WRITE(sc, MCPHY_EXT_DATA, val);
}

/* BMCR reset drops the extended settings, so restore them every time. */
static void
mcphy_reset(struct mii_softc *sc)
{

	mii_phy_reset(sc);

	/* The extended registers below are the copper ones only in UTP space */
	mcphy_ext_write(sc, MCPHY_X_SPACE,
	    mcphy_ext_read(sc, MCPHY_X_SPACE) & ~MCPHY_X_SPACE_FIBER);
	/* GMII link is unreliable unless the AFE DAC clock stays on */
	mcphy_ext_write(sc, MCPHY_X_AFE,
	    mcphy_ext_read(sc, MCPHY_X_AFE) | MCPHY_X_AFE_DAC_CLK_ON);

	/* Link is polled by mii_tick */
	PHY_WRITE(sc, MCPHY_IRQ_ENABLE, 0);
	(void)PHY_READ(sc, MCPHY_IRQ_STATUS);
}

static int
mcphy_service(struct mii_softc *sc, struct mii_data *mii, int cmd)
{

	switch (cmd) {
	case MII_POLLSTAT:
		break;
	case MII_MEDIACHG:
		mii_phy_setmedia(sc);
		break;
	case MII_TICK:
		if (mii_phy_tick(sc) == EJUSTRETURN)
			return (0);
		break;
	}

	PHY_STATUS(sc);
	mii_phy_update(sc, cmd);
	return (0);
}

static void
mcphy_status(struct mii_softc *sc)
{
	struct mii_data *mii = sc->mii_pdata;
	int bmsr, bmcr, stat;

	mii->mii_media_status = IFM_AVALID;
	mii->mii_media_active = IFM_ETHER;

	/* Link status latches low: the second read is the current state */
	(void)PHY_READ(sc, MII_BMSR);
	bmsr = PHY_READ(sc, MII_BMSR);
	if ((bmsr & BMSR_LINK) != 0)
		mii->mii_media_status |= IFM_ACTIVE;

	bmcr = PHY_READ(sc, MII_BMCR);
	if ((bmcr & BMCR_ISO) != 0) {
		mii->mii_media_active |= IFM_NONE;
		mii->mii_media_status = 0;
		return;
	}
	if ((bmcr & BMCR_LOOP) != 0)
		mii->mii_media_active |= IFM_LOOP;

	if ((bmcr & BMCR_AUTOEN) == 0) {
		mii->mii_media_active = mii->mii_media.ifm_cur->ifm_media;
		return;
	}

	/* Speed and duplex in MCPHY_STAT are only valid once resolved */
	if ((bmsr & (BMSR_ACOMP | BMSR_LINK)) != (BMSR_ACOMP | BMSR_LINK)) {
		mii->mii_media_active |= IFM_NONE;
		return;
	}

	stat = PHY_READ(sc, MCPHY_STAT);
	switch (stat & MCPHY_STAT_SPEED_MASK) {
	case MCPHY_STAT_SPEED_1000:
		mii->mii_media_active |= IFM_1000_T;
		break;
	case MCPHY_STAT_SPEED_100:
		mii->mii_media_active |= IFM_100_TX;
		break;
	case MCPHY_STAT_SPEED_10:
		mii->mii_media_active |= IFM_10_T;
		break;
	default:
		mii->mii_media_active |= IFM_NONE;
		return;
	}

	if ((stat & MCPHY_STAT_FDX) != 0)
		mii->mii_media_active |= IFM_FDX | mii_phy_flowstatus(sc);
	else
		mii->mii_media_active |= IFM_HDX;

	if (IFM_SUBTYPE(mii->mii_media_active) == IFM_1000_T &&
	    (PHY_READ(sc, MII_100T2SR) & GTSR_MS_RES) != 0)
		mii->mii_media_active |= IFM_ETH_MASTER;
}
