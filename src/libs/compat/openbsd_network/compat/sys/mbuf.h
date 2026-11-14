/*
 * Copyright 2022, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _OBSD_COMPAT_SYS_MBUF_H_
#define _OBSD_COMPAT_SYS_MBUF_H_

#include <sys/systm.h>

/* FreeBSD KASSERT */
#undef KASSERT
#define KASSERT KASSERT_FREEBSD

#include_next <sys/mbuf.h>

/* back to OpenBSD KASSERT */
#undef KASSERT
#define KASSERT KASSERT_OPENBSD

#include <sys/mutex.h>


#define ph_cookie PH_loc.ptr

#define M_DATABUF(m)	M_START(m)
#define M_READONLY(m)	(!M_WRITABLE(m))

#define MAXMCLBYTES MJUM16BYTES

#define M_IPV4_CSUM_OUT		CSUM_IP
#define M_TCP_CSUM_OUT		CSUM_IP_TCP
#define M_UDP_CSUM_OUT		CSUM_IP_UDP
#define M_IPV4_CSUM_IN_OK	(CSUM_IP_CHECKED | CSUM_IP_VALID)
#define M_TCP_CSUM_IN_OK	(CSUM_DATA_VALID | CSUM_PSEUDO_HDR)
#define M_UDP_CSUM_IN_OK	(CSUM_DATA_VALID | CSUM_PSEUDO_HDR)


static struct mbuf*
MCLGETL(struct mbuf* m, int how, int size)
{
	if (m == NULL)
		return m_get3(size, how, MT_DATA, M_PKTHDR);

	// OpenBSD takes any length and uses the smallest cluster that fits it.
	// m_cljget() only takes the exact cluster sizes. If none is large
	// enough, leave m without a cluster, which is how MCLGETL() fails.
	if (size <= MCLBYTES)
		size = MCLBYTES;
	else if (size <= MJUMPAGESIZE)
		size = MJUMPAGESIZE;
	else if (size <= MJUM9BYTES)
		size = MJUM9BYTES;
	else
		return m;

	m_cljget(m, how, size);
	return m;
}

/*
 * OpenBSD's m_prepend() prepends in place when there is leading space and
 * always adds to m_pkthdr.len, which is what FreeBSD's M_PREPEND() does.
 * FreeBSD's m_prepend() always allocates a new head and leaves the length
 * alone. This is not #defined over m_prepend: M_PREPEND() itself calls
 * m_prepend(), and net80211 uses M_PREPEND(), so the length would be added
 * twice. A driver that wants the OpenBSD meaning maps m_prepend to this.
 */
static struct mbuf*
m_prepend_openbsd(struct mbuf* m, int len, int how)
{
	M_PREPEND(m, len, how);
	return m;
}

/*
 * OpenBSD's m_pullup() returns the chain unchanged when the first mbuf
 * already holds len bytes. FreeBSD's always pulls into a new mbuf, and fails
 * once len is over MHLEN even when the data is contiguous in a cluster.
 */
static struct mbuf*
m_pullup_openbsd(struct mbuf* m, int len)
{
	if (m->m_len >= len)
		return m;
	return m_pullup(m, len);
}
#define m_pullup m_pullup_openbsd

static int
m_dup_pkthdr_openbsd(struct mbuf* to, const struct mbuf* from, int how)
{
	return !m_dup_pkthdr(to, from, how);
}
#define m_dup_pkthdr m_dup_pkthdr_openbsd

static int
m_tag_copy_chain_openbsd(struct mbuf* to, const struct mbuf* from, int how)
{
	return !m_tag_copy_chain(to, from, how);
}
#define m_tag_copy_chain m_tag_copy_chain_openbsd


/* FreeBSD methods not compatible with their OpenBSD counterparts */
#define m_defrag(mbuf, how) __m_defrag_unimplemented()


#include "mbuf-obsd.h"


#endif	/* _OBSD_COMPAT_SYS_MBUF_H_ */
