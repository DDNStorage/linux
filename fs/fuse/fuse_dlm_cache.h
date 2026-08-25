/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FUSE page cache lock implementation
 */

#ifndef _FS_FUSE_DLM_CACHE_H
#define _FS_FUSE_DLM_CACHE_H

#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/list.h>
#include <linux/rwsem.h>


struct fuse_inode;
struct fuse_range_lock;

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
