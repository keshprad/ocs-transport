// SPDX-License-Identifier: GPL-2.0-only
#include <linux/anon_inodes.h>
#include <linux/compat.h>
#include <linux/dma-buf.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "roce_mr_uapi.h"

struct roce_mr {
	struct kref refs;
	struct mm_struct *owner_mm;
	struct page **pages;
	unsigned long npages;
	u32 access;
};

static long roce_mr_info(const struct roce_mr *mr, unsigned long arg)
{
	const struct roce_mr_info info = {
		.length = (u64)mr->npages << PAGE_SHIFT,
		.access = mr->access,
		.abi_version = ROCE_MR_ABI_VERSION,
	};

	return copy_to_user((void __user *)arg, &info, sizeof(info)) ? -EFAULT : 0;
}

static vm_fault_t roce_mr_fault(struct vm_fault *vmf)
{
	struct roce_mr *mr = vmf->vma->vm_file->private_data;

	if (vmf->pgoff >= mr->npages)
		return VM_FAULT_SIGBUS;
	return vmf_insert_pfn(vmf->vma, vmf->address,
			      page_to_pfn(mr->pages[vmf->pgoff]));
}

static const struct vm_operations_struct roce_mr_vm_ops = {
	.fault = roce_mr_fault,
};

static int roce_mr_mmap(struct file *file, struct vm_area_struct *vma)
{
	const struct roce_mr *mr = file->private_data;
	bool writable = mr->access & ROCE_MR_ACCESS_LOCAL_WRITE;

	if (vma->vm_pgoff || vma_pages(vma) != mr->npages ||
	    !(vma->vm_flags & VM_MAYSHARE))
		return -EINVAL;
	if ((vma->vm_flags & VM_EXEC) || (!writable && (vma->vm_flags & VM_WRITE)))
		return -EACCES;

	/*
	 * The VMA's file reference keeps the GUP pins until it is unmapped.
	 * Special PTEs alias anonymous folios without a second rmap entry.
	 */
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTCOPY | VM_DONTEXPAND |
		      VM_DONTDUMP);
	vm_flags_clear(vma, VM_MAYEXEC | (writable ? 0 : VM_MAYWRITE));
	vma->vm_ops = &roce_mr_vm_ops;
	return 0;
}

static void roce_mr_destroy(struct roce_mr *mr, unsigned long pinned, bool dirty)
{
	if (pinned)
		unpin_user_pages_dirty_lock(mr->pages, pinned, dirty);
	atomic64_sub(mr->npages, &mr->owner_mm->pinned_vm);
	mmdrop(mr->owner_mm);
	kvfree(mr->pages);
	kfree(mr);
}

static void roce_mr_release_last(struct kref *refs)
{
	struct roce_mr *mr = container_of(refs, struct roce_mr, refs);

	roce_mr_destroy(mr, mr->npages, mr->access & ROCE_MR_ACCESS_LOCAL_WRITE);
}

static void roce_mr_put(struct roce_mr *mr)
{
	kref_put(&mr->refs, roce_mr_release_last);
}

#if IS_ENABLED(CONFIG_DMA_SHARED_BUFFER)
static struct sg_table *roce_mr_dma_map(struct dma_buf_attachment *attachment,
				       enum dma_data_direction direction)
{
	const struct roce_mr *mr = attachment->dmabuf->priv;
	struct device *dev = attachment->dev;
	struct sg_table *table;
	struct scatterlist *segment;
	unsigned int i, max_segment;
	int error;

	if (!valid_dma_direction(direction))
		return ERR_PTR(-EINVAL);
	if (!(mr->access & ROCE_MR_ACCESS_LOCAL_WRITE) && direction != DMA_TO_DEVICE)
		return ERR_PTR(-EACCES);
	/*
	 * The application's alias stays mapped. Keep CPU and DMA access coherent
	 * without requiring userspace to synchronize every access through this fd.
	 */
	if (!dev_is_dma_coherent(dev))
		return ERR_PTR(-EOPNOTSUPP);
	max_segment = min_t(size_t, dma_get_max_seg_size(dev), dma_max_mapping_size(dev));
	if (max_segment < PAGE_SIZE)
		return ERR_PTR(-EOPNOTSUPP);

	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return ERR_PTR(-ENOMEM);
	error = sg_alloc_table_from_pages_segment(table, mr->pages, mr->npages, 0,
						  (size_t)mr->npages << PAGE_SHIFT,
						  max_segment, GFP_KERNEL);
	if (error)
		goto free_table;
	error = dma_map_sgtable(dev, table, direction, 0);
	if (error)
		goto free_segments;
	for_each_sgtable_dma_sg(table, segment, i) {
		if (dma_need_sync(dev, sg_dma_address(segment))) {
			/* A bounce copy would decouple DMA from the application's pages. */
			dma_unmap_sgtable(dev, table, direction, DMA_ATTR_SKIP_CPU_SYNC);
			error = -EOPNOTSUPP;
			goto free_segments;
		}
	}
	return table;

free_segments:
	sg_free_table(table);
free_table:
	kfree(table);
	return ERR_PTR(error);
}

static void roce_mr_dma_unmap(struct dma_buf_attachment *attachment,
			      struct sg_table *table, enum dma_data_direction direction)
{
	dma_unmap_sgtable(attachment->dev, table, direction, 0);
	sg_free_table(table);
	kfree(table);
}

static void roce_mr_dma_release(struct dma_buf *buffer)
{
	roce_mr_put(buffer->priv);
}

static const struct dma_buf_ops roce_mr_dma_ops = {
	.map_dma_buf = roce_mr_dma_map,
	.unmap_dma_buf = roce_mr_dma_unmap,
	.release = roce_mr_dma_release,
};

static int roce_mr_export_dmabuf(struct roce_mr *mr)
{
	DEFINE_DMA_BUF_EXPORT_INFO(info);
	struct dma_buf *buffer;
	int fd;

	info.ops = &roce_mr_dma_ops;
	info.size = (size_t)mr->npages << PAGE_SHIFT;
	info.flags = mr->access & ROCE_MR_ACCESS_LOCAL_WRITE ? O_RDWR : O_RDONLY;
	info.priv = mr;
	kref_get(&mr->refs);
	buffer = dma_buf_export(&info);
	if (IS_ERR(buffer)) {
		roce_mr_put(mr);
		return PTR_ERR(buffer);
	}
	fd = dma_buf_fd(buffer, O_CLOEXEC);
	if (fd < 0)
		dma_buf_put(buffer);
	return fd;
}
#endif

static long roce_mr_mapping_ioctl(struct file *file, unsigned int command,
				  unsigned long arg)
{
	struct roce_mr *mr = file->private_data;

	switch (command) {
	case ROCE_MR_GET_INFO:
		return roce_mr_info(mr, arg);
	case ROCE_MR_EXPORT_DMABUF:
#if IS_ENABLED(CONFIG_DMA_SHARED_BUFFER)
		return roce_mr_export_dmabuf(mr);
#else
		return -EOPNOTSUPP;
#endif
	default:
		return -ENOTTY;
	}
}

static int roce_mr_release(struct inode *inode, struct file *file)
{
	roce_mr_put(file->private_data);
	return 0;
}

static const struct file_operations roce_mr_mapping_ops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = roce_mr_mapping_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
	.mmap = roce_mr_mmap,
	.release = roce_mr_release,
};

static long roce_mr_register(struct file *file, unsigned int command,
			     unsigned long arg)
{
	struct roce_mr_register request;
	struct mm_struct *mm = current->mm;
	unsigned int gup_flags = FOLL_LONGTERM;
	unsigned long address, length, end, npages, i;
	struct roce_mr *mr;
	int fd, pinned;

	if (command != ROCE_MR_REGISTER)
		return -ENOTTY;
	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;

	address = request.address;
	length = request.length;
	if (!mm || request.reserved ||
	    (request.access & ~ROCE_MR_ACCESS_LOCAL_WRITE) ||
	    request.address != address || request.length != length || !length ||
	    !PAGE_ALIGNED(address) || !PAGE_ALIGNED(length) ||
	    check_add_overflow(address, length, &end))
		return -EINVAL;
	if (!access_ok((void __user *)address, length))
		return -EFAULT;

	npages = length >> PAGE_SHIFT;
	if (npages > INT_MAX)
		return -E2BIG;
	if (!can_do_mlock())
		return -EPERM;

	/* Match the per-mm long-term pin accounting used for standard MRs. */
	if ((u64)atomic64_add_return(npages, &mm->pinned_vm) >
			(rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT) &&
	    !capable(CAP_IPC_LOCK)) {
		fd = -ENOMEM;
		goto uncharge;
	}

	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr) {
		fd = -ENOMEM;
		goto uncharge;
	}
	mr->pages = kvmalloc_array(npages, sizeof(*mr->pages), GFP_KERNEL);
	if (!mr->pages) {
		kfree(mr);
		fd = -ENOMEM;
		goto uncharge;
	}
	mmgrab(mm);
	mr->owner_mm = mm;
	mr->npages = npages;
	mr->access = request.access;
	if (mr->access & ROCE_MR_ACCESS_LOCAL_WRITE)
		gup_flags |= FOLL_WRITE;

	pinned = pin_user_pages_fast(address, (int)npages, gup_flags, mr->pages);
	if (pinned != (int)npages) {
		fd = pinned < 0 ? pinned : -EFAULT;
		goto destroy;
	}
	/* Reject shared zero pages whose original VMA could replace them. */
	for (i = 0; i < npages; ++i) {
		if (is_zero_pfn(page_to_pfn(mr->pages[i]))) {
			fd = -EFAULT;
			goto destroy;
		}
	}
	kref_init(&mr->refs);
	fd = anon_inode_getfd("[roce_mr]", &roce_mr_mapping_ops, mr,
			     O_CLOEXEC | (mr->access ? O_RDWR : O_RDONLY));
	if (fd >= 0)
		return fd;
destroy:
	roce_mr_destroy(mr, max(pinned, 0), false);
	return fd;
uncharge:
	atomic64_sub(npages, &mm->pinned_vm);
	return fd;
}

static const struct file_operations roce_mr_control_ops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = roce_mr_register,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
};

static struct miscdevice roce_mr_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "roce_mr",
	.fops = &roce_mr_control_ops,
	.mode = 0600,
};

module_misc_device(roce_mr_device);

MODULE_DESCRIPTION("Scoped host memory registration for RoCEv2 workers");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
