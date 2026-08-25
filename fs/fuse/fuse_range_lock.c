// SPDX-License-Identifier: GPL-2.0-only
/*
 * FUSE local IO range lock implementation
 *
 * See fuse_range_lock.h for a description of what this is used for.
 */
#include "fuse_i.h"
#include "fuse_range_lock.h"

#include <linux/interval_tree_generic.h>

/* Interval tree definitions for IO range locks */
static inline uint64_t fuse_range_lock_start(struct fuse_range_lock *lock)
{
	return lock->start;
}

static inline uint64_t fuse_range_lock_last(struct fuse_range_lock *lock)
{
	return lock->end;
}

INTERVAL_TREE_DEFINE(struct fuse_range_lock, rb, uint64_t, __subtree_end,
		   fuse_range_lock_start, fuse_range_lock_last, static,
		   fuse_range_it);

/**
 * fuse_range_lock_tree_init - Initialize the range lock manager
 * @inode: The fuse inode whose range lock tree to initialize
 */
void fuse_range_lock_tree_init(struct fuse_inode *inode)
{
	struct fuse_range_lock_tree *tree = &inode->io_range_lock;

	spin_lock_init(&tree->lock);
	tree->root = RB_ROOT_CACHED;
	init_waitqueue_head(&tree->waitq);
}

/**
 * fuse_range_conflicts - Test @lock's range against currently held ranges
 * @tree: The range lock tree
 * @lock: The range lock being tested (not yet inserted, or already held)
 * @ready_only: If true, ignore existing ranges still in INIT state
 *
 * A conflict occurs whenever an overlapping range exists (other than
 * @lock itself) and either @lock or that range is a WRITE lock (READ
 * ranges may overlap each other freely). When @ready_only is set, a
 * range still in INIT state (reserved, not yet touching the page cache)
 * is not considered -- see fuse_range_lock_acquire_ready().
 *
 * Caller holds @tree->lock.
 *
 * Return: true if @lock's range conflicts with an existing held range.
 */
static bool fuse_range_conflicts(struct fuse_range_lock_tree *tree,
				struct fuse_range_lock *lock,
				bool ready_only)
{
	struct fuse_range_lock *cur;

	cur = fuse_range_it_iter_first(&tree->root, lock->start, lock->end);
	while (cur) {
		if (cur != lock &&
		    (!ready_only || cur->state == FUSE_RANGE_LOCK_READY) &&
		    (lock->mode == FUSE_RANGE_LOCK_WRITE ||
		     cur->mode == FUSE_RANGE_LOCK_WRITE))
			return true;
		cur = fuse_range_it_iter_next(cur, lock->start, lock->end);
	}

	return false;
}

/**
 * fuse_range_try_lock_init - Try to insert @lock into @tree in INIT state
 * @tree: The range lock tree
 * @lock: The range lock to try to acquire
 *
 * Conflict tested against every existing range regardless of state, same
 * as a plain exclusive acquire -- this is what keeps two local IOs on an
 * overlapping range serialized against each other even while both are
 * still reserving (INIT), not yet touching the page cache.
 *
 * Return: true if @lock was inserted, false if the caller must wait.
 */
static bool fuse_range_try_lock_init(struct fuse_range_lock_tree *tree,
				    struct fuse_range_lock *lock)
{
	bool conflict;

	spin_lock(&tree->lock);

	conflict = fuse_range_conflicts(tree, lock, false);
	if (!conflict) {
		lock->state = FUSE_RANGE_LOCK_INIT;
		fuse_range_it_insert(lock, &tree->root);
	}

	spin_unlock(&tree->lock);

	return !conflict;
}

/**
 * fuse_range_try_lock_ready - Try to insert @lock into @tree in READY state
 * @tree: The range lock tree
 * @lock: The range lock to try to acquire
 *
 * Conflict tested only against existing READY ranges: an overlapping
 * range still in INIT state is ignored, so this never waits behind a
 * read/write that has only reserved a range and not yet started
 * touching the page cache.
 *
 * Return: true if @lock was inserted, false if the caller must wait.
 */
static bool fuse_range_try_lock_ready(struct fuse_range_lock_tree *tree,
				     struct fuse_range_lock *lock)
{
	bool conflict;

	spin_lock(&tree->lock);

	conflict = fuse_range_conflicts(tree, lock, true);
	if (!conflict) {
		lock->state = FUSE_RANGE_LOCK_READY;
		fuse_range_it_insert(lock, &tree->root);
	}

	spin_unlock(&tree->lock);

	return !conflict;
}

/**
 * fuse_range_try_mark_ready - Try to move an already-held @lock to READY
 * @tree: The range lock tree
 * @lock: The (already inserted) range lock to move to READY state
 *
 * Return: true if @lock is now READY, false if the caller must wait for
 * a conflicting READY range to be released.
 */
static bool fuse_range_try_mark_ready(struct fuse_range_lock_tree *tree,
				     struct fuse_range_lock *lock)
{
	bool conflict;

	spin_lock(&tree->lock);

	conflict = fuse_range_conflicts(tree, lock, true);
	if (!conflict)
		lock->state = FUSE_RANGE_LOCK_READY;

	spin_unlock(&tree->lock);

	return !conflict;
}

/**
 * fuse_range_lock_acquire_init - Reserve a byte range lock in INIT state
 * @inode: The fuse inode
 * @lock: Caller-allocated storage for the lock (e.g. on the stack)
 * @start: Start byte offset (inclusive)
 * @end: End byte offset (inclusive)
 * @mode: FUSE_RANGE_LOCK_READ or FUSE_RANGE_LOCK_WRITE
 *
 * Blocks until [start, end] can be reserved in the requested mode
 * without conflicting with any other currently held, overlapping range.
 */
void fuse_range_lock_acquire_init(struct fuse_inode *inode,
				 struct fuse_range_lock *lock,
				 uint64_t start, uint64_t end,
				 enum fuse_range_lock_mode mode)
{
	struct fuse_range_lock_tree *tree = &inode->io_range_lock;

	lock->start = start;
	lock->end = end;
	lock->mode = mode;

	wait_event(tree->waitq, fuse_range_try_lock_init(tree, lock));
}

/**
 * fuse_range_lock_mark_ready - Move a reserved range lock to READY state
 * @inode: The fuse inode
 * @lock: The range lock previously passed to fuse_range_lock_acquire_init()
 *
 * Blocks until no other currently held, overlapping READY range
 * conflicts with @lock.
 */
void fuse_range_lock_mark_ready(struct fuse_inode *inode,
			       struct fuse_range_lock *lock)
{
	struct fuse_range_lock_tree *tree = &inode->io_range_lock;

	wait_event(tree->waitq, fuse_range_try_mark_ready(tree, lock));
}

/**
 * fuse_range_lock_mark_init - Move a READY range lock back to INIT state
 * @inode: The fuse inode
 * @lock: The range lock previously moved to READY state
 *
 * Never blocks. Wakes waiters, since an invalidation may be waiting on
 * @lock's (until now READY) range and can now proceed around it.
 */
void fuse_range_lock_mark_init(struct fuse_inode *inode,
			      struct fuse_range_lock *lock)
{
	struct fuse_range_lock_tree *tree = &inode->io_range_lock;

	spin_lock(&tree->lock);
	lock->state = FUSE_RANGE_LOCK_INIT;
	spin_unlock(&tree->lock);

	wake_up_all(&tree->waitq);
}

/**
 * fuse_range_lock_acquire_ready - Acquire a byte range lock in READY state
 * @inode: The fuse inode
 * @lock: Caller-allocated storage for the lock (e.g. on the stack)
 * @start: Start byte offset (inclusive)
 * @end: End byte offset (inclusive)
 * @mode: FUSE_RANGE_LOCK_READ or FUSE_RANGE_LOCK_WRITE
 *
 * Blocks until [start, end] can be locked in the requested mode without
 * conflicting with any other currently held READY, overlapping range.
 * An overlapping range still in INIT state does not block this.
 */
void fuse_range_lock_acquire_ready(struct fuse_inode *inode,
				  struct fuse_range_lock *lock,
				  uint64_t start, uint64_t end,
				  enum fuse_range_lock_mode mode)
{
	struct fuse_range_lock_tree *tree = &inode->io_range_lock;

	lock->start = start;
	lock->end = end;
	lock->mode = mode;

	wait_event(tree->waitq, fuse_range_try_lock_ready(tree, lock));
}

/**
 * fuse_range_lock_release - Release a previously acquired range lock
 * @inode: The fuse inode
 * @lock: The range lock previously passed to
 *	fuse_range_lock_acquire_init() or fuse_range_lock_acquire_ready()
 */
void fuse_range_lock_release(struct fuse_inode *inode,
			    struct fuse_range_lock *lock)
{
	struct fuse_range_lock_tree *tree = &inode->io_range_lock;

	spin_lock(&tree->lock);
	fuse_range_it_remove(lock, &tree->root);
	spin_unlock(&tree->lock);

	/* Wake everyone; conflicting waiters will simply re-check and sleep. */
	wake_up_all(&tree->waitq);
}
