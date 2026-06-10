/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *   Copyright (C) 2009-2016 John Crispin <blogic@openwrt.org>
 *   Copyright (C) 2009-2016 Felix Fietkau <nbd@openwrt.org>
 *   Copyright (C) 2013-2016 Michael Lee <igvtee@gmail.com>
 */

#ifndef MT7988_DMA_H
#define MT7988_DMA_H

#include "mt7988_eth.h"
#include <linux/dma-mapping.h>
#include <linux/netdevice.h>
#include <net/page_pool/types.h>

/* ==========================================
 * DMA Initialization and Cleanup
 * ========================================== */
int mtk_dma_init(struct mtk_eth *eth);
void mtk_dma_free(struct mtk_eth *eth);
int mtk_start_dma(struct mtk_eth *eth);
void mtk_stop_dma(struct mtk_eth *eth, u32 glo_cfg);
int mtk_dma_busy_wait(struct mtk_eth *eth);

/* ==========================================
 * TX/RX Rings Allocation
 * ========================================== */
int mtk_tx_alloc(struct mtk_eth *eth);
void mtk_tx_clean(struct mtk_eth *eth);
int mtk_rx_alloc(struct mtk_eth *eth, int ring_no, int rx_flag);
void mtk_rx_clean(struct mtk_eth *eth, struct mtk_rx_ring *ring, bool in_sram);

/* ==========================================
 * NAPI & Polling
 * ========================================== */
int mtk_napi_tx(struct napi_struct *napi, int budget);
int mtk_napi_rx(struct napi_struct *napi, int budget);
int mtk_napi_init(struct mtk_eth *eth);

/* ==========================================
 * TX Data Path (Helpers)
 * ========================================== */
int mtk_cal_txd_req(struct mtk_eth *eth, struct sk_buff *skb);
void mtk_tx_set_dma_desc(struct net_device *dev, void *txd,
			 struct mtk_tx_dma_desc_info *info);
int mtk_tx_map(struct sk_buff *skb, struct net_device *dev, int tx_num,
	       struct mtk_tx_ring *ring, bool gso);
void mtk_tx_unmap(struct mtk_eth *eth, struct mtk_tx_buf *tx_buf,
		  struct xdp_frame_bulk *bq, bool napi);

/* ==========================================
 * RX Data Path (Helpers)
 * ========================================== */
bool mtk_rx_get_desc(struct mtk_eth *eth, struct mtk_rx_dma_v2 *rxd,
		     struct mtk_rx_dma_v2 *dma_rxd);
void mtk_update_rx_cpu_idx(struct mtk_eth *eth, struct mtk_rx_ring *ring);

/* ==========================================
 * Page Pool & Buffers
 * ========================================== */
struct page_pool *mtk_create_page_pool(struct mtk_eth *eth,
				       struct xdp_rxq_info *xdp_q, int id,
				       int size);
void *mtk_page_pool_get_buff(struct page_pool *pp, dma_addr_t *dma_addr,
			     gfp_t gfp_mask);
void mtk_rx_put_buff(struct mtk_rx_ring *ring, void *data, bool napi);
int mtk_max_frag_size(int mtu);
int mtk_max_buf_size(int frag_size);

/* ==========================================
 * NAPI
 * ========================================== */
int mtk_poll_rx(struct napi_struct *napi, int budget, struct mtk_eth *eth);
int mtk_poll_tx(struct mtk_eth *eth, int budget);

/* ==========================================
 * XDP DMA Helpers
 * ========================================== */
int mtk_xdp_frame_map(struct mtk_eth *eth, struct net_device *dev,
		      struct mtk_tx_dma_desc_info *txd_info,
		      struct mtk_tx_dma *txd, struct mtk_tx_buf *tx_buf,
		      void *data, u16 headroom, int index, bool dma_map);
int mtk_xdp_submit_frame(struct mtk_eth *eth, struct xdp_frame *xdpf,
			 struct net_device *dev, bool dma_map);

/* ==========================================
 * IRQ & Interrupt Coalescing (DIM)
 * ========================================== */
void mtk_tx_irq_disable(struct mtk_eth *eth, u32 mask);
void mtk_tx_irq_enable(struct mtk_eth *eth, u32 mask);
void mtk_rx_irq_disable(struct mtk_eth *eth, u32 mask);
void mtk_rx_irq_enable(struct mtk_eth *eth, u32 mask);
void mtk_dim_rx(struct work_struct *work);
void mtk_dim_tx(struct work_struct *work);

/* ==========================================
 * RSS & QDMA Scheduler
 * ========================================== */
u32 mtk_rss_indr_table(struct mtk_rss_params *rss_params, int index);
void mtk_set_queue_speed(struct mtk_eth *eth, unsigned int idx, int speed);

/* ==========================================
 * HW LRO
 * ========================================== */
int mtk_hwlro_rx_init(struct mtk_eth *eth);
void mtk_hwlro_rx_uninit(struct mtk_eth *eth);
int mtk_hwlro_get_ip_cnt(struct mtk_mac *mac);
int mtk_hwlro_add_ipaddr(struct net_device *dev, struct ethtool_rxnfc *cmd);
int mtk_hwlro_del_ipaddr(struct net_device *dev, struct ethtool_rxnfc *cmd);
void mtk_hwlro_netdev_disable(struct net_device *dev);
int mtk_hwlro_get_fdir_entry(struct net_device *dev, struct ethtool_rxnfc *cmd);
int mtk_hwlro_get_fdir_all(struct net_device *dev, struct ethtool_rxnfc *cmd,
			   u32 *rule_locs);

#endif /* MT7988_DMA_H */
