/*
 * Copyright 2026, Bryan Irvine
 * Copyright (c) 2020 Michael J Karels
 * Copyright (c) 2016, 2020 Jared McNeill <jmcneill@invisible.ca>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Driver for the Broadcom GENET v5 Ethernet controller of the BCM2711
 * (Raspberry Pi 4 / 400), as exposed by the Raspberry Pi 4 UEFI firmware in
 * ACPI mode (HID "BCM6E4E").
 *
 * The controller's DMA is not cache coherent, so all packet buffers live in
 * uncached memory and frames are copied in and out. The descriptors
 * themselves live in the controller's register space.
 *
 * Provenance: the register layout and the reset, DMA and ring
 * initialisation sequences follow FreeBSD's if_genet
 * (sys/arm64/broadcom/genet/if_genet.c and if_genetreg.h, by Michael J
 * Karels and Jared McNeill), which is itself derived from NetBSD's
 * bcmgenet (sys/dev/ic/bcmgenet.c, by Jared McNeill). This file is
 * therefore distributed under their BSD-2-Clause licence, with their
 * copyright notices kept above. The rest (the Haiku device manager, ACPI,
 * net_buffer and ioctl glue, the uncached-buffer scheme, the PHY polling)
 * was written for Haiku/RenkuOS and is offered under the same terms.
 *
 * Acknowledgements: thanks to the authors of the drivers this one was
 * written with, or checked against:
 *  - FreeBSD if_genet, Michael J Karels and Jared McNeill (BSD-2-Clause)
 *  - NetBSD bcmgenet, Jared McNeill (BSD-2-Clause)
 *  - Linux bcmgenet, Broadcom Corporation and contributors (GPL-2.0),
 *    compared against only; no code was taken from it
 *  - 9front's ethergenet for the Raspberry Pi 4 (MIT), compared against
 *    only; no code was taken from it
 */


#include <net/if_media.h>
#include <sys/sockio.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ACPI.h>
#include "acpi.h"

#include <condition_variable.h>
#include <ethernet.h>
#include <kernel.h>
#include <lock.h>
#include <net_buffer.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include "ether_driver.h"


//#define TRACE_GENET
#ifdef TRACE_GENET
#	define TRACE(x...) dprintf("genet: " x)
#else
#	define TRACE(x...) ;
#endif
#define INFO(x...) dprintf("genet: " x)
#define ERROR(x...) dprintf("genet: " x)


#define GENET_DRIVER_MODULE_NAME "drivers/network/genet/driver_v1"
#define GENET_DEVICE_MODULE_NAME "drivers/network/genet/device_v1"
#define GENET_DEVICE_ID_GENERATOR "genet/device_id"


// #pragma mark - registers (see FreeBSD if_genetreg.h)


#define BIT(n)	(1u << (n))

enum {
	SYS_REV_CTRL			= 0x000,
	SYS_PORT_CTRL			= 0x004,
	 SYS_PORT_MODE_EXT_GPHY	= 3,
	SYS_RBUF_FLUSH_CTRL		= 0x008,
	 SYS_RBUF_FLUSH_RESET	= BIT(1),
	EXT_RGMII_OOB_CTRL		= 0x08c,
	 RGMII_OOB_ID_MODE_DISABLE	= BIT(16),
	 RGMII_OOB_MODE_EN		= BIT(6),
	 RGMII_OOB_OOB_DISABLE	= BIT(5),
	 RGMII_OOB_LINK			= BIT(4),
	INTRL2_CPU_STAT			= 0x200,
	INTRL2_CPU_CLEAR		= 0x208,
	INTRL2_CPU_STAT_MASK	= 0x20c,
	INTRL2_CPU_SET_MASK		= 0x210,
	INTRL2_CPU_CLEAR_MASK	= 0x214,
	 IRQ_TXDMA_DONE			= BIT(16),
	 IRQ_RXDMA_DONE			= BIT(13),
	RBUF_CTRL				= 0x300,
	 RBUF_ALIGN_2B			= BIT(1),
	 RBUF_64B_EN			= BIT(0),
	RBUF_CHECK_CTRL			= 0x314,
	RBUF_TBUF_SIZE_CTRL		= 0x3b4,
	TBUF_CTRL				= 0x600,
	UMAC_CMD				= 0x808,
	 UMAC_CMD_LCL_LOOP_EN	= BIT(15),
	 UMAC_CMD_SW_RESET		= BIT(13),
	 UMAC_CMD_HD_EN			= BIT(10),
	 UMAC_CMD_PROMISC		= BIT(4),
	 UMAC_CMD_SPEED_MASK	= 3 << 2,
	 UMAC_CMD_SPEED_10		= 0 << 2,
	 UMAC_CMD_SPEED_100		= 1 << 2,
	 UMAC_CMD_SPEED_1000	= 2 << 2,
	 UMAC_CMD_RXEN			= BIT(1),
	 UMAC_CMD_TXEN			= BIT(0),
	UMAC_MAC0				= 0x80c,
	UMAC_MAC1				= 0x810,
	UMAC_MAX_FRAME_LEN		= 0x814,
	UMAC_MIB_CTRL			= 0xd80,
	MDIO_CMD				= 0xe14,
	 MDIO_START_BUSY		= BIT(29),
	 MDIO_READ_FAILED		= BIT(28),
	 MDIO_READ				= BIT(27),
	 MDIO_WRITE				= BIT(26),
	 MDIO_ADDR_SHIFT		= 21,
	 MDIO_REG_SHIFT			= 16,
	UMAC_MDF_CTRL			= 0xe50,
};

#define UMAC_MDF_ADDR0(n)	(0xe54 + (n) * 8)
#define UMAC_MDF_ADDR1(n)	(0xe58 + (n) * 8)
static const int kMaxMdfFilters = 17;

static const uint32 kDescCount = 256;
static const uint32 kDescSize = 12;
static const uint32 kDefaultQueue = 16;
static const uint32 kProdConsMask = 0xffff;
static const uint32 kBufferSize = 2048;
static const uint32 kMaxFrameSize = 1536;

#define RX_BASE		0x2000
#define TX_BASE		0x4000
#define RX_RING(q)	(RX_BASE + 0xc00 + 0x40 * (q))
#define TX_RING(q)	(TX_BASE + 0xc00 + 0x40 * (q))

#define RX_WRITE_PTR_LO(q)	(RX_RING(q) + 0x00)
#define RX_WRITE_PTR_HI(q)	(RX_RING(q) + 0x04)
#define RX_PROD_INDEX(q)	(RX_RING(q) + 0x08)
#define RX_CONS_INDEX(q)	(RX_RING(q) + 0x0c)
#define RX_RING_BUF_SIZE(q)	(RX_RING(q) + 0x10)
#define RX_START_ADDR_LO(q)	(RX_RING(q) + 0x14)
#define RX_START_ADDR_HI(q)	(RX_RING(q) + 0x18)
#define RX_END_ADDR_LO(q)	(RX_RING(q) + 0x1c)
#define RX_END_ADDR_HI(q)	(RX_RING(q) + 0x20)
#define RX_XON_XOFF_THRES(q) (RX_RING(q) + 0x28)
#define RX_READ_PTR_LO(q)	(RX_RING(q) + 0x2c)
#define RX_READ_PTR_HI(q)	(RX_RING(q) + 0x30)

#define TX_READ_PTR_LO(q)	(TX_RING(q) + 0x00)
#define TX_READ_PTR_HI(q)	(TX_RING(q) + 0x04)
#define TX_CONS_INDEX(q)	(TX_RING(q) + 0x08)
#define TX_PROD_INDEX(q)	(TX_RING(q) + 0x0c)
#define TX_RING_BUF_SIZE(q)	(TX_RING(q) + 0x10)
#define TX_START_ADDR_LO(q)	(TX_RING(q) + 0x14)
#define TX_START_ADDR_HI(q)	(TX_RING(q) + 0x18)
#define TX_END_ADDR_LO(q)	(TX_RING(q) + 0x1c)
#define TX_END_ADDR_HI(q)	(TX_RING(q) + 0x20)
#define TX_MBUF_DONE_THRES(q) (TX_RING(q) + 0x24)
#define TX_FLOW_PERIOD(q)	(TX_RING(q) + 0x28)
#define TX_WRITE_PTR_LO(q)	(TX_RING(q) + 0x2c)
#define TX_WRITE_PTR_HI(q)	(TX_RING(q) + 0x30)

#define RX_DESC_STATUS(i)	(RX_BASE + kDescSize * (i) + 0x00)
#define RX_DESC_ADDR_LO(i)	(RX_BASE + kDescSize * (i) + 0x04)
#define RX_DESC_ADDR_HI(i)	(RX_BASE + kDescSize * (i) + 0x08)
#define TX_DESC_STATUS(i)	(TX_BASE + kDescSize * (i) + 0x00)
#define TX_DESC_ADDR_LO(i)	(TX_BASE + kDescSize * (i) + 0x04)
#define TX_DESC_ADDR_HI(i)	(TX_BASE + kDescSize * (i) + 0x08)

enum {
	RX_DESC_BUFLEN_MASK		= 0xfff0000,
	RX_DESC_BUFLEN_SHIFT	= 16,
	RX_DESC_EOP				= BIT(14),
	RX_DESC_SOP				= BIT(13),
	RX_DESC_RX_ERROR		= BIT(2),

	TX_DESC_BUFLEN_SHIFT	= 16,
	TX_DESC_EOP				= BIT(14),
	TX_DESC_SOP				= BIT(13),
	TX_DESC_QTAG_MASK		= 0x1f80,
	TX_DESC_CRC				= BIT(6),
};

#define RX_DMA_RING_CFG		(RX_BASE + 0x1040 + 0x00)
#define RX_DMA_CTRL			(RX_BASE + 0x1040 + 0x04)
#define RX_SCB_BURST_SIZE	(RX_BASE + 0x1040 + 0x0c)
#define TX_DMA_RING_CFG		(TX_BASE + 0x1040 + 0x00)
#define TX_DMA_CTRL			(TX_BASE + 0x1040 + 0x04)
#define TX_SCB_BURST_SIZE	(TX_BASE + 0x1040 + 0x0c)
#define DMA_CTRL_EN			BIT(0)
#define DMA_CTRL_RBUF_EN(q)	BIT((q) + 1)


// MII registers
enum {
	MII_BMCR		= 0x00,
	 BMCR_RESET		= 0x8000,
	 BMCR_AUTOEN	= 0x1000,
	 BMCR_STARTNEG	= 0x0200,
	MII_BMSR		= 0x01,
	 BMSR_LINK		= 0x0004,
	 BMSR_ACOMP		= 0x0020,
	MII_PHYIDR1		= 0x02,
	MII_PHYIDR2		= 0x03,
	MII_ANAR		= 0x04,
	MII_ANLPAR		= 0x05,
	 ANAR_100_FD	= 0x0100,
	 ANAR_100_HD	= 0x0080,
	 ANAR_10_FD		= 0x0040,
	 ANAR_10_HD		= 0x0020,
	MII_100T2CR		= 0x09,
	 GTCR_ADV_1000TFDX	= 0x0200,
	 GTCR_ADV_1000THDX	= 0x0100,
	MII_100T2SR		= 0x0a,
	 GTSR_LP_1000TFDX	= 0x0800,
	 GTSR_LP_1000THDX	= 0x0400,

	// Broadcom specific (BCM54213PE)
	BRCM_AUX_CTL			= 0x18,
	 AUXCTL_SHDWSEL_MISC	= 0x07,
	 AUXCTL_MISC_WREN		= 0x8000,
	 AUXCTL_MISC_RGMII_SKEW_EN = 0x0100,
	 AUXCTL_SHDWSEL_READ_SHIFT = 12,
	BRCM_SHADOW_1C			= 0x1c,
	 SHD_CLK_CTL			= 0x03,
	 SHD_CLK_GTXCLK_EN		= 0x0200,
	 SHD_WRITE				= 0x8000,
};


// #pragma mark - driver state


struct genet_info {
	device_node*		node;

	area_id				registerArea;
	volatile uint32*	registers;
	uint32				irq;

	area_id				bufferArea;
	uint8*				rxBuffers;
	uint8*				txBuffers;
	phys_addr_t			rxPhysical;
	phys_addr_t			txPhysical;

	ether_address_t		macAddress;
	int					phyAddress;

	// RX
	mutex				rxLock;
	ConditionVariable	rxCondition;
	uint32				rxConsumer;
	bool				rxReady;

	// TX
	mutex				txLock;
	ConditionVariable	txCondition;
	uint32				txProducer;
	uint32				txConsumer;

	// link
	thread_id			linkThread;
	volatile bool		stopLinkThread;
	bool				linkUp;
	uint32				linkSpeed;		// in Mbit/s
	bool				fullDuplex;
	sem_id				linkStateSem;

	bool				nonblocking;
	bool				promiscuous;
	int32				openCount;
	volatile bool		closed;
		// set by close(), so that blocked readers and writers return
};


struct genet_handle {
	genet_info*			info;
};


static device_manager_info* sDeviceManager;
static net_buffer_module_info* sBufferModule;


static inline uint32
genet_read(genet_info* info, uint32 offset)
{
	return info->registers[offset / 4];
}


static inline void
genet_write(genet_info* info, uint32 offset, uint32 value)
{
	info->registers[offset / 4] = value;
}


// #pragma mark - DMA memory


static inline void
clean_invalidate_dcache(void* address, size_t size)
{
#ifdef __aarch64__
	uint64 ctr;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	size_t line = 4 << ((ctr >> 16) & 0xf);
	addr_t start = (addr_t)address & ~(line - 1);
	for (addr_t p = start; p < (addr_t)address + size; p += line)
		asm volatile("dc civac, %0" : : "r"(p) : "memory");
	asm volatile("dsb sy" : : : "memory");
#endif
}


static inline void
dma_write_barrier()
{
#ifdef __aarch64__
	asm volatile("dsb st" : : : "memory");
#else
	memory_write_barrier();
#endif
}


static inline void
dma_read_barrier()
{
#ifdef __aarch64__
	asm volatile("dsb ld" : : : "memory");
#else
	memory_read_barrier();
#endif
}


static status_t
genet_alloc_buffers(genet_info* info)
{
	size_t size = 2 * kDescCount * kBufferSize;
	void* address;
	info->bufferArea = create_area("genet buffers", &address,
		B_ANY_KERNEL_ADDRESS, size, B_32_BIT_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (info->bufferArea < 0)
		return info->bufferArea;

	physical_entry entry;
	status_t status = get_memory_map(address, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK) {
		delete_area(info->bufferArea);
		return status;
	}

	memset(address, 0, size);
	clean_invalidate_dcache(address, size);
	// The controller does not snoop the CPU caches.
	vm_set_area_memory_type(info->bufferArea, entry.address,
		B_WRITE_COMBINING_MEMORY);

	info->rxBuffers = (uint8*)address;
	info->txBuffers = (uint8*)address + kDescCount * kBufferSize;
	info->rxPhysical = entry.address;
	info->txPhysical = entry.address + kDescCount * kBufferSize;
	return B_OK;
}


// #pragma mark - MDIO / PHY


static int
genet_mdio_read(genet_info* info, int phy, int reg)
{
	genet_write(info, MDIO_CMD, MDIO_READ | (phy << MDIO_ADDR_SHIFT)
		| (reg << MDIO_REG_SHIFT));
	uint32 value = genet_read(info, MDIO_CMD);
	genet_write(info, MDIO_CMD, value | MDIO_START_BUSY);
	for (int retry = 1000; retry > 0; retry--) {
		value = genet_read(info, MDIO_CMD);
		if ((value & MDIO_START_BUSY) == 0) {
			if ((value & MDIO_READ_FAILED) != 0)
				return -1;
			return value & 0xffff;
		}
		spin(10);
	}
	ERROR("MDIO read timeout (phy %d, reg %d)\n", phy, reg);
	return -1;
}


static void
genet_mdio_write(genet_info* info, int phy, int reg, int value)
{
	genet_write(info, MDIO_CMD, MDIO_WRITE | (phy << MDIO_ADDR_SHIFT)
		| (reg << MDIO_REG_SHIFT) | (value & 0xffff));
	uint32 command = genet_read(info, MDIO_CMD);
	genet_write(info, MDIO_CMD, command | MDIO_START_BUSY);
	for (int retry = 1000; retry > 0; retry--) {
		if ((genet_read(info, MDIO_CMD) & MDIO_START_BUSY) == 0)
			return;
		spin(10);
	}
	ERROR("MDIO write timeout (phy %d, reg %d)\n", phy, reg);
}


static bool
genet_phy_present(genet_info* info, int phy)
{
	int id1 = genet_mdio_read(info, phy, MII_PHYIDR1);
	int id2 = genet_mdio_read(info, phy, MII_PHYIDR2);
	if (id1 < 0 || id2 < 0 || (id1 == 0xffff && id2 == 0xffff)
		|| (id1 == 0 && id2 == 0)) {
		return false;
	}
	INFO("PHY at address %d, id %04x:%04x\n", phy, id1, id2);
	return true;
}


static void
genet_phy_init(genet_info* info)
{
	info->phyAddress = -1;
	// The Pi 4's BCM54213PE is at address 1; scan the others just in case.
	if (genet_phy_present(info, 1))
		info->phyAddress = 1;
	for (int phy = 0; info->phyAddress < 0 && phy < 32; phy++) {
		if (phy != 1 && genet_phy_present(info, phy))
			info->phyAddress = phy;
	}
	if (info->phyAddress < 0) {
		ERROR("no PHY found\n");
		return;
	}
	int phy = info->phyAddress;

	// Reset the PHY
	genet_mdio_write(info, phy, MII_BMCR, BMCR_RESET);
	for (int i = 0; i < 100; i++) {
		int bmcr = genet_mdio_read(info, phy, MII_BMCR);
		if (bmcr >= 0 && (bmcr & BMCR_RESET) == 0)
			break;
		snooze(1000);
	}

	// "rgmii-rxid": the PHY adds the RX clock delay, but not the TX one.
	genet_mdio_write(info, phy, BRCM_AUX_CTL, AUXCTL_SHDWSEL_MISC
		| (AUXCTL_SHDWSEL_MISC << AUXCTL_SHDWSEL_READ_SHIFT));
	int misc = genet_mdio_read(info, phy, BRCM_AUX_CTL);
	if (misc >= 0) {
		misc |= AUXCTL_MISC_WREN | AUXCTL_MISC_RGMII_SKEW_EN;
		genet_mdio_write(info, phy, BRCM_AUX_CTL,
			AUXCTL_SHDWSEL_MISC | (misc & ~0x7));
	}

	genet_mdio_write(info, phy, BRCM_SHADOW_1C, SHD_CLK_CTL << 10);
	int clock = genet_mdio_read(info, phy, BRCM_SHADOW_1C);
	if (clock >= 0) {
		clock &= 0x3ff & ~SHD_CLK_GTXCLK_EN;
		genet_mdio_write(info, phy, BRCM_SHADOW_1C,
			SHD_WRITE | (SHD_CLK_CTL << 10) | clock);
	}

	// Advertise everything and start autonegotiation
	genet_mdio_write(info, phy, MII_ANAR,
		ANAR_100_FD | ANAR_100_HD | ANAR_10_FD | ANAR_10_HD | 0x01);
	genet_mdio_write(info, phy, MII_100T2CR,
		GTCR_ADV_1000TFDX | GTCR_ADV_1000THDX);
	genet_mdio_write(info, phy, MII_BMCR, BMCR_AUTOEN | BMCR_STARTNEG);
}


static void
genet_update_link(genet_info* info)
{
	if (info->phyAddress < 0)
		return;
	int phy = info->phyAddress;

	// BMSR link status is latched low; read twice
	genet_mdio_read(info, phy, MII_BMSR);
	int bmsr = genet_mdio_read(info, phy, MII_BMSR);
	bool up = bmsr >= 0 && (bmsr & BMSR_LINK) != 0
		&& (bmsr & BMSR_ACOMP) != 0;

	uint32 speed = 0;
	bool fullDuplex = true;
	if (up) {
		int gtcr = genet_mdio_read(info, phy, MII_100T2CR);
		int gtsr = genet_mdio_read(info, phy, MII_100T2SR);
		int anar = genet_mdio_read(info, phy, MII_ANAR);
		int anlpar = genet_mdio_read(info, phy, MII_ANLPAR);
		int common = anar & anlpar;
		if ((gtcr & GTCR_ADV_1000TFDX) && (gtsr & GTSR_LP_1000TFDX)) {
			speed = 1000;
		} else if ((gtcr & GTCR_ADV_1000THDX) && (gtsr & GTSR_LP_1000THDX)) {
			speed = 1000;
			fullDuplex = false;
		} else if (common & ANAR_100_FD) {
			speed = 100;
		} else if (common & ANAR_100_HD) {
			speed = 100;
			fullDuplex = false;
		} else if (common & ANAR_10_FD) {
			speed = 10;
		} else {
			speed = 10;
			fullDuplex = false;
		}
	}

	if (up == info->linkUp && speed == info->linkSpeed
		&& fullDuplex == info->fullDuplex) {
		return;
	}

	info->linkUp = up;
	info->linkSpeed = speed;
	info->fullDuplex = fullDuplex;

	if (up) {
		uint32 value = genet_read(info, EXT_RGMII_OOB_CTRL);
		value &= ~(RGMII_OOB_OOB_DISABLE | RGMII_OOB_ID_MODE_DISABLE);
		value |= RGMII_OOB_LINK | RGMII_OOB_MODE_EN;
		genet_write(info, EXT_RGMII_OOB_CTRL, value);

		value = genet_read(info, UMAC_CMD);
		value &= ~(UMAC_CMD_SPEED_MASK | UMAC_CMD_HD_EN);
		if (speed == 1000)
			value |= UMAC_CMD_SPEED_1000;
		else if (speed == 100)
			value |= UMAC_CMD_SPEED_100;
		else
			value |= UMAC_CMD_SPEED_10;
		if (!fullDuplex)
			value |= UMAC_CMD_HD_EN;
		genet_write(info, UMAC_CMD, value);

		INFO("link up, %" B_PRIu32 " Mbit/s, %s duplex\n", speed,
			fullDuplex ? "full" : "half");
	} else
		INFO("link down\n");

	if (info->linkStateSem >= 0)
		release_sem_etc(info->linkStateSem, 1, B_DO_NOT_RESCHEDULE);
}


static status_t
genet_link_thread(void* data)
{
	genet_info* info = (genet_info*)data;
	while (!info->stopLinkThread) {
		genet_update_link(info);
		snooze(1000000);
	}
	return B_OK;
}


// #pragma mark - controller setup


static void
genet_reset(genet_info* info)
{
	uint32 value = genet_read(info, SYS_RBUF_FLUSH_CTRL);
	value |= SYS_RBUF_FLUSH_RESET;
	genet_write(info, SYS_RBUF_FLUSH_CTRL, value);
	spin(10);
	value &= ~SYS_RBUF_FLUSH_RESET;
	genet_write(info, SYS_RBUF_FLUSH_CTRL, value);
	spin(10);
	genet_write(info, SYS_RBUF_FLUSH_CTRL, 0);
	spin(10);

	genet_write(info, UMAC_CMD, 0);
	genet_write(info, UMAC_CMD, UMAC_CMD_LCL_LOOP_EN | UMAC_CMD_SW_RESET);
	spin(10);
	genet_write(info, UMAC_CMD, 0);

	genet_write(info, UMAC_MIB_CTRL, 7);
	genet_write(info, UMAC_MIB_CTRL, 0);
}


static void
genet_dma_disable(genet_info* info)
{
	uint32 value = genet_read(info, TX_DMA_CTRL);
	value &= ~(DMA_CTRL_EN | DMA_CTRL_RBUF_EN(kDefaultQueue));
	genet_write(info, TX_DMA_CTRL, value);

	value = genet_read(info, RX_DMA_CTRL);
	value &= ~(DMA_CTRL_EN | DMA_CTRL_RBUF_EN(kDefaultQueue));
	genet_write(info, RX_DMA_CTRL, value);
}


static void
genet_set_mdf(genet_info* info, int n, const uint8* address)
{
	genet_write(info, UMAC_MDF_ADDR0(n), (address[0] << 8) | address[1]);
	genet_write(info, UMAC_MDF_ADDR1(n), (address[2] << 24)
		| (address[3] << 16) | (address[4] << 8) | address[5]);
}


static void
genet_setup_rx_filter(genet_info* info)
{
	uint32 command = genet_read(info, UMAC_CMD);
	uint32 mdfControl = 0;

	// We do not program multicast filters (yet): accept all multicast by
	// running promiscuous whenever asked, otherwise broadcast + unicast.
	if (info->promiscuous) {
		command |= UMAC_CMD_PROMISC;
	} else {
		static const uint8 kBroadcast[6]
			= { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
		command &= ~UMAC_CMD_PROMISC;
		genet_set_mdf(info, 0, kBroadcast);
		genet_set_mdf(info, 1, info->macAddress.ebyte);
		int n = 2;
		mdfControl = (BIT(kMaxMdfFilters) - 1) & ~(BIT(kMaxMdfFilters - n) - 1);
	}

	genet_write(info, UMAC_CMD, command);
	genet_write(info, UMAC_MDF_CTRL, mdfControl);
}


static void
genet_init_rings(genet_info* info)
{
	const uint32 q = kDefaultQueue;

	// TX
	info->txProducer = info->txConsumer = 0;
	genet_write(info, TX_SCB_BURST_SIZE, 0x08);
	genet_write(info, TX_READ_PTR_LO(q), 0);
	genet_write(info, TX_READ_PTR_HI(q), 0);
	genet_write(info, TX_CONS_INDEX(q), 0);
	genet_write(info, TX_PROD_INDEX(q), 0);
	genet_write(info, TX_RING_BUF_SIZE(q), (kDescCount << 16)
		| (kBufferSize & 0xffff));
	genet_write(info, TX_START_ADDR_LO(q), 0);
	genet_write(info, TX_START_ADDR_HI(q), 0);
	genet_write(info, TX_END_ADDR_LO(q), kDescCount * kDescSize / 4 - 1);
	genet_write(info, TX_END_ADDR_HI(q), 0);
	genet_write(info, TX_MBUF_DONE_THRES(q), 1);
	genet_write(info, TX_FLOW_PERIOD(q), 0);
	genet_write(info, TX_WRITE_PTR_LO(q), 0);
	genet_write(info, TX_WRITE_PTR_HI(q), 0);
	genet_write(info, TX_DMA_RING_CFG, BIT(q));
	uint32 value = genet_read(info, TX_DMA_CTRL);
	genet_write(info, TX_DMA_CTRL, value | DMA_CTRL_EN | DMA_CTRL_RBUF_EN(q));

	// RX
	info->rxConsumer = 0;
	genet_write(info, RX_SCB_BURST_SIZE, 0x08);
	genet_write(info, RX_WRITE_PTR_LO(q), 0);
	genet_write(info, RX_WRITE_PTR_HI(q), 0);
	genet_write(info, RX_PROD_INDEX(q), 0);
	genet_write(info, RX_CONS_INDEX(q), 0);
	genet_write(info, RX_RING_BUF_SIZE(q), (kDescCount << 16)
		| (kBufferSize & 0xffff));
	genet_write(info, RX_START_ADDR_LO(q), 0);
	genet_write(info, RX_START_ADDR_HI(q), 0);
	genet_write(info, RX_END_ADDR_LO(q), kDescCount * kDescSize / 4 - 1);
	genet_write(info, RX_END_ADDR_HI(q), 0);
	genet_write(info, RX_XON_XOFF_THRES(q), (5 << 16) | (kDescCount >> 4));
	genet_write(info, RX_READ_PTR_LO(q), 0);
	genet_write(info, RX_READ_PTR_HI(q), 0);
	genet_write(info, RX_DMA_RING_CFG, BIT(q));

	// Every RX descriptor permanently owns one buffer.
	for (uint32 i = 0; i < kDescCount; i++) {
		phys_addr_t address = info->rxPhysical + i * kBufferSize;
		genet_write(info, RX_DESC_ADDR_LO(i), (uint32)address);
		genet_write(info, RX_DESC_ADDR_HI(i), (uint32)(address >> 32));
	}

	value = genet_read(info, RX_DMA_CTRL);
	genet_write(info, RX_DMA_CTRL, value | DMA_CTRL_EN | DMA_CTRL_RBUF_EN(q));
}


static void
genet_enable(genet_info* info)
{
	genet_write(info, UMAC_MAX_FRAME_LEN, kMaxFrameSize);

	uint32 value = genet_read(info, RBUF_CTRL);
	value |= RBUF_ALIGN_2B;
	value &= ~RBUF_64B_EN;
	genet_write(info, RBUF_CTRL, value);
	genet_write(info, RBUF_CHECK_CTRL, 0);
	genet_write(info, TBUF_CTRL, genet_read(info, TBUF_CTRL) & ~RBUF_64B_EN);
	genet_write(info, RBUF_TBUF_SIZE_CTRL, 1);

	value = genet_read(info, UMAC_CMD);
	genet_write(info, UMAC_CMD, value | UMAC_CMD_TXEN | UMAC_CMD_RXEN);

	// Interrupts
	genet_write(info, INTRL2_CPU_SET_MASK, 0xffffffff);
	genet_write(info, INTRL2_CPU_CLEAR, 0xffffffff);
	genet_write(info, INTRL2_CPU_CLEAR_MASK, IRQ_TXDMA_DONE | IRQ_RXDMA_DONE);
}


static int32
genet_interrupt(void* data)
{
	genet_info* info = (genet_info*)data;

	uint32 status = genet_read(info, INTRL2_CPU_STAT)
		& ~genet_read(info, INTRL2_CPU_STAT_MASK);
	if (status == 0)
		return B_UNHANDLED_INTERRUPT;
	genet_write(info, INTRL2_CPU_CLEAR, status);

	if ((status & IRQ_RXDMA_DONE) != 0)
		info->rxCondition.NotifyAll();
	if ((status & IRQ_TXDMA_DONE) != 0)
		info->txCondition.NotifyAll();

	return B_HANDLED_INTERRUPT;
}


// #pragma mark - data path


static status_t
genet_receive(genet_info* info, net_buffer** _buffer)
{
	MutexLocker locker(info->rxLock);

retry:
	uint32 producer;
	while (true) {
		producer = genet_read(info, RX_PROD_INDEX(kDefaultQueue))
			& kProdConsMask;
		if (producer != info->rxConsumer)
			break;
		if (info->closed)
			return B_FILE_ERROR;
		if (info->nonblocking)
			return B_WOULD_BLOCK;

		ConditionVariableEntry entry;
		info->rxCondition.Add(&entry);
		locker.Unlock();
		// Re-check after registering, to not miss an interrupt; the timeout
		// makes us robust against lost interrupts as well.
		producer = genet_read(info, RX_PROD_INDEX(kDefaultQueue))
			& kProdConsMask;
		if (producer == info->rxConsumer && !info->closed) {
			status_t status = entry.Wait(B_RELATIVE_TIMEOUT | B_CAN_INTERRUPT,
				100000);
			if (status != B_OK && status != B_TIMED_OUT)
				return status;
		}
		locker.Lock();
	}

	dma_read_barrier();

	uint32 index = info->rxConsumer & (kDescCount - 1);
	uint32 descStatus = genet_read(info, RX_DESC_STATUS(index));
	uint32 length = (descStatus & RX_DESC_BUFLEN_MASK) >> RX_DESC_BUFLEN_SHIFT;

	status_t result = B_OK;
	net_buffer* buffer = NULL;
	if ((descStatus & (RX_DESC_SOP | RX_DESC_EOP | RX_DESC_RX_ERROR))
			!= (RX_DESC_SOP | RX_DESC_EOP) || length <= 2
		|| length > kBufferSize) {
		TRACE("rx error, status %#" B_PRIx32 "\n", descStatus);
		result = B_IO_ERROR;
	} else {
		// The frame is preceded by 2 bytes of padding (RBUF_ALIGN_2B).
		buffer = sBufferModule->create(0);
		if (buffer == NULL
			|| sBufferModule->append(buffer,
				info->rxBuffers + index * kBufferSize + 2, length - 2) != B_OK) {
			if (buffer != NULL)
				sBufferModule->free(buffer);
			buffer = NULL;
			result = B_NO_MEMORY;
		}
	}

	info->rxConsumer = (info->rxConsumer + 1) & kProdConsMask;
	genet_write(info, RX_CONS_INDEX(kDefaultQueue), info->rxConsumer);

	if (result == B_IO_ERROR)
		goto retry;
	if (result != B_OK)
		return result;

	*_buffer = buffer;
	return B_OK;
}


static status_t
genet_send(genet_info* info, net_buffer* buffer)
{
	size_t size = buffer->size;
	if (size > kMaxFrameSize)
		return B_BAD_VALUE;

	MutexLocker locker(info->txLock);

	while (true) {
		info->txConsumer = genet_read(info, TX_CONS_INDEX(kDefaultQueue))
			& kProdConsMask;
		uint32 queued = (info->txProducer - info->txConsumer) & kProdConsMask;
		if (queued < kDescCount - 1)
			break;
		if (info->closed)
			return B_FILE_ERROR;
		if (info->nonblocking)
			return B_WOULD_BLOCK;

		ConditionVariableEntry entry;
		info->txCondition.Add(&entry);
		locker.Unlock();
		if (!info->closed) {
			status_t status = entry.Wait(
				B_RELATIVE_TIMEOUT | B_CAN_INTERRUPT, 100000);
			if (status != B_OK && status != B_TIMED_OUT)
				return status;
		}
		locker.Lock();
	}

	uint32 index = info->txProducer & (kDescCount - 1);
	uint8* data = info->txBuffers + index * kBufferSize;
	if (sBufferModule->read(buffer, 0, data, size) != B_OK)
		return B_BAD_DATA;

	// Pad runt frames; the MAC does not do it for us.
	if (size < 60) {
		memset(data + size, 0, 60 - size);
		size = 60;
	}

	phys_addr_t address = info->txPhysical + index * kBufferSize;
	genet_write(info, TX_DESC_ADDR_LO(index), (uint32)address);
	genet_write(info, TX_DESC_ADDR_HI(index), (uint32)(address >> 32));
	dma_write_barrier();
	genet_write(info, TX_DESC_STATUS(index), TX_DESC_QTAG_MASK | TX_DESC_SOP
		| TX_DESC_EOP | TX_DESC_CRC | (size << TX_DESC_BUFLEN_SHIFT));

	info->txProducer = (info->txProducer + 1) & kProdConsMask;
	genet_write(info, TX_PROD_INDEX(kDefaultQueue), info->txProducer);

	locker.Unlock();
	sBufferModule->free(buffer);
	return B_OK;
}


// #pragma mark - device hooks


static status_t
genet_init_device(void* _info, void** _cookie)
{
	*_cookie = _info;
	return B_OK;
}


static void
genet_uninit_device(void* _cookie)
{
}


static status_t
genet_open(void* _info, const char* path, int openMode, void** _cookie)
{
	genet_info* info = (genet_info*)_info;

	genet_handle* handle = (genet_handle*)malloc(sizeof(genet_handle));
	if (handle == NULL)
		return B_NO_MEMORY;
	handle->info = info;
	info->closed = false;
	info->nonblocking = (openMode & O_NONBLOCK) != 0;
	atomic_add(&info->openCount, 1);

	*_cookie = handle;
	return B_OK;
}


static status_t
genet_close(void* cookie)
{
	genet_handle* handle = (genet_handle*)cookie;
	genet_info* info = handle->info;

	// Wake up the stack's reader thread (and any blocked writer): they wait
	// for frames with timeouts and would otherwise never notice the close,
	// which makes "ifconfig down" hang on an idle network.
	info->closed = true;
	info->rxCondition.NotifyAll();
	info->txCondition.NotifyAll();
	return B_OK;
}


static status_t
genet_free(void* cookie)
{
	genet_handle* handle = (genet_handle*)cookie;
	atomic_add(&handle->info->openCount, -1);
	free(handle);
	return B_OK;
}


static status_t
genet_ioctl(void* cookie, uint32 op, void* buffer, size_t length)
{
	genet_handle* handle = (genet_handle*)cookie;
	genet_info* info = handle->info;

	switch (op) {
		case ETHER_GETADDR:
			return user_memcpy(buffer, &info->macAddress,
				sizeof(info->macAddress));

		case ETHER_INIT:
			return B_OK;

		case ETHER_GETFRAMESIZE:
		{
			uint32 frameSize = kMaxFrameSize;
			if (length != sizeof(frameSize))
				return B_BAD_VALUE;
			return user_memcpy(buffer, &frameSize, sizeof(frameSize));
		}

		case ETHER_SETPROMISC:
		{
			int32 value;
			if (length != sizeof(value)
				|| user_memcpy(&value, buffer, sizeof(value)) != B_OK) {
				return B_BAD_VALUE;
			}
			info->promiscuous = value != 0;
			genet_setup_rx_filter(info);
			return B_OK;
		}

		case ETHER_NONBLOCK:
		{
			int32 value;
			if (length != sizeof(value)
				|| user_memcpy(&value, buffer, sizeof(value)) != B_OK) {
				return B_BAD_VALUE;
			}
			info->nonblocking = value != 0;
			return B_OK;
		}

		case ETHER_ADDMULTI:
		case ETHER_REMMULTI:
			// We accept all multicast frames via promiscuous mode only for
			// now; the stack copes with unfiltered multicast.
			if (!info->promiscuous) {
				info->promiscuous = true;
				genet_setup_rx_filter(info);
			}
			return B_OK;

		case ETHER_SET_LINK_STATE_SEM:
			if (user_memcpy(&info->linkStateSem, buffer, sizeof(sem_id))
					!= B_OK) {
				info->linkStateSem = -1;
				return B_BAD_ADDRESS;
			}
			return B_OK;

		case ETHER_GET_LINK_STATE:
		{
			ether_link_state_t state;
			state.media = IFM_ETHER;
			if (info->linkUp) {
				state.media |= IFM_ACTIVE
					| (info->fullDuplex ? IFM_FULL_DUPLEX : IFM_HALF_DUPLEX);
				if (info->linkSpeed == 1000)
					state.media |= IFM_1000_T;
				else if (info->linkSpeed == 100)
					state.media |= IFM_100_TX;
				else
					state.media |= IFM_10_T;
			}
			state.speed = (uint64)info->linkSpeed * 1000000;
			state.quality = info->linkUp ? 1000 : 0;
			return user_memcpy(buffer, &state, sizeof(state));
		}

		case ETHER_SEND_NET_BUFFER:
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return genet_send(info, (net_buffer*)buffer);

		case ETHER_RECEIVE_NET_BUFFER:
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return genet_receive(info, (net_buffer**)buffer);

		default:
			break;
	}

	return B_DEV_INVALID_IOCTL;
}


// #pragma mark - driver module


struct genet_crs {
	phys_addr_t	address;
	size_t		length;
	uint32		irq;
};


static acpi_status
genet_parse_crs(ACPI_RESOURCE* resource, void* context)
{
	genet_crs* crs = (genet_crs*)context;
	switch (resource->Type) {
		case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
			if (crs->address == 0) {
				crs->address = resource->Data.FixedMemory32.Address;
				crs->length = resource->Data.FixedMemory32.AddressLength;
			}
			break;
		case ACPI_RESOURCE_TYPE_EXTENDED_IRQ:
			if (crs->irq == 0 && resource->Data.ExtendedIrq.InterruptCount > 0)
				crs->irq = resource->Data.ExtendedIrq.Interrupts[0];
			break;
		case ACPI_RESOURCE_TYPE_IRQ:
			if (crs->irq == 0 && resource->Data.Irq.InterruptCount > 0)
				crs->irq = resource->Data.Irq.Interrupts[0];
			break;
	}
	return B_OK;
}


static float
genet_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "acpi") != 0) {
		return 0.0f;
	}

	const char* hid;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid,
			false) != B_OK || strcmp(hid, "BCM6E4E") != 0) {
		return 0.0f;
	}

	return 0.8f;
}


static status_t
genet_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Broadcom GENET Ethernet" }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, GENET_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static void
genet_free_info(genet_info* info)
{
	mutex_destroy(&info->rxLock);
	mutex_destroy(&info->txLock);
	delete info;
}


static status_t
genet_init_driver(device_node* node, void** _cookie)
{
	genet_info* info = new(std::nothrow) genet_info;
	if (info == NULL)
		return B_NO_MEMORY;
	memset((void*)info, 0, sizeof(*info));
	new(&info->rxCondition) ConditionVariable;
	new(&info->txCondition) ConditionVariable;
	info->node = node;
	info->registerArea = -1;
	info->bufferArea = -1;
	info->linkThread = -1;
	info->linkStateSem = -1;
	info->phyAddress = -1;
	mutex_init(&info->rxLock, "genet rx");
	mutex_init(&info->txLock, "genet tx");
	info->rxCondition.Init(info, "genet rx");
	info->txCondition.Init(info, "genet tx");

	// Resources from ACPI
	device_node* acpiNode = sDeviceManager->get_parent_node(node);
	acpi_device_module_info* acpi;
	acpi_device device;
	sDeviceManager->get_driver(acpiNode, (driver_module_info**)&acpi,
		(void**)&device);
	sDeviceManager->put_node(acpiNode);

	genet_crs crs = {};
	status_t status = acpi->walk_resources(device, (ACPI_STRING)"_CRS",
		genet_parse_crs, &crs);
	if (status != B_OK || crs.address == 0 || crs.irq == 0) {
		ERROR("missing ACPI resources\n");
		genet_free_info(info);
		return B_DEVICE_NOT_FOUND;
	}
	info->irq = crs.irq;

	void* registers;
	info->registerArea = map_physical_memory("genet registers", crs.address,
		(crs.length + B_PAGE_SIZE - 1) & ~(size_t)(B_PAGE_SIZE - 1),
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&registers);
	if (info->registerArea < 0) {
		status = info->registerArea;
		genet_free_info(info);
		return status;
	}
	info->registers = (volatile uint32*)registers;

	uint32 revision = genet_read(info, SYS_REV_CTRL);
	uint32 major = (revision >> 24) & 0xf;
	INFO("registers at %#" B_PRIxPHYSADDR ", irq %" B_PRIu32 ", revision %#"
		B_PRIx32 "\n", crs.address, crs.irq, revision);
	if (major != 6) {
		ERROR("unsupported GENET version (major %" B_PRIu32 ")\n", major);
		delete_area(info->registerArea);
		genet_free_info(info);
		return B_NOT_SUPPORTED;
	}

	// The firmware leaves the MAC address in the UMAC registers.
	uint32 mac0 = genet_read(info, UMAC_MAC0);
	uint32 mac1 = genet_read(info, UMAC_MAC1);
	info->macAddress.ebyte[0] = mac0 >> 24;
	info->macAddress.ebyte[1] = mac0 >> 16;
	info->macAddress.ebyte[2] = mac0 >> 8;
	info->macAddress.ebyte[3] = mac0;
	info->macAddress.ebyte[4] = mac1 >> 8;
	info->macAddress.ebyte[5] = mac1;
	INFO("MAC address %02x:%02x:%02x:%02x:%02x:%02x\n",
		info->macAddress.ebyte[0], info->macAddress.ebyte[1],
		info->macAddress.ebyte[2], info->macAddress.ebyte[3],
		info->macAddress.ebyte[4], info->macAddress.ebyte[5]);

	status = genet_alloc_buffers(info);
	if (status != B_OK) {
		delete_area(info->registerArea);
		genet_free_info(info);
		return status;
	}

	genet_reset(info);
	genet_dma_disable(info);

	genet_write(info, SYS_PORT_CTRL, SYS_PORT_MODE_EXT_GPHY);
	genet_write(info, UMAC_MAC0, mac0);
	genet_write(info, UMAC_MAC1, mac1 & 0xffff);
	genet_setup_rx_filter(info);
	genet_init_rings(info);
	genet_enable(info);

	status = install_io_interrupt_handler(info->irq, genet_interrupt, info, 0);
	if (status != B_OK)
		ERROR("could not install interrupt handler, polling only\n");

	genet_phy_init(info);

	info->linkThread = spawn_kernel_thread(genet_link_thread, "genet link",
		B_LOW_PRIORITY, info);
	resume_thread(info->linkThread);

	*_cookie = info;
	return B_OK;
}


static void
genet_uninit_driver(void* _cookie)
{
	genet_info* info = (genet_info*)_cookie;

	info->stopLinkThread = true;
	status_t result;
	wait_for_thread(info->linkThread, &result);

	genet_write(info, UMAC_CMD, 0);
	genet_dma_disable(info);
	genet_write(info, INTRL2_CPU_SET_MASK, 0xffffffff);
	remove_io_interrupt_handler(info->irq, genet_interrupt, info);

	delete_area(info->bufferArea);
	delete_area(info->registerArea);
	genet_free_info(info);
}


static status_t
genet_register_child_devices(void* _cookie)
{
	genet_info* info = (genet_info*)_cookie;

	int32 id = sDeviceManager->create_id(GENET_DEVICE_ID_GENERATOR);
	if (id < 0)
		return id;

	char name[64];
	snprintf(name, sizeof(name), "net/genet/%" B_PRId32, id);

	status_t status = sDeviceManager->publish_device(info->node, name,
		GENET_DEVICE_MODULE_NAME);
	if (status != B_OK)
		sDeviceManager->free_id(GENET_DEVICE_ID_GENERATOR, id);
	return status;
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ NET_BUFFER_MODULE_NAME, (module_info**)&sBufferModule },
	{}
};


static struct device_module_info sGenetDevice = {
	{
		GENET_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	genet_init_device,
	genet_uninit_device,
	NULL,	// remove

	genet_open,
	genet_close,
	genet_free,
	NULL,	// read
	NULL,	// write
	NULL,	// io
	genet_ioctl,

	NULL,	// select
	NULL,	// deselect
};


static struct driver_module_info sGenetDriver = {
	{
		GENET_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	genet_supports_device,
	genet_register_device,
	genet_init_driver,
	genet_uninit_driver,
	genet_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


module_info* modules[] = {
	(module_info*)&sGenetDriver,
	(module_info*)&sGenetDevice,
	NULL
};
