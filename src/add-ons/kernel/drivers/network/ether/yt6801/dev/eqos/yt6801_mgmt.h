/*
 * Copyright 2026, The RenkuOS authors.
 * SPDX-License-Identifier: MIT
 */

/*
 * Motorcomm YT6801 management block. Offsets are absolute in BAR0; the
 * EQoS core registers start at YTM_MAC_BASE.
 */

#ifndef _YT6801_MGMT_H
#define	_YT6801_MGMT_H

#define	YTM_PCI_VENDOR		0x1f0a
#define	YTM_PCI_DEVICE		0x6801

#define	YTM_BAR_MIN_SIZE	0x4000
#define	YTM_MAC_BASE		0x2000

#define	YTM_PHY_CTL		0x1004
#define	 YTM_PHY_CTL_RUN	(1U << 0)	/* 0 = PHY + MDIO held in reset */

#define	YTM_WAKE_CTL		0x1010
#define	 YTM_WAKE_CTL_OFF	(1U << 0)

#define	YTM_IRQ_CTL		0x1100
#define	 YTM_IRQ_CTL_ALL_OPEN	0x00000000
#define	 YTM_IRQ_CTL_ALL_MASKED	0xffff0000

#define	YTM_IRQ_PACE		0x1108
#define	 YTM_IRQ_PACE_RX_MASK	0x00000fff
#define	 YTM_IRQ_PACE_TX_SHIFT	16
#define	 YTM_IRQ_PACE_TX_MASK	0x0fff0000
#define	YTM_IRQ_PACE_USEC	100

#define	YTM_FUSE_CMD		0x1500
#define	 YTM_FUSE_CMD_READ	0x1
#define	 YTM_FUSE_CMD_GO	(1U << 2)
#define	 YTM_FUSE_CMD_ADDR_SHIFT	8
#define	YTM_FUSE_STAT		0x1504
#define	 YTM_FUSE_STAT_DONE	(1U << 1)
#define	 YTM_FUSE_STAT_BYTE_SHIFT	24

#define	YTM_MACADDR_LO		0x1520
#define	YTM_MACADDR_HI		0x1524

#define	YTM_CHIP_RESET		0x152c
#define	 YTM_CHIP_RESET_GO	(1U << 31)

/* Preset records in the eFuse: { le16 target, le32 value } */
#define	YTM_FUSE_REC_FIRST	18
#define	YTM_FUSE_REC_SIZE	6
#define	YTM_FUSE_REC_MAX	39

#endif
