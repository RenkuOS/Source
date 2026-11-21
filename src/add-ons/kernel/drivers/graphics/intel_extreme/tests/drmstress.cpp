/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 *
 * Native hardware regression: reject unsupported engine-map extensions,
 * then verify repeated PPGTT stores, ring wrapping and completion.
 * Build and run instructions: ../README.render-node.md.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include <intel_extreme/drm_uapi.h>


// batch: MI_STORE_DWORD_IMM (Gen8 4-dword, PPGTT: bit22 clear) then END
#define MI_STORE_DWORD_IMM_PPGTT	0x10000002
#define MI_BATCH_BUFFER_END_CMD		0x05000000

#define DATA_VA		0x10000000ULL
#define BATCH_VA	0x10001000ULL

static int failures;
#define CHECK(cond, msg) \
	do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)


static void*
gem_mmap(int fd, uint32 handle, size_t size)
{
	drm_i915_gem_mmap args;
	memset(&args, 0, sizeof(args));
	args.handle = handle;
	args.size = size;
	if (ioctl(fd, DRM_IOCTL_I915_GEM_MMAP, &args) != B_OK)
		return NULL;
	return (void*)(uintptr_t)args.addr_ptr;
}


static uint32
gem_create(int fd, size_t size)
{
	drm_i915_gem_create args;
	memset(&args, 0, sizeof(args));
	args.size = size;
	if (ioctl(fd, DRM_IOCTL_I915_GEM_CREATE, &args) != B_OK)
		return 0;
	return args.handle;
}


int
main(int argc, char** argv)
{
	int fd = open("/dev/graphics/intel_render_9x_000200", O_RDWR);
	if (fd < 0) {
		printf("FAIL: open render node\n");
		return 1;
	}

	// Engine-map slots must not be silently accepted as one shared context.
	struct {
		uint64_t extensions;
		struct { uint16_t engine_class, engine_instance; } engines[2];
	} engineMap = {};
	drm_i915_gem_context_create_ext_setparam engineParam = {};
	engineParam.base.name = 0; // I915_CONTEXT_CREATE_EXT_SETPARAM
	engineParam.param.param = I915_CONTEXT_PARAM_ENGINES;
	engineParam.param.size = sizeof(engineMap);
	engineParam.param.value = (uint64_t)(uintptr_t)&engineMap;
	drm_i915_gem_context_create_ext unsupported = {};
	unsupported.flags = I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS;
	unsupported.extensions = (uint64_t)(uintptr_t)&engineParam;
	status_t engineStatus = ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT,
		&unsupported);
	CHECK(engineStatus != B_OK, "engine map must be rejected until implemented");
	if (engineStatus == B_OK) {
		drm_i915_gem_context_destroy cleanup = {};
		cleanup.ctx_id = unsupported.ctx_id;
		ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, &cleanup);
		close(fd);
		return 1;
	}

	// context
	drm_i915_gem_context_create_ext ctx;
	memset(&ctx, 0, sizeof(ctx));
	if (ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT, &ctx) != B_OK) {
		printf("FAIL: CONTEXT_CREATE_EXT\n");
		return 1;
	}
	printf("context %u\n", ctx.ctx_id);

	// buffers
	uint32 dataHandle = gem_create(fd, 4096);
	uint32 batchHandle = gem_create(fd, 4096);
	CHECK(dataHandle != 0 && batchHandle != 0, "GEM_CREATE");

	volatile uint32_t* data = (volatile uint32_t*)gem_mmap(fd, dataHandle,
		4096);
	volatile uint32_t* batch = (volatile uint32_t*)gem_mmap(fd, batchHandle,
		4096);
	CHECK(data != NULL && batch != NULL, "GEM_MMAP");
	if (data == NULL || batch == NULL)
		return 1;
	memset((void*)data, 0, 4096);

	batch[0] = MI_STORE_DWORD_IMM_PPGTT;
	batch[1] = (uint32_t)(DATA_VA & 0xffffffff);
	batch[2] = (uint32_t)(DATA_VA >> 32);
	batch[3] = 0xDEADBEEF;
	batch[4] = MI_BATCH_BUFFER_END_CMD;

	// syncobj the submission will signal
	drm_syncobj_create syncCreate;
	memset(&syncCreate, 0, sizeof(syncCreate));
	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &syncCreate) != B_OK) {
		printf("FAIL: SYNCOBJ_CREATE\n");
		return 1;
	}
	printf("syncobj %u\n", syncCreate.handle);

	// exec objects: objects[0] is the batch (I915_EXEC_BATCH_FIRST)
	drm_i915_gem_exec_object2 objects[2];
	memset(objects, 0, sizeof(objects));
	objects[0].handle = batchHandle;
	objects[0].offset = BATCH_VA;
	objects[0].flags = I915_GEM_EXEC_OBJECT_PINNED
		| I915_GEM_EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	objects[1].handle = dataHandle;
	objects[1].offset = DATA_VA;
	objects[1].flags = I915_GEM_EXEC_OBJECT_PINNED
		| I915_GEM_EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	drm_i915_gem_exec_fence fence;
	memset(&fence, 0, sizeof(fence));
	fence.handle = syncCreate.handle;
	fence.flags = I915_EXEC_FENCE_SIGNAL;

	drm_i915_gem_execbuffer2 exec;
	memset(&exec, 0, sizeof(exec));
	exec.buffers_ptr = (uint64_t)(uintptr_t)objects;
	exec.buffer_count = 2;
	exec.batch_start_offset = 0;
	exec.batch_len = 5 * 4;
	exec.flags = I915_EXEC_RENDER | I915_EXEC_BATCH_FIRST
		| I915_EXEC_NO_RELOC | I915_EXEC_FENCE_ARRAY;
	exec.rsvd1 = ctx.ctx_id;
	exec.cliprects_ptr = (uint64_t)(uintptr_t)&fence;
	exec.num_cliprects = 1;

	const unsigned iterations = argc > 1 ? strtoul(argv[1], NULL, 10) : 4096;
	bigtime_t start = system_time();
	for (unsigned i = 0; i < iterations; ++i) {
		uint32 expected = 0xDEAD0000u + i;
		data[0] = 0;
		batch[3] = expected;
		if (ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, &exec) != B_OK) {
			printf("FAIL: EXECBUFFER2 iteration %u\n", i);
			++failures;
			break;
		}
		drm_i915_gem_wait gemWait = {};
		gemWait.bo_handle = dataHandle;
		gemWait.timeout_ns = 1000000000LL;
		CHECK(ioctl(fd, DRM_IOCTL_I915_GEM_WAIT, &gemWait) == B_OK, "GEM_WAIT");
		drm_syncobj_wait wait = {};
		wait.handles = (uint64_t)(uintptr_t)&syncCreate.handle;
		wait.count_handles = 1;
		wait.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
		wait.timeout_nsec = 0; // synchronous EXEC must already have completed
		CHECK(ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait) == B_OK, "SYNCOBJ poll");
		if (data[0] != expected) {
			printf("FAIL: iteration %u got %#x expected %#x\n", i, data[0], expected);
			++failures;
		}
		if (failures)
			break;
	}
	printf("Requested %u submissions, elapsed %.3f s\n", iterations,
		(system_time() - start) / 1000000.0);

	// teardown
	drm_syncobj_destroy syncDestroy;
	memset(&syncDestroy, 0, sizeof(syncDestroy));
	syncDestroy.handle = syncCreate.handle;
	ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &syncDestroy);

	drm_gem_close closeArgs;
	memset(&closeArgs, 0, sizeof(closeArgs));
	closeArgs.handle = batchHandle;
	ioctl(fd, DRM_IOCTL_GEM_CLOSE, &closeArgs);
	closeArgs.handle = dataHandle;
	ioctl(fd, DRM_IOCTL_GEM_CLOSE, &closeArgs);

	drm_i915_gem_context_destroy destroy;
	memset(&destroy, 0, sizeof(destroy));
	destroy.ctx_id = ctx.ctx_id;
	ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, &destroy);

	close(fd);

	printf(failures == 0 ? "PASS\n" : "FAIL: %d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
