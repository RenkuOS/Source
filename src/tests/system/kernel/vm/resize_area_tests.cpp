/*
 * Copyright 2025, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <OS.h>
#include <stdio.h>
#include <string.h>
#include <syscalls.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>


int
reservation_follows_resize_test()
{
	size_t reserveSize = 1 * 1024 * 1024;
	addr_t newAreaBase;
	status_t status = _kern_reserve_address_range(&newAreaBase,
		B_RANDOMIZED_ANY_ADDRESS, reserveSize);
	if (status < B_OK) {
		printf("reserve_address_range 1 unexpectedly failed!\n");
		return status;
	}

	area_id area = create_area("test area", (void**)&newAreaBase,
		B_EXACT_ADDRESS, B_PAGE_SIZE, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (area < B_OK) {
		printf("create_area 1 unexpectedly failed!\n");
		return area;
	}

	// Resize the area larger (shriking the reservation)
	status = resize_area(area, B_PAGE_SIZE * 2);
	if (status < B_OK)
		return status;

	// Make sure all is as expected
	addr_t past = (newAreaBase + B_PAGE_SIZE * 2);
	status = _kern_reserve_address_range(&past, B_EXACT_ADDRESS, B_PAGE_SIZE);
	if (status == B_OK) {
		printf("reserve_address_range 2 unexpectedly succeeded!\n");
		return status;
	}

	status = area_for((void*)(newAreaBase + B_PAGE_SIZE * 1));
	if (status != area)
		return status;

	// Now resize the area smaller again
	status = resize_area(area, B_PAGE_SIZE * 1);
	if (status < B_OK)
		return status;

	// And check that the reservation resized with it
	past = (newAreaBase + B_PAGE_SIZE * 1);
	status = _kern_reserve_address_range(&past, B_EXACT_ADDRESS, B_PAGE_SIZE);
	if (status == B_OK) {
		printf("reservation did not resize downwards!\n");
		return status;
	}

	// And check that the reservation still extends to where we expect
	past = (newAreaBase + reserveSize - B_PAGE_SIZE);
	status = _kern_reserve_address_range(&past, B_EXACT_ADDRESS, B_PAGE_SIZE);
	if (status == B_OK) {
		printf("reservation has wrong size!\n");
		return status;
	}

	past = (newAreaBase + reserveSize);
	status = _kern_reserve_address_range(&past, B_EXACT_ADDRESS, B_PAGE_SIZE);
	if (status != B_OK) {
		printf("reservation has wrong size!\n");
		return status;
	}

	delete_area(area);
	return _kern_unreserve_address_range(newAreaBase, reserveSize);
}


int
resize_past_reservation_test()
{
	size_t reserveSize = 4 * B_PAGE_SIZE;
	addr_t newAreaBase;
	status_t status = _kern_reserve_address_range(&newAreaBase,
		B_RANDOMIZED_ANY_ADDRESS, reserveSize * 2);
	if (status < B_OK) {
		printf("reserve_address_range 2 unexpectedly failed!\n");
		return status;
	}

	// Shrink the reservation partially
	status = _kern_unreserve_address_range(newAreaBase + reserveSize, reserveSize);
	if (status < B_OK)
		return status;

	// Make sure it's gone
	status = area_for((void*)(newAreaBase + reserveSize));
	if (status != B_ERROR)
		return -1;

	area_id area = create_area("test area", (void**)&newAreaBase,
		B_EXACT_ADDRESS, B_PAGE_SIZE, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (area < B_OK) {
		printf("create_area 2 unexpectedly failed!\n");
		return area;
	}

	// Resize the area larger, past the reservation entirely
	status = resize_area(area, reserveSize * 2);
	if (status < B_OK) {
		printf("failed to resize area past first reservation!\n");
		return status;
	}

	// Shrink again, put two new reservations there
	status = resize_area(area, reserveSize);
	if (status < B_OK)
		return status;

	addr_t reserve = newAreaBase + reserveSize + B_PAGE_SIZE;
	status = _kern_reserve_address_range(&reserve, B_EXACT_ADDRESS, B_PAGE_SIZE);
	if (status < B_OK)
		return status;

	reserve += B_PAGE_SIZE;
	status = _kern_reserve_address_range(&reserve, B_EXACT_ADDRESS, B_PAGE_SIZE);
	if (status < B_OK)
		return status;

	// Resize the area larger again, past both reservations
	status = resize_area(area, reserveSize * 2);
	if (status < B_OK) {
		printf("failed to resize area past second and third reservations!\n");
		return status;
	}

	return 0;
}


// Forks a child that keeps a copy of the memory at address, tries to shrink
// that memory's area to one page in the parent, and returns the result.
// The child's own area must keep its size either way.
static status_t
fork_and_shrink(void* address, status_t& childSizeOk)
{
	int toChild[2];
	int toParent[2];
	if (pipe(toChild) != 0 || pipe(toParent) != 0)
		return B_ERROR;

	pid_t child = fork();
	if (child < 0)
		return child;
	if (child == 0) {
		char c;
		read(toChild[0], &c, 1);
		area_info info;
		status_t ok = get_area_info(area_for(address), &info) == B_OK
			&& info.size == 4 * B_PAGE_SIZE ? B_OK : B_ERROR;
		write(toParent[1], &ok, sizeof(ok));
		_exit(0);
	}

	status_t status = resize_area(area_for(address), B_PAGE_SIZE);
	write(toChild[1], "x", 1);
	if (read(toParent[0], &childSizeOk, sizeof(childSizeOk))
			!= sizeof(childSizeOk)) {
		childSizeOk = B_ERROR;
	}
	waitpid(child, NULL, 0);
	close(toChild[0]);
	close(toChild[1]);
	close(toParent[0]);
	close(toParent[1]);
	return status;
}


int
shrink_shared_area_test()
{
	// A clone in another team must not be able to shrink the area under
	// its owner, and the owner must not shrink it under the clone.
	void* address;
	area_id area = create_area("shared test area", &address, B_ANY_ADDRESS,
		4 * B_PAGE_SIZE, B_NO_LOCK,
		B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
	if (area < B_OK)
		return area;

	int toChild[2];
	int toParent[2];
	if (pipe(toChild) != 0 || pipe(toParent) != 0)
		return B_ERROR;

	pid_t child = fork();
	if (child < 0)
		return child;
	if (child == 0) {
		void* cloneAddress;
		area_id clone = clone_area("shared test clone", &cloneAddress,
			B_ANY_ADDRESS, B_READ_AREA, area);
		status_t status = clone < B_OK
			? clone : resize_area(clone, B_PAGE_SIZE);
		write(toParent[1], &status, sizeof(status));
		char c;
		read(toChild[0], &c, 1);
		delete_area(clone);
		write(toParent[1], &status, sizeof(status));
		_exit(0);
	}

	status_t cloneStatus = B_ERROR;
	read(toParent[0], &cloneStatus, sizeof(cloneStatus));
	area_info info;
	get_area_info(area, &info);
	status_t ownerStatus = resize_area(area, B_PAGE_SIZE);
	write(toChild[1], "x", 1);
	status_t dummy;
	read(toParent[0], &dummy, sizeof(dummy));
	waitpid(child, NULL, 0);
	status_t unsharedStatus = resize_area(area, B_PAGE_SIZE);
	delete_area(area);
	close(toChild[0]);
	close(toChild[1]);
	close(toParent[0]);
	close(toParent[1]);

	if (cloneStatus != B_NOT_ALLOWED) {
		printf("a clone in another team shrank the area: %s\n",
			strerror(cloneStatus));
		return -1;
	}
	if (info.size != 4 * B_PAGE_SIZE) {
		printf("the area shrank to %zu bytes\n", info.size);
		return -1;
	}
	if (ownerStatus != B_NOT_ALLOWED) {
		printf("the owner shrank an area another team maps: %s\n",
			strerror(ownerStatus));
		return -1;
	}
	if (unsharedStatus != B_OK) {
		printf("shrinking the area failed once it was no longer shared: "
			"%s\n", strerror(unsharedStatus));
		return -1;
	}
	return 0;
}


int
shrink_after_fork_test()
{
	// fork() gives private memory a copy-on-write cache per team, so it
	// can still be shrunk. Shared memory stays shared, so it cannot.
	void* address;
	area_id area = create_area("private test area", &address, B_ANY_ADDRESS,
		4 * B_PAGE_SIZE, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (area < B_OK)
		return area;
	memset(address, 0x55, 4 * B_PAGE_SIZE);

	status_t childSizeOk;
	status_t status = fork_and_shrink(address, childSizeOk);
	delete_area(area);
	if (status != B_OK) {
		printf("shrinking private memory after fork() failed: %s\n",
			strerror(status));
		return -1;
	}
	if (childSizeOk != B_OK) {
		printf("shrinking private memory changed the child's copy\n");
		return -1;
	}

	address = mmap(NULL, 4 * B_PAGE_SIZE, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (address == MAP_FAILED)
		return B_NO_MEMORY;
	status = fork_and_shrink(address, childSizeOk);
	munmap(address, 4 * B_PAGE_SIZE);
	if (status != B_NOT_ALLOWED) {
		printf("shrinking shared memory after fork() did not fail with "
			"B_NOT_ALLOWED: %s\n", strerror(status));
		return -1;
	}
	if (childSizeOk != B_OK) {
		printf("shrinking shared memory changed the child's mapping\n");
		return -1;
	}
	return 0;
}


int
main()
{
	status_t status;

	if ((status = reservation_follows_resize_test()) != 0)
		return status;

	if ((status = resize_past_reservation_test()) != 0)
		return status;

	if ((status = shrink_shared_area_test()) != 0)
		return status;

	if ((status = shrink_after_fork_test()) != 0)
		return status;

	return 0;
}
