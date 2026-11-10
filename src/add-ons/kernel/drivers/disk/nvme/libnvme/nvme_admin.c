/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2017, Western Digital Corporation or its affiliates.
 *
 *   Redistribution and use in sourete and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of sourete code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "nvme_internal.h"

/*
 * Allocate a request, set its command and submit it
 * to the controller admin queue.
 */
static int nvme_admin_submit_cmd(struct nvme_ctrlr *ctrlr,
				 struct nvme_cmd *cmd,
				 void *buf, uint32_t len,
				 nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;

	if (buf)
		req = nvme_request_allocate_contig(&ctrlr->adminq, buf, len,
						   cb_fn, cb_arg);
	else
		req = nvme_request_allocate_null(&ctrlr->adminq, cb_fn, cb_arg);
	if (!req)
		return ENOMEM;

	memcpy(&req->cmd, cmd, sizeof(req->cmd));

	return nvme_qpair_submit_request(&ctrlr->adminq, req);
}

/*
 * Poll the controller admin queue waiting for a
 * command completion.
 */
static int nvme_admin_wait_cmd(struct nvme_ctrlr *ctrlr,
			       struct nvme_completion_poll_status *status)
{

	/* Wait for completion and check result */
	while (status->done == false)
		nvme_qpair_poll(&ctrlr->adminq, 0);

	if (nvme_cpl_is_error(&status->cpl)) {
		nvme_notice("Admin command failed\n");
		return ENXIO;
	}

	return 0;
}

/*
 * Execute an admin command.
 */
static int nvme_admin_exec_cmd(struct nvme_ctrlr *ctrlr,
			       struct nvme_cmd *cmd,
			       void *buf, uint32_t len)
{
	struct nvme_completion_poll_status status;
	int ret;

	/* Submit the command */
	status.done = false;
	ret = nvme_admin_submit_cmd(ctrlr, cmd, buf, len,
				    nvme_request_completion_poll_cb,
				    &status);
	if (ret != 0)
		return ret;

	/* Wait for the command completion and check result */
	return nvme_admin_wait_cmd(ctrlr, &status);
}

/*
 * Get a controller information.
 */
int nvme_admin_identify_ctrlr(struct nvme_ctrlr *ctrlr,
			      struct nvme_ctrlr_data *cdata)
{
	struct nvme_cmd cmd;

	/* Setup the command */
	memset(&cmd, 0, sizeof(struct nvme_cmd));
	cmd.opc = NVME_OPC_IDENTIFY;
	cmd.cdw10 = NVME_IDENTIFY_CTRLR;

	/* Execute the command */
	return nvme_admin_exec_cmd(ctrlr, &cmd,
				   cdata, sizeof(struct nvme_ctrlr_data));
}

/*
 * Get a controller feature.
 */
int nvme_admin_get_feature(struct nvme_ctrlr *ctrlr,
			   enum nvme_feat_sel sel,
			   enum nvme_feat feature,
			   uint32_t cdw11,
			   uint32_t *attributes)
{
	struct nvme_completion_poll_status status;
	struct nvme_cmd cmd;
	int ret;

	/* Setup the command */
	memset(&cmd, 0, sizeof(struct nvme_cmd));
	cmd.opc = NVME_OPC_GET_FEATURES;
	cmd.cdw10 = (sel << 8) | feature;
	cmd.cdw11 = cdw11;

	/* Submit the command */
	status.done = false;
	ret = nvme_admin_submit_cmd(ctrlr, &cmd, NULL, 0,
				    nvme_request_completion_poll_cb,
				    &status);
	if (ret == 0) {
		/* Wait for the command completion and check result */
		ret = nvme_admin_wait_cmd(ctrlr, &status);
		if (ret == 0 && attributes)
			*attributes = status.cpl.cdw0;
	}

	return ret;
}

/*
 * Set a feature.
 */
int nvme_admin_set_feature(struct nvme_ctrlr *ctrlr,
			   bool save,
			   enum nvme_feat feature,
			   uint32_t cdw11, uint32_t cdw12,
			   uint32_t cdw13, uint32_t cdw14, uint32_t cdw15,
			   void *buf, uint32_t len,
			   uint32_t *attributes)
{
	struct nvme_completion_poll_status status;
	struct nvme_cmd cmd;
	int ret;

	/* Setup the command */
	memset(&cmd, 0, sizeof(struct nvme_cmd));
	cmd.opc = NVME_OPC_SET_FEATURES;
	cmd.cdw10 = feature;
	if (save)
		cmd.cdw10 |= (1 << 31);
	cmd.cdw11 = cdw11;
	cmd.cdw12 = cdw12;
	cmd.cdw13 = cdw13;
	cmd.cdw14 = cdw14;
	cmd.cdw15 = cdw15;

	/* Submit the command */
	status.done = false;
	ret = nvme_admin_submit_cmd(ctrlr, &cmd, buf, len,
				    nvme_request_completion_poll_cb,
				    &status);
	if (ret == 0) {
		/* Wait for the command completion and check result */
		ret = nvme_admin_wait_cmd(ctrlr, &status);
		if (ret == 0 && attributes)
			*attributes = status.cpl.cdw0;
	}

	return ret;
}

/*
 * Create an I/O queue.
 */
int nvme_admin_create_ioq(struct nvme_ctrlr *ctrlr,
			  struct nvme_qpair *qpair,
			  enum nvme_io_queue_type io_qtype)
{
	struct nvme_cmd cmd;

	/* Setup the command */
	memset(&cmd, 0, sizeof(struct nvme_cmd));
	switch(io_qtype) {
	case NVME_IO_SUBMISSION_QUEUE:
		cmd.opc = NVME_OPC_CREATE_IO_SQ;
		cmd.cdw11 = (qpair->id << 16) | (qpair->qprio << 1) | 0x1;
		cmd.dptr.prp.prp1 = qpair->cmd_bus_addr;
		break;
	case NVME_IO_COMPLETION_QUEUE:
		cmd.opc = NVME_OPC_CREATE_IO_CQ;
#ifdef __HAIKU__ // TODO: Option!
		cmd.cdw11 = 0x1 | 0x2; /* enable interrupts */
#else
		cmd.cdw11 = 0x1;
#endif
		cmd.dptr.prp.prp1 = qpair->cpl_bus_addr;
		break;
	default:
		return EINVAL;
	}

	cmd.cdw10 = ((qpair->entries - 1) << 16) | qpair->id;

	/* Execute the command */
	return nvme_admin_exec_cmd(ctrlr, &cmd, NULL, 0);
}

/*
 * Delete an I/O queue.
 */
int nvme_admin_delete_ioq(struct nvme_ctrlr *ctrlr,
			  struct nvme_qpair *qpair,
			  enum nvme_io_queue_type io_qtype)
{
	struct nvme_cmd cmd;

	/* Setup the command */
	memset(&cmd, 0, sizeof(struct nvme_cmd));
	switch(io_qtype) {
	case NVME_IO_SUBMISSION_QUEUE:
		cmd.opc = NVME_OPC_DELETE_IO_SQ;
		break;
	case NVME_IO_COMPLETION_QUEUE:
		cmd.opc = NVME_OPC_DELETE_IO_CQ;
		break;
	default:
		return EINVAL;
	}
	cmd.cdw10 = qpair->id;

	/* Execute the command */
	return nvme_admin_exec_cmd(ctrlr, &cmd, NULL, 0);
}

/*
 * Get a namespace information.
 */
int nvme_admin_identify_ns(struct nvme_ctrlr *ctrlr,
			   uint16_t nsid,
			   struct nvme_ns_data *nsdata)
{
	struct nvme_cmd cmd;

	/* Setup the command */
	memset(&cmd, 0, sizeof(struct nvme_cmd));
	cmd.opc = NVME_OPC_IDENTIFY;
	cmd.cdw10 = NVME_IDENTIFY_NS;
	cmd.nsid = nsid;

	/* Execute the command */
	return nvme_admin_exec_cmd(ctrlr, &cmd,
				   nsdata, sizeof(struct nvme_ns_data));
}

/*
 * Attach a namespace.
 */
int nvme_admin_attach_ns(struct nvme_ctrlr *ctrlr,
			 uint32_t nsid,
			 struct nvme_ctrlr_list *clist)
{
	struct nvme_cmd cmd;

	/* Setup the command */
	memset(&cmd, 0, sizeof(struct nvme_cmd));
	cmd.opc = NVME_OPC_NS_ATTACHMENT;
	cmd.nsid = nsid;
	cmd.cdw10 = NVME_NS_CTRLR_ATTACH;

	/* Execute the command */
	return nvme_admin_exec_cmd(ctrlr, &cmd,
				   clist, sizeof(struct nvme_ctrlr_list));
}

/*
 * Detach a namespace.
 */
int nvme_admin_detach_ns(struct nvme_ctrlr *ctrlr,
			 uint32_t nsid,
			 struct nvme_ctrlr_list *clist)
{
	struct nvme_cmd cmd;

	/* Setup the command */
	memset(&cmd, 0, sizeof(struct nvme_cmd));
	cmd.opc = NVME_OPC_NS_ATTACHMENT;
	cmd.nsid = nsid;
	cmd.cdw10 = NVME_NS_CTRLR_DETACH;

	/* Execute the command */
	return nvme_admin_exec_cmd(ctrlr, &cmd,
				   clist, sizeof(struct nvme_ctrlr_list));
}

/*
 * Create a namespace.
 */
int nvme_admin_create_ns(struct nvme_ctrlr *ctrlr,
			 struct nvme_ns_data *nsdata,
			 unsigned int *nsid)
{
	struct nvme_completion_poll_status status;
	struct nvme_cmd cmd;
	int ret;

	/* Setup the command */
	memset(&cmd, 0, sizeof(struct nvme_cmd));
	cmd.opc = NVME_OPC_NS_MANAGEMENT;
	cmd.cdw10 = NVME_NS_MANAGEMENT_CREATE;

	/* Submit the command */
	status.done = false;
	ret = nvme_admin_submit_cmd(ctrlr, &cmd,
				    nsdata, sizeof(struct nvme_ns_data),
				    nvme_request_completion_poll_cb,
				    &status);
	if (ret == 0)
		/* Wait for the command completion and check result */
		ret = nvme_admin_wait_cmd(ctrlr, &status);

	if (ret != 0)
		return ret;

	*nsid = status.cpl.cdw0;

	return 0;
}

/*
 * Delete a namespace.
 */
int nvme_admin_delete_ns(struct nvme_ctrlr *ctrlr,
			 unsigned int nsid)
{
	struct nvme_cmd cmd;

	/* Setup the command */
	memset(&cmd, 0, sizeof(struct nvme_cmd));
	cmd.opc = NVME_OPC_NS_MANAGEMENT;
	cmd.cdw10 = NVME_NS_MANAGEMENT_DELETE;
	cmd.nsid = nsid;

	/* Execute the command */
	return nvme_admin_exec_cmd(ctrlr, &cmd, NULL, 0);
}

/*
 * Format media.
 * (entire device or just the specified namespace)
 */
int nvme_admin_format_nvm(struct nvme_ctrlr *ctrlr,
			  unsigned int nsid,
			  struct nvme_format *format)
{
	struct nvme_cmd cmd;

	/* Setup the command */
	memset(&cmd, 0, sizeof(struct nvme_cmd));
	cmd.opc = NVME_OPC_FORMAT_NVM;
	cmd.nsid = nsid;
	memcpy(&cmd.cdw10, format, sizeof(uint32_t));

	/* Execute the command */
	return nvme_admin_exec_cmd(ctrlr, &cmd, NULL, 0);
}

/*
 * Get a log page.
 */
int nvme_admin_get_log_page(struct nvme_ctrlr *ctrlr,
			    uint8_t log_page,
			    uint32_t nsid,
			    void *payload,
			    uint32_t payload_size)
{
	struct nvme_cmd cmd;

	/* Setup the command */
	memset(&cmd, 0, sizeof(struct nvme_cmd));
	cmd.opc = NVME_OPC_GET_LOG_PAGE;
	cmd.nsid = nsid;
	cmd.cdw10 = ((payload_size / sizeof(uint32_t)) - 1) << 16;
	cmd.cdw10 |= log_page;

	/* Execute the command */
	return nvme_admin_exec_cmd(ctrlr, &cmd, payload, payload_size);
}

/*
 * Abort an admin or an I/O command.
 */
int nvme_admin_abort_cmd(struct nvme_ctrlr *ctrlr,
			 uint16_t cid, uint16_t sqid)
{
	struct nvme_cmd cmd;

	/* Setup the command */
	memset(&cmd, 0, sizeof(struct nvme_cmd));
	cmd.opc = NVME_OPC_ABORT;
	cmd.cdw10 = (cid << 16) | sqid;

	/* Execute the command */
	return nvme_admin_exec_cmd(ctrlr, &cmd, NULL, 0);
}

/*
 * Admin pass-through timing. The NVMe specification leaves command timeouts
 * to the host, so these are our own choices.
 */
#define NVME_PASSTHROUGH_POLL_US	100
#define NVME_PASSTHROUGH_ABORT_GRACE_MS	5000

/*
 * Allocate a request whose completion is written to a poll status the
 * caller allocated, and submit it to the admin queue. Errors are never
 * retried, so the caller sees the drive's status as it was.
 */
static int nvme_admin_submit_polled(struct nvme_ctrlr *ctrlr,
				    const struct nvme_cmd *cmd,
				    void *buf, uint32_t len,
				    struct nvme_completion_poll_status *status,
				    struct nvme_request **reqp)
{
	struct nvme_request *req;
	int ret;

	if (buf)
		req = nvme_request_allocate_contig(&ctrlr->adminq, buf, len,
						   nvme_request_completion_poll_cb,
						   status);
	else
		req = nvme_request_allocate_null(&ctrlr->adminq,
						 nvme_request_completion_poll_cb,
						 status);
	if (!req)
		return ENOMEM;

	/*
	 * The data and metadata pointers are ours to set, never the caller's.
	 * With no buffer they stay zero; with one, the PRP builder fills them.
	 */
	memcpy(&req->cmd, cmd, sizeof(req->cmd));
	req->cmd.fuse = 0;
	req->cmd.psdt = 0;
	req->cmd.mptr = 0;
	memset(&req->cmd.dptr, 0, sizeof(req->cmd.dptr));
	req->retries = NVME_MAX_RETRY_COUNT;
	*reqp = req;

	/*
	 * On failure the request has already been freed, or completed
	 * with an error status, so nothing refers to the buffer.
	 */
	ret = nvme_qpair_submit_request(&ctrlr->adminq, req);
	if (ret != 0)
		return ret == ENXIO ? ENXIO : EIO;

	return 0;
}

/*
 * Poll the admin queue until both commands have completed (the second
 * one is optional) or the deadline passes.
 */
static bool nvme_admin_wait_until(struct nvme_ctrlr *ctrlr,
				  struct nvme_completion_poll_status *first,
				  struct nvme_completion_poll_status *second,
				  bigtime_t deadline)
{
	for (;;) {
		nvme_qpair_poll(&ctrlr->adminq, 0);

		if (first->done && (second == NULL || second->done))
			return true;

		if (system_time() >= deadline)
			return false;

		nvme_usleep(NVME_PASSTHROUGH_POLL_US);
	}
}

/*
 * Execute an admin command for a caller outside libnvme, waiting no
 * longer than timeout_ms. The caller holds ctrlr->lock.
 *
 * When the deadline passes, the command is aborted. An Abort is only a
 * request, and its own completion says nothing certain about the command
 * it names: the controller may post that command's completion before the
 * Abort's, after it, or not abort it at all (NVM Express Base
 * Specification 2.3, section 5.2.1, PDF pp. 200-201). Only the command's
 * own completion shows that the controller no longer accesses its buffer,
 * so that is what we wait for. If it never arrives, *buf_busy is set and
 * the caller must not free or reuse the buffer.
 */
int nvme_admin_passthrough(struct nvme_ctrlr *ctrlr,
			   const struct nvme_cmd *cmd,
			   void *buf, uint32_t len,
			   uint32_t timeout_ms,
			   struct nvme_cpl *cpl,
			   bool *buf_busy)
{
	struct nvme_qpair *adminq = &ctrlr->adminq;
	struct nvme_completion_poll_status *status;
	struct nvme_completion_poll_status *abort_status;
	struct nvme_request *req, *abort_req, *queued;
	struct nvme_tracker *tr;
	struct nvme_cmd abort_cmd;
	bool submitted = false;
	bool cancelled = false;
	bool done;
	uint16_t cid = 0;
	bigtime_t deadline;
	int ret;

	*buf_busy = false;

	if (timeout_ms == 0)
		return EINVAL;

	/*
	 * A buffer and its length come together. The length is capped at
	 * max_xfer_size, which is the smaller of the drive's MDTS and what one
	 * tracker's PRP list can describe.
	 */
	if ((buf == NULL) != (len == 0) || len > ctrlr->max_xfer_size)
		return EINVAL;

	/*
	 * Both poll statuses live on the heap: a command that completes
	 * after we gave up on it still writes its completion there.
	 */
	status = calloc(1, sizeof(*status));
	if (!status)
		return ENOMEM;

	deadline = system_time() + (bigtime_t)timeout_ms * 1000;

	ret = nvme_admin_submit_polled(ctrlr, cmd, buf, len, status, &req);
	if (ret != 0) {
		free(status);
		return ret;
	}

	if (nvme_admin_wait_until(ctrlr, status, NULL, deadline)) {
		memcpy(cpl, &status->cpl, sizeof(*cpl));
		free(status);
		return 0;
	}

	/*
	 * The deadline passed. If the request holds a tracker, the controller
	 * has it: note the tracker's command identifier for the Abort. If it
	 * is still waiting for a free tracker, the controller never saw it,
	 * so take it back. Completions are only processed under the queue
	 * lock, so the request cannot finish while we look.
	 */
	pthread_mutex_lock(&adminq->lock);
	done = status->done;
	if (!done) {
		LIST_FOREACH(tr, &adminq->outstanding_tr, list) {
			if (tr->req == req) {
				cid = tr->cid;
				submitted = true;
				break;
			}
		}
		if (!submitted) {
			STAILQ_FOREACH(queued, &adminq->queued_req, stailq) {
				if (queued == req)
					break;
			}
			if (queued) {
				STAILQ_REMOVE(&adminq->queued_req, req,
					      nvme_request, stailq);
				nvme_request_free_locked(req);
				cancelled = true;
			}
		}
	}
	pthread_mutex_unlock(&adminq->lock);

	if (done) {
		memcpy(cpl, &status->cpl, sizeof(*cpl));
		free(status);
		return 0;
	}

	if (cancelled) {
		nvme_notice("Admin command %02x timed out before submission\n",
			    (unsigned int)cmd->opc);
		free(status);
		return ETIMEDOUT;
	}

	if (submitted)
		nvme_notice("Admin command %02x (cid %u) timed out after %u ms, "
			    "aborting\n", (unsigned int)cmd->opc,
			    (unsigned int)cid, (unsigned int)timeout_ms);
	else
		nvme_notice("Admin command %02x timed out during submission\n",
			    (unsigned int)cmd->opc);

	/*
	 * Abort: CDW10 holds the CID in bits 31:16 and the submission queue
	 * ID in bits 15:00 (Base Specification 2.3, Figure 146, PDF p. 201).
	 * A request that was neither queued nor on a tracker is in the
	 * middle of being submitted, so there is no CID to name yet.
	 */
	abort_status = submitted ? calloc(1, sizeof(*abort_status)) : NULL;
	if (abort_status) {
		memset(&abort_cmd, 0, sizeof(abort_cmd));
		abort_cmd.opc = NVME_OPC_ABORT;
		abort_cmd.cdw10 = ((uint32_t)cid << 16) | adminq->id;

		if (nvme_admin_submit_polled(ctrlr, &abort_cmd, NULL, 0,
					     abort_status, &abort_req) != 0) {
			free(abort_status);
			abort_status = NULL;
		}
	}

	/*
	 * Even if the Abort could not be sent, give the command the same
	 * grace period to complete on its own.
	 */
	deadline = system_time()
		+ (bigtime_t)NVME_PASSTHROUGH_ABORT_GRACE_MS * 1000;
	nvme_admin_wait_until(ctrlr, status, abort_status, deadline);

	/* An Abort that is still outstanding keeps its status. */
	if (abort_status && abort_status->done)
		free(abort_status);

	if (status->done) {
		free(status);
		return ETIMEDOUT;
	}

	/*
	 * The controller may still read or write the buffer, and will
	 * still post a completion to status. Leave both allocated.
	 */
	nvme_err("Admin command %02x did not complete within the grace "
		 "period, leaking its buffer\n", (unsigned int)cmd->opc);
	*buf_busy = true;

	return ETIMEDOUT;
}

/*
 * Validate a FW.
 */
int nvme_admin_fw_commit(struct nvme_ctrlr *ctrlr,
			 const struct nvme_fw_commit *fw_commit)
{
	struct nvme_cmd cmd;

	/* Setup the command */
	memset(&cmd, 0, sizeof(struct nvme_cmd));
	cmd.opc = NVME_OPC_FIRMWARE_COMMIT;
	memcpy(&cmd.cdw10, fw_commit, sizeof(uint32_t));

	/* Execute the command */
	return nvme_admin_exec_cmd(ctrlr, &cmd, NULL, 0);
}

/*
 * Download to the device a firmware.
 */
int nvme_admin_fw_image_dl(struct nvme_ctrlr *ctrlr,
			   void *fw, uint32_t size,
			   uint32_t offset)
{
	struct nvme_cmd cmd;

	/* Setup the command */
	memset(&cmd, 0, sizeof(struct nvme_cmd));
	cmd.opc = NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD;
	cmd.cdw10 = (size >> 2) - 1;
	cmd.cdw11 = offset >> 2;

	/* Execute the command */
	return nvme_admin_exec_cmd(ctrlr, &cmd, fw, size);
}
