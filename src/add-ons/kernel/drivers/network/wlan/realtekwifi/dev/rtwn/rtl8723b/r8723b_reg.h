/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */
#ifndef R8723B_REG_H
#define R8723B_REG_H

#include <dev/rtwn/rtl8192e/r92e_reg.h>

/*
 * MAC registers of the RTL8723B that the other rtwn chips do not use, with
 * the offsets and bits of the rtw88 driver (reg.h, rtw8723x.h).
 */
#define R8723B_AFE_CTRL1		0x024
#define R8723B_AFE_CTRL3		0x02c
#define R8723B_PWR_DATA			0x038
#define R8723B_GPIO_INTM		0x048
#define R8723B_GPIO_PIN_CTRL_2		0x060
#define R8723B_PBP			0x104
#define R8723B_INT_MIG			0x304
#define R8723B_RXDMA_AGG_PG_TH		0x280
#define R8723B_RXDMA_MODE		0x290
#define R8723B_EARLY_MODE_CONTROL	0x2bc
#define R8723B_SINGLE_AMPDU_CTRL	0x4c7
#define R8723B_PKT_VO_VI_LIFE_TIME	0x4c0
#define R8723B_PKT_BE_BK_LIFE_TIME	0x4c2
#define R8723B_MAX_AGGR_NUM		0x4ca
#define R8723B_MACID_PKT_DROP0		0x4d0
#define R8723B_MACID_PKT_SLEEP		0x4d4
#define R8723B_PIFS			0x512
#define R8723B_SLOT			0x51b
#define R8723B_ATIMWND			0x55a
#define R8723B_MISC_CTRL		0x577
#define R8723B_RX_PKT_LIMIT		0x60c
#define R8723B_RESP_SIFS_CCK		0x63c
#define R8723B_RESP_SIFS_OFDM		0x63e
#define R8723B_NAV_UPPER		0x652
#define R8723B_AMPDU_MAX_TIME		0x456
#define R8723B_MCUTST_1			0x1c0

/* Bits for R92C_SYS_FUNC_EN. */
#define R8723B_SYS_FUNC_EN_25_1		0x2000

/* Bits for R8723B_AFE_CTRL3. */
#define R8723B_AFE_CTRL3_XTAL_M		0x00fff000
#define R8723B_AFE_CTRL3_XTAL_S		12

/* Bits for R8723B_PWR_DATA. */
#define R8723B_PWR_DATA_EEPRPAD_RFE_CTRL_EN 0x0800

/* Bits for R8723B_RXDMA_AGG_PG_TH. */
#define R8723B_RXDMA_AGG_PRE_CALC	0x20000000

/* Bits for R8723B_RXDMA_MODE. */
#define R8723B_RXDMA_MODE_DMA		0x02
#define R8723B_RXDMA_MODE_BURST_CNT_M	0x0c
#define R8723B_RXDMA_MODE_BURST_CNT_S	2
#define R8723B_RXDMA_MODE_BURST_SIZE_M	0x30
#define R8723B_RXDMA_MODE_BURST_SIZE_S	4

/*
 * Baseband registers.
 */
#define R8723B_BB_SEL_BTG		0x948
#define R8723B_OFDM0_XAAGC1		0xc50
#define R8723B_2ND_CCA_CTRL		0x976
#define R8723B_CTX			0xd03
#define R8723B_CTX_TYPE_M		0x70

/*
 * RF registers.
 */
#define R8723B_RF_WLINT			0x01
#define R8723B_RF_SYN_PFD		0xb0

#endif	/* R8723B_REG_H */
