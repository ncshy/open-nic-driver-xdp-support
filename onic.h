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
#ifndef __ONIC_H__
#define __ONIC_H__

#include <linux/netdevice.h>
#include <linux/cpumask.h>
#include <net/xdp.h>

#include "onic_hardware.h"

#define ONIC_MAX_QUEUES			64
#define ONIC_MAX_QDMA_BUF_SIZE		PAGE_SIZE - XDP_PACKET_HEADROOM
/* state bits */
#define ONIC_ERROR_INTR			0
#define ONIC_USER_INTR			1

/* flag bits */
#define ONIC_FLAG_MASTER_PF		0


#define ONIC_SKB_BUFF 0
#define ONIC_XDP_FRAME 1
struct onic_tx_buffer {
	union {
		struct sk_buff *skb;
		struct xdp_frame *xdpf;
	};
	dma_addr_t dma_addr;
	u32 len;
	u32 type;
	u64 time_stamp;
};

struct onic_rx_buffer {
	struct page *pg;
	unsigned int offset;
	u64 time_stamp;
};

enum {
	ONIC_XDP_PASS = 0,
	ONIC_XDP_TX,
	ONIC_XDP_REDIRECT,
	ONIC_XDP_DROP
};
/**
 * struct onic_ring - generic ring structure
 **/
struct onic_ring {
	u16 count;		/* number of descriptors */
	u8 *desc;		/* base address for descriptors */
	u8 *wb;			/* descriptor writeback */
	dma_addr_t dma_addr;	/* DMA address for descriptors */

	u16 next_to_use;
	u16 next_to_clean;
	u8 color;
};

struct onic_tx_queue {
	struct net_device *netdev;
	u16 qid;
	DECLARE_BITMAP(state, 32);

	struct onic_tx_buffer *buffer;
	struct onic_ring ring;
	struct onic_q_vector *vector;
};

/* Check cache line size */
struct onic_rx_queue {
	// 1st cache line
	struct net_device *netdev;
	u16 qid;
	struct onic_rx_buffer *buffer;
	struct onic_ring desc_ring;	//40 bytes
	// 2nd cache line
	struct onic_ring cmpl_ring;
	struct onic_q_vector *vector;
	struct page_pool *ppool;
	struct page_pool_params *pparam;
	// 3rd cache line
	struct xdp_rxq_info xdp_rxq;	//Internally cache aligned
	// 4th cache line
	struct napi_struct napi;
};

struct onic_q_vector {
	u16 vid;
	struct onic_private *priv;
	struct cpumask affinity_mask;
	int numa_node;
};

struct onic_xdp_stats {
	u64 xdp_passed;
	u64 xdp_dropped;
	u64 xdp_redirected;
	u64 xdp_txed;
	u64 xdp_tx_dropped;
	u64 xdp_tx_errors;
};

/**
 * struct onic_private - OpenNIC driver private data
 **/
struct onic_private {
	struct list_head dev_list;

	struct pci_dev *pdev;
	DECLARE_BITMAP(state, 32);
	DECLARE_BITMAP(flags, 32);

        int RS_FEC;

	u16 num_q_vectors;
	u16 num_tx_queues;
	u16 num_rx_queues;

	struct net_device *netdev;
	struct rtnl_link_stats64 netdev_stats;
	spinlock_t tx_lock;
	spinlock_t rx_lock;

	struct onic_q_vector *q_vector[ONIC_MAX_QUEUES];
	struct onic_tx_queue *tx_queue[ONIC_MAX_QUEUES];
	struct onic_rx_queue *rx_queue[ONIC_MAX_QUEUES];

	struct onic_hardware hw;
	struct bpf_prog *prog;
	struct onic_xdp_stats xdp_stats; // Make it per cpu to avoid contention
};

#endif
