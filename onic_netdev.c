/*
 * Copyright (c) 2020 Xilinx, Inc.
 * All rights reserved.
 *
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 */
#include <linux/version.h>
#include <linux/pci.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/filter.h>

#include "onic_netdev.h"
#include "qdma_access/qdma_register.h"
#include "onic.h"

#define ONIC_RX_DESC_STEP 256

inline static u16 onic_ring_get_real_count(struct onic_ring *ring)
{
	/* Valid writeback entry means one less count of descriptor entries */
	return (ring->wb) ? (ring->count - 1) : ring->count;
}

inline static bool onic_ring_full(struct onic_ring *ring)
{
	u16 real_count = onic_ring_get_real_count(ring);
	return ((ring->next_to_use + 1) % real_count) == ring->next_to_clean;
}

inline static void onic_ring_increment_head(struct onic_ring *ring)
{
	u16 real_count = onic_ring_get_real_count(ring);
	ring->next_to_use = (ring->next_to_use + 1) % real_count;
}

inline static void onic_ring_increment_tail(struct onic_ring *ring)
{
	u16 real_count = onic_ring_get_real_count(ring);
	ring->next_to_clean = (ring->next_to_clean + 1) % real_count;
}

static void onic_tx_clean(struct onic_tx_queue *q)
{
	struct onic_private *priv = netdev_priv(q->netdev);
	struct onic_ring *ring = &q->ring;
	struct qdma_wb_stat wb;
	int work, i;

	if (test_and_set_bit(0, q->state))
		return;

	qdma_unpack_wb_stat(&wb, ring->wb);

	if (wb.cidx == ring->next_to_clean) {
		clear_bit(0, q->state);
		return;
	}

	work = wb.cidx - ring->next_to_clean;
	if (work < 0)
		work += onic_ring_get_real_count(ring);

	for (i = 0; i < work; ++i) {
		struct onic_tx_buffer *buf = &q->buffer[ring->next_to_clean];
		dma_unmap_single(&priv->pdev->dev, buf->dma_addr, buf->len,
				 DMA_TO_DEVICE);
		if (buf->type == ONIC_SKB_BUFF) {
			struct sk_buff *skb = buf->skb;
			dev_kfree_skb_any(skb);
		} else if (buf->type == ONIC_XDP_FRAME) {
			struct xdp_frame *xdpf = buf->xdpf;
			xdp_return_frame_rx_napi(xdpf);
			kfree(xdpf);
		}

		onic_ring_increment_tail(ring);
	}

	clear_bit(0, q->state);
}

static bool onic_rx_high_watermark(struct onic_rx_queue *q)
{
	struct onic_ring *ring = &q->desc_ring;
	int unused;

	unused = ring->next_to_use - ring->next_to_clean;
	if (ring->next_to_use < ring->next_to_clean)
		unused += onic_ring_get_real_count(ring);

	return (unused < (ONIC_RX_DESC_STEP / 2));
}

static void onic_rx_refill(struct onic_rx_queue *q)
{
	struct onic_private *priv = netdev_priv(q->netdev);
	struct onic_ring *ring = &q->desc_ring;

	ring->next_to_use += ONIC_RX_DESC_STEP;
	ring->next_to_use %= onic_ring_get_real_count(ring);

	onic_set_rx_head(priv->hw.qdma, q->qid, ring->next_to_use);
}


static int onic_run_xdp(struct bpf_prog *xdp_prog, struct xdp_buff *xdpb) {
	int act;

	act = bpf_prog_run_xdp(xdp_prog, xdpb);

	if (act == XDP_PASS)
		return ONIC_XDP_PASS;
	else if (act == XDP_TX)
		return ONIC_XDP_TX;
	else if (act == XDP_REDIRECT)
		return ONIC_XDP_REDIRECT;
	else
		return ONIC_XDP_DROP;
}


static int onic_rx_poll(struct napi_struct *napi, int budget)
{
	struct onic_rx_queue *q =
		container_of(napi, struct onic_rx_queue, napi);
	struct onic_private *priv = netdev_priv(q->netdev);
	u16 qid = q->qid;
	struct onic_ring *desc_ring = &q->desc_ring;
	struct onic_ring *cmpl_ring = &q->cmpl_ring;
	struct qdma_c2h_cmpl cmpl;
	struct qdma_c2h_cmpl_stat cmpl_stat;
	u8 *cmpl_ptr;
	u8 *cmpl_stat_ptr;
	u32 color_stat;
	int work = 0;
	int i, rv;
	bool napi_cmpl_rval = 0;
	bool flipped = 0;
	bool debug = 0;
	struct xdp_buff xdpb;

	for (i = 0; i < priv->num_tx_queues; i++)
		onic_tx_clean(priv->tx_queue[i]);

	cmpl_ptr =
		cmpl_ring->desc + QDMA_C2H_CMPL_SIZE * cmpl_ring->next_to_clean;
	cmpl_stat_ptr =
		cmpl_ring->desc + QDMA_C2H_CMPL_SIZE * (cmpl_ring->count - 1);

	qdma_unpack_c2h_cmpl(&cmpl, cmpl_ptr);
	qdma_unpack_c2h_cmpl_stat(&cmpl_stat, cmpl_stat_ptr);

	color_stat = cmpl_stat.color;
	if (debug)
		netdev_info(
			q->netdev,
			"\n rx_poll:  cmpl_stat_pidx %u, color_cmpl_stat %u, cmpl_ring next_to_clean %u, cmpl_stat_cidx %u, intr_state %u, cmpl_ring->count %u",
			cmpl_stat.pidx, color_stat, cmpl_ring->next_to_clean,
			cmpl_stat.cidx, cmpl_stat.intr_state, cmpl_ring->count);

	if (debug)
		netdev_info(
			q->netdev,
			"c2h_cmpl pkt_id %u, pkt_len %u, error %u, color %u cmpl_ring->color:%u",
			cmpl.pkt_id, cmpl.pkt_len, cmpl.err, cmpl.color,
			cmpl_ring->color);

	/* Color of completion entries and completion ring are initialized to 0
	 * and 1 respectively.  When an entry is filled, it has a color bit of
	 * 1, thus making it the same as the completion ring color.  A different
	 * color indicates that we are done with the current batch.  When the
	 * ring index wraps around, the color flips in both software and
	 * hardware.  Therefore, it becomes that completion entries are filled
	 * with a color 0, and completion ring has a color 0 as well.
	 */
	if (cmpl.color != cmpl_ring->color) {
		if (debug)
			netdev_info(
				q->netdev,
				"color mismatch1: cmpl.color %u, cmpl_ring->color %u  cmpl_stat_color %u",
				cmpl.color, cmpl_ring->color, color_stat);
	}

	if (cmpl.err == 1) {
		if (debug)
			netdev_info(q->netdev,
				    "completion error detected in cmpl entry!");
		// todo: need to handle the error ...
		onic_qdma_clear_error_interrupt(priv->hw.qdma);
	}

	// main processing loop for rx_poll
	while ((cmpl_ring->next_to_clean != cmpl_stat.pidx)) {
		struct onic_rx_buffer *buf =
			&q->buffer[desc_ring->next_to_clean];
		struct sk_buff *skb;
		u8 *data;
		u8 *page;
		int len = cmpl.pkt_len;
		int xdp_ret = ONIC_XDP_PASS;
		/* maximum packet size is 1514, less than the page size */

		page = (u8 *)page_address(buf->pg);
		data = (u8 *)(page + buf->offset);

		xdpb.data_hard_start = page;
		xdpb.data = page + buf->offset;
		xdpb.data_end = xdpb.data + len;
		xdpb.frame_sz = PAGE_SIZE;
		xdpb.rxq = &q->xdp_rxq;
		if (priv->prog)
			xdp_ret = onic_run_xdp(priv->prog, &xdpb);
		if ( xdp_ret == ONIC_XDP_PASS ) {

			if (priv->prog)
				priv->xdp_stats.xdp_passed++;
			skb = napi_alloc_skb(napi, len);
			if (!skb) {
				rv = -ENOMEM;
				break;
			}

			skb_put_data(skb, data, len);
			skb->protocol = eth_type_trans(skb, q->netdev);
			skb->ip_summed = CHECKSUM_NONE;
			/*

			skb = build_skb(xdpb.data, PAGE_SIZE);		// Needs space at tail for struct skb_shared_info, handled through pparams->max_len
			page_pool_release_page(q->ppool, (struct page *)page);	// Disconnect page from page pool, to allow for regular page usage
			*/
			skb_record_rx_queue(skb, qid);
			page_pool_put_page(q->ppool, (struct page *)page, PAGE_SIZE, 0);	// Return page to page pool

			rv = napi_gro_receive(napi, skb);
			if (rv < 0) {
				netdev_err(q->netdev, "napi_gro_receive, err = %d", rv);
				break;
			}
		} else if (xdp_ret == ONIC_XDP_DROP) {
			priv->xdp_stats.xdp_dropped++;
			netdev_info(q->netdev, "xdp_dropped: %llu\n", priv->xdp_stats.xdp_dropped);
			page_pool_put_page(q->ppool, (struct page *)page, PAGE_SIZE, 0);	// Return page to page pool
		} else if (xdp_ret == ONIC_XDP_TX) {
			int ret;
			struct xdp_frame *xdpf;
			xdpf = kzalloc(sizeof(*xdpf), GFP_ATOMIC);
			if (!xdpf) {
				priv->xdp_stats.xdp_tx_dropped++;
				page_pool_put_page(q->ppool, (struct page *)page, PAGE_SIZE, 0);	// Return page to page pool
			} else {
				ret  = xdp_update_frame_from_buff(&xdpb, xdpf);
				if (ret < 0) {
					priv->xdp_stats.xdp_tx_dropped++;
					page_pool_put_page(q->ppool, (struct page *)page, PAGE_SIZE, 0);	// Return page to page pool
					kfree(xdpf);
				} else {
					onic_xmit_xdp_frame(xdpf, q->netdev, qid);
				}
			}
		}
		priv->netdev_stats.rx_packets++;
		priv->netdev_stats.rx_bytes += len;

		onic_ring_increment_tail(desc_ring);

		if (debug)
			netdev_info(
				q->netdev,
				"desc_ring %u next_to_use:%u next_to_clean:%u",
				onic_ring_get_real_count(desc_ring),
				desc_ring->next_to_use,
				desc_ring->next_to_clean);
		if (onic_ring_full(desc_ring)) {
			netdev_dbg(q->netdev, "desc_ring full");
		}

		if (onic_rx_high_watermark(q)) {
			netdev_dbg(q->netdev, "High watermark: h = %d, t = %d",
				   desc_ring->next_to_use,
				   desc_ring->next_to_clean);
			onic_rx_refill(q);
		}

		onic_ring_increment_tail(cmpl_ring);

		if (debug)
			netdev_info(
				q->netdev,
				"cmpl_ring %u next_to_use:%u next_to_clean:%u, flipped:%s",
				onic_ring_get_real_count(cmpl_ring),
				cmpl_ring->next_to_use,
				cmpl_ring->next_to_clean,
				flipped ? "true" : "false");
		if (onic_ring_full(cmpl_ring)) {
			netdev_dbg(q->netdev, "cmpl_ring full");
		}
		if (cmpl.color != cmpl_ring->color) {
			if (debug)
				netdev_info(
					q->netdev,
					"part 1. cmpl_ring->next_to_clean=%u color *** old fliping *** color[%u]",
					cmpl_ring->next_to_clean,
					cmpl_ring->color);
			cmpl_ring->color = (cmpl_ring->color == 0) ? 1 : 0;
			flipped = 1;
		}
		cmpl_ptr = cmpl_ring->desc +
			   (QDMA_C2H_CMPL_SIZE * cmpl_ring->next_to_clean);

		if ((++work) >= budget) {
			if (debug)
				netdev_info(q->netdev,
					    "watchdog work %u, budget %u", work,
					    budget);
			napi_complete(napi);
			napi_reschedule(napi);
			goto out_of_budget;
		}

		qdma_unpack_c2h_cmpl(&cmpl, cmpl_ptr);

		if (debug)
			netdev_info(
				q->netdev,
				"c2h_cmpl(b) pkt_id %u, pkt_len %u, error %u, color %u",
				cmpl.pkt_id, cmpl.pkt_len, cmpl.err,
				cmpl.color);
	}

	if (cmpl_ring->next_to_clean == cmpl_stat.pidx) {
		if (debug)
			netdev_info(
				q->netdev,
				"next_to_clean == cmpl_stat.pidx %u, napi_complete work %u, budget %u, rval %s",
				cmpl_stat.pidx, work, budget,
				napi_cmpl_rval ? "true" : "false");
		napi_cmpl_rval = napi_complete_done(napi, work);
		onic_set_completion_tail(priv->hw.qdma, qid,
					 cmpl_ring->next_to_clean, 1);
		if (debug)
			netdev_info(q->netdev, "onic_set_completion_tail ");
	} else if (cmpl_ring->next_to_clean == 0) {
		if (debug)
			netdev_info(
				q->netdev,
				"next_to_clean == 0, napi_complete work %u, budget %u, rval %s",
				work, budget,
				napi_cmpl_rval ? "true" : "false");
		if (debug)
			netdev_info(q->netdev,
				    "napi_complete work %u, budget %u, rval %s",
				    work, budget,
				    napi_cmpl_rval ? "true" : "false");
		napi_cmpl_rval = napi_complete_done(napi, work);
		onic_set_completion_tail(priv->hw.qdma, qid,
					 cmpl_ring->next_to_clean, 1);
		if (debug)
			netdev_info(q->netdev, "onic_set_completion_tail ");
	}

out_of_budget:
	if (debug)
		netdev_info(q->netdev, "rx_poll is done");
	if (debug)
		netdev_info(
			q->netdev,
			"rx_poll returning work %u, rx_packets %lld, rx_bytes %lld",
			work, priv->netdev_stats.rx_packets,
			priv->netdev_stats.rx_bytes);
	return work;
}

static void onic_clear_tx_queue(struct onic_private *priv, u16 qid)
{
	struct onic_tx_queue *q = priv->tx_queue[qid];
	struct onic_ring *ring;
	u32 size;
	int real_count;

	if (!q)
		return;

	onic_qdma_clear_tx_queue(priv->hw.qdma, qid);

	ring = &q->ring;
	real_count = onic_ring_get_real_count(ring);
	size = QDMA_H2C_ST_DESC_SIZE * real_count + QDMA_WB_STAT_SIZE;
	size = ALIGN(size, PAGE_SIZE);

	if (ring->desc)
		dma_free_coherent(&priv->pdev->dev, size, ring->desc,
				  ring->dma_addr);
	kfree(q->buffer);
	kfree(q);
	priv->tx_queue[qid] = NULL;
}

static int onic_init_tx_queue(struct onic_private *priv, u16 qid)
{
	const u8 rngcnt_idx = 0;
	struct net_device *dev = priv->netdev;
	struct onic_tx_queue *q;
	struct onic_ring *ring;
	struct onic_qdma_h2c_param param;
	u16 vid;
	u32 size, real_count;
	int rv;
	bool debug = 0;

	if (priv->tx_queue[qid]) {
		if (debug)
			netdev_info(dev, "Re-initializing TX queue %d", qid);
		onic_clear_tx_queue(priv, qid);
	}

	q = kzalloc(sizeof(struct onic_tx_queue), GFP_KERNEL);
	if (!q)
		return -ENOMEM;

	/* evenly assign to TX queues available vectors */
	vid = qid % priv->num_q_vectors;

	q->netdev = dev;
	q->vector = priv->q_vector[vid];
	q->qid = qid;

	ring = &q->ring;
	ring->count = onic_ring_count(rngcnt_idx);
	real_count = onic_ring_get_real_count(ring);

	/* allocate DMA memory for TX descriptor ring */
	size = QDMA_H2C_ST_DESC_SIZE * real_count + QDMA_WB_STAT_SIZE;
	size = ALIGN(size, PAGE_SIZE);
	ring->desc = dma_alloc_coherent(&priv->pdev->dev, size, &ring->dma_addr,
					GFP_KERNEL);
	if (!ring->desc) {
		rv = -ENOMEM;
		goto clear_tx_queue;
	}
	memset(ring->desc, 0, size);
	ring->wb = ring->desc + QDMA_H2C_ST_DESC_SIZE * real_count;
	ring->next_to_use = 0;
	ring->next_to_clean = 0;
	ring->color = 0;

	/* initialize TX buffers */
	q->buffer =
		kcalloc(real_count, sizeof(struct onic_tx_buffer), GFP_KERNEL);
	if (!q->buffer) {
		rv = -ENOMEM;
		goto clear_tx_queue;
	}

	/* initialize QDMA H2C queue */
	param.rngcnt_idx = rngcnt_idx;
	param.dma_addr = ring->dma_addr;
	param.vid = vid;
	rv = onic_qdma_init_tx_queue(priv->hw.qdma, qid, &param);
	if (rv < 0)
		goto clear_tx_queue;

	priv->tx_queue[qid] = q;
	return 0;

clear_tx_queue:
	onic_clear_tx_queue(priv, qid);
	return rv;
}

static void onic_clear_rx_queue(struct onic_private *priv, u16 qid)
{
	struct onic_rx_queue *q = priv->rx_queue[qid];
	struct onic_ring *ring;
	struct net_device *dev = priv->netdev;
	u32 size, real_count;
	int i;

	if (!q)
		return;

	onic_qdma_clear_rx_queue(priv->hw.qdma, qid);

	napi_disable(&q->napi);
	netif_napi_del(&q->napi);

	ring = &q->desc_ring;
	real_count = onic_ring_get_real_count(ring);
	size = QDMA_C2H_ST_DESC_SIZE * real_count + QDMA_WB_STAT_SIZE;
	size = ALIGN(size, PAGE_SIZE);

	if (ring->desc)
		dma_free_coherent(&priv->pdev->dev, size, ring->desc,
				  ring->dma_addr);

	ring = &q->cmpl_ring;
	real_count = onic_ring_get_real_count(ring);
	size = QDMA_C2H_CMPL_SIZE * real_count + QDMA_C2H_CMPL_STAT_SIZE;
	size = ALIGN(size, PAGE_SIZE);

	if (ring->desc)
		dma_free_coherent(&priv->pdev->dev, size, ring->desc,
				  ring->dma_addr);

	for (i = 0; i < real_count; ++i) {
		struct page *pg = q->buffer[i].pg;
		page_pool_recycle_direct(q->ppool, pg);
	}
	netdev_info(dev, "Freed memory for %d pages ", real_count);

	kfree(q->buffer);

	xdp_rxq_info_unreg_mem_model(&q->xdp_rxq);
	if(xdp_rxq_info_is_reg(&q->xdp_rxq))
		xdp_rxq_info_unreg(&q->xdp_rxq);


	if (q->pparam)
		kfree(q->pparam);
	kfree(q);
	priv->rx_queue[qid] = NULL;
}

static int onic_xdp_setup(struct net_device *dev, struct bpf_prog *prog, struct netlink_ext_ack *extack)
//extack is a mechanism to communicate with the user space via netlink
{
	int running, need_update;
	struct onic_private *priv = netdev_priv(dev);
	struct bpf_prog *old_prog;

	if (prog && (dev->mtu > ONIC_MAX_QDMA_BUF_SIZE)) {
		NL_SET_ERR_MSG_MOD(extack, "Program does not support XDP fragments\n"); //*_MOD() includes module name in error message
		return -EOPNOTSUPP;
	}

	need_update = !!priv->prog != !!prog;
	if (need_update) {
		running = netif_running(dev);
		if (running)
			onic_stop_netdev(dev);
		old_prog = xchg(&priv->prog, prog);
		if (old_prog)
			bpf_prog_put(old_prog);	//Needs to be bpf_prog_put since driver owns old_prog 
		if (running)
			onic_open_netdev(dev);
	}
	return 0;
}

int onic_xdp(struct net_device *dev, struct netdev_bpf *bpf)
{
	switch(bpf->command) {
	case XDP_SETUP_PROG:
		return onic_xdp_setup(dev, bpf->prog, bpf->extack);
	default:
		return -EINVAL;
	}
}

static void init_pparam(struct page_pool_params *pparams, struct onic_private *priv, const u8 desc_rngcnt_idx)
{
	pparams->order = 0;		// If order > 0, then multiple block of pages are requested per packet
					// e.g. Jumbo packets can ask for order 2 = 4 pages for 9000B packets
	//pparams->flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV;
	pparams->flags = 0;	// If I ask the page pool API to do DMA mapping, it fails to allocate memory. Why?
	pparams->pool_size = onic_ring_count(desc_rngcnt_idx);
	pparams->nid = 0;
	pparams->dev = &priv->netdev->dev;
	pparams->offset = XDP_PACKET_HEADROOM;
	pparams->dma_dir = DMA_BIDIRECTIONAL;
	pparams->max_len = PAGE_SIZE - (SKB_DATA_ALIGN(sizeof(struct skb_shared_info)) + pparams->offset);	//To align with build_skb() call
}

static int onic_init_rx_queue(struct onic_private *priv, u16 qid)
{
	const u8 bufsz_idx = 13;
	const u8 desc_rngcnt_idx = 13;
	//const u8 cmpl_rngcnt_idx = 15;
	const u8 cmpl_rngcnt_idx = 13;
	struct net_device *dev = priv->netdev;
	struct onic_rx_queue *q;
	struct onic_ring *ring;
	struct onic_qdma_c2h_param param;
	struct page_pool_params *pparam;
	struct page_pool *ppool;
	u16 vid;
	u32 size, real_count;
	int i, rv;
	int err;
	bool debug = 0;

	if (priv->rx_queue[qid]) {
		if (debug)
			netdev_info(dev, "Re-initializing RX queue %d", qid);
		onic_clear_rx_queue(priv, qid);
	}

	q = kzalloc(sizeof(struct onic_rx_queue), GFP_KERNEL);
	if (!q)
		return -ENOMEM;

	netdev_info(dev, "Allocated memory for onic_rx_queue ");
	/* evenly assign to RX queues available vectors */
	vid = qid % priv->num_q_vectors;

	q->netdev = dev;
	q->vector = priv->q_vector[vid];
	q->qid = qid;

	/* Setup per queue page pool */
	pparam = kzalloc(sizeof(struct page_pool_params), GFP_KERNEL);
	if (!pparam)
		goto clear_rx_queue;

	init_pparam(pparam, priv, desc_rngcnt_idx);
	ppool = page_pool_create(pparam);	// Only ring is initialized, pages are not allocated yet.
	if (!ppool)
		goto clear_rx_queue;
	q->ppool = ppool;
	q->pparam = pparam;

	err = xdp_rxq_info_reg(&q->xdp_rxq, q->netdev, q->qid, 0);
	if (err) {
		netdev_info(dev, "Failed to register device and queue for xdp");
		goto clear_rx_queue;
	}

	err = xdp_rxq_info_reg_mem_model(&q->xdp_rxq, MEM_TYPE_PAGE_POOL, q->ppool);
	if (err) {
		netdev_info(dev, "Failed to register driver memory model with xdp");
		goto clear_rx_queue;
	}

	netdev_info(dev, "page pool size, order onic_rx_queue %d %d ", q->pparam->pool_size, q->pparam->order);

	/* allocate DMA memory for RX descriptor ring */
	ring = &q->desc_ring;
	ring->count = onic_ring_count(desc_rngcnt_idx);
	real_count = ring->count - 1;

	size = QDMA_C2H_ST_DESC_SIZE * real_count + QDMA_WB_STAT_SIZE;
	size = ALIGN(size, PAGE_SIZE);
	ring->desc = dma_alloc_coherent(&priv->pdev->dev, size, &ring->dma_addr,
					GFP_KERNEL);
	if (!ring->desc) {
		rv = -ENOMEM;
		goto clear_rx_queue;
	}
	netdev_info(dev, "Allocated memory for ring->desc ");
	memset(ring->desc, 0, size);
	ring->wb = ring->desc + QDMA_C2H_ST_DESC_SIZE * real_count;
	ring->next_to_use = 0;
	ring->next_to_clean = 0;
	ring->color = 0;

	/* initialize RX buffers */
	q->buffer =
		kcalloc(real_count, sizeof(struct onic_rx_buffer), GFP_KERNEL);
	if (!q->buffer) {
		rv = -ENOMEM;
		goto clear_rx_queue;
	}
	netdev_info(dev, "Allocated memory for q->buffer ");

	for (i = 0; i < real_count; ++i) {
		struct page *pg = page_pool_dev_alloc_pages(q->ppool);
		if (!pg) {
			rv = -ENOMEM;
			goto clear_rx_queue;
		}
		//netdev_info(dev, "Allocated memory for page %d ", i);

		q->buffer[i].pg = pg;
		q->buffer[i].offset = q->pparam->offset;
	}
	netdev_info(dev, "Allocated memory for %d pages ", real_count);

	/* map pages and initialize descriptors */
	for (i = 0; i < real_count; ++i) {
		u8 *desc_ptr = ring->desc + QDMA_C2H_ST_DESC_SIZE * i;
		struct qdma_c2h_st_desc desc;
		struct page *pg = q->buffer[i].pg;
		unsigned int offset = q->buffer[i].offset;

		desc.dst_addr = dma_map_page(&priv->pdev->dev, pg, 0, PAGE_SIZE,
					     DMA_FROM_DEVICE);
		desc.dst_addr += offset;

		qdma_pack_c2h_st_desc(desc_ptr, &desc);
	}

	/* allocate DMA memory for completion ring */
	ring = &q->cmpl_ring;
	ring->count = onic_ring_count(cmpl_rngcnt_idx);
	real_count = ring->count - 1;

	size = QDMA_C2H_CMPL_SIZE * real_count + QDMA_C2H_CMPL_STAT_SIZE;
	size = ALIGN(size, PAGE_SIZE);
	ring->desc = dma_alloc_coherent(&priv->pdev->dev, size, &ring->dma_addr,
					GFP_KERNEL);
	if (!ring->desc) {
		rv = -ENOMEM;
		goto clear_rx_queue;
	}
	netdev_info(dev, "Allocated memory for completion ring ");
	memset(ring->desc, 0, size);
	ring->wb = ring->desc + QDMA_C2H_CMPL_SIZE * real_count;
	ring->next_to_use = 0;
	ring->next_to_clean = 0;
	ring->color = 1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,1,0)
	netif_napi_add(dev, &q->napi, onic_rx_poll);
#else
	netif_napi_add(dev, &q->napi, onic_rx_poll, 64);
#endif
	napi_enable(&q->napi);

	/* initialize QDMA C2H queue */
	param.bufsz_idx = bufsz_idx;
	param.desc_rngcnt_idx = desc_rngcnt_idx;
	param.cmpl_rngcnt_idx = cmpl_rngcnt_idx;
	param.cmpl_desc_sz = 0;
	param.desc_dma_addr = q->desc_ring.dma_addr;
	param.cmpl_dma_addr = q->cmpl_ring.dma_addr;
	param.vid = vid;
	if (debug)
		netdev_info(
			dev,
			"bufsz_idx %u, desc_rngcnt_idx %u, cmpl_rngcnt_idx %u, desc_dma_addr 0x%llx, cmpl_dma_addr 0x%llx, vid %d",
			bufsz_idx, desc_rngcnt_idx, cmpl_rngcnt_idx,
			q->desc_ring.dma_addr, q->cmpl_ring.dma_addr, vid);

	rv = onic_qdma_init_rx_queue(priv->hw.qdma, qid, &param);
	if (rv < 0)
		goto clear_rx_queue;

	/* fill RX descriptor ring with a few descriptors */
	q->desc_ring.next_to_use = ONIC_RX_DESC_STEP;
	onic_set_rx_head(priv->hw.qdma, qid, q->desc_ring.next_to_use);
	onic_set_completion_tail(priv->hw.qdma, qid, 0, 1);

	priv->rx_queue[qid] = q;
	return 0;

clear_rx_queue:
	onic_clear_rx_queue(priv, qid);

	return rv;
}

static int onic_init_tx_resource(struct onic_private *priv)
{
	struct net_device *dev = priv->netdev;
	int qid, rv;

	for (qid = 0; qid < priv->num_tx_queues; ++qid) {
		rv = onic_init_tx_queue(priv, qid);
		if (!rv)
			continue;

		netdev_err(dev, "onic_init_tx_queue %d, err = %d", qid, rv);
		goto clear_tx_resource;
	}

	return 0;

clear_tx_resource:
	while (qid--)
		onic_clear_tx_queue(priv, qid);
	return rv;
}

static int onic_init_rx_resource(struct onic_private *priv)
{
	struct net_device *dev = priv->netdev;
	int qid, rv;

	for (qid = 0; qid < priv->num_rx_queues; ++qid) {
		rv = onic_init_rx_queue(priv, qid);
		if (!rv)
			continue;

		netdev_err(dev, "onic_init_rx_queue %d, err = %d", qid, rv);
		goto clear_rx_resource;
	}

	return 0;

clear_rx_resource:
	while (qid--)
		onic_clear_rx_queue(priv, qid);
	return rv;
}

int onic_open_netdev(struct net_device *dev)
{
	struct onic_private *priv = netdev_priv(dev);
	int rv;

	rv = onic_init_tx_resource(priv);
	if (rv < 0)
		goto stop_netdev;

	rv = onic_init_rx_resource(priv);
	if (rv < 0)
		goto stop_netdev;

	netif_tx_start_all_queues(dev);
	netif_carrier_on(dev);
	return 0;

stop_netdev:
	onic_stop_netdev(dev);
	return rv;
}

int onic_stop_netdev(struct net_device *dev)
{
	struct onic_private *priv = netdev_priv(dev);
	int qid;

	/* stop sending */
	netif_carrier_off(dev);
	netif_tx_stop_all_queues(dev);

	for (qid = 0; qid < priv->num_tx_queues; ++qid)
		onic_clear_tx_queue(priv, qid);
	for (qid = 0; qid < priv->num_rx_queues; ++qid)
		onic_clear_rx_queue(priv, qid);

	return 0;
}

netdev_tx_t onic_xmit_frame(struct sk_buff *skb, struct net_device *dev)
{
	struct onic_private *priv = netdev_priv(dev);
	struct onic_tx_queue *q;
	struct onic_ring *ring;
	struct qdma_h2c_st_desc desc;
	u16 qid = skb->queue_mapping;
	dma_addr_t dma_addr;
	u8 *desc_ptr;
	int rv;
	bool debug = 0;

	q = priv->tx_queue[qid];
	ring = &q->ring;

	onic_tx_clean(q);

	if (onic_ring_full(ring)) {
		if (debug)
			netdev_info(dev, "ring is full");
		return NETDEV_TX_BUSY;
	}

	/* minimum Ethernet packet length is 60 */
	rv = skb_put_padto(skb, ETH_ZLEN);

	if (rv < 0)
		netdev_err(dev, "skb_put_padto failed, err = %d", rv);

	dma_addr = dma_map_single(&priv->pdev->dev, skb->data, skb->len,
				  DMA_TO_DEVICE);

	if (unlikely(dma_mapping_error(&priv->pdev->dev, dma_addr))) {
		dev_kfree_skb(skb);
		priv->netdev_stats.tx_dropped++;
		priv->netdev_stats.tx_errors++;
		/* Why is this returing TX_OK when it has failed ? */
		return NETDEV_TX_OK;
	}

	desc_ptr = ring->desc + QDMA_H2C_ST_DESC_SIZE * ring->next_to_use;
	desc.len = skb->len;
	desc.src_addr = dma_addr;
	desc.metadata = skb->len;
	qdma_pack_h2c_st_desc(desc_ptr, &desc);

	q->buffer[ring->next_to_use].skb = skb;
	q->buffer[ring->next_to_use].dma_addr = dma_addr;
	q->buffer[ring->next_to_use].len = skb->len;
	q->buffer[ring->next_to_use].type = ONIC_SKB_BUFF;

	priv->netdev_stats.tx_packets++;
	priv->netdev_stats.tx_bytes += skb->len;

	onic_ring_increment_head(ring);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
	if (onic_ring_full(ring) || !netdev_xmit_more()) {
#elif defined(RHEL_RELEASE_CODE) 
#if RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(8, 1)
        if (onic_ring_full(ring) || !netdev_xmit_more()) {
#endif
#else
	if (onic_ring_full(ring) || !skb->xmit_more) {
#endif
		wmb();
		onic_set_tx_head(priv->hw.qdma, qid, ring->next_to_use);
	}

	return NETDEV_TX_OK;
}

int onic_xmit_xdp_frame(struct xdp_frame *xdpf, struct net_device *dev, int rx_qid)
{
	struct onic_private *priv = netdev_priv(dev);
	struct onic_tx_queue *q;
	struct onic_ring *ring;
	struct netdev_queue *nq;
	struct page *page = virt_to_page(xdpf->data);
	struct qdma_h2c_st_desc desc;
	u16 qid = rx_qid;
	dma_addr_t dma_addr;
	u8 *desc_ptr;
	bool debug = 0;

	q = priv->tx_queue[qid];
	nq = netdev_get_tx_queue(dev, qid);
	__netif_tx_lock(nq, raw_smp_processor_id());
	ring = &q->ring;

	onic_tx_clean(q);

	if (onic_ring_full(ring)) {
		if (debug)
			netdev_info(dev, "ring is full");
		xdp_return_frame_rx_napi(xdpf);	
		__netif_tx_unlock(nq);
		return -1;
	}
	/* How does XDP frame ensure min length of 64 Bytes ? */
	dma_addr = page_pool_get_dma_addr(page) + sizeof(*xdpf) + xdpf->headroom;
	dma_sync_single_for_device(&priv->pdev->dev, dma_addr, xdpf->len,
				  DMA_BIDIRECTIONAL);

	desc_ptr = ring->desc + QDMA_H2C_ST_DESC_SIZE * ring->next_to_use;
	desc.len = xdpf->len;
	desc.src_addr = dma_addr;
	desc.metadata = xdpf->len;
	qdma_pack_h2c_st_desc(desc_ptr, &desc);

	q->buffer[ring->next_to_use].xdpf = xdpf;
	q->buffer[ring->next_to_use].dma_addr = dma_addr;
	q->buffer[ring->next_to_use].len = xdpf->len;
	q->buffer[ring->next_to_use].type = ONIC_XDP_FRAME;

	priv->xdp_stats.xdp_txed++;
	netdev_info(dev, "XDP txed = %llu", priv->xdp_stats.xdp_txed);

	onic_ring_increment_head(ring);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
	if (onic_ring_full(ring) || !netdev_xmit_more()) {
#elif defined(RHEL_RELEASE_CODE) 
#if RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(8, 1)
        if (onic_ring_full(ring) || !netdev_xmit_more()) {
#endif
#else
	if (onic_ring_full(ring) || !skb->xmit_more) {
#endif
		wmb();
		onic_set_tx_head(priv->hw.qdma, qid, ring->next_to_use);
	}
	__netif_tx_unlock(nq);
	return 0;
}

int onic_set_mac_address(struct net_device *dev, void *addr)
{
	struct sockaddr *saddr = addr;
	u8 *dev_addr = saddr->sa_data;
	if (!is_valid_ether_addr(saddr->sa_data))
		return -EADDRNOTAVAIL;

	netdev_info(dev, "Set MAC address to %02x:%02x:%02x:%02x:%02x:%02x",
			dev_addr[0], dev_addr[1], dev_addr[2],
			dev_addr[3], dev_addr[4], dev_addr[5]);
	eth_hw_addr_set(dev, dev_addr);
	return 0;
}

int onic_do_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	return 0;
}

int onic_change_mtu(struct net_device *dev, int mtu)
{
	netdev_info(dev, "Requested MTU = %d", mtu);
	return 0;
}

inline void onic_get_stats64(struct net_device *dev,
			     struct rtnl_link_stats64 *stats)
{
	struct onic_private *priv = netdev_priv(dev);

	stats->tx_packets = priv->netdev_stats.tx_packets;
	stats->tx_bytes = priv->netdev_stats.tx_bytes;
	stats->rx_packets = priv->netdev_stats.rx_packets;
	stats->rx_bytes = priv->netdev_stats.rx_bytes;
	stats->tx_dropped = priv->netdev_stats.tx_dropped;
	stats->tx_errors = priv->netdev_stats.tx_errors;
}
