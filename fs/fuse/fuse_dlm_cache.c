// SPDX-License-Identifier: GPL-2.0-only
/*
 * FUSE page lock cache implementation
 *
 * cache->ranges records the grants the server has given this client.  A
 * grant still on the wire covers nothing and must not appear there, but
 * a revoke has to be able to find it: otherwise a revoke processed
 * before the grant is recorded removes nothing, and the grant recorded
 * afterwards is never taken back.
 *
 * A range therefore carries enum fuse_dlm_range_state:
 *
 *  - REQUESTED, on cache->pending, while its FUSE_DLM_WB_LOCK is in
 *    flight.
 *
 *  - REVOKED, in either place.  On cache->pending it is a request a
 *    revoke overlapped while it was in flight, and
 *    fuse_dlm_request_commit() drops that grant instead of recording it.
 *    In cache->ranges it is a grant that was recorded and has since been
 *    taken away, kept because the page cache under it is still
 *    described.  It covers nothing either way, so the IO paths ask
 *    again.
 *
 *  - READ or WRITE, in cache->ranges.  The only states
 *    fuse_dlm_range_is_locked() reports as covered; the mode is not a
 *    separate field, since a range is either not held or held in one
 *    definite mode.
 *
 * In-flight requests are kept off the tree so the state is consulted
 * only where a request is retired, not by every tree walker.
 *
 * The record says nothing about the page cache under a range.  What is
 * cached there, and whether the server has seen it, is what the page
 * cache itself answers.
 */
#include "fuse_i.h"
#include "fuse_dlm_cache.h"

#include <linux/list.h>
#include <linux/pagemap.h>
#include <linux/sched/signal.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/interval_tree_generic.h>


/*
 * How often to ask again for a grant a revoke killed while it was in
 * flight, before giving up on the range.  Each pass is a round trip.
 */
#define FUSE_DLM_GRANT_RETRIES 16

/* Lifecycle of a range; see the file comment above */
enum fuse_dlm_range_state {
	/* FUSE_DLM_WB_LOCK in flight, on cache->pending */
	FUSE_DLM_RANGE_REQUESTED,
	/*
	 * On cache->pending, revoked in flight and the grant must not be
	 * recorded.  In cache->ranges, granted once and taken away, kept to
	 * describe the page cache under it.  Covers nothing either way.
	 */
	FUSE_DLM_RANGE_REVOKED,
	/* Granted shared, in cache->ranges */
	FUSE_DLM_RANGE_READ,
	/* Granted exclusive, in cache->ranges */
	FUSE_DLM_RANGE_WRITE,
};

/* A range of pages with a lock */
struct fuse_dlm_range {
	/* Interval tree node; only linked once granted */
	struct rb_node rb;
	/*
	 * The range, as byte offsets, both inclusive.  Grants arrive page
	 * aligned, and a range is split only at the bounds of another, so
	 * these are page aligned too.
	 */
	uint64_t start;
	uint64_t end;
	/* Subtree end value for interval tree */
	uint64_t __subtree_end;
	/* Lifecycle and, once granted, the mode; see the enum above */
	enum fuse_dlm_range_state state;
	/* Temporary list entry for operations, and the cache->pending link */
	struct list_head list;
};

/* The state a grant in @mode is recorded under */
static inline enum fuse_dlm_range_state
fuse_dlm_granted_state(enum fuse_page_lock_mode mode)
{
	return mode == FUSE_PAGE_LOCK_READ ? FUSE_DLM_RANGE_READ :
					     FUSE_DLM_RANGE_WRITE;
}

/**
 * fuse_dlm_state_satisfies - is a range in @held usable for @want
 * @held: the state of a range recorded in the tree
 * @want: FUSE_DLM_RANGE_READ or FUSE_DLM_RANGE_WRITE
 *
 * A WRITE grant is exclusive and so covers a READ request; nothing else
 * substitutes for anything.  The two pending states never appear in the
 * tree and cover nothing.
 */
static inline bool fuse_dlm_state_satisfies(enum fuse_dlm_range_state held,
					    enum fuse_dlm_range_state want)
{
	return held == want ||
	       (held == FUSE_DLM_RANGE_WRITE && want == FUSE_DLM_RANGE_READ);
}

/* Interval tree definitions for page ranges */
static inline uint64_t fuse_dlm_range_start(struct fuse_dlm_range *range)
{
	return range->start;
}

static inline uint64_t fuse_dlm_range_last(struct fuse_dlm_range *range)
{
	return range->end;
}

INTERVAL_TREE_DEFINE(struct fuse_dlm_range, rb, uint64_t, __subtree_end,
		    		fuse_dlm_range_start, fuse_dlm_range_last, static,
		    		fuse_page_it);

static void fuse_dlm_split_at(struct fuse_dlm_cache *cache, uint64_t off);

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
 * Caller holds @cache->lock for write.
 */
static void fuse_dlm_kill_pending(struct fuse_dlm_cache *cache,
				  uint64_t start, uint64_t end)
{
	struct fuse_dlm_range *req;

	list_for_each_entry(req, &cache->pending, list)
		if (req->start <= end && start <= req->end)
			req->state = FUSE_DLM_RANGE_REVOKED;
}

/**
 * fuse_page_cache_init - Initialize a page cache lock manager
 * @cache: The cache to initialize
 *
 * Initialize a page cache lock manager for a FUSE inode.
 *
 * Return: 0 on success, negative error code on failure
 */
int fuse_dlm_cache_init(struct fuse_inode *inode)
{
	struct fuse_dlm_cache *cache = &inode->dlm_locked_areas;

	if (!cache)
		return -EINVAL;

	init_rwsem(&cache->lock);
	cache->ranges = RB_ROOT_CACHED;
	INIT_LIST_HEAD(&cache->pending);

	return 0;
}

/**
 * fuse_page_cache_destroy - Clean up a page cache lock manager
 * @cache: The cache to clean up
 *
 * Release all locks and free all resources associated with the cache.
 */
void fuse_dlm_cache_release_locks(struct fuse_inode *inode)
{
	struct fuse_dlm_cache *cache = &inode->dlm_locked_areas;
	struct fuse_dlm_range *range;
	struct rb_node *node;

	if (!cache)
		return;

	/* Release all locks */
	down_write(&cache->lock);
	/*
	 * Every grant goes, so every request in flight is revoked.  Mark
	 * only; each node is owned by the thread waiting on its reply.
	 */
	fuse_dlm_kill_pending(cache, 0, U64_MAX);
	while ((node = rb_first_cached(&cache->ranges)) != NULL) {
		range = rb_entry(node, struct fuse_dlm_range, rb);
		fuse_page_it_remove(range, &cache->ranges);
		kfree(range);
	}
	up_write(&cache->lock);
}

/**
 * fuse_dlm_find_overlapping - Find a range that overlaps with [start, end]
 * @cache: The page cache
 * @start: Start byte offset
 * @end: End byte offset
 *
 * Return: Pointer to the first overlapping range, or NULL if none found
 */
static struct fuse_dlm_range *
fuse_dlm_find_overlapping(struct fuse_dlm_cache *cache, uint64_t start,
			  uint64_t end)
{
	return fuse_page_it_iter_first(&cache->ranges, start, end);
}

/**
 * fuse_page_try_merge - Try to merge ranges within a specific region
 * @cache: The page cache
 * @start: Start byte offset
 * @end: End byte offset
 *
 * Attempt to merge ranges within and adjacent to the specified region
 * that have the same lock mode.
 */
static void fuse_dlm_try_merge(struct fuse_dlm_cache *cache, uint64_t start,
			       uint64_t end)
{
	struct fuse_dlm_range *range, *next;
	uint64_t first = start ? start - 1 : start;
	uint64_t last = end < U64_MAX ? end + 1 : end;

	if (!cache)
		return;

	/*
	 * Find the first range that might need merging.  Directly adjacent
	 * ranges can merge, hence the region is widened by one unit to each
	 * side (saturating at the type bounds).  This must stay an
	 * interval-tree lookup: the tree holds every cached grant of the
	 * inode and strided writers grow it for the lifetime of the file,
	 * so seeding the merge by walking from the tree minimum would make
	 * every new grant cost a full scan.
	 */
	range = fuse_page_it_iter_first(&cache->ranges, first, last);

	/* Try to merge ranges in and around the specified region */
	while (range && range->start <= last) {
		/* Get next range before we potentially modify the tree */
		next = NULL;
		if (rb_next(&range->rb)) {
			next = rb_entry(rb_next(&range->rb),
					struct fuse_dlm_range, rb);
		}

		/* Merge neighbours the server has given us on the same terms */
		if (next && range->state == next->state &&
		    range->end + 1 == next->start) {
			/* Merge ranges: re-insert so __subtree_end is updated */
			fuse_page_it_remove(next, &cache->ranges);
			fuse_page_it_remove(range, &cache->ranges);
			range->end = next->end;
			fuse_page_it_insert(range, &cache->ranges);
			kfree(next);

			/* Continue with the same range */
			continue;
		}

		/* Move to next range */
		range = next;
	}
}

/**
 * fuse_dlm_lock_range_locked - Record a granted range of pages
 * @inode: The fuse inode
 * @start: Start byte offset
 * @end: End byte offset
 * @mode: Lock mode (read or write)
 *
 * Add a locked range on the specified range of pages.
 * If parts of the range are already locked, only add the remaining parts.
 * For overlapping ranges, handle lock compatibility:
 * - READ locks are compatible with existing READ locks
 * - READ locks are compatible with existing WRITE locks (downgrade not needed)
 * - WRITE locks need to upgrade existing READ locks
 *
 * Everything inserted here is READ or WRITE: this runs only after the
 * server has answered.
 *
 * Caller holds the cache lock for write.
 *
 * Return: 0 on success, negative error code on failure
 */
static int fuse_dlm_lock_range_locked(struct fuse_inode *inode, uint64_t start,
				      uint64_t end,
				      enum fuse_page_lock_mode mode)
{
	struct fuse_dlm_cache *cache = &inode->dlm_locked_areas;
	struct fuse_dlm_range *range, *new_range, *next;
	enum fuse_dlm_range_state want;
	bool covered_to_end = false;
	int ret = 0;
	LIST_HEAD(to_lock);
	LIST_HEAD(to_upgrade);
	uint64_t current_start = start;

	if (!cache || start > end)
		return -EINVAL;

	/* The state this grant records */
	want = fuse_dlm_granted_state(mode);

	/*
	 * Ranges are upgraded whole below, so split at the grant bounds
	 * first: a range extending past the grant would otherwise have its
	 * uncovered part upgraded with it, recording coverage the server
	 * never gave.  A read range half-covered by a write grant would
	 * report the other half held for write, and a revoked range
	 * half-regranted would report its still-revoked bytes as held, so
	 * writeback would send them without taking the range again.
	 */
	fuse_dlm_split_at(cache, start);
	if (end < U64_MAX)
		fuse_dlm_split_at(cache, end + 1);

	/* Find all ranges that overlap with [start, end] */
	range = fuse_page_it_iter_first(&cache->ranges, start, end);
	while (range) {
		/* Get next overlapping range before we potentially modify the tree */
		next = fuse_page_it_iter_next(range, start, end);

		/*
		 * A revoked range is covered again by this grant, and a read
		 * range needs upgrading when a write is granted.
		 */
		if (range->state == FUSE_DLM_RANGE_REVOKED ||
		    (want == FUSE_DLM_RANGE_WRITE &&
		     range->state != FUSE_DLM_RANGE_WRITE))
			list_add_tail(&range->list, &to_upgrade);
		/* If WRITE lock already exists - nothing to do */

		/* If there's a gap before this range, we need to add the missing range */
		if (current_start < range->start) {
			new_range = kmalloc(sizeof(*new_range), GFP_NOFS);
			if (!new_range) {
				ret = -ENOMEM;
				goto out_free;
			}

			new_range->start = current_start;
			new_range->end = range->start - 1;
			new_range->state = want;
			INIT_LIST_HEAD(&new_range->list);

			list_add_tail(&new_range->list, &to_lock);
		}

		/* Move current_start past this range */
		if (range->end >= end)
			covered_to_end = true;
		else
			current_start = max(current_start, range->end + 1);

		/* Move to next range */
		range = next;
	}

	/* If there's a gap after the last range to the end, extend the range */
	if (!covered_to_end && current_start <= end) {
		new_range = kmalloc(sizeof(*new_range), GFP_NOFS);
		if (!new_range) {
			ret = -ENOMEM;
			goto out_free;
		}

		new_range->start = current_start;
		new_range->end = end;
		new_range->state = want;
		INIT_LIST_HEAD(&new_range->list);

		list_add_tail(&new_range->list, &to_lock);
	}

	/* Everything on this list is now covered in @want */
	list_for_each_entry(range, &to_upgrade, list)
		range->state = want;

	/* Add all new ranges to the tree */
	list_for_each_entry(new_range, &to_lock, list) {
		/* Add to interval tree */
		fuse_page_it_insert(new_range, &cache->ranges);
	}

	/* Try to merge adjacent ranges with the same mode */
	fuse_dlm_try_merge(cache, start, end);

	return 0;

out_free:
	/* Free any ranges we allocated but didn't insert */
	while (!list_empty(&to_lock)) {
		new_range =
			list_first_entry(&to_lock, struct fuse_dlm_range, list);
		list_del(&new_range->list);
		kfree(new_range);
	}

	/*
	 * Nothing to undo on @to_upgrade: every goto here is taken before
	 * the loop above runs, so no state has been changed yet.
	 */
	return ret;
}

int fuse_dlm_lock_range(struct fuse_inode *inode, uint64_t start,
			uint64_t end, enum fuse_page_lock_mode mode)
{
	struct fuse_dlm_cache *cache = &inode->dlm_locked_areas;
	int ret;

	down_write(&cache->lock);
	ret = fuse_dlm_lock_range_locked(inode, start, end, mode);
	up_write(&cache->lock);

	return ret;
}

/**
 * fuse_dlm_request_begin - publish a lock request before it is sent
 * @inode: the fuse inode
 * @req: caller-owned storage for the request, live until commit or abort
 * @start: start byte offset being requested (inclusive)
 * @end: end byte offset being requested (inclusive)
 *
 * The mode is not recorded here: until the server answers the range is
 * held in neither, and the mode that reaches the tree is the one passed
 * to fuse_dlm_request_commit().
 *
 * A FUSE_DLM_WB_LOCK reply and a NOTIFY revoke are serviced on different
 * threads, so a revoke can be processed before the grant the reply
 * carries is recorded.  Publishing the request before it leaves gives
 * that revoke a node to mark; without one it removes nothing, and the
 * grant recorded afterwards is never taken back by any later NOTIFY.
 *
 * The request covers nothing while in flight, so it is kept off the
 * tree.  @req is reachable only through cache->pending, which both
 * fuse_dlm_request_commit() and fuse_dlm_request_abort() unlink under
 * the cache lock before the caller returns; stack storage is therefore
 * fine and nothing is allocated here.
 */
void fuse_dlm_request_begin(struct fuse_inode *inode,
			    struct fuse_dlm_range *req, uint64_t start,
			    uint64_t end)
{
	struct fuse_dlm_cache *cache = &inode->dlm_locked_areas;

	RB_CLEAR_NODE(&req->rb);
	req->start = start;
	req->end = end;
	req->state = FUSE_DLM_RANGE_REQUESTED;
	/* Nothing reads this while the request is pending; publish it set */

	down_write(&cache->lock);
	list_add_tail(&req->list, &cache->pending);
	up_write(&cache->lock);
}

/**
 * fuse_dlm_request_commit - retire a request and record its grant
 * @inode: the fuse inode
 * @req: the request published by fuse_dlm_request_begin()
 * @start: start byte offset the server granted (inclusive)
 * @end: end byte offset the server granted (inclusive)
 * @mode: the mode that was requested
 *
 * Unlinking @req and recording the grant are one step under the cache
 * lock, so a revoke lands either before it and is seen on @req, or after
 * it and finds the grant in the tree.
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

	down_write(&cache->lock);
	list_del(&req->list);
	revoked = req->state == FUSE_DLM_RANGE_REVOKED;
	if (!revoked)
		ret = fuse_dlm_lock_range_locked(inode, start, end, mode);
	up_write(&cache->lock);

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

	down_write(&cache->lock);
	list_del(&req->list);
	up_write(&cache->lock);
}

/**
 * fuse_dlm_split_at - make @off start a range
 * @cache: The page cache
 * @off: byte offset to split at
 *
 * Splits the range containing @off in two, both halves keeping the state
 * of the original, so a revoke can apply to one side only.  A no-op when
 * @off already starts a range or falls in a gap.
 *
 * Caller holds @cache->lock for write.
 *
 * Cannot fail: the split decides which bytes a caller goes on to name,
 * and both naming more than was written and lowering more than was sent
 * lose data.  iomap allocates the state it keeps per folio the same way.
 */
static void fuse_dlm_split_at(struct fuse_dlm_cache *cache, uint64_t off)
{
	struct fuse_dlm_range *range, *tail;

	if (!off)
		return;

	range = fuse_page_it_iter_first(&cache->ranges, off, off);
	if (!range || range->start == off)
		return;

	tail = kmalloc(sizeof(*tail), GFP_NOFS | __GFP_NOFAIL);

	*tail = *range;
	INIT_LIST_HEAD(&tail->list);
	tail->start = off;

	/*
	 * Bounds are never edited in place: the interval tree caches a
	 * subtree end that only insertion recomputes.
	 */
	fuse_page_it_remove(range, &cache->ranges);
	range->end = off - 1;
	fuse_page_it_insert(range, &cache->ranges);
	fuse_page_it_insert(tail, &cache->ranges);
}

/**
 * fuse_dlm_ranges_dropped - the page cache under [start, end] is gone
 * @inode: the fuse inode
 * @start: start byte offset (inclusive)
 * @end: end byte offset (inclusive)
 *
 * A revoked range exists only to describe page cache dirtied before the
 * grant was taken away.  Once that cache is gone the range has nothing
 * left to say and is freed; a range still held goes back to describing
 * nothing.
 *
 * The caller must have established that the range really is empty, not
 * merely asked for it to be dropped: a folio that survived an
 * invalidate is still there, and claiming otherwise would let writeback
 * send it with no record of where it came from.
 */
void fuse_dlm_ranges_dropped(struct fuse_inode *inode, uint64_t start,
			     uint64_t end)
{
	struct fuse_dlm_cache *cache = &inode->dlm_locked_areas;
	struct fuse_dlm_range *range, *next;

	if (start > end)
		return;

	down_write(&cache->lock);

	fuse_dlm_split_at(cache, start);
	if (end < U64_MAX)
		fuse_dlm_split_at(cache, end + 1);

	range = fuse_page_it_iter_first(&cache->ranges, start, end);
	while (range) {
		next = fuse_page_it_iter_next(range, start, end);

		if (range->state == FUSE_DLM_RANGE_REVOKED) {
			fuse_page_it_remove(range, &cache->ranges);
			kfree(range);
		}

		range = next;
	}

	fuse_dlm_try_merge(cache, start, end);

	up_write(&cache->lock);
}

/**
 * fuse_dlm_unlock_range - Revoke the grants over a range of pages
 * @inode: The fuse inode
 * @start: Start byte offset
 * @end: End byte offset
 *
 * The server has taken [start, end] back.  A range that has nothing
 * cached under it is removed; one that has is kept and marked
 * FUSE_DLM_RANGE_REVOKED, so the page cache it covers stays described.
 * Removing it instead would leave a gap, and a gap reads as "no record",
 * which is what an untracked range looks like: writeback would then send
 * folios dirtied under the grant that was just taken away.
 *
 * A revoked range covers nothing, so fuse_dlm_range_is_locked() reports
 * it uncovered and the IO paths request again.
 *
 * An inverted range is rejected rather than silently revoking nothing:
 * the callers revoke coverage, and a revoke that quietly keeps the grant
 * alive would let the re-validating IO paths trust a lock the server has
 * taken away.  To drop every grant use fuse_dlm_cache_release_locks()
 * (there is no in-band sentinel range for it).
 *
 * Return: 0 on success, negative error code on failure
 */
int fuse_dlm_unlock_range(struct fuse_inode *inode,
						uint64_t start, uint64_t end)
{
	struct fuse_dlm_cache *cache = &inode->dlm_locked_areas;
	struct fuse_dlm_range *range, *next;

	if (!cache || start > end)
		return -EINVAL;

	down_write(&cache->lock);

	/*
	 * Before touching the tree, and even when nothing in the tree
	 * overlaps: a revoke racing an in-flight grant finds no overlap
	 * because that grant is not recorded yet.
	 */
	fuse_dlm_kill_pending(cache, start, end);

	/* Split so the revoked region has its own ranges */
	fuse_dlm_split_at(cache, start);
	if (end < U64_MAX)
		fuse_dlm_split_at(cache, end + 1);

	range = fuse_page_it_iter_first(&cache->ranges, start, end);
	while (range) {
		/* Get next overlapping range before we modify the tree */
		next = fuse_page_it_iter_next(range, start, end);

		/*
		 * A revoked range is kept only to say that the page cache
		 * under it was dirtied under a grant that has gone, so that
		 * writeback takes the range again before sending it.  With
		 * nothing cached there it has nothing to say.
		 *
		 * A grant with no end is recorded to U64_MAX; the page cache
		 * is indexed by a signed offset, so ask it about as much of
		 * that as it can name.
		 */
		if (filemap_range_has_page(inode->inode.i_mapping, range->start,
					   min_t(uint64_t, range->end,
						 LLONG_MAX))) {
			range->state = FUSE_DLM_RANGE_REVOKED;
		} else {
			fuse_page_it_remove(range, &cache->ranges);
			kfree(range);
		}

		range = next;
	}

	fuse_dlm_try_merge(cache, start, end);

	up_write(&cache->lock);
	return 0;
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
bool fuse_dlm_range_is_locked(struct fuse_inode *inode, uint64_t start,
			      uint64_t end, enum fuse_page_lock_mode mode)
{
	struct fuse_dlm_cache *cache = &inode->dlm_locked_areas;
	struct fuse_dlm_range *range;
	enum fuse_dlm_range_state want;
	uint64_t current_start = start;

	if (!cache || start > end)
		return false;

	/* The state a range has to be in to cover this request */
	want = fuse_dlm_granted_state(mode);

	down_read(&cache->lock);

	/* Find the first range that overlaps with [start, end] */
	range = fuse_dlm_find_overlapping(cache, start, end);

	/* Check if the entire range is covered */
	while (range && current_start <= end) {
		/*
		 * The held lock must be at least as strong as the one
		 * requested. A WRITE lock (exclusive) satisfies a READ
		 * request, so only treat the range as uncovered when the
		 * held mode is weaker than what we ask for. This avoids
		 * re-requesting a READ lock for a range we already hold
		 * a WRITE lock on (e.g. read-after-write).
		 */
		if (!fuse_dlm_state_satisfies(range->state, want)) {
			/* Held lock is weaker than requested */
			up_read(&cache->lock);
			return false;
		}

		/* Check if there's a gap before this range */
		if (current_start < range->start) {
			/* Found a gap */
			up_read(&cache->lock);
			return false;
		}

		/* Covered through the end of the requested range? */
		if (range->end >= end) {
			up_read(&cache->lock);
			return true;
		}

		/* Move current_start past this range */
		current_start = range->end + 1;

		/* Get next overlapping range */
		range = fuse_page_it_iter_next(range, start, end);
	}

	/* Check if we covered the entire range */
	if (current_start <= end) {
		/* There's a gap at the end */
		up_read(&cache->lock);
		return false;
	}

	up_read(&cache->lock);
	return true;
}

/**
 * fuse_dlm_write_grant_exists - does the inode hold an exclusive grant anywhere
 * @fi: the fuse inode
 *
 * Unlike fuse_dlm_range_is_locked(), which asks whether one range is fully
 * covered, this asks whether any part of the file is held exclusively.  A
 * client that holds a write grant may be sitting on dirty page cache the
 * server has not seen, so its mtime and ctime run ahead of anything the
 * server can report.
 *
 * A revoked range is not counted.  The state does not keep the mode the
 * range was granted in, so a revoked one cannot be told from a read that
 * was taken away, and the dirty page cache such a range describes is
 * what the caller checks the mapping for instead.
 *
 * Return: true if at least one recorded range is held for write
 */
bool fuse_dlm_write_grant_exists(struct fuse_inode *fi)
{
	struct fuse_dlm_cache *cache = &fi->dlm_locked_areas;
	struct fuse_dlm_range *range;
	bool held = false;

	down_read(&cache->lock);
	for (range = fuse_dlm_find_overlapping(cache, 0, U64_MAX); range;
	     range = fuse_page_it_iter_next(range, 0, U64_MAX)) {
		if (range->state == FUSE_DLM_RANGE_WRITE) {
			held = true;
			break;
		}
	}
	up_read(&cache->lock);

	return held;
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
		}
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
	 * Retire the request and record the grant.  A server may grant more
	 * than was asked for, and recording all of it is what lets later IO
	 * over the same neighbourhood skip the round trip.
	 *
	 * fuse_dlm_kill_pending() matches a request in flight on the bounds
	 * published to it, so a revoke aimed only at the part beyond them
	 * marks nothing here.  It would have to be a revoke of a sub-range
	 * the server is granting in the same breath.
	 */
	err = fuse_dlm_request_commit(fi, &req, outarg.start, outarg.end, mode);
	if (err == -EAGAIN) {
		/*
		 * A revoke overlapping this range was processed while the
		 * request was in flight, so the grant is dead.  Retry
		 * rather than fail: no one else holds the range, and the
		 * write path turns an error into a failed write.
		 *
		 * Not forever, though.  Every pass is a whole round trip,
		 * which throttles the loop but does not end it, and
		 * writeback asks for a grant with a folio locked, so a node
		 * revoking as fast as the grants arrive would hold that
		 * folio and this task for as long as it kept going.
		 */
		if (fatal_signal_pending(current))
			return -EINTR;
		if (!tries--)
			return -EIO;
		goto restart;
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
