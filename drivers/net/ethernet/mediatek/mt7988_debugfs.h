/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (C) 2018 MediaTek Inc.
 * Copyright (C) 2009-2016 John Crispin <blogic@openwrt.org>
 * Copyright (C) 2009-2016 Felix Fietkau <nbd@openwrt.org>
 * Copyright (C) 2013-2016 Michael Lee <igvtee@gmail.com>
 *
 * Adapted for MT7988 standalone driver by
 * Semenets V. Pavel <p.semenets@gmail.com>
 */

#ifndef MT7988_DEBUGFS_H
#define MT7988_DEBUGFS_H

#include "mt7988_eth.h"

#define MTK_PSE_IQ_STA(x)	(0x180 + (x) * 0x4)
#define MTK_PSE_OQ_STA(x)	(0x1A0 + (x) * 0x4)

#define MTK_HW_LRO_AGG_FLUSH		1
#define MTK_HW_LRO_AGE_FLUSH		2
#define MTK_HW_LRO_NOT_IN_SEQ_FLUSH	3
#define MTK_HW_LRO_TIMESTAMP_FLUSH	4
#define MTK_HW_LRO_NON_RULE_FLUSH	5

#define MTK_LRO_RING_AGG_TIME_MASK	GENMASK(25, 10)
#define MTK_LRO_RING_AGG_CNT_L_MASK	GENMASK(31, 26)
#define MTK_LRO_RING_AGG_CNT_H_MASK	GENMASK(1, 0)
#define MTK_LRO_RING_AGE_TIME_L_MASK	GENMASK(31, 22)
#define MTK_LRO_RING_AGE_TIME_H_MASK	GENMASK(5, 0)

#define MTK_LRO_RING_AGE_TIME_L_OFFSET	22
#define MTK_LRO_RING_AGE_TIME_H_OFFSET	0
#define MTK_LRO_RING_AGG_TIME_OFFSET	10
#define MTK_LRO_RING_AGG_CNT_L_OFFSET	26
#define MTK_LRO_RING_AGG_CNT_H_OFFSET	0

#define MTK_RX_PORT_VALID_OFFSET	8

#define MTK_GDM1_TX_GBCNT	0x1C00

#define RX_DMA_GET_AGG_CNT_V2(_x)	(((_x) >> 16) & 0xff)
#define RX_DMA_GET_FLUSH_RSN_V2(_x)	((_x) & 0x7)

extern u32 dbg_show_level;

int mt7988_eth_debugfs_init(struct mtk_eth *eth);
void mt7988_eth_debugfs_exit(struct mtk_eth *eth);
void mt7988_hw_lro_stats_update(struct mtk_eth *eth, u32 ring_no,
				struct mtk_rx_dma_v2 *rxd);
void mt7988_hw_lro_flush_stats_update(struct mtk_eth *eth, u32 ring_no,
				      struct mtk_rx_dma_v2 *rxd);

#endif
