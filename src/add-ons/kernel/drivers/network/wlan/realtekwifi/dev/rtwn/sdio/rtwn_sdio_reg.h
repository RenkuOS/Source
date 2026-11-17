/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* Copyright (C) 2021 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 * Copyright (C) 2021 Jernej Skrabec <jernej.skrabec@gmail.com>
 *
 * SDIO register layout of Realtek WiFi chips, from the rtw88 driver
 * (sdio.h), used here under the BSD-3-Clause license.
 * Changes for RenkuOS by Akakor, 2026: only the registers that the rtwn
 * SDIO bus glue needs, with the names of rtwn.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef RTWN_SDIO_REG_H
#define RTWN_SDIO_REG_H

/*
 * CMD52 and CMD53 addresses on function 1: bits 16:13 select the block,
 * the low bits are the offset in it (the length in dwords for the transmit
 * FIFOs).
 */
#define RTWN_SDIO_ADDR_SDIO_REG		0
#define RTWN_SDIO_ADDR_TXFF_HIGH	4
#define RTWN_SDIO_ADDR_TXFF_NORMAL	5
#define RTWN_SDIO_ADDR_TXFF_LOW		6
#define RTWN_SDIO_ADDR_RXFF		7
#define RTWN_SDIO_ADDR_MAC_REG		8

#define RTWN_SDIO_ADDR(block, offset)	(((block) << 13) | (offset))
#define RTWN_SDIO_LOCAL(reg)		\
	RTWN_SDIO_ADDR(RTWN_SDIO_ADDR_SDIO_REG, (reg) & 0x0fff)
#define RTWN_SDIO_MAC(reg)		\
	RTWN_SDIO_ADDR(RTWN_SDIO_ADDR_MAC_REG, (reg) & 0xffff)
#define RTWN_SDIO_RX0FF(id)		\
	RTWN_SDIO_ADDR(RTWN_SDIO_ADDR_RXFF, (id) & 0x3)

#define RTWN_SDIO_BLOCK_SIZE		512
#define RTWN_SDIO_DATA_ALIGN		8

/* SDIO local registers. */
#define RTWN_SDIO_TX_CTRL		0x0000
#define RTWN_SDIO_HIMR			0x0014
#define RTWN_SDIO_HISR			0x0018
#define RTWN_SDIO_RX0_REQ_LEN		0x001c
#define RTWN_SDIO_OQT_FREE_PG		0x001e
#define RTWN_SDIO_FREE_TXPG		0x0020
#define RTWN_SDIO_HSUS_CTRL		0x0086

/* Bits for RTWN_SDIO_HIMR and RTWN_SDIO_HISR. */
#define RTWN_SDIO_INT_RX_REQUEST	0x00000001
#define RTWN_SDIO_INT_AVAL		0x00000002
#define RTWN_SDIO_INT_TXERR		0x00000004
#define RTWN_SDIO_INT_RXERR		0x00000008
#define RTWN_SDIO_INT_TXFOVW		0x00000010
#define RTWN_SDIO_INT_RXFOVW		0x00000020
#define RTWN_SDIO_INT_TXBCNOK		0x00000040
#define RTWN_SDIO_INT_TXBCNERR		0x00000080
#define RTWN_SDIO_INT_C2HCMD		0x00020000
#define RTWN_SDIO_INT_CPWM1		0x00040000
#define RTWN_SDIO_INT_CPWM2		0x00080000
#define RTWN_SDIO_INT_HSISR_IND		0x00100000
#define RTWN_SDIO_INT_GTINT3_IND	0x00200000
#define RTWN_SDIO_INT_GTINT4_IND	0x00400000
#define RTWN_SDIO_INT_PSTIMEOUT		0x00800000
#define RTWN_SDIO_INT_OCPINT		0x01000000

/* Status bits that are acknowledged by writing them back. */
#define RTWN_SDIO_HISR_CLEAR_MASK					\
	(RTWN_SDIO_INT_TXERR | RTWN_SDIO_INT_RXERR |			\
	 RTWN_SDIO_INT_TXFOVW | RTWN_SDIO_INT_RXFOVW |			\
	 RTWN_SDIO_INT_TXBCNOK | RTWN_SDIO_INT_TXBCNERR |		\
	 RTWN_SDIO_INT_C2HCMD | RTWN_SDIO_INT_CPWM1 |			\
	 RTWN_SDIO_INT_CPWM2 | RTWN_SDIO_INT_HSISR_IND |		\
	 RTWN_SDIO_INT_GTINT3_IND | RTWN_SDIO_INT_GTINT4_IND |		\
	 RTWN_SDIO_INT_PSTIMEOUT | RTWN_SDIO_INT_OCPINT)

/* Bits for RTWN_SDIO_FREE_TXPG (pages of each transmit FIFO). */
#define RTWN_SDIO_FREE_TXPG_HIGH(x)	((x) & 0xff)
#define RTWN_SDIO_FREE_TXPG_NORMAL(x)	(((x) >> 8) & 0xff)
#define RTWN_SDIO_FREE_TXPG_LOW(x)	(((x) >> 16) & 0xff)
#define RTWN_SDIO_FREE_TXPG_PUB(x)	(((x) >> 24) & 0xff)

/* Bits for RTWN_SDIO_HSUS_CTRL. */
#define RTWN_SDIO_HSUS_SUS_REQ		0x01
#define RTWN_SDIO_HSUS_RESUME_RDY	0x02

#endif	/* RTWN_SDIO_REG_H */
