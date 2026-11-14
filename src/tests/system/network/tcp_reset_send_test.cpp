/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright 2026, DeBeOS contributors.
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 */

/*!	send() after the peer reset the connection must fail the same way no
	matter whether the sender was blocked in send() when the RST arrived or
	called send() after it had been processed: the first send() reports
	ECONNRESET without a signal, every later one EPIPE plus SIGPIPE, as on
	Linux. Before the fix the blocked sender got EPIPE+SIGPIPE, while the
	unblocked one's first send() succeeded (no linger-0 RST was sent) or got
	ENOTCONN. After an orderly close send() is EPIPE+SIGPIPE too (it was
	ENOTCONN); after a refused connect() it stays ENOTCONN (Haiku-specific).
*/


#include <atomic>

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


static volatile sig_atomic_t sPipeSignals = 0;
static int sFailures = 0;


static void
pipe_handler(int)
{
	sPipeSignals++;
}


static void
check(const char* arm, const char* what, int got, int expected)
{
	if (got == expected) {
		printf("ok   %s: %s = %s\n", arm, what, strerror(expected));
		return;
	}
	printf("FAIL %s: %s = %d (%s), expected %d (%s)\n", arm, what, got,
		strerror(got), expected, strerror(expected));
	sFailures++;
}


static void
check_count(const char* arm, const char* what, int got, int expected)
{
	if (got == expected) {
		printf("ok   %s: %s = %d\n", arm, what, expected);
		return;
	}
	printf("FAIL %s: %s = %d, expected %d\n", arm, what, got, expected);
	sFailures++;
}


static void
connected_pair(int& client, int& server)
{
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t addrLen = sizeof(addr);
	if (listener < 0 || bind(listener, (sockaddr*)&addr, addrLen) != 0
		|| listen(listener, 1) != 0
		|| getsockname(listener, (sockaddr*)&addr, &addrLen) != 0) {
		fprintf(stderr, "listener setup failed: %s\n", strerror(errno));
		exit(2);
	}

	client = socket(AF_INET, SOCK_STREAM, 0);
	int sendBuffer = 4096;
	setsockopt(client, SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof(sendBuffer));
	if (client < 0 || connect(client, (sockaddr*)&addr, addrLen) != 0) {
		fprintf(stderr, "connect failed: %s\n", strerror(errno));
		exit(2);
	}
	server = accept(listener, NULL, NULL);
	if (server < 0) {
		fprintf(stderr, "accept failed: %s\n", strerror(errno));
		exit(2);
	}
	close(listener);
}


static void
reset_by_peer(int server)
{
	// SO_LINGER with a zero timeout turns close() into an abortive RST.
	linger abortive = { 1, 0 };
	setsockopt(server, SOL_SOCKET, SO_LINGER, &abortive, sizeof(abortive));
	close(server);
}


static int
send_errno(int fd, const char* buffer, size_t size)
{
	errno = 0;
	if (send(fd, buffer, size, 0) >= 0)
		return 0;
	return errno;
}


/*!	The first and second send() after the reset, plus the SIGPIPE count each
	one raised; both arms MUST produce the same four values.
*/
static void
check_after_reset(const char* arm, int firstErrno, int firstSignals,
	int client)
{
	char byte = 'x';
	check(arm, "first send() after reset", firstErrno, ECONNRESET);
	check_count(arm, "SIGPIPE from first send()", firstSignals, 0);

	int before = sPipeSignals;
	int secondErrno = send_errno(client, &byte, 1);
	check(arm, "second send() after reset", secondErrno, EPIPE);
	check_count(arm, "SIGPIPE from second send()", sPipeSignals - before, 1);
}


struct BlockedSender {
	int					fd = -1;
	int					error = 0;
	int					signals = 0;
	std::atomic<bool>	started{false};
};


static void*
blocked_sender(void* data)
{
	BlockedSender* sender = (BlockedSender*)data;
	static char chunk[65536];

	// The peer never reads, so this fills its receive window and our send
	// queue, and then blocks until the reset wakes it.
	int before = sPipeSignals;
	sender->started = true;
	while (true) {
		errno = 0;
		if (send(sender->fd, chunk, sizeof(chunk), 0) < 0)
			break;
	}
	sender->error = errno;
	sender->signals = sPipeSignals - before;
	return NULL;
}


static void
arm_blocked()
{
	int client, server;
	connected_pair(client, server);

	BlockedSender sender;
	sender.fd = client;
	pthread_t thread;
	int error = pthread_create(&thread, NULL, blocked_sender, &sender);
	if (error != 0) {
		fprintf(stderr, "pthread_create failed: %s\n", strerror(error));
		exit(2);
	}
	while (!sender.started)
		usleep(1000);
	usleep(500000);
		// long enough for the window to close and the sender to sleep

	reset_by_peer(server);
	pthread_join(thread, NULL);

	check_after_reset("blocked", sender.error, sender.signals, client);
	close(client);
}


static void
arm_unblocked()
{
	int client, server;
	connected_pair(client, server);

	reset_by_peer(server);
	usleep(200000);
		// let the RST be processed before the first send()

	char byte = 'x';
	int before = sPipeSignals;
	int error = send_errno(client, &byte, 1);
	check_after_reset("unblocked", error, sPipeSignals - before, client);
	close(client);
}


/*!	getsockopt(SO_ERROR) reports the reset and clears it; the send() after
	that still fails with EPIPE and SIGPIPE, as on Linux, and not with
	ENOTCONN as for a socket that was never connected.
*/
static void
arm_so_error()
{
	int client, server;
	connected_pair(client, server);

	reset_by_peer(server);
	usleep(200000);

	int error = 0;
	socklen_t length = sizeof(error);
	getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &length);
	check("so_error", "SO_ERROR after reset", error, ECONNRESET);

	char byte = 'x';
	int before = sPipeSignals;
	int sendErrno = send_errno(client, &byte, 1);
	check("so_error", "send() after SO_ERROR was read", sendErrno, EPIPE);
	check_count("so_error", "SIGPIPE from that send()", sPipeSignals - before,
		1);
	close(client);
}


/*!	Two more send()s, each of which must fail with \a expected and raise
	\a signals SIGPIPEs.
*/
static void
check_two_sends(const char* arm, int fd, int expected, int signals)
{
	char byte = 'x';
	for (int i = 0; i < 2; i++) {
		int before = sPipeSignals;
		int error = send_errno(fd, &byte, 1);
		check(arm, i == 0 ? "first send()" : "second send()", error, expected);
		check_count(arm, "SIGPIPE from that send()", sPipeSignals - before,
			signals);
	}
}


/*!	The peer closes, we shut down our side, and once our FIN is acknowledged
	the connection is CLOSED without an error: send() is EPIPE plus SIGPIPE.
*/
static void
arm_orderly_close()
{
	int client, server;
	connected_pair(client, server);

	close(server);
	usleep(200000);
		// let the peer's FIN arrive
	shutdown(client, SHUT_WR);
	usleep(200000);
		// let our FIN be acknowledged, which closes the connection

	check_two_sends("orderly", client, EPIPE, 1);
	close(client);
}


/*!	A blocking connect() to a port without a listener is refused. The socket
	never got connected, so send() is ENOTCONN without a signal, and does not
	report the refusal again. Linux gives EPIPE plus SIGPIPE instead, so the
	send() checks are Haiku-specific.
*/
static void
arm_refused_connect()
{
	// a loopback port nobody listens on
	int unused = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t addrLen = sizeof(addr);
	if (unused < 0 || bind(unused, (sockaddr*)&addr, addrLen) != 0
		|| getsockname(unused, (sockaddr*)&addr, &addrLen) != 0) {
		fprintf(stderr, "port setup failed: %s\n", strerror(errno));
		exit(2);
	}
	close(unused);

	int client = socket(AF_INET, SOCK_STREAM, 0);
	errno = 0;
	int error = connect(client, (sockaddr*)&addr, addrLen) == 0 ? 0 : errno;
	check("refused", "connect()", error, ECONNREFUSED);
#ifdef __linux__
	printf("skip refused: the send() checks are Haiku-specific\n");
#else
	check_two_sends("refused", client, ENOTCONN, 0);
#endif
	close(client);
}


int
main()
{
	signal(SIGPIPE, pipe_handler);

	arm_blocked();
	arm_unblocked();
	arm_so_error();
	arm_orderly_close();
	arm_refused_connect();

	if (sFailures != 0) {
		printf("%d check(s) failed\n", sFailures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
