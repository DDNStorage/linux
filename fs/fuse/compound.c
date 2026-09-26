// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE: Filesystem in Userspace
 * Copyright (C) 2025
 *
 * This file implements compound operations for FUSE, allowing multiple
 * operations to be batched into a single request to reduce round trips
 * between kernel and userspace.
 */

#include "fuse_i.h"

/*
 * Compound request builder and state tracker and args pointer storage
 */
struct fuse_compound_req {
	struct fuse_mount *fm;
	struct fuse_compound_in compound_header;
	struct fuse_compound_out result_header;

	/* Per-operation error codes */
	int op_errors[FUSE_MAX_COMPOUND_OPS];
	struct fuse_args *op_args[FUSE_MAX_COMPOUND_OPS];
};

struct fuse_compound_req *fuse_compound_alloc(struct fuse_mount *fm, u32 flags)
{
	struct fuse_compound_req *compound;

	compound = kzalloc(sizeof(*compound), GFP_KERNEL);
	if (!compound)
		return ERR_PTR(-ENOMEM);

	compound->fm = fm;
	compound->compound_header.flags = flags;

	return compound;
}

int fuse_compound_add(struct fuse_compound_req *compound,
		      struct fuse_args *args)
{
	if (!compound ||
	    compound->compound_header.count >= FUSE_MAX_COMPOUND_OPS)
		return -EINVAL;

	if (args->in_pages)
		return -EINVAL;

	compound->op_args[compound->compound_header.count] = args;
	compound->compound_header.count++;
	return 0;
}

/*
 * Copy a slot's payload into the caller's out_args structs.
 *
 * @avail is the slot's actual payload size (op_hdr->len minus the slot's
 * fuse_out_header). The caller is responsible for ensuring @resp..+@avail is
 * within the response buffer.
 *
 * Returns 0 on success, -EIO if the slot is shorter than the declared args
 * sum. Trailing bytes beyond the args sum are ignored (forward-compat).
 */
static int fuse_copy_response_per_req(struct fuse_args *args,
				      char *resp, size_t avail)
{
	int i;
	size_t copied = 0;

	for (i = 0; i < args->out_numargs; i++) {
		struct fuse_arg current_arg = args->out_args[i];
		size_t arg_size = current_arg.size;

		if (!current_arg.value || arg_size == 0)
			continue;

		if (copied + arg_size > avail)
			return -EIO;

		memcpy(current_arg.value, resp + copied, arg_size);
		copied += arg_size;
	}

	return 0;
}

int fuse_compound_get_error(struct fuse_compound_req *compound, int op_idx)
{
	return compound->op_errors[op_idx];
}

/*
 * Parse one slot's response. An empty slot (payload size 0) leaves the
 * caller's out_args untouched -- by convention callers zero-init their
 * out structs, so an empty slot means "no result for this op". Slots
 * with payload must contain at least the args sum; extra trailing bytes
 * are ignored. The returned pointer always advances by op_hdr->len.
 */
static void *fuse_compound_parse_one_op(struct fuse_compound_req *compound,
					int op_index, void *op_out_data,
					void *response_end)
{
	struct fuse_out_header *op_hdr = op_out_data;
	struct fuse_args *args = compound->op_args[op_index];
	size_t payload_size;

	if (op_hdr->len < sizeof(struct fuse_out_header))
		return NULL;

	/* Check if the entire operation response fits in the buffer */
	if ((char *)op_out_data + op_hdr->len > (char *)response_end)
		return NULL;

	if (op_hdr->error != 0)
		compound->op_errors[op_index] = op_hdr->error;

	payload_size = op_hdr->len - sizeof(struct fuse_out_header);

	if (args && payload_size > 0) {
		if (fuse_copy_response_per_req(args,
				(char *)op_out_data + sizeof(struct fuse_out_header),
				payload_size) < 0)
			return NULL;
	}

	return (char *)op_out_data + op_hdr->len;
}

static int fuse_compound_parse_resp(struct fuse_compound_req *compound,
				    u32 count, void *response,
				    size_t response_size)
{
	void *op_out_data = response;
	void *response_end = (char *)response + response_size;
	int i;

	if (!response || response_size < sizeof(struct fuse_out_header))
		return -EIO;

	for (i = 0; i < count && i < compound->result_header.count; i++) {
		op_out_data = fuse_compound_parse_one_op(compound, i,
							 op_out_data,
							 response_end);
		if (!op_out_data)
			return -EIO;
	}

	return 0;
}

ssize_t fuse_compound_send(struct fuse_compound_req *compound)
{
	struct fuse_args args = {
		.opcode = FUSE_COMPOUND,
		.nodeid = 0,
		.in_numargs = 2,
		.out_numargs = 2,
		.out_argvar = true,
	};
	size_t resp_buffer_size;
	size_t actual_response_size;
	size_t buffer_pos;
	size_t total_expected_out_size;
	void *buffer = NULL;
	void *resp_payload;
	ssize_t ret;
	int i;

	if (!compound) {
		pr_info_ratelimited("FUSE: compound request is NULL in %s\n",
				    __func__);
		return -EINVAL;
	}

	if (compound->compound_header.count == 0) {
		pr_info_ratelimited("FUSE: compound request contains no operations\n");
		return -EINVAL;
	}

	buffer_pos = 0;
	total_expected_out_size = 0;

	for (i = 0; i < compound->compound_header.count; i++) {
		struct fuse_args *op_args = compound->op_args[i];
		size_t needed_size = sizeof(struct fuse_in_header);
		int j;

		for (j = 0; j < op_args->in_numargs; j++)
			needed_size += op_args->in_args[j].size;

		buffer_pos += needed_size;

		for (j = 0; j < op_args->out_numargs; j++)
			total_expected_out_size += op_args->out_args[j].size;
	}

	buffer = kvmalloc(buffer_pos, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	buffer_pos = 0;
	for (i = 0; i < compound->compound_header.count; i++) {
		struct fuse_args *op_args = compound->op_args[i];
		struct fuse_in_header *hdr;
		size_t needed_size = sizeof(struct fuse_in_header);
		int j;

		for (j = 0; j < op_args->in_numargs; j++)
			needed_size += op_args->in_args[j].size;

		hdr = (struct fuse_in_header *)(buffer + buffer_pos);
		memset(hdr, 0, sizeof(*hdr));
		hdr->len = needed_size;
		hdr->opcode = op_args->opcode;
		hdr->nodeid = op_args->nodeid;
		hdr->uid = from_kuid(compound->fm->fc->user_ns,
				     current_fsuid());
		hdr->gid = from_kgid(compound->fm->fc->user_ns,
				     current_fsgid());
		hdr->pid = pid_nr_ns(task_pid(current),
				     compound->fm->fc->pid_ns);
		buffer_pos += sizeof(*hdr);

		for (j = 0; j < op_args->in_numargs; j++) {
			memcpy(buffer + buffer_pos, op_args->in_args[j].value,
			       op_args->in_args[j].size);
			buffer_pos += op_args->in_args[j].size;
		}
	}

	resp_buffer_size = total_expected_out_size +
			   (compound->compound_header.count *
			    sizeof(struct fuse_out_header));

	resp_payload = kvmalloc(resp_buffer_size, GFP_KERNEL | __GFP_ZERO);
	if (!resp_payload) {
		ret = -ENOMEM;
		goto out_free_buffer;
	}

	compound->compound_header.result_size = total_expected_out_size;

	args.in_args[0].size = sizeof(compound->compound_header);
	args.in_args[0].value = &compound->compound_header;
	args.in_args[1].size = buffer_pos;
	args.in_args[1].value = buffer;

	args.out_args[0].size = sizeof(compound->result_header);
	args.out_args[0].value = &compound->result_header;
	args.out_args[1].size = resp_buffer_size;
	args.out_args[1].value = resp_payload;

	ret = fuse_simple_request(compound->fm, &args);
	if (ret < 0)
		goto out;

	actual_response_size = args.out_args[1].size;

	/*
	 * compound_header (out_args[0]) is fixed-size and already validated
	 * by fuse_simple_request; check that the server didn't claim more
	 * results than we requested, and that the payload at least leaves
	 * room for one fuse_out_header per claimed result.
	 */
	if (compound->result_header.count > compound->compound_header.count) {
		pr_info_ratelimited("FUSE: compound response claims %u ops, request had %u\n",
				    compound->result_header.count,
				    compound->compound_header.count);
		ret = -EINVAL;
		goto out;
	}

	if (actual_response_size <
	    compound->result_header.count * sizeof(struct fuse_out_header)) {
		pr_info_ratelimited("FUSE: compound payload too small for %u ops (%zu bytes)\n",
				    compound->result_header.count,
				    actual_response_size);
		ret = -EINVAL;
		goto out;
	}

	ret = fuse_compound_parse_resp(compound, compound->result_header.count,
				       (char *)resp_payload,
				       actual_response_size);
out:
	kvfree(resp_payload);
out_free_buffer:
	kvfree(buffer);
	return ret;
}
