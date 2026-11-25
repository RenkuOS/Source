/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


//!	Audio for Intel Bay Trail tablets with an RT5640 codec.
// On Bay Trail the audio does not go through HD Audio: the low power engine
// (LPE, ACPI 80860F28), a DSP with its own SSP (I2S) ports and DMA, feeds a
// codec on I2C. This driver attaches to that codec (Realtek RT5640, ACPI
// 10EC5640) and will drive the LPE as well.
//
// This first step brings up the codec: it switches on its master clock
// through ACPI, checks and resets the chip, sets up jack detection and logs
// when headphones are plugged in or out. The DSP and playback follow.


#include <ACPI.h>
#include <KernelExport.h>
#include <device_manager.h>
#include <i2c.h>
#include <kernel.h>

#include <new>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>



#define DRIVER_NAME		"intel_lpe"
#define INTEL_LPE_DRIVER_MODULE_NAME "drivers/audio/hmulti/intel_lpe/driver_v1"

#define TRACE_ALWAYS(x...)	dprintf(DRIVER_NAME ": " x)


// RT5640 registers (ALC5640 datasheet, chapter 8); 8 bit register numbers,
// 16 bit big endian values.
#define RT5640_RESET				0x00
#define RT5640_IN1_IN2				0x0d
#define RT5640_PWR_DIG1				0x61
#define RT5640_PWR_ANLG1			0x63
#define RT5640_PWR_ANLG2			0x64
#define RT5640_JD_CTRL				0xbb
#define RT5640_IRQ_CTRL1			0xbd
#define RT5640_IRQ_CTRL2			0xbe
#define RT5640_INT_IRQ_ST			0xbf
#define RT5640_GPIO_CTRL1			0xc0
#define RT5640_GEN_CTRL1			0xfa
#define RT5640_GEN_CTRL2			0xfb
#define RT5640_VENDOR_ID			0xfe
#define RT5640_DEVICE_ID			0xff

#define RT5640_VENDOR_REALTEK		0x10ec

// MX-BF: status of the selected jack detect source
#define RT5640_STATUS_JD			(1 << 4)

#define JACK_POLL_INTERVAL			250000


struct intel_lpe_driver {
	device_node*			node;
	acpi_handle				codecHandle;

	i2c_device_interface*	i2c;
	i2c_device				i2cCookie;

	thread_id				pollThread;
	volatile bool			stopPolling;

};


static device_manager_info* sDeviceManager;


//	#pragma mark - RT5640


static status_t
rt5640_read(intel_lpe_driver* driver, uint8 reg, uint16* _value)
{
	uint8 data[2];
	status_t status = driver->i2c->acquire_bus(driver->i2cCookie);
	if (status != B_OK)
		return status;
	status = driver->i2c->exec_command(driver->i2cCookie, I2C_OP_READ_STOP,
		&reg, 1, data, sizeof(data));
	driver->i2c->release_bus(driver->i2cCookie);
	if (status == B_OK)
		*_value = (data[0] << 8) | data[1];
	return status;
}


static status_t
rt5640_write(intel_lpe_driver* driver, uint8 reg, uint16 value)
{
	uint8 command[3] = { reg, (uint8)(value >> 8), (uint8)value };
	status_t status = driver->i2c->acquire_bus(driver->i2cCookie);
	if (status != B_OK)
		return status;
	status = driver->i2c->exec_command(driver->i2cCookie, I2C_OP_WRITE_STOP,
		command, sizeof(command), NULL, 0);
	driver->i2c->release_bus(driver->i2cCookie);
	return status;
}


/*!	Switches on the master clock of the codec. Its ACPI device does that in
	_PS0: it forces platform clock 3 of the PMC on, which feeds the MCLK pin.
*/
static void
rt5640_power_on(intel_lpe_driver* driver)
{
	if (driver->codecHandle == NULL)
		return;

	acpi_module_info* acpi;
	if (get_module(B_ACPI_MODULE_NAME, (module_info**)&acpi) != B_OK)
		return;

	status_t status = acpi->evaluate_method(driver->codecHandle, "_PS0", NULL,
		NULL);
	if (status != B_OK)
		TRACE_ALWAYS("_PS0 of the codec failed: %s\n", strerror(status));
	put_module(B_ACPI_MODULE_NAME);
	snooze(10000);
}


static status_t
rt5640_probe(intel_lpe_driver* driver)
{
	rt5640_power_on(driver);

	uint16 vendor, device;
	status_t status = rt5640_read(driver, RT5640_VENDOR_ID, &vendor);
	if (status == B_OK)
		status = rt5640_read(driver, RT5640_DEVICE_ID, &device);
	if (status != B_OK) {
		TRACE_ALWAYS("codec does not answer: %s\n", strerror(status));
		return status;
	}
	if (vendor != RT5640_VENDOR_REALTEK) {
		TRACE_ALWAYS("unexpected codec vendor %#04x\n", vendor);
		return B_DEVICE_NOT_FOUND;
	}
	TRACE_ALWAYS("RT5640 codec, device id %#04x\n", device);

	// any write to MX-00 resets the codec to its defaults
	status = rt5640_write(driver, RT5640_RESET, 0);
	if (status != B_OK)
		return status;
	snooze(10000);

	// Jack detection as the Android driver of the tablet left the codec
	// (register dump taken while playing to the speakers): JD1 (the IN1N
	// pin, IN1 single ended) is the source, and the analog references and
	// biases that the jack detect comparator needs are on.
	// Android also routes the jack state to the IRQ output (MX-BD, MX-C0);
	// that line ends at a GPIO interrupt of the SoC that nothing handles yet,
	// so it stays off and the driver polls MX-BF instead.
	static const struct {
		uint8	reg;
		uint16	value;
	} kJackDetectSetup[] = {
		{ RT5640_PWR_ANLG1,		0xe818 },
		{ RT5640_GEN_CTRL1,		0x3e71 },
		{ RT5640_GEN_CTRL2,		0x4040 },
		{ RT5640_JD_CTRL,		0x4003 },
	};
	for (size_t i = 0; i < B_COUNT_OF(kJackDetectSetup); i++) {
		rt5640_write(driver, kJackDetectSetup[i].reg,
			kJackDetectSetup[i].value);
	}

	static const uint8 kRegisters[] = {
		RT5640_IN1_IN2, RT5640_PWR_DIG1, RT5640_PWR_ANLG1, RT5640_PWR_ANLG2,
		RT5640_JD_CTRL, RT5640_IRQ_CTRL1, RT5640_IRQ_CTRL2, RT5640_INT_IRQ_ST,
		RT5640_GPIO_CTRL1, RT5640_GEN_CTRL1, RT5640_GEN_CTRL2
	};
	for (size_t i = 0; i < B_COUNT_OF(kRegisters); i++) {
		uint16 value = 0;
		rt5640_read(driver, kRegisters[i], &value);
		TRACE_ALWAYS("MX-%02X = %04x\n", kRegisters[i], value);
	}
	return B_OK;
}


static status_t
jack_poll_thread(void* data)
{
	intel_lpe_driver* driver = (intel_lpe_driver*)data;
	uint16 last = 0xffff;
	uint16 lastJD2 = 0xffff;

	while (!driver->stopPolling) {
		uint16 status, gen2;
		if (rt5640_read(driver, RT5640_INT_IRQ_ST, &status) == B_OK
			&& rt5640_read(driver, RT5640_GEN_CTRL2, &gen2) == B_OK) {
			if (status != last || (gen2 & (1 << 11)) != lastJD2) {
				TRACE_ALWAYS("jack detect: JD1 %s (MX-BF %04x, MX-FB %04x)\n",
					(status & RT5640_STATUS_JD) != 0 ? "high" : "low",
					status, gen2);
				last = status;
				lastJD2 = gen2 & (1 << 11);
			}
		}
		snooze(JACK_POLL_INTERVAL);
	}
	return B_OK;
}


//	#pragma mark - driver module API


static float
intel_lpe_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK
		|| strcmp(bus, "i2c") != 0) {
		return 0.0;
	}

	const char* hid;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid,
			false) != B_OK
		|| strcmp(hid, "10EC5640") != 0) {
		return 0.0;
	}

	return 0.6;
}


static status_t
intel_lpe_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Intel LPE audio (RT5640)" }},
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, INTEL_LPE_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
intel_lpe_init_driver(device_node* node, void** _driverCookie)
{
	intel_lpe_driver* driver = new(std::nothrow) intel_lpe_driver;
	if (driver == NULL)
		return B_NO_MEMORY;
	memset(driver, 0, sizeof(*driver));
	driver->node = node;
	driver->pollThread = -1;

	device_node* parent = sDeviceManager->get_parent_node(node);
	uint64 handle = 0;
	if (sDeviceManager->get_attr_uint64(parent, ACPI_DEVICE_HANDLE_ITEM,
			&handle, false) == B_OK) {
		driver->codecHandle = (acpi_handle)(addr_t)handle;
	}
	status_t status = sDeviceManager->get_driver(parent,
		(driver_module_info**)&driver->i2c, (void**)&driver->i2cCookie);
	sDeviceManager->put_node(parent);

	if (status == B_OK)
		status = rt5640_probe(driver);
	if (status != B_OK) {
		TRACE_ALWAYS("probe failed: %s\n", strerror(status));
		delete driver;
		return status;
	}

	driver->pollThread = spawn_kernel_thread(jack_poll_thread,
		"intel_lpe poll", B_LOW_PRIORITY, driver);
	if (driver->pollThread >= 0)
		resume_thread(driver->pollThread);

	*_driverCookie = driver;
	return B_OK;
}


static void
intel_lpe_uninit_driver(void* driverCookie)
{
	intel_lpe_driver* driver = (intel_lpe_driver*)driverCookie;
	if (driver->pollThread >= 0) {
		driver->stopPolling = true;
		status_t result;
		wait_for_thread(driver->pollThread, &result);
	}
	delete driver;
}


driver_module_info intel_lpe_driver_module = {
	{
		INTEL_LPE_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	intel_lpe_supports_device,
	intel_lpe_register_device,
	intel_lpe_init_driver,
	intel_lpe_uninit_driver,
	NULL,	// register_child_devices
	NULL,	// rescan
	NULL,	// removed
};


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


module_info* modules[] = {
	(module_info*)&intel_lpe_driver_module,
	NULL
};
