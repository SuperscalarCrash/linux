// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for Loongson-1 APB DMA Controller
 *
 * Copyright (C) 2015-2024 Keguang Zhang <keguang.zhang@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/dmapool.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "../dmaengine.h"
#include "../virt-dma.h"

/* Loongson-1 DMA Control Register */
#define LS1X_DMA_CTRL		0x0

/* DMA Control Register Bits */
#define LS1X_DMA_STOP		BIT(4)
#define LS1X_DMA_START		BIT(3)
#define LS1X_DMA_ASK_VALID	BIT(2)

/* DMA Next Field Bits */
#define LS1X_DMA_NEXT_VALID	BIT(0)

/* DMA Command Field Bits */
#define LS1X_DMA_RAM2DEV	BIT(12)
#define LS1X_DMA_INT_MASK	BIT(0)

#define LS1X_DMA_LLI_ALIGNMENT	64
#define LS1X_DMA_LLI_ADDR_MASK	GENMASK(31, __ffs(LS1X_DMA_LLI_ALIGNMENT))
#define LS1X_DMA_MAX_CHANNELS	3
#define LS1X_DMA_START_TIMEOUT_US	3000
#define LS1X_DMA_QUERY_TIMEOUT_US	3000
#define LS1X_DMA_STOP_RETRIES		3
#define LS1X_DMA_STOP_SETTLE_US		10

/* DMA state returned by the ASK operation in the command word. */
#define LS1X_DMA_TRANS_OVER		BIT(3)
#define LS1X_DMA_READ_STATE_MASK	GENMASK(7, 4)
#define LS1X_DMA_WRITE_STATE_MASK	GENMASK(11, 8)

enum ls1x_dmadesc_offsets {
	LS1X_DMADESC_NEXT = 0,
	LS1X_DMADESC_SADDR,
	LS1X_DMADESC_DADDR,
	LS1X_DMADESC_LENGTH,
	LS1X_DMADESC_STRIDE,
	LS1X_DMADESC_CYCLES,
	LS1X_DMADESC_CMD,
	LS1X_DMADESC_SIZE
};

struct ls1x_dma_lli {
	unsigned int hw[LS1X_DMADESC_SIZE];
	dma_addr_t phys;
	struct list_head node;
} __aligned(LS1X_DMA_LLI_ALIGNMENT);

struct ls1x_dma_desc {
	struct virt_dma_desc vd;
	struct list_head lli_list;
	bool is_cyclic;
};

struct ls1x_dma_chan {
	struct virt_dma_chan vc;
	struct dma_pool *lli_pool;
	phys_addr_t src_addr;
	phys_addr_t dst_addr;
	enum dma_slave_buswidth src_addr_width;
	enum dma_slave_buswidth dst_addr_width;
	unsigned int bus_width;
	void __iomem *reg_base;
	int irq;
	bool poll_only;
	bool is_cyclic;
	enum dma_status status;
	struct ls1x_dma_lli *curr_lli;
	dma_addr_t curr_lli_phys;
	struct ls1x_dma_lli *stop_lli;
	dma_addr_t stop_lli_phys;
};

struct ls1x_dma {
	struct dma_device ddev;
	unsigned int nr_chans;
	struct ls1x_dma_chan chan[];
};

static irqreturn_t ls1x_dma_irq_handler(int irq, void *data);

#define to_ls1x_dma_chan(dchan)		\
	container_of(dchan, struct ls1x_dma_chan, vc.chan)

#define to_ls1x_dma_desc(d)		\
	container_of(d, struct ls1x_dma_desc, vd)

static inline struct device *chan2dev(struct dma_chan *chan)
{
	return &chan->dev->device;
}

static inline int ls1x_dma_query(struct ls1x_dma_chan *chan,
				 dma_addr_t lli_phys)
{
	struct dma_chan *dchan = &chan->vc.chan;
	int val, ret;

	val = lower_32_bits(lli_phys) & LS1X_DMA_LLI_ADDR_MASK;
	val |= LS1X_DMA_ASK_VALID;
	val |= dchan->chan_id;
	writel(val, chan->reg_base + LS1X_DMA_CTRL);
	ret = readl_poll_timeout_atomic(chan->reg_base + LS1X_DMA_CTRL, val,
					!(val & LS1X_DMA_ASK_VALID), 0,
					LS1X_DMA_QUERY_TIMEOUT_US);
	if (ret)
		dev_err(chan2dev(dchan), "failed to query DMA\n");

	return ret;
}

static inline int ls1x_dma_start(struct ls1x_dma_chan *chan,
				 dma_addr_t lli_phys)
{
	struct dma_chan *dchan = &chan->vc.chan;
	struct device *dev = chan2dev(dchan);
	int val, ret;

	val = lower_32_bits(lli_phys) & LS1X_DMA_LLI_ADDR_MASK;
	val |= LS1X_DMA_START;
	val |= dchan->chan_id;
	dma_wmb();
	writel(val, chan->reg_base + LS1X_DMA_CTRL);
	ret = readl_poll_timeout_atomic(chan->reg_base + LS1X_DMA_CTRL, val,
					!(val & LS1X_DMA_START), 0,
					LS1X_DMA_START_TIMEOUT_US);
	if (!ret)
		dev_dbg(dev, "start DMA with lli_phys=%pad\n", &lli_phys);
	else
		dev_err(dev, "failed to start DMA\n");

	return ret;
}

static int ls1x_dma_stop(struct ls1x_dma_chan *chan)
{
	struct dma_chan *dchan = &chan->vc.chan;
	struct device *dev = chan2dev(dchan);
	unsigned int state;
	int i, ret = -ETIMEDOUT;

	/*
	 * STOP is sampled by the hardware at safe transfer boundaries and is
	 * not self-clearing.  Use the ASK operation after a short settling
	 * period to verify that the transfer either completed or its working
	 * registers were cleared. Reassert STOP if a transfer was still moving
	 * when it was sampled.
	 */
	for (i = 0; i < LS1X_DMA_STOP_RETRIES; i++) {
		writel(LS1X_DMA_STOP | dchan->chan_id,
		       chan->reg_base + LS1X_DMA_CTRL);
		udelay(LS1X_DMA_STOP_SETTLE_US);

		ret = ls1x_dma_query(chan, chan->stop_lli_phys);
		if (ret)
			continue;

		dma_rmb();
		state = READ_ONCE(chan->stop_lli->hw[LS1X_DMADESC_CMD]);
		/*
		 * ASK writes its own snapshot through the DMA write state
		 * machine, so the reported write state need not be IDLE.  A
		 * completed transfer has TRANS_OVER set.  An aborted transfer
		 * has its working registers cleared by STOP.
		 */
		if ((state & LS1X_DMA_TRANS_OVER) ||
		    (!READ_ONCE(chan->stop_lli->hw[LS1X_DMADESC_DADDR]) &&
		     !READ_ONCE(chan->stop_lli->hw[LS1X_DMADESC_LENGTH]) &&
		     !READ_ONCE(chan->stop_lli->hw[LS1X_DMADESC_CYCLES])))
			return 0;
	}

	state = READ_ONCE(chan->stop_lli->hw[LS1X_DMADESC_CMD]);
	dev_err(dev,
		"failed to stop DMA safely: state=%#x read=%#x write=%#x mem=%#x dev=%#x remaining=%#x\n",
		state,
		(unsigned int)FIELD_GET(LS1X_DMA_READ_STATE_MASK, state),
		(unsigned int)FIELD_GET(LS1X_DMA_WRITE_STATE_MASK, state),
		READ_ONCE(chan->stop_lli->hw[LS1X_DMADESC_SADDR]),
		READ_ONCE(chan->stop_lli->hw[LS1X_DMADESC_DADDR]),
		READ_ONCE(chan->stop_lli->hw[LS1X_DMADESC_LENGTH]));
	return ret ?: -ETIMEDOUT;
}

static void ls1x_dma_free_chan_resources(struct dma_chan *dchan)
{
	struct ls1x_dma_chan *chan = to_ls1x_dma_chan(dchan);
	struct device *dev = dchan->device->dev;

	dma_free_coherent(dev, sizeof(struct ls1x_dma_lli),
			  chan->curr_lli, chan->curr_lli_phys);
	dma_free_coherent(dev, sizeof(struct ls1x_dma_lli),
			  chan->stop_lli, chan->stop_lli_phys);
	dma_pool_destroy(chan->lli_pool);
	chan->lli_pool = NULL;
	chan->curr_lli = NULL;
	chan->stop_lli = NULL;
	devm_free_irq(dev, chan->irq, chan);
	vchan_free_chan_resources(&chan->vc);
}

static int ls1x_dma_alloc_chan_resources(struct dma_chan *dchan)
{
	struct ls1x_dma_chan *chan = to_ls1x_dma_chan(dchan);
	struct device *dev = dchan->device->dev;
	int ret;

	ret = devm_request_irq(dev, chan->irq, ls1x_dma_irq_handler,
			       IRQF_SHARED, dma_chan_name(dchan), chan);
	if (ret) {
		dev_err(dev, "failed to request IRQ %d\n", chan->irq);
		return ret;
	}

	chan->lli_pool = dma_pool_create(dma_chan_name(dchan), dev,
					 sizeof(struct ls1x_dma_lli),
					 __alignof__(struct ls1x_dma_lli), 0);
	if (!chan->lli_pool) {
		devm_free_irq(dev, chan->irq, chan);
		return -ENOMEM;
	}

	/* Allocate independent snapshots for status/pause and STOP recovery. */
	chan->curr_lli = dma_alloc_coherent(dev, sizeof(struct ls1x_dma_lli),
					    &chan->curr_lli_phys, GFP_KERNEL);
	if (!chan->curr_lli) {
		dma_pool_destroy(chan->lli_pool);
		chan->lli_pool = NULL;
		devm_free_irq(dev, chan->irq, chan);
		return -ENOMEM;
	}

	chan->stop_lli = dma_alloc_coherent(dev, sizeof(struct ls1x_dma_lli),
					    &chan->stop_lli_phys, GFP_KERNEL);
	if (!chan->stop_lli) {
		dma_free_coherent(dev, sizeof(struct ls1x_dma_lli),
				  chan->curr_lli, chan->curr_lli_phys);
		chan->curr_lli = NULL;
		dma_pool_destroy(chan->lli_pool);
		chan->lli_pool = NULL;
		devm_free_irq(dev, chan->irq, chan);
		return -ENOMEM;
	}
	chan->status = DMA_COMPLETE;

	return 0;
}

static void ls1x_dma_free_desc(struct virt_dma_desc *vd)
{
	struct ls1x_dma_desc *desc = to_ls1x_dma_desc(vd);
	struct ls1x_dma_chan *chan = to_ls1x_dma_chan(vd->tx.chan);
	struct ls1x_dma_lli *lli, *_lli;

	list_for_each_entry_safe(lli, _lli, &desc->lli_list, node) {
		list_del(&lli->node);
		dma_pool_free(chan->lli_pool, lli, lli->phys);
	}

	kfree(desc);
}

static struct ls1x_dma_desc *ls1x_dma_alloc_desc(void)
{
	struct ls1x_dma_desc *desc;

	desc = kzalloc_obj(*desc, GFP_NOWAIT);
	if (!desc)
		return NULL;

	INIT_LIST_HEAD(&desc->lli_list);

	return desc;
}

static int ls1x_dma_prep_lli(struct dma_chan *dchan, struct ls1x_dma_desc *desc,
			     struct scatterlist *sgl, unsigned int sg_len,
			     enum dma_transfer_direction dir, bool is_cyclic)
{
	struct ls1x_dma_chan *chan = to_ls1x_dma_chan(dchan);
	struct ls1x_dma_lli *lli, *prev = NULL, *first = NULL;
	struct device *dev = chan2dev(dchan);
	struct list_head *pos = NULL;
	struct scatterlist *sg;
	unsigned int dev_addr, cmd, i;

	switch (dir) {
	case DMA_MEM_TO_DEV:
		dev_addr = chan->dst_addr;
		chan->bus_width = chan->dst_addr_width;
		cmd = LS1X_DMA_RAM2DEV;
		break;
	case DMA_DEV_TO_MEM:
		dev_addr = chan->src_addr;
		chan->bus_width = chan->src_addr_width;
		cmd = 0;
		break;
	default:
		dev_err(dev, "unsupported DMA direction: %s\n",
			dmaengine_get_direction_text(dir));
		return -EINVAL;
	}

	if (!chan->poll_only)
		cmd |= LS1X_DMA_INT_MASK;

	for_each_sg(sgl, sg, sg_len, i) {
		dma_addr_t buf_addr = sg_dma_address(sg);
		size_t buf_len = sg_dma_len(sg);
		dma_addr_t phys;

		if (!is_dma_copy_aligned(dchan->device, buf_addr, 0, buf_len)) {
			dev_err(dev, "buffer is not aligned\n");
			return -EINVAL;
		}

		/* allocate HW descriptors */
		lli = dma_pool_zalloc(chan->lli_pool, GFP_NOWAIT, &phys);
		if (!lli) {
			dev_err(dev, "failed to alloc lli %u\n", i);
			return -ENOMEM;
		}

		/* setup HW descriptors */
		lli->phys = phys;
		lli->hw[LS1X_DMADESC_SADDR] = buf_addr;
		lli->hw[LS1X_DMADESC_DADDR] = dev_addr;
		lli->hw[LS1X_DMADESC_LENGTH] = buf_len / chan->bus_width;
		lli->hw[LS1X_DMADESC_STRIDE] = 0;
		lli->hw[LS1X_DMADESC_CYCLES] = 1;
		lli->hw[LS1X_DMADESC_CMD] = cmd;

		if (prev)
			prev->hw[LS1X_DMADESC_NEXT] =
			    lli->phys | LS1X_DMA_NEXT_VALID;
		prev = lli;

		if (!first)
			first = lli;

		list_add_tail(&lli->node, &desc->lli_list);
	}

	if (is_cyclic)
		lli->hw[LS1X_DMADESC_NEXT] = first->phys | LS1X_DMA_NEXT_VALID;
	desc->is_cyclic = is_cyclic;

	list_for_each(pos, &desc->lli_list) {
		lli = list_entry(pos, struct ls1x_dma_lli, node);
		print_hex_dump_debug("LLI: ", DUMP_PREFIX_OFFSET, 16, 4,
				     lli, sizeof(*lli), false);
	}

	return 0;
}

static struct dma_async_tx_descriptor *
ls1x_dma_prep_slave_sg(struct dma_chan *dchan, struct scatterlist *sgl,
		       unsigned int sg_len, enum dma_transfer_direction dir,
		       unsigned long flags, void *context)
{
	struct ls1x_dma_desc *desc;

	dev_dbg(chan2dev(dchan), "sg_len=%u flags=0x%lx dir=%s\n",
		sg_len, flags, dmaengine_get_direction_text(dir));

	desc = ls1x_dma_alloc_desc();
	if (!desc)
		return NULL;

	if (ls1x_dma_prep_lli(dchan, desc, sgl, sg_len, dir, false)) {
		ls1x_dma_free_desc(&desc->vd);
		return NULL;
	}

	return vchan_tx_prep(to_virt_chan(dchan), &desc->vd, flags);
}

static struct dma_async_tx_descriptor *
ls1x_dma_prep_dma_cyclic(struct dma_chan *dchan, dma_addr_t buf_addr,
			 size_t buf_len, size_t period_len,
			 enum dma_transfer_direction dir, unsigned long flags)
{
	struct ls1x_dma_chan *chan = to_ls1x_dma_chan(dchan);
	struct ls1x_dma_desc *desc;
	struct scatterlist *sgl;
	unsigned int sg_len;
	unsigned int i;
	int ret;

	dev_dbg(chan2dev(dchan),
		"buf_len=%zu period_len=%zu flags=0x%lx dir=%s\n",
		buf_len, period_len, flags, dmaengine_get_direction_text(dir));

	/*
	 * Cyclic clients require a callback at every period. The Chiplab
	 * integration exposes only a transient interrupt pulse, so it uses
	 * completion polling and cannot provide period notifications.
	 */
	if (chan->poll_only)
		return NULL;

	desc = ls1x_dma_alloc_desc();
	if (!desc)
		return NULL;

	/* allocate the scatterlist */
	sg_len = buf_len / period_len;
	sgl = kmalloc_objs(*sgl, sg_len, GFP_NOWAIT);
	if (!sgl) {
		kfree(desc);
		return NULL;
	}

	sg_init_table(sgl, sg_len);
	for (i = 0; i < sg_len; ++i) {
		sg_set_page(&sgl[i], pfn_to_page(PFN_DOWN(buf_addr)),
			    period_len, offset_in_page(buf_addr));
		sg_dma_address(&sgl[i]) = buf_addr;
		sg_dma_len(&sgl[i]) = period_len;
		buf_addr += period_len;
	}

	ret = ls1x_dma_prep_lli(dchan, desc, sgl, sg_len, dir, true);
	kfree(sgl);
	if (ret) {
		ls1x_dma_free_desc(&desc->vd);
		return NULL;
	}

	return vchan_tx_prep(to_virt_chan(dchan), &desc->vd, flags);
}

static int ls1x_dma_slave_config(struct dma_chan *dchan,
				 struct dma_slave_config *config)
{
	struct ls1x_dma_chan *chan = to_ls1x_dma_chan(dchan);

	chan->src_addr = config->src_addr;
	chan->src_addr_width = config->src_addr_width;
	chan->dst_addr = config->dst_addr;
	chan->dst_addr_width = config->dst_addr_width;

	return 0;
}

static int ls1x_dma_pause(struct dma_chan *dchan)
{
	struct ls1x_dma_chan *chan = to_ls1x_dma_chan(dchan);
	int ret;

	guard(spinlock_irqsave)(&chan->vc.lock);
	/* save the current lli */
	ret = ls1x_dma_query(chan, chan->curr_lli_phys);
	if (!ret)
		ret = ls1x_dma_stop(chan);

	return ret;
}

static int ls1x_dma_resume(struct dma_chan *dchan)
{
	struct ls1x_dma_chan *chan = to_ls1x_dma_chan(dchan);

	guard(spinlock_irqsave)(&chan->vc.lock);

	chan->status = DMA_IN_PROGRESS;
	if (ls1x_dma_start(chan, chan->curr_lli_phys)) {
		chan->status = DMA_ERROR;
		return -ETIMEDOUT;
	}

	return 0;
}

static int ls1x_dma_terminate_all(struct dma_chan *dchan)
{
	struct ls1x_dma_chan *chan = to_ls1x_dma_chan(dchan);
	struct virt_dma_desc *vd;
	LIST_HEAD(head);
	int ret = 0;

	scoped_guard(spinlock_irqsave, &chan->vc.lock) {
		vd = vchan_next_desc(&chan->vc);
		if (vd) {
			ret = ls1x_dma_stop(chan);
			vchan_terminate_vdesc(vd);
		}
		chan->status = ret ? DMA_ERROR : DMA_COMPLETE;

		vchan_get_all_descriptors(&chan->vc, &head);
	}

	vchan_dma_desc_free_list(&chan->vc, &head);

	return ret;
}

static void ls1x_dma_synchronize(struct dma_chan *dchan)
{
	vchan_synchronize(to_virt_chan(dchan));
}

static enum dma_status ls1x_dma_tx_status(struct dma_chan *dchan,
					  dma_cookie_t cookie,
					  struct dma_tx_state *state)
{
	struct ls1x_dma_chan *chan = to_ls1x_dma_chan(dchan);
	struct virt_dma_desc *vd;
	enum dma_status status;
	size_t bytes = 0;

	status = dma_cookie_status(dchan, cookie, state);
	if (status == DMA_COMPLETE)
		return status;
	if (READ_ONCE(chan->status) == DMA_ERROR)
		return DMA_ERROR;

	scoped_guard(spinlock_irqsave, &chan->vc.lock) {
		vd = vchan_find_desc(&chan->vc, cookie);
		if (vd) {
			struct ls1x_dma_desc *desc = to_ls1x_dma_desc(vd);
			struct ls1x_dma_lli *lli;
			dma_addr_t next_phys;

			/* get the current lli */
			if (ls1x_dma_query(chan, chan->curr_lli_phys))
				return status;
			dma_rmb();

			/*
			 * Some integrations expose the DMA interrupt as a very
			 * short pulse.  Recover a missed interrupt by completing
			 * the active descriptor when ASK reports TRANS_OVER.
			 */
			if (!chan->is_cyclic &&
			    vd == vchan_next_desc(&chan->vc) &&
			    (READ_ONCE(chan->curr_lli->hw[LS1X_DMADESC_CMD]) &
			     LS1X_DMA_TRANS_OVER)) {
				chan->status = DMA_COMPLETE;
				list_del(&vd->node);
				vchan_cookie_complete(vd);
				status = DMA_COMPLETE;
				goto out;
			}

			/*
			 * ASK snapshots the remaining length of the current
			 * descriptor and the physical address of the next one.
			 * Do not derive the current LLI by matching NEXT fields:
			 * the hardware is allowed to clear or advance NEXT at a
			 * descriptor boundary, and list_for_each_entry() leaves
			 * lli pointing at a synthetic list-head container when no
			 * match is found.
			 */
			bytes = READ_ONCE(chan->curr_lli->hw[LS1X_DMADESC_LENGTH]) *
				chan->bus_width;
			next_phys = READ_ONCE(chan->curr_lli->hw[LS1X_DMADESC_NEXT]);

			while (next_phys & LS1X_DMA_NEXT_VALID) {
				dma_addr_t lli_phys =
					next_phys & LS1X_DMA_LLI_ADDR_MASK;
				bool found = false;

				list_for_each_entry(lli, &desc->lli_list, node) {
					if (lli->phys != lli_phys)
						continue;

					bytes +=
						READ_ONCE(lli->hw[LS1X_DMADESC_LENGTH]) *
						chan->bus_width;
					next_phys =
						READ_ONCE(lli->hw[LS1X_DMADESC_NEXT]);
					found = true;
					break;
				}

				if (!found)
					break;
			}
		}
	}

out:
	if (state)
		dma_set_residue(state, bytes);

	return status;
}

static void ls1x_dma_issue_pending(struct dma_chan *dchan)
{
	struct ls1x_dma_chan *chan = to_ls1x_dma_chan(dchan);

	guard(spinlock_irqsave)(&chan->vc.lock);

	if (vchan_issue_pending(&chan->vc)) {
		struct virt_dma_desc *vd = vchan_next_desc(&chan->vc);

		if (vd) {
			struct ls1x_dma_desc *desc = to_ls1x_dma_desc(vd);
			struct ls1x_dma_lli *lli;

			lli = list_first_entry(&desc->lli_list,
					       struct ls1x_dma_lli, node);
			chan->is_cyclic = desc->is_cyclic;
			chan->status = DMA_IN_PROGRESS;
			if (ls1x_dma_start(chan, lli->phys))
				chan->status = DMA_ERROR;
		}
	}
}

static irqreturn_t ls1x_dma_irq_handler(int irq, void *data)
{
	struct ls1x_dma_chan *chan = data;
	struct dma_chan *dchan = &chan->vc.chan;
	struct device *dev = chan2dev(dchan);
	struct virt_dma_desc *vd;

	scoped_guard(spinlock, &chan->vc.lock) {
		vd = vchan_next_desc(&chan->vc);
		if (!vd) {
			dev_warn(dev,
				 "IRQ %d with no active desc on channel %d\n",
				 irq, dchan->chan_id);
			return IRQ_NONE;
		}

		if (chan->is_cyclic) {
			vchan_cyclic_callback(vd);
		} else {
			chan->status = DMA_COMPLETE;
			list_del(&vd->node);
			vchan_cookie_complete(vd);
		}
	}

	dev_dbg(dev, "DMA IRQ %d on channel %d\n", irq, dchan->chan_id);

	return IRQ_HANDLED;
}

static int ls1x_dma_chan_probe(struct platform_device *pdev,
			       struct ls1x_dma *dma)
{
	void __iomem *reg_base;
	int id;

	reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(reg_base))
		return PTR_ERR(reg_base);

	for (id = 0; id < dma->nr_chans; id++) {
		struct ls1x_dma_chan *chan = &dma->chan[id];
		char pdev_irqname[16];

		snprintf(pdev_irqname, sizeof(pdev_irqname), "ch%d", id);
		chan->irq = platform_get_irq_byname(pdev, pdev_irqname);
		if (chan->irq < 0)
			return dev_err_probe(&pdev->dev, chan->irq,
					     "failed to get IRQ for ch%d\n",
					     id);

		chan->reg_base = reg_base;
		chan->poll_only = of_device_is_compatible(pdev->dev.of_node,
							  "loongson,chiplab-apbdma");
		chan->vc.desc_free = ls1x_dma_free_desc;
		vchan_init(&chan->vc, &dma->ddev);
	}

	return 0;
}

static void ls1x_dma_chan_remove(struct ls1x_dma *dma)
{
	int id;

	for (id = 0; id < dma->nr_chans; id++) {
		struct ls1x_dma_chan *chan = &dma->chan[id];

		if (chan->vc.chan.device == &dma->ddev) {
			list_del(&chan->vc.chan.device_node);
			tasklet_kill(&chan->vc.task);
		}
	}
}

static int ls1x_dma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dma_device *ddev;
	struct ls1x_dma *dma;
	int ret;

	ret = platform_irq_count(pdev);
	if (ret <= 0 || ret > LS1X_DMA_MAX_CHANNELS)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid number of IRQ channels: %d\n",
				     ret);

	dma = devm_kzalloc(dev, struct_size(dma, chan, ret), GFP_KERNEL);
	if (!dma)
		return -ENOMEM;
	dma->nr_chans = ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "failed to set 32-bit DMA mask\n");

	/* initialize DMA device */
	ddev = &dma->ddev;
	ddev->dev = dev;
	ddev->copy_align = DMAENGINE_ALIGN_4_BYTES;
	ddev->src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_1_BYTE) |
				BIT(DMA_SLAVE_BUSWIDTH_2_BYTES) |
				BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	ddev->dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_1_BYTE) |
				BIT(DMA_SLAVE_BUSWIDTH_2_BYTES) |
				BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	ddev->directions = BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV);
	ddev->residue_granularity = DMA_RESIDUE_GRANULARITY_SEGMENT;
	ddev->device_alloc_chan_resources = ls1x_dma_alloc_chan_resources;
	ddev->device_free_chan_resources = ls1x_dma_free_chan_resources;
	ddev->device_prep_slave_sg = ls1x_dma_prep_slave_sg;
	ddev->device_prep_dma_cyclic = ls1x_dma_prep_dma_cyclic;
	ddev->device_config = ls1x_dma_slave_config;
	ddev->device_pause = ls1x_dma_pause;
	ddev->device_resume = ls1x_dma_resume;
	ddev->device_terminate_all = ls1x_dma_terminate_all;
	ddev->device_synchronize = ls1x_dma_synchronize;
	ddev->device_tx_status = ls1x_dma_tx_status;
	ddev->device_issue_pending = ls1x_dma_issue_pending;
	dma_cap_set(DMA_SLAVE, ddev->cap_mask);
	INIT_LIST_HEAD(&ddev->channels);

	/* initialize DMA channels */
	ret = ls1x_dma_chan_probe(pdev, dma);
	if (ret)
		goto err;

	ret = dmaenginem_async_device_register(ddev);
	if (ret) {
		dev_err(dev, "failed to register DMA device\n");
		goto err;
	}

	ret = of_dma_controller_register(dev->of_node, of_dma_xlate_by_chan_id,
					 ddev);
	if (ret) {
		dev_err(dev, "failed to register DMA controller\n");
		goto err;
	}

	platform_set_drvdata(pdev, dma);
	dev_info(dev, "Loongson1 DMA driver registered%s\n",
		 of_device_is_compatible(dev->of_node, "loongson,chiplab-apbdma") ?
		 " in polling mode" : "");

	return 0;

err:
	ls1x_dma_chan_remove(dma);

	return ret;
}

static void ls1x_dma_remove(struct platform_device *pdev)
{
	struct ls1x_dma *dma = platform_get_drvdata(pdev);

	of_dma_controller_free(pdev->dev.of_node);
	ls1x_dma_chan_remove(dma);
}

static const struct of_device_id ls1x_dma_match[] = {
	{ .compatible = "loongson,chiplab-apbdma" },
	{ .compatible = "loongson,ls1b-apbdma" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ls1x_dma_match);

static struct platform_driver ls1x_dma_driver = {
	.probe = ls1x_dma_probe,
	.remove = ls1x_dma_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = ls1x_dma_match,
	},
};

module_platform_driver(ls1x_dma_driver);

MODULE_AUTHOR("Keguang Zhang <keguang.zhang@gmail.com>");
MODULE_DESCRIPTION("Loongson-1 APB DMA Controller driver");
MODULE_LICENSE("GPL");
