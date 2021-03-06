/*
 * NVM Express device driver
 * Copyright (c) 2011-2014, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/bitops.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/hdreg.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kdev_t.h>
#include <linux/kthread.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pci.h>
#include <linux/poison.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/t10-pi.h>
#include <linux/types.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <asm/unaligned.h>

#include "nvme.h"

#define NVME_Q_DEPTH		1024
#define NVME_AQ_DEPTH		256
#define SQ_SIZE(depth)		(depth * sizeof(struct nvme_command))
#define CQ_SIZE(depth)		(depth * sizeof(struct nvme_completion))

unsigned char admin_timeout = 60;
module_param(admin_timeout, byte, 0644);
MODULE_PARM_DESC(admin_timeout, "timeout in seconds for admin commands");

unsigned char nvme_io_timeout = 30;
module_param_named(io_timeout, nvme_io_timeout, byte, 0644);
MODULE_PARM_DESC(io_timeout, "timeout in seconds for I/O");

unsigned char shutdown_timeout = 5;
module_param(shutdown_timeout, byte, 0644);
MODULE_PARM_DESC(shutdown_timeout, "timeout in seconds for controller shutdown");

static int use_threaded_interrupts;
module_param(use_threaded_interrupts, int, 0);

static bool use_cmb_sqes = true;
module_param(use_cmb_sqes, bool, 0644);
MODULE_PARM_DESC(use_cmb_sqes, "use controller's memory buffer for I/O SQes");

static LIST_HEAD(dev_list);
static struct task_struct *nvme_thread;
static struct workqueue_struct *nvme_workq;
static wait_queue_head_t nvme_kthread_wait;

struct nvme_dev;
struct nvme_queue;
struct nvme_iod;

static int __nvme_reset(struct nvme_dev *dev);
static int nvme_reset(struct nvme_dev *dev);
static void nvme_process_cq(struct nvme_queue *nvmeq);
static void nvme_unmap_data(struct nvme_dev *dev, struct nvme_iod *iod);
static void nvme_dead_ctrl(struct nvme_dev *dev);

struct async_cmd_info {
	struct kthread_work work;
	struct kthread_worker *worker;
	struct request *req;
	u32 result;
	int status;
	void *ctx;
};

/*
 * Represents an NVM Express device.  Each nvme_dev is a PCI function.
 */
struct nvme_dev {
	struct list_head node;
	struct nvme_queue **queues;
	struct blk_mq_tag_set tagset;
	struct blk_mq_tag_set admin_tagset;
	u32 __iomem *dbs;
	struct device *dev;
	struct dma_pool *prp_page_pool;
	struct dma_pool *prp_small_pool;
	unsigned queue_count;
	unsigned online_queues;
	unsigned max_qid;
	int q_depth;
	u32 db_stride;
	struct msix_entry *entry;
	void __iomem *bar;
	struct work_struct reset_work;
	struct work_struct probe_work;
	struct work_struct scan_work;
	bool subsystem;
	void __iomem *cmb;
	dma_addr_t cmb_dma_addr;
	u64 cmb_size;
	u32 cmbsz;

	struct nvme_ctrl ctrl;
};

static inline struct nvme_dev *to_nvme_dev(struct nvme_ctrl *ctrl)
{
	return container_of(ctrl, struct nvme_dev, ctrl);
}

/*
 * An NVM Express queue.  Each device has at least two (one for admin
 * commands and one for I/O commands).
 */
struct nvme_queue {
	struct device *q_dmadev;
	struct nvme_dev *dev;
	char irqname[24];	/* nvme4294967295-65535\0 */
	spinlock_t q_lock;
	struct nvme_command *sq_cmds;
	struct nvme_command __iomem *sq_cmds_io;
	volatile struct nvme_completion *cqes;
	struct blk_mq_tags **tags;
	dma_addr_t sq_dma_addr;
	dma_addr_t cq_dma_addr;
	u32 __iomem *q_db;
	u16 q_depth;
	s16 cq_vector;
	u16 sq_head;
	u16 sq_tail;
	u16 cq_head;
	u16 qid;
	u8 cq_phase;
	u8 cqe_seen;
	struct async_cmd_info cmdinfo;
};

/*
 * The nvme_iod describes the data in an I/O, including the list of PRP
 * entries.  You can't see it in this data structure because C doesn't let
 * me express that.  Use nvme_alloc_iod to ensure there's enough space
 * allocated to store the PRP list.
 */
struct nvme_iod {
	unsigned long private;	/* For the use of the submitter of the I/O */
	int npages;		/* In the PRP list. 0 means small pool in use */
	int offset;		/* Of PRP list */
	int nents;		/* Used in scatterlist */
	int length;		/* Of data, in bytes */
	dma_addr_t first_dma;
	struct scatterlist meta_sg[1]; /* metadata requires single contiguous buffer */
	struct scatterlist sg[0];
};

/*
 * Check we didin't inadvertently grow the command struct
 */
static inline void _nvme_check_size(void)
{
	BUILD_BUG_ON(sizeof(struct nvme_rw_command) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_create_cq) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_create_sq) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_delete_queue) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_features) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_format_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_abort_cmd) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_command) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_id_ctrl) != 4096);
	BUILD_BUG_ON(sizeof(struct nvme_id_ns) != 4096);
	BUILD_BUG_ON(sizeof(struct nvme_lba_range_type) != 64);
	BUILD_BUG_ON(sizeof(struct nvme_smart_log) != 512);
}

typedef void (*nvme_completion_fn)(struct nvme_queue *, void *,
						struct nvme_completion *);

struct nvme_cmd_info {
	nvme_completion_fn fn;
	void *ctx;
	int aborted;
	struct nvme_queue *nvmeq;
	struct nvme_iod iod[0];
};

/*
 * Max size of iod being embedded in the request payload
 */
#define NVME_INT_PAGES		2
#define NVME_INT_BYTES(dev)	(NVME_INT_PAGES * (dev)->ctrl.page_size)
#define NVME_INT_MASK		0x01

/*
 * Will slightly overestimate the number of pages needed.  This is OK
 * as it only leads to a small amount of wasted memory for the lifetime of
 * the I/O.
 */
static int nvme_npages(unsigned size, struct nvme_dev *dev)
{
	unsigned nprps = DIV_ROUND_UP(size + dev->ctrl.page_size,
				      dev->ctrl.page_size);
	return DIV_ROUND_UP(8 * nprps, PAGE_SIZE - 8);
}

static unsigned int nvme_cmd_size(struct nvme_dev *dev)
{
	unsigned int ret = sizeof(struct nvme_cmd_info);

	ret += sizeof(struct nvme_iod);
	ret += sizeof(__le64 *) * nvme_npages(NVME_INT_BYTES(dev), dev);
	ret += sizeof(struct scatterlist) * NVME_INT_PAGES;

	return ret;
}

static int nvme_admin_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
				unsigned int hctx_idx)
{
	struct nvme_dev *dev = data;
	struct nvme_queue *nvmeq = dev->queues[0];

	WARN_ON(hctx_idx != 0);
	WARN_ON(dev->admin_tagset.tags[0] != hctx->tags);
	WARN_ON(nvmeq->tags);

	hctx->driver_data = nvmeq;
	nvmeq->tags = &dev->admin_tagset.tags[0];
	return 0;
}

static void nvme_admin_exit_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx)
{
	struct nvme_queue *nvmeq = hctx->driver_data;

	nvmeq->tags = NULL;
}

static int nvme_admin_init_request(void *data, struct request *req,
				unsigned int hctx_idx, unsigned int rq_idx,
				unsigned int numa_node)
{
	struct nvme_dev *dev = data;
	struct nvme_cmd_info *cmd = blk_mq_rq_to_pdu(req);
	struct nvme_queue *nvmeq = dev->queues[0];

	BUG_ON(!nvmeq);
	cmd->nvmeq = nvmeq;
	return 0;
}

static int nvme_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
			  unsigned int hctx_idx)
{
	struct nvme_dev *dev = data;
	struct nvme_queue *nvmeq = dev->queues[hctx_idx + 1];

	if (!nvmeq->tags)
		nvmeq->tags = &dev->tagset.tags[hctx_idx];

	WARN_ON(dev->tagset.tags[hctx_idx] != hctx->tags);
	hctx->driver_data = nvmeq;
	return 0;
}

static int nvme_init_request(void *data, struct request *req,
				unsigned int hctx_idx, unsigned int rq_idx,
				unsigned int numa_node)
{
	struct nvme_dev *dev = data;
	struct nvme_cmd_info *cmd = blk_mq_rq_to_pdu(req);
	struct nvme_queue *nvmeq = dev->queues[hctx_idx + 1];

	BUG_ON(!nvmeq);
	cmd->nvmeq = nvmeq;
	return 0;
}

static void nvme_set_info(struct nvme_cmd_info *cmd, void *ctx,
				nvme_completion_fn handler)
{
	cmd->fn = handler;
	cmd->ctx = ctx;
	cmd->aborted = 0;
	blk_mq_start_request(blk_mq_rq_from_pdu(cmd));
}

static void *iod_get_private(struct nvme_iod *iod)
{
	return (void *) (iod->private & ~0x1UL);
}

/*
 * If bit 0 is set, the iod is embedded in the request payload.
 */
static bool iod_should_kfree(struct nvme_iod *iod)
{
	return (iod->private & NVME_INT_MASK) == 0;
}

/* Special values must be less than 0x1000 */
#define CMD_CTX_BASE		((void *)POISON_POINTER_DELTA)
#define CMD_CTX_CANCELLED	(0x30C + CMD_CTX_BASE)
#define CMD_CTX_COMPLETED	(0x310 + CMD_CTX_BASE)
#define CMD_CTX_INVALID		(0x314 + CMD_CTX_BASE)

static void special_completion(struct nvme_queue *nvmeq, void *ctx,
						struct nvme_completion *cqe)
{
	if (ctx == CMD_CTX_CANCELLED)
		return;
	if (ctx == CMD_CTX_COMPLETED) {
		dev_warn(nvmeq->q_dmadev,
				"completed id %d twice on queue %d\n",
				cqe->command_id, le16_to_cpup(&cqe->sq_id));
		return;
	}
	if (ctx == CMD_CTX_INVALID) {
		dev_warn(nvmeq->q_dmadev,
				"invalid id %d completed on queue %d\n",
				cqe->command_id, le16_to_cpup(&cqe->sq_id));
		return;
	}
	dev_warn(nvmeq->q_dmadev, "Unknown special completion %p\n", ctx);
}

static void *cancel_cmd_info(struct nvme_cmd_info *cmd, nvme_completion_fn *fn)
{
	void *ctx;

	if (fn)
		*fn = cmd->fn;
	ctx = cmd->ctx;
	cmd->fn = special_completion;
	cmd->ctx = CMD_CTX_CANCELLED;
	return ctx;
}

static void async_req_completion(struct nvme_queue *nvmeq, void *ctx,
						struct nvme_completion *cqe)
{
	u32 result = le32_to_cpup(&cqe->result);
	u16 status = le16_to_cpup(&cqe->status) >> 1;

	if (status == NVME_SC_SUCCESS || status == NVME_SC_ABORT_REQ)
		++nvmeq->dev->ctrl.event_limit;
	if (status != NVME_SC_SUCCESS)
		return;

	switch (result & 0xff07) {
	case NVME_AER_NOTICE_NS_CHANGED:
		dev_info(nvmeq->q_dmadev, "rescanning\n");
		schedule_work(&nvmeq->dev->scan_work);
	default:
		dev_warn(nvmeq->q_dmadev, "async event result %08x\n", result);
	}
}

static void abort_completion(struct nvme_queue *nvmeq, void *ctx,
						struct nvme_completion *cqe)
{
	struct request *req = ctx;

	u16 status = le16_to_cpup(&cqe->status) >> 1;
	u32 result = le32_to_cpup(&cqe->result);

	blk_mq_free_request(req);

	dev_warn(nvmeq->q_dmadev, "Abort status:%x result:%x", status, result);
	++nvmeq->dev->ctrl.abort_limit;
}

static void async_completion(struct nvme_queue *nvmeq, void *ctx,
						struct nvme_completion *cqe)
{
	struct async_cmd_info *cmdinfo = ctx;
	cmdinfo->result = le32_to_cpup(&cqe->result);
	cmdinfo->status = le16_to_cpup(&cqe->status) >> 1;
	queue_kthread_work(cmdinfo->worker, &cmdinfo->work);
	blk_mq_free_request(cmdinfo->req);
}

static inline struct nvme_cmd_info *get_cmd_from_tag(struct nvme_queue *nvmeq,
				  unsigned int tag)
{
	struct request *req = blk_mq_tag_to_rq(*nvmeq->tags, tag);

	return blk_mq_rq_to_pdu(req);
}

/*
 * Called with local interrupts disabled and the q_lock held.  May not sleep.
 */
static void *nvme_finish_cmd(struct nvme_queue *nvmeq, int tag,
						nvme_completion_fn *fn)
{
	struct nvme_cmd_info *cmd = get_cmd_from_tag(nvmeq, tag);
	void *ctx;
	if (tag >= nvmeq->q_depth) {
		*fn = special_completion;
		return CMD_CTX_INVALID;
	}
	if (fn)
		*fn = cmd->fn;
	ctx = cmd->ctx;
	cmd->fn = special_completion;
	cmd->ctx = CMD_CTX_COMPLETED;
	return ctx;
}

/**
 * nvme_submit_cmd() - Copy a command into a queue and ring the doorbell
 * @nvmeq: The queue to use
 * @cmd: The command to send
 *
 * Safe to use from interrupt context
 */
static void __nvme_submit_cmd(struct nvme_queue *nvmeq,
						struct nvme_command *cmd)
{
	u16 tail = nvmeq->sq_tail;

	if (nvmeq->sq_cmds_io)
		memcpy_toio(&nvmeq->sq_cmds_io[tail], cmd, sizeof(*cmd));
	else
		memcpy(&nvmeq->sq_cmds[tail], cmd, sizeof(*cmd));

	if (++tail == nvmeq->q_depth)
		tail = 0;
	writel(tail, nvmeq->q_db);
	nvmeq->sq_tail = tail;
}

static void nvme_submit_cmd(struct nvme_queue *nvmeq, struct nvme_command *cmd)
{
	unsigned long flags;
	spin_lock_irqsave(&nvmeq->q_lock, flags);
	__nvme_submit_cmd(nvmeq, cmd);
	spin_unlock_irqrestore(&nvmeq->q_lock, flags);
}

static __le64 **iod_list(struct nvme_iod *iod)
{
	return ((void *)iod) + iod->offset;
}

static inline void iod_init(struct nvme_iod *iod, unsigned nbytes,
			    unsigned nseg, unsigned long private)
{
	iod->private = private;
	iod->offset = offsetof(struct nvme_iod, sg[nseg]);
	iod->npages = -1;
	iod->length = nbytes;
	iod->nents = 0;
}

static struct nvme_iod *
__nvme_alloc_iod(unsigned nseg, unsigned bytes, struct nvme_dev *dev,
		 unsigned long priv, gfp_t gfp)
{
	struct nvme_iod *iod = kmalloc(sizeof(struct nvme_iod) +
				sizeof(__le64 *) * nvme_npages(bytes, dev) +
				sizeof(struct scatterlist) * nseg, gfp);

	if (iod)
		iod_init(iod, bytes, nseg, priv);

	return iod;
}

static struct nvme_iod *nvme_alloc_iod(struct request *rq, struct nvme_dev *dev,
			               gfp_t gfp)
{
	unsigned size = !(rq->cmd_flags & REQ_DISCARD) ? blk_rq_bytes(rq) :
                                                sizeof(struct nvme_dsm_range);
	struct nvme_iod *iod;

	if (rq->nr_phys_segments <= NVME_INT_PAGES &&
	    size <= NVME_INT_BYTES(dev)) {
		struct nvme_cmd_info *cmd = blk_mq_rq_to_pdu(rq);

		iod = cmd->iod;
		iod_init(iod, size, rq->nr_phys_segments,
				(unsigned long) rq | NVME_INT_MASK);
		return iod;
	}

	return __nvme_alloc_iod(rq->nr_phys_segments, size, dev,
				(unsigned long) rq, gfp);
}

static void nvme_free_iod(struct nvme_dev *dev, struct nvme_iod *iod)
{
	const int last_prp = dev->ctrl.page_size / 8 - 1;
	int i;
	__le64 **list = iod_list(iod);
	dma_addr_t prp_dma = iod->first_dma;

	if (iod->npages == 0)
		dma_pool_free(dev->prp_small_pool, list[0], prp_dma);
	for (i = 0; i < iod->npages; i++) {
		__le64 *prp_list = list[i];
		dma_addr_t next_prp_dma = le64_to_cpu(prp_list[last_prp]);
		dma_pool_free(dev->prp_page_pool, prp_list, prp_dma);
		prp_dma = next_prp_dma;
	}

	if (iod_should_kfree(iod))
		kfree(iod);
}

#ifdef CONFIG_BLK_DEV_INTEGRITY
static void nvme_dif_prep(u32 p, u32 v, struct t10_pi_tuple *pi)
{
	if (be32_to_cpu(pi->ref_tag) == v)
		pi->ref_tag = cpu_to_be32(p);
}

static void nvme_dif_complete(u32 p, u32 v, struct t10_pi_tuple *pi)
{
	if (be32_to_cpu(pi->ref_tag) == p)
		pi->ref_tag = cpu_to_be32(v);
}

/**
 * nvme_dif_remap - remaps ref tags to bip seed and physical lba
 *
 * The virtual start sector is the one that was originally submitted by the
 * block layer.	Due to partitioning, MD/DM cloning, etc. the actual physical
 * start sector may be different. Remap protection information to match the
 * physical LBA on writes, and back to the original seed on reads.
 *
 * Type 0 and 3 do not have a ref tag, so no remapping required.
 */
static void nvme_dif_remap(struct request *req,
			void (*dif_swap)(u32 p, u32 v, struct t10_pi_tuple *pi))
{
	struct nvme_ns *ns = req->rq_disk->private_data;
	struct bio_integrity_payload *bip;
	struct t10_pi_tuple *pi;
	void *p, *pmap;
	u32 i, nlb, ts, phys, virt;

	if (!ns->pi_type || ns->pi_type == NVME_NS_DPS_PI_TYPE3)
		return;

	bip = bio_integrity(req->bio);
	if (!bip)
		return;

	pmap = kmap_atomic(bip->bip_vec->bv_page) + bip->bip_vec->bv_offset;

	p = pmap;
	virt = bip_get_seed(bip);
	phys = nvme_block_nr(ns, blk_rq_pos(req));
	nlb = (blk_rq_bytes(req) >> ns->lba_shift);
	ts = ns->disk->queue->integrity.tuple_size;

	for (i = 0; i < nlb; i++, virt++, phys++) {
		pi = (struct t10_pi_tuple *)p;
		dif_swap(phys, virt, pi);
		p += ts;
	}
	kunmap_atomic(pmap);
}
#else /* CONFIG_BLK_DEV_INTEGRITY */
static void nvme_dif_remap(struct request *req,
			void (*dif_swap)(u32 p, u32 v, struct t10_pi_tuple *pi))
{
}
static void nvme_dif_prep(u32 p, u32 v, struct t10_pi_tuple *pi)
{
}
static void nvme_dif_complete(u32 p, u32 v, struct t10_pi_tuple *pi)
{
}
#endif

static void req_completion(struct nvme_queue *nvmeq, void *ctx,
						struct nvme_completion *cqe)
{
	struct nvme_iod *iod = ctx;
	struct request *req = iod_get_private(iod);
	struct nvme_cmd_info *cmd_rq = blk_mq_rq_to_pdu(req);
	u16 status = le16_to_cpup(&cqe->status) >> 1;
	int error = 0;

	if (unlikely(status)) {
		if (!(status & NVME_SC_DNR || blk_noretry_request(req))
		    && (jiffies - req->start_time) < req->timeout) {
			unsigned long flags;

			nvme_unmap_data(nvmeq->dev, iod);

			blk_mq_requeue_request(req);
			spin_lock_irqsave(req->q->queue_lock, flags);
			if (!blk_queue_stopped(req->q))
				blk_mq_kick_requeue_list(req->q);
			spin_unlock_irqrestore(req->q->queue_lock, flags);
			return;
		}

		if (req->cmd_type == REQ_TYPE_DRV_PRIV) {
			if (cmd_rq->ctx == CMD_CTX_CANCELLED)
				error = -EINTR;
			else
				error = status;
		} else {
			error = nvme_error_status(status);
		}
	}

	if (req->cmd_type == REQ_TYPE_DRV_PRIV) {
		u32 result = le32_to_cpup(&cqe->result);
		req->special = (void *)(uintptr_t)result;
	}

	if (cmd_rq->aborted)
		dev_warn(nvmeq->dev->dev,
			"completing aborted command with status:%04x\n",
			error);

	nvme_unmap_data(nvmeq->dev, iod);
	blk_mq_complete_request(req, error);
}

static bool nvme_setup_prps(struct nvme_dev *dev, struct nvme_iod *iod,
		int total_len)
{
	struct dma_pool *pool;
	int length = total_len;
	struct scatterlist *sg = iod->sg;
	int dma_len = sg_dma_len(sg);
	u64 dma_addr = sg_dma_address(sg);
	u32 page_size = dev->ctrl.page_size;
	int offset = dma_addr & (page_size - 1);
	__le64 *prp_list;
	__le64 **list = iod_list(iod);
	dma_addr_t prp_dma;
	int nprps, i;

	length -= (page_size - offset);
	if (length <= 0)
		return true;

	dma_len -= (page_size - offset);
	if (dma_len) {
		dma_addr += (page_size - offset);
	} else {
		sg = sg_next(sg);
		dma_addr = sg_dma_address(sg);
		dma_len = sg_dma_len(sg);
	}

	if (length <= page_size) {
		iod->first_dma = dma_addr;
		return true;
	}

	nprps = DIV_ROUND_UP(length, page_size);
	if (nprps <= (256 / 8)) {
		pool = dev->prp_small_pool;
		iod->npages = 0;
	} else {
		pool = dev->prp_page_pool;
		iod->npages = 1;
	}

	prp_list = dma_pool_alloc(pool, GFP_ATOMIC, &prp_dma);
	if (!prp_list) {
		iod->first_dma = dma_addr;
		iod->npages = -1;
		return false;
	}
	list[0] = prp_list;
	iod->first_dma = prp_dma;
	i = 0;
	for (;;) {
		if (i == page_size >> 3) {
			__le64 *old_prp_list = prp_list;
			prp_list = dma_pool_alloc(pool, GFP_ATOMIC, &prp_dma);
			if (!prp_list)
				return false;
			list[iod->npages++] = prp_list;
			prp_list[0] = old_prp_list[i - 1];
			old_prp_list[i - 1] = cpu_to_le64(prp_dma);
			i = 1;
		}
		prp_list[i++] = cpu_to_le64(dma_addr);
		dma_len -= page_size;
		dma_addr += page_size;
		length -= page_size;
		if (length <= 0)
			break;
		if (dma_len > 0)
			continue;
		BUG_ON(dma_len < 0);
		sg = sg_next(sg);
		dma_addr = sg_dma_address(sg);
		dma_len = sg_dma_len(sg);
	}

	return true;
}

static int nvme_map_data(struct nvme_dev *dev, struct nvme_iod *iod,
		struct nvme_command *cmnd)
{
	struct request *req = iod_get_private(iod);
	struct request_queue *q = req->q;
	enum dma_data_direction dma_dir = rq_data_dir(req) ?
			DMA_TO_DEVICE : DMA_FROM_DEVICE;
	int ret = BLK_MQ_RQ_QUEUE_ERROR;

	sg_init_table(iod->sg, req->nr_phys_segments);
	iod->nents = blk_rq_map_sg(q, req, iod->sg);
	if (!iod->nents)
		goto out;

	ret = BLK_MQ_RQ_QUEUE_BUSY;
	if (!dma_map_sg(dev->dev, iod->sg, iod->nents, dma_dir))
		goto out;

	if (!nvme_setup_prps(dev, iod, blk_rq_bytes(req)))
		goto out_unmap;

	ret = BLK_MQ_RQ_QUEUE_ERROR;
	if (blk_integrity_rq(req)) {
		if (blk_rq_count_integrity_sg(q, req->bio) != 1)
			goto out_unmap;

		sg_init_table(iod->meta_sg, 1);
		if (blk_rq_map_integrity_sg(q, req->bio, iod->meta_sg) != 1)
			goto out_unmap;

		if (rq_data_dir(req))
			nvme_dif_remap(req, nvme_dif_prep);

		if (!dma_map_sg(dev->dev, iod->meta_sg, 1, dma_dir))
			goto out_unmap;
	}

	cmnd->rw.prp1 = cpu_to_le64(sg_dma_address(iod->sg));
	cmnd->rw.prp2 = cpu_to_le64(iod->first_dma);
	if (blk_integrity_rq(req))
		cmnd->rw.metadata = cpu_to_le64(sg_dma_address(iod->meta_sg));
	return BLK_MQ_RQ_QUEUE_OK;

out_unmap:
	dma_unmap_sg(dev->dev, iod->sg, iod->nents, dma_dir);
out:
	return ret;
}

static void nvme_unmap_data(struct nvme_dev *dev, struct nvme_iod *iod)
{
	struct request *req = iod_get_private(iod);
	enum dma_data_direction dma_dir = rq_data_dir(req) ?
			DMA_TO_DEVICE : DMA_FROM_DEVICE;

	if (iod->nents) {
		dma_unmap_sg(dev->dev, iod->sg, iod->nents, dma_dir);
		if (blk_integrity_rq(req)) {
			if (!rq_data_dir(req))
				nvme_dif_remap(req, nvme_dif_complete);
			dma_unmap_sg(dev->dev, iod->meta_sg, 1, dma_dir);
		}
	}

	nvme_free_iod(dev, iod);
}

/*
 * We reuse the small pool to allocate the 16-byte range here as it is not
 * worth having a special pool for these or additional cases to handle freeing
 * the iod.
 */
static int nvme_setup_discard(struct nvme_queue *nvmeq, struct nvme_ns *ns,
		struct nvme_iod *iod, struct nvme_command *cmnd)
{
	struct request *req = iod_get_private(iod);
	struct nvme_dsm_range *range;

	range = dma_pool_alloc(nvmeq->dev->prp_small_pool, GFP_ATOMIC,
						&iod->first_dma);
	if (!range)
		return BLK_MQ_RQ_QUEUE_BUSY;
	iod_list(iod)[0] = (__le64 *)range;
	iod->npages = 0;

	range->cattr = cpu_to_le32(0);
	range->nlb = cpu_to_le32(blk_rq_bytes(req) >> ns->lba_shift);
	range->slba = cpu_to_le64(nvme_block_nr(ns, blk_rq_pos(req)));

	memset(cmnd, 0, sizeof(*cmnd));
	cmnd->dsm.opcode = nvme_cmd_dsm;
	cmnd->dsm.nsid = cpu_to_le32(ns->ns_id);
	cmnd->dsm.prp1 = cpu_to_le64(iod->first_dma);
	cmnd->dsm.nr = 0;
	cmnd->dsm.attributes = cpu_to_le32(NVME_DSMGMT_AD);
	return BLK_MQ_RQ_QUEUE_OK;
}

/*
 * NOTE: ns is NULL when called on the admin queue.
 */
static int nvme_queue_rq(struct blk_mq_hw_ctx *hctx,
			 const struct blk_mq_queue_data *bd)
{
	struct nvme_ns *ns = hctx->queue->queuedata;
	struct nvme_queue *nvmeq = hctx->driver_data;
	struct nvme_dev *dev = nvmeq->dev;
	struct request *req = bd->rq;
	struct nvme_cmd_info *cmd = blk_mq_rq_to_pdu(req);
	struct nvme_iod *iod;
	struct nvme_command cmnd;
	int ret = BLK_MQ_RQ_QUEUE_OK;

	/*
	 * If formated with metadata, require the block layer provide a buffer
	 * unless this namespace is formated such that the metadata can be
	 * stripped/generated by the controller with PRACT=1.
	 */
	if (ns && ns->ms && !blk_integrity_rq(req)) {
		if (!(ns->pi_type && ns->ms == 8) &&
					req->cmd_type != REQ_TYPE_DRV_PRIV) {
			blk_mq_complete_request(req, -EFAULT);
			return BLK_MQ_RQ_QUEUE_OK;
		}
	}

	iod = nvme_alloc_iod(req, dev, GFP_ATOMIC);
	if (!iod)
		return BLK_MQ_RQ_QUEUE_BUSY;

	if (req->cmd_flags & REQ_DISCARD) {
		ret = nvme_setup_discard(nvmeq, ns, iod, &cmnd);
	} else {
		if (req->cmd_type == REQ_TYPE_DRV_PRIV)
			memcpy(&cmnd, req->cmd, sizeof(cmnd));
		else if (req->cmd_flags & REQ_FLUSH)
			nvme_setup_flush(ns, &cmnd);
		else
			nvme_setup_rw(ns, req, &cmnd);

		if (req->nr_phys_segments)
			ret = nvme_map_data(dev, iod, &cmnd);
	}

	if (ret)
		goto out;

	cmnd.common.command_id = req->tag;
	nvme_set_info(cmd, iod, req_completion);

	spin_lock_irq(&nvmeq->q_lock);
	__nvme_submit_cmd(nvmeq, &cmnd);
	nvme_process_cq(nvmeq);
	spin_unlock_irq(&nvmeq->q_lock);
	return BLK_MQ_RQ_QUEUE_OK;
out:
	nvme_free_iod(dev, iod);
	return ret;
}

static void __nvme_process_cq(struct nvme_queue *nvmeq, unsigned int *tag)
{
	u16 head, phase;

	head = nvmeq->cq_head;
	phase = nvmeq->cq_phase;

	for (;;) {
		void *ctx;
		nvme_completion_fn fn;
		struct nvme_completion cqe = nvmeq->cqes[head];
		if ((le16_to_cpu(cqe.status) & 1) != phase)
			break;
		nvmeq->sq_head = le16_to_cpu(cqe.sq_head);
		if (++head == nvmeq->q_depth) {
			head = 0;
			phase = !phase;
		}
		if (tag && *tag == cqe.command_id)
			*tag = -1;
		ctx = nvme_finish_cmd(nvmeq, cqe.command_id, &fn);
		fn(nvmeq, ctx, &cqe);
	}

	/* If the controller ignores the cq head doorbell and continuously
	 * writes to the queue, it is theoretically possible to wrap around
	 * the queue twice and mistakenly return IRQ_NONE.  Linux only
	 * requires that 0.1% of your interrupts are handled, so this isn't
	 * a big problem.
	 */
	if (head == nvmeq->cq_head && phase == nvmeq->cq_phase)
		return;

	if (likely(nvmeq->cq_vector >= 0))
		writel(head, nvmeq->q_db + nvmeq->dev->db_stride);
	nvmeq->cq_head = head;
	nvmeq->cq_phase = phase;

	nvmeq->cqe_seen = 1;
}

static void nvme_process_cq(struct nvme_queue *nvmeq)
{
	__nvme_process_cq(nvmeq, NULL);
}

static irqreturn_t nvme_irq(int irq, void *data)
{
	irqreturn_t result;
	struct nvme_queue *nvmeq = data;
	spin_lock(&nvmeq->q_lock);
	nvme_process_cq(nvmeq);
	result = nvmeq->cqe_seen ? IRQ_HANDLED : IRQ_NONE;
	nvmeq->cqe_seen = 0;
	spin_unlock(&nvmeq->q_lock);
	return result;
}

static irqreturn_t nvme_irq_check(int irq, void *data)
{
	struct nvme_queue *nvmeq = data;
	struct nvme_completion cqe = nvmeq->cqes[nvmeq->cq_head];
	if ((le16_to_cpu(cqe.status) & 1) != nvmeq->cq_phase)
		return IRQ_NONE;
	return IRQ_WAKE_THREAD;
}

static int nvme_poll(struct blk_mq_hw_ctx *hctx, unsigned int tag)
{
	struct nvme_queue *nvmeq = hctx->driver_data;

	if ((le16_to_cpu(nvmeq->cqes[nvmeq->cq_head].status) & 1) ==
	    nvmeq->cq_phase) {
		spin_lock_irq(&nvmeq->q_lock);
		__nvme_process_cq(nvmeq, &tag);
		spin_unlock_irq(&nvmeq->q_lock);

		if (tag == -1)
			return 1;
	}

	return 0;
}

static int nvme_submit_async_admin_req(struct nvme_dev *dev)
{
	struct nvme_queue *nvmeq = dev->queues[0];
	struct nvme_command c;
	struct nvme_cmd_info *cmd_info;
	struct request *req;

	req = blk_mq_alloc_request(dev->ctrl.admin_q, WRITE,
			BLK_MQ_REQ_NOWAIT | BLK_MQ_REQ_RESERVED);
	if (IS_ERR(req))
		return PTR_ERR(req);

	req->cmd_flags |= REQ_NO_TIMEOUT;
	cmd_info = blk_mq_rq_to_pdu(req);
	nvme_set_info(cmd_info, NULL, async_req_completion);

	memset(&c, 0, sizeof(c));
	c.common.opcode = nvme_admin_async_event;
	c.common.command_id = req->tag;

	blk_mq_free_request(req);
	__nvme_submit_cmd(nvmeq, &c);
	return 0;
}

static int nvme_submit_admin_async_cmd(struct nvme_dev *dev,
			struct nvme_command *cmd,
			struct async_cmd_info *cmdinfo, unsigned timeout)
{
	struct nvme_queue *nvmeq = dev->queues[0];
	struct request *req;
	struct nvme_cmd_info *cmd_rq;

	req = blk_mq_alloc_request(dev->ctrl.admin_q, WRITE, 0);
	if (IS_ERR(req))
		return PTR_ERR(req);

	req->timeout = timeout;
	cmd_rq = blk_mq_rq_to_pdu(req);
	cmdinfo->req = req;
	nvme_set_info(cmd_rq, cmdinfo, async_completion);
	cmdinfo->status = -EINTR;

	cmd->common.command_id = req->tag;

	nvme_submit_cmd(nvmeq, cmd);
	return 0;
}

static int adapter_delete_queue(struct nvme_dev *dev, u8 opcode, u16 id)
{
	struct nvme_command c;

	memset(&c, 0, sizeof(c));
	c.delete_queue.opcode = opcode;
	c.delete_queue.qid = cpu_to_le16(id);

	return nvme_submit_sync_cmd(dev->ctrl.admin_q, &c, NULL, 0);
}

static int adapter_alloc_cq(struct nvme_dev *dev, u16 qid,
						struct nvme_queue *nvmeq)
{
	struct nvme_command c;
	int flags = NVME_QUEUE_PHYS_CONTIG | NVME_CQ_IRQ_ENABLED;

	/*
	 * Note: we (ab)use the fact the the prp fields survive if no data
	 * is attached to the request.
	 */
	memset(&c, 0, sizeof(c));
	c.create_cq.opcode = nvme_admin_create_cq;
	c.create_cq.prp1 = cpu_to_le64(nvmeq->cq_dma_addr);
	c.create_cq.cqid = cpu_to_le16(qid);
	c.create_cq.qsize = cpu_to_le16(nvmeq->q_depth - 1);
	c.create_cq.cq_flags = cpu_to_le16(flags);
	c.create_cq.irq_vector = cpu_to_le16(nvmeq->cq_vector);

	return nvme_submit_sync_cmd(dev->ctrl.admin_q, &c, NULL, 0);
}

static int adapter_alloc_sq(struct nvme_dev *dev, u16 qid,
						struct nvme_queue *nvmeq)
{
	struct nvme_command c;
	int flags = NVME_QUEUE_PHYS_CONTIG | NVME_SQ_PRIO_MEDIUM;

	/*
	 * Note: we (ab)use the fact the the prp fields survive if no data
	 * is attached to the request.
	 */
	memset(&c, 0, sizeof(c));
	c.create_sq.opcode = nvme_admin_create_sq;
	c.create_sq.prp1 = cpu_to_le64(nvmeq->sq_dma_addr);
	c.create_sq.sqid = cpu_to_le16(qid);
	c.create_sq.qsize = cpu_to_le16(nvmeq->q_depth - 1);
	c.create_sq.sq_flags = cpu_to_le16(flags);
	c.create_sq.cqid = cpu_to_le16(qid);

	return nvme_submit_sync_cmd(dev->ctrl.admin_q, &c, NULL, 0);
}

static int adapter_delete_cq(struct nvme_dev *dev, u16 cqid)
{
	return adapter_delete_queue(dev, nvme_admin_delete_cq, cqid);
}

static int adapter_delete_sq(struct nvme_dev *dev, u16 sqid)
{
	return adapter_delete_queue(dev, nvme_admin_delete_sq, sqid);
}

/**
 * nvme_abort_req - Attempt aborting a request
 *
 * Schedule controller reset if the command was already aborted once before and
 * still hasn't been returned to the driver, or if this is the admin queue.
 */
static void nvme_abort_req(struct request *req)
{
	struct nvme_cmd_info *cmd_rq = blk_mq_rq_to_pdu(req);
	struct nvme_queue *nvmeq = cmd_rq->nvmeq;
	struct nvme_dev *dev = nvmeq->dev;
	struct request *abort_req;
	struct nvme_cmd_info *abort_cmd;
	struct nvme_command cmd;

	if (!nvmeq->qid || cmd_rq->aborted) {
		spin_lock(&dev_list_lock);
		if (!__nvme_reset(dev)) {
			dev_warn(dev->dev,
				 "I/O %d QID %d timeout, reset controller\n",
				 req->tag, nvmeq->qid);
		}
		spin_unlock(&dev_list_lock);
		return;
	}

	if (!dev->ctrl.abort_limit)
		return;

	abort_req = blk_mq_alloc_request(dev->ctrl.admin_q, WRITE,
			BLK_MQ_REQ_NOWAIT);
	if (IS_ERR(abort_req))
		return;

	abort_cmd = blk_mq_rq_to_pdu(abort_req);
	nvme_set_info(abort_cmd, abort_req, abort_completion);

	memset(&cmd, 0, sizeof(cmd));
	cmd.abort.opcode = nvme_admin_abort_cmd;
	cmd.abort.cid = req->tag;
	cmd.abort.sqid = cpu_to_le16(nvmeq->qid);
	cmd.abort.command_id = abort_req->tag;

	--dev->ctrl.abort_limit;
	cmd_rq->aborted = 1;

	dev_warn(nvmeq->q_dmadev, "Aborting I/O %d QID %d\n", req->tag,
							nvmeq->qid);
	nvme_submit_cmd(dev->queues[0], &cmd);
}

static void nvme_cancel_queue_ios(struct request *req, void *data, bool reserved)
{
	struct nvme_queue *nvmeq = data;
	void *ctx;
	nvme_completion_fn fn;
	struct nvme_cmd_info *cmd;
	struct nvme_completion cqe;

	if (!blk_mq_request_started(req))
		return;

	cmd = blk_mq_rq_to_pdu(req);

	if (cmd->ctx == CMD_CTX_CANCELLED)
		return;

	if (blk_queue_dying(req->q))
		cqe.status = cpu_to_le16((NVME_SC_ABORT_REQ | NVME_SC_DNR) << 1);
	else
		cqe.status = cpu_to_le16(NVME_SC_ABORT_REQ << 1);


	dev_warn(nvmeq->q_dmadev, "Cancelling I/O %d QID %d\n",
						req->tag, nvmeq->qid);
	ctx = cancel_cmd_info(cmd, &fn);
	fn(nvmeq, ctx, &cqe);
}

static enum blk_eh_timer_return nvme_timeout(struct request *req, bool reserved)
{
	struct nvme_cmd_info *cmd = blk_mq_rq_to_pdu(req);
	struct nvme_queue *nvmeq = cmd->nvmeq;

	dev_warn(nvmeq->q_dmadev, "Timeout I/O %d QID %d\n", req->tag,
							nvmeq->qid);
	spin_lock_irq(&nvmeq->q_lock);
	nvme_abort_req(req);
	spin_unlock_irq(&nvmeq->q_lock);

	/*
	 * The aborted req will be completed on receiving the abort req.
	 * We enable the timer again. If hit twice, it'll cause a device reset,
	 * as the device then is in a faulty state.
	 */
	return BLK_EH_RESET_TIMER;
}

static void nvme_free_queue(struct nvme_queue *nvmeq)
{
	dma_free_coherent(nvmeq->q_dmadev, CQ_SIZE(nvmeq->q_depth),
				(void *)nvmeq->cqes, nvmeq->cq_dma_addr);
	if (nvmeq->sq_cmds)
		dma_free_coherent(nvmeq->q_dmadev, SQ_SIZE(nvmeq->q_depth),
					nvmeq->sq_cmds, nvmeq->sq_dma_addr);
	kfree(nvmeq);
}

static void nvme_free_queues(struct nvme_dev *dev, int lowest)
{
	int i;

	for (i = dev->queue_count - 1; i >= lowest; i--) {
		struct nvme_queue *nvmeq = dev->queues[i];
		dev->queue_count--;
		dev->queues[i] = NULL;
		nvme_free_queue(nvmeq);
	}
}

/**
 * nvme_suspend_queue - put queue into suspended state
 * @nvmeq - queue to suspend
 */
static int nvme_suspend_queue(struct nvme_queue *nvmeq)
{
	int vector;

	spin_lock_irq(&nvmeq->q_lock);
	if (nvmeq->cq_vector == -1) {
		spin_unlock_irq(&nvmeq->q_lock);
		return 1;
	}
	vector = nvmeq->dev->entry[nvmeq->cq_vector].vector;
	nvmeq->dev->online_queues--;
	nvmeq->cq_vector = -1;
	spin_unlock_irq(&nvmeq->q_lock);

	if (!nvmeq->qid && nvmeq->dev->ctrl.admin_q)
		blk_mq_freeze_queue_start(nvmeq->dev->ctrl.admin_q);

	irq_set_affinity_hint(vector, NULL);
	free_irq(vector, nvmeq);

	return 0;
}

static void nvme_clear_queue(struct nvme_queue *nvmeq)
{
	spin_lock_irq(&nvmeq->q_lock);
	if (nvmeq->tags && *nvmeq->tags)
		blk_mq_all_tag_busy_iter(*nvmeq->tags, nvme_cancel_queue_ios, nvmeq);
	spin_unlock_irq(&nvmeq->q_lock);
}

static void nvme_disable_queue(struct nvme_dev *dev, int qid)
{
	struct nvme_queue *nvmeq = dev->queues[qid];

	if (!nvmeq)
		return;
	if (nvme_suspend_queue(nvmeq))
		return;

	/* Don't tell the adapter to delete the admin queue.
	 * Don't tell a removed adapter to delete IO queues. */
	if (qid && readl(dev->bar + NVME_REG_CSTS) != -1) {
		adapter_delete_sq(dev, qid);
		adapter_delete_cq(dev, qid);
	}

	spin_lock_irq(&nvmeq->q_lock);
	nvme_process_cq(nvmeq);
	spin_unlock_irq(&nvmeq->q_lock);
}

static int nvme_cmb_qdepth(struct nvme_dev *dev, int nr_io_queues,
				int entry_size)
{
	int q_depth = dev->q_depth;
	unsigned q_size_aligned = roundup(q_depth * entry_size,
					  dev->ctrl.page_size);

	if (q_size_aligned * nr_io_queues > dev->cmb_size) {
		u64 mem_per_q = div_u64(dev->cmb_size, nr_io_queues);
		mem_per_q = round_down(mem_per_q, dev->ctrl.page_size);
		q_depth = div_u64(mem_per_q, entry_size);

		/*
		 * Ensure the reduced q_depth is above some threshold where it
		 * would be better to map queues in system memory with the
		 * original depth
		 */
		if (q_depth < 64)
			return -ENOMEM;
	}

	return q_depth;
}

static int nvme_alloc_sq_cmds(struct nvme_dev *dev, struct nvme_queue *nvmeq,
				int qid, int depth)
{
	if (qid && dev->cmb && use_cmb_sqes && NVME_CMB_SQS(dev->cmbsz)) {
		unsigned offset = (qid - 1) * roundup(SQ_SIZE(depth),
						      dev->ctrl.page_size);
		nvmeq->sq_dma_addr = dev->cmb_dma_addr + offset;
		nvmeq->sq_cmds_io = dev->cmb + offset;
	} else {
		nvmeq->sq_cmds = dma_alloc_coherent(dev->dev, SQ_SIZE(depth),
					&nvmeq->sq_dma_addr, GFP_KERNEL);
		if (!nvmeq->sq_cmds)
			return -ENOMEM;
	}

	return 0;
}

static struct nvme_queue *nvme_alloc_queue(struct nvme_dev *dev, int qid,
							int depth)
{
	struct nvme_queue *nvmeq = kzalloc(sizeof(*nvmeq), GFP_KERNEL);
	if (!nvmeq)
		return NULL;

	nvmeq->cqes = dma_zalloc_coherent(dev->dev, CQ_SIZE(depth),
					  &nvmeq->cq_dma_addr, GFP_KERNEL);
	if (!nvmeq->cqes)
		goto free_nvmeq;

	if (nvme_alloc_sq_cmds(dev, nvmeq, qid, depth))
		goto free_cqdma;

	nvmeq->q_dmadev = dev->dev;
	nvmeq->dev = dev;
	snprintf(nvmeq->irqname, sizeof(nvmeq->irqname), "nvme%dq%d",
			dev->ctrl.instance, qid);
	spin_lock_init(&nvmeq->q_lock);
	nvmeq->cq_head = 0;
	nvmeq->cq_phase = 1;
	nvmeq->q_db = &dev->dbs[qid * 2 * dev->db_stride];
	nvmeq->q_depth = depth;
	nvmeq->qid = qid;
	nvmeq->cq_vector = -1;
	dev->queues[qid] = nvmeq;

	/* make sure queue descriptor is set before queue count, for kthread */
	mb();
	dev->queue_count++;

	return nvmeq;

 free_cqdma:
	dma_free_coherent(dev->dev, CQ_SIZE(depth), (void *)nvmeq->cqes,
							nvmeq->cq_dma_addr);
 free_nvmeq:
	kfree(nvmeq);
	return NULL;
}

static int queue_request_irq(struct nvme_dev *dev, struct nvme_queue *nvmeq,
							const char *name)
{
	if (use_threaded_interrupts)
		return request_threaded_irq(dev->entry[nvmeq->cq_vector].vector,
					nvme_irq_check, nvme_irq, IRQF_SHARED,
					name, nvmeq);
	return request_irq(dev->entry[nvmeq->cq_vector].vector, nvme_irq,
				IRQF_SHARED, name, nvmeq);
}

static void nvme_init_queue(struct nvme_queue *nvmeq, u16 qid)
{
	struct nvme_dev *dev = nvmeq->dev;

	spin_lock_irq(&nvmeq->q_lock);
	nvmeq->sq_tail = 0;
	nvmeq->cq_head = 0;
	nvmeq->cq_phase = 1;
	nvmeq->q_db = &dev->dbs[qid * 2 * dev->db_stride];
	memset((void *)nvmeq->cqes, 0, CQ_SIZE(nvmeq->q_depth));
	dev->online_queues++;
	spin_unlock_irq(&nvmeq->q_lock);
}

static int nvme_create_queue(struct nvme_queue *nvmeq, int qid)
{
	struct nvme_dev *dev = nvmeq->dev;
	int result;

	nvmeq->cq_vector = qid - 1;
	result = adapter_alloc_cq(dev, qid, nvmeq);
	if (result < 0)
		return result;

	result = adapter_alloc_sq(dev, qid, nvmeq);
	if (result < 0)
		goto release_cq;

	result = queue_request_irq(dev, nvmeq, nvmeq->irqname);
	if (result < 0)
		goto release_sq;

	nvme_init_queue(nvmeq, qid);
	return result;

 release_sq:
	adapter_delete_sq(dev, qid);
 release_cq:
	adapter_delete_cq(dev, qid);
	return result;
}

static struct blk_mq_ops nvme_mq_admin_ops = {
	.queue_rq	= nvme_queue_rq,
	.map_queue	= blk_mq_map_queue,
	.init_hctx	= nvme_admin_init_hctx,
	.exit_hctx      = nvme_admin_exit_hctx,
	.init_request	= nvme_admin_init_request,
	.timeout	= nvme_timeout,
};

static struct blk_mq_ops nvme_mq_ops = {
	.queue_rq	= nvme_queue_rq,
	.map_queue	= blk_mq_map_queue,
	.init_hctx	= nvme_init_hctx,
	.init_request	= nvme_init_request,
	.timeout	= nvme_timeout,
	.poll		= nvme_poll,
};

static void nvme_dev_remove_admin(struct nvme_dev *dev)
{
	if (dev->ctrl.admin_q && !blk_queue_dying(dev->ctrl.admin_q)) {
		blk_cleanup_queue(dev->ctrl.admin_q);
		blk_mq_free_tag_set(&dev->admin_tagset);
	}
}

static int nvme_alloc_admin_tags(struct nvme_dev *dev)
{
	if (!dev->ctrl.admin_q) {
		dev->admin_tagset.ops = &nvme_mq_admin_ops;
		dev->admin_tagset.nr_hw_queues = 1;
		dev->admin_tagset.queue_depth = NVME_AQ_DEPTH - 1;
		dev->admin_tagset.reserved_tags = 1;
		dev->admin_tagset.timeout = ADMIN_TIMEOUT;
		dev->admin_tagset.numa_node = dev_to_node(dev->dev);
		dev->admin_tagset.cmd_size = nvme_cmd_size(dev);
		dev->admin_tagset.driver_data = dev;

		if (blk_mq_alloc_tag_set(&dev->admin_tagset))
			return -ENOMEM;

		dev->ctrl.admin_q = blk_mq_init_queue(&dev->admin_tagset);
		if (IS_ERR(dev->ctrl.admin_q)) {
			blk_mq_free_tag_set(&dev->admin_tagset);
			return -ENOMEM;
		}
		if (!blk_get_queue(dev->ctrl.admin_q)) {
			nvme_dev_remove_admin(dev);
			dev->ctrl.admin_q = NULL;
			return -ENODEV;
		}
	} else
		blk_mq_unfreeze_queue(dev->ctrl.admin_q);

	return 0;
}

static int nvme_configure_admin_queue(struct nvme_dev *dev)
{
	int result;
	u32 aqa;
	u64 cap = lo_hi_readq(dev->bar + NVME_REG_CAP);
	struct nvme_queue *nvmeq;

	dev->subsystem = readl(dev->bar + NVME_REG_VS) >= NVME_VS(1, 1) ?
						NVME_CAP_NSSRC(cap) : 0;

	if (dev->subsystem &&
	    (readl(dev->bar + NVME_REG_CSTS) & NVME_CSTS_NSSRO))
		writel(NVME_CSTS_NSSRO, dev->bar + NVME_REG_CSTS);

	result = nvme_disable_ctrl(&dev->ctrl, cap);
	if (result < 0)
		return result;

	nvmeq = dev->queues[0];
	if (!nvmeq) {
		nvmeq = nvme_alloc_queue(dev, 0, NVME_AQ_DEPTH);
		if (!nvmeq)
			return -ENOMEM;
	}

	aqa = nvmeq->q_depth - 1;
	aqa |= aqa << 16;

	writel(aqa, dev->bar + NVME_REG_AQA);
	lo_hi_writeq(nvmeq->sq_dma_addr, dev->bar + NVME_REG_ASQ);
	lo_hi_writeq(nvmeq->cq_dma_addr, dev->bar + NVME_REG_ACQ);

	result = nvme_enable_ctrl(&dev->ctrl, cap);
	if (result)
		goto free_nvmeq;

	nvmeq->cq_vector = 0;
	result = queue_request_irq(dev, nvmeq, nvmeq->irqname);
	if (result) {
		nvmeq->cq_vector = -1;
		goto free_nvmeq;
	}

	return result;

 free_nvmeq:
	nvme_free_queues(dev, 0);
	return result;
}

static int nvme_kthread(void *data)
{
	struct nvme_dev *dev, *next;

	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock(&dev_list_lock);
		list_for_each_entry_safe(dev, next, &dev_list, node) {
			int i;
			u32 csts = readl(dev->bar + NVME_REG_CSTS);

			if ((dev->subsystem && (csts & NVME_CSTS_NSSRO)) ||
							csts & NVME_CSTS_CFS) {
				if (!__nvme_reset(dev)) {
					dev_warn(dev->dev,
						"Failed status: %x, reset controller\n",
						readl(dev->bar + NVME_REG_CSTS));
				}
				continue;
			}
			for (i = 0; i < dev->queue_count; i++) {
				struct nvme_queue *nvmeq = dev->queues[i];
				if (!nvmeq)
					continue;
				spin_lock_irq(&nvmeq->q_lock);
				nvme_process_cq(nvmeq);

				while (i == 0 && dev->ctrl.event_limit > 0) {
					if (nvme_submit_async_admin_req(dev))
						break;
					dev->ctrl.event_limit--;
				}
				spin_unlock_irq(&nvmeq->q_lock);
			}
		}
		spin_unlock(&dev_list_lock);
		schedule_timeout(round_jiffies_relative(HZ));
	}
	return 0;
}

/*
 * Create I/O queues.  Failing to create an I/O queue is not an issue,
 * we can continue with less than the desired amount of queues, and
 * even a controller without I/O queues an still be used to issue
 * admin commands.  This might be useful to upgrade a buggy firmware
 * for example.
 */
static void nvme_create_io_queues(struct nvme_dev *dev)
{
	unsigned i;

	for (i = dev->queue_count; i <= dev->max_qid; i++)
		if (!nvme_alloc_queue(dev, i, dev->q_depth))
			break;

	for (i = dev->online_queues; i <= dev->queue_count - 1; i++)
		if (nvme_create_queue(dev->queues[i], i)) {
			nvme_free_queues(dev, i);
			break;
		}
}

static void __iomem *nvme_map_cmb(struct nvme_dev *dev)
{
	u64 szu, size, offset;
	u32 cmbloc;
	resource_size_t bar_size;
	struct pci_dev *pdev = to_pci_dev(dev->dev);
	void __iomem *cmb;
	dma_addr_t dma_addr;

	if (!use_cmb_sqes)
		return NULL;

	dev->cmbsz = readl(dev->bar + NVME_REG_CMBSZ);
	if (!(NVME_CMB_SZ(dev->cmbsz)))
		return NULL;

	cmbloc = readl(dev->bar + NVME_REG_CMBLOC);

	szu = (u64)1 << (12 + 4 * NVME_CMB_SZU(dev->cmbsz));
	size = szu * NVME_CMB_SZ(dev->cmbsz);
	offset = szu * NVME_CMB_OFST(cmbloc);
	bar_size = pci_resource_len(pdev, NVME_CMB_BIR(cmbloc));

	if (offset > bar_size)
		return NULL;

	/*
	 * Controllers may support a CMB size larger than their BAR,
	 * for example, due to being behind a bridge. Reduce the CMB to
	 * the reported size of the BAR
	 */
	if (size > bar_size - offset)
		size = bar_size - offset;

	dma_addr = pci_resource_start(pdev, NVME_CMB_BIR(cmbloc)) + offset;
	cmb = ioremap_wc(dma_addr, size);
	if (!cmb)
		return NULL;

	dev->cmb_dma_addr = dma_addr;
	dev->cmb_size = size;
	return cmb;
}

static inline void nvme_release_cmb(struct nvme_dev *dev)
{
	if (dev->cmb) {
		iounmap(dev->cmb);
		dev->cmb = NULL;
	}
}

static size_t db_bar_size(struct nvme_dev *dev, unsigned nr_io_queues)
{
	return 4096 + ((nr_io_queues + 1) * 8 * dev->db_stride);
}

static int nvme_setup_io_queues(struct nvme_dev *dev)
{
	struct nvme_queue *adminq = dev->queues[0];
	struct pci_dev *pdev = to_pci_dev(dev->dev);
	int result, i, vecs, nr_io_queues, size;

	nr_io_queues = num_possible_cpus();
	result = nvme_set_queue_count(&dev->ctrl, &nr_io_queues);
	if (result < 0)
		return result;

	/*
	 * Degraded controllers might return an error when setting the queue
	 * count.  We still want to be able to bring them online and offer
	 * access to the admin queue, as that might be only way to fix them up.
	 */
	if (result > 0) {
		dev_err(dev->dev, "Could not set queue count (%d)\n", result);
		nr_io_queues = 0;
		result = 0;
	}

	if (dev->cmb && NVME_CMB_SQS(dev->cmbsz)) {
		result = nvme_cmb_qdepth(dev, nr_io_queues,
				sizeof(struct nvme_command));
		if (result > 0)
			dev->q_depth = result;
		else
			nvme_release_cmb(dev);
	}

	size = db_bar_size(dev, nr_io_queues);
	if (size > 8192) {
		iounmap(dev->bar);
		do {
			dev->bar = ioremap(pci_resource_start(pdev, 0), size);
			if (dev->bar)
				break;
			if (!--nr_io_queues)
				return -ENOMEM;
			size = db_bar_size(dev, nr_io_queues);
		} while (1);
		dev->dbs = dev->bar + 4096;
		adminq->q_db = dev->dbs;
	}

	/* Deregister the admin queue's interrupt */
	free_irq(dev->entry[0].vector, adminq);

	/*
	 * If we enable msix early due to not intx, disable it again before
	 * setting up the full range we need.
	 */
	if (!pdev->irq)
		pci_disable_msix(pdev);

	for (i = 0; i < nr_io_queues; i++)
		dev->entry[i].entry = i;
	vecs = pci_enable_msix_range(pdev, dev->entry, 1, nr_io_queues);
	if (vecs < 0) {
		vecs = pci_enable_msi_range(pdev, 1, min(nr_io_queues, 32));
		if (vecs < 0) {
			vecs = 1;
		} else {
			for (i = 0; i < vecs; i++)
				dev->entry[i].vector = i + pdev->irq;
		}
	}

	/*
	 * Should investigate if there's a performance win from allocating
	 * more queues than interrupt vectors; it might allow the submission
	 * path to scale better, even if the receive path is limited by the
	 * number of interrupts.
	 */
	nr_io_queues = vecs;
	dev->max_qid = nr_io_queues;

	result = queue_request_irq(dev, adminq, adminq->irqname);
	if (result) {
		adminq->cq_vector = -1;
		goto free_queues;
	}

	/* Free previously allocated queues that are no longer usable */
	nvme_free_queues(dev, nr_io_queues + 1);
	nvme_create_io_queues(dev);

	return 0;

 free_queues:
	nvme_free_queues(dev, 1);
	return result;
}

static void nvme_set_irq_hints(struct nvme_dev *dev)
{
	struct nvme_queue *nvmeq;
	int i;

	for (i = 0; i < dev->online_queues; i++) {
		nvmeq = dev->queues[i];

		if (!nvmeq->tags || !(*nvmeq->tags))
			continue;

		irq_set_affinity_hint(dev->entry[nvmeq->cq_vector].vector,
					blk_mq_tags_cpumask(*nvmeq->tags));
	}
}

static void nvme_dev_scan(struct work_struct *work)
{
	struct nvme_dev *dev = container_of(work, struct nvme_dev, scan_work);

	if (!dev->tagset.tags)
		return;
	nvme_scan_namespaces(&dev->ctrl);
	nvme_set_irq_hints(dev);
}

/*
 * Return: error value if an error occurred setting up the queues or calling
 * Identify Device.  0 if these succeeded, even if adding some of the
 * namespaces failed.  At the moment, these failures are silent.  TBD which
 * failures should be reported.
 */
static int nvme_dev_add(struct nvme_dev *dev)
{
	if (!dev->ctrl.tagset) {
		dev->tagset.ops = &nvme_mq_ops;
		dev->tagset.nr_hw_queues = dev->online_queues - 1;
		dev->tagset.timeout = NVME_IO_TIMEOUT;
		dev->tagset.numa_node = dev_to_node(dev->dev);
		dev->tagset.queue_depth =
				min_t(int, dev->q_depth, BLK_MQ_MAX_DEPTH) - 1;
		dev->tagset.cmd_size = nvme_cmd_size(dev);
		dev->tagset.flags = BLK_MQ_F_SHOULD_MERGE;
		dev->tagset.driver_data = dev;

		if (blk_mq_alloc_tag_set(&dev->tagset))
			return 0;
		dev->ctrl.tagset = &dev->tagset;
	}
	schedule_work(&dev->scan_work);
	return 0;
}

static int nvme_dev_map(struct nvme_dev *dev)
{
	u64 cap;
	int bars, result = -ENOMEM;
	struct pci_dev *pdev = to_pci_dev(dev->dev);

	if (pci_enable_device_mem(pdev))
		return result;

	dev->entry[0].vector = pdev->irq;
	pci_set_master(pdev);
	bars = pci_select_bars(pdev, IORESOURCE_MEM);
	if (!bars)
		goto disable_pci;

	if (pci_request_selected_regions(pdev, bars, "nvme"))
		goto disable_pci;

	if (dma_set_mask_and_coherent(dev->dev, DMA_BIT_MASK(64)) &&
	    dma_set_mask_and_coherent(dev->dev, DMA_BIT_MASK(32)))
		goto disable;

	dev->bar = ioremap(pci_resource_start(pdev, 0), 8192);
	if (!dev->bar)
		goto disable;

	if (readl(dev->bar + NVME_REG_CSTS) == -1) {
		result = -ENODEV;
		goto unmap;
	}

	/*
	 * Some devices don't advertse INTx interrupts, pre-enable a single
	 * MSIX vec for setup. We'll adjust this later.
	 */
	if (!pdev->irq) {
		result = pci_enable_msix(pdev, dev->entry, 1);
		if (result < 0)
			goto unmap;
	}

	cap = lo_hi_readq(dev->bar + NVME_REG_CAP);

	dev->q_depth = min_t(int, NVME_CAP_MQES(cap) + 1, NVME_Q_DEPTH);
	dev->db_stride = 1 << NVME_CAP_STRIDE(cap);
	dev->dbs = dev->bar + 4096;

	/*
	 * Temporary fix for the Apple controller found in the MacBook8,1 and
	 * some MacBook7,1 to avoid controller resets and data loss.
	 */
	if (pdev->vendor == PCI_VENDOR_ID_APPLE && pdev->device == 0x2001) {
		dev->q_depth = 2;
		dev_warn(dev->dev, "detected Apple NVMe controller, set "
			"queue depth=%u to work around controller resets\n",
			dev->q_depth);
	}

	if (readl(dev->bar + NVME_REG_VS) >= NVME_VS(1, 2))
		dev->cmb = nvme_map_cmb(dev);

	return 0;

 unmap:
	iounmap(dev->bar);
	dev->bar = NULL;
 disable:
	pci_release_regions(pdev);
 disable_pci:
	pci_disable_device(pdev);
	return result;
}

static void nvme_dev_unmap(struct nvme_dev *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev->dev);

	if (pdev->msi_enabled)
		pci_disable_msi(pdev);
	else if (pdev->msix_enabled)
		pci_disable_msix(pdev);

	if (dev->bar) {
		iounmap(dev->bar);
		dev->bar = NULL;
		pci_release_regions(pdev);
	}

	if (pci_is_enabled(pdev))
		pci_disable_device(pdev);
}

struct nvme_delq_ctx {
	struct task_struct *waiter;
	struct kthread_worker *worker;
	atomic_t refcount;
};

static void nvme_wait_dq(struct nvme_delq_ctx *dq, struct nvme_dev *dev)
{
	dq->waiter = current;
	mb();

	for (;;) {
		set_current_state(TASK_KILLABLE);
		if (!atomic_read(&dq->refcount))
			break;
		if (!schedule_timeout(ADMIN_TIMEOUT) ||
					fatal_signal_pending(current)) {
			/*
			 * Disable the controller first since we can't trust it
			 * at this point, but leave the admin queue enabled
			 * until all queue deletion requests are flushed.
			 * FIXME: This may take a while if there are more h/w
			 * queues than admin tags.
			 */
			set_current_state(TASK_RUNNING);
			nvme_disable_ctrl(&dev->ctrl,
				lo_hi_readq(dev->bar + NVME_REG_CAP));
			nvme_clear_queue(dev->queues[0]);
			flush_kthread_worker(dq->worker);
			nvme_disable_queue(dev, 0);
			return;
		}
	}
	set_current_state(TASK_RUNNING);
}

static void nvme_put_dq(struct nvme_delq_ctx *dq)
{
	atomic_dec(&dq->refcount);
	if (dq->waiter)
		wake_up_process(dq->waiter);
}

static struct nvme_delq_ctx *nvme_get_dq(struct nvme_delq_ctx *dq)
{
	atomic_inc(&dq->refcount);
	return dq;
}

static void nvme_del_queue_end(struct nvme_queue *nvmeq)
{
	struct nvme_delq_ctx *dq = nvmeq->cmdinfo.ctx;
	nvme_put_dq(dq);

	spin_lock_irq(&nvmeq->q_lock);
	nvme_process_cq(nvmeq);
	spin_unlock_irq(&nvmeq->q_lock);
}

static int adapter_async_del_queue(struct nvme_queue *nvmeq, u8 opcode,
						kthread_work_func_t fn)
{
	struct nvme_command c;

	memset(&c, 0, sizeof(c));
	c.delete_queue.opcode = opcode;
	c.delete_queue.qid = cpu_to_le16(nvmeq->qid);

	init_kthread_work(&nvmeq->cmdinfo.work, fn);
	return nvme_submit_admin_async_cmd(nvmeq->dev, &c, &nvmeq->cmdinfo,
								ADMIN_TIMEOUT);
}

static void nvme_del_cq_work_handler(struct kthread_work *work)
{
	struct nvme_queue *nvmeq = container_of(work, struct nvme_queue,
							cmdinfo.work);
	nvme_del_queue_end(nvmeq);
}

static int nvme_delete_cq(struct nvme_queue *nvmeq)
{
	return adapter_async_del_queue(nvmeq, nvme_admin_delete_cq,
						nvme_del_cq_work_handler);
}

static void nvme_del_sq_work_handler(struct kthread_work *work)
{
	struct nvme_queue *nvmeq = container_of(work, struct nvme_queue,
							cmdinfo.work);
	int status = nvmeq->cmdinfo.status;

	if (!status)
		status = nvme_delete_cq(nvmeq);
	if (status)
		nvme_del_queue_end(nvmeq);
}

static int nvme_delete_sq(struct nvme_queue *nvmeq)
{
	return adapter_async_del_queue(nvmeq, nvme_admin_delete_sq,
						nvme_del_sq_work_handler);
}

static void nvme_del_queue_start(struct kthread_work *work)
{
	struct nvme_queue *nvmeq = container_of(work, struct nvme_queue,
							cmdinfo.work);
	if (nvme_delete_sq(nvmeq))
		nvme_del_queue_end(nvmeq);
}

static void nvme_disable_io_queues(struct nvme_dev *dev)
{
	int i;
	DEFINE_KTHREAD_WORKER_ONSTACK(worker);
	struct nvme_delq_ctx dq;
	struct task_struct *kworker_task = kthread_run(kthread_worker_fn,
					&worker, "nvme%d", dev->ctrl.instance);

	if (IS_ERR(kworker_task)) {
		dev_err(dev->dev,
			"Failed to create queue del task\n");
		for (i = dev->queue_count - 1; i > 0; i--)
			nvme_disable_queue(dev, i);
		return;
	}

	dq.waiter = NULL;
	atomic_set(&dq.refcount, 0);
	dq.worker = &worker;
	for (i = dev->queue_count - 1; i > 0; i--) {
		struct nvme_queue *nvmeq = dev->queues[i];

		if (nvme_suspend_queue(nvmeq))
			continue;
		nvmeq->cmdinfo.ctx = nvme_get_dq(&dq);
		nvmeq->cmdinfo.worker = dq.worker;
		init_kthread_work(&nvmeq->cmdinfo.work, nvme_del_queue_start);
		queue_kthread_work(dq.worker, &nvmeq->cmdinfo.work);
	}
	nvme_wait_dq(&dq, dev);
	kthread_stop(kworker_task);
}

/*
* Remove the node from the device list and check
* for whether or not we need to stop the nvme_thread.
*/
static void nvme_dev_list_remove(struct nvme_dev *dev)
{
	struct task_struct *tmp = NULL;

	spin_lock(&dev_list_lock);
	list_del_init(&dev->node);
	if (list_empty(&dev_list) && !IS_ERR_OR_NULL(nvme_thread)) {
		tmp = nvme_thread;
		nvme_thread = NULL;
	}
	spin_unlock(&dev_list_lock);

	if (tmp)
		kthread_stop(tmp);
}

static void nvme_freeze_queues(struct nvme_dev *dev)
{
	struct nvme_ns *ns;

	list_for_each_entry(ns, &dev->ctrl.namespaces, list) {
		blk_mq_freeze_queue_start(ns->queue);

		spin_lock_irq(ns->queue->queue_lock);
		queue_flag_set(QUEUE_FLAG_STOPPED, ns->queue);
		spin_unlock_irq(ns->queue->queue_lock);

		blk_mq_cancel_requeue_work(ns->queue);
		blk_mq_stop_hw_queues(ns->queue);
	}
}

static void nvme_unfreeze_queues(struct nvme_dev *dev)
{
	struct nvme_ns *ns;

	list_for_each_entry(ns, &dev->ctrl.namespaces, list) {
		queue_flag_clear_unlocked(QUEUE_FLAG_STOPPED, ns->queue);
		blk_mq_unfreeze_queue(ns->queue);
		blk_mq_start_stopped_hw_queues(ns->queue, true);
		blk_mq_kick_requeue_list(ns->queue);
	}
}

static void nvme_dev_shutdown(struct nvme_dev *dev)
{
	int i;
	u32 csts = -1;

	nvme_dev_list_remove(dev);

	if (dev->bar) {
		nvme_freeze_queues(dev);
		csts = readl(dev->bar + NVME_REG_CSTS);
	}
	if (csts & NVME_CSTS_CFS || !(csts & NVME_CSTS_RDY)) {
		for (i = dev->queue_count - 1; i >= 0; i--) {
			struct nvme_queue *nvmeq = dev->queues[i];
			nvme_suspend_queue(nvmeq);
		}
	} else {
		nvme_disable_io_queues(dev);
		nvme_shutdown_ctrl(&dev->ctrl);
		nvme_disable_queue(dev, 0);
	}
	nvme_dev_unmap(dev);

	for (i = dev->queue_count - 1; i >= 0; i--)
		nvme_clear_queue(dev->queues[i]);
}

static int nvme_setup_prp_pools(struct nvme_dev *dev)
{
	dev->prp_page_pool = dma_pool_create("prp list page", dev->dev,
						PAGE_SIZE, PAGE_SIZE, 0);
	if (!dev->prp_page_pool)
		return -ENOMEM;

	/* Optimisation for I/Os between 4k and 128k */
	dev->prp_small_pool = dma_pool_create("prp list 256", dev->dev,
						256, 256, 0);
	if (!dev->prp_small_pool) {
		dma_pool_destroy(dev->prp_page_pool);
		return -ENOMEM;
	}
	return 0;
}

static void nvme_release_prp_pools(struct nvme_dev *dev)
{
	dma_pool_destroy(dev->prp_page_pool);
	dma_pool_destroy(dev->prp_small_pool);
}

static void nvme_pci_free_ctrl(struct nvme_ctrl *ctrl)
{
	struct nvme_dev *dev = to_nvme_dev(ctrl);

	put_device(dev->dev);
	if (dev->tagset.tags)
		blk_mq_free_tag_set(&dev->tagset);
	if (dev->ctrl.admin_q)
		blk_put_queue(dev->ctrl.admin_q);
	kfree(dev->queues);
	kfree(dev->entry);
	kfree(dev);
}

static void nvme_probe_work(struct work_struct *work)
{
	struct nvme_dev *dev = container_of(work, struct nvme_dev, probe_work);
	bool start_thread = false;
	int result;

	result = nvme_dev_map(dev);
	if (result)
		goto out;

	result = nvme_configure_admin_queue(dev);
	if (result)
		goto unmap;

	spin_lock(&dev_list_lock);
	if (list_empty(&dev_list) && IS_ERR_OR_NULL(nvme_thread)) {
		start_thread = true;
		nvme_thread = NULL;
	}
	list_add(&dev->node, &dev_list);
	spin_unlock(&dev_list_lock);

	if (start_thread) {
		nvme_thread = kthread_run(nvme_kthread, NULL, "nvme");
		wake_up_all(&nvme_kthread_wait);
	} else
		wait_event_killable(nvme_kthread_wait, nvme_thread);

	if (IS_ERR_OR_NULL(nvme_thread)) {
		result = nvme_thread ? PTR_ERR(nvme_thread) : -EINTR;
		goto disable;
	}

	nvme_init_queue(dev->queues[0], 0);
	result = nvme_alloc_admin_tags(dev);
	if (result)
		goto disable;

	result = nvme_init_identify(&dev->ctrl);
	if (result)
		goto free_tags;

	result = nvme_setup_io_queues(dev);
	if (result)
		goto free_tags;

	dev->ctrl.event_limit = 1;

	/*
	 * Keep the controller around but remove all namespaces if we don't have
	 * any working I/O queue.
	 */
	if (dev->online_queues < 2) {
		dev_warn(dev->dev, "IO queues not created\n");
		nvme_remove_namespaces(&dev->ctrl);
	} else {
		nvme_unfreeze_queues(dev);
		nvme_dev_add(dev);
	}

	return;

 free_tags:
	nvme_dev_remove_admin(dev);
	blk_put_queue(dev->ctrl.admin_q);
	dev->ctrl.admin_q = NULL;
	dev->queues[0]->tags = NULL;
 disable:
	nvme_disable_queue(dev, 0);
	nvme_dev_list_remove(dev);
 unmap:
	nvme_dev_unmap(dev);
 out:
	if (!work_busy(&dev->reset_work))
		nvme_dead_ctrl(dev);
}

static int nvme_remove_dead_ctrl(void *arg)
{
	struct nvme_dev *dev = (struct nvme_dev *)arg;
	struct pci_dev *pdev = to_pci_dev(dev->dev);

	if (pci_get_drvdata(pdev))
		pci_stop_and_remove_bus_device_locked(pdev);
	nvme_put_ctrl(&dev->ctrl);
	return 0;
}

static void nvme_dead_ctrl(struct nvme_dev *dev)
{
	dev_warn(dev->dev, "Device failed to resume\n");
	kref_get(&dev->ctrl.kref);
	if (IS_ERR(kthread_run(nvme_remove_dead_ctrl, dev, "nvme%d",
						dev->ctrl.instance))) {
		dev_err(dev->dev,
			"Failed to start controller remove task\n");
		nvme_put_ctrl(&dev->ctrl);
	}
}

static void nvme_reset_work(struct work_struct *ws)
{
	struct nvme_dev *dev = container_of(ws, struct nvme_dev, reset_work);
	bool in_probe = work_busy(&dev->probe_work);

	nvme_dev_shutdown(dev);

	/* Synchronize with device probe so that work will see failure status
	 * and exit gracefully without trying to schedule another reset */
	flush_work(&dev->probe_work);

	/* Fail this device if reset occured during probe to avoid
	 * infinite initialization loops. */
	if (in_probe) {
		nvme_dead_ctrl(dev);
		return;
	}
	/* Schedule device resume asynchronously so the reset work is available
	 * to cleanup errors that may occur during reinitialization */
	schedule_work(&dev->probe_work);
}

static int __nvme_reset(struct nvme_dev *dev)
{
	if (work_pending(&dev->reset_work))
		return -EBUSY;
	list_del_init(&dev->node);
	queue_work(nvme_workq, &dev->reset_work);
	return 0;
}

static int nvme_reset(struct nvme_dev *dev)
{
	int ret;

	if (!dev->ctrl.admin_q || blk_queue_dying(dev->ctrl.admin_q))
		return -ENODEV;

	spin_lock(&dev_list_lock);
	ret = __nvme_reset(dev);
	spin_unlock(&dev_list_lock);

	if (!ret) {
		flush_work(&dev->reset_work);
		flush_work(&dev->probe_work);
		return 0;
	}

	return ret;
}

static int nvme_pci_reg_read32(struct nvme_ctrl *ctrl, u32 off, u32 *val)
{
	*val = readl(to_nvme_dev(ctrl)->bar + off);
	return 0;
}

static int nvme_pci_reg_write32(struct nvme_ctrl *ctrl, u32 off, u32 val)
{
	writel(val, to_nvme_dev(ctrl)->bar + off);
	return 0;
}

static int nvme_pci_reg_read64(struct nvme_ctrl *ctrl, u32 off, u64 *val)
{
	*val = readq(to_nvme_dev(ctrl)->bar + off);
	return 0;
}

static bool nvme_pci_io_incapable(struct nvme_ctrl *ctrl)
{
	struct nvme_dev *dev = to_nvme_dev(ctrl);

	return !dev->bar || dev->online_queues < 2;
}

static int nvme_pci_reset_ctrl(struct nvme_ctrl *ctrl)
{
	return nvme_reset(to_nvme_dev(ctrl));
}

static const struct nvme_ctrl_ops nvme_pci_ctrl_ops = {
	.reg_read32		= nvme_pci_reg_read32,
	.reg_write32		= nvme_pci_reg_write32,
	.reg_read64		= nvme_pci_reg_read64,
	.io_incapable		= nvme_pci_io_incapable,
	.reset_ctrl		= nvme_pci_reset_ctrl,
	.free_ctrl		= nvme_pci_free_ctrl,
};

static int nvme_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int node, result = -ENOMEM;
	struct nvme_dev *dev;

	node = dev_to_node(&pdev->dev);
	if (node == NUMA_NO_NODE)
		set_dev_node(&pdev->dev, 0);

	dev = kzalloc_node(sizeof(*dev), GFP_KERNEL, node);
	if (!dev)
		return -ENOMEM;
	dev->entry = kzalloc_node(num_possible_cpus() * sizeof(*dev->entry),
							GFP_KERNEL, node);
	if (!dev->entry)
		goto free;
	dev->queues = kzalloc_node((num_possible_cpus() + 1) * sizeof(void *),
							GFP_KERNEL, node);
	if (!dev->queues)
		goto free;

	dev->dev = get_device(&pdev->dev);
	pci_set_drvdata(pdev, dev);

	INIT_LIST_HEAD(&dev->node);
	INIT_WORK(&dev->scan_work, nvme_dev_scan);
	INIT_WORK(&dev->probe_work, nvme_probe_work);
	INIT_WORK(&dev->reset_work, nvme_reset_work);

	result = nvme_setup_prp_pools(dev);
	if (result)
		goto put_pci;

	result = nvme_init_ctrl(&dev->ctrl, &pdev->dev, &nvme_pci_ctrl_ops,
			id->driver_data);
	if (result)
		goto release_pools;

	schedule_work(&dev->probe_work);
	return 0;

 release_pools:
	nvme_release_prp_pools(dev);
 put_pci:
	put_device(dev->dev);
 free:
	kfree(dev->queues);
	kfree(dev->entry);
	kfree(dev);
	return result;
}

static void nvme_reset_notify(struct pci_dev *pdev, bool prepare)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);

	if (prepare)
		nvme_dev_shutdown(dev);
	else
		schedule_work(&dev->probe_work);
}

static void nvme_shutdown(struct pci_dev *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);
	nvme_dev_shutdown(dev);
}

static void nvme_remove(struct pci_dev *pdev)
{
	struct nvme_dev *dev = pci_get_drvdata(pdev);

	spin_lock(&dev_list_lock);
	list_del_init(&dev->node);
	spin_unlock(&dev_list_lock);

	pci_set_drvdata(pdev, NULL);
	flush_work(&dev->probe_work);
	flush_work(&dev->reset_work);
	flush_work(&dev->scan_work);
	nvme_remove_namespaces(&dev->ctrl);
	nvme_dev_shutdown(dev);
	nvme_dev_remove_admin(dev);
	nvme_free_queues(dev, 0);
	nvme_release_cmb(dev);
	nvme_release_prp_pools(dev);
	nvme_put_ctrl(&dev->ctrl);
}

/* These functions are yet to be implemented */
#define nvme_error_detected NULL
#define nvme_dump_registers NULL
#define nvme_link_reset NULL
#define nvme_slot_reset NULL
#define nvme_error_resume NULL

#ifdef CONFIG_PM_SLEEP
static int nvme_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct nvme_dev *ndev = pci_get_drvdata(pdev);

	nvme_dev_shutdown(ndev);
	return 0;
}

static int nvme_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct nvme_dev *ndev = pci_get_drvdata(pdev);

	schedule_work(&ndev->probe_work);
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(nvme_dev_pm_ops, nvme_suspend, nvme_resume);

static const struct pci_error_handlers nvme_err_handler = {
	.error_detected	= nvme_error_detected,
	.mmio_enabled	= nvme_dump_registers,
	.link_reset	= nvme_link_reset,
	.slot_reset	= nvme_slot_reset,
	.resume		= nvme_error_resume,
	.reset_notify	= nvme_reset_notify,
};

/* Move to pci_ids.h later */
#define PCI_CLASS_STORAGE_EXPRESS	0x010802

static const struct pci_device_id nvme_id_table[] = {
	{ PCI_VDEVICE(INTEL, 0x0953),
		.driver_data = NVME_QUIRK_STRIPE_SIZE, },
	{ PCI_DEVICE_CLASS(PCI_CLASS_STORAGE_EXPRESS, 0xffffff) },
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x2001) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, nvme_id_table);

static struct pci_driver nvme_driver = {
	.name		= "nvme",
	.id_table	= nvme_id_table,
	.probe		= nvme_probe,
	.remove		= nvme_remove,
	.shutdown	= nvme_shutdown,
	.driver		= {
		.pm	= &nvme_dev_pm_ops,
	},
	.err_handler	= &nvme_err_handler,
};

static int __init nvme_init(void)
{
	int result;

	init_waitqueue_head(&nvme_kthread_wait);

	nvme_workq = create_singlethread_workqueue("nvme");
	if (!nvme_workq)
		return -ENOMEM;

	result = nvme_core_init();
	if (result < 0)
		goto kill_workq;

	result = pci_register_driver(&nvme_driver);
	if (result)
		goto core_exit;
	return 0;

 core_exit:
	nvme_core_exit();
 kill_workq:
	destroy_workqueue(nvme_workq);
	return result;
}

static void __exit nvme_exit(void)
{
	pci_unregister_driver(&nvme_driver);
	nvme_core_exit();
	destroy_workqueue(nvme_workq);
	BUG_ON(nvme_thread && !IS_ERR(nvme_thread));
	_nvme_check_size();
}

MODULE_AUTHOR("Matthew Wilcox <willy@linux.intel.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
module_init(nvme_init);
module_exit(nvme_exit);
