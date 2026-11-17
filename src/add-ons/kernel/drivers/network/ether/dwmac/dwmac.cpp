/*
 * Copyright 2023-2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 *
 * Synopsys DesignWare Ethernet QoS controller (dwmac 5.20) of the StarFive
 * JH7110, as on the Milk-V Mars and the VisionFive 2, with an RGMII PHY.
 *
 * The controller setup (MTL, MAC and DMA programming, descriptor rings and
 * MDIO) comes from the DWMAC driver of X547's device_manager2 branch. The
 * device manager glue, the net_buffer data path and the link polling follow
 * the genet driver. The JH7110 glue (interface mode in the AON syscon, TX
 * clock source) and the Motorcomm YT8531 setup (RGMII delays, TX clock
 * inversion, pad drive strength) follow the device tree bindings, with the
 * register values checked against the U-Boot drivers dwc_eth_qos_starfive.c
 * and phy/motorcomm.c.
 */


#include <net/if_media.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <bus/FDT.h>
#include <condition_variable.h>
#include <device_manager.h>
#include <ethernet.h>
#include <fdt_clock_reset.h>
#include <kernel.h>
#include <lock.h>
#include <net_buffer.h>
#include <util/AutoLock.h>

#include "ether_driver.h"

#include "DwmacRegs.h"


#define DWMAC_DRIVER_MODULE_NAME	"drivers/network/dwmac/driver_v1"
#define DWMAC_DEVICE_MODULE_NAME	"drivers/network/dwmac/device_v1"
#define DWMAC_DEVICE_ID_GENERATOR	"dwmac/device_id"

#define INFO(x...)	dprintf("dwmac: " x)
#define ERROR(x...)	dprintf("dwmac: " x)
//#define TRACE_DWMAC
#ifdef TRACE_DWMAC
#	define TRACE(x...) dprintf("dwmac: " x)
#else
#	define TRACE(x...) do { if (false) dprintf("dwmac: " x); } while (false)
#endif


static const char* const kCompatible[] = {
	"starfive,jh7110-dwmac",
	"snps,dwmac-5.20",
};


// descriptor rings and buffers
static const uint32 kDescCount = 64;
static const uint32 kDescSize = 32;
	// 16 bytes of descriptor, padded to the bus width (dsl in 64 bit units)
static const uint32 kBufferSize = 1568;
	// a multiple of the bus width, room for a VLAN tagged frame
static const uint32 kMaxFrameSize = 1514;
static const uint32 kAxiBusWidth = 8;

// descriptor bits (the read and write-back formats share these)
#define DES2_TX_IOC				(1u << 31)
#define DES3_RX_ES				(1u << 15)
#define DES3_RX_BUF1V			(1u << 24)
#define DES3_RX_IOC				(1u << 30)
#define DES3_LD					(1u << 28)
#define DES3_FD					(1u << 29)
#define DES3_OWN				(1u << 31)
#define DES3_LENGTH_MASK		0x7fff

#define MAC_VERSION				0x110
#define MAC_PACKET_FILTER_PR	(1u << 0)
#define MAC_PACKET_FILTER_PM	(1u << 4)

// IEEE 802.3 clause 22 PHY registers
#define MII_BMCR				0x00
#define MII_BMSR				0x01
#define MII_PHYIDR1				0x02
#define MII_PHYIDR2				0x03
#define MII_ANAR				0x04
#define MII_ANLPAR				0x05
#define MII_100T2CR				0x09
#define MII_100T2SR				0x0a
#define BMCR_RESET				(1u << 15)
#define BMCR_AUTOEN				(1u << 12)
#define BMCR_STARTNEG			(1u << 9)
#define BMSR_LINK				(1u << 2)
#define BMSR_ACOMP				(1u << 5)
#define ANAR_10					(1u << 5)
#define ANAR_10_FD				(1u << 6)
#define ANAR_TX					(1u << 7)
#define ANAR_TX_FD				(1u << 8)
#define ANAR_PAUSE_SYM			(1u << 10)
#define ANAR_PAUSE_ASYM			(1u << 11)
#define ANAR_CSMA				0x0001
#define GTCR_ADV_1000THDX		(1u << 8)
#define GTCR_ADV_1000TFDX		(1u << 9)
#define GTSR_LP_1000THDX		(1u << 10)
#define GTSR_LP_1000TFDX		(1u << 11)

// Motorcomm YT8531: extended registers behind an address/data pair
#define YT8531_PHY_ID				0x4f51e91b
#define YTPHY_PAGE_SELECT			0x1e
#define YTPHY_PAGE_DATA				0x1f
#define YT8531_CHIP_CONFIG			0xa001
#define YT8531_CCR_RXC_DLY_EN		(1u << 8)
#define YT8531_CCR_LDO_SHIFT		4
#define YT8531_CCR_LDO_MASK			(3u << 4)
#define YT8531_CCR_LDO_1V8			2
#define YT8531_RGMII_CONFIG1		0xa003
#define YT8531_RC1R_TX_CLK_INVERTED	(1u << 14)
#define YT8531_RC1R_RX_DELAY_SHIFT	10
#define YT8531_RC1R_RX_DELAY_MASK	(0xfu << 10)
#define YT8531_RC1R_GE_TX_DELAY_MASK	0xfu
#define YT8531_PAD_DRIVE_STRENGTH	0xa010
#define YT8531_RXC_DS_SHIFT			13
#define YT8531_RXC_DS_MASK			(7u << 13)
#define YT8531_RXD_DS_HI			(1u << 12)
#define YT8531_RXD_DS_LOW_SHIFT		4
#define YT8531_RXD_DS_LOW_MASK		(3u << 4)
#define YT8531_CLOCK_GATING			0x000c
#define YT8531_CGR_RX_CLK_EN		(1u << 12)
	// set: RX clock gated while there is no link
#define YT8531_SLEEP_CONTROL1		0x0027
#define YT8531_ESC1R_SLEEP_SW		(1u << 15)
#define YT8531_DELAY_STEP_PS		150
#define YT8531_RXC_DELAY_PS			1900

// JH7110 glue
#define JH7110_PHY_INTF_RGMII		1
#define JH7110_PHY_INTF_MASK		7
#define JH7110_AONCRG_BASE			0x17000000
#define JH7110_AONCLK_GMAC0_TX_REG	(5 * 4)
#define JH7110_CLK_MUX_SHIFT		24
#define JH7110_AONCLK_TX_FROM_RMII_RTX	1


struct dwmac_info {
	device_node*		node;

	area_id				registerArea;
	DwmacRegs volatile*	regs;
	uint32				irq;
	bool				interruptInstalled;

	area_id				dmaArea;
	uint8*				dmaBase;
	phys_addr_t			dmaPhysical;
	DwmacDesc*			txDescs;
	DwmacDesc*			rxDescs;
	uint8*				txBuffers;
	uint8*				rxBuffers;
	phys_addr_t			txDescsPhysical;
	phys_addr_t			rxDescsPhysical;
	phys_addr_t			txBuffersPhysical;
	phys_addr_t			rxBuffersPhysical;

	ether_address_t		macAddress;

	// PHY
	int					phyAddress;
	uint32				phyId;
	bool				txClockAdjust;
	bool				txClockInverted[3];
		// 10, 100, 1000 Mbit/s

	// RX
	mutex				rxLock;
	ConditionVariable	rxCondition;
	uint32				rxIndex;

	// TX
	mutex				txLock;
	ConditionVariable	txCondition;
	uint32				txIndex;

	// link
	thread_id			linkThread;
	volatile bool		stopLinkThread;
	bool				linkUp;
	uint32				linkSpeed;		// in Mbit/s
	bool				fullDuplex;
	sem_id				linkStateSem;

	bool				nonblocking;
	int32				openCount;
	volatile bool		closed;
};


struct dwmac_handle {
	dwmac_info*			info;
};


static device_manager_info* sDeviceManager;
static net_buffer_module_info* sBufferModule;


// #pragma mark - helpers


template<typename Cond> static status_t
wait_for_cond(Cond cond, int32 attempts, bigtime_t retryInterval)
{
	for (; attempts > 0; attempts--) {
		if (cond())
			return B_OK;
		snooze(retryInterval);
	}
	return B_TIMED_OUT;
}


static fdt_device_module_info*
get_fdt(device_node* node, fdt_device** _dev)
{
	fdt_device_module_info* module;
	if (sDeviceManager->get_driver(node, (driver_module_info**)&module,
			(void**)_dev) != B_OK) {
		return NULL;
	}
	return module;
}


static bool
is_compatible(device_node* node)
{
	const char* compatible;
	if (sDeviceManager->get_attr_string(node, "fdt/compatible", &compatible,
			false) != B_OK) {
		return false;
	}
	for (size_t i = 0; i < B_COUNT_OF(kCompatible); i++) {
		if (strcmp(compatible, kCompatible[i]) == 0)
			return true;
	}
	return false;
}


static bool
get_prop_u32(fdt_device_module_info* module, fdt_device* dev,
	const char* name, uint32* value, uint32 index = 0)
{
	int length;
	const uint32* prop = (const uint32*)module->get_prop(dev, name, &length);
	if (prop == NULL || length < (int)(4 * (index + 1)))
		return false;
	*value = B_BENDIAN_TO_HOST_INT32(prop[index]);
	return true;
}


static bool
has_prop(fdt_device_module_info* module, fdt_device* dev, const char* name)
{
	int length;
	return module->get_prop(dev, name, &length) != NULL;
}


/*!	Device tree node a phandle property of \a dev points to. The returned
	node must be put by the caller.
*/
static device_node*
get_phandle_node(fdt_device_module_info* module, fdt_device* dev,
	const char* name)
{
	uint32 phandle;
	if (!get_prop_u32(module, dev, name, &phandle))
		return NULL;

	device_node* busNode = module->get_bus(dev);
	fdt_bus_module_info* busModule;
	fdt_bus* bus;
	if (sDeviceManager->get_driver(busNode, (driver_module_info**)&busModule,
			(void**)&bus) != B_OK) {
		return NULL;
	}
	return busModule->node_by_phandle(bus, phandle);
}


/*!	Read-modify-write of one physical register. Returns the value read back.
*/
static uint32
update_physical_reg(phys_addr_t address, uint32 mask, uint32 bits,
	uint32* _before = NULL)
{
	phys_addr_t pageBase = address & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	void* virtualBase;
	area_id area = map_physical_memory("dwmac glue", pageBase, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&virtualBase);
	if (area < 0)
		return 0;

	vuint32* reg = (vuint32*)((addr_t)virtualBase + (address - pageBase));
	uint32 value = *reg;
	if (_before != NULL)
		*_before = value;
	if (mask != 0)
		*reg = (value & ~mask) | (bits & mask);
	value = *reg;
	delete_area(area);
	return value;
}


// #pragma mark - MDIO / PHY


static DwmacMdioAddrCr
mdio_clock_range()
{
	// the CSR clock of the JH7110 GMAC (AHB) runs at 250-300 MHz
	return DwmacMdioAddrCr::cr250_300;
}


static status_t
dwmac_mdio_wait(dwmac_info* info)
{
	return wait_for_cond([info]() { return !info->regs->mac.mdioAddr.gb; },
		10000, 10);
}


static int
dwmac_mdio_read(dwmac_info* info, int phy, int reg)
{
	if (dwmac_mdio_wait(info) != B_OK)
		return -1;

	DwmacMdioAddr mdioAddr {.val = info->regs->mac.mdioAddr.val};
	mdioAddr.val &= DwmacMdioAddr{.c45e = true, .skap = true}.val;
	mdioAddr.pa = phy;
	mdioAddr.rda = reg;
	mdioAddr.cr = mdio_clock_range();
	mdioAddr.goc = DwmacMdioAddrGoc::read;
	mdioAddr.gb = true;
	info->regs->mac.mdioAddr.val = mdioAddr.val;

	if (dwmac_mdio_wait(info) != B_OK)
		return -1;
	return info->regs->mac.mdioData.gd;
}


static status_t
dwmac_mdio_write(dwmac_info* info, int phy, int reg, uint16 value)
{
	status_t status = dwmac_mdio_wait(info);
	if (status != B_OK)
		return status;

	info->regs->mac.mdioData.val = value;

	DwmacMdioAddr mdioAddr {.val = info->regs->mac.mdioAddr.val};
	mdioAddr.val &= DwmacMdioAddr{.c45e = true, .skap = true}.val;
	mdioAddr.pa = phy;
	mdioAddr.rda = reg;
	mdioAddr.cr = mdio_clock_range();
	mdioAddr.goc = DwmacMdioAddrGoc::write;
	mdioAddr.gb = true;
	info->regs->mac.mdioAddr.val = mdioAddr.val;

	return dwmac_mdio_wait(info);
}


static int
ytphy_read_ext(dwmac_info* info, uint16 reg)
{
	if (dwmac_mdio_write(info, info->phyAddress, YTPHY_PAGE_SELECT, reg)
			!= B_OK) {
		return -1;
	}
	return dwmac_mdio_read(info, info->phyAddress, YTPHY_PAGE_DATA);
}


static int
ytphy_modify_ext(dwmac_info* info, uint16 reg, uint16 mask, uint16 bits)
{
	int value = ytphy_read_ext(info, reg);
	if (value < 0)
		return value;
	value = (value & ~mask) | (bits & mask);
	if (dwmac_mdio_write(info, info->phyAddress, YTPHY_PAGE_DATA, value)
			!= B_OK) {
		return -1;
	}
	return ytphy_read_ext(info, reg);
}


/*!	Drive strength code of the YT8531 for a pad current in microampere, at
	1.8 V I/O (the only supply the Mars and the VisionFive 2 use).
*/
static int
yt8531_drive_strength(uint32 microAmpere)
{
	static const uint32 kCurrents1V8[] = {
		1200, 2100, 2700, 2910, 3110, 3600, 3970, 4350
	};
	for (size_t i = 0; i < B_COUNT_OF(kCurrents1V8); i++) {
		if (kCurrents1V8[i] == microAmpere)
			return i;
	}
	return -1;
}


/*!	RGMII delays and pad drive strength of the Motorcomm YT8531, from the
	PHY node ("rx-internal-delay-ps", "tx-internal-delay-ps",
	"motorcomm,rx-clk-drv-microamp", "motorcomm,rx-data-drv-microamp").
*/
static void
yt8531_init(dwmac_info* info, fdt_device_module_info* module, fdt_device* phy)
{
	uint32 rxDelay = 1950, txDelay = 1950;
	if (phy != NULL) {
		get_prop_u32(module, phy, "rx-internal-delay-ps", &rxDelay);
		get_prop_u32(module, phy, "tx-internal-delay-ps", &txDelay);
	}

	uint16 rxClockDelay = 0;
	if (rxDelay >= YT8531_RXC_DELAY_PS) {
		rxClockDelay = YT8531_CCR_RXC_DLY_EN;
		rxDelay -= YT8531_RXC_DELAY_PS;
	}
	uint16 rxSteps = min_c(rxDelay / YT8531_DELAY_STEP_PS, 15);
	uint16 txSteps = min_c(txDelay / YT8531_DELAY_STEP_PS, 15);

	int chip = ytphy_modify_ext(info, YT8531_CHIP_CONFIG,
		YT8531_CCR_RXC_DLY_EN, rxClockDelay);
	int rgmii = ytphy_modify_ext(info, YT8531_RGMII_CONFIG1,
		YT8531_RC1R_RX_DELAY_MASK | YT8531_RC1R_GE_TX_DELAY_MASK,
		(rxSteps << YT8531_RC1R_RX_DELAY_SHIFT) | txSteps);
	INFO("YT8531: rx delay %s+%u steps, tx delay %u steps: chip config %#x, "
		"rgmii config %#x\n", rxClockDelay != 0 ? "1.9 ns " : "", rxSteps,
		txSteps, chip, rgmii);

	if (phy == NULL || chip < 0)
		return;

	if (((chip & YT8531_CCR_LDO_MASK) >> YT8531_CCR_LDO_SHIFT)
			!= YT8531_CCR_LDO_1V8) {
		INFO("YT8531: I/O not at 1.8 V, drive strength left as is\n");
	} else {
		uint32 current;
		if (get_prop_u32(module, phy, "motorcomm,rx-clk-drv-microamp",
				&current)) {
			int ds = yt8531_drive_strength(current);
			if (ds >= 0) {
				ytphy_modify_ext(info, YT8531_PAD_DRIVE_STRENGTH,
					YT8531_RXC_DS_MASK, ds << YT8531_RXC_DS_SHIFT);
			}
		}
		if (get_prop_u32(module, phy, "motorcomm,rx-data-drv-microamp",
				&current)) {
			int ds = yt8531_drive_strength(current);
			if (ds >= 0) {
				ytphy_modify_ext(info, YT8531_PAD_DRIVE_STRENGTH,
					YT8531_RXD_DS_HI | YT8531_RXD_DS_LOW_MASK,
					((ds & 4) != 0 ? YT8531_RXD_DS_HI : 0)
						| ((ds & 3) << YT8531_RXD_DS_LOW_SHIFT));
			}
		}
		INFO("YT8531: pad drive strength %#x\n",
			ytphy_read_ext(info, YT8531_PAD_DRIVE_STRENGTH));
	}

	info->txClockAdjust = has_prop(module, phy, "motorcomm,tx-clk-adj-enabled");
	info->txClockInverted[0] = has_prop(module, phy,
		"motorcomm,tx-clk-10-inverted");
	info->txClockInverted[1] = has_prop(module, phy,
		"motorcomm,tx-clk-100-inverted");
	info->txClockInverted[2] = has_prop(module, phy,
		"motorcomm,tx-clk-1000-inverted");
}


/*!	Finds the PHY: the "reg" of the "phy-handle" node, else the first address
	that answers on MDIO. Returns the PHY's device tree node, if any.
*/
static fdt_device*
dwmac_phy_find(dwmac_info* info, fdt_device_module_info* module,
	fdt_device* dev, fdt_device_module_info** _phyModule)
{
	fdt_device* phy = NULL;
	fdt_device_module_info* phyModule = NULL;
	// node_by_phandle() does not acquire a reference: nothing to put
	device_node* phyNode = get_phandle_node(module, dev, "phy-handle");
	if (phyNode != NULL) {
		phyModule = get_fdt(phyNode, &phy);
		uint32 address;
		if (phyModule != NULL
			&& get_prop_u32(phyModule, phy, "reg", &address)) {
			info->phyAddress = address;
		}
	}
	INFO("phy-handle %s, PHY address %d\n",
		phy != NULL ? "found" : "not found", info->phyAddress);

	if (info->phyAddress < 0) {
		for (int address = 0; address < 32; address++) {
			int id1 = dwmac_mdio_read(info, address, MII_PHYIDR1);
			if (id1 > 0 && id1 != 0xffff) {
				info->phyAddress = address;
				break;
			}
		}
	}
	if (info->phyAddress < 0) {
		ERROR("no PHY found\n");
		return NULL;
	}

	int id1 = dwmac_mdio_read(info, info->phyAddress, MII_PHYIDR1);
	int id2 = dwmac_mdio_read(info, info->phyAddress, MII_PHYIDR2);
	info->phyId = ((uint32)id1 << 16) | (uint16)id2;
	INFO("PHY at %d: id %#" B_PRIx32 ", bmcr %#x, bmsr %#x\n",
		info->phyAddress, info->phyId,
		dwmac_mdio_read(info, info->phyAddress, MII_BMCR),
		dwmac_mdio_read(info, info->phyAddress, MII_BMSR));

	*_phyModule = phyModule;
	return phy;
}


/*!	The MAC's software reset only completes with the RX clock of the PHY
	running. The YT8531 stops it while no cable is plugged in, and may go to
	sleep: keep the clock on and the PHY awake.
*/
static void
yt8531_keep_rx_clock(dwmac_info* info)
{
	int gating = ytphy_modify_ext(info, YT8531_CLOCK_GATING,
		YT8531_CGR_RX_CLK_EN, 0);
	int sleep = ytphy_modify_ext(info, YT8531_SLEEP_CONTROL1,
		YT8531_ESC1R_SLEEP_SW, 0);
	INFO("YT8531: rx clock always on (clock gating %#x), auto sleep off "
		"(sleep control %#x)\n", gating, sleep);
}


static void
dwmac_phy_init(dwmac_info* info, fdt_device_module_info* phyModule,
	fdt_device* phy)
{
	if (info->phyAddress < 0)
		return;

	if (info->phyId == YT8531_PHY_ID)
		yt8531_init(info, phyModule, phy);

	// advertise everything and restart autonegotiation
	int phyAddress = info->phyAddress;
	dwmac_mdio_write(info, phyAddress, MII_ANAR, ANAR_CSMA | ANAR_10
		| ANAR_10_FD | ANAR_TX | ANAR_TX_FD | ANAR_PAUSE_SYM
		| ANAR_PAUSE_ASYM);
	dwmac_mdio_write(info, phyAddress, MII_100T2CR,
		GTCR_ADV_1000TFDX | GTCR_ADV_1000THDX);
	dwmac_mdio_write(info, phyAddress, MII_BMCR, BMCR_AUTOEN | BMCR_STARTNEG);
}


static void
dwmac_set_speed(dwmac_info* info, uint32 speed, bool fullDuplex)
{
	DwmacMacConfig config {.val = info->regs->mac.config.val};
	switch (speed) {
		case 10:
			config.ps = true;
			config.fes = false;
			break;
		case 100:
			config.ps = true;
			config.fes = true;
			break;
		default:
			config.ps = false;
			config.fes = false;
			break;
	}
	config.dm = fullDuplex;
	info->regs->mac.config.val = config.val;
	if (!fullDuplex)
		info->regs->mtl.chan[0].txOpMode.ftq = true;

	if (info->phyId == YT8531_PHY_ID && info->txClockAdjust) {
		int index = speed == 10 ? 0 : speed == 100 ? 1 : 2;
		ytphy_modify_ext(info, YT8531_RGMII_CONFIG1,
			YT8531_RC1R_TX_CLK_INVERTED,
			info->txClockInverted[index] ? YT8531_RC1R_TX_CLK_INVERTED : 0);
	}
}


static void
dwmac_update_link(dwmac_info* info)
{
	if (info->phyAddress < 0)
		return;
	int phy = info->phyAddress;

	// BMSR link status is latched low; read twice
	dwmac_mdio_read(info, phy, MII_BMSR);
	int bmsr = dwmac_mdio_read(info, phy, MII_BMSR);
	bool up = bmsr >= 0 && (bmsr & BMSR_LINK) != 0
		&& (bmsr & BMSR_ACOMP) != 0;

	uint32 speed = 0;
	bool fullDuplex = true;
	if (up) {
		int gtcr = dwmac_mdio_read(info, phy, MII_100T2CR);
		int gtsr = dwmac_mdio_read(info, phy, MII_100T2SR);
		int anar = dwmac_mdio_read(info, phy, MII_ANAR);
		int anlpar = dwmac_mdio_read(info, phy, MII_ANLPAR);
		int common = anar & anlpar;
		if ((gtcr & GTCR_ADV_1000TFDX) && (gtsr & GTSR_LP_1000TFDX)) {
			speed = 1000;
		} else if ((gtcr & GTCR_ADV_1000THDX) && (gtsr & GTSR_LP_1000THDX)) {
			speed = 1000;
			fullDuplex = false;
		} else if (common & ANAR_TX_FD) {
			speed = 100;
		} else if (common & ANAR_TX) {
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
		dwmac_set_speed(info, speed, fullDuplex);
		INFO("link up, %" B_PRIu32 " Mbit/s, %s duplex (mac config %#"
			B_PRIx32 ")\n", speed, fullDuplex ? "full" : "half",
			info->regs->mac.config.val);
	} else
		INFO("link down\n");

	if (info->linkStateSem >= 0)
		release_sem_etc(info->linkStateSem, 1, B_DO_NOT_RESCHEDULE);
}


static status_t
dwmac_link_thread(void* data)
{
	dwmac_info* info = (dwmac_info*)data;
	int32 rounds = 0;
	while (!info->stopLinkThread) {
		dwmac_update_link(info);
		if (++rounds == 30 && !info->linkUp) {
			// a hint in the syslog when no link comes after half a minute
			INFO("still no link: bmsr %#x, dma status %#" B_PRIx32 "\n",
				dwmac_mdio_read(info, info->phyAddress, MII_BMSR),
				info->regs->dma.channels[0].status.val);
		}
		snooze(1000000);
	}
	return B_OK;
}


// #pragma mark - controller setup


static void
dwmac_configure_mtl(dwmac_info* info, uint32& tqs)
{
	DwmacRegs volatile* regs = info->regs;

	// store and forward in both directions, all FIFO RAM to queue 0
	regs->mtl.chan[0].txOpMode.tsf = true;
	regs->mtl.chan[0].txOpMode.txqen = DwmacMtlTxOpModeTxqen::enabled;
	regs->mtl.chan[0].txqWeight = 0x10;
	regs->mtl.chan[0].rxOpMode.rsf = true;

	// FIFO sizes are encoded as log2(n / 128), queue sizes as n / 256 - 1
	uint32 txFifoSize = regs->mac.hwFeature1.txFifoSize;
	uint32 rxFifoSize = regs->mac.hwFeature1.rxFifoSize;
	tqs = (128 << txFifoSize) / 256 - 1;
	uint32 rqs = (128 << rxFifoSize) / 256 - 1;
	regs->mtl.chan[0].txOpMode.tqs = tqs;
	regs->mtl.chan[0].rxOpMode.rqs = rqs;

	// flow control only with at least 4 KB of RX FIFO
	if (rqs >= 4096 / 256 - 1) {
		uint32 rfd, rfa;
		regs->mtl.chan[0].rxOpMode.val |= DwmacMtlRxOpMode{.ehfc = true}.val;
		if (rqs == 4096 / 256 - 1) {
			rfd = 0x3;	// full - 3 KB
			rfa = 0x1;	// full - 1.5 KB
		} else if (rqs == 8192 / 256 - 1) {
			rfd = 0x6;	// full - 4 KB
			rfa = 0xa;	// full - 6 KB
		} else if (rqs == 16384 / 256 - 1) {
			rfd = 0x6;	// full - 4 KB
			rfa = 0x12;	// full - 10 KB
		} else {
			rfd = 0x6;	// full - 4 KB
			rfa = 0x1e;	// full - 16 KB
		}
		regs->mtl.chan[0].rxOpMode.rfd = rfd;
		regs->mtl.chan[0].rxOpMode.rfa = rfa;
	}

	INFO("FIFO tx %u, rx %u bytes\n", 128u << txFifoSize,
		128u << rxFifoSize);
}


static void
dwmac_configure_mac(dwmac_info* info)
{
	DwmacRegs volatile* regs = info->regs;

	regs->mac.rxqCtrl0.rxq0en = DwmacRxqCtrl0Rxq0en::enabledDcb;
	// multicast and broadcast to queue 0
	regs->mac.rxqCtrl1 |= 0x00100000;
	// promiscuous, as X547's driver: the perfect filter is not exercised yet
	regs->mac.packetFilter = MAC_PACKET_FILTER_PR | MAC_PACKET_FILTER_PM;

	// flow control
	regs->mac.qxTxFlowCtrl[0].pt = 0xffff;
	regs->mac.txqPrtyMap0.pstq0 = 0;
	regs->mac.rxqCtrl2.psrq0 = 0;
	regs->mac.qxTxFlowCtrl[0].tfe = true;
	regs->mac.rxFlowCtrl.rfe = true;

	DwmacMacConfig config {.val = regs->mac.config.val};
	config.gpslce = false;
	config.wd = false;
	config.jd = false;
	config.je = false;
	config.cst = true;
	config.acs = true;
	regs->mac.config.val = config.val;

	const uint8* mac = info->macAddress.ebyte;
	regs->mac.addr[0].hi = (mac[5] << 8) | mac[4];
	regs->mac.addr[0].lo = (mac[3] << 24) | (mac[2] << 16) | (mac[1] << 8)
		| mac[0];
}


static void
dwmac_init_rx_desc(dwmac_info* info, uint32 index)
{
	DwmacDesc* desc = (DwmacDesc*)((uint8*)info->rxDescs + index * kDescSize);
	phys_addr_t address = info->rxBuffersPhysical + index * kBufferSize;
	desc->des0 = (uint32)address;
	desc->des1 = (uint32)(address >> 32);
	desc->des2 = 0;
	memory_full_barrier();
	desc->des3.val = DES3_OWN | DES3_RX_IOC | DES3_RX_BUF1V;
}


static void
dwmac_configure_dma(dwmac_info* info, uint32 tqs)
{
	DwmacRegs volatile* regs = info->regs;
	auto& channel = regs->dma.channels[0];

	channel.txControl.osp = true;
	channel.rxControl.rbsz = kBufferSize;
	channel.control.dsl = (kDescSize - sizeof(DwmacDesc)) / kAxiBusWidth;

	// burst length below half the FIFO
	uint32 pbl = min_c(tqs + 1, 32);
	channel.txControl.txpbl = pbl;
	channel.rxControl.rxpbl = 8;

	regs->dma.sysBusMode.val = DwmacDmaSysBusMode {
		.blen4 = true,
		.blen8 = true,
		.blen16 = true,
		.eame = true,
		.rdOsrLmt = 2,
	}.val;

	memset(info->txDescs, 0, kDescCount * kDescSize);
	for (uint32 i = 0; i < kDescCount; i++)
		dwmac_init_rx_desc(info, i);
	memory_full_barrier();

	channel.txBaseAddrHi = (uint32)(info->txDescsPhysical >> 32);
	channel.txBaseAddrLo = (uint32)info->txDescsPhysical;
	channel.txRingLen = kDescCount - 1;
	channel.rxBaseAddrHi = (uint32)(info->rxDescsPhysical >> 32);
	channel.rxBaseAddrLo = (uint32)info->rxDescsPhysical;
	channel.rxRingLen = kDescCount - 1;

	channel.txControl.st = true;
	channel.rxControl.sr = true;
	regs->mac.config.re = true;
	regs->mac.config.te = true;

	// The RX tail pointer points at the last descriptor: pointing at the
	// first would look like an empty ring. The TX tail is written on send.
	channel.rxEndAddr = info->rxDescsPhysical + (kDescCount - 1) * kDescSize;

	// interrupts for completed receives and transmits (normal summary)
	channel.intrEna.val = DwmacDmaChannelIntrEna {
		.tie = true,
		.rie = true,
		.nie = true,
	}.val;
	channel.status.val = channel.status.val;
}


static status_t
dwmac_alloc_buffers(dwmac_info* info)
{
	size_t descSize = 2 * kDescCount * kDescSize;
	size_t size = ROUNDUP(descSize + 2 * kDescCount * kBufferSize,
		B_PAGE_SIZE);
	void* address;
	info->dmaArea = create_area("dwmac dma", &address, B_ANY_KERNEL_ADDRESS,
		size, B_32_BIT_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (info->dmaArea < 0)
		return info->dmaArea;

	physical_entry entry;
	status_t status = get_memory_map(address, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK) {
		delete_area(info->dmaArea);
		info->dmaArea = -1;
		return status;
	}
	memset(address, 0, size);

	uint8* base = (uint8*)address;
	info->dmaBase = base;
	info->dmaPhysical = entry.address;
	info->txDescs = (DwmacDesc*)base;
	info->rxDescs = (DwmacDesc*)(base + kDescCount * kDescSize);
	info->txBuffers = base + descSize;
	info->rxBuffers = base + descSize + kDescCount * kBufferSize;
	info->txDescsPhysical = entry.address;
	info->rxDescsPhysical = entry.address + kDescCount * kDescSize;
	info->txBuffersPhysical = entry.address + descSize;
	info->rxBuffersPhysical = entry.address + descSize
		+ kDescCount * kBufferSize;
	TRACE("DMA memory at %#" B_PRIxPHYSADDR "\n", entry.address);
	return B_OK;
}


/*!	JH7110 glue: RGMII in the interface select field of the syscon named by
	"starfive,syscon" (phandle, offset, shift), and the TX clock taken from
	the RGMII clock when the board asks for it ("starfive,tx-use-rgmii-clk":
	the assigned parent of the TX clock in the device tree).
*/
static void
dwmac_jh7110_glue(fdt_device_module_info* module, fdt_device* dev)
{
	uint32 offset, shift;
	device_node* sysconNode = get_phandle_node(module, dev, "starfive,syscon");
	if (sysconNode != NULL && get_prop_u32(module, dev, "starfive,syscon",
			&offset, 1) && get_prop_u32(module, dev, "starfive,syscon",
			&shift, 2)) {
		fdt_device* syscon;
		fdt_device_module_info* sysconModule = get_fdt(sysconNode, &syscon);
		uint64 base, length;
		if (sysconModule != NULL
			&& sysconModule->get_reg(syscon, 0, &base, &length)) {
			uint32 before;
			uint32 after = update_physical_reg(base + offset,
				JH7110_PHY_INTF_MASK << shift, JH7110_PHY_INTF_RGMII << shift,
				&before);
			INFO("interface select (syscon %#" B_PRIx64 " + %#" B_PRIx32
				", shift %" B_PRIu32 "): %#" B_PRIx32 " -> %#" B_PRIx32 "\n",
				base, offset, shift, before, after);
		}
	} else
		ERROR("no \"starfive,syscon\", interface mode left as is\n");
	// node_by_phandle() does not acquire a reference: nothing to put

	if (has_prop(module, dev, "starfive,tx-use-rgmii-clk")) {
		uint32 before;
		uint32 after = update_physical_reg(
			JH7110_AONCRG_BASE + JH7110_AONCLK_GMAC0_TX_REG,
			1u << JH7110_CLK_MUX_SHIFT,
			JH7110_AONCLK_TX_FROM_RMII_RTX << JH7110_CLK_MUX_SHIFT, &before);
		INFO("tx clock from the RGMII clock: %#" B_PRIx32 " -> %#" B_PRIx32
			"\n", before, after);
	}
}


/*!	Brings the controller up as U-Boot and Linux do: resets asserted, clocks
	on, JH7110 glue (interface mode, TX clock source), resets released.
	Fails, before any access to the controller, if a clock or reset cannot
	be handled: an unclocked controller can stall the bus.
*/
static status_t
dwmac_clocks_and_resets(device_node* fdtNode, fdt_device_module_info* module,
	fdt_device* dev)
{
	fdt_clock_reset_module_info* clocks;
	if (get_module(JH7110_CLOCK_RESET_MODULE_NAME, (module_info**)&clocks)
			!= B_OK) {
		ERROR("no clock/reset driver, not touching the controller\n");
		return B_NOT_SUPPORTED;
	}

	static const char* const kClocks[] = {
		"stmmaceth", "pclk", "ptp_ref", "tx", "gtx"
	};
	static const char* const kResets[] = { "stmmaceth", "ahb" };
	fdt_clock clockHandles[B_COUNT_OF(kClocks)];
	fdt_reset resetHandles[B_COUNT_OF(kResets)];

	status_t status = B_OK;
	for (size_t i = 0; i < B_COUNT_OF(kClocks) && status == B_OK; i++) {
		status = clocks->get_clock(fdtNode, kClocks[i], 0, &clockHandles[i]);
		if (status != B_OK)
			ERROR("clock %s not found\n", kClocks[i]);
	}
	for (size_t i = 0; i < B_COUNT_OF(kResets) && status == B_OK; i++) {
		status = clocks->get_reset(fdtNode, kResets[i], 0, &resetHandles[i]);
		if (status != B_OK)
			ERROR("reset %s not found\n", kResets[i]);
	}

	if (status == B_OK) {
		for (size_t i = 0; i < B_COUNT_OF(kResets); i++)
			clocks->set_reset(resetHandles[i], true);

		for (size_t i = 0; i < B_COUNT_OF(kClocks) && status == B_OK; i++) {
			status = clocks->enable_clock(clockHandles[i], true);
			INFO("clock %s: %s, %" B_PRId64 " Hz\n", kClocks[i],
				status == B_OK && clocks->is_clock_enabled(clockHandles[i])
					? "on" : "off",
				clocks->get_clock_rate(clockHandles[i]));
			if (status != B_OK)
				ERROR("could not enable clock %s\n", kClocks[i]);
		}
	}

	if (status == B_OK) {
		dwmac_jh7110_glue(module, dev);
		INFO("tx clock now %" B_PRId64 " Hz\n",
			clocks->get_clock_rate(clockHandles[3]));

		snooze(10);
		for (size_t i = 0; i < B_COUNT_OF(kResets); i++) {
			clocks->set_reset(resetHandles[i], false);
			INFO("reset %s: %s\n", kResets[i],
				clocks->is_reset_asserted(resetHandles[i])
					? "asserted" : "released");
		}
	}

	put_module(JH7110_CLOCK_RESET_MODULE_NAME);
	return status;
}


static void
dwmac_get_mac_address(dwmac_info* info, fdt_device_module_info* module,
	fdt_device* dev)
{
	int length;
	const uint8* address = (const uint8*)module->get_prop(dev,
		"local-mac-address", &length);
	if (address == NULL || length != 6)
		address = (const uint8*)module->get_prop(dev, "mac-address", &length);
	if (address != NULL && length == 6
		&& (address[0] | address[1] | address[2] | address[3] | address[4]
			| address[5]) != 0) {
		memcpy(info->macAddress.ebyte, address, 6);
		INFO("MAC address from the device tree\n");
		return;
	}

	// what the firmware programmed, if it used the port
	uint32 hi = info->regs->mac.addr[0].hi & 0xffff;
	uint32 lo = info->regs->mac.addr[0].lo;
	if ((hi != 0xffff || lo != 0xffffffff) && (hi != 0 || lo != 0)) {
		info->macAddress.ebyte[0] = lo;
		info->macAddress.ebyte[1] = lo >> 8;
		info->macAddress.ebyte[2] = lo >> 16;
		info->macAddress.ebyte[3] = lo >> 24;
		info->macAddress.ebyte[4] = hi;
		info->macAddress.ebyte[5] = hi >> 8;
		INFO("MAC address left by the firmware\n");
		return;
	}

	// a locally administered fallback, stable across boots
	static const uint8 kFallback[6] = { 0x02, 0x4d, 0x61, 0x72, 0x73, 0x00 };
	memcpy(info->macAddress.ebyte, kFallback, 6);
	INFO("no MAC address, using a locally administered one\n");
}


// #pragma mark - data path


static int32
dwmac_interrupt(void* data)
{
	dwmac_info* info = (dwmac_info*)data;
	auto& channel = info->regs->dma.channels[0];

	DwmacDmaChannelStatus status {.val = channel.status.val};
	if (status.val == 0)
		return B_UNHANDLED_INTERRUPT;
	channel.status.val = status.val;

	if (status.ri || status.eri || status.rbu)
		info->rxCondition.NotifyAll();
	if (status.ti || status.eti || status.tbu)
		info->txCondition.NotifyAll();

	// the MAC interrupts (RGMII status, ...) are cleared by reading
	if (info->regs->mac.intStatus != 0)
		(void)info->regs->mac.phyifControlStatus.val;

	return B_HANDLED_INTERRUPT;
}


static inline DwmacDesc*
rx_desc(dwmac_info* info, uint32 index)
{
	return (DwmacDesc*)((uint8*)info->rxDescs + index * kDescSize);
}


static inline DwmacDesc*
tx_desc(dwmac_info* info, uint32 index)
{
	return (DwmacDesc*)((uint8*)info->txDescs + index * kDescSize);
}


static status_t
dwmac_receive(dwmac_info* info, net_buffer** _buffer)
{
	MutexLocker locker(info->rxLock);

	while (true) {
		DwmacDesc* desc = rx_desc(info, info->rxIndex);
		while ((desc->des3.val & DES3_OWN) != 0) {
			if (info->closed)
				return B_FILE_ERROR;
			if (info->nonblocking)
				return B_WOULD_BLOCK;

			ConditionVariableEntry entry;
			info->rxCondition.Add(&entry);
			locker.Unlock();
			// re-check after registering, to not miss an interrupt; the
			// timeout covers lost ones
			if ((desc->des3.val & DES3_OWN) != 0 && !info->closed) {
				status_t status = entry.Wait(
					B_RELATIVE_TIMEOUT | B_CAN_INTERRUPT, 100000);
				if (status != B_OK && status != B_TIMED_OUT)
					return status;
			}
			locker.Lock();
		}
		memory_read_barrier();

		uint32 des3 = desc->des3.val;
		uint32 length = des3 & DES3_LENGTH_MASK;
		uint8* data = info->rxBuffers + info->rxIndex * kBufferSize;

		status_t result = B_OK;
		net_buffer* buffer = NULL;
		if ((des3 & (DES3_FD | DES3_LD)) != (DES3_FD | DES3_LD)
			|| (des3 & DES3_RX_ES) != 0 || length < 14
			|| length > kBufferSize) {
			TRACE("rx error, des3 %#" B_PRIx32 "\n", des3);
			result = B_IO_ERROR;
		} else {
			buffer = sBufferModule->create(0);
			if (buffer == NULL
				|| sBufferModule->append(buffer, data, length) != B_OK) {
				if (buffer != NULL)
					sBufferModule->free(buffer);
				buffer = NULL;
				result = B_NO_MEMORY;
			}
		}

		// hand the descriptor back and move the tail pointer onto it
		dwmac_init_rx_desc(info, info->rxIndex);
		memory_full_barrier();
		info->regs->dma.channels[0].rxEndAddr = info->rxDescsPhysical
			+ info->rxIndex * kDescSize;
		info->rxIndex = (info->rxIndex + 1) % kDescCount;

		if (result == B_IO_ERROR)
			continue;
		if (result != B_OK)
			return result;

		*_buffer = buffer;
		return B_OK;
	}
}


static status_t
dwmac_send(dwmac_info* info, net_buffer* buffer)
{
	size_t size = buffer->size;
	if (size > kMaxFrameSize)
		return B_BAD_VALUE;

	MutexLocker locker(info->txLock);

	DwmacDesc* desc = tx_desc(info, info->txIndex);
	while ((desc->des3.val & DES3_OWN) != 0) {
		if (info->closed)
			return B_FILE_ERROR;
		if (info->nonblocking)
			return B_WOULD_BLOCK;

		ConditionVariableEntry entry;
		info->txCondition.Add(&entry);
		locker.Unlock();
		if ((desc->des3.val & DES3_OWN) != 0 && !info->closed) {
			status_t status = entry.Wait(B_RELATIVE_TIMEOUT | B_CAN_INTERRUPT,
				100000);
			if (status != B_OK && status != B_TIMED_OUT)
				return status;
		}
		locker.Lock();
	}

	uint8* data = info->txBuffers + info->txIndex * kBufferSize;
	if (sBufferModule->read(buffer, 0, data, size) != B_OK)
		return B_BAD_DATA;
	if (size < 60) {
		memset(data + size, 0, 60 - size);
		size = 60;
	}

	phys_addr_t address = info->txBuffersPhysical
		+ info->txIndex * kBufferSize;
	desc->des0 = (uint32)address;
	desc->des1 = (uint32)(address >> 32);
	desc->des2 = DES2_TX_IOC | size;
	// the controller must see the rest of the descriptor before OWN
	memory_full_barrier();
	desc->des3.val = DES3_OWN | DES3_FD | DES3_LD | size;
	memory_full_barrier();

	info->txIndex = (info->txIndex + 1) % kDescCount;
	info->regs->dma.channels[0].txEndAddr = info->txDescsPhysical
		+ info->txIndex * kDescSize;

	locker.Unlock();
	sBufferModule->free(buffer);
	return B_OK;
}


// #pragma mark - device hooks


static status_t
dwmac_init_device(void* _info, void** _cookie)
{
	*_cookie = _info;
	return B_OK;
}


static void
dwmac_uninit_device(void* _cookie)
{
}


static status_t
dwmac_open(void* _info, const char* path, int openMode, void** _cookie)
{
	dwmac_info* info = (dwmac_info*)_info;

	dwmac_handle* handle = (dwmac_handle*)malloc(sizeof(dwmac_handle));
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
dwmac_close(void* cookie)
{
	dwmac_handle* handle = (dwmac_handle*)cookie;
	dwmac_info* info = handle->info;

	// wake up blocked readers and writers, so that "ifconfig down" returns
	info->closed = true;
	info->rxCondition.NotifyAll();
	info->txCondition.NotifyAll();
	return B_OK;
}


static status_t
dwmac_free(void* cookie)
{
	dwmac_handle* handle = (dwmac_handle*)cookie;
	atomic_add(&handle->info->openCount, -1);
	free(handle);
	return B_OK;
}


static status_t
dwmac_ioctl(void* cookie, uint32 op, void* buffer, size_t length)
{
	dwmac_handle* handle = (dwmac_handle*)cookie;
	dwmac_info* info = handle->info;

	switch (op) {
		case ETHER_GETADDR:
			return user_memcpy(buffer, &info->macAddress,
				sizeof(info->macAddress));

		case ETHER_INIT:
			return B_OK;

		case ETHER_GETFRAMESIZE:
		{
			uint32 frameSize = kMaxFrameSize;
			return user_memcpy(buffer, &frameSize, sizeof(frameSize));
		}

		case ETHER_SETPROMISC:
		{
			int32 promiscuous;
			if (user_memcpy(&promiscuous, buffer, sizeof(promiscuous))
					!= B_OK) {
				return B_BAD_ADDRESS;
			}
			// always promiscuous for now (see dwmac_configure_mac())
			TRACE("promiscuous mode %s requested\n",
				promiscuous != 0 ? "on" : "off");
			return B_OK;
		}

		case ETHER_NONBLOCK:
		{
			int32 nonblocking;
			if (user_memcpy(&nonblocking, buffer, sizeof(nonblocking))
					!= B_OK) {
				return B_BAD_ADDRESS;
			}
			info->nonblocking = nonblocking != 0;
			return B_OK;
		}

		case ETHER_ADDMULTI:
		case ETHER_REMMULTI:
			// all multicast frames are passed
			return B_OK;

		case ETHER_SET_LINK_STATE_SEM:
			if (user_memcpy(&info->linkStateSem, buffer, sizeof(sem_id))
					!= B_OK) {
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
			return dwmac_send(info, (net_buffer*)buffer);

		case ETHER_RECEIVE_NET_BUFFER:
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return dwmac_receive(info, (net_buffer**)buffer);

		default:
			break;
	}

	return B_DEV_INVALID_IOCTL;
}


// #pragma mark - driver module


static float
dwmac_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}
	if (!is_compatible(parent))
		return 0.0f;

	fdt_device* dev;
	fdt_device_module_info* module = get_fdt(parent, &dev);
	if (module == NULL)
		return 0.0f;
	int length;
	const char* status = (const char*)module->get_prop(dev, "status", &length);
	if (status != NULL && strcmp(status, "okay") != 0
		&& strcmp(status, "ok") != 0) {
		return 0.0f;
	}
	return 0.8f;
}


static status_t
dwmac_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "DesignWare Ethernet QoS" }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, DWMAC_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static void
dwmac_free_info(dwmac_info* info)
{
	if (info->interruptInstalled)
		remove_io_interrupt_handler(info->irq, dwmac_interrupt, info);
	if (info->dmaArea >= 0)
		delete_area(info->dmaArea);
	if (info->registerArea >= 0)
		delete_area(info->registerArea);
	mutex_destroy(&info->rxLock);
	mutex_destroy(&info->txLock);
	delete info;
}


static status_t
dwmac_init_driver(device_node* node, void** _cookie)
{
	dwmac_info* info = new(std::nothrow) dwmac_info;
	if (info == NULL)
		return B_NO_MEMORY;
	memset((void*)info, 0, sizeof(*info));
	new(&info->rxCondition) ConditionVariable;
	new(&info->txCondition) ConditionVariable;
	info->node = node;
	info->registerArea = -1;
	info->dmaArea = -1;
	info->linkThread = -1;
	info->linkStateSem = -1;
	info->phyAddress = -1;
	mutex_init(&info->rxLock, "dwmac rx");
	mutex_init(&info->txLock, "dwmac tx");
	info->rxCondition.Init(info, "dwmac rx");
	info->txCondition.Init(info, "dwmac tx");

	device_node* fdtNode = sDeviceManager->get_parent_node(node);
	fdt_device* dev;
	fdt_device_module_info* module = get_fdt(fdtNode, &dev);
	uint64 regs, regsLength, irq;
	if (module == NULL || !module->get_reg(dev, 0, &regs, &regsLength)
		|| !module->get_interrupt(dev, 0, NULL, &irq)) {
		ERROR("no registers or interrupt in the device tree\n");
		sDeviceManager->put_node(fdtNode);
		dwmac_free_info(info);
		return B_BAD_DATA;
	}
	info->irq = irq;

	// before the first register access: an unclocked controller can stall
	// the bus, and this line then shows which one
	INFO("init %#" B_PRIx64 ", irq %" B_PRIu32 "\n", regs, info->irq);

	status_t status = dwmac_clocks_and_resets(fdtNode, module, dev);
	if (status != B_OK) {
		sDeviceManager->put_node(fdtNode);
		dwmac_free_info(info);
		return status;
	}

	void* registers;
	info->registerArea = map_physical_memory("dwmac registers", regs,
		ROUNDUP(regsLength, B_PAGE_SIZE), B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &registers);
	if (info->registerArea < 0) {
		status = info->registerArea;
		sDeviceManager->put_node(fdtNode);
		dwmac_free_info(info);
		return status;
	}
	info->regs = (DwmacRegs volatile*)registers;

	uint32 version = *(vuint32*)((addr_t)registers + MAC_VERSION);
	INFO("version %#" B_PRIx32 ", hw features %#" B_PRIx32 " %#" B_PRIx32
		"\n", version, info->regs->mac.hwFeature0,
		info->regs->mac.hwFeature1.val);

	dwmac_get_mac_address(info, module, dev);
	INFO("MAC address %02x:%02x:%02x:%02x:%02x:%02x\n",
		info->macAddress.ebyte[0], info->macAddress.ebyte[1],
		info->macAddress.ebyte[2], info->macAddress.ebyte[3],
		info->macAddress.ebyte[4], info->macAddress.ebyte[5]);

	// MDIO works before the reset: find the PHY and keep its RX clock on
	fdt_device_module_info* phyModule = NULL;
	fdt_device* phy = dwmac_phy_find(info, module, dev, &phyModule);
	if (info->phyId == YT8531_PHY_ID)
		yt8531_keep_rx_clock(info);

	// software reset; it completes only with the RX and TX clocks running
	info->regs->dma.busMode.swr = true;
	if (wait_for_cond([info]() { return !info->regs->dma.busMode.swr; },
			100, 1000) != B_OK) {
		ERROR("software reset did not complete (no clock from the PHY?)\n");
		if (info->phyAddress >= 0) {
			ERROR("PHY id %#" B_PRIx32 ", bmsr %#x\n", info->phyId,
				dwmac_mdio_read(info, info->phyAddress, MII_BMSR));
			if (info->phyId == YT8531_PHY_ID) {
				ERROR("YT8531 clock gating %#x, sleep control %#x, chip "
					"config %#x\n", ytphy_read_ext(info, YT8531_CLOCK_GATING),
					ytphy_read_ext(info, YT8531_SLEEP_CONTROL1),
					ytphy_read_ext(info, YT8531_CHIP_CONFIG));
			}
		}
		sDeviceManager->put_node(fdtNode);
		dwmac_free_info(info);
		return B_IO_ERROR;
	}

	// 1 us tick (for the LPI timers), as X547's driver sets it
	info->regs->mac.usTicCounter = 125 - 1;

	status = dwmac_alloc_buffers(info);
	if (status != B_OK) {
		sDeviceManager->put_node(fdtNode);
		dwmac_free_info(info);
		return status;
	}

	uint32 tqs;
	dwmac_configure_mtl(info, tqs);
	dwmac_configure_mac(info);
	dwmac_configure_dma(info, tqs);

	status = install_io_interrupt_handler(info->irq, dwmac_interrupt, info, 0);
	if (status == B_OK)
		info->interruptInstalled = true;
	else
		ERROR("could not install the interrupt handler, polling only\n");

	dwmac_phy_init(info, phyModule, phy);
	sDeviceManager->put_node(fdtNode);

	info->linkThread = spawn_kernel_thread(dwmac_link_thread, "dwmac link",
		B_LOW_PRIORITY, info);
	resume_thread(info->linkThread);

	*_cookie = info;
	return B_OK;
}


static void
dwmac_uninit_driver(void* _cookie)
{
	dwmac_info* info = (dwmac_info*)_cookie;

	info->stopLinkThread = true;
	status_t result;
	wait_for_thread(info->linkThread, &result);

	auto& channel = info->regs->dma.channels[0];
	channel.intrEna.val = 0;
	channel.txControl.st = false;
	channel.rxControl.sr = false;
	info->regs->mac.config.re = false;
	info->regs->mac.config.te = false;

	dwmac_free_info(info);
}


static status_t
dwmac_register_child_devices(void* _cookie)
{
	dwmac_info* info = (dwmac_info*)_cookie;

	int32 id = sDeviceManager->create_id(DWMAC_DEVICE_ID_GENERATOR);
	if (id < 0)
		return id;

	char name[64];
	snprintf(name, sizeof(name), "net/dwmac/%" B_PRId32, id);

	status_t status = sDeviceManager->publish_device(info->node, name,
		DWMAC_DEVICE_MODULE_NAME);
	if (status != B_OK)
		sDeviceManager->free_id(DWMAC_DEVICE_ID_GENERATOR, id);
	return status;
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ NET_BUFFER_MODULE_NAME, (module_info**)&sBufferModule },
	{}
};


static struct device_module_info sDwmacDevice = {
	{
		DWMAC_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	dwmac_init_device,
	dwmac_uninit_device,
	NULL,	// remove

	dwmac_open,
	dwmac_close,
	dwmac_free,
	NULL,	// read
	NULL,	// write
	NULL,	// io
	dwmac_ioctl,

	NULL,	// select
	NULL,	// deselect
};


static struct driver_module_info sDwmacDriver = {
	{
		DWMAC_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	dwmac_supports_device,
	dwmac_register_device,
	dwmac_init_driver,
	dwmac_uninit_driver,
	dwmac_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


module_info* modules[] = {
	(module_info*)&sDwmacDriver,
	(module_info*)&sDwmacDevice,
	NULL
};
