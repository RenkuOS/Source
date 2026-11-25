/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */

//!	The connection filter: see headers/private/net/net_filter.h.


#include "net_filter_private.h"

#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <KernelExport.h>

#include <condition_variable.h>
#include <kernel.h>
#include <lock.h>
#include <team.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>

#include <net_filter.h>
#include <net_socket.h>

#include "stack_private.h"
#include "utility.h"


//#define TRACE_FILTER
#ifdef TRACE_FILTER
#	define TRACE(x...) dprintf(STACK_DEBUG_PREFIX "filter: " x)
#else
#	define TRACE(x...) ;
#endif


struct pending_request : DoublyLinkedListLinkImpl<pending_request> {
	uint32				id;
	int32				verdict;
	ConditionVariable	condition;
};

typedef DoublyLinkedList<pending_request> RequestList;

static mutex sFilterLock = MUTEX_INITIALIZER("net filter");
static int32 sFilterPort = -1;
	// read without the lock on the fast path, so always through atomic_get()
static team_id sFilterTeam = -1;
static int32 sNextID = 1;
static int32 sGeneration = 1;
static RequestList sPending;


static bool
is_loopback(const sockaddr* address)
{
	if (address->sa_family == AF_INET) {
		const in_addr_t ip = ntohl(((const sockaddr_in*)address)->sin_addr.s_addr);
		return (ip >> 24) == 127;
	}
	if (address->sa_family == AF_INET6) {
		const in6_addr& ip = ((const sockaddr_in6*)address)->sin6_addr;
		// ::1 (IN6_IS_ADDR_LOOPBACK() needs libroot's in6addr_loopback)
		static const uint8 kLoopback[16] = { 0, 0, 0, 0, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 1 };
		if (memcmp(ip.s6_addr, kLoopback, sizeof(kLoopback)) == 0)
			return true;
		// an IPv4-mapped loopback address (::ffff:127.x.x.x)
		return IN6_IS_ADDR_V4MAPPED(&ip) && ip.s6_addr[12] == 127;
	}
	return false;
}


static bool
is_filtered_socket(net_socket* socket, const sockaddr* remote)
{
	if (socket->family != AF_INET && socket->family != AF_INET6)
		return false;
	if (socket->type != SOCK_STREAM && socket->type != SOCK_DGRAM)
		return false;
	if (remote == NULL || remote->sa_family != socket->family)
		return false;
	return !is_loopback(remote);
}


/*!	Forgets the server. The caller holds sFilterLock. Every held request sees
	that its port is gone and is allowed.
*/
static void
unregister_locked(port_id port)
{
	if (sFilterPort != port)
		return;

	TRACE("server port %" B_PRId32 " gone, filter off\n", port);
	atomic_set(&sFilterPort, -1);
	sFilterTeam = -1;
	atomic_add(&sGeneration, 1);

	RequestList::Iterator iterator = sPending.GetIterator();
	while (pending_request* request = iterator.Next())
		request->condition.NotifyAll();
}


static status_t
copy_in(void* to, const void* from, size_t size, size_t length)
{
	if (from == NULL || length < size)
		return B_BAD_VALUE;
	if (is_syscall()) {
		if (!IS_USER_ADDRESS(from) || user_memcpy(to, from, size) != B_OK)
			return B_BAD_ADDRESS;
	} else
		memcpy(to, from, size);
	return B_OK;
}


static status_t
copy_out(void* to, const void* from, size_t size)
{
	if (is_syscall()) {
		if (!IS_USER_ADDRESS(to) || user_memcpy(to, from, size) != B_OK)
			return B_BAD_ADDRESS;
	} else
		memcpy(to, from, size);
	return B_OK;
}


//	#pragma mark - interface for net_socket.cpp


bool
net_filter_is_op(uint32 op)
{
	return op == NET_FILTER_REGISTER || op == NET_FILTER_UNREGISTER
		|| op == NET_FILTER_VERDICT || op == NET_FILTER_FLUSH
		|| op == NET_FILTER_ABORT;
}


uint32
net_filter_generation()
{
	return (uint32)atomic_get(&sGeneration);
}


bool
net_filter_address_matches(const sockaddr* pattern, const sockaddr* address)
{
	if (pattern->sa_len == 0)
		return true;
	if (pattern->sa_family != address->sa_family)
		return false;

	if (pattern->sa_family == AF_INET) {
		const sockaddr_in* a = (const sockaddr_in*)pattern;
		const sockaddr_in* b = (const sockaddr_in*)address;
		return a->sin_addr.s_addr == b->sin_addr.s_addr
			&& (a->sin_port == 0 || a->sin_port == b->sin_port);
	}
	if (pattern->sa_family == AF_INET6) {
		const sockaddr_in6* a = (const sockaddr_in6*)pattern;
		const sockaddr_in6* b = (const sockaddr_in6*)address;
		return memcmp(&a->sin6_addr, &b->sin6_addr, sizeof(in6_addr)) == 0
			&& (a->sin6_port == 0 || a->sin6_port == b->sin6_port);
	}
	return false;
}


/*!	Whether a datagram to or from \a remote is DNS traffic to copy to the
	server: a UDP socket of an IP family, port 53, and a server registered.
	Cheap; call it before gathering the data.
*/
bool
net_filter_is_dns(net_socket* socket, const sockaddr* remote)
{
	if (atomic_get(&sFilterPort) < 0 || socket->type != SOCK_DGRAM
		|| remote == NULL || remote->sa_family != socket->family)
		return false;
	if (remote->sa_family == AF_INET)
		return ((const sockaddr_in*)remote)->sin_port == htons(53);
	if (remote->sa_family == AF_INET6)
		return ((const sockaddr_in6*)remote)->sin6_port == htons(53);
	return false;
}


/*!	Copies a DNS message to the server's port without waiting: when the port
	is full or gone, the copy is dropped. \a userData: \a data is the
	caller's userland buffer.
*/
void
net_filter_notify_dns(net_socket* socket, uint8 direction,
	const sockaddr* remote, const void* data, size_t length, bool userData)
{
	const port_id port = atomic_get(&sFilterPort);
	if (port < 0 || data == NULL || length == 0)
		return;
	length = min_c(length, direction == NET_FILTER_OUTGOING
		? NET_FILTER_DNS_QUERY_MAX : NET_FILTER_DNS_MAX);

	const size_t size = sizeof(net_filter_dns) + length;
	net_filter_dns* message = (net_filter_dns*)malloc(size);
	if (message == NULL)
		return;
	memset(message, 0, sizeof(net_filter_dns));
	message->team = team_get_current_team_id();
	message->direction = direction;
	message->family = socket->family;
	message->length = (uint16)length;
	memcpy(&message->local, &socket->address, min_c(socket->address.ss_len,
		sizeof(sockaddr_storage)));
	memcpy(&message->remote, remote, min_c(remote->sa_len,
		sizeof(sockaddr_storage)));
	if (userData) {
		if (!IS_USER_ADDRESS(data)
			|| user_memcpy(message->data, data, length) != B_OK) {
			free(message);
			return;
		}
	} else
		memcpy(message->data, data, length);

	if (write_port_etc(port, NET_FILTER_DNS_CODE, message, size,
			B_RELATIVE_TIMEOUT, 0) != B_OK) {
		TRACE("DNS copy dropped\n");
	}
	free(message);
}


/*!	Asks the server about a new connection of the current team and waits for
	its verdict. Returns B_OK to allow it, B_NOT_ALLOWED (EPERM) to deny it.
	Must be called without any locks held.
*/
status_t
net_filter_check(net_socket* socket, uint8 direction, const sockaddr* remote,
	const sockaddr* local)
{
	if (atomic_get(&sFilterPort) < 0 || !is_filtered_socket(socket, remote))
		return B_OK;

	const team_id team = team_get_current_team_id();
	if (team == B_SYSTEM_TEAM)
		return B_OK;

	net_filter_request message;
	memset(&message, 0, sizeof(message));
	message.team = team;
	message.direction = direction;
	message.family = socket->family;
	message.protocol = socket->type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
	memcpy(&message.remote, remote, min_c(remote->sa_len,
		sizeof(sockaddr_storage)));
	if (local != NULL) {
		memcpy(&message.local, local, min_c(local->sa_len,
			sizeof(sockaddr_storage)));
	}

	pending_request request;
	request.id = message.id = (uint32)atomic_add(&sNextID, 1);
	request.verdict = 0;
	request.condition.Init(&request, "net filter verdict");

	MutexLocker locker(sFilterLock);
	const port_id port = sFilterPort;
	if (port < 0 || team == sFilterTeam)
		return B_OK;
	sPending.Add(&request);
	locker.Unlock();

	TRACE("ask #%" B_PRIu32 ": team %" B_PRId32 ", direction %u\n", request.id,
		team, direction);

	// A full port means the server is alive but behind: deny rather than let
	// a burst of connections through unasked.
	status_t status = write_port_etc(port, NET_FILTER_REQUEST_CODE, &message,
		sizeof(message), B_RELATIVE_TIMEOUT, 1000000);

	locker.Lock();
	if (status == B_BAD_PORT_ID)
		unregister_locked(port);
	else if (status != B_OK)
		request.verdict = NET_FILTER_DENY;

	const bigtime_t deadline = system_time() + NET_FILTER_TIMEOUT;
	while (request.verdict == 0) {
		if (sFilterPort != port) {
			// the server quit or was replaced: nothing filters this one
			request.verdict = NET_FILTER_ALLOW;
			break;
		}

		ConditionVariableEntry entry;
		request.condition.Add(&entry);
		locker.Unlock();
		status = entry.Wait(B_RELATIVE_TIMEOUT | B_CAN_INTERRUPT, 1000000);
		locker.Lock();

		if (request.verdict != 0)
			break;
		if (status == B_INTERRUPTED) {
			sPending.Remove(&request);
			return B_INTERRUPTED;
		}

		// A server that died leaves its port deleted; notice it here
		// rather than hold the connection for the whole timeout.
		port_info info;
		if (sFilterPort == port && get_port_info(port, &info) != B_OK)
			unregister_locked(port);
		else if (system_time() >= deadline)
			request.verdict = NET_FILTER_DENY;
	}
	sPending.Remove(&request);

	TRACE("verdict #%" B_PRIu32 ": %s\n", request.id,
		request.verdict == NET_FILTER_ALLOW ? "allow" : "deny");
	return request.verdict == NET_FILTER_ALLOW ? B_OK : B_NOT_ALLOWED;
}


/*!	Handles the NET_FILTER_* ioctls except NET_FILTER_ABORT, which needs the
	socket list (net_socket.cpp).
*/
status_t
net_filter_control(uint32 op, void* data, size_t length)
{
	if (geteuid() != 0)
		return B_NOT_ALLOWED;

	switch (op) {
		case NET_FILTER_REGISTER:
		{
			port_id port;
			status_t status = copy_in(&port, data, sizeof(port), length);
			if (status != B_OK)
				return status;

			port_info info;
			if (get_port_info(port, &info) != B_OK)
				return B_BAD_PORT_ID;

			MutexLocker _(sFilterLock);
			port_info current;
			if (sFilterPort >= 0 && sFilterPort != port
				&& sFilterTeam != team_get_current_team_id()
				&& get_port_info(sFilterPort, &current) == B_OK) {
				// another live server is registered
				return B_BUSY;
			}
			if (sFilterPort >= 0)
				unregister_locked(sFilterPort);

			sFilterTeam = team_get_current_team_id();
			atomic_add(&sGeneration, 1);
			atomic_set(&sFilterPort, port);
			TRACE("server team %" B_PRId32 " registered port %" B_PRId32 "\n",
				sFilterTeam, port);
			return B_OK;
		}

		case NET_FILTER_UNREGISTER:
		{
			MutexLocker _(sFilterLock);
			if (sFilterTeam != team_get_current_team_id())
				return B_NOT_ALLOWED;
			unregister_locked(sFilterPort);
			return B_OK;
		}

		case NET_FILTER_VERDICT:
		{
			net_filter_verdict verdict;
			status_t status = copy_in(&verdict, data, sizeof(verdict), length);
			if (status != B_OK)
				return status;
			if (verdict.verdict != NET_FILTER_ALLOW
				&& verdict.verdict != NET_FILTER_DENY)
				return B_BAD_VALUE;

			MutexLocker _(sFilterLock);
			if (sFilterTeam != team_get_current_team_id())
				return B_NOT_ALLOWED;

			RequestList::Iterator iterator = sPending.GetIterator();
			while (pending_request* request = iterator.Next()) {
				if (request->id == verdict.id) {
					request->verdict = verdict.verdict;
					request->condition.NotifyAll();
					return B_OK;
				}
			}
			// it timed out or its thread was interrupted meanwhile
			return B_ENTRY_NOT_FOUND;
		}

		case NET_FILTER_FLUSH:
		{
			MutexLocker _(sFilterLock);
			if (sFilterTeam != team_get_current_team_id())
				return B_NOT_ALLOWED;
			atomic_add(&sGeneration, 1);
			return B_OK;
		}
	}

	return B_BAD_VALUE;
}


status_t
net_filter_copy_abort_in(net_filter_abort* abort, void* data, size_t length)
{
	if (geteuid() != 0)
		return B_NOT_ALLOWED;
	{
		MutexLocker _(sFilterLock);
		if (sFilterTeam != team_get_current_team_id())
			return B_NOT_ALLOWED;
	}
	status_t status = copy_in(abort, data, sizeof(*abort), length);
	if (status == B_OK && abort->remote.ss_len > sizeof(sockaddr_storage))
		return B_BAD_VALUE;
	return status;
}


status_t
net_filter_copy_abort_out(void* data, const net_filter_abort* abort)
{
	return copy_out(data, abort, sizeof(*abort));
}
