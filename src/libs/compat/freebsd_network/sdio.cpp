/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


/*!	The "sdio" bus of the FreeBSD compat layer: finds the SDIO functions that
	the mmc bus manager has published, offers them to the drivers, and gives
	the drivers access to them (dev/sdio/sdio_haiku.h).
*/


extern "C" {
#include <sys/bus.h>
#include <sys/systm.h>

#include <dev/sdio/sdio_haiku.h>

#include "device.h"
}

#include <algorithm>
#include <new>

#include <KernelExport.h>
#include <device_manager.h>

#include <mmc.h>


struct sdio_func {
	device_node*			node;
	mmc_device_interface*	mmc;
	void*					busCookie;
	uint16					rca;
	uint8					number;
	uint16					blockSize;

	// interrupt handling, see sdio_setup_intr()
	void					(*handler)(void*);
	void*					handlerArg;
	sem_id					interruptSem;
	thread_id				interruptThread;
	volatile bool			stopInterruptThread;
};

struct compat_sdio_device {
	struct sdio_func		func;
	struct sdio_attach_args	args;
};


static device_manager_info* sDeviceManager;


//	#pragma mark - access


static status_t
direct_io(struct sdio_func* func, bool isWrite, uint8 function,
	uint32_t address, uint8 value, uint8* _result)
{
	return func->mmc->sdio_rw_direct(func->node, func->busCookie, func->rca,
		isWrite, function, address, value, _result);
}


static int
extended_io(struct sdio_func* func, bool isWrite, uint32_t address,
	uint8* buffer, size_t length, bool incrementAddress)
{
	const uint32 blockSize = func->blockSize;

	while (length > 0) {
		uint32 blockCount = 0;
		uint32 chunk;
		if (blockSize > 0 && length >= blockSize) {
			// block mode, as many whole blocks as fit into one call
			blockCount = std::min((size_t)(B_PAGE_SIZE / blockSize),
				length / blockSize);
			if (blockCount > 511)
				blockCount = 511;
			chunk = blockCount * blockSize;
		} else {
			// byte mode for the rest
			chunk = std::min(length,
				(size_t)(blockSize > 0 ? std::min(blockSize, 512u) : 512u));
		}

		status_t status = func->mmc->sdio_rw_extended(func->node,
			func->busCookie, func->rca, isWrite, func->number, address,
			incrementAddress, buffer, blockCount,
			blockCount > 0 ? blockSize : chunk);
		if (status != B_OK)
			return EIO;

		buffer += chunk;
		length -= chunk;
		if (incrementAddress)
			address += chunk;
	}

	return 0;
}


extern "C" struct sdio_func*
sdio_get_function(device_t dev)
{
	struct sdio_attach_args* args
		= (struct sdio_attach_args*)device_get_ivars(dev);
	return args != NULL ? args->func : NULL;
}


extern "C" uint8_t
sdio_read_1(struct sdio_func* func, uint32_t addr, int* err)
{
	uint8 value = 0xff;
	status_t status = direct_io(func, false, func->number, addr, 0, &value);
	if (err != NULL)
		*err = status == B_OK ? 0 : EIO;
	return value;
}


extern "C" void
sdio_write_1(struct sdio_func* func, uint32_t addr, uint8_t value, int* err)
{
	status_t status = direct_io(func, true, func->number, addr, value, NULL);
	if (err != NULL)
		*err = status == B_OK ? 0 : EIO;
}


extern "C" uint8_t
sdio_f0_read_1(struct sdio_func* func, uint32_t addr, int* err)
{
	uint8 value = 0xff;
	status_t status = direct_io(func, false, 0, addr, 0, &value);
	if (err != NULL)
		*err = status == B_OK ? 0 : EIO;
	return value;
}


extern "C" void
sdio_f0_write_1(struct sdio_func* func, uint32_t addr, uint8_t value,
	int* err)
{
	status_t status = direct_io(func, true, 0, addr, value, NULL);
	if (err != NULL)
		*err = status == B_OK ? 0 : EIO;
}


extern "C" uint16_t
sdio_read_2(struct sdio_func* func, uint32_t addr, int* err)
{
	uint8 bytes[4] = {};
	int result = sdio_read_bytes(func, addr, bytes, 2, true);
	if (err != NULL)
		*err = result;
	return bytes[0] | (bytes[1] << 8);
}


extern "C" void
sdio_write_2(struct sdio_func* func, uint32_t addr, uint16_t value, int* err)
{
	uint8 bytes[2] = { (uint8)value, (uint8)(value >> 8) };
	int result = sdio_write_bytes(func, addr, bytes, 2, true);
	if (err != NULL)
		*err = result;
}


extern "C" uint32_t
sdio_read_4(struct sdio_func* func, uint32_t addr, int* err)
{
	uint8 bytes[4] = {};
	int result = sdio_read_bytes(func, addr, bytes, 4, true);
	if (err != NULL)
		*err = result;
	return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16)
		| ((uint32)bytes[3] << 24);
}


extern "C" void
sdio_write_4(struct sdio_func* func, uint32_t addr, uint32_t value, int* err)
{
	uint8 bytes[4] = { (uint8)value, (uint8)(value >> 8),
		(uint8)(value >> 16), (uint8)(value >> 24) };
	int result = sdio_write_bytes(func, addr, bytes, 4, true);
	if (err != NULL)
		*err = result;
}


extern "C" int
sdio_read_bytes(struct sdio_func* func, uint32_t addr, void* buffer,
	size_t length, bool incrementAddress)
{
	return extended_io(func, false, addr, (uint8*)buffer, length,
		incrementAddress);
}


extern "C" int
sdio_write_bytes(struct sdio_func* func, uint32_t addr, const void* buffer,
	size_t length, bool incrementAddress)
{
	return extended_io(func, true, addr, (uint8*)buffer, length,
		incrementAddress);
}


/*!	Sets IOEx of the function in the CCCR and waits for IORx (SDIO
	Simplified Specification 3.00, 6.9).
*/
extern "C" int
sdio_enable_func(struct sdio_func* func)
{
	uint8 enabled;
	if (direct_io(func, false, 0, 0x02, 0, &enabled) != B_OK)
		return EIO;
	enabled |= 1 << func->number;
	if (direct_io(func, true, 0, 0x02, enabled, NULL) != B_OK)
		return EIO;

	bigtime_t deadline = system_time() + 1000000;
	while (system_time() < deadline) {
		uint8 ready;
		if (direct_io(func, false, 0, 0x03, 0, &ready) != B_OK)
			return EIO;
		if ((ready & (1 << func->number)) != 0)
			return 0;
		snooze(10000);
	}

	dprintf("sdio: function %u did not get ready\n", func->number);
	return ETIMEDOUT;
}


extern "C" int
sdio_disable_func(struct sdio_func* func)
{
	uint8 enabled;
	if (direct_io(func, false, 0, 0x02, 0, &enabled) != B_OK)
		return EIO;
	enabled &= ~(1 << func->number);
	return direct_io(func, true, 0, 0x02, enabled, NULL) == B_OK ? 0 : EIO;
}


/*!	Sets the I/O block size of the function in its FBR (6.10). */
extern "C" int
sdio_set_block_size(struct sdio_func* func, uint16_t size)
{
	if (size == 0 || size > 2048)
		return EINVAL;

	uint32 fbr = func->number * 0x100;
	if (direct_io(func, true, 0, fbr + 0x10, size & 0xff, NULL) != B_OK
		|| direct_io(func, true, 0, fbr + 0x11, size >> 8, NULL) != B_OK) {
		return EIO;
	}

	func->blockSize = size;
	return 0;
}


extern "C" uint16_t
sdio_get_block_size(struct sdio_func* func)
{
	return func->blockSize;
}


//	#pragma mark - interrupts


static void
card_interrupt(void* cookie)
{
	// interrupt context: let the thread do the work
	struct sdio_func* func = (struct sdio_func*)cookie;
	release_sem_etc(func->interruptSem, 1, B_DO_NOT_RESCHEDULE);
}


static status_t
interrupt_thread(void* cookie)
{
	struct sdio_func* func = (struct sdio_func*)cookie;

	while (true) {
		status_t status = acquire_sem(func->interruptSem);
		if (status != B_OK || func->stopInterruptThread)
			break;

		func->handler(func->handlerArg);

		// the handler has cleared the source, take the next one
		func->mmc->sdio_set_interrupt_handler(func->node, func->busCookie,
			&card_interrupt, func);
	}

	return B_OK;
}


/*!	Enables the interrupt of the function in the CCCR (IENx and IENM) and
	calls \a handler in a thread whenever the card interrupt fires.
*/
extern "C" int
sdio_setup_intr(struct sdio_func* func, void (*handler)(void*), void* arg)
{
	if (func->mmc->sdio_set_interrupt_handler == NULL)
		return EOPNOTSUPP;
	if (func->interruptThread >= 0)
		return EBUSY;

	func->handler = handler;
	func->handlerArg = arg;
	func->stopInterruptThread = false;
	func->interruptSem = create_sem(0, "sdio interrupt");
	if (func->interruptSem < 0)
		return ENOMEM;

	func->interruptThread = spawn_kernel_thread(&interrupt_thread,
		"sdio interrupt", B_REAL_TIME_DISPLAY_PRIORITY, func);
	if (func->interruptThread < 0) {
		delete_sem(func->interruptSem);
		func->interruptSem = -1;
		return ENOMEM;
	}
	resume_thread(func->interruptThread);

	uint8 enabled;
	if (direct_io(func, false, 0, 0x04, 0, &enabled) != B_OK
		|| direct_io(func, true, 0, 0x04, enabled | 1 | (1 << func->number),
			NULL) != B_OK) {
		sdio_teardown_intr(func);
		return EIO;
	}

	if (func->mmc->sdio_set_interrupt_handler(func->node, func->busCookie,
			&card_interrupt, func) != B_OK) {
		sdio_teardown_intr(func);
		return EOPNOTSUPP;
	}

	return 0;
}


extern "C" void
sdio_teardown_intr(struct sdio_func* func)
{
	if (func->interruptThread < 0)
		return;

	func->mmc->sdio_set_interrupt_handler(func->node, func->busCookie, NULL,
		NULL);

	uint8 enabled;
	if (direct_io(func, false, 0, 0x04, 0, &enabled) == B_OK) {
		enabled &= ~(1 << func->number);
		if ((enabled & 0xfe) == 0)
			enabled = 0;
		direct_io(func, true, 0, 0x04, enabled, NULL);
	}

	func->stopInterruptThread = true;
	delete_sem(func->interruptSem);
	status_t result;
	wait_for_thread(func->interruptThread, &result);
	func->interruptThread = -1;
	func->interruptSem = -1;
}


//	#pragma mark - bus


status_t
init_sdio()
{
	if (sDeviceManager != NULL)
		return B_OK;

	return get_module(B_DEVICE_MANAGER_MODULE_NAME,
		(module_info**)&sDeviceManager);
}


void
uninit_sdio()
{
	if (sDeviceManager == NULL)
		return;

	put_module(B_DEVICE_MANAGER_MODULE_NAME);
	sDeviceManager = NULL;
}


static void
free_compat_sdio_device(void* cookie)
{
	free(cookie);
}


static void
prepare_sdio_attach(void* cookie, device_t device)
{
	compat_sdio_device* compat = (compat_sdio_device*)cookie;
	device_set_ivars(device, &compat->args);
}


/*!	Fills in \a device for the SDIO function \a node, if it is one. */
static bool
get_sdio_function(device_node* node, compat_sdio_device* device)
{
	uint8 function;
	uint16 vendor;
	uint16 deviceId;
	uint16 rca;
	if (sDeviceManager->get_attr_uint8(node, kSdioFunctionAttribute,
			&function, false) != B_OK
		|| sDeviceManager->get_attr_uint16(node, kSdioVendorAttribute,
			&vendor, false) != B_OK
		|| sDeviceManager->get_attr_uint16(node, kSdioDeviceAttribute,
			&deviceId, false) != B_OK
		|| sDeviceManager->get_attr_uint16(node, kMmcRcaAttribute, &rca,
			false) != B_OK) {
		return false;
	}
	uint8 classCode = 0;
	sDeviceManager->get_attr_uint8(node, kSdioClassAttribute, &classCode,
		false);

	// The function node itself is driven by the mmc_device_interface; the
	// bus object is the cookie of its parent, the mmc bus node.
	mmc_device_interface* mmc;
	void* unused;
	if (sDeviceManager->get_driver(node, (driver_module_info**)&mmc, &unused)
			!= B_OK || mmc->sdio_rw_direct == NULL
		|| mmc->sdio_rw_extended == NULL) {
		return false;
	}
	device_node* busNode = sDeviceManager->get_parent_node(node);
	driver_module_info* busModule;
	void* busCookie = NULL;
	status_t status = sDeviceManager->get_driver(busNode, &busModule,
		&busCookie);
	sDeviceManager->put_node(busNode);
	if (status != B_OK || busCookie == NULL)
		return false;

	memset(device, 0, sizeof(*device));
	device->func.node = node;
	device->func.mmc = mmc;
	device->func.busCookie = busCookie;
	device->func.rca = rca;
	device->func.number = function;
	device->func.interruptSem = -1;
	device->func.interruptThread = -1;
	device->args.vendor = vendor;
	device->args.device = deviceId;
	device->args.function = function;
	device->args.class_code = classCode;
	device->args.func = &device->func;
	return true;
}


/*!	Walks the device tree below \a parent and offers every SDIO function to
	the drivers.
*/
static void
probe_sdio_nodes(device_node* parent, device_t root, driver_t* drivers[],
	bool* _found)
{
	// An empty attribute list matches every child; NULL matches none.
	const device_attr anyChild[] = { { NULL } };
	device_node* node = NULL;
	while (sDeviceManager->get_next_child_node(parent, anyChild, &node)
			== B_OK) {
		compat_sdio_device probe;
		if (get_sdio_function(node, &probe)) {
			dprintf("sdio: function %u of %04x:%04x\n", probe.args.function,
				probe.args.vendor, probe.args.device);
			struct device device = {};
			device.parent = root;
			device.root = root;
			device_set_ivars(&device, &probe.args);

			driver_t* driver = __haiku_probe_drivers(&device, drivers);
			if (driver != NULL) {
				compat_sdio_device* compat = (compat_sdio_device*)malloc(
					sizeof(compat_sdio_device));
				if (compat != NULL) {
					*compat = probe;
					compat->args.func = &compat->func;
					// The node pointer is kept without a reference: the
					// device manager has no call to take one, and the node
					// of a function lives as long as its card.
					report_probed_device(
						root_device_softc::BUS_sdio, compat, driver,
						prepare_sdio_attach, free_compat_sdio_device);
					*_found = true;
				}
			}
		}

		probe_sdio_nodes(node, root, drivers, _found);
	}
}


status_t
_fbsd_init_hardware_sdio(driver_t* drivers[])
{
	status_t status = init_sdio();
	if (status != B_OK)
		return status;

	device_t root;
	status = init_root_device(&root, root_device_softc::BUS_sdio);
	if (status != B_OK)
		return status;

	bool found = false;
	device_node* rootNode = sDeviceManager->get_root_node();
	probe_sdio_nodes(rootNode, root, drivers, &found);
	sDeviceManager->put_node(rootNode);

	device_delete_child(NULL, root);

	if (found)
		return B_OK;

	uninit_sdio();
	return B_NOT_SUPPORTED;
}
