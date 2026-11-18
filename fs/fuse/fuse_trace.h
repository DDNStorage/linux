/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM fuse

#if !defined(_TRACE_FUSE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_FUSE_H

#include <linux/tracepoint.h>
#include "fuse_qlease.h"

#define OPCODES							\
	EM( FUSE_LOOKUP,		"FUSE_LOOKUP")		\
	EM( FUSE_FORGET,		"FUSE_FORGET")		\
	EM( FUSE_GETATTR,		"FUSE_GETATTR")		\
	EM( FUSE_SETATTR,		"FUSE_SETATTR")		\
	EM( FUSE_READLINK,		"FUSE_READLINK")	\
	EM( FUSE_SYMLINK,		"FUSE_SYMLINK")		\
	EM( FUSE_MKNOD,			"FUSE_MKNOD")		\
	EM( FUSE_MKDIR,			"FUSE_MKDIR")		\
	EM( FUSE_UNLINK,		"FUSE_UNLINK")		\
	EM( FUSE_RMDIR,			"FUSE_RMDIR")		\
	EM( FUSE_RENAME,		"FUSE_RENAME")		\
	EM( FUSE_LINK,			"FUSE_LINK")		\
	EM( FUSE_OPEN,			"FUSE_OPEN")		\
	EM( FUSE_READ,			"FUSE_READ")		\
	EM( FUSE_WRITE,			"FUSE_WRITE")		\
	EM( FUSE_STATFS,		"FUSE_STATFS")		\
	EM( FUSE_RELEASE,		"FUSE_RELEASE")		\
	EM( FUSE_FSYNC,			"FUSE_FSYNC")		\
	EM( FUSE_SETXATTR,		"FUSE_SETXATTR")	\
	EM( FUSE_GETXATTR,		"FUSE_GETXATTR")	\
	EM( FUSE_LISTXATTR,		"FUSE_LISTXATTR")	\
	EM( FUSE_REMOVEXATTR,		"FUSE_REMOVEXATTR")	\
	EM( FUSE_FLUSH,			"FUSE_FLUSH")		\
	EM( FUSE_INIT,			"FUSE_INIT")		\
	EM( FUSE_OPENDIR,		"FUSE_OPENDIR")		\
	EM( FUSE_READDIR,		"FUSE_READDIR")		\
	EM( FUSE_RELEASEDIR,		"FUSE_RELEASEDIR")	\
	EM( FUSE_FSYNCDIR,		"FUSE_FSYNCDIR")	\
	EM( FUSE_GETLK,			"FUSE_GETLK")		\
	EM( FUSE_SETLK,			"FUSE_SETLK")		\
	EM( FUSE_SETLKW,		"FUSE_SETLKW")		\
	EM( FUSE_ACCESS,		"FUSE_ACCESS")		\
	EM( FUSE_CREATE,		"FUSE_CREATE")		\
	EM( FUSE_INTERRUPT,		"FUSE_INTERRUPT")	\
	EM( FUSE_BMAP,			"FUSE_BMAP")		\
	EM( FUSE_DESTROY,		"FUSE_DESTROY")		\
	EM( FUSE_IOCTL,			"FUSE_IOCTL")		\
	EM( FUSE_POLL,			"FUSE_POLL")		\
	EM( FUSE_NOTIFY_REPLY,		"FUSE_NOTIFY_REPLY")	\
	EM( FUSE_BATCH_FORGET,		"FUSE_BATCH_FORGET")	\
	EM( FUSE_FALLOCATE,		"FUSE_FALLOCATE")	\
	EM( FUSE_READDIRPLUS,		"FUSE_READDIRPLUS")	\
	EM( FUSE_RENAME2,		"FUSE_RENAME2")		\
	EM( FUSE_LSEEK,			"FUSE_LSEEK")		\
	EM( FUSE_COPY_FILE_RANGE,	"FUSE_COPY_FILE_RANGE")	\
	EM( FUSE_SETUPMAPPING,		"FUSE_SETUPMAPPING")	\
	EM( FUSE_REMOVEMAPPING,		"FUSE_REMOVEMAPPING")	\
	EM( FUSE_SYNCFS,		"FUSE_SYNCFS")		\
	EM( FUSE_TMPFILE,		"FUSE_TMPFILE")		\
	EM( FUSE_STATX,			"FUSE_STATX")		\
	EM( FUSE_DLM_WB_LOCK,		"FUSE_DLM_WB_LOCK")	\
	EM( FUSE_QUOTA_ADMIT,		"FUSE_QUOTA_ADMIT")	\
	EMe(CUSE_INIT,			"CUSE_INIT")

/*
 * This will turn the above table into TRACE_DEFINE_ENUM() for each of the
 * entries.
 */
#undef EM
#undef EMe
#define EM(a, b)	TRACE_DEFINE_ENUM(a);
#define EMe(a, b)	TRACE_DEFINE_ENUM(a);

OPCODES

/* Now we redfine it with the table that __print_symbolic needs. */
#undef EM
#undef EMe
#define EM(a, b)	{a, b},
#define EMe(a, b)	{a, b}

#define FUSE_REQ_TRACE_FIELDS				\
	__field(dev_t,			connection)	\
	__field(uint64_t,		unique)		\
	__field(enum fuse_opcode,	opcode)		\
	__field(uint32_t,		len)		\

#define FUSE_REQ_TRACE_ASSIGN(req)				\
	do {							\
		__entry->connection	= req->fm->fc->dev;	\
		__entry->unique		= req->in.h.unique;	\
		__entry->opcode		= req->in.h.opcode;	\
		__entry->len		= req->in.h.len;	\
	} while (0)


TRACE_EVENT(fuse_request_enqueue,
	TP_PROTO(const struct fuse_req *req),
	TP_ARGS(req),
	TP_STRUCT__entry(FUSE_REQ_TRACE_FIELDS),
	TP_fast_assign(FUSE_REQ_TRACE_ASSIGN(req)),

	TP_printk("connection %u req %llu opcode %u (%s) len %u ",
		  __entry->connection, __entry->unique, __entry->opcode,
		  __print_symbolic(__entry->opcode, OPCODES), __entry->len)
);

TRACE_EVENT(fuse_request_bg_enqueue,
	TP_PROTO(const struct fuse_req *req),
	TP_ARGS(req),
	TP_STRUCT__entry(FUSE_REQ_TRACE_FIELDS),
	TP_fast_assign(FUSE_REQ_TRACE_ASSIGN(req)),

	TP_printk("connection %u req %llu opcode %u (%s) len %u ",
		  __entry->connection, __entry->unique, __entry->opcode,
		  __print_symbolic(__entry->opcode, OPCODES), __entry->len)
);

TRACE_EVENT(fuse_request_send,
	TP_PROTO(const struct fuse_req *req),
	TP_ARGS(req),
	TP_STRUCT__entry(FUSE_REQ_TRACE_FIELDS),
	TP_fast_assign(FUSE_REQ_TRACE_ASSIGN(req)),

	TP_printk("connection %u req %llu opcode %u (%s) len %u ",
		  __entry->connection, __entry->unique, __entry->opcode,
		  __print_symbolic(__entry->opcode, OPCODES), __entry->len)
);


TRACE_EVENT(fuse_request_end,
	TP_PROTO(const struct fuse_req *req),

	TP_ARGS(req),

	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	unique)
		__field(uint32_t,	len)
		__field(int32_t,	error)
	),

	TP_fast_assign(
		__entry->connection	=	req->fm->fc->dev;
		__entry->unique		=	req->in.h.unique;
		__entry->len		=	req->out.h.len;
		__entry->error		=	req->out.h.error;
	),

	TP_printk("connection %u req %llu len %u error %d", __entry->connection,
		  __entry->unique, __entry->len, __entry->error)
);

TRACE_EVENT(fuse_qlease_reply,
	TP_PROTO(struct inode *inode, const struct fuse_quota_lease *ql,
		 bool wanted),
	TP_ARGS(inode, ql, wanted),
	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	nodeid)
		__field(uint64_t,	lease_id)
		__field(uint64_t,	gen)
		__field(int32_t,	ttl_ms)
		__field(uint32_t,	flags)
		__field(int32_t,	error)
		__field(bool,		wanted)
	),
	TP_fast_assign(
		__entry->connection	= get_fuse_conn(inode)->dev;
		__entry->nodeid		= get_fuse_inode(inode)->nodeid;
		__entry->lease_id	= ql->lease_id;
		__entry->gen		= ql->gen;
		__entry->ttl_ms		= ql->ttl_ms;
		__entry->flags		= ql->flags;
		__entry->error		= ql->error;
		__entry->wanted		= wanted;
	),
	TP_printk("connection %u nodeid %llu lease %llx gen %llu ttl %d flags %x error %d wanted %d",
		  __entry->connection, __entry->nodeid, __entry->lease_id,
		  __entry->gen, __entry->ttl_ms, __entry->flags,
		  __entry->error, __entry->wanted)
);

DECLARE_EVENT_CLASS(fuse_qlease_class,
	TP_PROTO(struct fuse_qlease *l),
	TP_ARGS(l),
	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	lease_id)
		__field(uint64_t,	gen)
		__field(long,		left_ms)
		__field(bool,		rejected)
	),
	TP_fast_assign(
		__entry->connection	= l->fc->dev;
		__entry->lease_id	= l->lease_id;
		__entry->gen		= l->gen;
		__entry->left_ms	= (long)(l->deadline - jiffies) * 1000 / HZ;
		__entry->rejected	= l->rejected;
	),
	TP_printk("connection %u lease %llx gen %llu left %ld ms rejected %d",
		  __entry->connection, __entry->lease_id, __entry->gen,
		  __entry->left_ms, __entry->rejected)
);

DEFINE_EVENT(fuse_qlease_class, fuse_qlease_install,
	     TP_PROTO(struct fuse_qlease *l), TP_ARGS(l));
DEFINE_EVENT(fuse_qlease_class, fuse_qlease_extend,
	     TP_PROTO(struct fuse_qlease *l), TP_ARGS(l));
DEFINE_EVENT(fuse_qlease_class, fuse_qlease_expire,
	     TP_PROTO(struct fuse_qlease *l), TP_ARGS(l));

TRACE_EVENT(fuse_qlease_flush,
	TP_PROTO(struct fuse_qlease *l, unsigned int ninodes),
	TP_ARGS(l, ninodes),
	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	lease_id)
		__field(unsigned int,	ninodes)
	),
	TP_fast_assign(
		__entry->connection	= l->fc->dev;
		__entry->lease_id	= l->lease_id;
		__entry->ninodes	= ninodes;
	),
	TP_printk("connection %u lease %llx inodes %u",
		  __entry->connection, __entry->lease_id, __entry->ninodes)
);

/* @flags: FUSE_QLEASE_* of the admit reply, 0 when @err is set */
TRACE_EVENT(fuse_qlease_renew,
	TP_PROTO(struct fuse_qlease *l, bool background, int err,
		 uint32_t flags),
	TP_ARGS(l, background, err, flags),
	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	lease_id)
		__field(bool,		background)
		__field(int,		err)
		__field(uint32_t,	flags)
	),
	TP_fast_assign(
		__entry->connection	= l->fc->dev;
		__entry->lease_id	= l->lease_id;
		__entry->background	= background;
		__entry->err		= err;
		__entry->flags		= flags;
	),
	TP_printk("connection %u lease %llx background %d err %d flags %x",
		  __entry->connection, __entry->lease_id, __entry->background,
		  __entry->err, __entry->flags)
);

/*
 * A renewal's admit reply after the generation check: installed, or dropped
 * because a newer reply was applied while the renewal was in flight. The
 * lease fields are the resulting state.
 */
TRACE_EVENT(fuse_qlease_renew_reply,
	TP_PROTO(struct fuse_qlease *l, const struct fuse_quota_lease *ql,
		 bool installed),
	TP_ARGS(l, ql, installed),
	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	lease_id)
		__field(uint64_t,	reply_gen)
		__field(uint64_t,	gen)
		__field(uint32_t,	flags)
		__field(long,		left_ms)
		__field(bool,		rejected)
		__field(bool,		installed)
	),
	TP_fast_assign(
		__entry->connection	= l->fc->dev;
		__entry->lease_id	= l->lease_id;
		__entry->reply_gen	= ql->gen;
		__entry->gen		= l->gen;
		__entry->flags		= ql->flags;
		__entry->left_ms	= (long)(l->deadline - jiffies) * 1000 / HZ;
		__entry->rejected	= l->rejected;
		__entry->installed	= installed;
	),
	TP_printk("connection %u lease %llx reply gen %llu flags %x installed %d gen %llu left %ld ms rejected %d",
		  __entry->connection, __entry->lease_id, __entry->reply_gen,
		  __entry->flags, __entry->installed, __entry->gen,
		  __entry->left_ms, __entry->rejected)
);

/* The session quota state, fired once from the INIT reply */
TRACE_EVENT(fuse_qlease_quota_state,
	TP_PROTO(struct fuse_conn *fc, bool off),
	TP_ARGS(fc, off),
	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(bool,		off)
	),
	TP_fast_assign(
		__entry->connection	= fc->dev;
		__entry->off		= off;
	),
	TP_printk("connection %u quota %s",
		  __entry->connection, __entry->off ? "off" : "enabled")
);

/* A first-admission FUSE_QUOTA_ADMIT for a cached grant with no lease */
TRACE_EVENT(fuse_qlease_first_admit,
	TP_PROTO(struct inode *inode),
	TP_ARGS(inode),
	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	nodeid)
	),
	TP_fast_assign(
		__entry->connection	= get_fuse_conn(inode)->dev;
		__entry->nodeid		= get_fuse_inode(inode)->nodeid;
	),
	TP_printk("connection %u nodeid %llu",
		  __entry->connection, __entry->nodeid)
);

/*
 * The grant check again after a lease request sent from it (@sent: a
 * first-admission admit, a lock upcall that carried the first admission, a
 * renewal, or a wait for another task's renewal): @init_us is how long the
 * range lock stayed in INIT (with a writer's inode lock held) for it and,
 * for a renewal, its flush; @err is its result; @held is false when a
 * revoke dropped the grant meanwhile.
 */
TRACE_EVENT(fuse_qlease_recheck,
	TP_PROTO(struct inode *inode, int sent, int err, s64 init_us,
		 bool held),
	TP_ARGS(inode, sent, err, init_us, held),
	TP_STRUCT__entry(
		__field(dev_t,		connection)
		__field(uint64_t,	nodeid)
		__field(int,		sent)
		__field(int,		err)
		__field(s64,		init_us)
		__field(bool,		held)
	),
	TP_fast_assign(
		__entry->connection	= get_fuse_conn(inode)->dev;
		__entry->nodeid		= get_fuse_inode(inode)->nodeid;
		__entry->sent		= sent;
		__entry->err		= err;
		__entry->init_us	= init_us;
		__entry->held		= held;
	),
	TP_printk("connection %u nodeid %llu sent %s err %d init %lld us held %d",
		  __entry->connection, __entry->nodeid,
		  __print_symbolic(__entry->sent,
				   { FUSE_QLEASE_SENT_ADMIT, "admit" },
				   { FUSE_QLEASE_SENT_LOCK, "lock" },
				   { FUSE_QLEASE_SENT_RENEW, "renew" },
				   { FUSE_QLEASE_SENT_WAIT, "wait" }),
		  __entry->err, __entry->init_us, __entry->held)
);

#endif /* _TRACE_FUSE_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE fuse_trace
#include <trace/define_trace.h>
