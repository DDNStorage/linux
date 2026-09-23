// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE: Filesystem in Userspace
 * Copyright (c) 2023-2026 DataDirect Networks.
 */

#ifndef _FS_FUSE_GDS_H
#define _FS_FUSE_GDS_H

#include <linux/dma-direction.h>

struct fuse_args_pages;
struct fuse_conn;
struct fuse_io_args;
struct request;

struct nvfs_rdma_info
{
        uint8_t    version;     /* to support future changes to structure */
        uint8_t    flags;       /* if bit 0 != 0, then gid field is valid */
        uint16_t   lid;         /* subnet local identifier of the client node port */
        uint32_t   qp_num;      /* QP number of DCT on the client node */
        uint64_t   rem_vaddr;   /* remote address */
        uint32_t   size;
        uint32_t   rkey;
        uint64_t   gid[2];      /* 16-byte global identifier of the client node port */
        uint32_t   dc_key;
};

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

bool fuse_is_gds_buffer(struct fuse_args_pages *ap);
int fuse_gds_get_gpu_sglist_rdma_info(struct fuse_conn *fc, bool write,
		struct fuse_io_args *ia);
int fuse_register_nvfs_dma_ops(struct nvfs_dma_rw_ops *ops);
void fuse_unregister_nvfs_dma_ops(void);

#endif
