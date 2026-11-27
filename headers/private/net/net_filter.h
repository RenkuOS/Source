/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef NET_FILTER_H
#define NET_FILTER_H

/*!	Connection filter: lets one userland server (an application firewall)
	allow or deny each new connection, per team, before it is made.

	The server opens any AF_INET socket and registers a port with
	ioctl(fd, NET_FILTER_REGISTER, &port, sizeof(port)). For each new outgoing
	connection, unconnected datagram destination and accepted incoming
	connection of another team, the stack writes a net_filter_request with
	the code NET_FILTER_REQUEST_CODE to that port and holds the calling thread
	until the server answers with NET_FILTER_VERDICT, or until
	NET_FILTER_TIMEOUT passes (then the connection is denied). Non-blocking
	sockets are held too: connect(), sendto() and accept() may wait up to
	that long for a connection the server doesn't know yet.

	Loopback and non-IP sockets, raw sockets, the kernel and the server's own
	team are never asked. When the port is deleted (the server quit or
	crashed) the filter unregisters itself and every held connection is
	allowed: without a server, nothing is filtered.

	So that the server can name addresses (host and domain rules,
	blocklists), the stack also copies DNS traffic to the port, without
	holding it: each allowed datagram sent to port 53 and each datagram
	received from port 53, as a net_filter_dns message with the code
	NET_FILTER_DNS_CODE. A full port drops the copy, never the datagram.
*/

#include <OS.h>
#include <sys/socket.h>


#define NET_FILTER_TIMEOUT			60000000LL	// microseconds

// ioctl() ops, on any AF_INET/AF_INET6 socket; root only
enum {
	NET_FILTER_REGISTER		= 'nfrg',	// port_id
	NET_FILTER_UNREGISTER	= 'nfur',	// no argument
	NET_FILTER_VERDICT		= 'nfvd',	// net_filter_verdict
	NET_FILTER_FLUSH		= 'nffl',	// no argument; forget cached verdicts
	NET_FILTER_ABORT		= 'nfab'	// net_filter_abort; in: matches,
										// out: number of sockets cut
};

// message codes of what is written to the server's port
#define NET_FILTER_REQUEST_CODE		'nfrq'
#define NET_FILTER_DNS_CODE			'nfdn'

// the most DNS payload copied (an EDNS answer); a query is cut to its header
#define NET_FILTER_DNS_MAX			4096
#define NET_FILTER_DNS_QUERY_MAX	512

enum {
	NET_FILTER_OUTGOING	= 1,
	NET_FILTER_INCOMING	= 2
};

enum {
	NET_FILTER_ALLOW	= 1,
	NET_FILTER_DENY		= 2
};

typedef struct net_filter_request {
	uint32					id;
	team_id					team;
	uint8					direction;	// NET_FILTER_OUTGOING/INCOMING
	uint8					family;		// AF_INET, AF_INET6
	uint8					protocol;	// IPPROTO_TCP, IPPROTO_UDP
	uint8					_reserved;
	struct sockaddr_storage	local;
	struct sockaddr_storage	remote;
} net_filter_request;

typedef struct net_filter_dns {
	team_id					team;		// the team that sent or received it
	uint8					direction;	// NET_FILTER_OUTGOING: a query
	uint8					family;
	uint16					length;		// of data
	struct sockaddr_storage	local;
	struct sockaddr_storage	remote;		// the DNS server
	uint8					data[0];	// the DNS message (UDP payload)
} net_filter_dns;

typedef struct net_filter_verdict {
	uint32					id;
	int32					verdict;	// NET_FILTER_ALLOW/DENY
} net_filter_verdict;

typedef struct net_filter_abort {
	team_id					team;		// -1: any team
	struct sockaddr_storage	remote;		// ss_len 0: any; port 0: any port
	int32					count;		// out: sockets cut
} net_filter_abort;


#endif	// NET_FILTER_H
