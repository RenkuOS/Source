/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * Exercises the PowerVR BXE-4-32 driver of the JH7110 through its interface:
 * device information, a buffer mapped into the GPU address space, a compute
 * context, compute jobs one by one and in batches, a job whose control stream
 * is not mapped (the GPU MMU has to fault, the firmware to recover) and jobs
 * after that recovery.
 *
 * The jobs are the smallest the compute data master runs: a control stream
 * of one STREAM_TERMINATE word (Mesa csbgen/rogue/cdm.xml). There is no
 * shader compiler here; what is checked is that the CDM fetches its control
 * stream through the client's page tables and completes.
 *
 *   powervr_test [jobs]
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

static int sFailures;


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
		" recoveries, firmware started %" B_PRIu32 " times\n", label,
		statistics.jobs_submitted, statistics.jobs_completed,
		statistics.interrupts, statistics.mmu_page_faults,
		statistics.hardware_recoveries, statistics.firmware_restarts);
}


int
main(int argc, char** argv)
{
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

	print_statistics(fd, "at the end");
	close(fd);

	if (sFailures > 0) {
		printf("%d check(s) failed\n", sFailures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
