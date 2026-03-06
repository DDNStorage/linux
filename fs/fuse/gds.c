// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE: Filesystem in Userspace
 * Copyright (c) 2023-2025 DataDirect Networks.
 */

#include <linux/module.h>
#include <linux/dma-buf.h>
#include <linux/refcount.h>
#include <rdma/ib_umem.h>
#include <linux/pci.h>
#include <linux/dma-direct.h>
#include "gds.h"

#define GDS_MOCK_TEST 0

/* NVIDIA GPU Direct Storage interface and operations */

static atomic_t nvfs_ops_refcnt = ATOMIC_INIT(0);
static struct nvfs_dma_rw_ops *nvfs_ops = NULL;

static struct nvfs_dma_rw_ops* get_nvfs_dma_ops(void)
{
	struct nvfs_dma_rw_ops *ops;

	rcu_read_lock();
	ops = rcu_dereference(nvfs_ops);
	if (ops)
		atomic_inc(&nvfs_ops_refcnt);
	rcu_read_unlock();
	return ops;
}

static void put_nvfs_dma_ops(void)
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


/* DMA-buf operations for GPU Direct Storage memory regions */

static int fuse_dmabuf_attach(struct dma_buf *dmabuf,
			    struct dma_buf_attachment *attach)
{
	struct fuse_dmabuf_entry *entry = (struct fuse_dmabuf_entry *)dmabuf->priv;
	struct fuse_refcnt_sgt *sgt_ref;

	struct ib_umem_dmabuf *umem_dma_buf = (struct ib_umem_dmabuf *)attach->importer_priv;

	if (umem_dma_buf->umem.address != 0) {
		return -EINVAL;
	}

	if (umem_dma_buf->umem.length > dmabuf->size) {
		return -EINVAL;
	}

	sgt_ref = fuse_dmabuf_get_sgt(entry);
	if (!sgt_ref) {
		return -EINVAL;
	}

	entry->length = umem_dma_buf->umem.length;
	attach->priv = sgt_ref;
	return 0;
}

static void fuse_dmabuf_detach(struct dma_buf *dmabuf,
			    struct dma_buf_attachment *attach)
{
	struct fuse_refcnt_sgt *sgt_ref = (struct fuse_refcnt_sgt *)attach->priv;
	kref_put(&sgt_ref->kref, fuse_dmabuf_release_sgt);
}

static struct sg_table *fuse_dmabuf_map_dma(struct dma_buf_attachment *attach,
					      enum dma_data_direction direction)
{
	struct fuse_refcnt_sgt *sgt_ref = (struct fuse_refcnt_sgt *)attach->priv;
	return &sgt_ref->sgt;
}

static void fuse_dmabuf_unmap_dma(struct dma_buf_attachment *attach,
				    struct sg_table *sgt,
				    enum dma_data_direction direction)
{
}

static void fuse_dmabuf_release(struct dma_buf *dmabuf)
{
}

static const struct dma_buf_ops fuse_gds_dmabuf_ops = {
	.attach		= fuse_dmabuf_attach,
	.detach		= fuse_dmabuf_detach,
	.map_dma_buf	= fuse_dmabuf_map_dma,
	.unmap_dma_buf	= fuse_dmabuf_unmap_dma,
	.release	= fuse_dmabuf_release,
};


int fuse_create_dmabuf(struct fuse_dmabuf_entry *ent, size_t size)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	int dmabuf_fd;

	spin_lock_init(&ent->lock);
	exp_info.ops = &fuse_gds_dmabuf_ops;
	exp_info.size = size;
	exp_info.flags = O_RDWR | O_CLOEXEC;

	exp_info.priv = ent;
	exp_info.exp_name = "fuse_gds";
	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		return PTR_ERR(dmabuf);
	}
	ent->dmabuf = dmabuf;

	dmabuf_fd = dma_buf_fd(dmabuf, O_RDWR | O_CLOEXEC);
	if (dmabuf_fd < 0) {
		dma_buf_put(dmabuf);	// double check
		return dmabuf_fd;
	}
	ent->fd = dmabuf_fd;
	return 0;
}


/* Reference-counted scatter-gather table management for DMA-buf entries */

void fuse_dmabuf_release_sgt(struct kref *kref)
{
	struct fuse_refcnt_sgt *sgt_ref = container_of(kref, struct fuse_refcnt_sgt, kref);

	sg_free_table(&sgt_ref->sgt);
	kfree(sgt_ref);
}

void fuse_dmabuf_set_sgt(struct fuse_dmabuf_entry *ent, struct fuse_refcnt_sgt *sgt_ref)
{
	struct fuse_refcnt_sgt *old_sgt_ref;

	spin_lock(&ent->lock);
	old_sgt_ref = ent->sgt_ref;
	ent->sgt_ref = sgt_ref;
	spin_unlock(&ent->lock);
	if (old_sgt_ref)
		kref_put(&old_sgt_ref->kref, fuse_dmabuf_release_sgt);
}

struct fuse_refcnt_sgt *fuse_dmabuf_get_sgt(struct fuse_dmabuf_entry *ent)
{
	struct fuse_refcnt_sgt *sgt_ref;
	spin_lock(&ent->lock);
	sgt_ref = ent->sgt_ref;
	if (sgt_ref)
		kref_get(&sgt_ref->kref);
	spin_unlock(&ent->lock);
	return sgt_ref;
}

void fuse_dmabuf_clear_sgt(struct fuse_dmabuf_entry *ent)
{
	fuse_dmabuf_set_sgt(ent, NULL);
}


/* Network device registration management for GDS operations */

int fuse_dmabuf_register_netdev(struct fuse_conn *fc, const char *pci_dev_name)
{
	struct fuse_dmabuf_netdev *entry, *new_entry;
	struct pci_dev *pci_dev;
	int domain, bus, dev, fn;
	int ret = 0;

	if (sscanf(pci_dev_name, "%x:%x:%x.%d", &domain, &bus, &dev, &fn) != 4) {
		return -EINVAL;
	}

	pci_dev = pci_get_domain_bus_and_slot(domain, bus, PCI_DEVFN(dev, fn));
	if (!pci_dev) {
		return -ENODEV;
	}

	new_entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!new_entry) {
		ret = -ENOMEM;
		goto out;
	}
	new_entry->netdev = pci_dev;

	spin_lock(&fc->gds_netdev_lock);
	list_for_each_entry_rcu(entry, &fc->gds_netdev_list, list) {
		if (entry->netdev == pci_dev) {
			ret = -EINVAL;
			goto out_unlock;
		}
	}
	list_add_rcu(&new_entry->list, &fc->gds_netdev_list);
	spin_unlock(&fc->gds_netdev_lock);
	return 0;

out_unlock:
	spin_unlock(&fc->gds_netdev_lock);
	kfree(new_entry);
out:
	pci_dev_put(pci_dev);
	return ret;
}

int fuse_dmabuf_unregister_netdev(struct fuse_conn *fc, const char *pci_dev_name)
{
	struct fuse_dmabuf_netdev *entry;
	struct pci_dev *pci_dev;
	int domain, bus, dev, fn;
	bool found = false;

	if (sscanf(pci_dev_name, "%x:%x:%x.%d", &domain, &bus, &dev, &fn) != 4) {
		return -EINVAL;
	}

	pci_dev = pci_get_domain_bus_and_slot(domain, bus, PCI_DEVFN(dev, fn));
	if (!pci_dev) {
		return -ENODEV;
	}

	spin_lock(&fc->gds_netdev_lock);
	list_for_each_entry_rcu(entry, &fc->gds_netdev_list, list) {
		if (entry->netdev == pci_dev) {
			found = true;
			list_del_rcu(&entry->list);
			pci_dev_put(entry->netdev);
		}
	}
	spin_unlock(&fc->gds_netdev_lock);
	pci_dev_put(pci_dev);

	if (!found) {
		return -ENOENT;
	}
	kfree_rcu(entry, rcu);
	return 0;
}

void fuse_dmabuf_cleanup_netdev(struct fuse_conn *fc)
{
	struct fuse_dmabuf_netdev *entry;

	spin_lock(&fc->gds_netdev_lock);
	list_for_each_entry_rcu(entry, &fc->gds_netdev_list, list) {
		list_del_rcu(&entry->list);
		pci_dev_put(entry->netdev);
		kfree_rcu(entry, rcu);
	}
	spin_unlock(&fc->gds_netdev_lock);
}


/* GPU buffer detection and DMA scatter-gather mapping operations */

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

#if GDS_MOCK_TEST
	return true;
#else
	return ap->args.user_pages && num_pages > 1 && nvfs_dma_ops_is_gds_page(pages[0]);
#endif
}


static int nvfs_dma_ops_dma_map_sg(struct device *device,
			struct scatterlist *sglist,
			int nents,
			enum dma_data_direction dma_dir,
			unsigned long attrs)
{
	struct nvfs_dma_rw_ops *ops = get_nvfs_dma_ops();
	int err = -EIO;
	if (ops) {
		err = ops->nvfs_dma_map_sg_attrs(device, sglist, nents, dma_dir, attrs);
		put_nvfs_dma_ops();
	}
	return err ? -EIO : 0;
}

static int nvfs_dma_ops_dma_unmap_sg(struct device *device,
			struct scatterlist *sglist,
			int nents,
			enum dma_data_direction dma_dir)
{
	struct nvfs_dma_rw_ops *ops = get_nvfs_dma_ops();
	int err = -EIO;
	if (ops) {
		err = ops->nvfs_dma_unmap_sg(device, sglist, nents, dma_dir);
		put_nvfs_dma_ops();
	}
	return err ? -EIO : 0;
}

static int nvfs_dma_ops_dma_map_sg_mock(struct device *dev,
				struct scatterlist *sglist,
				int nents,
				enum dma_data_direction dma_dir)
{
	const struct dma_map_ops *ops = dev->dma_ops;
	dma_set_min_align_mask(dev, 0xfff); /* TEMP: for testing */

	int new_nents = dma_map_sg(dev, sglist, nents, dma_dir);
	if (new_nents == 0) {
		return -ENOMEM;
	}
	return 0;
}

static int nvfs_dma_ops_dma_unmap_sg_mock(struct device *dev,
			struct scatterlist *sglist,
			int nents,
			enum dma_data_direction dma_dir)
{
	dma_unmap_sg(dev, sglist, nents, dma_dir);
	return 0;
}

int fuse_gds_map_sg(struct fuse_conn *fc, int write, struct fuse_io_args *ia)
{
	struct fuse_refcnt_sgt *sgt_ref;
	struct scatterlist *sg;
	struct pci_dev *dev = NULL;
	struct fuse_dmabuf_netdev *entry;
	struct fuse_mr_in *mr_in = &ia->ap.args.mr.mr_in;
	int err = 0;
	unsigned int i;

	if (!ia->ap.num_pages) {
		return -EINVAL;
	}

	// TEMP: use only the first netdev
	rcu_read_lock();
	list_for_each_entry_rcu(entry, &fc->gds_netdev_list, list) {
		dev = pci_dev_get(entry->netdev);
		break;
	}
	rcu_read_unlock();
	if (!dev) {
		return -ENODEV;
	}

	sgt_ref = kzalloc(sizeof(*sgt_ref), GFP_KERNEL);
	if (!sgt_ref) {
		err = -ENOMEM;
		goto out;
	}
	kref_init(&sgt_ref->kref);

	if (sg_alloc_table(&sgt_ref->sgt, ia->ap.num_pages, GFP_KERNEL)) {
		err = -ENOMEM;
		goto out_sgt;
	}

	sg = sgt_ref->sgt.sgl;
	for(i = 0; i < ia->ap.num_pages; i++) {
		sg_set_page(sg, ia->ap.pages[i], ia->ap.descs[i].length + ia->ap.descs[i].offset, 0);
		sg = sg_next(sg);
	}

#if GDS_MOCK_TEST
	err = nvfs_dma_ops_dma_map_sg_mock(dev->dev.parent, sgt_ref->sgt.sgl, ia->ap.num_pages,
					write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
#else
	err = nvfs_dma_ops_dma_map_sg(dev->dev.parent, sgt_ref->sgt.sgl, ia->ap.num_pages,
					write ? DMA_TO_DEVICE : DMA_FROM_DEVICE, 0);
#endif

	ia->ap.args.is_gds = 1;
	mr_in->type = FUSE_MR_DMABUF;
	mr_in->rdma_dmabuf.sgt = (uint64_t)sgt_ref;
	mr_in->rdma_dmabuf.iova_offset = ia->ap.descs[0].offset;
	if (write)
		ia->write.in.write_flags |= FUSE_WRITE_GDS;
	else
		ia->read.in.read_flags |= FUSE_READ_GDS; /* not used for now */
	return err;

out_sgt:
	kfree(sgt_ref);
out:
	pci_dev_put(dev);
	return err;
}

int fuse_gds_unmap_sg(struct fuse_conn *fc, int write, struct fuse_io_args *ia)
{
	struct sg_table *sgt = (struct sg_table *)ia->ap.args.mr.mr_in.rdma_dmabuf.sgt;
	struct pci_dev *dev = NULL;
	struct fuse_dmabuf_netdev *entry;
	int err;

	/* TODO: handle netdev list change between map and unmap */
	rcu_read_lock();
	list_for_each_entry_rcu(entry, &fc->gds_netdev_list, list) {
		dev = pci_dev_get(entry->netdev);
		break;
	}
	rcu_read_unlock();
	if (!dev)
		return -ENODEV;

#if GDS_MOCK_TEST
	err = nvfs_dma_ops_dma_unmap_sg_mock(dev->dev.parent, sgt->sgl, sgt->nents,
					write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
#else
	err = nvfs_dma_ops_dma_unmap_sg(dev->dev.parent, sgt->sgl, sgt->nents,
					write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
#endif

	pci_dev_put(dev);
	return err;
}

MODULE_IMPORT_NS(DMA_BUF);
