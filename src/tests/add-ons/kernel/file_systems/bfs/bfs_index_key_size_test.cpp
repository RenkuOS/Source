/*
 * Copyright 2026, Dru Satori, dru@druware.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	Regression test: BFS must not put a value shorter than an index's key
	size into the index B+tree (https://github.com/RenkuOS/Source/issues/713).

	The tree compares the keys of an int64 index as whole int64 values, so a
	4-byte key was compared using bytes that are not its own: it sorted into
	the wrong place, could not be removed when its file was deleted, and the
	entry stayed behind pointing at a freed inode. Once a new file reused
	that inode, a query returned the new file twice.

	The test creates the int64 index "test:bfs_index_key_size", writes
	kFileCount files with 8-byte values, one file with a 4-byte int32 value,
	and one file whose int64 value 1234 is overwritten by the int32 value
	1234. It then checks that:
	- a query returns only the 8-byte values (short values are not indexed);
	- after all files are deleted, the query returns nothing;
	- after the same number of files is created again, reusing the freed
	  inodes, the query returns each of them exactly once;
	- syslog has no "Could not find value in index" line for the index.
	It removes its files and the index at the end, prints PASS and exits
	with 0, or prints FAIL and exits with 1.

	Usage: bfs_index_key_size_test <scratch directory on a BFS volume>

	Warning: on a BFS without the fix, this test leaves broken entries in
	the volume's indices. Run it on a scratch volume, or run "checkfs" on
	the volume afterwards to rebuild them.
*/


#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fs_attr.h>
#include <fs_index.h>
#include <fs_info.h>
#include <fs_query.h>
#include <TypeConstants.h>


static const char* kIndex = "test:bfs_index_key_size";
static const char* kQuery = "test:bfs_index_key_size>0";
static const int kFileCount = 200;
static const int kMaxHits = 4 * kFileCount;

static dev_t sDevice;
static const char* sDirectory;
static bool sFailed = false;


static void
check(bool ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		sFailed = true;
}


static int
compare_ino(const void* a, const void* b)
{
	ino_t first = *(const ino_t*)a;
	ino_t second = *(const ino_t*)b;
	return first < second ? -1 : first > second ? 1 : 0;
}


/*!	Returns the number of query hits, and in \a _duplicates the number of
	hits for an inode that was already returned.
*/
static int
query_count(int* _duplicates)
{
	DIR* query = fs_open_query(sDevice, kQuery, 0);
	if (query == NULL) {
		perror("fs_open_query");
		exit(1);
	}

	ino_t nodes[kMaxHits];
	int count = 0;
	struct dirent* entry;
	while ((entry = fs_read_query(query)) != NULL) {
		if (count < kMaxHits)
			nodes[count] = entry->d_ino;
		count++;
	}
	fs_close_query(query);

	int stored = count < kMaxHits ? count : kMaxHits;
	qsort(nodes, stored, sizeof(ino_t), compare_ino);
	*_duplicates = 0;
	for (int i = 1; i < stored; i++) {
		if (nodes[i] == nodes[i - 1])
			(*_duplicates)++;
	}
	return count;
}


/*!	Returns the number of "Could not find value" lines for the test index
	in the syslog, or -1 if it cannot be read.
*/
static int
syslog_errors()
{
	FILE* file = fopen("/var/log/syslog", "r");
	if (file == NULL)
		return -1;

	char pattern[128];
	snprintf(pattern, sizeof(pattern), "Could not find value in index \"%s\"",
		kIndex);
	char line[1024];
	int count = 0;
	while (fgets(line, sizeof(line), file) != NULL) {
		if (strstr(line, pattern) != NULL)
			count++;
	}
	fclose(file);
	return count;
}


static void
write_file(const char* name, uint32 type, const void* value, size_t size)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s", sDirectory, name);
	int fd = open(path, O_WRONLY | O_CREAT, 0644);
	if (fd < 0 || fs_write_attr(fd, kIndex, type, 0, value, size)
			!= (ssize_t)size) {
		perror(path);
		exit(1);
	}
	close(fd);
}


static void
write_files(int round)
{
	for (int i = 0; i < kFileCount; i++) {
		char name[32];
		snprintf(name, sizeof(name), "f%d-%04d", round, i);
		int64 value = 1000 + i;
		write_file(name, B_INT64_TYPE, &value, sizeof(value));
	}
}


static void
remove_files()
{
	DIR* dir = opendir(sDirectory);
	if (dir == NULL) {
		perror(sDirectory);
		exit(1);
	}
	struct dirent* entry;
	while ((entry = readdir(dir)) != NULL) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/%s", sDirectory, entry->d_name);
		if (unlink(path) != 0)
			perror(path);
	}
	closedir(dir);
}


int
main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <scratch directory on a BFS volume>\n",
			argv[0]);
		return 1;
	}
	sDirectory = argv[1];
	if (mkdir(sDirectory, 0755) != 0) {
		perror(sDirectory);
		return 1;
	}
	sDevice = dev_for_path(sDirectory);
	fs_remove_index(sDevice, kIndex);
		// a leftover from an interrupted run
	if (fs_create_index(sDevice, kIndex, B_INT64_TYPE, 0) != 0) {
		perror("fs_create_index");
		return 1;
	}
	int syslogBefore = syslog_errors();

	write_files(0);
	int32 shortValue = 1234;
	write_file("short", B_INT32_TYPE, &shortValue, sizeof(shortValue));
	int64 longValue = 1234;
	write_file("retyped", B_INT64_TYPE, &longValue, sizeof(longValue));
	write_file("retyped", B_INT32_TYPE, &shortValue, sizeof(shortValue));

	int duplicates;
	int count = query_count(&duplicates);
	printf("indexed: %d hits, %d duplicates (expected %d, 0)\n", count,
		duplicates, kFileCount);
	check(count == kFileCount && duplicates == 0,
		"only the 8-byte values are indexed");

	remove_files();
	sync();
	count = query_count(&duplicates);
	printf("after deleting all files: %d hits (expected 0)\n", count);
	check(count == 0, "no hits after deleting all files");

	// The new files reuse the freed inodes: a stale entry finds one again
	write_files(1);
	count = query_count(&duplicates);
	printf("after reusing the inodes: %d hits, %d duplicates "
		"(expected %d, 0)\n", count, duplicates, kFileCount);
	check(count == kFileCount && duplicates == 0,
		"reused inodes are returned once");

	remove_files();
	sync();
	sleep(1);
		// let syslog_daemon write out the kernel log
	int syslogAfter = syslog_errors();
	if (syslogBefore < 0) {
		printf("skipped: /var/log/syslog is not readable\n");
	} else {
		printf("syslog: %d new \"Could not find value\" lines (expected 0)\n",
			syslogAfter - syslogBefore);
		check(syslogAfter == syslogBefore, "every removal found its key");
	}

	if (fs_remove_index(sDevice, kIndex) != 0)
		perror("fs_remove_index");
	if (rmdir(sDirectory) != 0)
		perror(sDirectory);

	printf("%s\n", sFailed ? "FAIL" : "PASS");
	return sFailed ? 1 : 0;
}
