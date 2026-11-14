/*
 * Copyright 2026, DeBeOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Ported from Haiku-Graviton PR #643, issue #633.
 *
 * SPDX-License-Identifier: MIT
 */

// An AF_UNIX stream read must never return the descriptors of more than one
// sendmsg(), as on Linux; Ladybird's LibIPC TransportSocket relies on it.


#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>


#define REPORT_ERROR(msg, ...) \
	fprintf(stderr, "%s:%d: " msg "\n", __FILE__, __LINE__, ##__VA_ARGS__)


static const int kSends = 5;
static const int kRoomForFDs = 64;


static int
send_byte_and_fds(int sock, char byte, const int* fds, int count)
{
	char control[CMSG_SPACE(sizeof(int) * kRoomForFDs)];
	memset(control, 0, sizeof(control));

	struct iovec iov = { &byte, 1 };
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = CMSG_SPACE(sizeof(int) * count);

	struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int) * count);
	memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * count);

	if (sendmsg(sock, &msg, 0) != 1) {
		REPORT_ERROR("sendmsg() failed: %s", strerror(errno));
		return 1;
	}
	return 0;
}


static int
send_byte_and_fd(int sock, char byte, int fd)
{
	return send_byte_and_fds(sock, byte, &fd, 1);
}


// Receives with a control buffer of controlLength bytes (none if 0) and
// requires exactly one byte equal to expectedByte, MSG_CTRUNC set, and exactly
// expectedFDs descriptors: a short control buffer truncates, it does not fail
// the read and lose its data.
static int
receive_truncated(int sock, char expectedByte, size_t controlLength,
	int expectedFDs)
{
	char buffer[16];
	char control[CMSG_SPACE(sizeof(int) * kRoomForFDs)];
	memset(control, 0, sizeof(control));

	struct iovec iov = { buffer, sizeof(buffer) };
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = controlLength > 0 ? control : NULL;
	msg.msg_controllen = controlLength;

	ssize_t bytes = recvmsg(sock, &msg, 0);
	if (bytes != 1 || buffer[0] != expectedByte) {
		REPORT_ERROR("short-control recvmsg() returned %zd bytes (%s), "
			"expected 1 byte %#x", bytes, bytes < 0 ? strerror(errno) : "-",
			expectedByte);
		return 1;
	}
	if ((msg.msg_flags & MSG_CTRUNC) == 0) {
		REPORT_ERROR("short-control recvmsg() did not set MSG_CTRUNC "
			"(msg_flags %#x)", msg.msg_flags);
		return 1;
	}

	int fdCount = 0;
	if (msg.msg_control != NULL) {
		for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
				cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
				continue;
			int count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			int* fds = (int*)CMSG_DATA(cmsg);
			for (int i = 0; i < count; i++)
				close(fds[i]);
			fdCount += count;
		}
	}
	if (fdCount != expectedFDs) {
		REPORT_ERROR("short-control recvmsg() returned %d descriptors, "
			"expected %d", fdCount, expectedFDs);
		return 1;
	}
	return 0;
}


// Receives with room for kRoomForFDs descriptors and requires exactly one
// byte, equal to expectedByte, and exactly one descriptor referring to the
// same file as expectedFD.
static int
receive_one(int sock, char expectedByte, int expectedFD, int flags)
{
	char buffer[4096];
	char control[CMSG_SPACE(sizeof(int) * kRoomForFDs)];
	memset(control, 0, sizeof(control));

	struct iovec iov = { buffer, sizeof(buffer) };
	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);

	ssize_t bytes = recvmsg(sock, &msg, flags);
	if (bytes != 1) {
		REPORT_ERROR("recvmsg(%#x) returned %zd bytes, expected 1 (%s)",
			flags, bytes, bytes < 0 ? strerror(errno) : "spans sends");
		return 1;
	}
	if (buffer[0] != expectedByte) {
		REPORT_ERROR("recvmsg(%#x) returned byte %#x, expected %#x", flags,
			buffer[0], expectedByte);
		return 1;
	}

	int fdCount = 0;
	int receivedFD = -1;
	for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
			cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
			continue;
		int count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		int* fds = (int*)CMSG_DATA(cmsg);
		for (int i = 0; i < count; i++) {
			if (receivedFD < 0)
				receivedFD = fds[i];
			else
				close(fds[i]);
		}
		fdCount += count;
	}
	if (fdCount != 1) {
		REPORT_ERROR("recvmsg(%#x) returned %d descriptors, expected 1",
			flags, fdCount);
		if (receivedFD >= 0)
			close(receivedFD);
		return 1;
	}

	struct stat expected, received;
	int status = fstat(expectedFD, &expected) == 0
		&& fstat(receivedFD, &received) == 0
		&& expected.st_dev == received.st_dev
		&& expected.st_ino == received.st_ino ? 0 : 1;
	if (status != 0)
		REPORT_ERROR("recvmsg(%#x) returned the wrong descriptor", flags);
	close(receivedFD);
	return status;
}


int
main()
{
	int pair[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
		REPORT_ERROR("socketpair() failed: %s", strerror(errno));
		return 1;
	}

	// Distinct files per send, so a descriptor from the wrong send is caught.
	int pipes[kSends][2];
	for (int i = 0; i < kSends; i++) {
		if (pipe(pipes[i]) != 0) {
			REPORT_ERROR("pipe() failed: %s", strerror(errno));
			return 1;
		}
	}

	// Queue every send before the first read, so a read could span them all.
	for (int i = 0; i < kSends; i++) {
		if (send_byte_and_fd(pair[0], 'a' + i, pipes[i][0]) != 0)
			return 1;
	}

	// MSG_PEEK must stop at the same boundary and not consume anything.
	if (receive_one(pair[1], 'a', pipes[0][0], MSG_PEEK) != 0)
		return 1;

	for (int i = 0; i < kSends; i++) {
		if (receive_one(pair[1], 'a' + i, pipes[i][0], 0) != 0)
			return 1;
	}

	// One send carrying four descriptors, read with room for only two: the
	// byte arrives, two descriptors are installed, MSG_CTRUNC is set. Then
	// the same with no control buffer at all. A following read must still
	// work -- the old EINVAL left the data queued and wedged the stream.
	int four[4] = { pipes[0][0], pipes[1][0], pipes[2][0], pipes[3][0] };
	if (send_byte_and_fds(pair[0], 'x', four, 4) != 0
		|| send_byte_and_fd(pair[0], 'y', pipes[4][0]) != 0
		|| send_byte_and_fd(pair[0], 'z', pipes[4][0]) != 0)
		return 1;
	if (receive_truncated(pair[1], 'x', CMSG_SPACE(sizeof(int) * 2), 2) != 0)
		return 1;
	if (receive_truncated(pair[1], 'y', 0, 0) != 0)
		return 1;
	if (receive_one(pair[1], 'z', pipes[4][0], 0) != 0)
		return 1;

	printf("unix_stream_rights_test: all tests passed\n");
	return 0;
}
