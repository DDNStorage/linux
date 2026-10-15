// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE: Filesystem in Userspace
 * Copyright (c) 2023-2026 DataDirect Networks.
 */

#include <linux/delay.h>
#include <linux/scatterlist.h>
#include "fuse_i.h"

#define GDS_MOCK_TEST 0

/* NVIDIA GPU Direct Storage interface and operations */

static atomic_t nvfs_ops_refcnt = ATOMIC_INIT(0);
static struct nvfs_dma_rw_ops *nvfs_ops = NULL;

static inline struct nvfs_dma_rw_ops* get_nvfs_dma_ops(void)
{
	struct nvfs_dma_rw_ops *ops;

	rcu_read_lock();
	ops = rcu_dereference(nvfs_ops);
	if (ops)
		atomic_inc(&nvfs_ops_refcnt);
	rcu_read_unlock();
	return ops;
}

static inline void put_nvfs_dma_ops(void)
{
	atomic_dec(&nvfs_ops_refcnt);
}

int fuse_register_nvfs_dma_ops(struct nvfs_dma_rw_ops *ops)
{
	if (!ops)
		return -EINVAL;

	rcu_assign_pointer(nvfs_ops, ops);
	return 0;
}

void fuse_unregister_nvfs_dma_ops(void)
{
	rcu_assign_pointer(nvfs_ops, NULL);
	synchronize_rcu();
	while (atomic_read(&nvfs_ops_refcnt) > 0)
		msleep(100);
}
EXPORT_SYMBOL_GPL(fuse_register_nvfs_dma_ops);
EXPORT_SYMBOL_GPL(fuse_unregister_nvfs_dma_ops);

/* GPU buffer detection and DMA scatter-gather mapping operations */

#if !GDS_MOCK_TEST
static bool nvfs_dma_ops_is_gds_page(struct page *page)
{
	struct nvfs_dma_rw_ops *ops = get_nvfs_dma_ops();
	if (ops) {
		bool ret = ops->nvfs_is_gpu_page(page);
		put_nvfs_dma_ops();
		return ret;
	}
	return false;
}

bool fuse_is_gds_buffer(struct fuse_args_pages *ap)
{
	struct page **pages = ap->pages;
	unsigned int num_pages = ap->num_pages;

	return ap->args.user_pages && num_pages > 0 && nvfs_dma_ops_is_gds_page(pages[0]);
}

static int nvfs_get_gpu_sglist_rdma_info(struct scatterlist *sglist,
			int nents,
			struct nvfs_rdma_info *rdma_infop)
{
	struct nvfs_dma_rw_ops *ops = get_nvfs_dma_ops();
	int rc = -EIO;
	if (ops) {
		/* The function returns the number of processed entries upon successful completion */
		rc = ops->nvfs_get_gpu_sglist_rdma_info(sglist, nents, rdma_infop);
		put_nvfs_dma_ops();
	}
	return rc > 0 ? 0: -EIO;
}

int fuse_gds_get_gpu_sglist_rdma_info(struct fuse_conn *fc, bool write,
			struct fuse_io_args *ia)
{
	struct sg_table sgt;
	struct scatterlist *sgl;
	int rc = 0;
	unsigned int i;

	if (!ia->ap.num_pages) {
		return -EINVAL;
	}

	ia->ap.args.use_gds = 0;
	if (sg_alloc_table(&sgt, ia->ap.num_pages, GFP_KERNEL)) {
		rc = -ENOMEM;
		goto out;
	}

	sgl = sgt.sgl;
	for(i = 0; i < ia->ap.num_pages; i++) {
		sg_set_page(sgl, ia->ap.pages[i], ia->ap.descs[i].length, ia->ap.descs[i].offset);
		sgl = sg_next(sgl);
	}

	rc = nvfs_get_gpu_sglist_rdma_info(sgt.sgl, ia->ap.num_pages, fuse_get_readwrite_rdma_info(ia));
	if (rc) {
		goto out_sgt;
	}

	ia->ap.args.use_gds = 1;

out_sgt:
	sg_free_table(&sgt);
out:
	return rc;
}

#else
bool fuse_is_gds_buffer(struct fuse_args_pages *ap)
{
	return true;
}

int fuse_gds_get_gpu_sglist_rdma_info(struct fuse_conn *fc, bool write,
			struct fuse_io_args *ia)
{
	struct nvfs_rdma_info *rdma_infop = fuse_get_readwrite_rdma_info(ia);
	unsigned int total_len = 0;
	for (unsigned int i = 0; i < ia->ap.num_pages; i++) {
		total_len += ia->ap.descs[i].length;
	}
	rdma_infop->version = 2;
	rdma_infop->flags = 1;
	rdma_infop->lid = 0;
	rdma_infop->qp_num = 3;
	rdma_infop->rem_vaddr = 4;
	rdma_infop->size = total_len;
	rdma_infop->rkey = 5;
	rdma_infop->gid[0] = 6;
	rdma_infop->gid[1] = 7;
	rdma_infop->dc_key = 8;

	ia->ap.args.use_gds = 1;
	return 0;
}
#endif
