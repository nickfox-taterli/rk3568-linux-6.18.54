// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co.Ltd
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 */

#include <linux/version.h>
#include <linux/rk-dma-heap.h>
#include <linux/file.h>
#include <linux/fdtable.h>

#if KERNEL_VERSION(5, 10, 0) <= LINUX_VERSION_CODE
#include <linux/dma-map-ops.h>
#endif

#include "rknpu_drv.h"
#include "rknpu_ioctl.h"
#include "rknpu_mem.h"

#ifdef CONFIG_ROCKCHIP_RKNPU_DMA_HEAP

int rknpu_mem_create_ioctl(struct rknpu_device *rknpu_dev, unsigned long data,
			   struct file *file)
{
	struct rknpu_mem_create args;
	int ret = -EINVAL;
	struct dma_buf_attachment *attachment;
	struct sg_table *table;
	struct scatterlist *sgl;
	dma_addr_t phys;
	struct dma_buf *dmabuf;
	struct rknpu_mem_object *rknpu_obj = NULL;
	struct rknpu_session *session = NULL;
	int fd = -1;
	bool fd_created = false;
	unsigned int length;

	if (unlikely(copy_from_user(&args, (struct rknpu_mem_create *)data,
				    sizeof(struct rknpu_mem_create)))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		ret = -EFAULT;
		return ret;
	}

	/* A contiguous CMA buffer also satisfies callers that allow SG memory. */
	if (!args.size || args.size > UINT_MAX - (PAGE_SIZE - 1))
		return -EINVAL;

	rknpu_obj = kzalloc(sizeof(*rknpu_obj), GFP_KERNEL);
	if (!rknpu_obj)
		return -ENOMEM;
	refcount_set(&rknpu_obj->refcount, 1);

	if (args.handle > 0) {
		fd = args.handle;

		dmabuf = dma_buf_get(fd);
		if (IS_ERR(dmabuf)) {
			ret = PTR_ERR(dmabuf);
			goto err_free_obj;
		}

		rknpu_obj->dmabuf = dmabuf;
		rknpu_obj->owner = 0;
	} else {
		/* Start test kernel alloc/free dma buf */
		dmabuf = rk_dma_heap_buffer_alloc(rknpu_dev->heap, args.size,
						  O_CLOEXEC | O_RDWR, 0x0,
						  dev_name(rknpu_dev->dev));
		if (IS_ERR(dmabuf)) {
			LOG_ERROR("dmabuf alloc failed, args.size = %llu\n",
				  args.size);
			ret = PTR_ERR(dmabuf);
			goto err_free_obj;
		}

		rknpu_obj->dmabuf = dmabuf;
		rknpu_obj->owner = 1;

		fd = dma_buf_fd(dmabuf, O_CLOEXEC | O_RDWR);
		if (fd < 0) {
			LOG_ERROR("dmabuf fd get failed\n");
			ret = fd;
			goto err_free_dma_buf;
		}
		fd_created = true;
		get_dma_buf(dmabuf);
	}

	attachment = dma_buf_attach(dmabuf, rknpu_dev->dev);
	if (IS_ERR(attachment)) {
		LOG_ERROR("dma_buf_attach failed\n");
		ret = PTR_ERR(attachment);
		goto err_free_dma_buf;
	}

	table = dma_buf_map_attachment(attachment, DMA_BIDIRECTIONAL);
	if (IS_ERR(table)) {
		LOG_ERROR("dma_buf_attach failed\n");
		dma_buf_detach(dmabuf, attachment);
		ret = PTR_ERR(table);
		goto err_free_dma_buf;
	}
	if (table->nents != 1) {
		ret = -EINVAL;
		goto err_detach_dma_buf;
	}

	sgl = table->sgl;
	phys = sg_dma_address(sgl);
	length = sg_dma_len(sgl);
	LOG_DEBUG("%s, %d, phys: %pad, length: %u\n", __func__,
		  __LINE__, &phys, length);

	if (args.flags & RKNPU_MEM_KERNEL_MAPPING) {
		if (length < PAGE_ALIGN(args.size)) {
			ret = -EINVAL;
			goto err_detach_dma_buf;
		}
		/* One DMA segment can comprise noncontiguous physical pages. */
		ret = dma_buf_vmap_unlocked(dmabuf, &rknpu_obj->vmap);
		if (ret)
			goto err_detach_dma_buf;
		if (rknpu_obj->vmap.is_iomem) {
			ret = -EINVAL;
			dma_buf_vunmap_unlocked(dmabuf, &rknpu_obj->vmap);
			goto err_detach_dma_buf;
		}
		rknpu_obj->kv_addr = rknpu_obj->vmap.vaddr;
	}

	rknpu_obj->size = PAGE_ALIGN(args.size);
	rknpu_obj->dma_addr = phys;
	rknpu_obj->sgt = table;
	rknpu_obj->attachment = attachment;

	args.size = rknpu_obj->size;
	args.obj_addr = (__u64)(uintptr_t)rknpu_obj;
	args.dma_addr = rknpu_obj->dma_addr;
	args.handle = fd;

	LOG_DEBUG(
		"args.handle: %d, args.size: %lld, rknpu_obj: %#llx, rknpu_obj->dma_addr: %#llx\n",
		args.handle, args.size, (__u64)(uintptr_t)rknpu_obj,
		(__u64)rknpu_obj->dma_addr);

	session = file->private_data;
	if (!session) {
		ret = -EFAULT;
		goto err_unmap_kv_addr;
	}

	if (unlikely(copy_to_user((struct rknpu_mem_create *)data, &args,
				  sizeof(struct rknpu_mem_create)))) {
		LOG_ERROR("%s: copy_to_user failed\n", __func__);
		ret = -EFAULT;
		goto err_unmap_kv_addr;
	}

	spin_lock(&rknpu_dev->lock);
	list_add_tail(&rknpu_obj->head, &session->list);

	spin_unlock(&rknpu_dev->lock);

	return 0;

err_unmap_kv_addr:
	if (rknpu_obj->kv_addr)
		dma_buf_vunmap_unlocked(dmabuf, &rknpu_obj->vmap);
	rknpu_obj->kv_addr = NULL;

err_detach_dma_buf:
	dma_buf_unmap_attachment(attachment, table, DMA_BIDIRECTIONAL);
	dma_buf_detach(dmabuf, attachment);

err_free_dma_buf:
	if (fd_created)
		close_fd(fd);
	dma_buf_put(dmabuf);

err_free_obj:
	kfree(rknpu_obj);

	return ret;
}

void rknpu_mem_object_release(struct rknpu_mem_object *rknpu_obj)
{
	if (!refcount_dec_and_test(&rknpu_obj->refcount))
		return;

	if (rknpu_obj->kv_addr)
		dma_buf_vunmap_unlocked(rknpu_obj->dmabuf,
					  &rknpu_obj->vmap);
	if (rknpu_obj->attachment) {
		dma_buf_unmap_attachment(rknpu_obj->attachment, rknpu_obj->sgt,
					DMA_BIDIRECTIONAL);
		dma_buf_detach(rknpu_obj->dmabuf, rknpu_obj->attachment);
	}
	dma_buf_put(rknpu_obj->dmabuf);
	kfree(rknpu_obj);
}

struct rknpu_mem_object *rknpu_mem_object_get(struct rknpu_session *session,
						      u64 obj_addr)
{
	struct rknpu_device *rknpu_dev;
	struct rknpu_mem_object *entry, *rknpu_obj = NULL;

	if (!session || !obj_addr)
		return NULL;

	rknpu_dev = session->rknpu_dev;
	spin_lock(&rknpu_dev->lock);
	list_for_each_entry(entry, &session->list, head) {
		if ((uintptr_t)entry == obj_addr &&
		    refcount_inc_not_zero(&entry->refcount)) {
			rknpu_obj = entry;
			break;
		}
	}
	spin_unlock(&rknpu_dev->lock);

	return rknpu_obj;
}

int rknpu_mem_destroy_ioctl(struct rknpu_device *rknpu_dev, unsigned long data,
			    struct file *file)
{
	struct rknpu_mem_object *rknpu_obj, *entry, *q;
	struct rknpu_session *session = NULL;
	struct rknpu_mem_destroy args;
	bool found = false;
	int ret = -EFAULT;

	if (unlikely(copy_from_user(&args, (struct rknpu_mem_destroy *)data,
				    sizeof(struct rknpu_mem_destroy)))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		ret = -EFAULT;
		return ret;
	}

	rknpu_obj = (struct rknpu_mem_object *)(uintptr_t)args.obj_addr;

	spin_lock(&rknpu_dev->lock);
	session = file->private_data;
	if (!session) {
		spin_unlock(&rknpu_dev->lock);
		ret = -EFAULT;
		return ret;
	}
	list_for_each_entry_safe(entry, q, &session->list, head) {
		if (entry == rknpu_obj) {
			list_del_init(&entry->head);
			found = true;
			break;
		}
	}
	spin_unlock(&rknpu_dev->lock);

	if (!found)
		return -EINVAL;

	LOG_DEBUG(
		"free args.handle: %d, rknpu_obj: %#llx, rknpu_obj->dma_addr: %#llx\n",
		args.handle, (__u64)(uintptr_t)rknpu_obj,
		(__u64)rknpu_obj->dma_addr);
	rknpu_mem_object_release(rknpu_obj);

	return 0;
}

/*
 * begin cpu access => for_cpu = true
 * end cpu access => for_cpu = false
 */
static void __maybe_unused rknpu_dma_buf_sync(
	struct rknpu_device *rknpu_dev, struct rknpu_mem_object *rknpu_obj,
	u32 offset, u32 length, enum dma_data_direction dir, bool for_cpu)
{
	struct device *dev = rknpu_dev->dev;
	struct sg_table *sgt = rknpu_obj->sgt;
	struct scatterlist *sg = sgt->sgl;
	dma_addr_t sg_dma_addr = sg_dma_address(sg);
	unsigned int len = 0;
	int i;

	for_each_sgtable_sg(sgt, sg, i) {
		unsigned int sg_offset, sg_left, size = 0;

		len += sg->length;
		if (len <= offset) {
			sg_dma_addr += sg->length;
			continue;
		}

		sg_left = len - offset;
		sg_offset = sg->length - sg_left;

		size = (length < sg_left) ? length : sg_left;

		if (for_cpu)
			dma_sync_single_range_for_cpu(dev, sg_dma_addr,
						      sg_offset, size, dir);
		else
			dma_sync_single_range_for_device(dev, sg_dma_addr,
							 sg_offset, size, dir);

		offset += size;
		length -= size;
		sg_dma_addr += sg->length;

		if (length == 0)
			break;
	}
}

int rknpu_mem_sync_ioctl(struct rknpu_device *rknpu_dev, unsigned long data,
			 struct file *file)
{
	struct rknpu_mem_object *rknpu_obj = NULL;
	struct rknpu_session *session = file->private_data;
	struct rknpu_mem_sync args;

	if (unlikely(copy_from_user(&args, (struct rknpu_mem_sync *)data,
				    sizeof(struct rknpu_mem_sync)))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	rknpu_obj = rknpu_mem_object_get(session, args.obj_addr);
	if (!rknpu_obj)
		return -EINVAL;

	if (args.offset > rknpu_obj->size ||
	    args.size > rknpu_obj->size - args.offset) {
		rknpu_mem_object_release(rknpu_obj);
		return -EINVAL;
	}

	if (args.flags & RKNPU_MEM_SYNC_TO_DEVICE)
		rknpu_dma_buf_sync(rknpu_dev, rknpu_obj, args.offset, args.size,
				   DMA_TO_DEVICE, false);
	if (args.flags & RKNPU_MEM_SYNC_FROM_DEVICE)
		rknpu_dma_buf_sync(rknpu_dev, rknpu_obj, args.offset, args.size,
				   DMA_FROM_DEVICE, true);

	rknpu_mem_object_release(rknpu_obj);
	return 0;
}

#endif
