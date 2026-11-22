/*
 * Copyright 2022, Jérôme Duval. All rights reserved.
 *
 * Distributed under the terms of the MIT License.
 */


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <Drivers.h>
#include <KernelExport.h>
#include <OS.h>

#include <kernel.h>

#include "ACPIPrivate.h"


#define	TRACE(x...)			//dprintf("acpi_call: " x)
#define TRACE_ALWAYS(x...)	dprintf("acpi_call: " x)
#define CALLED() 			TRACE("CALLED %s\n", __PRETTY_FUNCTION__)

#define MAX_CALL_PATH_LEN		1024
#define MAX_CALL_ARGS			7
#define MAX_CALL_ARG_DATA_LEN	65536
#define MAX_CALL_PACKAGE_DEPTH	8


typedef struct {
	device_node *node;
	acpi_root_info	*acpi;
	void	*acpi_cookie;
} acpi_call_device_info;


struct acpi_call_desc
{
	char*			path;
	acpi_objects	args;
	acpi_status	retval;
	acpi_data	result;
	acpi_size	reslen;
};


//	#pragma mark - device module API


static status_t
acpi_call_init_device(void* _node, void** _cookie)
{
	CALLED();
	device_node *node = (device_node *)_node;

	acpi_call_device_info* device = (acpi_call_device_info*)calloc(1, sizeof(acpi_call_device_info));
	if (device == NULL)
		return B_NO_MEMORY;

	device->node = node;
	status_t err = gDeviceManager->get_driver(node, (driver_module_info **)&device->acpi,
		(void **)&device->acpi_cookie);
	if (err != B_OK) {
		free(device);
		return err;
	}

	*_cookie = device;
	return err;
}


static void
acpi_call_uninit_device(void* _cookie)
{
	CALLED();
	acpi_call_device_info* device = (acpi_call_device_info*)_cookie;
	free(device);
}


static status_t
acpi_call_open(void* _device, const char* path, int openMode, void** _cookie)
{
	CALLED();
	if (geteuid() != 0)
		return B_PERMISSION_DENIED;

	acpi_call_device_info* device = (acpi_call_device_info*)_device;

	*_cookie = device;
	return B_OK;
}


static status_t
acpi_call_read(void *cookie, off_t position, void *buffer, size_t *numBytes)
{
	TRACE("read(%p, %" B_PRIdOFF", %p, %lu)\n", cookie, position, buffer, *numBytes);
	return B_ERROR;
}


static status_t
acpi_call_write(void *cookie, off_t position, const void *buffer,
	size_t *numBytes)
{
	TRACE("write(%p, %" B_PRIdOFF", %p, %lu)\n", cookie, position, buffer, *numBytes);
	return B_ERROR;
}


static void
acpi_call_free_args(acpi_object_type* args, uint32 count)
{
	for (uint32 i = 0; i < count; i++) {
		switch (args[i].object_type) {
			case ACPI_TYPE_STRING:
				free(args[i].string.string);
				args[i].string.string = NULL;
				break;
			case ACPI_TYPE_BUFFER:
				free(args[i].buffer.buffer);
				args[i].buffer.buffer = NULL;
				break;
		}
	}
}


static status_t
acpi_call_copy_args(const acpi_objects& userArgs, acpi_object_type* kernelArgs)
{
	if (userArgs.count == 0)
		return B_OK;
	if (userArgs.count > MAX_CALL_ARGS)
		return B_BAD_VALUE;
	if (userArgs.pointer == NULL || !IS_USER_ADDRESS(userArgs.pointer))
		return B_BAD_ADDRESS;

	if (user_memcpy(kernelArgs, userArgs.pointer,
			userArgs.count * sizeof(acpi_object_type)) != B_OK) {
		return B_BAD_ADDRESS;
	}

	for (uint32 i = 0; i < userArgs.count; i++) {
		switch (kernelArgs[i].object_type) {
			case ACPI_TYPE_INTEGER:
				break;

			case ACPI_TYPE_STRING:
			{
				uint32 len = kernelArgs[i].string.len;
				const char* userString = kernelArgs[i].string.string;
				if (len > MAX_CALL_ARG_DATA_LEN) {
					acpi_call_free_args(kernelArgs, i);
					return B_BAD_VALUE;
				}
				if (len > 0
					&& (userString == NULL || !IS_USER_ADDRESS(userString))) {
					acpi_call_free_args(kernelArgs, i);
					return B_BAD_ADDRESS;
				}
				char* kernelString = (char*)malloc((size_t)len + 1);
				if (kernelString == NULL) {
					acpi_call_free_args(kernelArgs, i);
					return B_NO_MEMORY;
				}
				if (len > 0
					&& user_memcpy(kernelString, userString, len) != B_OK) {
					free(kernelString);
					acpi_call_free_args(kernelArgs, i);
					return B_BAD_ADDRESS;
				}
				kernelString[len] = '\0';
				kernelArgs[i].string.string = kernelString;
				break;
			}

			case ACPI_TYPE_BUFFER:
			{
				uint32 len = kernelArgs[i].buffer.length;
				const void* userBuffer = kernelArgs[i].buffer.buffer;
				if (len > MAX_CALL_ARG_DATA_LEN) {
					acpi_call_free_args(kernelArgs, i);
					return B_BAD_VALUE;
				}
				if (len > 0
					&& (userBuffer == NULL || !IS_USER_ADDRESS(userBuffer))) {
					acpi_call_free_args(kernelArgs, i);
					return B_BAD_ADDRESS;
				}
				void* kernelBuffer = malloc(len > 0 ? len : 1);
				if (kernelBuffer == NULL) {
					acpi_call_free_args(kernelArgs, i);
					return B_NO_MEMORY;
				}
				if (len > 0) {
					if (user_memcpy(kernelBuffer, userBuffer, len) != B_OK) {
						free(kernelBuffer);
						acpi_call_free_args(kernelArgs, i);
						return B_BAD_ADDRESS;
					}
				} else
					*(uint8*)kernelBuffer = 0;

				kernelArgs[i].buffer.buffer = kernelBuffer;
				break;
			}

			default:
				acpi_call_free_args(kernelArgs, i);
				return B_BAD_VALUE;
		}
	}

	return B_OK;
}


static void*
acpi_call_rebase_pointer(const void* kernelPtr, size_t dataLength,
	const uint8* base, size_t copiedLength, uint8* target)
{
	if (kernelPtr == NULL)
		return NULL;

	addr_t ptrAddr = (addr_t)kernelPtr;
	addr_t baseAddr = (addr_t)base;
	if (ptrAddr < baseAddr)
		return NULL;

	size_t offset = ptrAddr - baseAddr;
	if (offset > copiedLength || dataLength > copiedLength - offset)
		return NULL;

	return target + offset;
}


static void
acpi_call_fixup_pointers(acpi_object_type* object, const uint8* base,
	size_t totalLength, size_t copiedLength, uint8* target, uint32 depth)
{
	CALLED();
	switch (object->object_type) {
		case ACPI_TYPE_STRING:
			if (object->string.len < copiedLength) {
				object->string.string = (char*)acpi_call_rebase_pointer(
					object->string.string, (size_t)object->string.len + 1,
					base, copiedLength, target);
			} else
				object->string.string = NULL;

			if (object->string.string == NULL)
				object->string.len = 0;
			break;

		case ACPI_TYPE_BUFFER:
			object->buffer.buffer = acpi_call_rebase_pointer(
				object->buffer.buffer, object->buffer.length,
				base, copiedLength, target);
			if (object->buffer.buffer == NULL)
				object->buffer.length = 0;
			break;

		case ACPI_TYPE_PACKAGE:
		{
			uint32 count = object->package.count;
			acpi_object_type* elements = object->package.objects;
			if (depth < MAX_CALL_PACKAGE_DEPTH
				&& (size_t)count <= totalLength / sizeof(acpi_object_type)
				&& acpi_call_rebase_pointer(elements,
					(size_t)count * sizeof(acpi_object_type),
					base, totalLength, target) != NULL) {
				for (uint32 i = 0; i < count; i++) {
					acpi_call_fixup_pointers(&elements[i], base, totalLength,
						copiedLength, target, depth + 1);
				}
			}
			if (depth >= MAX_CALL_PACKAGE_DEPTH
				|| (size_t)count > copiedLength / sizeof(acpi_object_type)) {
				object->package.objects = NULL;
				object->package.count = 0;
				break;
			}
			acpi_object_type* userElements
				= (acpi_object_type*)acpi_call_rebase_pointer(elements,
					(size_t)count * sizeof(acpi_object_type),
					base, copiedLength, target);
			if (userElements == NULL) {
				object->package.objects = NULL;
				object->package.count = 0;
				break;
			}
			object->package.objects = userElements;
			break;
		}

		case ACPI_TYPE_LOCAL_REFERENCE:
			object->reference.handle = NULL;
			break;
	}
}


static status_t
acpi_call_control(void *_device, uint32 op, void *buffer, size_t length)
{
	TRACE("control(%p, %" B_PRIu32 ", %p, %lu)\n", _device, op, buffer, length);
	acpi_call_device_info* device = (acpi_call_device_info*)_device;

	if (op == 'ACCA') {
		struct acpi_call_desc params;
		char path[MAX_CALL_PATH_LEN];
		if (buffer == NULL || !IS_USER_ADDRESS(buffer)
			|| user_memcpy(&params, buffer, sizeof(params)) != B_OK
			|| params.path == NULL || !IS_USER_ADDRESS(params.path)) {
			return B_BAD_ADDRESS;
		}

		ssize_t pathLength = user_strlcpy(path, params.path, sizeof(path));
		if (pathLength < 0)
			return B_BAD_ADDRESS;
		if ((size_t)pathLength >= sizeof(path))
			return B_NAME_TOO_LONG;

		acpi_object_type kernelArgObjects[MAX_CALL_ARGS];
		status_t status = acpi_call_copy_args(params.args, kernelArgObjects);
		if (status != B_OK)
			return status;

		acpi_objects kernelArgs;
		kernelArgs.count = params.args.count;
		kernelArgs.pointer = params.args.count > 0 ? kernelArgObjects : NULL;

		acpi_data result;
		result.length = ACPI_ALLOCATE_BUFFER;
		result.pointer = NULL;

		params.retval = device->acpi->evaluate_method(NULL, path, &kernelArgs,
			&result);
		acpi_call_free_args(kernelArgObjects, kernelArgs.count);
		params.reslen = 0;

		if (params.retval == B_OK && result.pointer != NULL) {
			params.reslen = result.length;
			if (params.result.pointer != NULL) {
				if (!IS_USER_ADDRESS(params.result.pointer)) {
					free(result.pointer);
					return B_BAD_ADDRESS;
				}
				size_t copyLength = min_c(params.result.length, result.length);
				if (result.length >= sizeof(acpi_object_type)) {
					acpi_call_fixup_pointers(
						(acpi_object_type*)result.pointer,
						(const uint8*)result.pointer, result.length,
						copyLength, (uint8*)params.result.pointer, 0);
				}
				params.result.length = copyLength;
				if (user_memcpy(params.result.pointer, result.pointer,
						copyLength) != B_OK) {
					free(result.pointer);
					return B_BAD_ADDRESS;
				}
			}
			free(result.pointer);
		} else {
			free(result.pointer);
		}

		if (user_memcpy(buffer, &params, sizeof(params)) != B_OK)
			return B_BAD_ADDRESS;

		return B_OK;
	}

	return B_ERROR;
}


static status_t
acpi_call_close(void *cookie)
{
	TRACE("close(%p)\n", cookie);
	return B_OK;
}


static status_t
acpi_call_free(void *cookie)
{
	TRACE("free(%p)\n", cookie);
	return B_OK;
}


//	#pragma mark -


struct device_module_info gAcpiCallDeviceModule = {
	{
		ACPI_CALL_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	acpi_call_init_device,
	acpi_call_uninit_device,
	NULL, // remove,

	acpi_call_open,
	acpi_call_close,
	acpi_call_free,
	acpi_call_read,
	acpi_call_write,
	NULL,	// io
	acpi_call_control,

	NULL,	// select
	NULL,	// deselect
};
