// SPDX-License-Identifier: GPL-2.0-only
#include <linux/anon_inodes.h>
#include <linux/compat.h>
#include <linux/cred.h>
#include <linux/dma-buf.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/nsproxy.h>
#include <linux/overflow.h>
#include <linux/pid.h>
#include <linux/random.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <crypto/algapi.h>
#include <net/genetlink.h>
#include <net/sock.h>

#include "roce_mr_uapi.h"

struct roce_mr {
	struct kref refs;
	struct mm_struct *owner_mm;
	struct page **pages;
	unsigned long npages;
	u32 access;
};

static const struct file_operations roce_mr_mapping_ops;

static int roce_mr_new_fd(struct roce_mr *mr)
{
	struct file *file;
	int fd;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;
	file = anon_inode_getfile("[roce_mr]", &roce_mr_mapping_ops, mr,
				  mr->access ? O_RDWR : O_RDONLY);
	if (IS_ERR(file)) {
		put_unused_fd(fd);
		return PTR_ERR(file);
	}
	fd_install(fd, file);
	return fd;
}

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

#if IS_ENABLED(CONFIG_NET)
#define ROCE_MR_TICKET_LIFETIME (30 * HZ)
#define ROCE_MR_MAX_TICKETS 1024

struct roce_mr_pending_ticket {
	struct list_head link;
	struct file *exporter;
	struct pid *worker;
	const struct cred *owner;
	struct net *net;
	struct roce_mr_ticket token;
	unsigned long expires;
};

static DEFINE_MUTEX(roce_mr_tickets_lock);
static LIST_HEAD(roce_mr_tickets);
static unsigned int roce_mr_ticket_count;
static void roce_mr_reap_tickets(struct work_struct *work);
static DECLARE_DELAYED_WORK(roce_mr_ticket_work, roce_mr_reap_tickets);

static struct genl_family roce_mr_genl_family = {
	.name = ROCE_MR_GENL_NAME,
	.version = ROCE_MR_GENL_VERSION,
	.maxattr = ROCE_MR_GENL_A_PAD,
	.netnsok = true,
	.module = THIS_MODULE,
};

static bool roce_mr_same_user(const struct cred *a, const struct cred *b)
{
	return a->user_ns == b->user_ns && uid_eq(a->euid, b->euid);
}

static void roce_mr_free_ticket(struct roce_mr_pending_ticket *ticket)
{
	put_pid(ticket->worker);
	put_cred(ticket->owner);
	put_net(ticket->net);
	kfree(ticket);
}

static void roce_mr_remove_ticket(struct roce_mr_pending_ticket *ticket)
{
	list_del(&ticket->link);
	--roce_mr_ticket_count;
	roce_mr_free_ticket(ticket);
}

static void roce_mr_expire_tickets(void)
{
	struct roce_mr_pending_ticket *ticket, *next;

	list_for_each_entry_safe(ticket, next, &roce_mr_tickets, link) {
		if (time_before(jiffies, ticket->expires))
			break;
		roce_mr_remove_ticket(ticket);
	}
}

static void roce_mr_reap_tickets(struct work_struct *work)
{
	struct roce_mr_pending_ticket *ticket;

	mutex_lock(&roce_mr_tickets_lock);
	roce_mr_expire_tickets();
	ticket = list_first_entry_or_null(&roce_mr_tickets,
					struct roce_mr_pending_ticket, link);
	if (ticket)
		schedule_delayed_work(&roce_mr_ticket_work,
				      max_t(long, 1, ticket->expires - jiffies));
	mutex_unlock(&roce_mr_tickets_lock);
}

static void roce_mr_cancel_ticket(struct file *file)
{
	struct roce_mr_pending_ticket *ticket;

	mutex_lock(&roce_mr_tickets_lock);
	list_for_each_entry(ticket, &roce_mr_tickets, link) {
		if (ticket->exporter == file) {
			roce_mr_remove_ticket(ticket);
			break;
		}
	}
	mutex_unlock(&roce_mr_tickets_lock);
}

static struct pid *roce_mr_worker_pid(int fd, struct net *net)
{
	const struct cred *cred;
	struct task_struct *task;
	struct socket *socket;
	struct pid *worker = NULL;
	bool same_net;
	int ret;

	socket = sockfd_lookup(fd, &ret);
	if (!socket)
		return ERR_PTR(ret);
	if (socket->ops->family != AF_UNIX ||
	    (socket->type != SOCK_STREAM && socket->type != SOCK_SEQPACKET) ||
	    READ_ONCE(socket->state) != SS_CONNECTED) {
		ret = -ENOTCONN;
		goto out_socket;
	}
	spin_lock(&socket->sk->sk_peer_lock);
	cred = socket->sk->sk_peer_cred;
	if (cred && roce_mr_same_user(cred, current_cred()))
		worker = get_pid(socket->sk->sk_peer_pid);
	spin_unlock(&socket->sk->sk_peer_lock);
	ret = -EACCES;
out_socket:
	sockfd_put(socket);
	if (!worker)
		return ERR_PTR(ret);

	task = get_pid_task(worker, PIDTYPE_TGID);
	if (!task) {
		ret = -ESRCH;
		goto out_pid;
	}
	cred = get_task_cred(task);
	ret = roce_mr_same_user(cred, current_cred()) ? 0 : -EACCES;
	put_cred(cred);
	task_lock(task);
	same_net = task->nsproxy && task->nsproxy->net_ns == net;
	task_unlock(task);
	put_task_struct(task);
	if (!ret && !same_net)
		ret = -EXDEV;
	if (!ret)
		return worker;
out_pid:
	put_pid(worker);
	return ERR_PTR(ret);
}

static int roce_mr_notify_ticket(struct roce_mr_pending_ticket *ticket, u32 portid, u64 cookie)
{
	struct sk_buff *skb;
	void *header;

	skb = genlmsg_new(nla_total_size(sizeof(ticket->token)) +
			  nla_total_size_64bit(sizeof(cookie)),
			  GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	header = genlmsg_put(skb, 0, 0, &roce_mr_genl_family, 0,
			     ROCE_MR_GENL_CMD_TICKET);
	if (!header || nla_put(skb, ROCE_MR_GENL_A_TICKET, sizeof(ticket->token),
			       &ticket->token) ||
	    nla_put_u64_64bit(skb, ROCE_MR_GENL_A_COOKIE, cookie, ROCE_MR_GENL_A_PAD)) {
		nlmsg_free(skb);
		return -EMSGSIZE;
	}
	genlmsg_end(skb, header);
	return genlmsg_unicast(ticket->net, skb, portid);
}

static long roce_mr_export_ticket(struct file *file, unsigned long arg)
{
	struct roce_mr_export_ticket request;
	struct roce_mr_pending_ticket *ticket, *other;
	struct net *net = current->nsproxy->net_ns;
	struct pid *worker;
	int ret = 0;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (!request.portid)
		return -EINVAL;
	worker = roce_mr_worker_pid(request.worker_socket, net);
	if (IS_ERR(worker))
		return PTR_ERR(worker);
	ticket = kzalloc(sizeof(*ticket), GFP_KERNEL);
	if (!ticket) {
		put_pid(worker);
		return -ENOMEM;
	}
	ticket->exporter = file;
	ticket->worker = worker;
	ticket->owner = get_current_cred();
	ticket->net = get_net(net);
	get_random_bytes(&ticket->token, sizeof(ticket->token));

	mutex_lock(&roce_mr_tickets_lock);
	roce_mr_expire_tickets();
	if (roce_mr_ticket_count >= ROCE_MR_MAX_TICKETS) {
		ret = -ENOSPC;
		goto out;
	}
	list_for_each_entry(other, &roce_mr_tickets, link) {
		if (other->exporter == file) {
			ret = -EBUSY;
			goto out;
		}
		if (!crypto_memneq(&other->token, &ticket->token, sizeof(ticket->token))) {
			ret = -EAGAIN;
			goto out;
		}
	}
	ret = roce_mr_notify_ticket(ticket, request.portid, request.cookie);
	if (ret < 0)
		goto out;
	/* Importers take the same lock, including those woken by the notification. */
	ticket->expires = jiffies + ROCE_MR_TICKET_LIFETIME;
	if (list_empty(&roce_mr_tickets))
		schedule_delayed_work(&roce_mr_ticket_work, ROCE_MR_TICKET_LIFETIME);
	list_add_tail(&ticket->link, &roce_mr_tickets);
	++roce_mr_ticket_count;
	mutex_unlock(&roce_mr_tickets_lock);
	return 0;
out:
	mutex_unlock(&roce_mr_tickets_lock);
	roce_mr_free_ticket(ticket);
	return ret;
}

static long roce_mr_import_ticket(unsigned long arg)
{
	struct roce_mr_pending_ticket *ticket;
	struct roce_mr_ticket token;
	struct roce_mr *mr;
	int fd = -ENOENT;

	if (copy_from_user(&token, (void __user *)arg, sizeof(token)))
		return -EFAULT;
	mutex_lock(&roce_mr_tickets_lock);
	roce_mr_expire_tickets();
	list_for_each_entry(ticket, &roce_mr_tickets, link) {
		if (crypto_memneq(&ticket->token, &token, sizeof(token)))
			continue;
		if (ticket->worker != task_tgid(current) ||
		    !roce_mr_same_user(ticket->owner, current_cred()) ||
		    ticket->net != current->nsproxy->net_ns) {
			fd = -EACCES;
			break;
		}
		/* Exporter file release cannot drop its MR ref while this lock is held. */
		mr = ticket->exporter->private_data;
		kref_get(&mr->refs);
		fd = roce_mr_new_fd(mr);
		if (fd >= 0)
			roce_mr_remove_ticket(ticket);
		else
			roce_mr_put(mr);
		break;
	}
	mutex_unlock(&roce_mr_tickets_lock);
	return fd;
}

static int roce_mr_tickets_init(void)
{
	return genl_register_family(&roce_mr_genl_family);
}

static void roce_mr_tickets_exit(void)
{
	cancel_delayed_work_sync(&roce_mr_ticket_work);
	genl_unregister_family(&roce_mr_genl_family);
}
#else
static long roce_mr_export_ticket(struct file *file, unsigned long arg)
{
	return -EOPNOTSUPP;
}

static long roce_mr_import_ticket(unsigned long arg)
{
	return -EOPNOTSUPP;
}

static void roce_mr_cancel_ticket(struct file *file) {}
static int roce_mr_tickets_init(void) { return 0; }
static void roce_mr_tickets_exit(void) {}
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
	case ROCE_MR_EXPORT_TICKET:
		return roce_mr_export_ticket(file, arg);
	default:
		return -ENOTTY;
	}
}

static int roce_mr_release(struct inode *inode, struct file *file)
{
	roce_mr_cancel_ticket(file);
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
	fd = roce_mr_new_fd(mr);
	if (fd >= 0)
		return fd;
destroy:
	roce_mr_destroy(mr, max(pinned, 0), false);
	return fd;
uncharge:
	atomic64_sub(npages, &mm->pinned_vm);
	return fd;
}

static long roce_mr_control_ioctl(struct file *file, unsigned int command,
				  unsigned long arg)
{
	if (command == ROCE_MR_IMPORT_TICKET)
		return roce_mr_import_ticket(arg);
	return roce_mr_register(file, command, arg);
}

static const struct file_operations roce_mr_control_ops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = roce_mr_control_ioctl,
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

static int __init roce_mr_init(void)
{
	int ret = roce_mr_tickets_init();

	if (ret)
		return ret;
	ret = misc_register(&roce_mr_device);
	if (ret)
		roce_mr_tickets_exit();
	return ret;
}

static void __exit roce_mr_exit(void)
{
	misc_deregister(&roce_mr_device);
	roce_mr_tickets_exit();
}

module_init(roce_mr_init);
module_exit(roce_mr_exit);

MODULE_DESCRIPTION("Scoped host memory registration for RoCEv2 workers");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
