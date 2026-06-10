// SPDX-License-Identifier: GPL-2.0-only
/*
 *   Copyright (C) 2009-2016 John Crispin <blogic@openwrt.org>
 *   Copyright (C) 2009-2016 Felix Fietkau <nbd@openwrt.org>
 *   Copyright (C) 2013-2016 Michael Lee <igvtee@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/bpf_trace.h>
#include <linux/dim.h>
#include <linux/dma-mapping.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jhash.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/tcp.h>
#include <net/dsa.h>
#include <net/page_pool/helpers.h>
#include <net/xdp.h>

#include "mt7988_dma.h"
#include "mt7988_eth.h"

struct mtk_poll_state {
	struct netdev_queue *txq;
	unsigned int total;
	unsigned int done;
	unsigned int bytes;
};

static void mtk_stats_update(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->mac[i] || !eth->mac[i]->hw_stats)
			continue;
		if (spin_trylock(&eth->mac[i]->hw_stats->stats_lock)) {
			mtk_stats_update_mac(eth->mac[i]);
			spin_unlock(&eth->mac[i]->hw_stats->stats_lock);
		}
	}
}

static void mtk_handle_status_irq(struct mtk_eth *eth)
{
	u32 status2 = mtk_r32(eth, MTK_FE_INT_STATUS);

	if (unlikely(status2 & (MTK_GDM1_AF | MTK_GDM2_AF))) {
		mtk_stats_update(eth);
		mtk_w32(eth, (MTK_GDM1_AF | MTK_GDM2_AF), MTK_FE_INT_STATUS);
	}
}

static void mtk_hwlro_val_ipaddr(struct mtk_eth *eth, int idx, __be32 ip)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	u32 reg_val;

	idx += 1;

	reg_val = mtk_r32(eth, MTK_LRO_CTRL_DW2_CFG(idx));

	/* invalidate the IP setting */
	mtk_w32(eth, (reg_val & ~MTK_RING_MYIP_VLD), MTK_LRO_CTRL_DW2_CFG(idx));

	mtk_w32(eth, ip, MTK_LRO_DIP_DW0_CFG(idx));

	/* validate the IP setting */
	mtk_w32(eth, (reg_val | MTK_RING_MYIP_VLD), MTK_LRO_CTRL_DW2_CFG(idx));
}

static void mtk_hwlro_inval_ipaddr(struct mtk_eth *eth, int idx)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	u32 reg_val;

	idx += 1;

	reg_val = mtk_r32(eth, MTK_LRO_CTRL_DW2_CFG(idx));

	/* invalidate the IP setting */
	mtk_w32(eth, (reg_val & ~MTK_RING_MYIP_VLD), MTK_LRO_CTRL_DW2_CFG(idx));

	mtk_w32(eth, 0, MTK_LRO_DIP_DW0_CFG(idx));
}

int mtk_hwlro_get_ip_cnt(struct mtk_mac *mac)
{
	int cnt = 0;
	int i;

	for (i = 0; i < MTK_MAX_LRO_IP_CNT; i++) {
		if (mac->hwlro_ip[i])
			cnt++;
	}

	return cnt;
}

int mtk_max_frag_size(int mtu)
{
	/* make sure buf_size will be at least MTK_MAX_RX_LENGTH */
	if (mtu + MTK_RX_ETH_HLEN < MTK_MAX_RX_LENGTH_2K)
		mtu = MTK_MAX_RX_LENGTH_2K - MTK_RX_ETH_HLEN;

	return SKB_DATA_ALIGN(MTK_RX_HLEN + mtu) +
	       SKB_DATA_ALIGN(sizeof(struct skb_shared_info));
}

int mtk_max_buf_size(int frag_size)
{
	int buf_size = frag_size - NET_SKB_PAD - NET_IP_ALIGN -
		       SKB_DATA_ALIGN(sizeof(struct skb_shared_info));

	WARN_ON(buf_size < MTK_MAX_RX_LENGTH_2K);

	return buf_size;
}

bool mtk_rx_get_desc(struct mtk_eth *eth, struct mtk_rx_dma_v2 *rxd,
		     struct mtk_rx_dma_v2 *dma_rxd)
{
	rxd->rxd2 = READ_ONCE(dma_rxd->rxd2);
	if (!(rxd->rxd2 & RX_DMA_DONE)) {
		return false;
	}
	rxd->rxd1 = READ_ONCE(dma_rxd->rxd1);
	rxd->rxd3 = READ_ONCE(dma_rxd->rxd3);
	rxd->rxd4 = READ_ONCE(dma_rxd->rxd4);
	rxd->rxd5 = READ_ONCE(dma_rxd->rxd5);
	rxd->rxd6 = READ_ONCE(dma_rxd->rxd6);
	rxd->rxd7 = READ_ONCE(dma_rxd->rxd7);

	return true;
}

static void *mtk_qdma_phys_to_virt(struct mtk_tx_ring *ring, u32 desc)
{
	return ring->dma + (desc - ring->phys);
}

static struct mtk_tx_buf *mtk_desc_to_tx_buf(struct mtk_tx_ring *ring,
					     void *txd, u32 txd_size)
{
	int idx = (txd - ring->dma) / txd_size;

	return &ring->buf[idx];
}

void mtk_update_rx_cpu_idx(struct mtk_eth *eth, struct mtk_rx_ring *ring)
{
	mtk_w32(eth, ring->calc_idx, ring->crx_idx_reg);
}

/* the qdma core needs scratch memory to be setup */
static int mtk_init_fq_dma(struct mtk_eth *eth)
{
	const struct mtk_soc_data *soc = eth->soc;
	dma_addr_t phy_ring_tail;
	int cnt = soc->tx.fq_dma_size;
	dma_addr_t dma_addr;
	int i, j, len;

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_SRAM))
		eth->scratch_ring = eth->sram_base;
	else
		eth->scratch_ring = dma_alloc_coherent(eth->dma_dev,
						       cnt * soc->tx.desc_size,
						       &eth->phy_scratch_ring,
						       GFP_KERNEL);

	if (unlikely(!eth->scratch_ring))
		return -ENOMEM;

	phy_ring_tail = eth->phy_scratch_ring + soc->tx.desc_size * (cnt - 1);

	for (j = 0; j < DIV_ROUND_UP(soc->tx.fq_dma_size, MTK_FQ_DMA_LENGTH);
	     j++) {
		len = min_t(int, cnt - j * MTK_FQ_DMA_LENGTH,
			    MTK_FQ_DMA_LENGTH);
		eth->scratch_head[j] =
			kcalloc(len, MTK_QDMA_PAGE_SIZE, GFP_KERNEL);

		if (unlikely(!eth->scratch_head[j]))
			return -ENOMEM;

		dma_addr = dma_map_single(eth->dma_dev, eth->scratch_head[j],
					  len * MTK_QDMA_PAGE_SIZE,
					  DMA_FROM_DEVICE);

		if (unlikely(dma_mapping_error(eth->dma_dev, dma_addr)))
			return -ENOMEM;

		for (i = 0; i < len; i++) {
			struct mtk_tx_dma_v2 *txd;

			txd = eth->scratch_ring +
			      (j * MTK_FQ_DMA_LENGTH + i) * soc->tx.desc_size;
			WRITE_ONCE(txd->txd1,
				   dma_addr + i * MTK_QDMA_PAGE_SIZE);
			if (j * MTK_FQ_DMA_LENGTH + i < cnt)
				WRITE_ONCE(txd->txd2,
					   eth->phy_scratch_ring +
						   (j * MTK_FQ_DMA_LENGTH + i +
						    1) * soc->tx.desc_size);

			WRITE_ONCE(txd->txd3,
				   TX_DMA_PLEN0(MTK_QDMA_PAGE_SIZE) |
					   TX_DMA_PREP_ADDR64(
						   dma_addr +
						   i * MTK_QDMA_PAGE_SIZE));

			WRITE_ONCE(txd->txd4, 0);
			WRITE_ONCE(txd->txd5, 0);
			WRITE_ONCE(txd->txd6, 0);
			WRITE_ONCE(txd->txd7, 0);
			WRITE_ONCE(txd->txd8, 0);
		}
	}

	mtk_w32(eth, eth->phy_scratch_ring, soc->reg_map->qdma.fq_head);
	mtk_w32(eth, phy_ring_tail, soc->reg_map->qdma.fq_tail);
	mtk_w32(eth, (cnt << 16) | cnt, soc->reg_map->qdma.fq_count);
	mtk_w32(eth, MTK_QDMA_PAGE_SIZE << 16, soc->reg_map->qdma.fq_blen);

	return 0;
}

int mtk_tx_alloc(struct mtk_eth *eth)
{
	const struct mtk_soc_data *soc = eth->soc;
	struct mtk_tx_ring *ring = &eth->tx_ring;
	int i, sz = soc->tx.desc_size;
	struct mtk_tx_dma_v2 *txd;
	int ring_size = MTK_QDMA_RING_SIZE;
	u32 ofs, val;

	ring->buf = kcalloc(ring_size, sizeof(*ring->buf), GFP_KERNEL);
	if (!ring->buf)
		goto no_tx_mem;

	if (MTK_HAS_CAPS(soc->caps, MTK_SRAM)) {
		ring->dma = eth->sram_base + soc->tx.fq_dma_size * sz;
		ring->phys = eth->phy_scratch_ring +
			     soc->tx.fq_dma_size * (dma_addr_t)sz;
	} else {
		ring->dma = dma_alloc_coherent(eth->dma_dev, ring_size * sz,
					       &ring->phys, GFP_KERNEL);
	}

	if (!ring->dma)
		goto no_tx_mem;

	for (i = 0; i < ring_size; i++) {
		int next = (i + 1) % ring_size;
		u32 next_ptr = ring->phys + next * sz;

		txd = ring->dma + i * sz;

		WRITE_ONCE(txd->txd2, next_ptr);
		WRITE_ONCE(txd->txd3, TX_DMA_LS0 | TX_DMA_OWNER_CPU);
		WRITE_ONCE(txd->txd4, 0);
		WRITE_ONCE(txd->txd5, 0);
		WRITE_ONCE(txd->txd6, 0);
		WRITE_ONCE(txd->txd7, 0);
		WRITE_ONCE(txd->txd8, 0);
	}

	ring->dma_size = ring_size;
	atomic_set(&ring->free_count, ring_size - 2);
	ring->next_free = ring->dma;
	ring->last_free = (void *)txd;
	ring->last_free_ptr = (u32)(ring->phys + ((ring_size - 1) * sz));
	ring->thresh = MAX_SKB_FRAGS;

	/* make sure that all changes to the dma ring are flushed before we
	 * continue
	 */
	wmb();

	mtk_w32(eth, ring->phys, soc->reg_map->qdma.ctx_ptr);
	mtk_w32(eth, ring->phys, soc->reg_map->qdma.dtx_ptr);
	mtk_w32(eth, ring->phys + ((ring_size - 1) * sz),
		soc->reg_map->qdma.crx_ptr);
	mtk_w32(eth, ring->last_free_ptr, soc->reg_map->qdma.drx_ptr);

	for (i = 0, ofs = 0; i < MTK_QDMA_NUM_QUEUES; i++) {
		val = (QDMA_RES_THRES << 8) | QDMA_RES_THRES;
		mtk_w32(eth, val, soc->reg_map->qdma.qtx_cfg + ofs);

		val = MTK_QTX_SCH_MIN_RATE_EN |
		      /* minimum: 10 Mbps */
		      FIELD_PREP(MTK_QTX_SCH_MIN_RATE_MAN, 1) |
		      FIELD_PREP(MTK_QTX_SCH_MIN_RATE_EXP, 4) |
		      MTK_QTX_SCH_LEAKY_BUCKET_SIZE;
		mtk_w32(eth, val, soc->reg_map->qdma.qtx_sch + ofs);
		ofs += MTK_QTX_OFFSET;
	}
	val = MTK_QDMA_TX_SCH_MAX_WFQ | (MTK_QDMA_TX_SCH_MAX_WFQ << 16);
	mtk_w32(eth, val, soc->reg_map->qdma.tx_sch_rate);
	mtk_w32(eth, val, soc->reg_map->qdma.tx_sch_rate + 4);

	return 0;

no_tx_mem:
	return -ENOMEM;
}

void mtk_tx_clean(struct mtk_eth *eth)
{
	const struct mtk_soc_data *soc = eth->soc;
	struct mtk_tx_ring *ring = &eth->tx_ring;
	int i;

	if (ring->buf) {
		for (i = 0; i < ring->dma_size; i++)
			mtk_tx_unmap(eth, &ring->buf[i], NULL, false);
		kfree(ring->buf);
		ring->buf = NULL;
	}
	if (!MTK_HAS_CAPS(soc->caps, MTK_SRAM) && ring->dma) {
		dma_free_coherent(eth->dma_dev,
				  ring->dma_size * soc->tx.desc_size, ring->dma,
				  ring->phys);
		ring->dma = NULL;
	}
}

int mtk_rx_alloc(struct mtk_eth *eth, int ring_no, int rx_flag)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	const struct mtk_soc_data *soc = eth->soc;
	struct mtk_rx_ring *ring;
	int rx_data_len, rx_dma_size, tx_ring_size = MTK_QDMA_RING_SIZE;
	int i;

	if (rx_flag == MTK_RX_FLAGS_QDMA) {
		if (ring_no)
			return -EINVAL;
		ring = &eth->rx_ring_qdma;
	} else {
		ring = &eth->rx_ring[ring_no];
	}

	if (rx_flag == MTK_RX_FLAGS_HWLRO) {
		rx_data_len = MTK_MAX_LRO_RX_LENGTH;
		rx_dma_size = MTK_HW_LRO_DMA_SIZE;
	} else {
		rx_data_len = ETH_DATA_LEN;
		rx_dma_size = soc->rx.dma_size;
	}

	ring->frag_size = mtk_max_frag_size(rx_data_len);
	ring->buf_size = mtk_max_buf_size(ring->frag_size);
	ring->data = kcalloc(rx_dma_size, sizeof(*ring->data), GFP_KERNEL);
	if (!ring->data)
		return -ENOMEM;

	{
		struct page_pool *pp;

		pp = mtk_create_page_pool(eth, &ring->xdp_q, ring_no,
					  rx_dma_size);
		if (IS_ERR(pp))
			return PTR_ERR(pp);

		ring->page_pool = pp;
	}

	if (!MTK_HAS_CAPS(eth->soc->caps, MTK_SRAM) ||
	    rx_flag != MTK_RX_FLAGS_NORMAL) {
		ring->dma = dma_alloc_coherent(
			eth->dma_dev, rx_dma_size * eth->soc->rx.desc_size,
			&ring->phys, GFP_KERNEL);
	} else {
		struct mtk_tx_ring *tx_ring = &eth->tx_ring;

		ring->dma = tx_ring->dma + tx_ring_size *
						   eth->soc->tx.desc_size *
						   (ring_no + 1);
		ring->phys = tx_ring->phys + tx_ring_size *
						     eth->soc->tx.desc_size *
						     (ring_no + 1);
	}

	if (!ring->dma)
		return -ENOMEM;

	for (i = 0; i < rx_dma_size; i++) {
		struct mtk_rx_dma_v2 *rxd;
		dma_addr_t dma_addr;
		void *data;

		rxd = ring->dma + i * eth->soc->rx.desc_size;
		data = mtk_page_pool_get_buff(ring->page_pool, &dma_addr,
					      GFP_KERNEL);
		if (!data)
			return -ENOMEM;

		WRITE_ONCE(rxd->rxd1, (unsigned int)dma_addr);
		ring->data[i] = data;
		WRITE_ONCE(rxd->rxd2, RX_DMA_PREP_PLEN0(ring->buf_size) |
					      RX_DMA_PREP_ADDR64(dma_addr));
		WRITE_ONCE(rxd->rxd3, 0);
		WRITE_ONCE(rxd->rxd4, 0);
		WRITE_ONCE(rxd->rxd5, 0);
		WRITE_ONCE(rxd->rxd6, 0);
		WRITE_ONCE(rxd->rxd7, 0);
		WRITE_ONCE(rxd->rxd8, 0);
	}

	ring->dma_size = rx_dma_size;
	ring->calc_idx_update = false;
	ring->calc_idx = rx_dma_size - 1;
	if (rx_flag == MTK_RX_FLAGS_QDMA)
		ring->crx_idx_reg =
			reg_map->qdma.qcrx_ptr + ring_no * MTK_QRX_OFFSET;
	else
		ring->crx_idx_reg =
			reg_map->adma.pcrx_ptr + ring_no * MTK_QRX_OFFSET;
	ring->ring_no = ring_no;
	/* make sure that all changes to the dma ring are flushed before we
	 * continue
	 */
	wmb();

	if (rx_flag == MTK_RX_FLAGS_QDMA) {
		mtk_w32(eth, ring->phys,
			reg_map->qdma.rx_ptr + ring_no * MTK_QRX_OFFSET);
		mtk_w32(eth, rx_dma_size,
			reg_map->qdma.rx_cnt_cfg + ring_no * MTK_QRX_OFFSET);
		mtk_w32(eth, MTK_PST_DRX_IDX_CFG(ring_no),
			reg_map->qdma.rst_idx);
	} else {
		mtk_w32(eth, ring->phys,
			reg_map->adma.rx_ptr + ring_no * MTK_QRX_OFFSET);
		mtk_w32(eth, rx_dma_size,
			reg_map->adma.rx_cnt_cfg + ring_no * MTK_QRX_OFFSET);
		mtk_w32(eth, MTK_PST_DRX_IDX_CFG(ring_no),
			reg_map->adma.rst_idx);
	}
	mtk_w32(eth, ring->calc_idx, ring->crx_idx_reg);

	return 0;
}

void mtk_rx_clean(struct mtk_eth *eth, struct mtk_rx_ring *ring, bool in_sram)
{
	int i;

	if (ring->data && ring->dma) {
		for (i = 0; i < ring->dma_size; i++) {
			struct mtk_rx_dma *rxd;

			if (!ring->data[i])
				continue;

			rxd = ring->dma + i * eth->soc->rx.desc_size;
			if (!rxd->rxd1)
				continue;

			dma_unmap_single(eth->dma_dev, (u64)rxd->rxd1,
					 ring->buf_size, DMA_FROM_DEVICE);
			mtk_rx_put_buff(ring, ring->data[i], false);
		}
		kfree(ring->data);
		ring->data = NULL;
	}

	if (!in_sram && ring->dma) {
		dma_free_coherent(eth->dma_dev,
				  ring->dma_size * eth->soc->rx.desc_size,
				  ring->dma, ring->phys);
		ring->dma = NULL;
	}

	if (ring->page_pool) {
		if (xdp_rxq_info_is_reg(&ring->xdp_q))
			xdp_rxq_info_unreg(&ring->xdp_q);
		page_pool_destroy(ring->page_pool);
		ring->page_pool = NULL;
	}
}

/* wait for DMA to finish whatever it is doing before we start using it again */
int mtk_dma_busy_wait(struct mtk_eth *eth)
{
	unsigned int reg = eth->soc->reg_map->qdma.glo_cfg;
	int ret;
	u32 val;

	ret = readx_poll_timeout_atomic(__raw_readl, eth->base + reg, val,
					!(val &
					  (MTK_RX_DMA_BUSY | MTK_TX_DMA_BUSY)),
					5, MTK_DMA_BUSY_TIMEOUT_US);
	if (ret)
		dev_err(eth->dev, "DMA init timeout\n");

	return ret;
}

int mtk_hwlro_rx_init(struct mtk_eth *eth)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	int i;
	u32 val;
	u32 ring_ctrl_dw1 = 0, ring_ctrl_dw2 = 0, ring_ctrl_dw3 = 0;
	u32 lro_ctrl_dw0 = 0, lro_ctrl_dw3 = 0;

	/* set LRO rings to auto-learn modes */
	ring_ctrl_dw2 |= MTK_RING_AUTO_LERAN_MODE;

	/* validate LRO ring */
	ring_ctrl_dw2 |= MTK_RING_VLD;

	/* set AGE timer (unit: 20us) */
	ring_ctrl_dw2 |= MTK_RING_AGE_TIME_H;
	ring_ctrl_dw1 |= MTK_RING_AGE_TIME_L;

	/* set max AGG timer (unit: 20us) */
	ring_ctrl_dw2 |= MTK_RING_MAX_AGG_TIME;

	/* set max LRO AGG count */
	ring_ctrl_dw2 |= MTK_RING_MAX_AGG_CNT_L;
	ring_ctrl_dw3 |= MTK_RING_MAX_AGG_CNT_H;

	for (i = 1; i <= MTK_HW_LRO_RING_NUM; i++) {
		mtk_w32(eth, ring_ctrl_dw1, MTK_LRO_CTRL_DW1_CFG(i));
		mtk_w32(eth, ring_ctrl_dw2, MTK_LRO_CTRL_DW2_CFG(i));
		mtk_w32(eth, ring_ctrl_dw3, MTK_LRO_CTRL_DW3_CFG(i));
	}

	/* IPv4 checksum update enable */
	lro_ctrl_dw0 |= MTK_L3_CKS_UPD_EN;

	/* switch priority comparison to packet count mode */
	lro_ctrl_dw0 |= MTK_LRO_ALT_PKT_CNT_MODE;

	/* bandwidth threshold setting */
	mtk_w32(eth, MTK_HW_LRO_BW_THRE, MTK_ADMA_LRO_CTRL_DW2);

	/* auto-learn score delta setting */
	mtk_w32(eth, MTK_HW_LRO_REPLACE_DELTA,
		reg_map->adma.lro_alt_score_delta);

	/* set refresh timer for altering flows to 1 sec. (unit: 20us) */
	mtk_w32(eth, (MTK_HW_LRO_TIMER_UNIT << 16) | MTK_HW_LRO_REFRESH_TIME,
		MTK_ADMA_LRO_ALT_REFRESH_TIMER);

	/* the minimal remaining room of SDL0 in RXD for lro aggregation */
	lro_ctrl_dw3 |= MTK_LRO_MIN_RXD_SDL;

	val = mtk_r32(eth, reg_map->adma.rx_cfg);
	mtk_w32(eth, val | (MTK_ADMA_LRO_SDL << MTK_RX_CFG_SDL_OFFSET),
		reg_map->adma.rx_cfg);
	lro_ctrl_dw0 |= MTK_ADMA_LRO_SDL << MTK_CTRL_DW0_SDL_OFFSET;

	/* enable HW LRO */
	lro_ctrl_dw0 |= MTK_LRO_EN;

	/* enable cpu reason black list */
	lro_ctrl_dw0 |= MTK_LRO_CRSN_BNW;

	mtk_w32(eth, lro_ctrl_dw3, MTK_ADMA_LRO_CTRL_DW3);
	mtk_w32(eth, lro_ctrl_dw0, reg_map->adma.lro_ctrl_dw0);

	/* no use PPE cpu reason */
	mtk_w32(eth, 0xffffffff, MTK_ADMA_LRO_CTRL_DW1);

	/* Set perLRO GRP INT */
	mtk_m32(eth, MTK_RX_DONE_INT(MTK_HW_LRO_RING(1)),
		MTK_RX_DONE_INT(MTK_HW_LRO_RING(1)), reg_map->adma.int_grp);
	mtk_m32(eth, MTK_RX_DONE_INT(MTK_HW_LRO_RING(2)),
		MTK_RX_DONE_INT(MTK_HW_LRO_RING(2)), reg_map->adma.int_grp2);
	mtk_m32(eth, MTK_RX_DONE_INT(MTK_HW_LRO_RING(3)),
		MTK_RX_DONE_INT(MTK_HW_LRO_RING(3)), reg_map->adma.int_grp3);

	return 0;
}

void mtk_hwlro_rx_uninit(struct mtk_eth *eth)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	int i;
	u32 val;

	/* relinquish lro rings, flush aggregated packets */
	mtk_w32(eth, MTK_LRO_RING_RELINQUISH_REQ, reg_map->adma.lro_ctrl_dw0);

	/* wait for relinquishments done */
	for (i = 0; i < 10; i++) {
		val = mtk_r32(eth, reg_map->adma.lro_ctrl_dw0);
		if (val & MTK_LRO_RING_RELINQUISH_DONE) {
			msleep(20);
			continue;
		}
		break;
	}

	/* invalidate lro rings */
	for (i = 1; i <= MTK_HW_LRO_RING_NUM; i++)
		mtk_w32(eth, 0, MTK_LRO_CTRL_DW2_CFG(i));

	/* disable HW LRO */
	mtk_w32(eth, 0, reg_map->adma.lro_ctrl_dw0);
}

void mtk_set_queue_speed(struct mtk_eth *eth, unsigned int idx, int speed)
{
	const struct mtk_soc_data *soc = eth->soc;
	u32 ofs, val;

	val = MTK_QTX_SCH_MIN_RATE_EN |
	      /* minimum: 10 Mbps */
	      FIELD_PREP(MTK_QTX_SCH_MIN_RATE_MAN, 1) |
	      FIELD_PREP(MTK_QTX_SCH_MIN_RATE_EXP, 4) |
	      MTK_QTX_SCH_LEAKY_BUCKET_SIZE;

	switch (speed) {
	case SPEED_10:
		val |= MTK_QTX_SCH_MAX_RATE_EN |
		       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_MAN, 1) |
		       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_EXP, 4) |
		       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_WEIGHT, 1);
		break;
	case SPEED_100:
		val |= MTK_QTX_SCH_MAX_RATE_EN |
		       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_MAN, 1) |
		       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_EXP, 5) |
		       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_WEIGHT, 1);
		break;
	default:
		break;
	}

	ofs = MTK_QTX_OFFSET * idx;
	mtk_w32(eth, val, soc->reg_map->qdma.qtx_sch + ofs);
}

u32 mtk_rss_indr_table(struct mtk_rss_params *rss_params, int index)
{
	u32 val = 0;
	int i;

	for (i = 16 * index; i < 16 * index + 16; i++)
		val |= (rss_params->indirection_table[i] << (2 * (i % 16)));

	return val;
}

static int mtk_rss_init(struct mtk_eth *eth)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	struct mtk_rss_params *rss_params = &eth->rss_params;
	static u8 hash_key[MTK_RSS_HASH_KEYSIZE] = {
		0xfa, 0x01, 0xac, 0xbe, 0x3b, 0xb7, 0x42, 0x6a, 0x0c, 0xf2,
		0x30, 0x80, 0xa3, 0x2d, 0xcb, 0x77, 0xb4, 0x30, 0x7b, 0xae,
		0xcb, 0x2b, 0xca, 0xd0, 0xb0, 0x8f, 0xa3, 0x43, 0x3d, 0x25,
		0x67, 0x41, 0xc2, 0x0e, 0x5b, 0x25, 0xda, 0x56, 0x5a, 0x6d
	};
	u32 val;
	int i;

	memcpy(rss_params->hash_key, hash_key, MTK_RSS_HASH_KEYSIZE);
	for (i = 0; i < MTK_RSS_MAX_INDIRECTION_TABLE; i++)
		rss_params->indirection_table[i] = i % eth->soc->rss_num;

	/* Hash Type */
	val = mtk_r32(eth, reg_map->adma.rss_glo_cfg);
	val |= MTK_RSS_IPV4_STATIC_HASH;
	val |= MTK_RSS_IPV6_STATIC_HASH;
	mtk_w32(eth, val, reg_map->adma.rss_glo_cfg);

	/* Hash Key */
	for (i = 0; i < MTK_RSS_HASH_KEYSIZE / sizeof(u32); i++)
		mtk_w32(eth, rss_params->hash_key[i], MTK_RSS_HASH_KEY_DW(i));

	/* Select the size of indirection table */
	for (i = 0; i < MTK_RSS_MAX_INDIRECTION_TABLE / 16; i++)
		mtk_w32(eth, mtk_rss_indr_table(rss_params, i),
			MTK_RSS_INDR_TABLE_DW(i));

	/* Pause */
	val |= MTK_RSS_CFG_REQ;
	mtk_w32(eth, val, reg_map->adma.rss_glo_cfg);

	/* Enable RSS*/
	val |= MTK_RSS_EN;
	mtk_w32(eth, val, reg_map->adma.rss_glo_cfg);

	/* Release pause */
	val &= ~(MTK_RSS_CFG_REQ);
	mtk_w32(eth, val, reg_map->adma.rss_glo_cfg);

	/* Set perRSS GRP INT */
	mtk_m32(eth, MTK_RX_DONE_INT(MTK_RSS_RING(0)),
		MTK_RX_DONE_INT(MTK_RSS_RING(0)), reg_map->adma.int_grp);
	mtk_m32(eth, MTK_RX_DONE_INT(MTK_RSS_RING(1)),
		MTK_RX_DONE_INT(MTK_RSS_RING(1)), reg_map->adma.int_grp2);
	mtk_m32(eth, MTK_RX_DONE_INT(MTK_RSS_RING(2)),
		MTK_RX_DONE_INT(MTK_RSS_RING(2)), reg_map->adma.int_grp3);

	/* Set GRP INT */
	mtk_w32(eth, 0x210FFFF2, MTK_FE_INT_GRP);

	/* Enable RSS delay interrupt */
	mtk_w32(eth, MTK_MAX_DELAY_INT_V2, reg_map->adma.rss_delay_int);

	return 0;
}

static void mtk_rss_uninit(struct mtk_eth *eth)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	u32 val;

	/* Pause */
	val = mtk_r32(eth, reg_map->adma.rss_glo_cfg);
	val |= MTK_RSS_CFG_REQ;
	mtk_w32(eth, val, reg_map->adma.rss_glo_cfg);

	/* Disable RSS*/
	val &= ~(MTK_RSS_EN);
	mtk_w32(eth, val, reg_map->adma.rss_glo_cfg);

	/* Release pause */
	val &= ~(MTK_RSS_CFG_REQ);
	mtk_w32(eth, val, reg_map->adma.rss_glo_cfg);
}

int mtk_dma_init(struct mtk_eth *eth)
{
	int err;
	u32 i;

	if (mtk_dma_busy_wait(eth))
		return -EBUSY;

	/* QDMA needs scratch memory for internal reordering of the
	 * descriptors
	 */
	err = mtk_init_fq_dma(eth);
	if (err)
		return err;

	err = mtk_tx_alloc(eth);
	if (err)
		return err;

	err = mtk_rx_alloc(eth, 0, MTK_RX_FLAGS_QDMA);
	if (err)
		return err;

	err = mtk_rx_alloc(eth, 0, MTK_RX_FLAGS_NORMAL);
	if (err)
		return err;

	if (eth->hwlro) {
		for (i = 0; i < MTK_HW_LRO_RING_NUM; i++) {
			err = mtk_rx_alloc(eth, MTK_HW_LRO_RING(i),
					   MTK_RX_FLAGS_HWLRO);
			if (err)
				return err;
		}
		err = mtk_hwlro_rx_init(eth);
		if (err)
			return err;
	}

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSS)) {
		for (i = 0; i < MTK_RX_RSS_NUM; i++) {
			err = mtk_rx_alloc(eth, MTK_RSS_RING(i),
					   MTK_RX_FLAGS_NORMAL);
			if (err)
				return err;
		}
		err = mtk_rss_init(eth);
		if (err)
			return err;
	}

	/* Enable random early drop and set drop threshold automatically */
	mtk_w32(eth, FC_THRES_DROP_MODE | FC_THRES_DROP_EN | FC_THRES_MIN,
		eth->soc->reg_map->qdma.fc_th);
	mtk_w32(eth, 0x0, eth->soc->reg_map->qdma.hred);

	return 0;
}

void mtk_dma_free(struct mtk_eth *eth)
{
	const struct mtk_soc_data *soc = eth->soc;
	int i, j;

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i])
			continue;

		for (j = 0; j < MTK_QDMA_NUM_QUEUES; j++)
			netdev_tx_reset_queue(
				netdev_get_tx_queue(eth->netdev[i], j));
	}

	if (!MTK_HAS_CAPS(soc->caps, MTK_SRAM) && eth->scratch_ring) {
		dma_free_coherent(eth->dma_dev,
				  MTK_QDMA_RING_SIZE * soc->tx.desc_size,
				  eth->scratch_ring, eth->phy_scratch_ring);
		eth->scratch_ring = NULL;
		eth->phy_scratch_ring = 0;
	}
	mtk_tx_clean(eth);
	mtk_rx_clean(eth, &eth->rx_ring[0], MTK_HAS_CAPS(soc->caps, MTK_SRAM));
	mtk_rx_clean(eth, &eth->rx_ring_qdma, false);

	if (eth->hwlro) {
		mtk_hwlro_rx_uninit(eth);
		for (i = 0; i < MTK_HW_LRO_RING_NUM; i++)
			mtk_rx_clean(eth, &eth->rx_ring[MTK_HW_LRO_RING(i)], 0);
	}

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSS)) {
		mtk_rss_uninit(eth);
		for (i = 0; i < MTK_RX_RSS_NUM; i++)
			mtk_rx_clean(eth, &eth->rx_ring[MTK_RSS_RING(i)], 1);
	}

	for (i = 0; i < DIV_ROUND_UP(soc->tx.fq_dma_size, MTK_FQ_DMA_LENGTH);
	     i++) {
		kfree(eth->scratch_head[i]);
		eth->scratch_head[i] = NULL;
	}
}

int mtk_start_dma(struct mtk_eth *eth)
{
	u32 val;
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	int err;

	err = mtk_dma_init(eth);
	if (err) {
		mtk_dma_free(eth);
		return err;
	}

	val = mtk_r32(eth, reg_map->qdma.glo_cfg);
	val |= MTK_TX_DMA_EN | MTK_RX_DMA_EN | MTK_TX_BT_32DWORDS |
	       MTK_NDP_CO_PRO | MTK_RX_2B_OFFSET | MTK_TX_WB_DDONE;
	val &= ~MTK_RESV_BUF_MASK;
	val |= MTK_MUTLI_CNT | MTK_RESV_BUF | MTK_WCOMP_EN | MTK_DMAD_WR_WDONE |
	       MTK_CHK_DDONE_EN | MTK_LEAKY_BUCKET_EN;
	mtk_w32(eth, val, reg_map->qdma.glo_cfg);

	mtk_w32(eth, MTK_RX_DMA_EN | MTK_RX_BT_32DWORDS | MTK_MULTI_EN,
		reg_map->adma.glo_cfg);

	if (eth->hwlro) {
		val = mtk_r32(eth, eth->soc->reg_map->adma.glo_cfg);
		mtk_w32(eth, val | MTK_RX_DMA_LRO_EN,
			eth->soc->reg_map->adma.glo_cfg);
	}

	return 0;
}

void mtk_stop_dma(struct mtk_eth *eth, u32 glo_cfg)
{
	u32 val;
	int i;

	/* stop the dma engine */
	spin_lock_bh(&eth->page_lock);
	val = mtk_r32(eth, glo_cfg);
	mtk_w32(eth, val & ~(MTK_TX_WB_DDONE | MTK_RX_DMA_EN | MTK_TX_DMA_EN),
		glo_cfg);
	spin_unlock_bh(&eth->page_lock);

	/* wait for dma stop */
	for (i = 0; i < 10; i++) {
		val = mtk_r32(eth, glo_cfg);
		if (val & (MTK_TX_DMA_BUSY | MTK_RX_DMA_BUSY)) {
			msleep(20);
			continue;
		}
		break;
	}
}

void mtk_tx_unmap(struct mtk_eth *eth, struct mtk_tx_buf *tx_buf,
		  struct xdp_frame_bulk *bq, bool napi)
{
	if (tx_buf->flags & MTK_TX_FLAGS_SINGLE0) {
		dma_unmap_single(eth->dma_dev,
				 dma_unmap_addr(tx_buf, dma_addr0),
				 dma_unmap_len(tx_buf, dma_len0),
				 DMA_TO_DEVICE);
	} else if (tx_buf->flags & MTK_TX_FLAGS_PAGE0) {
		dma_unmap_page(eth->dma_dev, dma_unmap_addr(tx_buf, dma_addr0),
			       dma_unmap_len(tx_buf, dma_len0), DMA_TO_DEVICE);
	}

	if (tx_buf->data && tx_buf->data != (void *)MTK_DMA_DUMMY_DESC) {
		if (tx_buf->type == MTK_TYPE_SKB) {
			struct sk_buff *skb = tx_buf->data;

			if (napi)
				napi_consume_skb(skb, napi);
			else
				dev_kfree_skb_any(skb);
		} else {
			struct xdp_frame *xdpf = tx_buf->data;

			if (napi && tx_buf->type == MTK_TYPE_XDP_TX)
				xdp_return_frame_rx_napi(xdpf);
			else if (bq)
				xdp_return_frame_bulk(xdpf, bq);
			else
				xdp_return_frame(xdpf);
		}
	}
	tx_buf->flags = 0;
	tx_buf->data = NULL;
}

static void setup_tx_buf(struct mtk_eth *eth, struct mtk_tx_buf *tx_buf,
			 struct mtk_tx_dma *txd, dma_addr_t mapped_addr,
			 size_t size, int idx)
{
	dma_unmap_addr_set(tx_buf, dma_addr0, mapped_addr);
	dma_unmap_len_set(tx_buf, dma_len0, size);
}

void mtk_tx_set_dma_desc(struct net_device *dev, void *txd,
			 struct mtk_tx_dma_desc_info *info)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	struct mtk_tx_dma_v2 *desc = txd;
	u32 data;

	WRITE_ONCE(desc->txd1, info->addr);

	data = TX_DMA_PLEN0(info->size);
	if (info->last)
		data |= TX_DMA_LS0;

	/* Всегда добавляем старшие биты 36-битного адреса для MT7988.
     * Если адрес < 4GB, макрос вернет 0 и ничего не испортит.
     */
	data |= TX_DMA_PREP_ADDR64(info->addr);

	WRITE_ONCE(desc->txd3, data);

	/* set forward port */
	switch (mac->id) {
	case MTK_GMAC1_ID:
		data = PSE_GDM1_PORT << TX_DMA_FPORT_SHIFT_V2;
		break;
	case MTK_GMAC2_ID:
		data = PSE_GDM2_PORT << TX_DMA_FPORT_SHIFT_V2;
		break;
	case MTK_GMAC3_ID:
		data = PSE_GDM3_PORT << TX_DMA_FPORT_SHIFT_V2;
		break;
	}

	data |= TX_DMA_SWC_V2 | QID_BITS_V2(info->qid);
	WRITE_ONCE(desc->txd4, data);

	data = 0;
	if (info->first) {
		if (info->gso)
			data |= TX_DMA_TSO_V2;
		/* tx checksum offload */
		if (info->csum)
			data |= TX_DMA_CHKSUM_V2;
		if (netdev_uses_dsa(dev))
			data |= TX_DMA_SPTAG_V3;
	}
	WRITE_ONCE(desc->txd5, data);

	data = 0;
	if (info->first && info->vlan)
		data |= TX_DMA_INS_VLAN_V2 | info->vlan_tci;
	WRITE_ONCE(desc->txd6, data);

	WRITE_ONCE(desc->txd7, 0);
	WRITE_ONCE(desc->txd8, 0);
}

int mtk_tx_map(struct sk_buff *skb, struct net_device *dev, int tx_num,
	       struct mtk_tx_ring *ring, bool gso)
{
	struct mtk_tx_dma_desc_info txd_info = {
		.size = skb_headlen(skb),
		.gso = gso,
		.csum = skb->ip_summed == CHECKSUM_PARTIAL,
		.vlan = skb_vlan_tag_present(skb),
		.qid = skb_get_queue_mapping(skb),
		.vlan_tci = skb_vlan_tag_get(skb),
		.first = true,
		.last = !skb_is_nonlinear(skb),
	};
	struct netdev_queue *txq;
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	const struct mtk_soc_data *soc = eth->soc;
	struct mtk_tx_dma *itxd, *txd;
	struct mtk_tx_buf *itx_buf, *tx_buf;
	int i, n_desc = 1;
	int queue = skb_get_queue_mapping(skb);
	int k = 0;

	txq = netdev_get_tx_queue(dev, queue);
	itxd = ring->next_free;
	if (itxd == ring->last_free)
		return -ENOMEM;

	itx_buf = mtk_desc_to_tx_buf(ring, itxd, soc->tx.desc_size);
	memset(itx_buf, 0, sizeof(*itx_buf));

	txd_info.addr = dma_map_single(eth->dma_dev, skb->data, txd_info.size,
				       DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(eth->dma_dev, txd_info.addr)))
		return -ENOMEM;

	mtk_tx_set_dma_desc(dev, itxd, &txd_info);

	itx_buf->flags |= MTK_TX_FLAGS_SINGLE0;
	itx_buf->mac_id = mac->id;
	setup_tx_buf(eth, itx_buf, NULL, txd_info.addr, txd_info.size, k++);

	/* TX SG offload */
	txd = itxd;

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		unsigned int offset = 0;
		int frag_size = skb_frag_size(frag);

		while (frag_size) {
			txd = mtk_qdma_phys_to_virt(ring, txd->txd2);
			if (txd == ring->last_free)
				goto err_dma;

			n_desc++;

			memset(&txd_info, 0,
			       sizeof(struct mtk_tx_dma_desc_info));
			txd_info.size = min_t(unsigned int, frag_size,
					      soc->tx.dma_max_len);
			txd_info.qid = queue;
			txd_info.last = i == skb_shinfo(skb)->nr_frags - 1 &&
					!(frag_size - txd_info.size);
			txd_info.addr = skb_frag_dma_map(eth->dma_dev, frag,
							 offset, txd_info.size,
							 DMA_TO_DEVICE);
			if (unlikely(dma_mapping_error(eth->dma_dev,
						       txd_info.addr)))
				goto err_dma;

			mtk_tx_set_dma_desc(dev, txd, &txd_info);

			tx_buf = mtk_desc_to_tx_buf(ring, txd,
						    soc->tx.desc_size);
			memset(tx_buf, 0, sizeof(*tx_buf));
			tx_buf->data = (void *)MTK_DMA_DUMMY_DESC;
			tx_buf->flags |= MTK_TX_FLAGS_PAGE0;
			tx_buf->mac_id = mac->id;

			setup_tx_buf(eth, tx_buf, NULL, txd_info.addr,
				     txd_info.size, k++);

			frag_size -= txd_info.size;
			offset += txd_info.size;
		}
	}

	/* store skb to cleanup */
	itx_buf->type = MTK_TYPE_SKB;
	itx_buf->data = skb;

	netdev_tx_sent_queue(txq, skb->len);
	skb_tx_timestamp(skb);

	ring->next_free = mtk_qdma_phys_to_virt(ring, txd->txd2);
	atomic_sub(n_desc, &ring->free_count);

	/* make sure that all changes to the dma ring are flushed before we
	 * continue
	 */
	wmb();

	if (netif_xmit_stopped(txq) || !netdev_xmit_more())
		mtk_w32(eth, txd->txd2, soc->reg_map->qdma.ctx_ptr);

	return 0;

err_dma:
	do {
		tx_buf = mtk_desc_to_tx_buf(ring, itxd, soc->tx.desc_size);

		/* unmap dma */
		mtk_tx_unmap(eth, tx_buf, NULL, false);

		itxd->txd3 = TX_DMA_LS0 | TX_DMA_OWNER_CPU;

		itxd = mtk_qdma_phys_to_virt(ring, itxd->txd2);
	} while (itxd != txd);

	return -ENOMEM;
}

int mtk_cal_txd_req(struct mtk_eth *eth, struct sk_buff *skb)
{
	int i, nfrags = 1;
	skb_frag_t *frag;

	if (skb_is_gso(skb)) {
		for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
			frag = &skb_shinfo(skb)->frags[i];
			nfrags += DIV_ROUND_UP(skb_frag_size(frag),
					       eth->soc->tx.dma_max_len);
		}
	} else {
		nfrags += skb_shinfo(skb)->nr_frags;
	}

	return nfrags;
}

struct page_pool *mtk_create_page_pool(struct mtk_eth *eth,
				       struct xdp_rxq_info *xdp_q, int id,
				       int size)
{
	struct page_pool_params pp_params = {
		.order = 0,
		.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.pool_size = size,
		.nid = NUMA_NO_NODE,
		.dev = eth->dma_dev,
		.offset = MTK_PP_HEADROOM,
		.max_len = MTK_PP_MAX_BUF_SIZE,
	};
	struct page_pool *pp;
	int err;

	pp_params.dma_dir = rcu_access_pointer(eth->prog) ? DMA_BIDIRECTIONAL :
							    DMA_FROM_DEVICE;
	pp = page_pool_create(&pp_params);
	if (IS_ERR(pp))
		return pp;

	err = __xdp_rxq_info_reg(xdp_q, eth->dummy_dev, id,
				 eth->rx_napi[id].napi.napi_id, PAGE_SIZE);
	if (err < 0)
		goto err_free_pp;

	err = xdp_rxq_info_reg_mem_model(xdp_q, MEM_TYPE_PAGE_POOL, pp);
	if (err)
		goto err_unregister_rxq;

	return pp;

err_unregister_rxq:
	xdp_rxq_info_unreg(xdp_q);
err_free_pp:
	page_pool_destroy(pp);

	return ERR_PTR(err);
}

void *mtk_page_pool_get_buff(struct page_pool *pp, dma_addr_t *dma_addr,
			     gfp_t gfp_mask)
{
	struct page *page;

	page = page_pool_alloc_pages(pp, gfp_mask | __GFP_NOWARN);
	if (!page)
		return NULL;

	*dma_addr = page_pool_get_dma_addr(page) + MTK_PP_HEADROOM;
	return page_address(page);
}

void mtk_rx_put_buff(struct mtk_rx_ring *ring, void *data, bool napi)
{
	if (ring->page_pool)
		page_pool_put_full_page(ring->page_pool,
					virt_to_head_page(data), napi);
	else
		skb_free_frag(data);
}

int mtk_xdp_frame_map(struct mtk_eth *eth, struct net_device *dev,
		      struct mtk_tx_dma_desc_info *txd_info,
		      struct mtk_tx_dma *txd, struct mtk_tx_buf *tx_buf,
		      void *data, u16 headroom, int index, bool dma_map)
{
	struct mtk_mac *mac = netdev_priv(dev);

	if (dma_map) { /* ndo_xdp_xmit */
		txd_info->addr = dma_map_single(eth->dma_dev, data,
						txd_info->size, DMA_TO_DEVICE);
		if (unlikely(dma_mapping_error(eth->dma_dev, txd_info->addr)))
			return -ENOMEM;

		tx_buf->flags |= MTK_TX_FLAGS_SINGLE0;
	} else {
		struct page *page = virt_to_head_page(data);

		txd_info->addr = page_pool_get_dma_addr(page) +
				 sizeof(struct xdp_frame) + headroom;
		dma_sync_single_for_device(eth->dma_dev, txd_info->addr,
					   txd_info->size, DMA_BIDIRECTIONAL);
	}
	mtk_tx_set_dma_desc(dev, txd, txd_info);

	tx_buf->mac_id = mac->id;
	tx_buf->type = dma_map ? MTK_TYPE_XDP_NDO : MTK_TYPE_XDP_TX;
	tx_buf->data = (void *)MTK_DMA_DUMMY_DESC;

	setup_tx_buf(eth, tx_buf, NULL, txd_info->addr, txd_info->size, index);

	return 0;
}

int mtk_xdp_submit_frame(struct mtk_eth *eth, struct xdp_frame *xdpf,
			 struct net_device *dev, bool dma_map)
{
	struct skb_shared_info *sinfo = xdp_get_shared_info_from_frame(xdpf);
	const struct mtk_soc_data *soc = eth->soc;
	struct mtk_tx_ring *ring = &eth->tx_ring;
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_tx_dma_desc_info txd_info = {
		.size = xdpf->len,
		.first = true,
		.last = !xdp_frame_has_frags(xdpf),
		.qid = mac->id,
	};
	int err, index = 0, n_desc = 1, nr_frags;
	struct mtk_tx_buf *htx_buf, *tx_buf;
	struct mtk_tx_dma *htxd, *txd;
	void *data = xdpf->data;

	if (unlikely(test_bit(MTK_RESETTING, &eth->state)))
		return -EBUSY;

	nr_frags = unlikely(xdp_frame_has_frags(xdpf)) ? sinfo->nr_frags : 0;
	if (unlikely(atomic_read(&ring->free_count) <= 1 + nr_frags))
		return -EBUSY;

	spin_lock(&eth->page_lock);

	txd = ring->next_free;
	if (txd == ring->last_free) {
		spin_unlock(&eth->page_lock);
		return -ENOMEM;
	}
	htxd = txd;

	tx_buf = mtk_desc_to_tx_buf(ring, txd, soc->tx.desc_size);
	memset(tx_buf, 0, sizeof(*tx_buf));
	htx_buf = tx_buf;

	for (;;) {
		err = mtk_xdp_frame_map(eth, dev, &txd_info, txd, tx_buf, data,
					xdpf->headroom, index, dma_map);
		if (err < 0)
			goto unmap;

		if (txd_info.last)
			break;

		txd = mtk_qdma_phys_to_virt(ring, txd->txd2);
		if (txd == ring->last_free)
			goto unmap;

		tx_buf = mtk_desc_to_tx_buf(ring, txd, soc->tx.desc_size);
		memset(tx_buf, 0, sizeof(*tx_buf));
		n_desc++;

		memset(&txd_info, 0, sizeof(struct mtk_tx_dma_desc_info));
		txd_info.size = skb_frag_size(&sinfo->frags[index]);
		txd_info.last = index + 1 == nr_frags;
		txd_info.qid = mac->id;
		data = skb_frag_address(&sinfo->frags[index]);

		index++;
	}
	/* store xdpf for cleanup */
	htx_buf->data = xdpf;

	ring->next_free = mtk_qdma_phys_to_virt(ring, txd->txd2);
	atomic_sub(n_desc, &ring->free_count);

	/* make sure that all changes to the dma ring are flushed before we
	 * continue
	 */
	wmb();

	mtk_w32(eth, txd->txd2, soc->reg_map->qdma.ctx_ptr);

	spin_unlock(&eth->page_lock);

	return 0;

unmap:
	while (htxd != txd) {
		tx_buf = mtk_desc_to_tx_buf(ring, htxd, soc->tx.desc_size);
		mtk_tx_unmap(eth, tx_buf, NULL, false);

		htxd->txd3 = TX_DMA_LS0 | TX_DMA_OWNER_CPU;

		htxd = mtk_qdma_phys_to_virt(ring, htxd->txd2);
	}

	spin_unlock(&eth->page_lock);

	return err;
}

static int mtk_queue_stopped(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i])
			continue;
		if (netif_queue_stopped(eth->netdev[i]))
			return 1;
	}

	return 0;
}

static void mtk_wake_queue(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i])
			continue;
		netif_tx_wake_all_queues(eth->netdev[i]);
	}
}

static u32 mtk_xdp_run(struct mtk_eth *eth, struct mtk_rx_ring *ring,
		       struct xdp_buff *xdp, struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_hw_stats *hw_stats = mac->hw_stats;
	u64 *count = &hw_stats->xdp_stats.rx_xdp_drop;
	struct bpf_prog *prog;
	u32 act = XDP_PASS;

	rcu_read_lock();

	prog = rcu_dereference(eth->prog);
	if (!prog)
		goto out;

	act = bpf_prog_run_xdp(prog, xdp);
	switch (act) {
	case XDP_PASS:
		count = &hw_stats->xdp_stats.rx_xdp_pass;
		goto update_stats;
	case XDP_REDIRECT:
		if (unlikely(xdp_do_redirect(dev, xdp, prog))) {
			act = XDP_DROP;
			break;
		}

		count = &hw_stats->xdp_stats.rx_xdp_redirect;
		goto update_stats;
	case XDP_TX: {
		struct xdp_frame *xdpf = xdp_convert_buff_to_frame(xdp);

		if (!xdpf || mtk_xdp_submit_frame(eth, xdpf, dev, false)) {
			count = &hw_stats->xdp_stats.rx_xdp_tx_errors;
			act = XDP_DROP;
			break;
		}

		count = &hw_stats->xdp_stats.rx_xdp_tx;
		goto update_stats;
	}
	default:
		bpf_warn_invalid_xdp_action(dev, prog, act);
		fallthrough;
	case XDP_ABORTED:
		trace_xdp_exception(dev, prog, act);
		fallthrough;
	case XDP_DROP:
		break;
	}

	page_pool_put_full_page(ring->page_pool, virt_to_head_page(xdp->data),
				true);

update_stats:
	u64_stats_update_begin(&hw_stats->syncp);
	*count = *count + 1;
	u64_stats_update_end(&hw_stats->syncp);
out:
	rcu_read_unlock();

	return act;
}

int mtk_poll_rx(struct napi_struct *napi, int budget, struct mtk_eth *eth)
{
	struct dim_sample dim_sample = {};
	struct mtk_napi *rx_napi = container_of(napi, struct mtk_napi, napi);
	struct mtk_rx_ring *ring = rx_napi->rx_ring;
	bool xdp_flush = false;
	int idx;
	struct sk_buff *skb;
	u8 *data, *new_data;
	struct mtk_rx_dma_v2 *rxd, trxd;
	int done = 0, bytes = 0;
	dma_addr_t dma_addr = DMA_MAPPING_ERROR;
	int ppe_idx = 0;

	if (unlikely(!ring))
		goto rx_done;

	while (done < budget) {
		unsigned int pktlen, *rxdcsum;
		struct net_device *netdev;
		u32 hash, reason;
		int mac = 0;

		idx = NEXT_DESP_IDX(ring->calc_idx, ring->dma_size);
		rxd = ring->dma + idx * eth->soc->rx.desc_size;
		data = ring->data[idx];

		if (!mtk_rx_get_desc(eth, &trxd, rxd))
			break;

		/* find out which mac the packet come from. values start at 1 */
		{
			u32 val = RX_DMA_GET_SPORT_V2(trxd.rxd5);

			switch (val) {
			case PSE_GDM1_PORT:
			case PSE_GDM2_PORT:
				mac = val - 1;
				break;
			case PSE_GDM3_PORT:
				mac = MTK_GMAC3_ID;
				break;
			default:
				break;
			}
		}

		if (unlikely(mac < 0 || mac >= MTK_MAX_DEVS ||
			     !eth->netdev[mac]))
			goto release_desc;

		netdev = eth->netdev[mac];
		ppe_idx = eth->mac[mac]->ppe_idx;

		if (unlikely(test_bit(MTK_RESETTING, &eth->state)))
			goto release_desc;

		pktlen = RX_DMA_GET_PLEN0(trxd.rxd2);

		/* alloc new buffer */
		{
			struct page *page = virt_to_head_page(data);
			struct xdp_buff xdp;
			u32 ret;

			new_data = mtk_page_pool_get_buff(
				ring->page_pool, &dma_addr, GFP_ATOMIC);
			if (unlikely(!new_data)) {
				netdev->stats.rx_dropped++;
				goto release_desc;
			}

			dma_sync_single_for_cpu(
				eth->dma_dev,
				page_pool_get_dma_addr(page) + MTK_PP_HEADROOM,
				pktlen, page_pool_get_dma_dir(ring->page_pool));

			xdp_init_buff(&xdp, PAGE_SIZE, &ring->xdp_q);
			xdp_prepare_buff(&xdp, data, MTK_PP_HEADROOM, pktlen,
					 false);
			xdp_buff_clear_frags_flag(&xdp);

			ret = mtk_xdp_run(eth, ring, &xdp, netdev);
			if (ret == XDP_REDIRECT)
				xdp_flush = true;

			if (ret != XDP_PASS)
				goto skip_rx;

			skb = build_skb(data, PAGE_SIZE);
			if (unlikely(!skb)) {
				page_pool_put_full_page(ring->page_pool, page,
							true);
				netdev->stats.rx_dropped++;
				goto skip_rx;
			}

			skb_reserve(skb, xdp.data - xdp.data_hard_start);
			skb_put(skb, xdp.data_end - xdp.data);
			skb_mark_for_recycle(skb);
		}

		skb->dev = netdev;
		bytes += skb->len;

		reason = FIELD_GET(MTK_RXD5_PPE_CPU_REASON, trxd.rxd5);
		hash = trxd.rxd5 & MTK_RXD5_FOE_ENTRY;
		if (hash != MTK_RXD5_FOE_ENTRY)
			skb_set_hash(skb, jhash_1word(hash, 0),
				     PKT_HASH_TYPE_L4);
		rxdcsum = &trxd.rxd3;

		if (*rxdcsum & eth->soc->rx.dma_l4_valid)
			skb->ip_summed = CHECKSUM_UNNECESSARY;
		else
			skb_checksum_none_assert(skb);
		skb->protocol = eth_type_trans(skb, netdev);

		if (reason == MTK_PPE_CPU_REASON_HIT_UNBIND_RATE_REACHED)
			mtk_ppe_check_skb(eth->ppe[ppe_idx], skb, hash);

		skb_record_rx_queue(skb, 0);
		napi_gro_receive(napi, skb);

skip_rx:
		ring->data[idx] = new_data;
		WRITE_ONCE(rxd->rxd1, (unsigned int)dma_addr);
release_desc:
		WRITE_ONCE(rxd->rxd2, RX_DMA_PREP_PLEN0(ring->buf_size) |
					      RX_DMA_PREP_ADDR64(dma_addr));

		ring->calc_idx = idx;
		done++;
	}

rx_done:
	if (done) {
		/* make sure that all changes to the dma ring are flushed before
		 * we continue
		 */
		wmb();
		mtk_update_rx_cpu_idx(eth, ring);
	}

	eth->rx_packets += done;
	eth->rx_bytes += bytes;
	dim_update_sample(eth->rx_events, eth->rx_packets, eth->rx_bytes,
			  &dim_sample);
	net_dim(&eth->rx_dim, dim_sample);

	if (xdp_flush)
		xdp_do_flush();

	return done;
}

static void mtk_poll_tx_done(struct mtk_eth *eth, struct mtk_poll_state *state,
			     u8 mac, struct sk_buff *skb)
{
	struct netdev_queue *txq;
	struct net_device *dev;
	unsigned int bytes = skb->len;

	state->total++;
	eth->tx_packets++;
	eth->tx_bytes += bytes;

	dev = eth->netdev[mac];
	if (!dev)
		return;

	txq = netdev_get_tx_queue(dev, skb_get_queue_mapping(skb));
	if (state->txq == txq) {
		state->done++;
		state->bytes += bytes;
		return;
	}

	if (state->txq)
		netdev_tx_completed_queue(state->txq, state->done,
					  state->bytes);

	state->txq = txq;
	state->done = 1;
	state->bytes = bytes;
}

static int mtk_poll_tx_qdma(struct mtk_eth *eth, int budget,
			    struct mtk_poll_state *state)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	struct mtk_tx_ring *ring = &eth->tx_ring;
	struct mtk_tx_buf *tx_buf;
	struct xdp_frame_bulk bq;
	struct mtk_tx_dma *desc;
	u32 cpu, dma;

	cpu = ring->last_free_ptr;
	dma = mtk_r32(eth, reg_map->qdma.drx_ptr);

	desc = mtk_qdma_phys_to_virt(ring, cpu);
	xdp_frame_bulk_init(&bq);

	while ((cpu != dma) && budget) {
		u32 next_cpu = desc->txd2;

		desc = mtk_qdma_phys_to_virt(ring, desc->txd2);
		if ((desc->txd3 & TX_DMA_OWNER_CPU) == 0)
			break;

		tx_buf = mtk_desc_to_tx_buf(ring, desc, eth->soc->tx.desc_size);
		if (!tx_buf->data)
			break;

		if (tx_buf->data != (void *)MTK_DMA_DUMMY_DESC) {
			if (tx_buf->type == MTK_TYPE_SKB)
				mtk_poll_tx_done(eth, state, tx_buf->mac_id,
						 tx_buf->data);

			budget--;
		}
		mtk_tx_unmap(eth, tx_buf, &bq, true);

		ring->last_free = desc;
		atomic_inc(&ring->free_count);

		cpu = next_cpu;
	}
	xdp_flush_frame_bulk(&bq);

	ring->last_free_ptr = cpu;
	mtk_w32(eth, cpu, reg_map->qdma.crx_ptr);

	return budget;
}

int mtk_poll_tx(struct mtk_eth *eth, int budget)
{
	struct mtk_tx_ring *ring = &eth->tx_ring;
	struct dim_sample dim_sample = {};
	struct mtk_poll_state state = {};

	budget = mtk_poll_tx_qdma(eth, budget, &state);

	if (state.txq)
		netdev_tx_completed_queue(state.txq, state.done, state.bytes);

	dim_update_sample(eth->tx_events, eth->tx_packets, eth->tx_bytes,
			  &dim_sample);
	net_dim(&eth->tx_dim, dim_sample);

	if (mtk_queue_stopped(eth) &&
	    (atomic_read(&ring->free_count) > ring->thresh))
		mtk_wake_queue(eth);

	return state.total;
}

int mtk_napi_tx(struct napi_struct *napi, int budget)
{
	struct mtk_eth *eth = container_of(napi, struct mtk_eth, tx_napi);
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	int tx_done = 0;

	mtk_handle_status_irq(eth);
	mtk_w32(eth, MTK_TX_DONE_INT, reg_map->tx_irq_status);
	tx_done = mtk_poll_tx(eth, budget);

	if (unlikely(netif_msg_intr(eth))) {
		dev_info(eth->dev, "done tx %d, intr 0x%08x/0x%x\n", tx_done,
			 mtk_r32(eth, reg_map->tx_irq_status),
			 mtk_r32(eth, reg_map->tx_irq_mask));
	}

	if (tx_done == budget)
		return budget;

	if (mtk_r32(eth, reg_map->tx_irq_status) & MTK_TX_DONE_INT)
		return budget;

	if (napi_complete_done(napi, tx_done))
		mtk_tx_irq_enable(eth, MTK_TX_DONE_INT);

	return tx_done;
}

int mtk_napi_rx(struct napi_struct *napi, int budget)
{
	struct mtk_napi *rx_napi = container_of(napi, struct mtk_napi, napi);
	struct mtk_eth *eth = rx_napi->eth;
	struct mtk_rx_ring *ring = rx_napi->rx_ring;
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	int rx_done_total = 0;

	mtk_handle_status_irq(eth);

	do {
		int rx_done;

		mtk_w32(eth, MTK_RX_DONE_INT(ring->ring_no),
			reg_map->adma.irq_status);
		rx_done = mtk_poll_rx(napi, budget - rx_done_total, eth);
		rx_done_total += rx_done;

		if (unlikely(netif_msg_intr(eth))) {
			dev_info(eth->dev, "done rx %d, intr 0x%08x/0x%x\n",
				 rx_done,
				 mtk_r32(eth, reg_map->adma.irq_status),
				 mtk_r32(eth, reg_map->adma.irq_mask));
		}

		if (rx_done_total == budget)
			return budget;

	} while (mtk_r32(eth, reg_map->adma.irq_status) &
		 MTK_RX_DONE_INT(ring->ring_no));

	if (napi_complete_done(napi, rx_done_total))
		mtk_rx_irq_enable(eth, MTK_RX_DONE_INT(ring->ring_no));

	return rx_done_total;
}

int mtk_napi_init(struct mtk_eth *eth)
{
	struct mtk_napi *rx_napi;
	int i;

	/* Настраиваем Base Ring 0 -> NAPI 0 */
	rx_napi = &eth->rx_napi[0];
	rx_napi->eth = eth;
	rx_napi->rx_ring = &eth->rx_ring[0];

	/* Настраиваем RSS Rings 1, 2, 3 -> NAPI 1, 2, 3 */
	if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSS)) {
		for (i = 0; i < MTK_RX_RSS_NUM; i++) {
			/* MTK_RSS_RING(i) вернет 1, 2, 3 */
			int ring_no = MTK_RSS_RING(i);

			rx_napi = &eth->rx_napi[ring_no];
			rx_napi->eth = eth;
			rx_napi->rx_ring = &eth->rx_ring[ring_no];
		}
	}

	/* ВНИМАНИЕ: Кольца HW LRO (4,5,6,7) мы пока не трогаем!
	 * В нашей схеме 4 IRQ они не используются и NAPI для них не создается.
	 */

	return 0;
}

inline void mtk_tx_irq_disable(struct mtk_eth *eth, u32 mask)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&eth->tx_irq_lock, flags);
	val = mtk_r32(eth, eth->soc->reg_map->tx_irq_mask);
	mtk_w32(eth, val & ~mask, eth->soc->reg_map->tx_irq_mask);
	spin_unlock_irqrestore(&eth->tx_irq_lock, flags);
}

inline void mtk_tx_irq_enable(struct mtk_eth *eth, u32 mask)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&eth->tx_irq_lock, flags);
	val = mtk_r32(eth, eth->soc->reg_map->tx_irq_mask);
	mtk_w32(eth, val | mask, eth->soc->reg_map->tx_irq_mask);
	spin_unlock_irqrestore(&eth->tx_irq_lock, flags);
}

inline void mtk_rx_irq_disable(struct mtk_eth *eth, u32 mask)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&eth->rx_irq_lock, flags);
	val = mtk_r32(eth, eth->soc->reg_map->adma.irq_mask);
	mtk_w32(eth, val & ~mask, eth->soc->reg_map->adma.irq_mask);
	spin_unlock_irqrestore(&eth->rx_irq_lock, flags);
}

inline void mtk_rx_irq_enable(struct mtk_eth *eth, u32 mask)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&eth->rx_irq_lock, flags);
	val = mtk_r32(eth, eth->soc->reg_map->adma.irq_mask);
	mtk_w32(eth, val | mask, eth->soc->reg_map->adma.irq_mask);
	spin_unlock_irqrestore(&eth->rx_irq_lock, flags);
}

void mtk_dim_rx(struct work_struct *work)
{
	struct dim *dim = container_of(work, struct dim, work);
	struct mtk_eth *eth = container_of(dim, struct mtk_eth, rx_dim);
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	struct dim_cq_moder cur_profile;
	u32 val, cur;

	cur_profile =
		net_dim_get_rx_moderation(eth->rx_dim.mode, dim->profile_ix);
	spin_lock_bh(&eth->dim_lock);

	val = mtk_r32(eth, reg_map->adma.delay_irq);
	val &= MTK_ADMA_DELAY_TX_MASK;
	val |= MTK_ADMA_DELAY_RX_EN;

	cur = min_t(u32, DIV_ROUND_UP(cur_profile.usec, 20),
		    MTK_ADMA_DELAY_PTIME_MASK);
	val |= cur << MTK_ADMA_DELAY_RX_PTIME_SHIFT;

	cur = min_t(u32, cur_profile.pkts, MTK_ADMA_DELAY_PINT_MASK);
	val |= cur << MTK_ADMA_DELAY_RX_PINT_SHIFT;

	mtk_w32(eth, val, reg_map->adma.delay_irq);
	mtk_w32(eth, val, reg_map->qdma.delay_irq);

	spin_unlock_bh(&eth->dim_lock);

	dim->state = DIM_START_MEASURE;
}

void mtk_dim_tx(struct work_struct *work)
{
	struct dim *dim = container_of(work, struct dim, work);
	struct mtk_eth *eth = container_of(dim, struct mtk_eth, tx_dim);
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	struct dim_cq_moder cur_profile;
	u32 val, cur;

	cur_profile =
		net_dim_get_tx_moderation(eth->tx_dim.mode, dim->profile_ix);
	spin_lock_bh(&eth->dim_lock);

	val = mtk_r32(eth, reg_map->adma.delay_irq);
	val &= MTK_ADMA_DELAY_RX_MASK;
	val |= MTK_ADMA_DELAY_TX_EN;

	cur = min_t(u32, DIV_ROUND_UP(cur_profile.usec, 20),
		    MTK_ADMA_DELAY_PTIME_MASK);
	val |= cur << MTK_ADMA_DELAY_TX_PTIME_SHIFT;

	cur = min_t(u32, cur_profile.pkts, MTK_ADMA_DELAY_PINT_MASK);
	val |= cur << MTK_ADMA_DELAY_TX_PINT_SHIFT;

	mtk_w32(eth, val, reg_map->adma.delay_irq);
	mtk_w32(eth, val, reg_map->qdma.delay_irq);

	spin_unlock_bh(&eth->dim_lock);

	dim->state = DIM_START_MEASURE;
}

int mtk_hwlro_add_ipaddr(struct net_device *dev, struct ethtool_rxnfc *cmd)
{
	struct ethtool_rx_flow_spec *fsp =
		(struct ethtool_rx_flow_spec *)&cmd->fs;
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	int hwlro_idx;
	u32 ip4dst;

	if ((fsp->flow_type != TCP_V4_FLOW) ||
	    (!fsp->h_u.tcp_ip4_spec.ip4dst) || (fsp->location > 1))
		return -EINVAL;

	ip4dst = htonl(fsp->h_u.tcp_ip4_spec.ip4dst);
	mac->hwlro_ip[fsp->location] = ip4dst;
	hwlro_idx = (mac->id * MTK_MAX_LRO_IP_CNT) + fsp->location;

	mac->hwlro_ip_cnt = mtk_hwlro_get_ip_cnt(mac);

	mtk_hwlro_val_ipaddr(eth, hwlro_idx, mac->hwlro_ip[fsp->location]);

	return 0;
}

int mtk_hwlro_del_ipaddr(struct net_device *dev, struct ethtool_rxnfc *cmd)
{
	struct ethtool_rx_flow_spec *fsp =
		(struct ethtool_rx_flow_spec *)&cmd->fs;
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	int hwlro_idx;

	if (fsp->location > 1)
		return -EINVAL;

	mac->hwlro_ip[fsp->location] = 0;
	hwlro_idx = (mac->id * MTK_MAX_LRO_IP_CNT) + fsp->location;

	mac->hwlro_ip_cnt = mtk_hwlro_get_ip_cnt(mac);

	mtk_hwlro_inval_ipaddr(eth, hwlro_idx);

	return 0;
}

void mtk_hwlro_netdev_disable(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	int i, hwlro_idx;

	for (i = 0; i < MTK_MAX_LRO_IP_CNT; i++) {
		mac->hwlro_ip[i] = 0;
		hwlro_idx = (mac->id * MTK_MAX_LRO_IP_CNT) + i;

		mtk_hwlro_inval_ipaddr(eth, hwlro_idx);
	}

	mac->hwlro_ip_cnt = 0;
}

int mtk_hwlro_get_fdir_entry(struct net_device *dev, struct ethtool_rxnfc *cmd)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct ethtool_rx_flow_spec *fsp =
		(struct ethtool_rx_flow_spec *)&cmd->fs;

	/* only tcp dst ipv4 is meaningful, others are meaningless */
	fsp->flow_type = TCP_V4_FLOW;
	fsp->h_u.tcp_ip4_spec.ip4dst = ntohl(mac->hwlro_ip[fsp->location]);
	fsp->m_u.tcp_ip4_spec.ip4dst = 0;

	fsp->h_u.tcp_ip4_spec.ip4src = 0;
	fsp->m_u.tcp_ip4_spec.ip4src = 0xffffffff;
	fsp->h_u.tcp_ip4_spec.psrc = 0;
	fsp->m_u.tcp_ip4_spec.psrc = 0xffff;
	fsp->h_u.tcp_ip4_spec.pdst = 0;
	fsp->m_u.tcp_ip4_spec.pdst = 0xffff;
	fsp->h_u.tcp_ip4_spec.tos = 0;
	fsp->m_u.tcp_ip4_spec.tos = 0xff;

	return 0;
}

int mtk_hwlro_get_fdir_all(struct net_device *dev, struct ethtool_rxnfc *cmd,
			   u32 *rule_locs)
{
	int cnt = 0;

	cmd->rule_cnt = cnt;

	return 0;
}
