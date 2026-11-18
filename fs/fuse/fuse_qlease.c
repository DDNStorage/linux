// SPDX-License-Identifier: GPL-2.0
/*
 * Quota admission leases for writeback-cache writes (RED-52469).
 */
#include "fuse_i.h"
#include "fuse_qlease.h"
#include "fuse_trace.h"

#include <linux/hashtable.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/slab.h>

static unsigned int qlease_reject_hold_ms = 1000;
module_param(qlease_reject_hold_ms, uint, 0644);
MODULE_PARM_DESC(qlease_reject_hold_ms,
		 "Time a rejected quota lease fails writes locally (ms)");

unsigned int fuse_qlease_mmap_retry_ms = 5000;
module_param_named(qlease_mmap_retry_ms, fuse_qlease_mmap_retry_ms, uint,
		   0644);
MODULE_PARM_DESC(qlease_mmap_retry_ms,
		 "Time an mmap admit error other than EDQUOT is retried (ms)");

static void fuse_qlease_work(struct work_struct *work);
static void fuse_qlease_iput_work(struct work_struct *work);
static void fuse_qlease_unmap_work(struct work_struct *work);

/*
 * The inode references a renewal's flush took (igrab()), kept until its
 * admit has returned: the last reference of an unlinked inode evicts it,
 * and eviction waits for the writeback the flush just started.
 */
struct fuse_qlease_iputs {
	struct llist_node	node;
	unsigned int		n;
	struct inode		*inodes[];
};

void fuse_qlease_conn_init(struct fuse_conn *fc)
{
	/* wire layouts shared with redfsd (src/fuse/fuse.hpp) */
	BUILD_BUG_ON(sizeof(struct fuse_quota_lease) != 32);
	BUILD_BUG_ON(sizeof(struct fuse_quota_admit_in) != 8);
	BUILD_BUG_ON(sizeof(struct fuse_dlm_lock_lease_out) != 56);
	BUILD_BUG_ON(offsetof(struct fuse_dlm_lock_lease_out, lease) != 24);

	spin_lock_init(&fc->qlease_lock);
	hash_init(fc->qlease_hash);
	init_llist_head(&fc->qlease_iputs);
	INIT_WORK(&fc->qlease_iput_work, fuse_qlease_iput_work);
}

/*
 * Record the session quota state from the INIT reply, before any regular
 * inode exists. It never changes after INIT: a lease part's QUOTA_OFF flag
 * does not move it either way.
 */
void fuse_qlease_conn_start(struct fuse_conn *fc, bool enabled)
{
	WRITE_ONCE(fc->qlease_quota_off, !enabled);
	trace_fuse_qlease_quota_state(fc, !enabled);
}

/*
 * Out of the hash, with no reference left. A lockless reader may still be
 * looking at it through a stale fi->qlease under rcu_read_lock().
 */
static void fuse_qlease_free(struct fuse_qlease *l)
{
	WARN_ON(!list_empty(&l->inodes));
	kfree_rcu(l, rcu);
}

/* The caller holds a reference, or fc->qlease_lock with @l in the hash */
static void fuse_qlease_get(struct fuse_qlease *l)
{
	refcount_inc(&l->ref);
}

void fuse_qlease_put(struct fuse_qlease *l)
{
	struct fuse_conn *fc = l->fc;

	if (!refcount_dec_and_lock(&l->ref, &fc->qlease_lock))
		return;
	hash_del(&l->hnode);
	spin_unlock(&fc->qlease_lock);
	fuse_qlease_free(l);
}

/*
 * Stop @l from being armed again. dying is written under fc->qlease_lock
 * and l->lock and read under either (READ_ONCE() for the hash walk).
 */
static void fuse_qlease_set_dying(struct fuse_qlease *l)
{
	lockdep_assert_held(&l->fc->qlease_lock);
	spin_lock(&l->lock);
	WRITE_ONCE(l->dying, true);
	spin_unlock(&l->lock);
}

/*
 * Called from fuse_conn_destroy() at unmount, after the connection was
 * aborted and before kill_*_super() evicts the inodes. A work instance holds
 * an inode reference while it flushes or sends the admit, and would keep that
 * inode busy across evict_inodes(), so every lease is marked dying, which
 * stops fuse_qlease_arm(), and its work is cancelled. The leases stay linked;
 * eviction drops their link references.
 */
void fuse_qlease_conn_shutdown(struct fuse_conn *fc)
{
	struct fuse_qlease *l;
	int bkt;

	for (;;) {
		struct fuse_qlease *victim = NULL;

		spin_lock(&fc->qlease_lock);
		hash_for_each(fc->qlease_hash, bkt, l, hnode)
			if (!victim && !READ_ONCE(l->dying))
				victim = l;
		if (victim) {
			fuse_qlease_set_dying(victim);
			fuse_qlease_get(victim);
		}
		spin_unlock(&fc->qlease_lock);
		if (!victim)
			break;

		if (cancel_delayed_work_sync(&victim->work))
			fuse_qlease_put(victim);	/* queued work's ref */
		fuse_qlease_put(victim);
	}

	/*
	 * No writer or fault is left at unmount to defer more. The references
	 * a foreground renewal deferred are dropped now, not cancelled: they
	 * would keep their inodes busy across evict_inodes().
	 */
	flush_work(&fc->qlease_iput_work);
}

/*
 * Called from fuse_conn_put() after every inode of the connection is gone.
 * Only the queued work can still hold a reference then. Each lease is pinned
 * before the lock is dropped, so a work instance that runs and puts its
 * reference meanwhile cannot free it under us. A lease something else still
 * holds is a leak: warn and take it out of the table so the loop ends;
 * hash_del() is hlist_del_init(), so the holder's later put stays safe.
 */
void fuse_qlease_conn_destroy(struct fuse_conn *fc)
{
	struct fuse_qlease *l;
	struct hlist_node *tmp;
	int bkt;

	/* fuse_qlease_conn_shutdown() dropped them before the inodes went */
	WARN_ON_ONCE(!llist_empty(&fc->qlease_iputs));

	for (;;) {
		struct fuse_qlease *victim = NULL;

		spin_lock(&fc->qlease_lock);
		hash_for_each_safe(fc->qlease_hash, bkt, tmp, l, hnode) {
			victim = l;
			break;
		}
		if (victim) {
			/* a running instance must not queue it again */
			fuse_qlease_set_dying(victim);
			fuse_qlease_get(victim);
		}
		spin_unlock(&fc->qlease_lock);
		if (!victim)
			break;

		if (cancel_delayed_work_sync(&victim->work))
			fuse_qlease_put(victim);	/* queued work's ref */

		spin_lock(&fc->qlease_lock);
		if (WARN_ON_ONCE(refcount_read(&victim->ref) != 1))
			hash_del(&victim->hnode);
		spin_unlock(&fc->qlease_lock);
		fuse_qlease_put(victim);
	}
}

static struct fuse_qlease *fuse_qlease_find(struct fuse_conn *fc, u64 id)
{
	struct fuse_qlease *l;

	lockdep_assert_held(&fc->qlease_lock);
	hash_for_each_possible(fc->qlease_hash, l, hnode, id)
		if (l->lease_id == id)
			return l;
	return NULL;
}

/* Queue the work item at @when; the queued work holds one reference. */
static void fuse_qlease_arm(struct fuse_qlease *l, unsigned long when)
{
	unsigned long delay = time_after(when, jiffies) ? when - jiffies : 0;

	/* every arm is serialized by l->lock, see the get below */
	lockdep_assert_held(&l->lock);
	/*
	 * A dying lease is being cancelled: cancel_delayed_work_sync() disables
	 * the work meanwhile, so mod_delayed_work() would queue nothing and
	 * return false, and the reference below would never be put.
	 */
	if (l->dying)
		return;
	/*
	 * mod_delayed_work() returns false when it queued an idle work
	 * (including one that is running now, whose instance puts its own
	 * reference), so the newly queued work needs a reference.
	 */
	if (!mod_delayed_work(system_unbound_wq, &l->work, delay))
		fuse_qlease_get(l);
}

void fuse_qlease_inode_init(struct fuse_inode *fi)
{
	RCU_INIT_POINTER(fi->qlease, NULL);
	INIT_LIST_HEAD(&fi->qlease_node);
	fi->qlease_mmap = NULL;
}

/*
 * The shared-writable mmap state of @fi, allocated on the first call. Called
 * by fuse_wb_mmap() for a VM_SHARED and VM_MAYWRITE mapping on a quota
 * enabled lease session, under mmap_lock. Racing mappings allocate once:
 * the loser of the cmpxchg() frees its copy. Returns 0 or -ENOMEM, which
 * fails the mmap.
 */
int fuse_qlease_mmap_prepare(struct fuse_inode *fi)
{
	struct fuse_qlease_mmap *m;

	if (READ_ONCE(fi->qlease_mmap))
		return 0;
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->fi = fi;
	spin_lock_init(&m->lock);
	m->unmap_first = ULONG_MAX;	/* empty */
	m->unmap_last = 0;
	INIT_WORK(&m->unmap_work, fuse_qlease_unmap_work);
	/* release: a reader that sees the pointer sees it initialised */
	if (cmpxchg_release(&fi->qlease_mmap, NULL, m))
		kfree(m);
	return 0;
}

/*
 * The mmap state, or NULL if @fi was never mapped shared-writable. Set once
 * and freed only at eviction, so any user that pins the inode may keep it.
 */
static struct fuse_qlease_mmap *fuse_qlease_mmap(struct fuse_inode *fi)
{
	/* pairs with cmpxchg_release() in fuse_qlease_mmap_prepare() */
	return smp_load_acquire(&fi->qlease_mmap);
}

/*
 * The owner check of a lockless reader: qlease_uid/gid are written under
 * fc->qlease_lock before fi->qlease is published, and a mismatch only sends
 * the writer to an admit, so a racing relink costs a request at most.
 */
static bool fuse_qlease_owner_matches(struct fuse_inode *fi)
{
	struct inode *inode = &fi->inode;

	return uid_eq(READ_ONCE(fi->qlease_uid), inode->i_uid) &&
	       gid_eq(READ_ONCE(fi->qlease_gid), inode->i_gid);
}

/* fi->qlease for a holder of fc->qlease_lock */
static struct fuse_qlease *fuse_qlease_linked(struct fuse_inode *fi)
{
	return rcu_dereference_protected(fi->qlease,
		lockdep_is_held(&get_fuse_conn(&fi->inode)->qlease_lock));
}

/*
 * Link @fi to @l; takes a reference for the link. The work exits without
 * re-arming when it finds no linked inode, so the first link of a lease
 * with no work queued arms it at the deadline: it then expires or waits
 * for a renewal as usual. This is no renewal at a lock reply (D5).
 */
static void fuse_qlease_link(struct fuse_inode *fi, struct fuse_qlease *l)
{
	lockdep_assert_held(&l->fc->qlease_lock);
	/*
	 * The pending test under l->lock too: a renewal arms under l->lock
	 * only, and its arm at renew_at must not be pushed to the deadline.
	 */
	spin_lock(&l->lock);
	if (list_empty(&l->inodes) && !delayed_work_pending(&l->work))
		fuse_qlease_arm(l, l->deadline);
	spin_unlock(&l->lock);
	fuse_qlease_get(l);
	/*
	 * Order an earlier unlink's NULL store of fi->qlease before the new
	 * owner: pairs with the second smp_rmb() in fuse_qlease_state_rcu().
	 */
	smp_wmb();
	WRITE_ONCE(fi->qlease_uid, fi->inode.i_uid);
	WRITE_ONCE(fi->qlease_gid, fi->inode.i_gid);
	list_add_tail(&fi->qlease_node, &l->inodes);
	/* publishes the owner above to lockless readers */
	rcu_assign_pointer(fi->qlease, l);
}

/* Unlink @fi; returns the lease whose link reference the caller must put. */
static struct fuse_qlease *fuse_qlease_unlink_locked(struct fuse_inode *fi)
{
	struct fuse_qlease *l = fuse_qlease_linked(fi);

	if (!l)
		return NULL;
	list_del_init(&fi->qlease_node);
	/* a reader that still sees @l is covered by RCU (kfree_rcu()) */
	RCU_INIT_POINTER(fi->qlease, NULL);
	return l;
}

void fuse_qlease_inode_evict(struct fuse_inode *fi)
{
	struct fuse_conn *fc = get_fuse_conn(&fi->inode);
	struct fuse_qlease_mmap *m;
	struct fuse_qlease *l;

	/* set from the INIT reply, before any regular inode exists */
	if (!fc->quota_lease)
		return;
	/*
	 * Unlink first: the lease work queues unmap_work only for linked
	 * inodes, under qlease_lock, so it cannot be queued again after this.
	 * page_mkwrite unmaps inline and queues nothing.
	 */
	spin_lock(&fc->qlease_lock);
	l = fuse_qlease_unlink_locked(fi);
	spin_unlock(&fc->qlease_lock);
	m = fi->qlease_mmap;
	if (m) {
		cancel_work_sync(&m->unmap_work);
		fi->qlease_mmap = NULL;
		kfree(m);
	}
	if (l)
		fuse_qlease_put(l);
}

static bool fuse_qlease_expired(struct fuse_qlease *l)
{
	return !time_before(jiffies, l->deadline);
}

static bool fuse_qlease_rejected_now(struct fuse_qlease *l)
{
	return l->rejected && time_before(jiffies, l->reject_until);
}

/* The state the write fast path reads, as one snapshot */
struct fuse_qlease_snap {
	unsigned long	deadline;
	unsigned long	reject_until;
	bool		rejected;
};

/* Lockless: a seqcount read of the state written under l->lock */
static void fuse_qlease_snapshot(struct fuse_qlease *l,
				  struct fuse_qlease_snap *s)
{
	unsigned int seq;

	do {
		seq = read_seqcount_begin(&l->seq);
		s->deadline = l->deadline;
		s->reject_until = l->reject_until;
		s->rejected = l->rejected;
	} while (read_seqcount_retry(&l->seq, seq));
}

/* What the lockless check found, see fuse_qlease_check() */
enum fuse_qlease_state {
	FUSE_QLEASE_ST_VALID,
	FUSE_QLEASE_ST_NONE,		/* no lease, or the owner changed */
	FUSE_QLEASE_ST_REJECTED,	/* within its hold time */
	FUSE_QLEASE_ST_RENEW,		/* expired, or past its hold time */
};

/*
 * The lease rules for @fi without a lock, under rcu_read_lock(). A valid
 * lease is marked used when @use. *@lp is the lease found, which may be
 * used only until rcu_read_unlock() without a reference.
 */
static enum fuse_qlease_state fuse_qlease_state_rcu(struct fuse_inode *fi,
						      struct fuse_qlease **lp,
						      bool use)
{
	struct fuse_qlease_snap s;
	struct fuse_qlease *l;

	l = rcu_dereference(fi->qlease);
	*lp = l;
	if (!l)
		return FUSE_QLEASE_ST_NONE;
	/* pairs with rcu_assign_pointer() in fuse_qlease_link() */
	smp_rmb();
	if (!fuse_qlease_owner_matches(fi))
		return FUSE_QLEASE_ST_NONE;
	/*
	 * A relink A -> B may have written B's owner after we loaded A: the
	 * owner read above may then be B's. Read the pointer again after the
	 * owner (pairs with the smp_wmb() in fuse_qlease_link(), which orders
	 * the unlink's NULL store before the new owner); if it moved, the
	 * owner we compared may not be @l's, so treat it as no lease.
	 */
	smp_rmb();
	if (rcu_access_pointer(fi->qlease) != l)
		return FUSE_QLEASE_ST_NONE;
	fuse_qlease_snapshot(l, &s);
	if (s.rejected && time_before(jiffies, s.reject_until))
		return FUSE_QLEASE_ST_REJECTED;
	if (s.rejected || !time_before(jiffies, s.deadline))
		return FUSE_QLEASE_ST_RENEW;
	/*
	 * The renewal resets used under l->lock. A write whose set races that
	 * reset may go uncounted; a later write sets it again. If the writer
	 * then goes idle, the lease lapses at its deadline instead of being
	 * renewed in the background, and its next write pays one foreground
	 * renewal. Quota safety is unaffected: nothing is admitted without a
	 * valid lease.
	 */
	if (use && !READ_ONCE(l->used))
		WRITE_ONCE(l->used, true);
	return FUSE_QLEASE_ST_VALID;
}

/* Whether @fi has a valid lease, marking it used if @use. Takes no lock. */
static bool fuse_qlease_valid_use(struct fuse_inode *fi, bool use)
{
	struct fuse_qlease *l;
	bool valid;

	rcu_read_lock();
	valid = fuse_qlease_state_rcu(fi, &l, use) == FUSE_QLEASE_ST_VALID;
	rcu_read_unlock();
	return valid;
}

/*
 * Whether @fi has a valid lease; does not mark it used. Takes no lock: the
 * fault fast path calls it under mmap_lock.
 */
bool fuse_qlease_valid(struct fuse_inode *fi)
{
	return fuse_qlease_valid_use(fi, false);
}

/*
 * Install the state of lease part @ql in @l, whatever @l holds. Callers
 * order replies first: a new lease, fuse_qlease_reject_applies() for a
 * REJECTED part, and fuse_qlease_apply_renewal() for a renewal's reply.
 * Called with l->lock held.
 */
static void fuse_qlease_set_reply(struct fuse_qlease *l,
				   const struct fuse_quota_lease *ql)
{
	unsigned long ttl = msecs_to_jiffies(ql->ttl_ms > 0 ? ql->ttl_ms : 0);

	lockdep_assert_held(&l->lock);
	write_seqcount_begin(&l->seq);
	/* Never move the generation backwards */
	l->gen = max(l->gen, ql->gen);
	if (ql->flags & FUSE_QLEASE_REJECTED) {
		l->rejected = true;
		l->reject_until = jiffies +
				  msecs_to_jiffies(qlease_reject_hold_ms);
		l->deadline = jiffies;
	} else {
		l->rejected = false;
		l->deadline = jiffies + ttl;
		l->renew_at = jiffies + ttl / 2;
	}
	write_seqcount_end(&l->seq);
}

/*
 * The error of a lease request as the write sees it, for the error of a
 * lease part with no lease and no QUOTA_OFF and for a QUOTA_ADMIT request.
 * redfsd retries a lease request itself and never answers -EAGAIN, so
 * -EAGAIN is a protocol error. -EINTR answers the FUSE_INTERRUPT a signal
 * sent: the syscall restarts if the signal is still pending, and anything
 * else (a background request, which no signal interrupts) sees -EIO.
 * Anything that is not a plain negative errno is a protocol error, and so
 * is -ENOSYS: callers read it as "the server has no DLM, go ahead", which a
 * lease request must never mean.
 */
static int fuse_qlease_reply_error(int32_t error)
{
	if (error == -EINTR)
		return signal_pending(current) ? -ERESTARTSYS : -EIO;
	if (error >= 0 || error < -MAX_ERRNO || error == -ENOSYS ||
	    error == -EAGAIN)
		return -EIO;
	return error;
}

/*
 * Whether a REJECTED lease part may be applied to @l. A rejection older
 * than the reply that last renewed a valid lease is stale: a lock reply
 * that was in flight across a renewal must not reject the renewed lease,
 * and a renewal's reply must not reject a lease that a newer lock reply
 * extended.
 */
static bool fuse_qlease_reject_applies(struct fuse_qlease *l,
					const struct fuse_quota_lease *ql)
{
	lockdep_assert_held(&l->lock);
	return ql->gen > l->gen || l->rejected || fuse_qlease_expired(l);
}

/*
 * Step 3: apply the lease part of a write lock reply, or of the reply to a
 * first-admission FUSE_QUOTA_ADMIT, which has the same rules.
 * @wanted: the write found no valid lease at its grant check, so the reply
 * is its first admission. Returns 0 when the write may go ahead, -EDQUOT
 * while the lease is rejected, FUSE_QLEASE_RESTART when the write must
 * check its lease again (the lease linked is expired or being renewed), or
 * another error. A wanted part with no
 * lease maps its error as fuse_qlease_reply_error() does, so it never
 * returns -ENOSYS or a positive value. A REJECTED part older than the last
 * renewal of a valid lease is ignored (fuse_qlease_reject_applies()).
 */
int fuse_qlease_apply(struct inode *inode, const struct fuse_quota_lease *ql,
		       bool wanted)
{
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_qlease *fresh = NULL, *l, *cur, *old = NULL;
	int ret = 0;

	/* A QUOTA OFF session skips admission; its parts change nothing */
	if (!fc->quota_lease || fuse_qlease_quota_off(fc))
		return 0;

	trace_fuse_qlease_reply(inode, ql, wanted);

	/*
	 * Reserved on a quota enabled session (redfsd sends it only to a
	 * session that started QUOTA OFF): install and link nothing, change
	 * nothing, and let the write go ahead.
	 */
	if (ql->flags & FUSE_QLEASE_QUOTA_OFF)
		return 0;

	if (!ql->lease_id) {
		if (!wanted)
			return 0;
		return fuse_qlease_reply_error(ql->error);
	}
	/* a granted lease without a TTL is a protocol error */
	if (!(ql->flags & FUSE_QLEASE_REJECTED) && ql->ttl_ms <= 0)
		return wanted ? -EIO : 0;

	/*
	 * fc->qlease_lock for the hash and the link, then l->lock for the
	 * state under a reference of our own. Each is atomic on its own, which
	 * is all a lock reply needs: a check between them sees the state from
	 * before this reply, as it would have if it had run just before.
	 */
	spin_lock(&fc->qlease_lock);
	l = fuse_qlease_find(fc, ql->lease_id);
	if (!l) {
		/* Allocate only on a miss, outside the lock, then look again */
		spin_unlock(&fc->qlease_lock);
		fresh = kzalloc(sizeof(*fresh), GFP_KERNEL);
		if (!fresh)
			return wanted ? -ENOMEM : 0;
		spin_lock(&fc->qlease_lock);
		l = fuse_qlease_find(fc, ql->lease_id);
		/* if another reply installed it meanwhile, fresh is freed below */
	}

	cur = fuse_qlease_linked(fi);
	if (cur && cur != l) {
		/* redfsd sees another owner (D2: lease_id is the identity) */
		old = fuse_qlease_unlink_locked(fi);
		cur = NULL;	/* unlinked: a wanted part links @l below */
		if (!wanted) {
			/* A current rejection is applied; this write goes ahead */
			if (l && (ql->flags & FUSE_QLEASE_REJECTED))
				fuse_qlease_get(l);
			else
				l = NULL;
			spin_unlock(&fc->qlease_lock);
			if (l) {
				spin_lock(&l->lock);
				if (fuse_qlease_reject_applies(l, ql))
					fuse_qlease_set_reply(l, ql);
				spin_unlock(&l->lock);
				fuse_qlease_put(l);
			}
			goto out;
		}
	}

	if (!l) {
		/* A lease the mount does not hold yet: install it */
		l = fresh;
		fresh = NULL;
		l->fc = fc;
		l->lease_id = ql->lease_id;
		spin_lock_init(&l->lock);
		seqcount_spinlock_init(&l->seq, &l->lock);
		INIT_LIST_HEAD(&l->inodes);
		init_waitqueue_head(&l->waitq);
		INIT_DELAYED_WORK(&l->work, fuse_qlease_work);
		refcount_set(&l->ref, 1);	/* dropped below once linked */
		/* nobody sees it before hash_add(); the lock is for lockdep */
		spin_lock(&l->lock);
		fuse_qlease_set_reply(l, ql);
		spin_unlock(&l->lock);
		hash_add(fc->qlease_hash, &l->hnode, l->lease_id);
		fuse_qlease_link(fi, l);
		spin_lock(&l->lock);
		fuse_qlease_arm(l, l->rejected ? l->reject_until : l->renew_at);
		if (l->rejected)
			ret = -EDQUOT;
		trace_fuse_qlease_install(l);
		spin_unlock(&l->lock);
		refcount_dec(&l->ref);		/* link and work hold it now */
		spin_unlock(&fc->qlease_lock);
		goto out;
	}

	if (!cur) {
		fuse_qlease_link(fi, l);
	} else if (!fuse_qlease_owner_matches(fi)) {
		/* same lease, owner refreshed: record the current owner */
		WRITE_ONCE(fi->qlease_uid, inode->i_uid);
		WRITE_ONCE(fi->qlease_gid, inode->i_gid);
	}
	fuse_qlease_get(l);		/* fi may be unlinked once unlocked */
	spin_unlock(&fc->qlease_lock);

	spin_lock(&l->lock);
	if (ql->flags & FUSE_QLEASE_REJECTED) {
		if (fuse_qlease_reject_applies(l, ql))
			fuse_qlease_set_reply(l, ql);
	} else if (!l->rejected && !fuse_qlease_expired(l) &&
		   ql->gen > l->gen) {
		/* Extend a valid lease; never revive, never re-arm (D5) */
		write_seqcount_begin(&l->seq);
		l->gen = ql->gen;
		l->deadline = jiffies +
			      msecs_to_jiffies(ql->ttl_ms > 0 ? ql->ttl_ms : 0);
		write_seqcount_end(&l->seq);
		trace_fuse_qlease_extend(l);
	}

	if (fuse_qlease_rejected_now(l))
		ret = -EDQUOT;
	else if (wanted && (fuse_qlease_expired(l) || l->renewing))
		ret = FUSE_QLEASE_RESTART;
	else if (wanted && l->rejected)
		ret = FUSE_QLEASE_RESTART;	/* hold time passed: renew */
	spin_unlock(&l->lock);
	fuse_qlease_put(l);
out:
	if (old)
		fuse_qlease_put(old);
	kfree(fresh);
	return ret;
}

/**
 * fuse_qlease_check - the lease rules at the grant check of a write
 * @inode: the inode written, on a quota enabled session
 * @lp: where to return the lease to renew, with a reference the caller
 *	puts with fuse_qlease_put(); NULL to take none
 *
 * Called by fuse_get_dlm_lock() for a buffered write or a store fault,
 * with the range lock LOCKED when the caller passed one, before the grant
 * decides whether a request goes out. Sends nothing, never sleeps and takes
 * no lock: fi->qlease is read under RCU and the lease state with a seqcount
 * loop. Only the renew case takes a reference, with refcount_inc_not_zero();
 * a lease whose last reference is gone is treated as no lease.
 *
 * Return: 0 when the lease is valid (it is marked used), -EDQUOT while it
 * is rejected within its hold time, FUSE_QLEASE_NONE when the inode has
 * no lease or its owner changed, or FUSE_QLEASE_RENEW when the lease
 * expired or its hold time passed.
 */
int fuse_qlease_check(struct inode *inode, struct fuse_qlease **lp)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_qlease *l;
	int ret;

	rcu_read_lock();
	switch (fuse_qlease_state_rcu(fi, &l, true)) {
	case FUSE_QLEASE_ST_VALID:
		ret = 0;
		break;
	case FUSE_QLEASE_ST_REJECTED:
		ret = -EDQUOT;
		break;
	case FUSE_QLEASE_ST_RENEW:
		/* Expired, or rejected past its hold time: renew */
		ret = FUSE_QLEASE_RENEW;
		if (!lp)
			break;
		if (refcount_inc_not_zero(&l->ref))
			*lp = l;
		else
			ret = FUSE_QLEASE_NONE;	/* being freed */
		break;
	default:
		ret = FUSE_QLEASE_NONE;
		break;
	}
	rcu_read_unlock();
	return ret;
}

static int fuse_qlease_send_admit(struct inode *inode,
				   struct fuse_quota_lease *out,
				   bool background, uint32_t flags)
{
	struct fuse_mount *fm = get_fuse_mount(inode);
	struct fuse_quota_admit_in inarg = { .flags = flags };
	FUSE_ARGS(args);
	int err;

	memset(out, 0, sizeof(*out));
	args.opcode = FUSE_QUOTA_ADMIT;
	args.nodeid = get_node_id(inode);
	args.in_numargs = 1;
	args.in_args[0].size = sizeof(inarg);
	args.in_args[0].value = &inarg;
	args.out_numargs = 1;
	args.out_args[0].size = sizeof(*out);
	args.out_args[0].value = out;
	/*
	 * The background work has no caller credentials to send. A foreground
	 * writer sends its own and stays killable, like the rest of the write.
	 */
	args.force = background;
	args.nocreds = background;
	err = fuse_simple_request(fm, &args);
	/* -EAGAIN, -ENOSYS: -EIO; -EINTR: restart or -EIO */
	return err ? fuse_qlease_reply_error(err) : 0;
}

/**
 * fuse_qlease_first_admit - first admission of an inode with a cached grant
 * @inode: the inode written, which has no valid lease
 *
 * Sends FUSE_QUOTA_ADMIT without FUSE_QADMIT_RENEW and flushes nothing.
 * Called from fuse_get_dlm_lock() with the range lock in INIT. Not
 * single-flight: the inode has no lease entry to wait on yet. The reply is
 * applied as the lease part of a lock reply is (fuse_qlease_apply()), so a
 * rejected part installs or links the lease in REJECTED and the write fails
 * with -EDQUOT, and a part that names an expired lease links the inode and
 * returns FUSE_QLEASE_RESTART, after which the grant check renews it.
 *
 * Return: what fuse_qlease_apply() returns, or the request error. -EIO for
 * a reply with no lease and no QUOTA_OFF, which a dedicated admit never
 * sends.
 */
int fuse_qlease_first_admit(struct inode *inode)
{
	struct fuse_quota_lease ql;
	int err;

	trace_fuse_qlease_first_admit(inode);
	err = fuse_qlease_send_admit(inode, &ql, false, 0);
	if (err)
		return err;
	if (!ql.lease_id && !(ql.flags & FUSE_QLEASE_QUOTA_OFF))
		return -EIO;
	return fuse_qlease_apply(inode, &ql, true);
}

static void fuse_qlease_put_inodes(struct fuse_qlease_iputs *b)
{
	unsigned int i;

	for (i = 0; i < b->n; i++)
		iput(b->inodes[i]);
	kvfree(b);
}

static void fuse_qlease_iput_work(struct work_struct *work)
{
	struct fuse_conn *fc = container_of(work, struct fuse_conn,
					     qlease_iput_work);
	struct fuse_qlease_iputs *b, *tmp;
	struct llist_node *list = llist_del_all(&fc->qlease_iputs);

	llist_for_each_entry_safe(b, tmp, list, node)
		fuse_qlease_put_inodes(b);
}

/*
 * Start writeback of the linked inodes whose mapping is tagged dirty (D6).
 * Never waits for it. The caller holds a reference on @l. A foreground
 * renewer also holds its inode lock (a fault holds none) with its range lock
 * in INIT, and a fault without FAULT_FLAG_ALLOW_RETRY holds mmap_lock:
 * writeback takes none of them.
 *
 * Returns the inode references taken, or NULL. They are not dropped here:
 * the last one of an unlinked inode evicts it, which waits for the writeback
 * just started. The caller drops them once its admit has returned, with no
 * lock held (fuse_qlease_renew()).
 */
static struct fuse_qlease_iputs *fuse_qlease_flush(struct fuse_qlease *l)
{
	struct fuse_conn *fc = l->fc;
	struct fuse_qlease_iputs *batch = NULL;
	struct fuse_inode *fi;
	unsigned int n = 0, cap = 0, i;

	spin_lock(&fc->qlease_lock);
	list_for_each_entry(fi, &l->inodes, qlease_node)
		if (mapping_tagged(fi->inode.i_mapping, PAGECACHE_TAG_DIRTY))
			cap++;
	spin_unlock(&fc->qlease_lock);
	if (!cap)
		goto out;
	batch = kvmalloc(struct_size(batch, inodes, cap), GFP_KERNEL);
	if (!batch)
		goto out;

	spin_lock(&fc->qlease_lock);
	list_for_each_entry(fi, &l->inodes, qlease_node) {
		struct inode *inode = &fi->inode;

		if (n == cap)
			break;
		if (!mapping_tagged(inode->i_mapping, PAGECACHE_TAG_DIRTY))
			continue;
		inode = igrab(inode);	/* NULL while being evicted */
		if (inode)
			batch->inodes[n++] = inode;
	}
	spin_unlock(&fc->qlease_lock);
	batch->n = n;

	for (i = 0; i < n; i++)
		filemap_flush(batch->inodes[i]->i_mapping); /* WB_SYNC_NONE */
	if (!n) {
		kvfree(batch);
		batch = NULL;
	}
out:
	trace_fuse_qlease_flush(l, n);
	return batch;
}

/*
 * Apply the admit reply of a renewal of @l, with the ordering rules of
 * fuse_qlease_apply(). Lock replies and first-admission admits are not
 * serialized with the renewal, so a reply built after the renewal's may
 * have been applied while it was in flight. Called with l->lock held.
 *
 * A REJECTED part is applied only when fuse_qlease_reject_applies(): it
 * never rejects a lease that a newer reply renewed. A granted part installs
 * its deadline and renew_at when it is newer than the installed state, or
 * when the lease is not valid (expired, or rejected past its hold time):
 * that revives the lease, which only a renewal may do.
 *
 * Otherwise the newer state is kept: a newer rejection within its hold time
 * stays, and a valid lease keeps the deadline a newer lock reply gave it.
 * The renewal still counts for that valid lease (its flush ran and redfsd
 * admitted it), so renew_at moves to half the time left.
 *
 * Afterwards the lease is either valid or rejected. Called with l->lock
 * held.
 */
static void fuse_qlease_apply_renewal(struct fuse_qlease *l,
				       const struct fuse_quota_lease *ql)
{
	bool install;

	lockdep_assert_held(&l->lock);
	if (ql->flags & FUSE_QLEASE_REJECTED)
		install = fuse_qlease_reject_applies(l, ql);
	else
		install = ql->gen > l->gen ||
			  ((l->rejected || fuse_qlease_expired(l)) &&
			   !fuse_qlease_rejected_now(l));
	if (install) {
		fuse_qlease_set_reply(l, ql);
	} else if (!l->rejected) {
		write_seqcount_begin(&l->seq);
		l->renew_at = jiffies + (l->deadline - jiffies) / 2;
		write_seqcount_end(&l->seq);
	}
	trace_fuse_qlease_renew_reply(l, ql, install);
}

/*
 * Renew @l through @via, an inode linked to it that the caller holds a
 * reference on, as does it on @l. Single-flight: a foreground caller that
 * finds a renewal in flight sleeps killably until it finished and returns
 * FUSE_QLEASE_WAITED; a background caller returns 0 and leaves it to the
 * renewer.
 *
 * The reply is applied by fuse_qlease_apply_renewal(), under the generation
 * rules of a lock reply: a reply older than one applied while the renewal
 * was in flight neither moves the deadline back nor rejects a valid lease.
 * The work is armed, and the result chosen, from the resulting lease state:
 * renew_at for a valid lease, reject_until for a rejected one. A -EDQUOT
 * request error carries no generation and always moves the lease to REJECTED.
 *
 * Returns 0 (the lease is valid after the reply, or the reply carried
 * QUOTA_OFF and the lease was left unchanged and unarmed),
 * FUSE_QLEASE_WAITED (another renewer finished), FUSE_QLEASE_RESTART (@via
 * changed owner and was unlinked), -EDQUOT (the lease is rejected after the
 * reply, or the request failed with -EDQUOT), -ERESTARTSYS or the request
 * error.
 *
 * A foreground renewer calls this from the grant check of a write or a store
 * fault, with its range lock in INIT. The admit carries FUSE_QADMIT_RENEW
 * either way. The flush's inode references are kept until the admit has
 * returned: the background work then drops them here, and a foreground
 * renewer hands them to fc->qlease_iput_work, which drops them holding no
 * lock.
 */
int fuse_qlease_renew(struct fuse_qlease *l, struct inode *via,
		       bool background)
{
	struct fuse_conn *fc = l->fc;
	struct fuse_qlease_iputs *batch;
	struct fuse_quota_lease ql;
	int err;

	spin_lock(&l->lock);
	if (l->renewing) {
		spin_unlock(&l->lock);
		if (background)
			return 0;
		err = wait_event_killable(l->waitq, !READ_ONCE(l->renewing));
		return err ? err : FUSE_QLEASE_WAITED;
	}
	write_seqcount_begin(&l->seq);
	WRITE_ONCE(l->renewing, true);
	/* writes admitted from here on count toward the next renewal */
	WRITE_ONCE(l->used, false);
	write_seqcount_end(&l->seq);
	spin_unlock(&l->lock);

	batch = fuse_qlease_flush(l);
	err = fuse_qlease_send_admit(via, &ql, background,
				      FUSE_QADMIT_RENEW);
	/*
	 * A dedicated admit never answers lease_id 0 without an error or
	 * QUOTA_OFF, and a granted lease always has a TTL.
	 */
	if (!err && !(ql.flags & FUSE_QLEASE_QUOTA_OFF) &&
	    (!ql.lease_id ||
	     (!(ql.flags & FUSE_QLEASE_REJECTED) && ql.ttl_ms <= 0)))
		err = -EIO;
	trace_fuse_qlease_renew(l, background, err, err ? 0 : ql.flags);

	if (!err && !(ql.flags & FUSE_QLEASE_QUOTA_OFF) &&
	    ql.lease_id != l->lease_id) {
		/*
		 * via was chowned since it linked: unlink it, let it relink.
		 * A link change, so fc->qlease_lock; the lease state is left
		 * alone and renewing is cleared below like on every path.
		 */
		struct fuse_inode *vfi = get_fuse_inode(via);

		spin_lock(&fc->qlease_lock);
		if (fuse_qlease_linked(vfi) == l) {
			fuse_qlease_unlink_locked(vfi);
			refcount_dec(&l->ref);	/* caller still holds one */
		}
		spin_unlock(&fc->qlease_lock);
		err = FUSE_QLEASE_RESTART;
	}

	spin_lock(&l->lock);
	if (!err && (ql.flags & FUSE_QLEASE_QUOTA_OFF)) {
		/*
		 * Reserved here (redfsd sends it only to a session that
		 * started QUOTA OFF, which never renews): leave the lease's
		 * deadline and state alone and arm nothing. The foreground
		 * writer goes ahead; the background work exits.
		 */
	} else if (err == FUSE_QLEASE_RESTART) {
		/* via was unlinked above */
	} else if (!err) {
		fuse_qlease_apply_renewal(l, &ql);
		/* arm and answer from the resulting state, not the reply */
		if (l->rejected) {
			fuse_qlease_arm(l, l->reject_until);
			err = -EDQUOT;
		} else {
			fuse_qlease_arm(l, l->renew_at);
		}
	} else if (err == -EDQUOT) {
		/*
		 * No lease part, so no generation to order it by. It answers
		 * the newest request this renewer sent, after a voucher
		 * refresh, and a rejection only shortens admission: apply it.
		 */
		write_seqcount_begin(&l->seq);
		l->rejected = true;
		l->reject_until = jiffies +
				  msecs_to_jiffies(qlease_reject_hold_ms);
		write_seqcount_end(&l->seq);
		fuse_qlease_arm(l, l->reject_until);
	} else {
		/*
		 * Any other error leaves the lease to expire. A background
		 * renewal that found this one in flight queued nothing, so
		 * arm the work for the expired path (unmap).
		 */
		fuse_qlease_arm(l, l->deadline);
	}
	write_seqcount_begin(&l->seq);
	WRITE_ONCE(l->renewing, false);
	write_seqcount_end(&l->seq);
	spin_unlock(&l->lock);
	wake_up_all(&l->waitq);

	if (batch && background) {
		fuse_qlease_put_inodes(batch);
	} else if (batch) {
		llist_add(&batch->node, &fc->qlease_iputs);
		queue_work(system_unbound_wq, &fc->qlease_iput_work);
	}
	return err;
}

/* Pick one linked inode that is not bad to send the admit for; NULL if none */
static struct inode *fuse_qlease_pick_inode(struct fuse_qlease *l)
{
	struct fuse_inode *fi;
	struct inode *inode = NULL;

	spin_lock(&l->fc->qlease_lock);
	list_for_each_entry(fi, &l->inodes, qlease_node) {
		/* a stale inode's nodeid may already name another file */
		if (fuse_is_bad(&fi->inode))
			continue;
		inode = igrab(&fi->inode);
		if (inode)
			break;
	}
	spin_unlock(&l->fc->qlease_lock);
	return inode;
}

/*
 * Add pages @first..@last to the inode's unmap range and queue the work.
 * m->lock is a leaf: the lease work calls this under fc->qlease_lock.
 */
static void fuse_qlease_queue_unmap(struct fuse_qlease_mmap *m,
				     pgoff_t first, pgoff_t last)
{
	spin_lock(&m->lock);
	m->unmap_first = min(m->unmap_first, first);
	m->unmap_last = max(m->unmap_last, last);
	spin_unlock(&m->lock);
	queue_work(system_unbound_wq, &m->unmap_work);
}

/*
 * Unmap every linked inode that is mapped shared-writable. One without mmap
 * state never was, so it has no PTE to clear. fc->qlease_lock keeps each
 * inode linked, and so not evicted, while its work is queued.
 */
static void fuse_qlease_unmap_linked(struct fuse_qlease *l)
{
	struct fuse_qlease_mmap *m;
	struct fuse_inode *fi;

	spin_lock(&l->fc->qlease_lock);
	list_for_each_entry(fi, &l->inodes, qlease_node) {
		m = fuse_qlease_mmap(fi);
		if (m && mapping_writably_mapped(fi->inode.i_mapping))
			fuse_qlease_queue_unmap(m, 0, ULONG_MAX);
	}
	spin_unlock(&l->fc->qlease_lock);
}

/*
 * Queued by the lease work for a linked inode. Eviction cancels it before
 * it frees the mmap state, after unlinking (so the lease work cannot queue
 * it again), so the inode and its mmap state outlive it.
 */
static void fuse_qlease_unmap_work(struct work_struct *work)
{
	struct fuse_qlease_mmap *m = container_of(work,
						   struct fuse_qlease_mmap,
						   unmap_work);
	struct fuse_inode *fi = m->fi;
	pgoff_t first, last;

	spin_lock(&m->lock);
	first = m->unmap_first;
	last = m->unmap_last;
	m->unmap_first = ULONG_MAX;
	m->unmap_last = 0;
	spin_unlock(&m->lock);
	if (first > last)
		return;
	/*
	 * Clear the PTEs so the next store faults through filemap_fault.
	 * For the whole file nr wraps to 0, which unmap_mapping_pages() takes
	 * as "to the end".
	 */
	unmap_mapping_pages(fi->inode.i_mapping, first, last - first + 1,
			    false);
}

/*
 * Fires at renew_at after a renewal or install, at the deadline of an idle
 * lease, or at reject_until. Holds the queued work's reference. A lease with
 * no linked inode is left alone: none is left to renew or unmap, and the
 * superblock may be gone.
 */
static void fuse_qlease_work(struct work_struct *work)
{
	struct fuse_qlease *l = container_of(to_delayed_work(work),
					      struct fuse_qlease, work);
	struct fuse_conn *fc = l->fc;
	bool renew, expired;
	int tries;

	if (!READ_ONCE(fc->connected))
		goto out;

	/* qlease_lock for l->inodes, l->lock for the state (that order) */
	spin_lock(&fc->qlease_lock);
	spin_lock(&l->lock);
	/*
	 * Defensive: a QUOTA OFF session never installs a lease, so no work
	 * runs for one. If it did, it would not renew, flush, unmap or re-arm.
	 */
	if (l->dying || list_empty(&l->inodes) || fuse_qlease_quota_off(fc)) {
		spin_unlock(&l->lock);
		spin_unlock(&fc->qlease_lock);
		goto out;
	}
	expired = fuse_qlease_expired(l);
	renew = READ_ONCE(l->used) && !expired && !l->rejected;
	if (!renew && !expired && !l->rejected) {
		/* idle, or the deadline moved by a lock reply: wait for it */
		fuse_qlease_arm(l, l->deadline);
		spin_unlock(&l->lock);
		spin_unlock(&fc->qlease_lock);
		goto out;
	}
	spin_unlock(&l->lock);
	spin_unlock(&fc->qlease_lock);

	for (tries = 0; renew && tries < 4; tries++) {
		struct inode *via = fuse_qlease_pick_inode(l);
		int err;

		if (!via)
			break;
		err = fuse_qlease_renew(l, via, true);
		iput(via);
		if (err != FUSE_QLEASE_RESTART) {
			if (!err)
				goto out;
			break;
		}
	}

	/* Idle and expired, rejected, or the renewal failed */
	trace_fuse_qlease_expire(l);
	fuse_qlease_unmap_linked(l);
out:
	fuse_qlease_put(l);	/* the queued work's reference */
}

/*
 * Record an admit error met by a fault that runs with mmap_lock dropped,
 * where it can only return VM_FAULT_RETRY; the retried fault, which holds
 * mmap_lock again, takes the mark and returns VM_FAULT_SIGBUS. -EDQUOT
 * marks at once. Any other error marks only once it has persisted for
 * qlease_mmap_retry_ms; until then the fault keeps being retried.
 */
static void fuse_qlease_set_mark(struct fuse_qlease_mmap *m, int err)
{
	WRITE_ONCE(m->err_at, jiffies);
	/* pairs with the xchg() in fuse_qlease_take_mark() */
	smp_wmb();
	WRITE_ONCE(m->err, err);
}

/* No mmap state: no shared-writable mapping to fault, nothing to record */
void fuse_qlease_mark(struct fuse_inode *fi, int err)
{
	struct fuse_qlease_mmap *m = fuse_qlease_mmap(fi);
	unsigned long since;

	if (!m)
		return;
	if (err == -EDQUOT) {
		fuse_qlease_set_mark(m, -EDQUOT);
		return;
	}
	since = READ_ONCE(m->err_since);
	if (!since)
		WRITE_ONCE(m->err_since, jiffies ?: 1);
	else if (time_after(jiffies, since +
			    msecs_to_jiffies(fuse_qlease_mmap_retry_ms)))
		fuse_qlease_set_mark(m, err);
}

/*
 * An admission went through: a mark not yet taken is stale, since the
 * retried fault would be admitted now too, and a later error starts its
 * retry time anew.
 */
void fuse_qlease_mark_clear(struct fuse_inode *fi)
{
	struct fuse_qlease_mmap *m = fuse_qlease_mmap(fi);

	if (!m)
		return;
	if (READ_ONCE(m->err))
		WRITE_ONCE(m->err, 0);
	if (READ_ONCE(m->err_since))
		WRITE_ONCE(m->err_since, 0);
}

/*
 * Under mmap_lock: a recorded mark becomes one SIGBUS. Only the retried
 * fault is meant to meet it, which comes right back; a mark nobody took
 * (the task was killed, the range unmapped) must not hit an unrelated
 * store later, so one older than a second is dropped. An -EDQUOT mark
 * counts only while the lease is still rejected. Taking a mark, used or
 * not, ends its episode: the next error starts a new retry window. No
 * mmap state means no mark.
 */
bool fuse_qlease_take_mark(struct fuse_inode *fi)
{
	struct fuse_qlease_mmap *m = fuse_qlease_mmap(fi);
	struct fuse_qlease *l;
	int err;

	if (!m || !READ_ONCE(m->err))
		return false;
	err = xchg(&m->err, 0);	/* full barrier */
	if (!err)
		return false;
	WRITE_ONCE(m->err_since, 0);
	if (time_after(jiffies, READ_ONCE(m->err_at) + HZ))
		return false;
	if (err != -EDQUOT)
		return true;
	/* no lease linked: the server gave none, nothing to check */
	rcu_read_lock();
	l = rcu_dereference(fi->qlease);
	if (l) {
		struct fuse_qlease_snap snap;

		fuse_qlease_snapshot(l, &snap);
		if (!snap.rejected)
			err = 0;
	}
	rcu_read_unlock();
	return err != 0;
}

/*
 * The writeback-cache page_mkwrite, under mmap_lock (or the per-VMA lock),
 * after the WRITE grant was verified, with the folio not locked yet. Never
 * sends and takes no lease lock: with a valid lease the page may be dirtied;
 * otherwise the page is unmapped here, so that the store refaults through
 * fuse_wb_filemap_fault(), which admits with mmap_lock dropped. The unmap may
 * sleep on i_mmap_rwsem, which nests inside mmap_lock as in any fault, and
 * waits on nothing else. Returns 0, VM_FAULT_SIGBUS or VM_FAULT_NOPAGE.
 */
vm_fault_t fuse_qlease_page_mkwrite(struct inode *inode, pgoff_t index)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_qlease_mmap *m;

	if (!fc->quota_lease)
		return 0;
	if (fuse_qlease_quota_off(fc))
		return 0;

	if (fuse_qlease_valid_use(fi, true)) {
		fuse_qlease_mark_clear(fi);
		return 0;
	}

	if (fuse_qlease_take_mark(fi))
		return VM_FAULT_SIGBUS;
	/*
	 * fuse_wb_mmap() allocated the mmap state for the shared-writable
	 * mapping this store came through, so it cannot be missing here; if
	 * it were, the mark above would never be recorded for this inode.
	 */
	m = fuse_qlease_mmap(fi);
	if (WARN_ON_ONCE(!m))
		return VM_FAULT_SIGBUS;
	/*
	 * The PTE stays read-only after NOPAGE, so until it is gone this store
	 * comes back here. Clear it now rather than from the unmap work: a
	 * store would otherwise spin through page_mkwrite under mmap_lock
	 * until the workqueue ran. Shared mappings only; a private COW copy
	 * is not the file's page.
	 */
	unmap_mapping_pages(inode->i_mapping, index, 1, false);
	return VM_FAULT_NOPAGE;
}
