// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE: Filesystem in Userspace
 * Copyright (c) 2023-2025 DataDirect Networks.
 */

#ifndef _FS_FUSE_GDS_H
#define _FS_FUSE_GDS_H

#include <linux/list.h>
#include <linux/kref.h>
#include <linux/scatterlist.h>
#include <linux/dma-buf.h>
#include "fuse_i.h"

struct request;
struct nvfs_rdma_info;

struct fuse_dmabuf_netdev {
	struct pci_dev *netdev;
	struct list_head list;
	struct rcu_head rcu;
};

struct fuse_refcnt_sgt {
	struct sg_table sgt;
	struct kref kref;
};

struct fuse_dmabuf_entry {
	spinlock_t lock;
	int fd;
	int iova_offset;
	size_t length; 		/* length of the current read/write operation (TODO) */
	struct dma_buf *dmabuf;
	struct fuse_refcnt_sgt *sgt_ref;
};

typedef struct fuse_mr_dmabuf nvfs_rdma_dmabuf;
typedef struct fuse_mr_rdma_info nvfs_rdma_info;

struct nvfs_dma_rw_ops {
	unsigned long long ft_bmap; /* feature bitmap */

	int (*nvfs_blk_rq_map_sg) (struct request_queue *q,
				   struct request *req,
				   struct scatterlist *sglist);

	int (*nvfs_dma_map_sg_attrs) (struct device *device,
				      struct scatterlist *sglist,
				      int nents,
				      enum dma_data_direction dma_dir,
				      unsigned long attrs);

	int (*nvfs_dma_unmap_sg) (struct device *device,
				  struct scatterlist *sglist,
				  int nents,
				  enum dma_data_direction dma_dir);
	bool (*nvfs_is_gpu_page) (struct page *);
	unsigned int (*nvfs_gpu_index) (struct page *page);
	unsigned int (*nvfs_device_priority) (struct device *dev, unsigned int dev_index);
	int (*nvfs_get_gpu_sglist_rdma_info) (struct scatterlist *sglist,
					      int nents,
					      struct nvfs_rdma_info *rdma_infop);
};

extern struct fuse_dmabuf fuse_dmabuf;


void fuse_dmabuf_set_sgt(struct fuse_dmabuf_entry *ent, struct fuse_refcnt_sgt *sgt_ref);
struct fuse_refcnt_sgt *fuse_dmabuf_get_sgt(struct fuse_dmabuf_entry *ent);
void fuse_dmabuf_clear_sgt(struct fuse_dmabuf_entry *ent);
void fuse_dmabuf_release_sgt(struct kref *kref);

bool fuse_is_gds_buffer(struct fuse_args_pages *ap);
int fuse_gds_map_sg(struct fuse_conn *fc, int write, struct fuse_io_args *ia);
int fuse_gds_unmap_sg(struct fuse_conn *fc, int write, struct fuse_io_args *ia);

int fuse_dmabuf_register_netdev(struct fuse_conn *fc, const char *pci_dev_name);
int fuse_dmabuf_unregister_netdev(struct fuse_conn *fc, const char *pci_dev_name);
void fuse_dmabuf_cleanup_netdev(struct fuse_conn *fc);
int fuse_create_dmabuf(struct fuse_dmabuf_entry *ent, size_t size);

int fuse_register_nvfs_dma_ops(struct nvfs_dma_rw_ops *ops);
void fuse_unregister_nvfs_dma_ops(void);
#endif
