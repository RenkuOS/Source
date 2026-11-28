/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 *
 * StarFive JH7110 ("starfive,jh7110-usb"): the Cadence USB controller in
 * host mode is an xHCI controller. Before the xHCI registers can be used,
 * the glue needs what Linux's cdns3-starfive and phy-jh7110-usb and the
 * Cadence DRD code do:
 *  - the STG syscon strapped to host, with its PLL and reference clock,
 *  - the USB 2.0 PHY: 125 MHz clock, the low speed keep-alive for host mode,
 *    and the SYS syscon "split" bit (USB 2.0 only: the device trees of the
 *    boards give the controller no USB 3 PHY),
 *  - the pin group of the node (VBUS) and the over-current input of the
 *    controller,
 *  - the controller clocks on and resets released,
 *  - the OTG block asked for the host role, then XHCI_READY.
 * The device tree gives the xHCI registers and interrupt in the "cdns,usb3"
 * child node, translated by the glue node's "ranges".
 */


#include "xhci_jh7110.h"

#include <stdio.h>
#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <bus/FDT.h>
#include <fdt_clock_reset.h>
#include <kernel.h>


#define TRACE_JH7110(x...)	dprintf("xhci jh7110: " x)

// syscons; the device tree has "starfive,stg-syscon = <&stg_syscon offset>"
// and the PHY driver finds the SYS syscon by its compatible
#define JH7110_STG_SYSCON			0x10240000
#define JH7110_SYS_SYSCON			0x13030000

#define STG_USB_STRAP_HOST			(1u << 17)
#define STG_USB_STRAP_MASK			(7u << 16)
#define STG_USB_SUSPENDM_HOST		(1u << 19)
#define STG_USB_SUSPENDM_BYPS		(1u << 20)
#define STG_USB_PLL_EN				(1u << 22)
#define STG_USB_REFCLK_MODE			(1u << 23)
#define STG_USB_MISC_MASK			(0xfu << 20)

#define SYS_USB_PDRSTN				0x18
#define SYS_USB_SPLIT				(1u << 17)

// SYS GPIO controller: one byte per GPIO (DOEN, DOUT) and per input signal
// (GPI: 0 is low, 1 is high, 2 + n is GPIO n)
#define JH7110_SYS_GPIO				0x13040000
#define GPIO_DOEN					0x000
#define GPIO_DOUT					0x040
#define GPIO_GPI					0x080
#define GPIO_DOEN_MASK				0x3f
#define GPIO_DOUT_MASK				0x7f
#define GPIO_GPI_MASK				0x7f
#define GPI_NONE					0xff
#define GPI_SYS_USB_OVERCURRENT		2
#define GPI_HIGH					1

#define PHY_LS_KEEPALIVE			0x4
#define PHY_LS_KEEPALIVE_ENABLE		(1u << 4)
#define PHY_CLOCK_RATE				125000000

// Cadence DRD (OTG) registers. Version 1 starts with an ID register; in
// version 0 the first register is the (write only, reads 0) command.
#define OTG_V1_CMD					0x10
#define OTG_V1_SIMULATE				0x40
#define OTGCMD_HOST_BUS_REQ			(1u << 1)
#define OTGCMD_OTG_DIS				(1u << 3)
#define OTGSTS_STRAP(x)				(((x) >> 12) & 7)
#define OTGSTS_XHCI_READY			(1u << 26)


extern device_manager_info* gDeviceManager;


static vuint32*
map_registers(phys_addr_t address, size_t size, const char* name,
	area_id& area)
{
	phys_addr_t base = ROUNDDOWN(address, B_PAGE_SIZE);
	size_t length = ROUNDUP(address + size, B_PAGE_SIZE) - base;
	void* virtualBase;
	area = map_physical_memory(name, base, length, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &virtualBase);
	if (area < 0)
		return NULL;
	return (vuint32*)((addr_t)virtualBase + (address - base));
}


static void
update_register(phys_addr_t address, uint32 mask, uint32 bits)
{
	area_id area;
	vuint32* reg = map_registers(address, 4, "jh7110 usb syscon", area);
	if (reg == NULL)
		return;
	*reg = (*reg & ~mask) | (bits & mask);
	delete_area(area);
}


/*!	Finds the child of \a parent whose "fdt/compatible" is \a compatible;
	the caller puts the node.
*/
static device_node*
find_child(device_node* parent, const char* compatible)
{
	device_attr attrs[] = {
		{ "fdt/compatible", B_STRING_TYPE, { .string = compatible }},
		{ NULL }
	};
	device_node* child = NULL;
	if (gDeviceManager->get_next_child_node(parent, attrs, &child) != B_OK)
		return NULL;
	return child;
}


static int32
reg_index(fdt_device_module_info* fdt, fdt_device* device, const char* name)
{
	int length;
	const char* names = (const char*)fdt->get_prop(device, "reg-names",
		&length);
	if (names == NULL)
		return -1;
	int32 index = 0;
	for (int offset = 0; offset < length; index++) {
		if (strcmp(names + offset, name) == 0)
			return index;
		offset += strlen(names + offset) + 1;
	}
	return -1;
}


static status_t
enable_clocks_and_resets(fdt_clock_reset_module_info* clocks,
	device_node* glue, device_node* phy)
{
	// the PHY first: its 125 MHz clock and the application clock
	fdt_clock clock;
	status_t status = clocks->get_clock(phy, "125m", 0, &clock);
	if (status == B_OK) {
		int64 rate = clocks->set_clock_rate(clock, PHY_CLOCK_RATE);
		if (rate != PHY_CLOCK_RATE)
			TRACE_JH7110("PHY clock at %" B_PRId64 " Hz\n", rate);
		status = clocks->enable_clock(clock, true);
	}
	if (status == B_OK)
		status = clocks->get_clock(phy, "app_125m", 0, &clock);
	if (status == B_OK)
		status = clocks->enable_clock(clock, true);
	if (status != B_OK) {
		TRACE_JH7110("PHY clocks: %s\n", strerror(status));
		return status;
	}

	static const char* const kClocks[] = { "lpm", "stb", "apb", "axi",
		"utmi_apb" };
	for (size_t i = 0; i < B_COUNT_OF(kClocks); i++) {
		status = clocks->get_clock(glue, kClocks[i], 0, &clock);
		if (status == B_OK)
			status = clocks->enable_clock(clock, true);
		if (status != B_OK) {
			TRACE_JH7110("clock %s: %s\n", kClocks[i], strerror(status));
			return status;
		}
	}

	static const char* const kResets[] = { "pwrup", "apb", "axi",
		"utmi_apb" };
	for (size_t i = 0; i < B_COUNT_OF(kResets); i++) {
		fdt_reset reset;
		status = clocks->get_reset(glue, kResets[i], 0, &reset);
		if (status == B_OK)
			status = clocks->set_reset(reset, false);
		if (status != B_OK) {
			TRACE_JH7110("reset %s: %s\n", kResets[i], strerror(status));
			return status;
		}
	}
	return B_OK;
}


static void
set_gpio_byte(uint32 offset, uint32 index, uint32 mask, uint32 value)
{
	uint32 shift = 8 * (index % 4);
	update_register(JH7110_SYS_GPIO + offset + 4 * (index / 4), mask << shift,
		value << shift);
}


/*!	Applies the "pinctrl-0" pin group of the glue node (on the Milk-V Mars:
	GPIO 25 driven by the controller's USB_DRIVE_VBUS, the VBUS switch of
	the two ports). Its "pinmux" entries are GPIOMUX(pin, dout, doen, din).
	The over-current input of the controller is tied high (no over-current)
	unless the group routes it to a pin: left at its reset value (low), the
	controller sees every port in over-current, and the xHCI reset does not
	complete.
*/
static void
apply_pinctrl(fdt_device_module_info* fdt, fdt_device* device,
	device_node* soc)
{
	// any child: get_next_child_node() matches nothing with NULL
	static const device_attr kAnyNode[] = { { NULL } };
	bool overCurrentRouted = false;
	int length;
	const uint32* group = (const uint32*)fdt->get_prop(device, "pinctrl-0",
		&length);
	device_node* pinctrl = group != NULL && length >= 4
		? find_child(soc, "starfive,jh7110-sys-pinctrl") : NULL;
	if (pinctrl != NULL) {
		uint32 phandle = B_BENDIAN_TO_HOST_INT32(group[0]);
		device_node* node = NULL;
		while (gDeviceManager->get_next_child_node(pinctrl, kAnyNode, &node)
				== B_OK) {
			fdt_device_module_info* nodeFdt;
			fdt_device* nodeDevice;
			gDeviceManager->get_driver(node, (driver_module_info**)&nodeFdt,
				(void**)&nodeDevice);
			const uint32* value = (const uint32*)nodeFdt->get_prop(nodeDevice,
				"phandle", &length);
			if (value == NULL || length != 4
				|| B_BENDIAN_TO_HOST_INT32(*value) != phandle) {
				continue;
			}

			device_node* pins = NULL;
			while (gDeviceManager->get_next_child_node(node, kAnyNode, &pins)
					== B_OK) {
				fdt_device_module_info* pinsFdt;
				fdt_device* pinsDevice;
				gDeviceManager->get_driver(pins,
					(driver_module_info**)&pinsFdt, (void**)&pinsDevice);
				const uint32* pinmux = (const uint32*)pinsFdt->get_prop(
					pinsDevice, "pinmux", &length);
				for (int i = 0; pinmux != NULL && i < length / 4; i++) {
					uint32 mux = B_BENDIAN_TO_HOST_INT32(pinmux[i]);
					uint32 pin = mux & 0x3f;
					uint32 doen = (mux >> 10) & 0x3f;
					uint32 dout = (mux >> 16) & 0xff;
					uint32 din = mux >> 24;
					TRACE_JH7110("GPIO %" B_PRIu32 ": dout %" B_PRIu32
						", doen %" B_PRIu32 ", din %" B_PRIu32 "\n", pin, dout,
						doen, din);
					set_gpio_byte(GPIO_DOUT, pin, GPIO_DOUT_MASK, dout);
					set_gpio_byte(GPIO_DOEN, pin, GPIO_DOEN_MASK, doen);
					if (din != GPI_NONE) {
						set_gpio_byte(GPIO_GPI, din, GPIO_GPI_MASK, pin + 2);
						if (din == GPI_SYS_USB_OVERCURRENT)
							overCurrentRouted = true;
					}
				}
			}
			gDeviceManager->put_node(node);
			break;
		}
		gDeviceManager->put_node(pinctrl);
	}

	if (!overCurrentRouted) {
		set_gpio_byte(GPIO_GPI, GPI_SYS_USB_OVERCURRENT, GPIO_GPI_MASK,
			GPI_HIGH);
	}
}


/*!	Asks the Cadence DRD block for the host role and waits for the xHCI
	controller to be ready, as cdns3_drd_switch_host() does.
*/
static status_t
switch_to_host(phys_addr_t otgAddress)
{
	area_id area;
	vuint32* otg = map_registers(otgAddress, 0x80, "jh7110 usb otg", area);
	if (otg == NULL)
		return B_NO_MEMORY;

	status_t status = B_OK;
	if (otg[0] == 0) {
		// version 0: never seen on the JH7110
		TRACE_JH7110("DRD version 0 is not supported\n");
		status = B_NOT_SUPPORTED;
	} else {
		vuint32* cmd = otg + OTG_V1_CMD / 4;
		vuint32* sts = cmd + 1;
		TRACE_JH7110("DRD id %#" B_PRIx32 ", revision %#" B_PRIx32
			", status %#" B_PRIx32 " (strap %" B_PRIu32 ")\n", otg[0], otg[1],
			*sts, OTGSTS_STRAP(*sts));
		otg[OTG_V1_SIMULATE / 4] = 1;
		*cmd = OTGCMD_HOST_BUS_REQ | OTGCMD_OTG_DIS;

		status = B_TIMED_OUT;
		for (int i = 0; i < 1000; i++) {
			if ((*sts & OTGSTS_XHCI_READY) != 0) {
				status = B_OK;
				break;
			}
			spin(100);
		}
		if (status != B_OK)
			TRACE_JH7110("no XHCI_READY, status %#" B_PRIx32 "\n", *sts);
	}

	delete_area(area);
	return status;
}


status_t
jh7110_usb_init(device_node* glue, phys_addr_t* _address, uint64* _length,
	uint32* _irq)
{
	fdt_device_module_info* fdt;
	fdt_device* device;
	if (gDeviceManager->get_driver(glue, (driver_module_info**)&fdt,
			(void**)&device) != B_OK) {
		return B_ERROR;
	}

	// the controller node and the translation of its addresses
	int length;
	const uint32* ranges = (const uint32*)fdt->get_prop(device, "ranges",
		&length);
	if (ranges == NULL || length != 16) {
		TRACE_JH7110("unexpected \"ranges\"\n");
		return B_BAD_VALUE;
	}
	uint64 childBase = B_BENDIAN_TO_HOST_INT32(ranges[0]);
	uint64 parentBase = ((uint64)B_BENDIAN_TO_HOST_INT32(ranges[1]) << 32)
		| B_BENDIAN_TO_HOST_INT32(ranges[2]);

	device_node* controller = find_child(glue, "cdns,usb3");
	if (controller == NULL) {
		TRACE_JH7110("no \"cdns,usb3\" node\n");
		return B_DEVICE_NOT_FOUND;
	}
	fdt_device_module_info* controllerFdt;
	fdt_device* controllerDevice;
	gDeviceManager->get_driver(controller, (driver_module_info**)&controllerFdt,
		(void**)&controllerDevice);

	uint64 otgAddress, otgLength, xhciAddress, xhciLength, irq;
	int32 otgIndex = reg_index(controllerFdt, controllerDevice, "otg");
	int32 xhciIndex = reg_index(controllerFdt, controllerDevice, "xhci");
	bool found = otgIndex >= 0 && xhciIndex >= 0
		&& controllerFdt->get_reg(controllerDevice, otgIndex, &otgAddress,
			&otgLength)
		&& controllerFdt->get_reg(controllerDevice, xhciIndex, &xhciAddress,
			&xhciLength)
		&& controllerFdt->get_interrupt(controllerDevice, 0, NULL, &irq);
		// "host" is the first interrupt
	gDeviceManager->put_node(controller);
	if (!found) {
		TRACE_JH7110("controller registers or interrupt missing\n");
		return B_BAD_VALUE;
	}
	otgAddress = otgAddress - childBase + parentBase;
	xhciAddress = xhciAddress - childBase + parentBase;

	// the STG syscon register for the controller mode
	const uint32* syscon = (const uint32*)fdt->get_prop(device,
		"starfive,stg-syscon", &length);
	if (syscon == NULL || length != 8) {
		TRACE_JH7110("no \"starfive,stg-syscon\"\n");
		return B_BAD_VALUE;
	}
	phys_addr_t modeRegister = JH7110_STG_SYSCON
		+ B_BENDIAN_TO_HOST_INT32(syscon[1]);

	// the USB 2.0 PHY, a sibling node
	device_node* soc = gDeviceManager->get_parent_node(glue);
	device_node* phy = find_child(soc, "starfive,jh7110-usb-phy");
	apply_pinctrl(fdt, device, soc);
	gDeviceManager->put_node(soc);
	if (phy == NULL) {
		TRACE_JH7110("no USB PHY node\n");
		return B_DEVICE_NOT_FOUND;
	}
	fdt_device_module_info* phyFdt;
	fdt_device* phyDevice;
	gDeviceManager->get_driver(phy, (driver_module_info**)&phyFdt,
		(void**)&phyDevice);
	uint64 phyAddress, phyLength;
	if (!phyFdt->get_reg(phyDevice, 0, &phyAddress, &phyLength)) {
		gDeviceManager->put_node(phy);
		return B_BAD_VALUE;
	}

	TRACE_JH7110("xHCI at %#" B_PRIx64 ", OTG at %#" B_PRIx64 ", PHY at %#"
		B_PRIx64 ", irq %" B_PRIu64 "\n", xhciAddress, otgAddress, phyAddress,
		irq);

	// strapped to host before the resets are released, as Linux does
	update_register(modeRegister, STG_USB_MISC_MASK,
		STG_USB_SUSPENDM_BYPS | STG_USB_PLL_EN | STG_USB_REFCLK_MODE);
	update_register(modeRegister, STG_USB_STRAP_MASK | STG_USB_SUSPENDM_HOST,
		STG_USB_STRAP_HOST | STG_USB_SUSPENDM_HOST);

	fdt_clock_reset_module_info* clocks;
	status_t status = get_module(JH7110_CLOCK_RESET_MODULE_NAME,
		(module_info**)&clocks);
	if (status == B_OK) {
		status = enable_clocks_and_resets(clocks, glue, phy);
		put_module(JH7110_CLOCK_RESET_MODULE_NAME);
	} else
		TRACE_JH7110("no clock/reset driver\n");
	gDeviceManager->put_node(phy);
	if (status != B_OK)
		return status;

	// PHY in host mode: keep-alive for low speed devices; USB 2.0 only
	update_register(phyAddress + PHY_LS_KEEPALIVE, PHY_LS_KEEPALIVE_ENABLE,
		PHY_LS_KEEPALIVE_ENABLE);
	update_register(JH7110_SYS_SYSCON + SYS_USB_PDRSTN, SYS_USB_SPLIT,
		SYS_USB_SPLIT);

	status = switch_to_host(otgAddress);
	if (status != B_OK)
		return status;

	*_address = xhciAddress;
	*_length = xhciLength;
	*_irq = irq;
	return B_OK;
}
