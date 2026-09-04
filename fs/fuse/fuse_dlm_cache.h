/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FUSE page cache lock implementation
 */

#ifndef _FS_FUSE_DLM_CACHE_H
#define _FS_FUSE_DLM_CACHE_H

#include <linux/types.h>
#include <linux/bitmap.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/xarray.h>


struct fuse_inode;
struct fuse_dlm_range;
struct fuse_file;

/* Lock modes for page ranges */
enum fuse_page_lock_mode { FUSE_PAGE_LOCK_READ, FUSE_PAGE_LOCK_WRITE };

/*
 * A range held on one of the two lists in struct fuse_dlm_cache: an IO
 * between confirming a grant and publishing the page cache it covers,
 * or a revoke taking grants away.  Caller-owned storage, live until the
 * matching unpin or revoke end.
 *
 * @owner is the pinning task where the pin is dropped by owner, and NULL
 * on every fence and on a pin dropped by node because the fill it covers
 * ends in another task.
 */
struct fuse_dlm_span {
	/* Page-aligned byte offsets, both inclusive */
	uint64_t		start;
	uint64_t		end;
	/* The pinning task, NULL for a revoke */
	struct task_struct	*owner;
	struct list_head	list;
};

/*
 * fuse_get_dlm_lock() result: the server granted the lock but recording
 * it locally failed, leaving the grant invisible to
 * fuse_dlm_lock_is_held().  The IO is covered cluster-wide; the caller
 * must proceed without re-validating (a re-request would spin) instead
 * of failing the IO.
 */
#define FUSE_DLM_GRANT_UNRECORDED 1

/*
 * Coverage is kept per aligned region of the file rather than in one
 * structure for the whole inode, because one structure needs one lock
 * and every thread writing the file then serialises on it however far
 * apart their ranges are.  The region wants to be small enough that
 * concurrent writers land in different ones and large enough that the
 * per-region overhead stays amortised.
 */
#define FUSE_DLM_SHARD_SHIFT	24
#define FUSE_DLM_SHARD_SIZE	(1ULL << FUSE_DLM_SHARD_SHIFT)
#define FUSE_DLM_SHARD_PAGES	(FUSE_DLM_SHARD_SIZE / PAGE_SIZE)

/* First and last byte offset (both inclusive) covered by shard @idx */
#define FUSE_DLM_SHARD_FIRST(idx)	((uint64_t)(idx) << FUSE_DLM_SHARD_SHIFT)
#define FUSE_DLM_SHARD_LAST(idx)	(FUSE_DLM_SHARD_FIRST(idx) + \
					 FUSE_DLM_SHARD_SIZE - 1)

/*
 * The grants over one region, a bit per page.
 *
 * @granted says the page is covered, @write that it is covered for
 * write.  A write grant sets both, so a read request is answered by
 * @granted alone and nothing has to compare modes; @write is a subset of
 * @granted.
 *
 * A region is a fixed span of pages, so the maps are a fixed size and
 * recording a grant neither allocates nor rearranges anything: adjacent
 * grants coalesce because they set neighbouring bits, and a region
 * fragmented to the last page costs no more than the two maps it already
 * has.
 *
 * Bits are set with the atomic helpers, since recorders run concurrently
 * under fuse_dlm_cache.lock held for read.  They are cleared only under
 * that lock held for write, which excludes every reader and every
 * recorder, so the revoke path uses the plain bulk helpers and a query
 * needs no lock of its own.
 */
struct fuse_dlm_shard {
	unsigned long granted[BITS_TO_LONGS(FUSE_DLM_SHARD_PAGES)];
	unsigned long write[BITS_TO_LONGS(FUSE_DLM_SHARD_PAGES)];
};

/*
 * Page cache lock manager.
 *
 * The shards hold the grants the client has been given.  A request still
 * on the wire covers nothing and lives on @pending instead, so the
 * shards answer for grants only.  See struct fuse_dlm_range in
 * fuse_dlm_cache.c.
 *
 * Locking, outermost first:
 *
 *   @lock        rw_semaphore over the whole cache.  Taken for read by
 *                everything that adds or reads coverage, and for write
 *                only by the paths that take coverage away
 *                (fuse_dlm_unlock_range, fuse_dlm_cache_release_locks).
 *                That is the invariant the shard walks rely on:
 *                **coverage is only ever removed under @lock held for
 *                write**, so a walker holding it for read sees a set of
 *                grants that can grow under it but never shrink, and may
 *                therefore visit shards one at a time and read their
 *                maps without any further lock.  Shards are freed only
 *                there too, so a shard pointer stays good for as long as
 *                the read side is held.
 *   @pending_lock the pending list.  Innermost, and the only lock
 *                fuse_dlm_request_begin() and fuse_dlm_request_abort()
 *                take at all.
 *   @pin_lock    the pin and fence lists.  Innermost, taken alone, and
 *                never held across a sleep.
 *
 * @lock says what is covered now, which is not enough for a writer: it
 * confirms a grant, then copies and dirties, and a revoke landing in
 * between sends those bytes out after the server has handed the lock on.
 * The pin closes that: a revoke waits for the pins over the range it is
 * taking away before it removes anything, so a grant confirmed under a
 * pin is still held when the bytes become visible to writeback.  A read
 * is the same the other way round, a fill landing in a range the revoke
 * has already swept staying cached under no grant at all.
 *
 * Both sides are ranges rather than a count, so a revoke fences only the
 * writers it overlaps and a write outside it runs on.  Refusal and wait
 * test the same overlap, which is what makes the wait converge: once a
 * fence is published no pin that would prolong it is admitted.  The
 * nodes are caller storage, so nothing is allocated to take a pin and
 * the writeback path can take one with a folio held.
 */
struct fuse_dlm_cache {
	/* See the locking comment above */
	struct rw_semaphore lock;
	/*
	 * struct fuse_dlm_shard by offset >> FUSE_DLM_SHARD_SHIFT,
	 * allocated when a region first holds a grant.
	 */
	struct xarray shards;
	/* Protects @pending and the killed flag of everything on it */
	spinlock_t pending_lock;
	/*
	 * FUSE_DLM_WB_LOCK requests in flight.  Owned by the queueing
	 * thread; the revoke paths only mark them killed.
	 */
	struct list_head pending;
	/* Protects @pins and @fences */
	spinlock_t pin_lock;
	/*
	 * IO between confirming a grant and publishing under it: a writer
	 * over what it is about to dirty, a read over what it is about to
	 * fill.  See the pin comment above.
	 */
	struct list_head pins;
	/* Revokes in progress, each over the range it takes away */
	struct list_head fences;
	/* Both directions: pins draining, and the fences they wait on */
	wait_queue_head_t pin_wq;
};

/* Initialize a page cache lock manager */
void fuse_dlm_cache_init(struct fuse_inode *inode);

/* Clean up a page cache lock manager */
void fuse_dlm_cache_release_locks(struct fuse_inode *inode);

/*
 * Publish a FUSE_DLM_WB_LOCK for [start, end] before it is sent, so a
 * revoke processed while the reply is on the wire can mark it.  @req is
 * caller-owned storage, live until the matching commit or abort.  The
 * mode is not recorded until the grant is, so only the commit takes it.
 */
void fuse_dlm_request_begin(struct fuse_inode *inode,
			    struct fuse_dlm_range *req, uint64_t start,
			    uint64_t end);

/*
 * Retire @req and record the grant [start, end] as one step under the
 * cache lock.  -EAGAIN means a revoke overlapped @req in flight and
 * nothing was recorded; the caller must request again.  @req is retired
 * either way.
 */
int fuse_dlm_request_commit(struct fuse_inode *inode,
			    struct fuse_dlm_range *req, uint64_t start,
			    uint64_t end, enum fuse_page_lock_mode mode);

/* Retire @req without recording anything */
void fuse_dlm_request_abort(struct fuse_inode *inode,
			    struct fuse_dlm_range *req);

/* Unlock a range of pages */
int fuse_dlm_unlock_range(struct fuse_inode *inode, uint64_t start,
			  uint64_t end);

/*
 * Hold the grants over [@offset, @offset + @length) against revocation
 * until fuse_dlm_unpin(), which drops the pin this task last took.  @pin
 * is caller-owned storage, live until then.  fuse_dlm_pin() waits out a
 * revoke overlapping that range and must not be called with a folio
 * held; fuse_dlm_trypin() never sleeps and fails instead.  Neither may
 * be held across a DLM request: that request is answered by the server
 * the revoke came from.
 */
void fuse_dlm_pin(struct fuse_inode *inode, struct fuse_dlm_span *pin,
		  loff_t offset, size_t length);
bool fuse_dlm_trypin(struct fuse_inode *inode, struct fuse_dlm_span *pin,
		     loff_t offset, size_t length);
void fuse_dlm_unpin(struct fuse_inode *inode);

/*
 * fuse_dlm_trypin() for a fill whose reply lands in another task: @pin
 * is dropped by node rather than by owner, and is live from the request
 * until fuse_dlm_unpin_span().
 */
bool fuse_dlm_trypin_span(struct fuse_inode *inode, struct fuse_dlm_span *pin,
			  loff_t offset, size_t length);
void fuse_dlm_unpin_span(struct fuse_inode *inode, struct fuse_dlm_span *pin);

/*
 * Fence the writers that hold a grant over [@offset, @offset + @len) but
 * have not dirtied under it yet, for the duration of a revoke.  @len <= 0
 * means to EOF, as in fuse_notify_inval_inode().  @fence is caller-owned
 * storage, live until the matching end.  Between these the caller may
 * drop coverage over that range knowing nothing will be dirtied under
 * what it drops, and a write outside it is left alone.
 */
void fuse_dlm_revoke_begin(struct fuse_inode *inode,
			   struct fuse_dlm_span *fence, loff_t offset,
			   loff_t len);
void fuse_dlm_revoke_end(struct fuse_inode *inode,
			 struct fuse_dlm_span *fence);

/* Re-validate a fuse_get_dlm_lock() grant against the live lock tree */
bool fuse_dlm_lock_is_held(struct fuse_inode *inode, loff_t offset,
			   size_t length, enum fuse_page_lock_mode mode);

/* Hold [start, end] again so writeback can send what it found revoked */
int fuse_dlm_regrant_range(struct fuse_file *ff, struct inode *inode,
			   uint64_t start, uint64_t end);


/* This is the interface to the filesystem */
int fuse_get_dlm_lock(struct file *file, loff_t offset,
		      size_t length, enum fuse_page_lock_mode mode);

#endif /* _FS_FUSE_DLM_CACHE_H */
