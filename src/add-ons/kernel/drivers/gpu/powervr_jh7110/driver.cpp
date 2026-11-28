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
#include <stddef.h>
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
#include "pvr_render.h"


extern "C" {
#include <libfdt.h>
}


#define DEVICE_NAME			"gpu/powervr_jh7110"
#define MAX_JOBS_IN_FLIGHT	128
	// a compute job takes 184 bytes of the 32 KB client CCB; the CCB space
	// check of the submit catches the larger render and transfer commands
#define MAX_SUBMIT_JOBS		8
#define MAX_STREAM_SIZE		4096
#define MAX_STATIC_STATE_SIZE	1024
#define RENDER_JOB_RING		256
#define CLEANUP_TIMEOUT		2000000


extern void* gFDT;

int32 api_version = B_CUR_DRIVER_API_VERSION;

static const char* sDeviceNames[] = { DEVICE_NAME, NULL };
static PowerVRDevice* sDevice;
static pvr_device_desc sDeviceDesc;
static uint32 sNextFreeListID;
static uint32 sNextJobID;
static uint32 sFreeListGrows;
static uint32 sFreeListReconstructions;


struct Buffer : DoublyLinkedListLinkImpl<Buffer> {
	int32			handle;
	area_id			kernelArea;
	area_id			userArea;
	team_id			team;
	size_t			size;
	uint8*			kernelAddress;
	physical_entry*	runs;
	uint32			runCount;
	uint32			mappingCount;
};

struct Mapping : DoublyLinkedListLinkImpl<Mapping> {
	Buffer*			buffer;
	uint64			deviceAddress;
	uint64			offset;
	uint64			size;
	int32			pinCount;		// free lists in it: no unmapping
};

// a job of a render context: its queue and timeline value
struct RenderJob {
	uint32			sequence;
	pvr_gpu_context* queue;
	uint32			value;
};

struct Context : DoublyLinkedListLinkImpl<Context> {
	int32			handle;
	uint32			id;				// server_common_context_id
	uint32			type;			// POWERVR_CONTEXT_*
	pvr_gpu_context	gpu;			// compute and transfer
	pvr_render_context render;		// render: geometry and fragment queues
	uint32			lastSequence;
		// compute and transfer: also the last timeline value of the queue
	uint32			geometryValue;	// render: last timeline values
	uint32			fragmentValue;
	RenderJob		renderJobs[RENDER_JOB_RING];
		// render: the queue and value of the last jobs by sequence
	int32			waiters;		// POWERVR_WAIT without the device lock
	bool			released;		// the firmware let go of it
	// reported by the next POWERVR_WAIT
	uint32			resetFlags;
	uint32			resetReason;
	uint64			faultAddress;
};

struct FreeList : DoublyLinkedListLinkImpl<FreeList> {
	int32			handle;
	pvr_free_list	list;
	Mapping*		mapping;		// holds the page list
	uint64			pageListOffset;	// into the mapped buffer
	bool			released;
};

struct Dataset : DoublyLinkedListLinkImpl<Dataset> {
	int32			handle;
	pvr_hwrt_dataset set;
	bool			released;
};

typedef DoublyLinkedList<Buffer> BufferList;
typedef DoublyLinkedList<Mapping> MappingList;
typedef DoublyLinkedList<Context> ContextList;
typedef DoublyLinkedList<FreeList> FreeListList;
typedef DoublyLinkedList<Dataset> DatasetList;

struct Client : DoublyLinkedListLinkImpl<Client> {
	pvr_mmu_context*	mmu;
	pvr_fw_mem_context	memory;
	bool				memoryCreated;
	BufferList			buffers;
	MappingList			mappings;
	ContextList			contexts;
	FreeListList		freeLists;
	DatasetList			datasets;
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


/*!	Writes back and invalidates the cache lines of [offset, offset + size)
	of the buffer.
*/
static void
sync_buffer_range(Buffer* buffer, uint64 offset, uint64 size)
{
	uint64 runStart = 0;
	for (uint32 i = 0; i < buffer->runCount && size > 0; i++) {
		uint64 runSize = buffer->runs[i].size;
		if (offset < runStart + runSize) {
			uint64 skip = offset - runStart;
			uint64 length = min_c(runSize - skip, size);
			sDevice->Sync(buffer->runs[i].address + skip, length);
			offset += length;
			size -= length;
		}
		runStart += runSize;
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
	buffer->kernelAddress = (uint8*)address;
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


static uint32
uapi_context_type(uint32 type)
{
	switch (type) {
		case POWERVR_CONTEXT_RENDER:
			return PVR_UAPI_CTX_TYPE_RENDER;
		case POWERVR_CONTEXT_TRANSFER:
			return PVR_UAPI_CTX_TYPE_TRANSFER_FRAG;
		default:
			return PVR_UAPI_CTX_TYPE_COMPUTE;
	}
}


static status_t
create_context(Client* client, const powervr_create_context& request,
	Context** _context)
{
	if (request.type < POWERVR_CONTEXT_COMPUTE
		|| request.type > POWERVR_CONTEXT_TRANSFER || request.priority > 2
		|| request.static_state_size > MAX_STATIC_STATE_SIZE
		|| (request.static_state_size > 0
			&& !IS_USER_ADDRESS(request.static_state))) {
		return B_BAD_VALUE;
	}

	// the static state is parsed from a kernel copy
	uint8 staticState[MAX_STATIC_STATE_SIZE] __attribute__((aligned(8)));
	if (request.static_state_size > 0 && user_memcpy(staticState,
			request.static_state, request.static_state_size) != B_OK) {
		return B_BAD_ADDRESS;
	}

	Context* context = new(std::nothrow) Context;
	if (context == NULL)
		return B_NO_MEMORY;
	memset(&context->gpu, 0, sizeof(context->gpu));
	memset(&context->render, 0, sizeof(context->render));
	context->type = request.type;
	context->id = sDevice->NextContextID();

	const pvr_ctx_env& env = sDevice->ContextEnvironment();
	status_t status;
	if (request.type == POWERVR_CONTEXT_COMPUTE
		&& request.static_state_size == 0) {
		// version 1: no context switch state
		pvr_ctx_params params;
		memset(&params, 0, sizeof(params));
		params.type = PVR_CTX_TYPE_COMPUTE;
		params.priority = request.priority;
		params.cccb_size_log2 = PVR_CTX_CCCB_SIZE_LOG2_LINUX;
		params.ctx_id = context->id;
		params.pid = team_get_current_team_id();
		status = pvr_gpu_context_create(env, &client->memory, params,
			&context->gpu);
	} else {
		pvr_context_create_args args;
		memset(&args, 0, sizeof(args));
		args.type = uapi_context_type(request.type);
		args.priority = request.priority;
		args.ctx_id = context->id;
		args.pid = team_get_current_team_id();
		args.cccb_size_log2 = PVR_CTX_CCCB_SIZE_LOG2_LINUX;
		args.static_state = request.static_state_size > 0
			? staticState : NULL;
		args.static_state_len = request.static_state_size;
		args.callstack_addr = request.callstack_address;
		if (request.type == POWERVR_CONTEXT_RENDER) {
			status = pvr_render_context_create_uapi(env, sDeviceDesc,
				&client->memory, args, &context->render);
		} else {
			status = pvr_gpu_context_create_uapi(env, sDeviceDesc,
				&client->memory, args, &context->gpu);
		}
	}
	if (status == B_OK) {
		// new pages in the MIPS page table: flush the firmware's MMU caches
		status = pvr_mmu_flush_exec(env, sDevice->Firmware(), client->mmu,
			PVR_MMU_SYNC_LEVEL_2_FLAGS);
		if (status != B_OK) {
			// the firmware never got to use it
			if (request.type == POWERVR_CONTEXT_RENDER)
				pvr_render_context_destroy(env, &context->render);
			else
				pvr_gpu_context_destroy(env, &context->gpu);
		}
	}
	if (status != B_OK) {
		delete context;
		return status;
	}

	context->handle = ++client->nextHandle;
	context->lastSequence = 0;
	context->geometryValue = 0;
	context->fragmentValue = 0;
	memset(context->renderJobs, 0, sizeof(context->renderJobs));
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
	const bool render = context->type == POWERVR_CONTEXT_RENDER;
	if (!context->released && sDevice->IsRunning()) {
		// a job may wait for a free list grow: serve the firmware's
		// requests between the attempts
		status_t status = B_ERROR;
		for (int attempt = 0; attempt < 4 && status != B_OK; attempt++) {
			if (attempt > 0)
				sDevice->ProcessFirmwareEvents();
			status = render
				? pvr_render_context_cleanup(env, sDevice->Firmware(),
					&context->render, CLEANUP_TIMEOUT / 4)
				: pvr_gpu_context_cleanup(env, sDevice->Firmware(),
					&context->gpu, CLEANUP_TIMEOUT / 4);
		}
		if (status != B_OK) {
			POWERVR_TRACE("context %" B_PRIu32 " not released by the "
				"firmware: %s\n", context->id, strerror(status));
			return false;
		}
	}
	context->released = true;
	if (render)
		pvr_render_context_destroy(env, &context->render);
	else
		pvr_gpu_context_destroy(env, &context->gpu);
	client->contexts.Remove(context);
	delete context;
	return true;
}


//!	The last timeline value the firmware wrote for a queue.
static uint32
queue_value(pvr_gpu_context* queue)
{
	sDevice->Sync(queue->objs[PVR_CTX_OBJ_UFO].phys, sizeof(uint32));
	return *queue->ufo;
}


//!	Compute and transfer contexts: sequences are the queue's values.
static uint32
completed_sequence(Context* context)
{
	return queue_value(&context->gpu);
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


/*!	Which timeline value of which queue a sequence of the context stands
	for, with the device lock held. A render job too old to be in the ring
	is done: the ring holds more jobs than can be in flight.
*/
static void
wait_target(Context* context, uint32 sequence, pvr_gpu_context** _queue,
	uint32* _value)
{
	if (context->type != POWERVR_CONTEXT_RENDER) {
		*_queue = &context->gpu;
		*_value = sequence;
		return;
	}
	const RenderJob& job = context->renderJobs[sequence % RENDER_JOB_RING];
	if (job.queue != NULL && job.sequence == sequence) {
		*_queue = job.queue;
		*_value = job.value;
	} else
		*_queue = NULL;
}


/*!	Waits without the device lock: only the UFO of a queue of the context
	is read, which the firmware writes and nothing frees while the context
	is waited for (DELETE_CONTEXT refuses it).
*/
static status_t
wait_for(pvr_gpu_context* queue, uint32 value, uint32 sequence,
	bigtime_t timeout, uint32* _completed)
{
	bigtime_t end = timeout == B_INFINITE_TIMEOUT
		? B_INFINITE_TIMEOUT : system_time() + timeout;
	while (true) {
		if (queue == NULL || sequence_reached(queue_value(queue), value)) {
			*_completed = sequence;
			return B_OK;
		}
		*_completed = sequence - 1;
		if (sDevice->IsLost())
			return B_DEV_NOT_READY;
		bigtime_t now = system_time();
		if (end != B_INFINITE_TIMEOUT && now >= end)
			return B_TIMED_OUT;
		// the interrupt wakes us; the timeout covers a missed one
		status_t status = sDevice->WaitForEvent(
			end == B_INFINITE_TIMEOUT ? 10000 : min_c(end - now, 10000));
		if (status == B_INTERRUPTED)
			return status;
	}
}


//	#pragma mark - free lists


// PM pages of one grow: the parameter manager only sees them by address
struct PMBlock {
	area_id			area;
	uint32			runCount;
	physical_entry	runs[];
};


static status_t
pm_pages_alloc(void* cookie, uint32 count, void** _block)
{
	size_t size = (size_t)count * B_PAGE_SIZE;
	PMBlock* block = (PMBlock*)malloc(sizeof(PMBlock)
		+ count * sizeof(physical_entry));
	if (block == NULL)
		return B_NO_MEMORY;

	void* address;
	block->area = create_area("powervr parameter buffer", &address,
		B_ANY_KERNEL_ADDRESS, size, B_32_BIT_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (block->area < 0) {
		free(block);
		return B_NO_MEMORY;
	}
	block->runCount = count;
	if (get_memory_map_etc(B_CURRENT_TEAM, address, size, block->runs,
			&block->runCount) != B_OK) {
		delete_area(block->area);
		free(block);
		return B_NO_MEMORY;
	}

	// zeroed, and out of the cache before the GPU writes there
	memset(address, 0, size);
	for (uint32 i = 0; i < block->runCount; i++)
		sDevice->Sync(block->runs[i].address, block->runs[i].size);
	*_block = block;
	return B_OK;
}


static uint64
pm_page_phys(void* cookie, void* _block, uint32 index)
{
	PMBlock* block = (PMBlock*)_block;
	uint64 offset = (uint64)index * B_PAGE_SIZE;
	for (uint32 i = 0; i < block->runCount; i++) {
		if (offset < block->runs[i].size)
			return block->runs[i].address + offset;
		offset -= block->runs[i].size;
	}
	return 0;
}


static void
pm_pages_free(void* cookie, void* _block)
{
	PMBlock* block = (PMBlock*)_block;
	delete_area(block->area);
	free(block);
}


/*!	Writes page list entries into the client buffer behind the free list.
	There is no uncached mapping on the JH7110: the lines are written back
	and invalidated first, so the CPU writes into fresh copies of what the
	parameter manager wrote, and written back at once. The firmware waits
	for the grow reply meanwhile and the new entries are beyond the ones in
	use.
*/
static status_t
page_list_write(void* cookie, void* pageList, uint32 byteOffset,
	const uint32* entries, uint32 count)
{
	FreeList* freeList = (FreeList*)pageList;
	Buffer* buffer = freeList->mapping->buffer;
	uint64 offset = freeList->pageListOffset + byteOffset;
	size_t size = count * sizeof(uint32);
	if (offset + size > freeList->mapping->offset + freeList->mapping->size)
		return B_BAD_VALUE;

	sync_buffer_range(buffer, offset, size);
	memcpy(buffer->kernelAddress + offset, entries, size);
	sync_buffer_range(buffer, offset, size);
	return B_OK;
}


static const pvr_free_list_hooks kFreeListHooks = {
	NULL,
	pm_pages_alloc,
	pm_page_phys,
	pm_pages_free,
	page_list_write,
};


static FreeList*
find_free_list(Client* client, int32 handle)
{
	for (FreeListList::Iterator iterator = client->freeLists.GetIterator();
			FreeList* freeList = iterator.Next();) {
		if (freeList->handle == handle)
			return freeList;
	}
	return NULL;
}


//!	By the ID the firmware knows it by, among all clients.
static pvr_free_list*
lookup_free_list(void* cookie, uint32 fwID)
{
	ClientList* lists[] = { &sClients, &sParkedClients };
	for (int i = 0; i < 2; i++) {
		for (ClientList::Iterator clients = lists[i]->GetIterator();
				Client* client = clients.Next();) {
			for (FreeListList::Iterator iterator
					= client->freeLists.GetIterator();
					FreeList* freeList = iterator.Next();) {
				if (freeList->list.fw_id == fwID)
					return &freeList->list;
			}
		}
	}
	return NULL;
}


static Mapping*
find_mapping_containing(Client* client, uint64 address, uint64 size)
{
	for (MappingList::Iterator iterator = client->mappings.GetIterator();
			Mapping* mapping = iterator.Next();) {
		if (address >= mapping->deviceAddress
			&& address - mapping->deviceAddress <= mapping->size
			&& size <= mapping->size - (address - mapping->deviceAddress)) {
			return mapping;
		}
	}
	return NULL;
}


static status_t
create_free_list(Client* client, const powervr_create_free_list& request,
	FreeList** _freeList)
{
	uint64 listSize = (uint64)request.max_pages * PVR_FREE_LIST_ENTRY_SIZE;
	Mapping* mapping = find_mapping_containing(client,
		request.free_list_address, listSize);
	if (mapping == NULL || listSize == 0)
		return B_BAD_VALUE;

	FreeList* freeList = new(std::nothrow) FreeList;
	if (freeList == NULL)
		return B_NO_MEMORY;
	memset(&freeList->list, 0, sizeof(freeList->list));

	// the page list starts inside the mapping: offset the cookie's view
	uint64 skip = request.free_list_address - mapping->deviceAddress;
	freeList->mapping = mapping;
	freeList->pageListOffset = mapping->offset + skip;

	pvr_free_list_create_args args;
	memset(&args, 0, sizeof(args));
	args.free_list_gpu_addr = request.free_list_address;
	args.initial_num_pages = request.initial_pages;
	args.max_num_pages = request.max_pages;
	args.grow_num_pages = request.grow_pages;
	args.grow_threshold = request.grow_threshold;
	args.fw_id = ++sNextFreeListID;
	if (args.fw_id == 0)
		args.fw_id = ++sNextFreeListID;
	args.page_list = freeList;
	args.page_list_size = mapping->size - skip;

	const pvr_ctx_env& env = sDevice->ContextEnvironment();
	status_t status = pvr_free_list_create(env, kFreeListHooks, args,
		&freeList->list);
	if (status == B_OK) {
		status = pvr_mmu_flush_exec(env, sDevice->Firmware(), client->mmu,
			PVR_MMU_SYNC_LEVEL_2_FLAGS);
		if (status != B_OK)
			pvr_free_list_destroy(env, &freeList->list);
	}
	if (status != B_OK) {
		delete freeList;
		return status;
	}

	mapping->pinCount++;
	freeList->handle = ++client->nextHandle;
	freeList->released = false;
	client->freeLists.Add(freeList);
	*_freeList = freeList;
	return B_OK;
}


/*!	B_BUSY while a data set uses the free list; false (and nothing freed)
	if the firmware does not let go of it.
*/
static status_t
release_free_list(Client* client, FreeList* freeList)
{
	if (freeList->list.hwrt_count > 0)
		return B_BUSY;

	const pvr_ctx_env& env = sDevice->ContextEnvironment();
	if (!freeList->released && sDevice->IsRunning()) {
		status_t status = pvr_free_list_cleanup(env, sDevice->Firmware(),
			&freeList->list, CLEANUP_TIMEOUT / 2);
		if (status != B_OK) {
			// pending requests may hold it: serve them, then try again
			sDevice->ProcessFirmwareEvents();
			status = pvr_free_list_cleanup(env, sDevice->Firmware(),
				&freeList->list, CLEANUP_TIMEOUT / 2);
		}
		if (status != B_OK) {
			POWERVR_TRACE("free list %" B_PRIu32 " not released by the "
				"firmware: %s\n", freeList->list.fw_id, strerror(status));
			return B_ERROR;
		}
	}
	freeList->released = true;
	status_t status = pvr_free_list_destroy(env, &freeList->list);
	if (status != B_OK)
		return status;
	freeList->mapping->pinCount--;
	client->freeLists.Remove(freeList);
	delete freeList;
	return B_OK;
}


/*!	The firmware CCB commands the device leaves to the driver: requests to
	grow a free list and to rebuild free lists after a hardware recovery.
	Called with the device lock held; the firmware waits for the answers.
*/
static void
firmware_event(void* cookie, uint32 type, const uint32* words)
{
	const pvr_ctx_env& env = sDevice->ContextEnvironment();
	pvr_fwif_state* firmware = sDevice->Firmware();
	switch (type) {
		case ROGUE_FWIF_FWCCB_CMD_FREELIST_GROW & 0xffff:
		{
			const rogue_fwif_fwccb_cmd_freelist_gs_data* request
				= (const rogue_fwif_fwccb_cmd_freelist_gs_data*)words;
			pvr_free_list* freeList = lookup_free_list(NULL,
				request->freelist_id);
			if (freeList == NULL) {
				POWERVR_TRACE("grow request for unknown free list %"
					B_PRIu32 "\n", request->freelist_id);
				return;
			}
			sFreeListGrows++;
			status_t status = pvr_free_list_process_grow_req(env, firmware,
				freeList);
			// the kernel CCB was full: the firmware drains it on its own
			bigtime_t start = system_time();
			while (status == B_BUSY && system_time() - start < 100000) {
				snooze(20);
				status = pvr_free_list_send_pending_reply(env, firmware,
					freeList);
			}
			if (status != B_OK) {
				POWERVR_TRACE("free list %" B_PRIu32 " grow: %s\n",
					request->freelist_id, strerror(status));
			}
			break;
		}

		case ROGUE_FWIF_FWCCB_CMD_FREELISTS_RECONSTRUCTION & 0xffff:
		{
			sFreeListReconstructions++;
			rogue_fwif_kccb_cmd reply;
			status_t status = pvr_free_list_process_reconstruct_req(env,
				firmware,
				(const rogue_fwif_fwccb_cmd_freelists_reconstruction_data*)
					words, lookup_free_list, NULL, &reply);
			bigtime_t start = system_time();
			while (status == B_BUSY && system_time() - start < 100000) {
				snooze(20);
				status = pvr_kccb_send_cmd(env, firmware, &reply, NULL);
			}
			if (status != B_OK) {
				POWERVR_TRACE("free list reconstruction: %s\n",
					strerror(status));
			}
			break;
		}

		case 107:
			// ROGUE_FWIF_FWCCB_CMD_UPDATE_STATS: Linux ignores it too
			break;

		default:
			POWERVR_TRACE("firmware event %" B_PRIu32 "\n", type);
			break;
	}
}


//	#pragma mark - HWRT data sets


static Dataset*
find_dataset(Client* client, int32 handle)
{
	for (DatasetList::Iterator iterator = client->datasets.GetIterator();
			Dataset* dataset = iterator.Next();) {
		if (dataset->handle == handle)
			return dataset;
	}
	return NULL;
}


static status_t
create_dataset(Client* client, const powervr_create_hwrt_dataset& request,
	Dataset** _dataset)
{
	FreeList* local = find_free_list(client,
		request.free_lists[POWERVR_HWRT_FREE_LIST_LOCAL]);
	FreeList* global = NULL;
	if (request.free_lists[POWERVR_HWRT_FREE_LIST_GLOBAL] >= 0) {
		global = find_free_list(client,
			request.free_lists[POWERVR_HWRT_FREE_LIST_GLOBAL]);
		if (global == NULL)
			return B_BAD_VALUE;
	}
	if (local == NULL)
		return B_BAD_VALUE;

	pvr_hwrt_create_args args;
	memset(&args, 0, sizeof(args));
	args.tpc_dev_addr = request.tpc_address;
	args.tpc_size = request.tpc_size;
	args.tpc_stride = request.tpc_stride;
	args.vheap_table_dev_addr = request.vheap_table_address;
	args.rtc_dev_addr = request.rtc_address;
	for (int i = 0; i < 2; i++) {
		args.rt_data[i].pm_mlist_dev_addr
			= request.rt_data[i].pm_mlist_address;
		args.rt_data[i].macrotile_array_dev_addr
			= request.rt_data[i].macrotile_array_address;
		args.rt_data[i].region_header_dev_addr
			= request.rt_data[i].region_header_address;
	}
	args.free_lists[ROGUE_FW_LOCAL_FREELIST] = &local->list;
	args.free_lists[ROGUE_FW_GLOBAL_FREELIST]
		= global != NULL ? &global->list : NULL;
	args.width = request.width;
	args.height = request.height;
	args.samples = request.samples;
	args.layers = request.layers;
	args.isp_merge_lower_x = request.isp_merge_lower_x;
	args.isp_merge_lower_y = request.isp_merge_lower_y;
	args.isp_merge_scale_x = request.isp_merge_scale_x;
	args.isp_merge_scale_y = request.isp_merge_scale_y;
	args.isp_merge_upper_x = request.isp_merge_upper_x;
	args.isp_merge_upper_y = request.isp_merge_upper_y;
	args.region_header_size = request.region_header_size;

	Dataset* dataset = new(std::nothrow) Dataset;
	if (dataset == NULL)
		return B_NO_MEMORY;
	memset(&dataset->set, 0, sizeof(dataset->set));

	const pvr_ctx_env& env = sDevice->ContextEnvironment();
	status_t status = pvr_hwrt_dataset_create(env, sDeviceDesc, args,
		&dataset->set);
	if (status == B_OK) {
		status = pvr_mmu_flush_exec(env, sDevice->Firmware(), client->mmu,
			PVR_MMU_SYNC_LEVEL_2_FLAGS);
		if (status != B_OK)
			pvr_hwrt_dataset_destroy(env, &dataset->set);
	}
	if (status != B_OK) {
		delete dataset;
		return status;
	}

	dataset->handle = ++client->nextHandle;
	dataset->released = false;
	client->datasets.Add(dataset);
	*_dataset = dataset;
	return B_OK;
}


static bool
release_dataset(Client* client, Dataset* dataset)
{
	const pvr_ctx_env& env = sDevice->ContextEnvironment();
	if (!dataset->released && sDevice->IsRunning()) {
		status_t status = B_ERROR;
		for (int attempt = 0; attempt < 4 && status != B_OK; attempt++) {
			if (attempt > 0)
				sDevice->ProcessFirmwareEvents();
			status = pvr_hwrt_dataset_cleanup(env, sDevice->Firmware(),
				&dataset->set, CLEANUP_TIMEOUT / 4);
		}
		if (status != B_OK) {
			POWERVR_TRACE("HWRT data set not released by the firmware: "
				"%s\n", strerror(status));
			return false;
		}
	}
	dataset->released = true;
	pvr_hwrt_dataset_destroy(env, &dataset->set);
	client->datasets.Remove(dataset);
	delete dataset;
	return true;
}


//	#pragma mark - job submission


static uint32
job_type(uint32 type)
{
	switch (type) {
		case POWERVR_JOB_GEOMETRY:
			return PVR_JOB_TYPE_GEOMETRY;
		case POWERVR_JOB_FRAGMENT:
			return PVR_JOB_TYPE_FRAGMENT;
		case POWERVR_JOB_COMPUTE:
			return PVR_JOB_TYPE_COMPUTE;
		default:
			return PVR_JOB_TYPE_TRANSFER_FRAG;
	}
}


//!	The counter of the last timeline value of a queue of the context.
static uint32*
queue_counter(Context* context, pvr_gpu_context* queue)
{
	if (context->type != POWERVR_CONTEXT_RENDER)
		return &context->lastSequence;
	return queue == &context->render.geom
		? &context->geometryValue : &context->fragmentValue;
}


/*!	POWERVR_SUBMIT_JOBS: the jobs are checked, parsed and given their
	timeline values, then queued all at once, as pvr_submit_jobs() does.
	A geometry job followed by a fragment job of the same context and data
	set (Mesa's partial render job) goes as one combined kick. Jobs are not
	made to wait for anything: the client orders them by waiting itself.
*/
static status_t
submit_jobs(Client* client, powervr_job* requests, uint32 count)
{
	if (count == 0 || count > MAX_SUBMIT_JOBS)
		return B_BAD_VALUE;
	if (sDevice->IsLost())
		return B_DEV_NOT_READY;

	pvr_job* jobs = (pvr_job*)calloc(count, sizeof(pvr_job));
	uint8* streams = (uint8*)malloc(count * MAX_STREAM_SIZE);
	MemoryDeleter jobsDeleter(jobs);
	MemoryDeleter streamsDeleter(streams);
	if (jobs == NULL || streams == NULL)
		return B_NO_MEMORY;

	Context* contexts[MAX_SUBMIT_JOBS];
	uint32 nextValue[MAX_SUBMIT_JOBS];
	for (uint32 i = 0; i < count; i++) {
		const powervr_job& request = requests[i];
		if (request.stream_size == 0 || request.stream_size > MAX_STREAM_SIZE
			|| !IS_USER_ADDRESS(request.stream)
			|| request.type > POWERVR_JOB_TRANSFER) {
			return B_BAD_VALUE;
		}
		uint8* stream = streams + i * MAX_STREAM_SIZE;
		if (user_memcpy(stream, request.stream, request.stream_size) != B_OK)
			return B_BAD_ADDRESS;

		Context* context = find_context(client, request.context);
		if (context == NULL)
			return B_BAD_VALUE;
		const bool renderJob = request.type == POWERVR_JOB_GEOMETRY
			|| request.type == POWERVR_JOB_FRAGMENT;
		if (renderJob != (context->type == POWERVR_CONTEXT_RENDER))
			return B_BAD_VALUE;
		contexts[i] = context;

		pvr_job_args args;
		memset(&args, 0, sizeof(args));
		args.type = job_type(request.type);
		args.flags = request.flags;
		args.stream = stream;
		args.stream_size = request.stream_size;
		args.id = ++sNextJobID;
		if (renderJob) {
			Dataset* dataset = find_dataset(client, request.hwrt_dataset);
			if (dataset == NULL || request.hwrt_index >= ROGUE_FWIF_NUM_RTDATAS)
				return B_BAD_VALUE;
			args.render = &context->render;
			args.hwrt = &dataset->set.data[request.hwrt_index];
		} else {
			if (request.hwrt_dataset != -1)
				return B_BAD_VALUE;
			args.context = &context->gpu;
		}
		status_t status = pvr_job_init(sDeviceDesc, args, &jobs[i]);
		if (status != B_OK)
			return status;

		// timeline values: one more per job on each queue
		uint32 value = *queue_counter(context, jobs[i].queue) + 1;
		for (uint32 j = 0; j < i; j++) {
			if (jobs[j].queue == jobs[i].queue)
				value = nextValue[j] + 1;
		}
		if (context->type != POWERVR_CONTEXT_RENDER && value == 0)
			value = 1;
		if (value - queue_value(jobs[i].queue) > MAX_JOBS_IN_FLIGHT)
			return B_WOULD_BLOCK;
		nextValue[i] = value;
		jobs[i].value = value;

		// Mesa's partial render job depends on the geometry job before it
		jobs[i].depends_on_previous = i > 0
			&& request.type == POWERVR_JOB_FRAGMENT
			&& (request.flags & POWERVR_FRAGMENT_PARTIAL_RENDER) != 0
			&& requests[i - 1].type == POWERVR_JOB_GEOMETRY;
	}

	const pvr_ctx_env& env = sDevice->ContextEnvironment();
	sync_mapped_buffers(client);
	status_t status = pvr_mmu_flush_exec(env, sDevice->Firmware(),
		client->mmu, 0);
	if (status != B_OK)
		return status;

	// a full CCB drains as the firmware runs, which may need a free list
	// grow answered first
	uint32 submitted = 0;
	bigtime_t start = system_time();
	while (true) {
		status = pvr_jobs_submit(env, sDevice->Firmware(), jobs, count,
			&submitted);
		if (status != B_WOULD_BLOCK || system_time() - start > 1000000)
			break;
		sDevice->ProcessFirmwareEvents();
		snooze(50);
	}
	if (status != B_OK && submitted == 0)
		return status;

	// the values of the submitted jobs are used, the others not
	for (uint32 i = 0; i < submitted; i++) {
		Context* context = contexts[i];
		*queue_counter(context, jobs[i].queue) = jobs[i].value;
		if (context->type == POWERVR_CONTEXT_RENDER) {
			uint32 sequence = ++context->lastSequence;
			RenderJob& entry
				= context->renderJobs[sequence % RENDER_JOB_RING];
			entry.sequence = sequence;
			entry.queue = jobs[i].queue;
			entry.value = jobs[i].value;
		}
		requests[i].sequence = context->lastSequence;
		client->jobsSubmitted++;
	}
	return status;
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
	new(&client->freeLists) FreeListList;
	new(&client->datasets) DatasetList;
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
	// jobs reference their contexts and data sets, data sets their free
	// lists, free lists their page list mapping
	while (Context* context = client->contexts.Head()) {
		if (!release_context(client, context))
			return false;
	}
	while (Dataset* dataset = client->datasets.Head()) {
		if (!release_dataset(client, dataset))
			return false;
	}
	while (FreeList* freeList = client->freeLists.Head()) {
		if (release_free_list(client, freeList) != B_OK)
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
		for (DatasetList::Iterator iterator = client->datasets.GetIterator();
				Dataset* dataset = iterator.Next();) {
			dataset->released = true;
		}
		for (FreeListList::Iterator iterator
				= client->freeLists.GetIterator();
				FreeList* freeList = iterator.Next();) {
			freeList->released = true;
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
				|| pvr_find_heap_containing(request.device_address, size)
					== NULL
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
			mapping->pinCount = 0;
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
					if (mapping->pinCount > 0)
						return B_BUSY;
					remove_mapping(client, mapping);
					return pvr_mmu_flush_exec(sDevice->ContextEnvironment(),
						sDevice->Firmware(), client->mmu, 0);
				}
			}
			return B_BAD_VALUE;
		}

		case POWERVR_CREATE_CONTEXT:
		{
			// version 1 clients pass the request without the static state
			const size_t version1Size
				= offsetof(powervr_create_context, static_state);
			powervr_create_context request;
			memset(&request, 0, sizeof(request));
			if ((length != sizeof(request) && length != version1Size)
				|| buffer == NULL || !IS_USER_ADDRESS(buffer)
				|| user_memcpy(&request, buffer, length) != B_OK) {
				return B_BAD_VALUE;
			}
			MutexLocker locker(sDevice->LockObject());
			Context* context;
			status_t status = create_context(client, request, &context);
			if (status != B_OK)
				return status;
			request.handle = context->handle;
			return user_memcpy(buffer, &request, length);
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
			if (context == NULL || context->type != POWERVR_CONTEXT_COMPUTE)
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
			pvr_gpu_context* queue;
			uint32 value;
			wait_target(context, request.sequence, &queue, &value);
			context->waiters++;
			locker.Unlock();

			status = wait_for(queue, value, request.sequence,
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

		case POWERVR_CREATE_FREE_LIST:
		{
			powervr_create_free_list request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;
			MutexLocker locker(sDevice->LockObject());
			FreeList* freeList;
			status = create_free_list(client, request, &freeList);
			if (status != B_OK)
				return status;
			request.handle = freeList->handle;
			return copy_out(buffer, request);
		}

		case POWERVR_DELETE_FREE_LIST:
		{
			int32 handle;
			status_t status = copy_in(handle, buffer, length);
			if (status != B_OK)
				return status;
			MutexLocker locker(sDevice->LockObject());
			FreeList* freeList = find_free_list(client, handle);
			if (freeList == NULL)
				return B_BAD_VALUE;
			return release_free_list(client, freeList);
		}

		case POWERVR_CREATE_HWRT_DATASET:
		{
			powervr_create_hwrt_dataset request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;
			MutexLocker locker(sDevice->LockObject());
			Dataset* dataset;
			status = create_dataset(client, request, &dataset);
			if (status != B_OK)
				return status;
			request.handle = dataset->handle;
			return copy_out(buffer, request);
		}

		case POWERVR_DELETE_HWRT_DATASET:
		{
			int32 handle;
			status_t status = copy_in(handle, buffer, length);
			if (status != B_OK)
				return status;
			MutexLocker locker(sDevice->LockObject());
			Dataset* dataset = find_dataset(client, handle);
			if (dataset == NULL)
				return B_BAD_VALUE;
			return release_dataset(client, dataset) ? B_OK : B_BUSY;
		}

		case POWERVR_SUBMIT_JOBS:
		{
			powervr_submit_jobs request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;
			if (request.count == 0 || request.count > MAX_SUBMIT_JOBS
				|| !IS_USER_ADDRESS(request.jobs)) {
				return B_BAD_VALUE;
			}
			powervr_job jobs[MAX_SUBMIT_JOBS];
			size_t size = request.count * sizeof(powervr_job);
			if (user_memcpy(jobs, request.jobs, size) != B_OK)
				return B_BAD_ADDRESS;
			MutexLocker locker(sDevice->LockObject());
			status = submit_jobs(client, jobs, request.count);
			locker.Unlock();
			if (user_memcpy(request.jobs, jobs, size) != B_OK)
				return B_BAD_ADDRESS;
			return status;
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
			statistics.free_list_grows = sFreeListGrows;
			statistics.free_list_reconstructions = sFreeListReconstructions;
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
	sDevice->SetEventHook(firmware_event, NULL);
	pvr_device_desc_init_bxe_4_32(&sDeviceDesc);
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
