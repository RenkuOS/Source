/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * Exercises the PowerVR BXE-4-32 driver of the JH7110 through its interface:
 * device information, a buffer mapped into the GPU address space, a compute
 * context, compute jobs one by one and in batches, a job whose control stream
 * is not mapped (the GPU MMU has to fault, the firmware to recover) and jobs
 * after that recovery. With interface version 2 it also creates and destroys
 * the objects of rendering: free lists, HWRT data sets, render and transfer
 * contexts, also with a client that goes away holding them.
 *
 * The jobs are the smallest the compute data master runs: a control stream
 * of one STREAM_TERMINATE word (Mesa csbgen/rogue/cdm.xml). There is no
 * shader compiler here; what is checked is that the CDM fetches its control
 * stream through the client's page tables and completes.
 *
 *   powervr_test [jobs]
 *   powervr_test stats		only prints the device's counters
 *   powervr_test perf [s]	samples the GPU's performance counters every
 *							second for s seconds (default 10)
 */


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include <powervr_jh7110.h>


static const uint64 kBufferAddress = POWERVR_GENERAL_HEAP_BASE + 0x100000;
static const uint64 kUnmappedAddress = POWERVR_GENERAL_HEAP_BASE + 0x40000000;
static const uint32 kStreamTerminate = 0x80000000;
	// CDMCTRL STREAM_TERMINATE: block_type 2 in bits 31:30

static const uint64 kRenderAddress = POWERVR_GENERAL_HEAP_BASE + 0x1000000;
static const uint32 kRenderBufferSize = 1024 * 1024;

static int sFailures;
static const char* sStep = "";
	// the step of create_render_objects() that failed


#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			printf("FAIL: " __VA_ARGS__); \
			printf("\n"); \
			sFailures++; \
		} \
	} while (false)


static status_t
control(int fd, uint32 op, void* data, size_t size)
{
	if (ioctl(fd, op, data, size) < 0)
		return errno;
	return B_OK;
}


static status_t
submit(int fd, int32 context, uint64 streamAddress, uint32* _sequence)
{
	powervr_submit_compute submit;
	memset(&submit, 0, sizeof(submit));
	submit.context = context;
	submit.registers.cdm_ctrl_stream_base = streamAddress;
	submit.registers.cdm_context_state_base = kBufferAddress + 0x1000;
	// TPU_BORDER_COLOUR_TABLE_CDM: address in bits 37:0, shift 2
	submit.registers.tpu_border_colour_table = (kBufferAddress + 0x2000) >> 2;
	status_t status = control(fd, POWERVR_SUBMIT_COMPUTE, &submit,
		sizeof(submit));
	*_sequence = submit.sequence;
	return status;
}


static status_t
wait_for(int fd, int32 context, uint32 sequence, bigtime_t timeout,
	powervr_wait* _wait = NULL)
{
	powervr_wait wait;
	memset(&wait, 0, sizeof(wait));
	wait.context = context;
	wait.sequence = sequence;
	wait.timeout = timeout;
	status_t status = control(fd, POWERVR_WAIT, &wait, sizeof(wait));
	if (_wait != NULL)
		*_wait = wait;
	return status;
}


static bool
get_statistics(int fd, powervr_statistics& statistics)
{
	return control(fd, POWERVR_GET_STATISTICS, &statistics,
		sizeof(statistics)) == B_OK;
}


struct WaitThreadData {
	int				fd;
	int32			context;
	uint32			sequence;
	status_t		status;
	powervr_wait	result;
};


static status_t
wait_thread(void* data)
{
	WaitThreadData* wait = (WaitThreadData*)data;
	wait->status = wait_for(wait->fd, wait->context, wait->sequence,
		5000000, &wait->result);
	return B_OK;
}


static void
print_statistics(int fd, const char* label)
{
	powervr_statistics statistics;
	if (control(fd, POWERVR_GET_STATISTICS, &statistics, sizeof(statistics))
			!= B_OK) {
		printf("%s: no statistics\n", label);
		return;
	}
	printf("%s: %" B_PRIu32 " jobs submitted, %" B_PRIu32 " completed, %"
		B_PRIu32 " interrupts, %" B_PRIu32 " MMU faults, %" B_PRIu32
		" recoveries, firmware started %" B_PRIu32 " times, %" B_PRIu32
		" free list grows, %" B_PRIu32 " rebuilds, %" B_PRIu32 " MB in %"
		B_PRIu32 " ms of cache sync, %" B_PRIu32 " ms waiting\n", label,
		statistics.jobs_submitted, statistics.jobs_completed,
		statistics.interrupts, statistics.mmu_page_faults,
		statistics.hardware_recoveries, statistics.firmware_restarts,
		statistics.free_list_grows, statistics.free_list_reconstructions,
		statistics.cache_sync_megabytes, statistics.cache_sync_milliseconds,
		statistics.wait_milliseconds);
}


/*!	A render context static state as Mesa's pvr driver packs it
	(pvr_drm_render_ctx_static_state_init()): the KMD stream header (length
	in bytes), the VDM and geometry context state addresses and the store
	and resume tasks of the two geometry states, all zero here.
*/
static uint32
render_static_state(uint64* stream, uint64 stateAddress)
{
	uint32 count = 0;
	stream[count++] = 0;
	stream[count++] = stateAddress;
	stream[count++] = 0;
	stream[count++] = stateAddress + 0x1000;
	for (int i = 0; i < 2 * 10; i++)
		stream[count++] = 0;
	stream[0] = count * sizeof(uint64);
	return count * sizeof(uint64);
}


/*!	Creates the objects of a render in the client fd: a buffer mapped at
	kRenderAddress holding the free list (offset 0) and the data the HWRT
	data set points to, a free list, a data set and render and transfer
	contexts. The handles are returned; nothing runs on the GPU.
*/
static status_t
create_render_objects(int fd, int32* _buffer, int32* _globalList,
	int32* _freeList, int32* _dataset, int32* _renderContext,
	int32* _transferContext)
{
	powervr_create_buffer buffer;
	memset(&buffer, 0, sizeof(buffer));
	buffer.size = kRenderBufferSize;
	sStep = "CREATE_BUFFER";
	status_t status = control(fd, POWERVR_CREATE_BUFFER, &buffer,
		sizeof(buffer));
	if (status != B_OK)
		return status;
	*_buffer = buffer.handle;

	powervr_map_buffer map;
	memset(&map, 0, sizeof(map));
	map.handle = buffer.handle;
	map.device_address = kRenderAddress;
	sStep = "MAP_BUFFER";
	status = control(fd, POWERVR_MAP_BUFFER, &map, sizeof(map));
	if (status != B_OK)
		return status;

	// the device wide free list a render target list grows from (Mesa's
	// global free list)
	powervr_create_free_list globalList;
	memset(&globalList, 0, sizeof(globalList));
	globalList.free_list_address = kRenderAddress + 0x4000;
	globalList.initial_pages = 64;
	globalList.max_pages = 256;
	globalList.grow_pages = 32;
	globalList.grow_threshold = 13;
	sStep = "CREATE_FREE_LIST global";
	status = control(fd, POWERVR_CREATE_FREE_LIST, &globalList,
		sizeof(globalList));
	if (status != B_OK)
		return status;
	*_globalList = globalList.handle;

	powervr_create_free_list freeList;
	memset(&freeList, 0, sizeof(freeList));
	freeList.free_list_address = kRenderAddress;
	freeList.initial_pages = 64;
	freeList.max_pages = 256;
	freeList.grow_pages = 32;
	freeList.grow_threshold = 13;
	sStep = "CREATE_FREE_LIST";
	status = control(fd, POWERVR_CREATE_FREE_LIST, &freeList,
		sizeof(freeList));
	if (status != B_OK)
		return status;
	*_freeList = freeList.handle;

	powervr_create_hwrt_dataset dataset;
	memset(&dataset, 0, sizeof(dataset));
	dataset.tpc_address = kRenderAddress + 0x10000;
	dataset.tpc_size = 0x10000;
	dataset.tpc_stride = 0x400;
	dataset.vheap_table_address = kRenderAddress + 0x20000;
	dataset.rtc_address = kRenderAddress + 0x30000;
	for (int i = 0; i < 2; i++) {
		uint64 base = kRenderAddress + 0x40000 + i * 0x20000;
		dataset.rt_data[i].pm_mlist_address = base;
		dataset.rt_data[i].macrotile_array_address = base + 0x8000;
		dataset.rt_data[i].region_header_address = base + 0x10000;
	}
	dataset.free_lists[POWERVR_HWRT_FREE_LIST_LOCAL] = freeList.handle;
	dataset.free_lists[POWERVR_HWRT_FREE_LIST_GLOBAL] = globalList.handle;
	dataset.width = 256;
	dataset.height = 256;
	dataset.samples = 1;
	dataset.layers = 1;
	dataset.isp_merge_scale_x = 0x3f800000;	// 1.0f
	dataset.isp_merge_scale_y = 0x3f800000;
	dataset.region_header_size = 0x8000;
	sStep = "CREATE_HWRT_DATASET";
	status = control(fd, POWERVR_CREATE_HWRT_DATASET, &dataset,
		sizeof(dataset));
	if (status != B_OK)
		return status;
	*_dataset = dataset.handle;

	uint64 staticState[32];
	powervr_create_context context;
	memset(&context, 0, sizeof(context));
	context.type = POWERVR_CONTEXT_RENDER;
	context.priority = 1;
	context.static_state = staticState;
	context.static_state_size = render_static_state(staticState,
		kRenderAddress + 0x80000);
	context.callstack_address = kRenderAddress + 0x90000;
	sStep = "CREATE_CONTEXT render";
	status = control(fd, POWERVR_CREATE_CONTEXT, &context, sizeof(context));
	if (status != B_OK)
		return status;
	*_renderContext = context.handle;

	memset(&context, 0, sizeof(context));
	context.type = POWERVR_CONTEXT_TRANSFER;
	context.priority = 1;
	sStep = "CREATE_CONTEXT transfer";
	status = control(fd, POWERVR_CREATE_CONTEXT, &context, sizeof(context));
	if (status != B_OK)
		return status;
	*_transferContext = context.handle;
	return B_OK;
}


static void
test_render_objects(int cycles)
{
	int fd = open(POWERVR_JH7110_DEVICE, O_RDWR);
	CHECK(fd >= 0, "second open: %s", strerror(errno));
	if (fd < 0)
		return;

	bigtime_t start = system_time();
	int created = 0;
	for (int cycle = 0; cycle < cycles; cycle++) {
		int32 buffer = -1, globalList = -1, freeList = -1, dataset = -1,
			renderContext = -1, transferContext = -1;
		status_t status = create_render_objects(fd, &buffer, &globalList,
			&freeList, &dataset, &renderContext, &transferContext);
		CHECK(status == B_OK, "render objects, cycle %d, %s: %s", cycle,
			sStep, strerror(status));
		if (status != B_OK)
			break;

		// in use: a free list by its data set, nothing else
		status = control(fd, POWERVR_DELETE_FREE_LIST, &freeList,
			sizeof(freeList));
		CHECK(status == B_BUSY, "DELETE_FREE_LIST of a used list: %s",
			strerror(status));

		status = control(fd, POWERVR_DELETE_CONTEXT, &transferContext,
			sizeof(transferContext));
		CHECK(status == B_OK, "DELETE_CONTEXT transfer: %s",
			strerror(status));
		status = control(fd, POWERVR_DELETE_CONTEXT, &renderContext,
			sizeof(renderContext));
		CHECK(status == B_OK, "DELETE_CONTEXT render: %s", strerror(status));
		status = control(fd, POWERVR_DELETE_HWRT_DATASET, &dataset,
			sizeof(dataset));
		CHECK(status == B_OK, "DELETE_HWRT_DATASET: %s", strerror(status));
		status = control(fd, POWERVR_DELETE_FREE_LIST, &freeList,
			sizeof(freeList));
		CHECK(status == B_OK, "DELETE_FREE_LIST: %s", strerror(status));
		status = control(fd, POWERVR_DELETE_FREE_LIST, &globalList,
			sizeof(globalList));
		CHECK(status == B_OK, "DELETE_FREE_LIST global: %s",
			strerror(status));

		powervr_unmap_buffer unmap;
		unmap.device_address = kRenderAddress;
		unmap.size = 0;
		status = control(fd, POWERVR_UNMAP_BUFFER, &unmap, sizeof(unmap));
		CHECK(status == B_OK, "UNMAP_BUFFER render: %s", strerror(status));
		status = control(fd, POWERVR_DELETE_BUFFER, &buffer, sizeof(buffer));
		CHECK(status == B_OK, "DELETE_BUFFER render: %s", strerror(status));
		created++;
	}
	printf("render objects created and destroyed %d times in %" B_PRId64
		" ms\n", created, (system_time() - start) / 1000);

	// a client that goes away with all of them: the driver cleans up
	int32 buffer, globalList, freeList, dataset, renderContext,
		transferContext;
	status_t status = create_render_objects(fd, &buffer, &globalList,
		&freeList, &dataset, &renderContext, &transferContext);
	CHECK(status == B_OK, "render objects before close, %s: %s", sStep,
		strerror(status));
	close(fd);
}


int
main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "stats") == 0) {
		int fd = open(POWERVR_JH7110_DEVICE, O_RDWR);
		if (fd < 0)
			return 1;
		print_statistics(fd, "device");
		close(fd);
		return 0;
	}

	if (argc > 1 && strcmp(argv[1], "perf") == 0) {
		int seconds = argc > 2 ? atoi(argv[2]) : 10;
		int fd = open(POWERVR_JH7110_DEVICE, O_RDWR);
		if (fd < 0)
			return 1;
		powervr_perf_counters previous, current;
		if (control(fd, POWERVR_GET_PERF_COUNTERS, &previous,
				sizeof(previous)) != B_OK) {
			printf("no performance counters\n");
			return 1;
		}
		printf("   TA busy  3D busy  TA|3D   TA jobs 3D jobs  SLC rd/wr "
			"stall (%% of the cycles)\n");
		for (int i = 0; i < seconds; i++) {
			snooze(1000000);
			if (control(fd, POWERVR_GET_PERF_COUNTERS, &current,
					sizeof(current)) != B_OK) {
				break;
			}
			// the counters wrap: unsigned differences over one second
			double cycles = (current.time - previous.time)
				* (double)current.core_clock_mhz;
			printf("   %6.1f%%  %6.1f%%  %6.1f%%  %6" B_PRIu32 "  %6"
				B_PRIu32 "  %5.1f%% %5.1f%%\n",
				100.0 * (uint32)(current.ta_cycles - previous.ta_cycles)
					/ cycles,
				100.0 * (uint32)(current.fragment_cycles
					- previous.fragment_cycles) / cycles,
				100.0 * (uint32)(current.ta_or_fragment_cycles
					- previous.ta_or_fragment_cycles) / cycles,
				(uint32)(current.ta_phases - previous.ta_phases),
				(uint32)(current.fragment_phases - previous.fragment_phases),
				100.0 * (uint32)(current.slc_read_stalls
					- previous.slc_read_stalls) / cycles,
				100.0 * (uint32)(current.slc_write_stalls
					- previous.slc_write_stalls) / cycles);
			previous = current;
		}
		close(fd);
		return 0;
	}

	int jobs = argc > 1 ? atoi(argv[1]) : 1000;
	if (jobs < 1)
		jobs = 1;

	bigtime_t start = system_time();
	int fd = open(POWERVR_JH7110_DEVICE, O_RDWR);
	if (fd < 0) {
		printf("cannot open %s: %s\n", POWERVR_JH7110_DEVICE,
			strerror(errno));
		return 1;
	}
	printf("opened in %" B_PRId64 " ms (GPU powered and firmware started)\n",
		(system_time() - start) / 1000);

	powervr_info info;
	status_t status = control(fd, POWERVR_GET_INFO, &info, sizeof(info));
	CHECK(status == B_OK, "GET_INFO: %s", strerror(status));
	printf("GPU BVNC %u.%u.%u.%u, %" B_PRIu32 " MHz, firmware %u.%u build %"
		B_PRIu32 ", interface %" B_PRIu32 "\n",
		(unsigned)(info.bvnc >> 48), (unsigned)(info.bvnc >> 32) & 0xffff,
		(unsigned)(info.bvnc >> 16) & 0xffff, (unsigned)info.bvnc & 0xffff,
		info.core_clock_mhz, info.firmware_major, info.firmware_minor,
		info.firmware_build, info.api_version);

	// a buffer: control stream, context state, border colour table
	powervr_create_buffer create;
	memset(&create, 0, sizeof(create));
	create.size = 64 * 1024;
	status = control(fd, POWERVR_CREATE_BUFFER, &create, sizeof(create));
	CHECK(status == B_OK, "CREATE_BUFFER: %s", strerror(status));
	if (status != B_OK)
		return 1;
	((uint32*)create.address)[0] = kStreamTerminate;

	powervr_map_buffer map;
	memset(&map, 0, sizeof(map));
	map.handle = create.handle;
	map.device_address = kBufferAddress;
	status = control(fd, POWERVR_MAP_BUFFER, &map, sizeof(map));
	CHECK(status == B_OK, "MAP_BUFFER: %s", strerror(status));

	powervr_map_buffer overlap = map;
	status = control(fd, POWERVR_MAP_BUFFER, &overlap, sizeof(overlap));
	CHECK(status == B_BUSY, "overlapping MAP_BUFFER accepted: %s",
		strerror(status));

	powervr_create_context context;
	memset(&context, 0, sizeof(context));
	context.type = POWERVR_CONTEXT_COMPUTE;
	context.priority = 1;
	status = control(fd, POWERVR_CREATE_CONTEXT, &context, sizeof(context));
	CHECK(status == B_OK, "CREATE_CONTEXT: %s", strerror(status));
	if (status != B_OK)
		return 1;

	// one job
	uint32 sequence;
	start = system_time();
	status = submit(fd, context.handle, kBufferAddress, &sequence);
	CHECK(status == B_OK, "SUBMIT_COMPUTE: %s", strerror(status));
	status = wait_for(fd, context.handle, sequence, 2000000);
	CHECK(status == B_OK, "first job: %s", strerror(status));
	printf("first compute job: %s in %" B_PRId64 " us\n", strerror(status),
		system_time() - start);

	// one by one
	start = system_time();
	int completed = 0;
	for (int i = 0; i < jobs; i++) {
		if (submit(fd, context.handle, kBufferAddress, &sequence) != B_OK
			|| wait_for(fd, context.handle, sequence, 2000000) != B_OK) {
			break;
		}
		completed++;
	}
	bigtime_t elapsed = system_time() - start;
	CHECK(completed == jobs, "only %d of %d jobs one by one", completed, jobs);
	printf("%d jobs one by one: %" B_PRId64 " us each\n", completed,
		completed > 0 ? elapsed / completed : 0);

	// in batches of 64, waiting for the last one
	start = system_time();
	completed = 0;
	while (completed < jobs) {
		int batch = min_c(64, jobs - completed);
		status = B_OK;
		for (int i = 0; i < batch && status == B_OK; i++)
			status = submit(fd, context.handle, kBufferAddress, &sequence);
		if (status == B_OK)
			status = wait_for(fd, context.handle, sequence, 5000000);
		if (status != B_OK)
			break;
		completed += batch;
	}
	elapsed = system_time() - start;
	CHECK(completed == jobs, "only %d of %d jobs in batches: %s", completed,
		jobs, strerror(status));
	printf("%d jobs in batches of 64: %" B_PRId64 " us each\n", completed,
		completed > 0 ? elapsed / completed : 0);

	// a big buffer, mapped from an offset, with the control stream deep in
	// it: the mapping walks several physical runs
	powervr_create_buffer big;
	memset(&big, 0, sizeof(big));
	big.size = 8 * 1024 * 1024;
	status = control(fd, POWERVR_CREATE_BUFFER, &big, sizeof(big));
	CHECK(status == B_OK, "CREATE_BUFFER of 8 MB: %s", strerror(status));
	if (status == B_OK) {
		const uint64 bigAddress = POWERVR_GENERAL_HEAP_BASE + 0x10000000;
		const uint64 offset = 1024 * 1024;
		const uint64 streamOffset = 5 * 1024 * 1024 + 0x340;
		((uint32*)big.address)[streamOffset / 4] = kStreamTerminate;
		powervr_map_buffer bigMap;
		memset(&bigMap, 0, sizeof(bigMap));
		bigMap.handle = big.handle;
		bigMap.offset = offset;
		bigMap.device_address = bigAddress;
		status = control(fd, POWERVR_MAP_BUFFER, &bigMap, sizeof(bigMap));
		CHECK(status == B_OK, "MAP_BUFFER at an offset: %s",
			strerror(status));
		powervr_wait result;
		if (status == B_OK) {
			status = submit(fd, context.handle,
				bigAddress + streamOffset - offset, &sequence);
			if (status == B_OK) {
				status = wait_for(fd, context.handle, sequence, 2000000,
					&result);
			}
			CHECK(status == B_OK && result.flags == 0,
				"job in the 8 MB buffer: %s, flags %#" B_PRIx32,
				strerror(status), result.flags);
			printf("job with its control stream 5 MB into an 8 MB buffer: "
				"%s\n", strerror(status));
			powervr_unmap_buffer bigUnmap;
			bigUnmap.device_address = bigAddress;
			bigUnmap.size = 0;
			control(fd, POWERVR_UNMAP_BUFFER, &bigUnmap, sizeof(bigUnmap));
		}
		control(fd, POWERVR_DELETE_BUFFER, &big.handle, sizeof(big.handle));
	}

	// while another thread waits for the last of 120 queued jobs (some
	// 16 ms), DELETE_CONTEXT has to refuse the context
	status = B_OK;
	for (int i = 0; i < 120 && status == B_OK; i++)
		status = submit(fd, context.handle, kBufferAddress, &sequence);
	if (status == B_OK) {
		WaitThreadData data;
		memset(&data, 0, sizeof(data));
		data.fd = fd;
		data.context = context.handle;
		data.sequence = sequence;
		thread_id thread = spawn_thread(wait_thread, "powervr waiter",
			B_NORMAL_PRIORITY, &data);
		resume_thread(thread);
		snooze(2000);
		status_t deleteStatus = control(fd, POWERVR_DELETE_CONTEXT,
			&context.handle, sizeof(context.handle));
		status_t threadStatus;
		wait_for_thread(thread, &threadStatus);
		printf("DELETE_CONTEXT while it is waited for: %s, the wait: %s\n",
			strerror(deleteStatus), strerror(data.status));
		CHECK(deleteStatus == B_BUSY,
			"DELETE_CONTEXT during a wait: %s", strerror(deleteStatus));
		CHECK(data.status == B_OK, "the wait: %s", strerror(data.status));
	}

	// a control stream the MMU does not translate
	powervr_statistics before, after;
	get_statistics(fd, before);
	print_statistics(fd, "before the fault");
	start = system_time();
	powervr_wait result;
	memset(&result, 0, sizeof(result));
	status = submit(fd, context.handle, kUnmappedAddress, &sequence);
	if (status == B_OK) {
		WaitThreadData data;
		memset(&data, 0, sizeof(data));
		data.fd = fd;
		data.context = context.handle;
		data.sequence = sequence;
		thread_id thread = spawn_thread(wait_thread, "powervr fault waiter",
			B_NORMAL_PRIORITY, &data);
		resume_thread(thread);
		status_t threadStatus;
		wait_for_thread(thread, &threadStatus);
		status = data.status;
		result = data.result;
	}
	printf("job with an unmapped control stream: %s after %" B_PRId64
		" us, flags %#" B_PRIx32 ", reason %" B_PRIu32 ", fault at %#"
		B_PRIx64 "\n", strerror(status), system_time() - start, result.flags,
		result.reset_reason, result.fault_address);
	// the firmware may queue the reset notification right after it wrote
	// the timeline value: then the next wait reports it
	powervr_wait next;
	memset(&next, 0, sizeof(next));
	status_t nextStatus = submit(fd, context.handle, kBufferAddress,
		&sequence);
	if (nextStatus == B_OK) {
		nextStatus = wait_for(fd, context.handle, sequence, 2000000,
			&next);
	}
	CHECK(nextStatus == B_OK, "job after the recovery: %s",
		strerror(nextStatus));
	if (next.flags != 0) {
		result.flags |= next.flags;
		result.reset_reason = next.reset_reason;
		result.fault_address = next.fault_address;
	}
	get_statistics(fd, after);
	print_statistics(fd, "after the fault");
	CHECK((result.flags & POWERVR_WAIT_CONTEXT_RESET) != 0
			&& (result.flags & POWERVR_WAIT_PAGE_FAULT) != 0
			&& result.fault_address >= kUnmappedAddress
			&& result.fault_address < kUnmappedAddress + B_PAGE_SIZE,
		"the faulting job was not reported as a page fault at %#" B_PRIx64,
		kUnmappedAddress);
	CHECK(after.mmu_page_faults >= before.mmu_page_faults + 1,
		"MMU faults %" B_PRIu32 " -> %" B_PRIu32, before.mmu_page_faults,
		after.mmu_page_faults);
	// the counters are the device's: other clients may fault too
	CHECK(after.hardware_recoveries >= before.hardware_recoveries + 1,
		"recoveries %" B_PRIu32 " -> %" B_PRIu32,
		before.hardware_recoveries, after.hardware_recoveries);

	printf("job after the recovery: %s\n", strerror(nextStatus));

	// teardown through the interface
	status = control(fd, POWERVR_DELETE_CONTEXT, &context.handle,
		sizeof(context.handle));
	CHECK(status == B_OK, "DELETE_CONTEXT: %s", strerror(status));
	status = control(fd, POWERVR_DELETE_BUFFER, &create.handle,
		sizeof(create.handle));
	CHECK(status == B_BUSY, "DELETE_BUFFER of a mapped buffer: %s",
		strerror(status));
	powervr_unmap_buffer unmap;
	unmap.device_address = kBufferAddress;
	unmap.size = 0;
	status = control(fd, POWERVR_UNMAP_BUFFER, &unmap, sizeof(unmap));
	CHECK(status == B_OK, "UNMAP_BUFFER: %s", strerror(status));
	status = control(fd, POWERVR_DELETE_BUFFER, &create.handle,
		sizeof(create.handle));
	CHECK(status == B_OK, "DELETE_BUFFER: %s", strerror(status));

	if (info.api_version >= 2)
		test_render_objects(20);

	print_statistics(fd, "at the end");
	close(fd);

	if (sFailures > 0) {
		printf("%d check(s) failed\n", sFailures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
