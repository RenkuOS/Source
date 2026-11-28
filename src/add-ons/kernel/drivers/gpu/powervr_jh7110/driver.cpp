/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * Driver of the Imagination PowerVR BXE-4-32 GPU of the StarFive JH7110.
 * See headers/private/drivers/powervr_jh7110.h for the interface.
 *
 * The GPU and its firmware start with the first open and stop with the last
 * close. Each open file is a Client: a GPU address space (MMU context and
 * firmware memory context), the buffers it created, what it mapped and its
 * compute contexts. Jobs complete through the timeline UFO of their
 * context, which the firmware writes; the firmware interrupt wakes the
 * waiters.
 */


#include <new>
#include <stdlib.h>
#include <string.h>

#include <AutoDeleter.h>
#include <Drivers.h>
#include <KernelExport.h>
#include <kernel.h>
#include <lock.h>
#include <team.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>
#include <vm/vm.h>

#include <powervr_jh7110.h>

#include "PowerVRDevice.h"


extern "C" {
#include <libfdt.h>
}


#define DEVICE_NAME			"gpu/powervr_jh7110"
#define MAX_JOBS_IN_FLIGHT	128
	// a compute job takes 184 bytes of the 32 KB client CCB


extern void* gFDT;

int32 api_version = B_CUR_DRIVER_API_VERSION;

static const char* sDeviceNames[] = { DEVICE_NAME, NULL };
static PowerVRDevice* sDevice;


struct Buffer : DoublyLinkedListLinkImpl<Buffer> {
	int32			handle;
	area_id			kernelArea;
	area_id			userArea;
	team_id			team;
	size_t			size;
	physical_entry*	runs;
	uint32			runCount;
	uint32			mappingCount;
};

struct Mapping : DoublyLinkedListLinkImpl<Mapping> {
	Buffer*			buffer;
	uint64			deviceAddress;
	uint64			offset;
	uint64			size;
};

struct Context : DoublyLinkedListLinkImpl<Context> {
	int32			handle;
	uint32			id;				// server_common_context_id
	pvr_gpu_context	gpu;
	uint32			lastSequence;
	int32			waiters;		// POWERVR_WAIT without the device lock
	bool			released;		// the firmware let go of it
	// reported by the next POWERVR_WAIT
	uint32			resetFlags;
	uint32			resetReason;
	uint64			faultAddress;
};

typedef DoublyLinkedList<Buffer> BufferList;
typedef DoublyLinkedList<Mapping> MappingList;
typedef DoublyLinkedList<Context> ContextList;

struct Client : DoublyLinkedListLinkImpl<Client> {
	pvr_mmu_context*	mmu;
	pvr_fw_mem_context	memory;
	bool				memoryCreated;
	BufferList			buffers;
	MappingList			mappings;
	ContextList			contexts;
	int32				nextHandle;
	uint32				jobsSubmitted;
	uint32				jobsCompleted;
};

typedef DoublyLinkedList<Client> ClientList;

static ClientList sClients;
static ClientList sParkedClients;
	// clients whose contexts the firmware did not release: everything they
	// own stays until the firmware is halted


//	#pragma mark - buffers and mappings


static Buffer*
find_buffer(Client* client, int32 handle)
{
	for (BufferList::Iterator iterator = client->buffers.GetIterator();
			Buffer* buffer = iterator.Next();) {
		if (buffer->handle == handle)
			return buffer;
	}
	return NULL;
}


static Context*
find_context(Client* client, int32 handle)
{
	for (ContextList::Iterator iterator = client->contexts.GetIterator();
			Context* context = iterator.Next();) {
		if (context->handle == handle)
			return context;
	}
	return NULL;
}


static void
sync_buffer(Buffer* buffer)
{
	for (uint32 i = 0; i < buffer->runCount; i++)
		sDevice->Sync(buffer->runs[i].address, buffer->runs[i].size);
}


//!	Writes back (and invalidates) every buffer the GPU can reach.
static void
sync_mapped_buffers(Client* client)
{
	for (BufferList::Iterator iterator = client->buffers.GetIterator();
			Buffer* buffer = iterator.Next();) {
		if (buffer->mappingCount > 0)
			sync_buffer(buffer);
	}
}


static status_t
create_buffer(Client* client, uint64 size, Buffer** _buffer, void** _address)
{
	if (size == 0 || size > 512 * 1024 * 1024)
		return B_BAD_VALUE;
	size = ROUNDUP(size, B_PAGE_SIZE);

	Buffer* buffer = new(std::nothrow) Buffer;
	if (buffer == NULL)
		return B_NO_MEMORY;
	ObjectDeleter<Buffer> bufferDeleter(buffer);

	void* address;
	buffer->kernelArea = create_area("powervr buffer", &address,
		B_ANY_KERNEL_ADDRESS, size, B_32_BIT_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (buffer->kernelArea < 0)
		return buffer->kernelArea;

	uint32 maxRuns = size / B_PAGE_SIZE;
	buffer->runs = (physical_entry*)malloc(maxRuns * sizeof(physical_entry));
	uint32 runCount = maxRuns;
	status_t status = buffer->runs != NULL
		? get_memory_map_etc(B_CURRENT_TEAM, address, size, buffer->runs,
			&runCount)
		: B_NO_MEMORY;
	if (status != B_OK) {
		free(buffer->runs);
		delete_area(buffer->kernelArea);
		return status;
	}
	buffer->runCount = runCount;
	buffer->size = size;
	buffer->mappingCount = 0;

	// zeroed and out of the cache: a dirty line evicted later would land
	// on what the GPU wrote
	memset(address, 0, size);
	sync_buffer(buffer);

	buffer->team = team_get_current_team_id();
	buffer->userArea = vm_clone_area(buffer->team, "powervr buffer",
		_address, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA,
		REGION_NO_PRIVATE_MAP, buffer->kernelArea, true);
	if (buffer->userArea < 0) {
		status = buffer->userArea;
		free(buffer->runs);
		delete_area(buffer->kernelArea);
		return status;
	}

	buffer->handle = ++client->nextHandle;
	client->buffers.Add(buffer);
	*_buffer = bufferDeleter.Detach();
	return B_OK;
}


static void
delete_buffer(Client* client, Buffer* buffer)
{
	client->buffers.Remove(buffer);
	vm_change_clones_to_null_areas(buffer->kernelArea);
	if (buffer->userArea >= 0)
		vm_delete_area(buffer->team, buffer->userArea, true);
	delete_area(buffer->kernelArea);
	free(buffer->runs);
	delete buffer;
}


//!	Maps [offset, offset + size) of the buffer, run by run.
static status_t
map_buffer(Client* client, Buffer* buffer, uint64 offset, uint64 size,
	uint64 deviceAddress, uint32 flags)
{
	uint64 runStart = 0;
	uint64 done = 0;
	status_t status = B_OK;
	for (uint32 i = 0; i < buffer->runCount && done < size; i++) {
		uint64 runSize = buffer->runs[i].size;
		uint64 runEnd = runStart + runSize;
		if (runEnd > offset + done) {
			uint64 skip = offset + done - runStart;
			uint64 length = min_c(runSize - skip, size - done);
			status = pvr_mmu_map(client->mmu, deviceAddress + done,
				buffer->runs[i].address + skip, length, flags);
			if (status != B_OK)
				break;
			done += length;
		}
		runStart = runEnd;
	}
	if (status != B_OK && done > 0)
		pvr_mmu_unmap(client->mmu, deviceAddress, done);
	return status;
}


static void
remove_mapping(Client* client, Mapping* mapping)
{
	pvr_mmu_unmap(client->mmu, mapping->deviceAddress, mapping->size);
	mapping->buffer->mappingCount--;
	client->mappings.Remove(mapping);
	delete mapping;
}


static bool
overlaps_mapping(Client* client, uint64 deviceAddress, uint64 size)
{
	for (MappingList::Iterator iterator = client->mappings.GetIterator();
			Mapping* mapping = iterator.Next();) {
		if (deviceAddress < mapping->deviceAddress + mapping->size
			&& mapping->deviceAddress < deviceAddress + size) {
			return true;
		}
	}
	return false;
}


//	#pragma mark - contexts and jobs


static status_t
create_context(Client* client, const powervr_create_context& request,
	Context** _context)
{
	if (request.type != POWERVR_CONTEXT_COMPUTE || request.priority > 2)
		return B_BAD_VALUE;

	Context* context = new(std::nothrow) Context;
	if (context == NULL)
		return B_NO_MEMORY;
	memset(&context->gpu, 0, sizeof(context->gpu));

	pvr_ctx_params params;
	memset(&params, 0, sizeof(params));
	params.type = PVR_CTX_TYPE_COMPUTE;
	params.priority = request.priority;
	params.cccb_size_log2 = PVR_CTX_CCCB_SIZE_LOG2_LINUX;
	params.ctx_id = context->id = sDevice->NextContextID();
	params.pid = team_get_current_team_id();

	const pvr_ctx_env& env = sDevice->ContextEnvironment();
	status_t status = pvr_gpu_context_create(env, &client->memory, params,
		&context->gpu);
	if (status == B_OK) {
		// new pages in the MIPS page table: flush the firmware's MMU caches
		status = pvr_mmu_flush_exec(env, sDevice->Firmware(), client->mmu,
			PVR_MMU_SYNC_LEVEL_2_FLAGS);
	}
	if (status != B_OK) {
		pvr_gpu_context_destroy(env, &context->gpu);
		delete context;
		return status;
	}

	context->handle = ++client->nextHandle;
	context->lastSequence = 0;
	context->waiters = 0;
	context->released = false;
	context->resetFlags = 0;
	context->resetReason = 0;
	context->faultAddress = 0;
	client->contexts.Add(context);
	*_context = context;
	return B_OK;
}


/*!	Asks the firmware to release the context, then frees it. If the
	firmware keeps it, nothing is freed and false is returned: its client
	has to stay until the firmware is halted.
*/
static bool
release_context(Client* client, Context* context)
{
	const pvr_ctx_env& env = sDevice->ContextEnvironment();
	if (!context->released && sDevice->IsRunning()) {
		status_t status = pvr_gpu_context_cleanup(env, sDevice->Firmware(),
			&context->gpu, 2000000);
		if (status != B_OK) {
			POWERVR_TRACE("context %" B_PRIu32 " not released by the "
				"firmware: %s\n", context->id, strerror(status));
			return false;
		}
	}
	context->released = true;
	pvr_gpu_context_destroy(env, &context->gpu);
	client->contexts.Remove(context);
	delete context;
	return true;
}


static uint32
completed_sequence(Context* context)
{
	sDevice->Sync(context->gpu.objs[PVR_CTX_OBJ_UFO].phys, sizeof(uint32));
	return *context->gpu.ufo;
}


static inline bool
sequence_reached(uint32 completed, uint32 sequence)
{
	return (int32)(completed - sequence) >= 0;
}


static status_t
submit_compute(Client* client, Context* context,
	const powervr_submit_compute& request, uint32* _sequence)
{
	if ((request.flags & ~(POWERVR_SUBMIT_PREVENT_ALL_OVERLAP
			| POWERVR_SUBMIT_SINGLE_CORE)) != 0) {
		return B_BAD_VALUE;
	}
	if (sDevice->IsLost())
		return B_DEV_NOT_READY;
	if (context->lastSequence - completed_sequence(context)
			>= MAX_JOBS_IN_FLIGHT) {
		return B_WOULD_BLOCK;
	}

	const pvr_ctx_env& env = sDevice->ContextEnvironment();
	sync_mapped_buffers(client);
	status_t status = pvr_mmu_flush_exec(env, sDevice->Firmware(),
		client->mmu, 0);
	if (status != B_OK)
		return status;

	rogue_fwif_cmd_compute command;
	memset(&command, 0, sizeof(command));
	const powervr_compute_registers& registers = request.registers;
	command.regs.tpu_border_colour_table = registers.tpu_border_colour_table;
	command.regs.cdm_ctrl_stream_base = registers.cdm_ctrl_stream_base;
	command.regs.cdm_context_state_base_addr
		= registers.cdm_context_state_base;
	command.regs.cdm_resume_pds1 = registers.cdm_resume_pds1;
	command.regs.cdm_item = registers.cdm_item;
	command.regs.compute_cluster = registers.compute_cluster;
	command.regs.tpu_tag_cdm_ctrl = registers.tpu_tag_cdm_ctrl;
	command.flags = request.flags;
	command.execute_count = 0;

	uint32 sequence = context->lastSequence + 1;
	if (sequence == 0)
		sequence = 1;
	uint32 slot;
	status = pvr_gpu_context_submit_compute(env, sDevice->Firmware(),
		&context->gpu, &command, sequence, &slot);
	// B_BUSY: the job is in the client CCB, but the kernel CCB was full for
	// its KICK. The firmware drains the kernel CCB on its own in
	// microseconds; kick again (with the current CCB write offset).
	bigtime_t start = system_time();
	while (status == B_BUSY && system_time() - start < 1000000) {
		snooze(50);
		status = pvr_gpu_context_kick(env, sDevice->Firmware(),
			&context->gpu, &slot);
	}
	if (status != B_OK) {
		POWERVR_TRACE("context %" B_PRIu32 ": kick failed: %s\n",
			context->id, strerror(status));
		// the job is queued but never kicked: account for it, or the
		// sequence numbers of the context go out of step
		context->lastSequence = sequence;
		return status;
	}

	context->lastSequence = sequence;
	client->jobsSubmitted++;
	*_sequence = sequence;
	return B_OK;
}


/*!	Waits without the device lock: only the UFO of the context is read,
	which the firmware writes and nothing frees while the client is open.
*/
static status_t
wait_for(Client* client, Context* context, uint32 sequence,
	bigtime_t timeout, uint32* _completed)
{
	bigtime_t end = timeout == B_INFINITE_TIMEOUT
		? B_INFINITE_TIMEOUT : system_time() + timeout;
	while (true) {
		uint32 completed = completed_sequence(context);
		if (sequence_reached(completed, sequence)) {
			*_completed = completed;
			return B_OK;
		}
		if (sDevice->IsLost()) {
			*_completed = completed;
			return B_DEV_NOT_READY;
		}
		bigtime_t now = system_time();
		if (end != B_INFINITE_TIMEOUT && now >= end) {
			*_completed = completed;
			return B_TIMED_OUT;
		}
		// the interrupt wakes us; the timeout covers a missed one
		status_t status = sDevice->WaitForEvent(
			end == B_INFINITE_TIMEOUT ? 10000 : min_c(end - now, 10000));
		if (status == B_INTERRUPTED)
			return status;
	}
}


//	#pragma mark - clients


//!	Called with the device lock held, from ProcessFirmwareEvents().
static void
context_reset(void* cookie, uint32 contextID, uint32 reason, uint32 flags,
	uint64 faultAddress)
{
	for (ClientList::Iterator clients = sClients.GetIterator();
			Client* client = clients.Next();) {
		for (ContextList::Iterator contexts = client->contexts.GetIterator();
				Context* context = contexts.Next();) {
			if (context->id != contextID)
				continue;
			context->resetFlags |= POWERVR_WAIT_CONTEXT_RESET;
			if ((flags & 1) != 0) {		// CONTEXT_RESET_FLAG_PF
				context->resetFlags |= POWERVR_WAIT_PAGE_FAULT;
				context->faultAddress = faultAddress;
			}
			context->resetReason = reason;
			return;
		}
	}
}


static status_t
create_client(Client** _client)
{
	Client* client = new(std::nothrow) Client;
	if (client == NULL)
		return B_NO_MEMORY;
	client->mmu = NULL;
	client->memoryCreated = false;
	client->nextHandle = 0;
	client->jobsSubmitted = 0;
	client->jobsCompleted = 0;

	status_t status = pvr_mmu_context_create(sDevice->MMUEnvironment(),
		&client->mmu);
	if (status == B_OK) {
		memset(&client->memory, 0, sizeof(client->memory));
		status = pvr_fw_mem_context_create(sDevice->ContextEnvironment(),
			client->mmu, &client->memory);
		client->memoryCreated = status == B_OK;
	}
	if (status != B_OK) {
		if (client->mmu != NULL)
			pvr_mmu_context_destroy(client->mmu);
		delete client;
		return status;
	}
	*_client = client;
	return B_OK;
}


/*!	Frees a client, unless the firmware keeps one of its contexts: then
	returns false and leaves everything (page tables, memory context,
	buffers) to free_parked_clients().
*/
static bool
release_client(Client* client)
{
	const pvr_ctx_env& env = sDevice->ContextEnvironment();
	while (Context* context = client->contexts.Head()) {
		if (!release_context(client, context))
			return false;
	}
	while (Mapping* mapping = client->mappings.Head())
		remove_mapping(client, mapping);
	if (client->mmu != NULL && sDevice->IsRunning()) {
		pvr_mmu_flush_exec(env, sDevice->Firmware(), client->mmu,
			PVR_MMU_SYNC_LEVEL_2_FLAGS);
	}
	if (client->memoryCreated)
		pvr_fw_mem_context_destroy(env, &client->memory);
	if (client->mmu != NULL)
		pvr_mmu_context_destroy(client->mmu);
	while (Buffer* buffer = client->buffers.Head())
		delete_buffer(client, buffer);
	delete client;
	return true;
}


//!	With the firmware halted: nothing can use their memory any more.
static void
free_parked_clients()
{
	while (Client* client = sParkedClients.RemoveHead()) {
		for (ContextList::Iterator iterator = client->contexts.GetIterator();
				Context* context = iterator.Next();) {
			context->released = true;
		}
		release_client(client);
	}
}


//	#pragma mark - device hooks


static void
stop_device()
{
	sDevice->Stop();
	free_parked_clients();
	sDevice->ReleaseMemory();
}


static status_t
powervr_open(const char* name, uint32 flags, void** _cookie)
{
	MutexLocker locker(sDevice->LockObject());

	if (sDevice->IsLost())
		return B_DEV_NOT_READY;
	status_t status = sDevice->Start();
	if (status != B_OK)
		return status;

	Client* client;
	status = create_client(&client);
	if (status != B_OK) {
		if (sClients.IsEmpty() && sParkedClients.IsEmpty())
			stop_device();
		return status;
	}
	sClients.Add(client);
	*_cookie = client;
	return B_OK;
}


static status_t
powervr_close(void* cookie)
{
	return B_OK;
}


static status_t
powervr_free(void* cookie)
{
	MutexLocker locker(sDevice->LockObject());
	Client* client = (Client*)cookie;
	sDevice->ProcessFirmwareEvents();
	sClients.Remove(client);
	if (!release_client(client))
		sParkedClients.Add(client);
	if (sClients.IsEmpty())
		stop_device();
	return B_OK;
}


static status_t
powervr_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
powervr_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


template<typename Type>
static status_t
copy_in(Type& value, const void* buffer, size_t length)
{
	if (length != sizeof(Type) || buffer == NULL || !IS_USER_ADDRESS(buffer))
		return B_BAD_VALUE;
	return user_memcpy(&value, buffer, sizeof(Type));
}


template<typename Type>
static status_t
copy_out(void* buffer, const Type& value)
{
	return user_memcpy(buffer, &value, sizeof(Type));
}


static status_t
powervr_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	Client* client = (Client*)cookie;

	switch (op) {
		case POWERVR_GET_INFO:
		{
			powervr_info info;
			memset(&info, 0, sizeof(info));
			info.api_version = POWERVR_JH7110_API_VERSION;
			info.core_clock_mhz = sDevice->CoreClock() / 1000000;
			info.bvnc = sDevice->BVNC();
			info.firmware_major = sDevice->FirmwareMajor();
			info.firmware_minor = sDevice->FirmwareMinor();
			info.firmware_build = sDevice->FirmwareBuild();
			info.general_heap_base = POWERVR_GENERAL_HEAP_BASE;
			info.general_heap_size = POWERVR_GENERAL_HEAP_SIZE;
			info.page_size = B_PAGE_SIZE;
			if (length != sizeof(info) || !IS_USER_ADDRESS(buffer))
				return B_BAD_VALUE;
			return copy_out(buffer, info);
		}

		case POWERVR_CREATE_BUFFER:
		{
			powervr_create_buffer request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;
			MutexLocker locker(sDevice->LockObject());
			Buffer* created;
			status = create_buffer(client, request.size, &created,
				&request.address);
			if (status != B_OK)
				return status;
			request.size = created->size;
			request.handle = created->handle;
			request.area = created->userArea;
			return copy_out(buffer, request);
		}

		case POWERVR_DELETE_BUFFER:
		{
			int32 handle;
			status_t status = copy_in(handle, buffer, length);
			if (status != B_OK)
				return status;
			MutexLocker locker(sDevice->LockObject());
			Buffer* found = find_buffer(client, handle);
			if (found == NULL)
				return B_BAD_VALUE;
			if (found->mappingCount > 0)
				return B_BUSY;
			delete_buffer(client, found);
			return B_OK;
		}

		case POWERVR_MAP_BUFFER:
		{
			powervr_map_buffer request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;
			MutexLocker locker(sDevice->LockObject());
			Buffer* found = find_buffer(client, request.handle);
			if (found == NULL)
				return B_BAD_VALUE;
			uint64 size = request.size != 0
				? request.size : found->size - request.offset;
			if ((request.offset | size | request.device_address)
					% B_PAGE_SIZE != 0
				|| request.offset > found->size
				|| size > found->size - request.offset || size == 0
				|| request.device_address < POWERVR_GENERAL_HEAP_BASE
				|| request.device_address + size > POWERVR_GENERAL_HEAP_BASE
					+ POWERVR_GENERAL_HEAP_SIZE
				|| (request.flags & ~POWERVR_MAP_READ_ONLY) != 0) {
				return B_BAD_VALUE;
			}
			if (overlaps_mapping(client, request.device_address, size))
				return B_BUSY;

			Mapping* mapping = new(std::nothrow) Mapping;
			if (mapping == NULL)
				return B_NO_MEMORY;
			status = map_buffer(client, found, request.offset, size,
				request.device_address,
				(request.flags & POWERVR_MAP_READ_ONLY) != 0
					? PVR_MMU_MAP_READ_ONLY : 0);
			if (status != B_OK) {
				delete mapping;
				return status;
			}
			mapping->buffer = found;
			mapping->deviceAddress = request.device_address;
			mapping->offset = request.offset;
			mapping->size = size;
			found->mappingCount++;
			client->mappings.Add(mapping);
			return B_OK;
		}

		case POWERVR_UNMAP_BUFFER:
		{
			powervr_unmap_buffer request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;
			MutexLocker locker(sDevice->LockObject());
			for (MappingList::Iterator iterator
					= client->mappings.GetIterator();
					Mapping* mapping = iterator.Next();) {
				if (mapping->deviceAddress == request.device_address
					&& (request.size == 0 || request.size == mapping->size)) {
					remove_mapping(client, mapping);
					return pvr_mmu_flush_exec(sDevice->ContextEnvironment(),
						sDevice->Firmware(), client->mmu, 0);
				}
			}
			return B_BAD_VALUE;
		}

		case POWERVR_CREATE_CONTEXT:
		{
			powervr_create_context request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;
			MutexLocker locker(sDevice->LockObject());
			Context* context;
			status = create_context(client, request, &context);
			if (status != B_OK)
				return status;
			request.handle = context->handle;
			return copy_out(buffer, request);
		}

		case POWERVR_DELETE_CONTEXT:
		{
			int32 handle;
			status_t status = copy_in(handle, buffer, length);
			if (status != B_OK)
				return status;
			MutexLocker locker(sDevice->LockObject());
			Context* context = find_context(client, handle);
			if (context == NULL)
				return B_BAD_VALUE;
			if (context->waiters > 0)
				return B_BUSY;
			return release_context(client, context) ? B_OK : B_BUSY;
		}

		case POWERVR_SUBMIT_COMPUTE:
		{
			powervr_submit_compute request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;
			MutexLocker locker(sDevice->LockObject());
			Context* context = find_context(client, request.context);
			if (context == NULL)
				return B_BAD_VALUE;
			status = submit_compute(client, context, request,
				&request.sequence);
			if (status != B_OK)
				return status;
			return copy_out(buffer, request);
		}

		case POWERVR_WAIT:
		{
			powervr_wait request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;
			MutexLocker locker(sDevice->LockObject());
			Context* context = find_context(client, request.context);
			if (context == NULL)
				return B_BAD_VALUE;
			if (!sequence_reached(context->lastSequence, request.sequence))
				return B_BAD_VALUE;
			// the context stays while it is waited for (DELETE_CONTEXT
			// refuses it), even with the lock dropped
			context->waiters++;
			locker.Unlock();

			status = wait_for(client, context, request.sequence,
				request.timeout, &request.completed);

			locker.Lock();
			context->waiters--;
			sDevice->ProcessFirmwareEvents();
			if (status == B_OK) {
				// the CPU reads what the GPU wrote: drop stale cache lines
				sync_mapped_buffers(client);
				client->jobsCompleted++;
			}
			request.flags = context->resetFlags;
			request.reset_reason = context->resetReason;
			request.fault_address = context->faultAddress;
			context->resetFlags = 0;
			locker.Unlock();
			status_t copyStatus = copy_out(buffer, request);
			return status != B_OK ? status : copyStatus;
		}

		case POWERVR_GET_STATISTICS:
		{
			if (length != sizeof(powervr_statistics)
				|| !IS_USER_ADDRESS(buffer)) {
				return B_BAD_VALUE;
			}
			MutexLocker locker(sDevice->LockObject());
			sDevice->ProcessFirmwareEvents();
			powervr_statistics statistics;
			memset(&statistics, 0, sizeof(statistics));
			statistics.jobs_submitted = client->jobsSubmitted;
			statistics.jobs_completed = client->jobsCompleted;
			statistics.interrupts = sDevice->interrupts;
			statistics.mmu_page_faults = sDevice->mmuPageFaults;
			statistics.hardware_recoveries = sDevice->hardwareRecoveries;
			statistics.firmware_restarts = sDevice->firmwareStarts;
			locker.Unlock();
			return copy_out(buffer, statistics);
		}
	}

	return B_DEV_INVALID_IOCTL;
}


static device_hooks sHooks = {
	powervr_open,
	powervr_close,
	powervr_free,
	powervr_control,
	powervr_read,
	powervr_write,
};


//	#pragma mark - driver API


status_t
init_hardware()
{
	// the GPU is only touched when the device is opened
	if (gFDT == NULL
		|| fdt_node_offset_by_compatible(gFDT, -1, "starfive,jh7110") < 0) {
		return B_DEVICE_NOT_FOUND;
	}
	return B_OK;
}


status_t
init_driver()
{
	sDevice = new(std::nothrow) PowerVRDevice;
	if (sDevice == NULL)
		return B_NO_MEMORY;
	sDevice->SetResetHook(context_reset, NULL);
	new(&sClients) ClientList;
	new(&sParkedClients) ClientList;
	return B_OK;
}


void
uninit_driver()
{
	delete sDevice;
	sDevice = NULL;
}


const char**
publish_devices()
{
	return sDeviceNames;
}


device_hooks*
find_device(const char* name)
{
	return &sHooks;
}
