/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2022 MediaTek Inc.
 * Author: Henry Yen <henry.yen@mediatek.com>
 * Adapted for MT7988 standalone driver by
 * Semenets V. Pavel <p.semenets@gmail.com>
 */

#ifndef MT7988_RESET_H
#define MT7988_RESET_H

#include "mt7988_eth.h"

#define MTK_FE_START_RESET		0x2000
#define MTK_FE_RESET_DONE		0x2001
#define MTK_WIFI_RESET_DONE		0x2002
#define MTK_WIFI_CHIP_ONLINE		0x2003
#define MTK_WIFI_CHIP_OFFLINE		0x2004
#define MTK_FE_RESET_NAT_DONE		0x4001

#define MTK_FE_STOP_TRAFFIC		(0x2005)
#define MTK_FE_STOP_TRAFFIC_DONE	(0x2006)
#define MTK_FE_START_TRAFFIC		(0x2007)
#define MTK_FE_STOP_TRAFFIC_DONE_FAIL	(0x2008)

#define MTK_WDMA_CNT			3
#define MTK_GDM_RX_BASE		0x8
#define MTK_GDM_CNT_OFFSET		0x80
#define MTK_GDM_TX_BASE		0x48
#define MTK_QDMA_PAGE_NUM		8

enum mtk_reset_event_id {
	MTK_EVENT_FORCE		= 0,
	MTK_EVENT_WARM_CNT		= 1,
	MTK_EVENT_COLD_CNT		= 2,
	MTK_EVENT_TOTAL_CNT		= 3,
	MTK_EVENT_FQ_EMPTY		= 8,
	MTK_EVENT_TSO_FAIL		= 12,
	MTK_EVENT_TSO_ILLEGAL		= 13,
	MTK_EVENT_TSO_ALIGN		= 14,
	MTK_EVENT_RFIFO_OV		= 18,
	MTK_EVENT_RFIFO_UF		= 19,
};

void mtk_reset_event_update(struct mtk_eth *eth, u32 id);
u32 mtk_check_reset_event(struct mtk_eth *eth, u32 status);
void mtk_prepare_reset_fe(struct mtk_eth *eth);
void mtk_dma_monitor_work(struct work_struct *work);
void mtk_dump_netsys_info(struct mtk_eth *eth);
int mtk_reset_init(struct mtk_eth *eth);
void mtk_reset_deinit(struct mtk_eth *eth);

extern struct notifier_block mtk_eth_netdevice_nb;
extern const char * const mtk_reset_event_name[32];

#endif
