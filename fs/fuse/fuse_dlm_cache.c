// SPDX-License-Identifier: GPL-2.0-only
/*
 * FUSE page lock cache implementation
 *
 * The shards record the grants the server has given this client, a bit
 * per page.  A grant still on the wire covers nothing and must not
 * appear there, but a revoke has to be able to find it: otherwise a
 * revoke processed before the grant is recorded removes nothing, and the
 * grant recorded afterwards is never taken back.  A request in flight
 * therefore waits on cache->pending, where a revoke marks it killed and
 * fuse_dlm_request_commit() drops the grant instead of recording it.
 *
 * Keeping requests out of the shards leaves every walker looking at
 * grants alone.
 *
 * The record says nothing about the page cache under a page.  What is
 * cached there, and whether the server has seen it, is what the page
 * cache itself answers.  A revoked grant is therefore forgotten, not
 * kept: writeback holds the range again for every run it sends, and an
 * absent record and a revoked one both make it ask.
 */
#include "fuse_i.h"
#include "fuse_dlm_cache.h"

#include <linux/bitmap.h>
#include <linux/list.h>
#include <linux/sched/signal.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>


/*
 * How often to ask again for a grant a revoke killed while it was in
 * flight, or the server refused as contended, before giving up on the
 * range.  Each pass is a round trip.
 */
#define FUSE_DLM_GRANT_RETRIES 16

/*
 * How far beyond the requested range a grant is recorded.
 *
 * A server may grant more than was asked for, and recording the extra is
 * what lets the writes that follow skip the round trip entirely.  But
 * coverage is kept per shard, so the number of records a grant creates
 * grows with its size, and outarg is the server's to choose: an
 * unbounded grant would be an unbounded amount of work here.  Cap it.
 * Recording less than the server gave is safe - it only costs a
 * re-request - and a cap this size still covers thousands of writes.
 */
#define FUSE_DLM_MAX_EXTRA_GRANT (1ULL << 30)

/* A FUSE_DLM_WB_LOCK request in flight, on cache->pending */
struct fuse_dlm_range {
	/* The range asked for, as byte offsets, both inclusive */
	uint64_t start;
	uint64_t end;
	/* A revoke overlapped this request in flight */
	bool killed;
	/* The cache->pending link */
	struct list_head list;
};

/*
 * Bit of the page at @off within its shard.  Callers pass page aligned
 * bounds: a grant is aligned in __fuse_get_dlm_lock(), a revoke in
 * fuse_dlm_revoke_inval_range() and a query in fuse_dlm_lock_is_held().
 */
static unsigned long fuse_dlm_bit(uint64_t off)
{
	return (off & (FUSE_DLM_SHARD_SIZE - 1)) >> PAGE_SHIFT;
}

/**
 * fuse_dlm_shard_get - the shard covering @off, created if there is none
 * @cache: the page cache
 * @off: byte offset the caller is about to record a grant at
 *
 * Only the recording path needs a shard to exist; readers and the revoke
 * paths treat a missing one as a region holding no grant.
 *
 * Return: the shard, or NULL if it could not be allocated.
 */
static struct fuse_dlm_shard *fuse_dlm_shard_get(struct fuse_dlm_cache *cache,
						 uint64_t off)
{
	unsigned long idx = off >> FUSE_DLM_SHARD_SHIFT;
	struct fuse_dlm_shard *shard, *old;

	shard = xa_load(&cache->shards, idx);
	if (shard)
		return shard;

	shard = kzalloc(sizeof(*shard), GFP_NOFS);
	if (!shard)
		return NULL;

	/*
	 * Two recorders can reach the same empty region at once; the loser
	 * drops its shard and takes the winner's.
	 */
	old = xa_cmpxchg(&cache->shards, idx, NULL, shard, GFP_NOFS);
	if (old) {
		kfree(shard);
		/* xa_cmpxchg() returns an errno as an internal entry */
		return xa_is_err(old) ? NULL : old;
	}

	return shard;
}

/**
 * fuse_dlm_kill_pending - mark in-flight requests overlapping [start, end]
 * @cache: The page cache
 * @start: Start byte offset of the revoked region
 * @end: End byte offset of the revoked region
 *
 * A revoke overlapping a request still on the wire has nothing to remove
 * from the tree, since that grant is not recorded yet.  Marking it makes
 * fuse_dlm_request_commit() drop the grant instead of recording it.
 *
 * The nodes are owned by the threads waiting on their replies: mark
 * only, never remove or free.
 *
 * Takes @cache->pending_lock.  A request published after this returns is
 * one whose FUSE_DLM_WB_LOCK had not been sent when the revoke was
 * processed, so the grant it goes on to receive answers a request made
 * after the revoke and is recorded, not killed.
 */
static void fuse_dlm_kill_pending(struct fuse_dlm_cache *cache,
				  uint64_t start, uint64_t end)
{
	struct fuse_dlm_range *req;

	spin_lock(&cache->pending_lock);
	list_for_each_entry(req, &cache->pending, list)
		if (req->start <= end && start <= req->end)
			req->killed = true;
	spin_unlock(&cache->pending_lock);
}

/**
 * fuse_dlm_cache_init - Initialize a page cache lock manager
 * @inode: The fuse inode to initialize the cache of
 */
void fuse_dlm_cache_init(struct fuse_inode *inode)
{
	struct fuse_dlm_cache *cache = &inode->dlm_locked_areas;

	init_rwsem(&cache->lock);
	xa_init(&cache->shards);
	spin_lock_init(&cache->pending_lock);
	INIT_LIST_HEAD(&cache->pending);
}

/**
 * fuse_dlm_cache_release_locks - Clean up a page cache lock manager
 * @inode: The fuse inode to clean up the cache of
 *
 * Release all locks and free all resources associated with the cache.
 */
void fuse_dlm_cache_release_locks(struct fuse_inode *inode)
{
	struct fuse_dlm_cache *cache = &inode->dlm_locked_areas;
	struct fuse_dlm_shard *shard;
	unsigned long idx;

	/*
	 * Coverage goes away here, so the whole cache is taken for write:
	 * see the locking comment on struct fuse_dlm_cache.
	 */
	down_write(&cache->lock);
	/*
	 * Every grant goes, so every request in flight is revoked.  Mark
	 * only; each node is owned by the thread waiting on its reply.
	 */
	fuse_dlm_kill_pending(cache, 0, U64_MAX);

	xa_for_each(&cache->shards, idx, shard)
		kfree(shard);
	/*
	 * Leaves the xarray empty and usable: an inode is released again
	 * on every O_TRUNC open, not only on eviction.
	 */
	xa_destroy(&cache->shards);
	up_write(&cache->lock);
}

/**
 * fuse_dlm_shard_record - record a grant over [@start, @end] in @shard
 * @shard: the shard covering [@start, @end]
 * @start: start byte offset (inclusive)
 * @end: end byte offset (inclusive)
 * @mode: the mode it was granted in
 *
 * A write grant sets both maps, so a read query is answered by @granted
 * alone, and a write grant over a page already held for read upgrades it
 * by setting the bit the read grant left clear.
 *
 * @granted is set before @write, so a query racing this sees the page
 * covered for read before it sees it covered for write.  Either order is
 * safe, a bit not yet seen only costing a re-request of a range already
 * held, but this one never reports a write grant the read map does not
 * back.
 *
 * [@start, @end] must be page aligned and lie wholly inside @shard.
 * Caller holds fuse_dlm_cache.lock for read.
 */
static void fuse_dlm_shard_record(struct fuse_dlm_shard *shard, uint64_t start,
				  uint64_t end, enum fuse_page_lock_mode mode)
{
	unsigned long bit = fuse_dlm_bit(start);
	unsigned long last = fuse_dlm_bit(end);

	for (; bit <= last; bit++) {
		set_bit(bit, shard->granted);
		if (mode == FUSE_PAGE_LOCK_WRITE)
			set_bit(bit, shard->write);
	}
}

/**
 * fuse_dlm_record_grant - record a grant across the shards it spans
 * @cache: the page cache
 * @start: start byte offset the server granted (inclusive)
 * @end: end byte offset the server granted (inclusive)
 * @mode: the mode it was granted in
 *
 * Each shard is filled in on its own: coverage only grows here, and a
 * walker under @cache->lock held for read may see part of the grant
 * before the rest, which makes it ask again for a range it already has
 * rather than trust one it has not got.
 *
 * Caller holds @cache->lock for read.
 *
 * Return: 0 on success, negative error code on failure.  A failure part
 * way through leaves the shards already done recorded, which under
 * reports the grant and is safe.
 */
static int fuse_dlm_record_grant(struct fuse_dlm_cache *cache, uint64_t start,
				 uint64_t end, enum fuse_page_lock_mode mode)
{
	unsigned long idx, last_idx;

	if (start > end)
		return -EINVAL;

	last_idx = end >> FUSE_DLM_SHARD_SHIFT;

	for (idx = start >> FUSE_DLM_SHARD_SHIFT; idx <= last_idx; idx++) {
		struct fuse_dlm_shard *shard;
		uint64_t lo = max(start, FUSE_DLM_SHARD_FIRST(idx));
		uint64_t hi = min(end, FUSE_DLM_SHARD_LAST(idx));

		shard = fuse_dlm_shard_get(cache, lo);
		if (!shard)
			return -ENOMEM;

		fuse_dlm_shard_record(shard, lo, hi, mode);
	}

	return 0;
}

/**
 * fuse_dlm_request_begin - publish a lock request before it is sent
 * @inode: the fuse inode
 * @req: caller-owned storage for the request, live until commit or abort
 * @start: start byte offset being requested (inclusive)
 * @end: end byte offset being requested (inclusive)
 *
 * The mode is not recorded here: until the server answers the range is
 * held in neither, and the mode that reaches the shards is the one
 * passed to fuse_dlm_request_commit().
 *
 * A FUSE_DLM_WB_LOCK reply and a NOTIFY revoke are serviced on different
 * threads, so a revoke can be processed before the grant the reply
 * carries is recorded.  Publishing the request before it leaves gives
 * that revoke a node to mark; without one it removes nothing, and the
 * grant recorded afterwards is never taken back by any later NOTIFY.
 *
 * The request covers nothing while in flight, so it is kept out of the
 * shards.  @req is reachable only through cache->pending, which both
 * fuse_dlm_request_commit() and fuse_dlm_request_abort() unlink before
 * the caller returns; stack storage is therefore fine and nothing is
 * allocated here.
 *
 * Publishing touches the pending list and nothing else, so it takes
 * @cache->pending_lock alone.  It deliberately does not take
 * @cache->lock: this runs on every cached write that is not already
 * covered, and taking the cache rwsem for write here made every writer
 * of a file queue behind every other one before its request had even
 * been sent.
 */
void fuse_dlm_request_begin(struct fuse_inode *inode,
			    struct fuse_dlm_range *req, uint64_t start,
			    uint64_t end)
{
	struct fuse_dlm_cache *cache = &inode->dlm_locked_areas;

	req->start = start;
	req->end = end;
	req->killed = false;

	spin_lock(&cache->pending_lock);
	list_add_tail(&req->list, &cache->pending);
	spin_unlock(&cache->pending_lock);
}

/**
 * fuse_dlm_request_commit - retire a request and record its grant
 * @inode: the fuse inode
 * @req: the request published by fuse_dlm_request_begin()
 * @start: start byte offset the server granted (inclusive)
 * @end: end byte offset the server granted (inclusive)
 * @mode: the mode that was requested
 *
 * Unlinking @req and recording the grant are one step under
 * @cache->lock held for read, so a revoke - which takes it for write -
 * lands either before it and is seen on @req, or after it and finds the
 * grant in the tree.
 *
 * @req is retired in every case and may be reused.
 *
 * Return: -EAGAIN if a revoke overlapped @req while it was in flight,
 * nothing recorded; otherwise the result of recording the grant.
 */
int fuse_dlm_request_commit(struct fuse_inode *inode,
			    struct fuse_dlm_range *req, uint64_t start,
			    uint64_t end, enum fuse_page_lock_mode mode)
{
	struct fuse_dlm_cache *cache = &inode->dlm_locked_areas;
	bool revoked;
	int ret = 0;

	/*
	 * Read, not write: recording adds coverage.  Holding it across the
	 * unlink and the record is what keeps a revoke from landing
	 * between them, since the revoke paths take it for write.
	 */
	down_read(&cache->lock);

	spin_lock(&cache->pending_lock);
	list_del(&req->list);
	revoked = req->killed;
	spin_unlock(&cache->pending_lock);

	if (!revoked)
		ret = fuse_dlm_record_grant(cache, start, end, mode);
	up_read(&cache->lock);

	return revoked ? -EAGAIN : ret;
}

/**
 * fuse_dlm_request_abort - retire a request that got no usable reply
 * @inode: the fuse inode
 * @req: the request published by fuse_dlm_request_begin()
 *
 * Nothing is recorded, so a mark left by a revoke does not matter.
 */
void fuse_dlm_request_abort(struct fuse_inode *inode,
			    struct fuse_dlm_range *req)
{
	struct fuse_dlm_cache *cache = &inode->dlm_locked_areas;

	spin_lock(&cache->pending_lock);
	list_del(&req->list);
	spin_unlock(&cache->pending_lock);
}

/**
 * fuse_dlm_unlock_range - Revoke the grants over a range of pages
 * @inode: The fuse inode
 * @start: Start byte offset
 * @end: End byte offset
 *
 * The server has taken [start, end] back, so the grants over it are
 * removed and the IO paths ask again.  Page cache dirtied under a grant
 * that has gone is not lost by this: writeback takes the range again for
 * every run it sends, and a range it finds unrecorded is a range it asks
 * for.
 *
 * An inverted range is rejected rather than silently revoking nothing:
 * the callers revoke coverage, and a revoke that quietly keeps the grant
 * alive would let the re-validating IO paths trust a lock the server has
 * taken away.  To drop every grant use fuse_dlm_cache_release_locks()
 * (there is no in-band sentinel range for it).
 *
 * Return: 0 on success, negative error code on failure
 */
int fuse_dlm_unlock_range(struct fuse_inode *inode, uint64_t start,
			  uint64_t end)
{
	struct fuse_dlm_cache *cache = &inode->dlm_locked_areas;
	struct fuse_dlm_shard *shard;
	unsigned long idx;

	if (start > end)
		return -EINVAL;

	/*
	 * Write, not read: this is a path that takes coverage away, and
	 * the shard walkers rely on that never happening under them.  See
	 * the locking comment on struct fuse_dlm_cache.
	 */
	down_write(&cache->lock);

	/*
	 * Before touching any map, and even when nothing is covered: a
	 * revoke racing an in-flight grant finds nothing set, because that
	 * grant is not recorded yet.
	 */
	fuse_dlm_kill_pending(cache, start, end);

	/*
	 * Only the regions that hold something, not every index in the
	 * range: a revoke to EOF runs to U64_MAX and walking that index
	 * by index would never finish.
	 */
	xa_for_each_range(&cache->shards, idx, shard,
			  start >> FUSE_DLM_SHARD_SHIFT,
			  end >> FUSE_DLM_SHARD_SHIFT) {
		uint64_t lo = max(start, FUSE_DLM_SHARD_FIRST(idx));
		uint64_t hi = min(end, FUSE_DLM_SHARD_LAST(idx));
		unsigned long first = fuse_dlm_bit(lo);
		unsigned long nbits = fuse_dlm_bit(hi) - first + 1;

		/*
		 * Plain, not atomic: @cache->lock is held for write, so no
		 * reader and no recorder can be looking at these words.
		 */
		bitmap_clear(shard->granted, first, nbits);
		bitmap_clear(shard->write, first, nbits);

		/*
		 * Keep the shard table to the regions that hold something:
		 * a file revoked a region at a time would otherwise leave an
		 * empty shard behind for every one of them.
		 */
		if (bitmap_empty(shard->granted, FUSE_DLM_SHARD_PAGES)) {
			xa_erase(&cache->shards, idx);
			kfree(shard);
		}
	}

	up_write(&cache->lock);
	return 0;
}

/*
 * Is every page of [@from, @to] covered in @mode?  Both bounds are page
 * aligned and lie inside @shard.  A write grant sets both maps, so a
 * read request is answered by @granted alone.
 *
 * No lock of its own: bits are set atomically and cleared only under
 * fuse_dlm_cache.lock held for write, which the caller holds for read.
 * A bit set concurrently may be missed, which costs a re-request of a
 * range already held.
 */
static bool fuse_dlm_shard_covers(struct fuse_dlm_shard *shard, uint64_t from,
				  uint64_t to, enum fuse_page_lock_mode mode)
{
	const unsigned long *map = mode == FUSE_PAGE_LOCK_WRITE ?
				   shard->write : shard->granted;
	unsigned long first = fuse_dlm_bit(from);
	unsigned long last = fuse_dlm_bit(to);

	return find_next_zero_bit(map, last + 1, first) > last;
}

/**
 * fuse_dlm_range_is_locked - Check if a byte range is already locked
 * @inode: The fuse inode
 * @start: Start byte offset
 * @end: End byte offset
 * @mode: Lock mode to check for
 *
 * Return: true if the entire range is locked, false otherwise
 */
static bool fuse_dlm_range_is_locked(struct fuse_inode *inode, uint64_t start,
				     uint64_t end,
				     enum fuse_page_lock_mode mode)
{
	struct fuse_dlm_cache *cache = &inode->dlm_locked_areas;
	unsigned long idx, last_idx;
	bool covered = true;

	if (start > end)
		return false;

	/*
	 * Read: coverage is only ever removed under @cache->lock held for
	 * write, so the set of grants can grow under this walk but never
	 * shrink.  That is what lets the shards be visited one at a time,
	 * and their maps read without any further lock.  The worst a
	 * concurrent recorder can do is make this report a range uncovered
	 * that has just become covered, and the caller then asks for a
	 * grant it already holds.
	 */
	down_read(&cache->lock);

	last_idx = end >> FUSE_DLM_SHARD_SHIFT;

	for (idx = start >> FUSE_DLM_SHARD_SHIFT; idx <= last_idx; idx++) {
		struct fuse_dlm_shard *shard = xa_load(&cache->shards, idx);
		uint64_t lo = max(start, FUSE_DLM_SHARD_FIRST(idx));
		uint64_t hi = min(end, FUSE_DLM_SHARD_LAST(idx));

		if (!shard || !fuse_dlm_shard_covers(shard, lo, hi, mode)) {
			covered = false;
			break;
		}
	}

	up_read(&cache->lock);

	return covered;
}

/**
 * fuse_dlm_lock_is_held - check that a byte range is covered by a granted lock
 * @fi:     the fuse inode
 * @offset: byte offset into the file (need not be page-aligned)
 * @length: length of the region in bytes (need not be page-aligned)
 * @mode:   FUSE_PAGE_LOCK_READ or FUSE_PAGE_LOCK_WRITE
 *
 * Re-validation helper for fuse_get_dlm_lock() callers: checks the same
 * page-aligned range a fuse_get_dlm_lock() call with these arguments
 * requests, against the live lock tree.
 */
bool fuse_dlm_lock_is_held(struct fuse_inode *fi, loff_t offset,
			   size_t length, enum fuse_page_lock_mode mode)
{
	uint64_t end = (offset + length - 1) | (PAGE_SIZE - 1);

	/*
	 * An empty range needs no coverage.  Reporting it held keeps the
	 * re-validating IO paths from re-requesting a lock the tree can
	 * never show (the page-aligned end would invert below).
	 */
	if (!length)
		return true;

	return fuse_dlm_range_is_locked(fi, offset & PAGE_MASK, end, mode);
}

/**
 * fuse_get_dlm_lock - request a dlm lock from the fuse server
 * @file:   the file being accessed
 * @offset: byte offset into the file (need not be page-aligned)
 * @length: length of the region in bytes (need not be page-aligned)
 * @mode:   FUSE_PAGE_LOCK_READ or FUSE_PAGE_LOCK_WRITE
 *
 * Return: 0 when the range is covered by a recorded grant on return,
 * FUSE_DLM_GRANT_UNRECORDED when the server granted the lock but
 * recording it failed (covered cluster-wide, invisible to
 * fuse_dlm_lock_is_held()), a negative error code otherwise.  Callers
 * re-validating the grant must not re-request on a nonzero return or
 * they would spin.
 */
static int __fuse_get_dlm_lock(struct fuse_file *ff, struct inode *inode,
			       loff_t offset, size_t length,
			       enum fuse_page_lock_mode mode)
{
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_mount *fm = ff->fm;

	FUSE_ARGS(args);
	struct fuse_dlm_lock_in inarg;
	struct fuse_dlm_lock_out outarg;
	struct fuse_dlm_range req;
	uint64_t pg_start, pg_end;
	uint64_t grant_start, grant_end;
	int tries = FUSE_DLM_GRANT_RETRIES;
	int err;

	/* An empty range needs no lock. */
	if (!length)
		return 0;

	/*
	 * note that the offset and length don't have to be page aligned
	 * here but since we only get here on writeback caching we will
	 * send out page aligned requests
	 */
	pg_start = (uint64_t)offset & PAGE_MASK;
	pg_end = ((uint64_t)offset + length - 1) | (PAGE_SIZE - 1);

restart:
	/* note that this can be run from different processes
	 * at the same time. It is intentionally not protected
	 * since a DLM implementation in the FUSE server should take care
	 * of any races in lock requests.
	 * The early exit uses the same helper the callers re-validate
	 * with, so this check and a later fuse_dlm_lock_is_held() can
	 * never disagree about what counts as covered. */
	if (fuse_dlm_lock_is_held(fi, offset, length, mode)) {
		/*
		 * Already covered, and the record says nothing beyond that,
		 * so this is one shared acquisition end to end.
		 */
		return 0;
	}

	memset(&inarg, 0, sizeof(inarg));
	memset(&outarg, 0, sizeof(outarg));
	inarg.fh = ff->fh;

	inarg.start = pg_start;
	inarg.end = pg_end;
	inarg.type = (mode == FUSE_PAGE_LOCK_WRITE) ?
		FUSE_DLM_LOCK_WRITE : FUSE_DLM_LOCK_READ;

	args.opcode = FUSE_DLM_WB_LOCK;
	args.nodeid = get_node_id(inode);
	args.in_numargs = 1;
	args.in_args[0].size = sizeof(inarg);
	args.in_args[0].value = &inarg;
	args.out_numargs = 1;
	args.out_args[0].size = sizeof(outarg);
	args.out_args[0].value = &outarg;

	/* Publish before sending; see fuse_dlm_request_begin() */
	fuse_dlm_request_begin(fi, &req, inarg.start, inarg.end);

	err = fuse_simple_request(fm, &args);
	if (err) {
		fuse_dlm_request_abort(fi, &req);
		if (err == -ENOSYS) {
			/* fuse server does not support dlm, save the info */
			fc->dlm = 0;
			return err;
		}
		/*
		 * The range is contended, the same answer a READ gets and
		 * fuse_do_readfolio() turns into AOP_TRUNCATED_PAGE for its
		 * caller to retry.  There is no such convention here, and
		 * the writeback caller loses the folio it is holding on an
		 * error, so ask again instead of reporting it.
		 */
		if (err == -EDEADLK || err == -EAGAIN)
			goto retry;
		return err;
	}

	if (inarg.start < outarg.start || inarg.end > outarg.end) {
		/* fuse server is seriously broken */
		fuse_dlm_request_abort(fi, &req);
		pr_warn("fuse: dlm lock request for %llu:%llu returned %llu:%llu bytes\n",
			inarg.start, inarg.end, outarg.start, outarg.end);
		fuse_abort_conn(fc);
		return -EIO;
	}

	/*
	 * Keep the recorded grant to a bounded distance either side of
	 * what was asked for; see FUSE_DLM_MAX_EXTRA_GRANT.  Both bounds
	 * stay outside [pg_start, pg_end], so the range this call has to
	 * cover is still covered.
	 */
	grant_start = outarg.start;
	grant_end = outarg.end;
	/*
	 * Both differences are safe: the check above established
	 * outarg.start <= pg_start <= pg_end <= outarg.end, and a branch
	 * is only taken when there is more than the cap to give back, so
	 * neither adjusted bound can wrap.
	 */
	if (pg_start - grant_start > FUSE_DLM_MAX_EXTRA_GRANT)
		grant_start = pg_start - FUSE_DLM_MAX_EXTRA_GRANT;
	if (grant_end - pg_end > FUSE_DLM_MAX_EXTRA_GRANT)
		grant_end = pg_end + FUSE_DLM_MAX_EXTRA_GRANT;

	/*
	 * Align inward.  A page is covered only when it is covered whole,
	 * and the bit helpers take that as given; the bounds themselves are
	 * the server's to choose, only their superset property having been
	 * checked.  Rounding inward cannot uncover [pg_start, pg_end],
	 * which is page aligned already.
	 */
	grant_start = ALIGN(grant_start, PAGE_SIZE);
	/* A last-byte offset, so it is the successor that aligns */
	grant_end -= (grant_end + 1) & (PAGE_SIZE - 1);

	/* Retire the request and record the grant */
	err = fuse_dlm_request_commit(fi, &req, grant_start, grant_end, mode);
	if (err == -EAGAIN) {
		/*
		 * A revoke overlapping this range was processed while the
		 * request was in flight, so the grant is dead.  Retry
		 * rather than fail: no one else holds the range, and the
		 * write path turns an error into a failed write.
		 */
		goto retry;
	}

	/*
	 * A failure to record (small-allocation -ENOMEM) does not undo
	 * the grant: coverage exists cluster-wide, only the local
	 * bookkeeping is missing.  Report that as
	 * FUSE_DLM_GRANT_UNRECORDED so callers neither fail an IO that
	 * is actually covered nor keep re-requesting a grant that will
	 * not become visible.
	 */
	if (err)
		return FUSE_DLM_GRANT_UNRECORDED;

	return 0;

retry:
	/*
	 * Ask again, but not forever.  Every pass is a whole round trip,
	 * which throttles the loop but does not end it, and writeback asks
	 * for a grant with a folio locked, so a node taking the range as
	 * fast as this asks for it would hold that folio and this task for
	 * as long as it kept going.
	 */
	if (fatal_signal_pending(current))
		return -EINTR;
	if (!tries--)
		return -EIO;
	goto restart;
}

int fuse_get_dlm_lock(struct file *file, loff_t offset,
		      size_t length, enum fuse_page_lock_mode mode)
{
	return __fuse_get_dlm_lock(file->private_data, file_inode(file),
				   offset, length, mode);
}

/**
 * fuse_dlm_regrant_range - hold [start, end] again for writeback
 * @ff: a fuse file open for writing on @inode
 * @inode: the inode
 * @start: start byte offset (inclusive)
 * @end: end byte offset (inclusive)
 *
 * Writeback holds the range again before sending a folio, since a revoke
 * may have arrived between the write and the send.  Whatever the other
 * holder wrote in between is overwritten, which for two writers that
 * never synchronised is a legitimate order.
 *
 * A range still held is the ordinary case: the grant is found recorded
 * and nothing is sent to the server.
 */
int fuse_dlm_regrant_range(struct fuse_file *ff, struct inode *inode,
			   uint64_t start, uint64_t end)
{
	return __fuse_get_dlm_lock(ff, inode, start, end - start + 1,
				   FUSE_PAGE_LOCK_WRITE);
}
