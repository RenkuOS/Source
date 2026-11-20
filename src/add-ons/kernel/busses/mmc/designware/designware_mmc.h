/*
 * Copyright 2024-2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef DESIGNWARE_MMC_H
#define DESIGNWARE_MMC_H


#include <SupportDefs.h>


// Synopsys DesignWare Mobile Storage Host ("dw-mshc") registers, from
// version 2.40a on (data FIFO at 0x200). The internal DMA controller (IDMAC)
// registers move when the controller is built with 64 bit addresses
// (HCON.ADDR_CONFIG).

#define DW_MMC_CTRL				0x000
#define DW_MMC_PWREN			0x004
#define DW_MMC_CLKDIV			0x008
#define DW_MMC_CLKSRC			0x00c
#define DW_MMC_CLKENA			0x010
#define DW_MMC_TMOUT			0x014
#define DW_MMC_CTYPE			0x018
#define DW_MMC_BLKSIZ			0x01c
#define DW_MMC_BYTCNT			0x020
#define DW_MMC_INTMASK			0x024
#define DW_MMC_CMDARG			0x028
#define DW_MMC_CMD				0x02c
#define DW_MMC_RESP0			0x030
#define DW_MMC_RESP1			0x034
#define DW_MMC_RESP2			0x038
#define DW_MMC_RESP3			0x03c
#define DW_MMC_MINTSTS			0x040
#define DW_MMC_RINTSTS			0x044
#define DW_MMC_STATUS			0x048
#define DW_MMC_FIFOTH			0x04c
#define DW_MMC_CDETECT			0x050
#define DW_MMC_WRTPRT			0x054
#define DW_MMC_VERID			0x06c
#define DW_MMC_HCON				0x070
#define DW_MMC_UHS_REG			0x074
#define DW_MMC_BMOD				0x080
#define DW_MMC_PLDMND			0x084
// 32 bit address layout
#define DW_MMC_DBADDR			0x088
#define DW_MMC_IDSTS32			0x08c
#define DW_MMC_IDINTEN32		0x090
// 64 bit address layout
#define DW_MMC_DBADDRL			0x088
#define DW_MMC_DBADDRU			0x08c
#define DW_MMC_IDSTS64			0x090
#define DW_MMC_IDINTEN64		0x094

#define DW_MMC_REGS_SIZE		0x204

// CTRL
#define DW_MMC_CTRL_RESET		(1u << 0)
#define DW_MMC_CTRL_FIFO_RESET	(1u << 1)
#define DW_MMC_CTRL_DMA_RESET	(1u << 2)
#define DW_MMC_CTRL_INT_ENABLE	(1u << 4)
#define DW_MMC_CTRL_DMA_ENABLE	(1u << 5)
#define DW_MMC_CTRL_USE_IDMAC	(1u << 25)
#define DW_MMC_CTRL_ALL_RESETS \
	(DW_MMC_CTRL_RESET | DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET)

// CLKENA
#define DW_MMC_CLKENA_ENABLE	(1u << 0)

// CTYPE
#define DW_MMC_CTYPE_1BIT		0
#define DW_MMC_CTYPE_4BIT		(1u << 0)
#define DW_MMC_CTYPE_8BIT		(1u << 16)

// interrupts (INTMASK, MINTSTS, RINTSTS)
#define DW_MMC_INT_CD			(1u << 0)
#define DW_MMC_INT_RE			(1u << 1)	// response error
#define DW_MMC_INT_CMD_DONE		(1u << 2)
#define DW_MMC_INT_DTO			(1u << 3)	// data transfer over
#define DW_MMC_INT_TXDR			(1u << 4)
#define DW_MMC_INT_RXDR			(1u << 5)
#define DW_MMC_INT_RCRC			(1u << 6)
#define DW_MMC_INT_DCRC			(1u << 7)
#define DW_MMC_INT_RTO			(1u << 8)	// response timeout
#define DW_MMC_INT_DRTO			(1u << 9)	// data read timeout
#define DW_MMC_INT_HTO			(1u << 10)	// data starvation by host
#define DW_MMC_INT_FRUN			(1u << 11)
#define DW_MMC_INT_HLE			(1u << 12)	// hardware locked write error
#define DW_MMC_INT_SBE			(1u << 13)	// start bit / busy clear error
#define DW_MMC_INT_ACD			(1u << 14)	// auto command done
#define DW_MMC_INT_EBE			(1u << 15)	// end bit error
#define DW_MMC_INT_ALL			0xffffffffu

#define DW_MMC_INT_CMD_ERROR \
	(DW_MMC_INT_RE | DW_MMC_INT_RCRC | DW_MMC_INT_RTO | DW_MMC_INT_HLE)
#define DW_MMC_INT_DATA_ERROR \
	(DW_MMC_INT_DCRC | DW_MMC_INT_DRTO | DW_MMC_INT_HTO | DW_MMC_INT_FRUN \
		| DW_MMC_INT_SBE | DW_MMC_INT_EBE)

// CMD
#define DW_MMC_CMD_RESP_EXP		(1u << 6)
#define DW_MMC_CMD_RESP_LONG	(1u << 7)
#define DW_MMC_CMD_RESP_CRC		(1u << 8)
#define DW_MMC_CMD_DAT_EXP		(1u << 9)
#define DW_MMC_CMD_DAT_WR		(1u << 10)
#define DW_MMC_CMD_SEND_STOP	(1u << 12)
#define DW_MMC_CMD_PRV_DAT_WAIT	(1u << 13)
#define DW_MMC_CMD_STOP			(1u << 14)
#define DW_MMC_CMD_INIT			(1u << 15)
#define DW_MMC_CMD_UPD_CLK		(1u << 21)
#define DW_MMC_CMD_USE_HOLD_REG	(1u << 29)
#define DW_MMC_CMD_START		(1u << 31)

// STATUS
#define DW_MMC_STATUS_DATA_BUSY	(1u << 9)

// FIFOTH
#define DW_MMC_FIFOTH_RX_WMARK(x)	(((x) >> 16) & 0xfff)

// HCON
#define DW_MMC_HCON_TRANS_MODE(x)	(((x) >> 16) & 0x3)
#define DW_MMC_HCON_TRANS_IDMAC		0
#define DW_MMC_HCON_ADDR_CONFIG_64	(1u << 27)

// BMOD
#define DW_MMC_BMOD_SWR			(1u << 0)
#define DW_MMC_BMOD_FB			(1u << 1)
#define DW_MMC_BMOD_DE			(1u << 7)

// IDSTS
#define DW_MMC_IDSTS_TI			(1u << 0)
#define DW_MMC_IDSTS_RI			(1u << 1)
#define DW_MMC_IDSTS_FBE		(1u << 2)
#define DW_MMC_IDSTS_DU			(1u << 4)
#define DW_MMC_IDSTS_CES		(1u << 5)
#define DW_MMC_IDSTS_NIS		(1u << 8)
#define DW_MMC_IDSTS_AIS		(1u << 9)
#define DW_MMC_IDSTS_ALL		0x3ffu
#define DW_MMC_IDSTS_ERROR \
	(DW_MMC_IDSTS_FBE | DW_MMC_IDSTS_DU | DW_MMC_IDSTS_CES)

// IDMAC descriptor DES0
#define DW_MMC_DES0_DIC			(1u << 1)	// disable interrupt on completion
#define DW_MMC_DES0_LD			(1u << 2)	// last descriptor
#define DW_MMC_DES0_FS			(1u << 3)	// first descriptor
#define DW_MMC_DES0_CH			(1u << 4)	// second address is the next one
#define DW_MMC_DES0_OWN			(1u << 31)

// One descriptor moves at most this many bytes (buffer 1 size, 13 bits; Linux
// uses 4 KB as well)
#define DW_MMC_DESC_MAX_BYTES	4096


struct dw_mmc_desc32 {
	uint32	des0;
	uint32	des1;		// buffer 1 size
	uint32	des2;		// buffer 1 address
	uint32	des3;		// next descriptor
};


struct dw_mmc_desc64 {
	uint32	des0;
	uint32	des1;
	uint32	des2;		// buffer 1 size
	uint32	des3;
	uint32	des4;		// buffer 1 address, low
	uint32	des5;		// buffer 1 address, high
	uint32	des6;		// next descriptor, low
	uint32	des7;		// next descriptor, high
};


#endif	// DESIGNWARE_MMC_H
