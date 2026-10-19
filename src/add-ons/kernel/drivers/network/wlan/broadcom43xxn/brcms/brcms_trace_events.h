/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	Linux tracepoints, which are not built here.

	The imported sources sprinkle these through the hot paths. Upstream they
	compile to nothing unless tracing is configured in, and that is what
	they do here.
*/
#ifndef BRCMS_TRACE_EVENTS_H
#define BRCMS_TRACE_EVENTS_H

#define trace_brcms_txdesc(...)		do {} while (0)
#define trace_brcms_ampdu_session(...)	do {} while (0)
#define trace_brcms_txstatus(...)	do {} while (0)
#define trace_brcms_dpc(...)		do {} while (0)
#define trace_brcms_macintstatus(...)	do {} while (0)
#define trace_brcms_timer(...)		do {} while (0)
#define trace_brcms_info(...)		do {} while (0)
#define trace_brcms_dbg(...)		do {} while (0)
#define trace_brcms_err(...)		do {} while (0)
#define trace_brcms_crit(...)		do {} while (0)
#define trace_brcms_warn(...)		do {} while (0)

#endif	// BRCMS_TRACE_EVENTS_H
