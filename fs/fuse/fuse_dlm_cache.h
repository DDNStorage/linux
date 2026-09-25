/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FUSE page cache lock and local IO range lock declarations
 */

#ifndef _FS_FUSE_DLM_CACHE_H
#define _FS_FUSE_DLM_CACHE_H

#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/list.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/wait.h>


struct fuse_inode;

/*
 * FUSE local IO range lock
 *
 * Interval-tree based byte-range lock, embedded in each fuse_inode,
 * used to:
 *
 *  - Serialize concurrent cached reads/writes that touch overlapping
 *    byte ranges of the same file, while letting non-overlapping IO
 *    proceed concurrently.
 *
 *  - Let attribute/BRL invalidation (fuse_reverse_inval_inode(),
 *    truncate in fuse_do_setattr()) block only on in-progress IO that
 *    overlaps with the range being invalidated, instead of serializing
 *    with all IO on the inode.
 *
 * This is modeled after the interval tree used by fuse_dlm_cache, but
 * unlike the DLM cache (which only records ranges the client has been
 * granted a lock for by the server), this tree tracks in-progress local
 * holders and provides blocking acquire/release semantics.
 *
 * Each held range additionally carries a two-state lifecycle:
 *
 *  - INIT: a read/write has reserved the range (so another overlapping
 *    local reader/writer queues behind it, same as before) but has not
 *    yet touched the page cache -- typically while a fuse_get_dlm_lock()
 *    request to the server is in flight.  An INIT range is invisible to
 *    invalidation: fuse_range_lock_acquire_ready() ignores it, so a
 *    NOTIFY invalidate with an overlapping range never waits on the
 *    (unbounded, cluster round trip) DLM request.
 *
 *  - READY: the holder is about to, or is actively, touching the page
 *    cache.  Fully exclusive against any overlapping range per the usual
 *    READ/WRITE compatibility rules, including against other READY
 *    holders and, unlike INIT, against invalidation.
 *
 * A read/write reserves its range with fuse_range_lock_acquire_init(),
 * does whatever DLM work it needs, then calls fuse_range_lock_mark_ready()
 * once it is about to touch the page cache; that call itself blocks until
 * any overlapping READY holder (e.g. an in-progress invalidation that
 * raced ahead of it) is done.  fuse_range_lock_mark_init() is available
 * to move a READY range back to INIT, e.g. if a caller must redo DLM work
 * without letting that block a fresh invalidate on the same range in the
 * meantime -- it never blocks.  Invalidation instead calls
 * fuse_range_lock_acquire_ready(), which never waits on an INIT range.
 * Both sides release with fuse_range_lock_release().
 *
 * This locking exists to protect against DLM-covered writeback IO and
 * invalidation racing on the same range, so callers should only use it
 * when both the writeback cache and DLM are in use for the connection;
 * see individual function comments below.
 */
/* Lock modes for IO range locks */
enum fuse_range_lock_mode {
	/* Shared: compatible with other READ holders on overlapping ranges */
	FUSE_RANGE_LOCK_READ,
	/* Exclusive: incompatible with any overlapping READ or WRITE holder */
	FUSE_RANGE_LOCK_WRITE,
};

/* Lifecycle state of a held range lock; see the range lock comment above. */
enum fuse_range_lock_state {
	/* Reserved, not yet touching the page cache; invisible to
	 * invalidation. */
	FUSE_RANGE_LOCK_INIT,
	/* Actively about to touch, or touching, the page cache; fully
	 * exclusive, including against invalidation. */
	FUSE_RANGE_LOCK_READY,
};

/* Per-inode range lock manager */
struct fuse_range_lock_tree {
	/* Protects the interval tree below */
	spinlock_t lock;
	/* Interval tree of currently held ranges */
	struct rb_root_cached root;
	/* Waiters for a range to become available */
	wait_queue_head_t waitq;
};

/*
 * A single held range lock. The caller owns the storage (typically on
 * the stack, for the duration of one IO/invalidation call) and passes
 * it to fuse_range_lock_acquire_init()/fuse_range_lock_acquire_ready()
 * and to fuse_range_lock_release().
 */
struct fuse_range_lock {
	/* Interval tree node */
	struct rb_node rb;
	/* Start byte offset (inclusive) */
	uint64_t start;
	/* End byte offset (inclusive) */
	uint64_t end;
	/* Subtree end value for interval tree */
	uint64_t __subtree_end;
	/* Lock mode */
	enum fuse_range_lock_mode mode;
	/* Lifecycle state; see the range lock comment above */
	enum fuse_range_lock_state state;
};

/* Initialize the range lock manager embedded in a fuse_inode */
void fuse_range_lock_tree_init(struct fuse_inode *inode);

/*
 * Reserve a range lock on [start, end] (inclusive byte offsets) in the
 * given mode, in INIT state. Blocks until the range can be reserved
 * without conflicting with any other currently held, overlapping range
 * (INIT or READY), same as a plain exclusive acquire. Invisible to
 * fuse_range_lock_acquire_ready() until fuse_range_lock_mark_ready()
 * is called.
 *
 * Caller must only call this when both the writeback cache and DLM are
 * in use for @inode's connection; see the range lock comment above.
 */
void fuse_range_lock_acquire_init(struct fuse_inode *inode,
				 struct fuse_range_lock *lock,
				 uint64_t start, uint64_t end,
				 enum fuse_range_lock_mode mode);

/*
 * Move a range lock reserved by fuse_range_lock_acquire_init() from
 * INIT to READY state. Blocks until no other overlapping READY holder
 * conflicts (e.g. an invalidation that raced ahead while this range was
 * still INIT).
 *
 * Caller must only call this when both the writeback cache and DLM are
 * in use for @inode's connection; see the range lock comment above.
 */
void fuse_range_lock_mark_ready(struct fuse_inode *inode,
			       struct fuse_range_lock *lock);

/*
 * Move a range lock back from READY to INIT state, e.g. because the
 * holder must redo some DLM work before it can touch the page cache
 * again. Never blocks.
 *
 * Caller must only call this when both the writeback cache and DLM are
 * in use for @inode's connection; see the range lock comment above.
 */
void fuse_range_lock_mark_init(struct fuse_inode *inode,
			      struct fuse_range_lock *lock);

/*
 * Acquire a range lock on [start, end] (inclusive byte offsets) in the
 * given mode, directly in READY state. Blocks until the range can be
 * locked without conflicting with any other currently held READY,
 * overlapping range; an overlapping INIT range is ignored. Used by
 * invalidation, which must not wait on a read/write that has only
 * reserved a range and not yet started touching the page cache.
 *
 * Caller must only call this when both the writeback cache and DLM are
 * in use for @inode's connection; see the range lock comment above.
 */
void fuse_range_lock_acquire_ready(struct fuse_inode *inode,
				  struct fuse_range_lock *lock,
				  uint64_t start, uint64_t end,
				  enum fuse_range_lock_mode mode);

/*
 * Release a previously acquired range lock and wake any waiters.
 *
 * Caller must only call this to release a lock that was actually
 * acquired via fuse_range_lock_acquire_init()/_ready(); see the range
 * lock comment above.
 */
void fuse_range_lock_release(struct fuse_inode *inode,
			    struct fuse_range_lock *lock);

/* Lock modes for page ranges */
enum fuse_page_lock_mode { FUSE_PAGE_LOCK_READ, FUSE_PAGE_LOCK_WRITE };

/*
 * fuse_get_dlm_lock() result: the server granted the lock but recording
 * it locally failed, leaving the grant invisible to
 * fuse_dlm_lock_is_held().  The IO is covered cluster-wide; the caller
 * must proceed without re-validating (a re-request would spin) instead
 * of failing the IO.
 */
#define FUSE_DLM_GRANT_UNRECORDED 1

/* Page cache lock manager */
struct fuse_dlm_cache {
	/* Lock protecting the tree */
	struct rw_semaphore lock;
	/* Interval tree of locked ranges */
	struct rb_root_cached ranges;
};

/* Initialize a page cache lock manager */
int fuse_dlm_cache_init(struct fuse_inode *inode);

/* Clean up a page cache lock manager */
void fuse_dlm_cache_release_locks(struct fuse_inode *inode);

/* Lock a range of pages */
int fuse_dlm_lock_range(struct fuse_inode *inode, uint64_t start,
			uint64_t end, enum fuse_page_lock_mode mode);

/* Unlock a range of pages */
int fuse_dlm_unlock_range(struct fuse_inode *inode, uint64_t start,
			  uint64_t end);

/* Check if a page range is already locked */
bool fuse_dlm_range_is_locked(struct fuse_inode *inode, uint64_t start,
			      uint64_t end, enum fuse_page_lock_mode mode);

/* Re-validate a fuse_get_dlm_lock() grant against the live lock tree */
bool fuse_dlm_lock_is_held(struct fuse_inode *inode, loff_t offset,
			   size_t length, enum fuse_page_lock_mode mode);

/* Is any part of the file held for write? */
bool fuse_dlm_write_grant_exists(struct fuse_inode *inode);

/*
 * This is the interface to the filesystem.
 *
 * @rlock: optional IO range lock, previously reserved by the caller in
 * INIT state via fuse_range_lock_acquire_init(), covering (at least)
 * [offset, offset + length - 1].  When non-NULL, it is moved to READY
 * as part of processing a reply that leaves the range covered -- i.e.
 * the already-held fast path, a granted lock, or the server having no
 * DLM at all -- as soon as that outcome is known, which for a request
 * that reaches the server is before this function's caller is even
 * woken up (see fuse_get_dlm_lock_complete() in fuse_dlm_cache.c).
 * Left at INIT on a hard error, since the caller will not touch the
 * page cache and releases it directly.
 */
int fuse_get_dlm_lock(struct file *file, loff_t offset,
		      size_t length, enum fuse_page_lock_mode mode,
		      struct fuse_range_lock *rlock);

#endif /* _FS_FUSE_DLM_CACHE_H */
