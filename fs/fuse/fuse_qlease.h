/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Quota admission leases for writeback-cache writes (RED-52469).
 *
 * A lease belongs to one file owner on one connection and admits dirty
 * data until its deadline. Inodes of one owner share it. See
 * doc/design/specs/red-52469-wbc-quota-admission-lease.md in red-fs.
 */
#ifndef _FS_FUSE_QLEASE_H
#define _FS_FUSE_QLEASE_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/refcount.h>
#include <linux/rcupdate.h>
#include <linux/seqlock.h>
#include <linux/spinlock.h>
#include <linux/mm_types.h>

struct fuse_conn;
struct fuse_inode;
struct fuse_quota_lease;
struct inode;

/*
 * The lease changed under the request (a renewal in flight finished, or the
 * reply linked an expired lease): check the grant and the lease again.
 * fuse_get_dlm_lock() does so itself.
 */
#define FUSE_QLEASE_RESTART	2
/* fuse_qlease_check(): the inode has no lease, or its owner changed */
#define FUSE_QLEASE_NONE	3
/* fuse_qlease_check(): expired, or rejected past its hold time */
#define FUSE_QLEASE_RENEW	4
/* fuse_qlease_renew(): waited for another task's renewal; check again */
#define FUSE_QLEASE_WAITED	5

/* What the grant check sent before it checks again (fuse_qlease_recheck) */
enum fuse_qlease_sent {
	FUSE_QLEASE_SENT_NONE,
	FUSE_QLEASE_SENT_ADMIT,	/* first-admission FUSE_QUOTA_ADMIT */
	FUSE_QLEASE_SENT_LOCK,		/* lock upcall carrying the first one */
	FUSE_QLEASE_SENT_RENEW,	/* renewal, flush and admit */
	FUSE_QLEASE_SENT_WAIT,		/* waited for another renewal */
};

/*
 * Locking (lock order: fc->qlease_lock, then l->lock, then m->lock):
 *
 * fc->qlease_lock protects the hash (hnode) and the inode links: fi->qlease,
 * fi->qlease_node, fi->qlease_uid/gid and l->inodes. Nothing else.
 *
 * l->lock protects the lease state: tsd_seq, ug_seq, deadline, reject_until,
 * renew_at, rejected, renewing, dying, the reset of used, and the arming of
 * the work.
 * Every change of deadline, reject_until or rejected is made in a
 * write_seqcount_begin(&l->seq) section under l->lock, so the write fast
 * path (fuse_qlease_check(), fuse_qlease_valid()) reads them with a
 * seqcount loop and takes no lock. used is set there with WRITE_ONCE() only.
 *
 * Lifetime: fi->qlease is set with rcu_assign_pointer() under
 * fc->qlease_lock and read under rcu_read_lock(). The last put removes the
 * lease from the hash under fc->qlease_lock (refcount_dec_and_lock()) and
 * frees it with kfree_rcu(), so a lockless reader that found it through
 * fi->qlease may read it until rcu_read_unlock(). A reader that keeps it
 * takes a reference with refcount_inc_not_zero() and treats failure as no
 * lease. While a lease is in the hash its count is at least one.
 *
 * References: one per linked inode and one while the work item is queued.
 * A renewal runs under its caller's reference (the writer's, or the queued
 * work's). The work and waitq live in the lease and are used only by
 * holders of a reference.
 */
struct fuse_qlease {
	struct hlist_node	hnode;		/* fc->qlease_lock */
	struct fuse_conn	*fc;
	u64			lease_id;	/* constant */
	spinlock_t		lock;
	seqcount_spinlock_t	seq;		/* deadline, rejected, ... */
	u64			tsd_seq;	/* newest TSD voucher seq applied */
	u64			ug_seq;		/* newest UG voucher seq applied */
	unsigned long		deadline;	/* jiffies */
	unsigned long		reject_until;	/* jiffies, valid if rejected */
	unsigned long		renew_at;	/* jiffies, half TTL */
	bool			used;		/* admitted a write since renewal */
	bool			renewing;
	bool			rejected;
	bool			dying;		/* conn going away: never arm */
	struct list_head	inodes;		/* fc->qlease_lock */
	wait_queue_head_t	waitq;		/* waiters for renewing */
	struct delayed_work	work;
	refcount_t		ref;
	struct rcu_head		rcu;
};

/*
 * The shared-writable mmap state of a regular inode (fi->qlease_mmap).
 * Allocated by fuse_wb_mmap() for the first VM_SHARED and VM_MAYWRITE
 * mapping on a quota enabled lease session, published with cmpxchg(), and
 * freed at eviction after unmap_work was cancelled. An inode without one
 * has never been mapped shared-writable: it has no mark to take and no
 * PTE to unmap, and every user treats NULL that way.
 */
struct fuse_qlease_mmap {
	struct fuse_inode	*fi;		/* back pointer for unmap_work */
	/* error recorded with mmap_lock dropped, see fuse_qlease_mark() */
	int			err;
	unsigned long		err_at;		/* jiffies */
	unsigned long		err_since;
	/* one-shot admission token, see fuse_qlease_mark_admitted() */
	unsigned long		admitted_at;	/* jiffies, 0: none */
	/* unmap range, empty if first > last; lock is a leaf lock */
	spinlock_t		lock;
	pgoff_t			unmap_first;
	pgoff_t			unmap_last;
	struct work_struct	unmap_work;
};

void fuse_qlease_conn_init(struct fuse_conn *fc);
void fuse_qlease_conn_shutdown(struct fuse_conn *fc);
void fuse_qlease_conn_destroy(struct fuse_conn *fc);
void fuse_qlease_inode_init(struct fuse_inode *fi);
void fuse_qlease_inode_evict(struct fuse_inode *fi);
int fuse_qlease_mmap_prepare(struct fuse_inode *fi);

bool fuse_qlease_valid(struct fuse_inode *fi);
int fuse_qlease_check(struct inode *inode, struct fuse_qlease **lp);
void fuse_qlease_put(struct fuse_qlease *l);
int fuse_qlease_first_admit(struct inode *inode);
int fuse_qlease_apply(struct inode *inode, const struct fuse_quota_lease *ql,
		       bool wanted);
void fuse_qlease_conn_start(struct fuse_conn *fc, bool enabled);

/* module param qlease_mmap_retry_ms */
extern unsigned int fuse_qlease_mmap_retry_ms;

int fuse_qlease_renew(struct fuse_qlease *l, struct inode *via,
		       bool background);
vm_fault_t fuse_qlease_page_mkwrite(struct inode *inode, pgoff_t index);
void fuse_qlease_mark(struct fuse_inode *fi, int err);
void fuse_qlease_mark_clear(struct fuse_inode *fi);
bool fuse_qlease_take_mark(struct fuse_inode *fi);
void fuse_qlease_mark_admitted(struct fuse_inode *fi);
bool fuse_qlease_admitted_recent(struct fuse_inode *fi);

#endif /* _FS_FUSE_QLEASE_H */
