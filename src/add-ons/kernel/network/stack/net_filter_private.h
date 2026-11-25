/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef NET_FILTER_PRIVATE_H
#define NET_FILTER_PRIVATE_H


#include <net_filter.h>
#include <net_socket.h>


bool		net_filter_is_op(uint32 op);
status_t	net_filter_control(uint32 op, void* data, size_t length);
status_t	net_filter_check(net_socket* socket, uint8 direction,
				const sockaddr* remote, const sockaddr* local);
uint32		net_filter_generation();
bool		net_filter_is_dns(net_socket* socket, const sockaddr* remote);
void		net_filter_notify_dns(net_socket* socket, uint8 direction,
				const sockaddr* remote, const void* data, size_t length,
				bool userData);
bool		net_filter_address_matches(const sockaddr* pattern,
				const sockaddr* address);
status_t	net_filter_copy_abort_in(net_filter_abort* abort, void* data,
				size_t length);
status_t	net_filter_copy_abort_out(void* data, const net_filter_abort* abort);


#endif	// NET_FILTER_PRIVATE_H
