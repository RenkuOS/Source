/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 *
 * Register definitions for the Intel PCH SMBus host controller
 * (PCI device 31, function 4).
 *
 * Source: Intel 334659-005, "7th and 8th Generation Intel Processor Family
 * I/O for U/Y Platforms ... Datasheet, Volume 2 of 2", chapter 7
 * "SMBus Interface (D31:F4)", tables 7-1 and 7-2. Device IDs from Intel
 * 332995-004, Volume 1, table 2-1 (9D23 = Sunrise Point-LP SMBus).
 *
 * Notes on that datasheet:
 * - The binary default shown for DID reads 0x9C22, an older PCH; the real
 *   ID comes from Volume 1 (9D23 for PCH-U/Y).
 * - Some descriptions refer to "D31:F3" for SMB_SMI_EN; on this PCH the
 *   SMBus is D31:F4 and the bit is HCFG bit 1.
 * - The offsets in table 7-2 are authoritative; in the text extraction the
 *   bit tables appear shifted by one register heading.
 */
#ifndef _I801_SMBUS_HARDWARE_H
#define _I801_SMBUS_HARDWARE_H


// PCI configuration space (table 7-1)
#define SMB_PCI_SMBMBAR_LO		0x10	// MMIO BAR, 64 bit
#define SMB_PCI_SMBMBAR_HI		0x14
#define SMB_PCI_SBA				0x20	// I/O BAR (32 bytes)
#define SMB_PCI_HCFG			0x40	// Host Configuration (8 bit)
#define SMB_PCI_TCOBASE			0x50
#define SMB_PCI_TCOCTL			0x54
#define SMB_PCI_HTIM			0x64	// Host SMBus Timing (BIOS owned)
#define SMB_PCI_SMBSM			0x80	// Power Gating

// HCFG bits
#define SMB_HCFG_HST_EN			(1 << 0)	// host controller enable
#define SMB_HCFG_SMB_SMI_EN		(1 << 1)	// route interrupts to SMI#
#define SMB_HCFG_I2C_EN			(1 << 2)	// I2C formatting
#define SMB_HCFG_SSRESET		(1 << 3)	// soft reset, self clearing
#define SMB_HCFG_SPD_WD			(1 << 4)	// block writes to 0x50 - 0x57

#define SMB_SBA_MASK			0xffe0		// SBA bits 15:5

// Device Status (PCI_status, 0x06) bit 3, INTS: this function asserts its
// interrupt (7.1.4). Lets the shared interrupt handler leave HSTS alone
// when the interrupt is not ours.
#define SMB_PCI_STATUS_INTS		(1 << 3)


// I/O (and MMIO) registers, offsets from SBA (table 7-2)
#define SMB_HSTS				0x00	// Host Status
#define SMB_HCTL				0x02	// Host Control
#define SMB_HCMD				0x03	// Host Command
#define SMB_TSA					0x04	// Transmit Slave Address
#define SMB_HD0					0x05	// Data 0 / block count
#define SMB_HD1					0x06	// Data 1
#define SMB_HBD					0x07	// Host Block Data
#define SMB_PEC					0x08	// Packet Error Check data
#define SMB_RSA					0x09	// Receive Slave Address
#define SMB_SD					0x0a	// Slave Data (16 bit)
#define SMB_AUXS				0x0c	// Auxiliary Status
#define SMB_AUXC				0x0d	// Auxiliary Control
#define SMB_SMLC				0x0e	// SMLINK pin control
#define SMB_SMBC				0x0f	// SMBUS pin control
#define SMB_SSTS				0x10	// Slave Status (64 kHz domain)
#define SMB_SCMD				0x11	// Slave Command (64 kHz domain)
#define SMB_NDA					0x14	// Notify Device Address
#define SMB_NDLB				0x16	// Notify Data Low Byte
#define SMB_NDHB				0x17	// Notify Data High Byte

// HSTS bits (all RW1C except IUS)
#define SMB_HSTS_HBSY			(1 << 0)	// host busy
#define SMB_HSTS_INTR			(1 << 1)	// command completed
#define SMB_HSTS_DERR			(1 << 2)	// device error (NACK, timeout, CRC)
#define SMB_HSTS_BERR			(1 << 3)	// bus error (collision)
#define SMB_HSTS_FAIL			(1 << 4)	// killed transaction
#define SMB_HSTS_SMBALERT		(1 << 5)
#define SMB_HSTS_IUS			(1 << 6)	// in use semaphore
#define SMB_HSTS_BDS			(1 << 7)	// byte done
#define SMB_HSTS_ERRORS			(SMB_HSTS_DERR | SMB_HSTS_BERR \
									| SMB_HSTS_FAIL)
#define SMB_HSTS_DONE			(SMB_HSTS_INTR | SMB_HSTS_ERRORS)
#define SMB_HSTS_CLEAR			(SMB_HSTS_BDS | SMB_HSTS_DONE)

// HCTL bits
#define SMB_HCTL_INTREN			(1 << 0)
#define SMB_HCTL_KILL			(1 << 1)
#define SMB_HCTL_CMD_SHIFT		2
#define SMB_HCTL_CMD_MASK		(7 << SMB_HCTL_CMD_SHIFT)
#define SMB_HCTL_LAST_BYTE		(1 << 5)
#define SMB_HCTL_START			(1 << 6)
#define SMB_HCTL_PEC_EN			(1 << 7)

// SMB_CMD encodings (HCTL bits 4:2)
#define SMB_CMD_QUICK			(0 << SMB_HCTL_CMD_SHIFT)
#define SMB_CMD_BYTE			(1 << SMB_HCTL_CMD_SHIFT)
#define SMB_CMD_BYTE_DATA		(2 << SMB_HCTL_CMD_SHIFT)
#define SMB_CMD_WORD_DATA		(3 << SMB_HCTL_CMD_SHIFT)
#define SMB_CMD_PROCESS_CALL	(4 << SMB_HCTL_CMD_SHIFT)
#define SMB_CMD_BLOCK			(5 << SMB_HCTL_CMD_SHIFT)
#define SMB_CMD_I2C_READ		(6 << SMB_HCTL_CMD_SHIFT)
#define SMB_CMD_BLOCK_PROCESS	(7 << SMB_HCTL_CMD_SHIFT)

// TSA: 7 bit address in bits 7:1, bit 0 is the direction
#define SMB_TSA_READ			1
#define SMB_TSA_WRITE			0

// AUXS bits
#define SMB_AUXS_CRCE			(1 << 0)

// AUXC bits
#define SMB_AUXC_AAC			(1 << 0)	// automatically append CRC
#define SMB_AUXC_E32B			(1 << 1)	// 32 byte block buffer

// SSTS bits
#define SMB_SSTS_HOST_NOTIFY	(1 << 0)	// RW1C; while set, new Host
											// Notify messages are NACKed

// SCMD bits
#define SMB_SCMD_HNI			(1 << 0)	// Host Notify interrupt enable
#define SMB_SCMD_HNW			(1 << 1)	// Host Notify wake enable
#define SMB_SCMD_SMBALERT_DIS	(1 << 2)

// NDA: 7 bit device address in bits 7:1


#endif	// _I801_SMBUS_HARDWARE_H
