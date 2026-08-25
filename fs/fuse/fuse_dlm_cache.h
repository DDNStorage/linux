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
struct fuse_dlm_range;

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

/*
 * Page cache lock manager.
 *
 * @ranges holds the grants the client has been given and not had taken
 * back.  A request still on the wire covers nothing and lives on
 * @pending instead, so tree walkers never filter on state.  See enum
 * fuse_dlm_range_state in fuse_dlm_cache.c.
 */
struct fuse_dlm_cache {
	/* Lock protecting the tree and the pending list */
	struct rw_semaphore lock;
	/* Interval tree of granted ranges (FUSE_DLM_RANGE_READ/_WRITE) */
	struct rb_root_cached ranges;
	/*
	 * FUSE_DLM_WB_LOCK requests in flight (REQUESTED, or REVOKED once
	 * a revoke has overlapped one).  Owned by the queueing thread; the
	 * revoke paths mark them only.
	 */
	struct list_head pending;
};

/* Initialize a page cache lock manager */
int fuse_dlm_cache_init(struct fuse_inode *inode);

/* Clean up a page cache lock manager */
void fuse_dlm_cache_release_locks(struct fuse_inode *inode);

/* Lock a range of pages */
int fuse_dlm_lock_range(struct fuse_inode *inode, uint64_t start,
			uint64_t end, enum fuse_page_lock_mode mode);

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

/* Check if a page range is already locked */
bool fuse_dlm_range_is_locked(struct fuse_inode *inode, uint64_t start,
			      uint64_t end, enum fuse_page_lock_mode mode);

/* Re-validate a fuse_get_dlm_lock() grant against the live lock tree */
bool fuse_dlm_lock_is_held(struct fuse_inode *inode, loff_t offset,
			   size_t length, enum fuse_page_lock_mode mode);

/* Is any part of the file held for write? */
bool fuse_dlm_write_grant_exists(struct fuse_inode *inode);

/* This is the interface to the filesystem */
int fuse_get_dlm_lock(struct file *file, loff_t offset,
		      size_t length, enum fuse_page_lock_mode mode);

#endif /* _FS_FUSE_DLM_CACHE_H */
