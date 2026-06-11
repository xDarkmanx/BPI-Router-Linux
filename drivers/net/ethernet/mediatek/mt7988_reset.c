// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 MediaTek Inc.
 * Author: Henry Yen <henry.yen@mediatek.com>
 * Adapted for MT7988 standalone driver by
 * Semenets V. Pavel <p.semenets@gmail.com>
 */

#include <linux/regmap.h>
#include "mt7988_eth.h"
#include "mt7988_reset.h"

const char * const mtk_reset_event_name[32] = {
	[MTK_EVENT_FORCE]	= "Force",
	[MTK_EVENT_WARM_CNT]	= "Warm",
	[MTK_EVENT_COLD_CNT]	= "Cold",
	[MTK_EVENT_TOTAL_CNT]	= "Total",
	[MTK_EVENT_FQ_EMPTY]	= "FQ Empty",
	[MTK_EVENT_TSO_FAIL]	= "TSO Fail",
	[MTK_EVENT_TSO_ILLEGAL]	= "TSO Illegal",
	[MTK_EVENT_TSO_ALIGN]	= "TSO Align",
	[MTK_EVENT_RFIFO_OV]	= "RFIFO OV",
	[MTK_EVENT_RFIFO_UF]	= "RFIFO UF",
};

static int mtk_wifi_num;
static int mtk_rest_cnt;

void mtk_reset_event_update(struct mtk_eth *eth, u32 id)
{
	struct mtk_reset_event *reset_event = &eth->reset_event;
	reset_event->count[id]++;
}

static void mtk_dump_reg(void *_eth, char *name, u32 offset, u32 range)
{
	struct mtk_eth *eth = _eth;
	u32 cur = offset;

	pr_info("\n============ %s ============\n", name);
	while (cur < offset + range) {
		pr_info("0x%x: %08x %08x %08x %08x\n",
			cur, mtk_r32(eth, cur), mtk_r32(eth, cur + 0x4),
			mtk_r32(eth, cur + 0x8), mtk_r32(eth, cur + 0xc));
		cur += 0x10;
	}
}

void mtk_dump_netsys_info(struct mtk_eth *eth)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	u32 adma_base = reg_map->adma.glo_cfg - 0x204;
	u32 qdma_base = reg_map->qdma.glo_cfg - 0x204;
	u32 id;

	mtk_dump_reg(eth, "FE", 0x0, 0x500);
	mtk_dump_reg(eth, "ADMA", adma_base, 0x300);
	for (id = 0; id < MTK_QDMA_PAGE_NUM; id++) {
		mtk_w32(eth, id, qdma_base + 0x1f0);
		pr_info("\nQDMA PAGE:%x ", mtk_r32(eth, qdma_base + 0x1f0));
		mtk_dump_reg(eth, "QDMA", qdma_base, 0x100);
		mtk_w32(eth, 0, qdma_base + 0x1f0);
	}
	mtk_dump_reg(eth, "QDMA", reg_map->qdma.rx_ptr, 0x300);
	mtk_dump_reg(eth, "WDMA", reg_map->wdma_base[0], 0x600);
	mtk_dump_reg(eth, "PPE", reg_map->ppe_base + 0x200, 0x200);
	mtk_dump_reg(eth, "GMAC", 0x10000, 0x300);
	mtk_dump_reg(eth, "XGMAC0", 0x12000, 0x300);
	mtk_dump_reg(eth, "XGMAC1", 0x13000, 0x300);
}

u32 mtk_check_reset_event(struct mtk_eth *eth, u32 status)
{
	u32 ret = 0, val = 0;

	if ((status & MTK_FE_INT_FQ_EMPTY) ||
	    (status & MTK_FE_INT_RFIFO_UF) ||
	    (status & MTK_FE_INT_RFIFO_OV) ||
	    (status & MTK_FE_INT_TSO_FAIL) ||
	    (status & MTK_FE_INT_TSO_ALIGN) ||
	    (status & MTK_FE_INT_TSO_ILLEGAL)) {
		while (status) {
			val = ffs((unsigned int)status) - 1;
			mtk_reset_event_update(eth, val);
			status &= ~(1 << val);
		}
		ret = 1;
	}

	if (ret) {
		mtk_reset_event_update(eth, MTK_EVENT_TOTAL_CNT);
		mtk_dump_netsys_info(eth);
	}

	return ret;
}

static u32 mtk_monitor_wdma_tx(struct mtk_eth *eth)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	static u32 pre_dtx[MTK_WDMA_CNT];
	static u32 err_cnt[MTK_WDMA_CNT];
	u32 i, cur_dtx, tx_busy, err_flag = 0;

	for (i = 0; i < MTK_WDMA_CNT; i++) {
		cur_dtx = mtk_r32(eth, reg_map->wdma_base[i] + 0xC);
		tx_busy = mtk_r32(eth, reg_map->wdma_base[i] + 0x204) &
			  MTK_TX_DMA_BUSY;
		if (cur_dtx == pre_dtx[i] && tx_busy) {
			err_cnt[i]++;
			if (err_cnt[i] >= 3) {
				pr_info("WDMA %d TX Info\n", i);
				pr_info("err_cnt = %d", err_cnt[i]);
				pr_info("prev_dtx = 0x%x  | cur_dtx = 0x%x\n",
					pre_dtx[i], cur_dtx);
				pr_info("WDMA_CTX_PTR = 0x%x\n",
					mtk_r32(eth, reg_map->wdma_base[i] + 0x8));
				pr_info("WDMA_DTX_PTR = 0x%x\n",
					mtk_r32(eth, reg_map->wdma_base[i] + 0xC));
				pr_info("WDMA_GLO_CFG = 0x%x\n",
					mtk_r32(eth, reg_map->wdma_base[i] + 0x204));
				pr_info("WDMA_TX_DBG_MON0 = 0x%x\n",
					mtk_r32(eth, reg_map->wdma_base[i] + 0x230));
				pr_info("==============================\n");
				err_flag = 1;
			}
		} else {
			err_cnt[i] = 0;
		}
		pre_dtx[i] = cur_dtx;
	}

	if (err_flag)
		return MTK_FE_START_RESET;
	else
		return 0;
}

static u32 mtk_monitor_wdma_rx(struct mtk_eth *eth)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	static u32 pre_drx[MTK_WDMA_CNT];
	static u32 pre_opq[MTK_WDMA_CNT];
	static u32 err_cnt[MTK_WDMA_CNT];
	u32 i = 0, cur_drx = 0, rx_busy = 0, err_flag = 0;
	u32 cur_opq = 0;

	for (i = 0; i < MTK_WDMA_CNT; i++) {
		cur_drx = mtk_r32(eth, reg_map->wdma_base[i] + 0x10C);
		rx_busy = mtk_r32(eth, reg_map->wdma_base[i] + 0x204) &
			  MTK_RX_DMA_BUSY;
		if (i == 0)
			cur_opq = (mtk_r32(eth, reg_map->pse_oq_sta + 5 * 4) &
				   0x1FF);
		else if (i == 1)
			cur_opq = (mtk_r32(eth, reg_map->pse_oq_sta + 5 * 4) &
				   0x1FF0000);
		else
			cur_opq = (mtk_r32(eth, reg_map->pse_oq_sta + 7 * 4) &
				   0x1FF0000);

		if (cur_drx == pre_drx[i] && rx_busy && cur_opq != 0 &&
		    cur_opq == pre_opq[i]) {
			err_cnt[i]++;
			if (err_cnt[i] >= 3) {
				pr_info("WDMA %d RX Info\n", i);
				pr_info("err_cnt = %d", err_cnt[i]);
				pr_info("prev_drx = 0x%x  | cur_drx = 0x%x\n",
					pre_drx[i], cur_drx);
				pr_info("WDMA_CRX_PTR = 0x%x\n",
					mtk_r32(eth, reg_map->wdma_base[i] + 0x108));
				pr_info("WDMA_DRX_PTR = 0x%x\n",
					mtk_r32(eth, reg_map->wdma_base[i] + 0x10C));
				pr_info("WDMA_GLO_CFG = 0x%x\n",
					mtk_r32(eth, reg_map->wdma_base[i] + 0x204));
				pr_info("==============================\n");
				err_flag = 1;
			}
		} else {
			err_cnt[i] = 0;
		}
		pre_drx[i] = cur_drx;
		pre_opq[i] = cur_opq;
	}

	if (err_flag)
		return MTK_FE_START_RESET;
	else
		return 0;
}

static u32 mtk_monitor_rx_fc(struct mtk_eth *eth)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	u32 i = 0, mib_base = 0, gdm_fc = 0;

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		mib_base = reg_map->gdm1_cnt + i * MTK_GDM_CNT_OFFSET +
			   MTK_GDM_RX_FC;
		gdm_fc = mtk_r32(eth, mib_base);
		if (gdm_fc < 1)
			return 1;
	}
	return 0;
}

static u32 mtk_monitor_qdma_tx(struct mtk_eth *eth)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	static u32 err_cnt_qtx;
	u32 err_flag = 0;
	u32 is_rx_fc = 0;
	u32 qdma_base = reg_map->qdma.glo_cfg - 0x204;

	u32 is_qfsm_hang = (mtk_r32(eth, qdma_base + 0x234) & 0xF00) != 0;
	u32 is_qfwd_hang = mtk_r32(eth, qdma_base + 0x308) == 0;

	is_rx_fc = mtk_monitor_rx_fc(eth);
	if (is_qfsm_hang && is_qfwd_hang && is_rx_fc) {
		err_cnt_qtx++;
		if (err_cnt_qtx >= 3) {
			pr_info("QDMA Tx Info\n");
			pr_info("err_cnt = %d", err_cnt_qtx);
			pr_info("is_qfsm_hang = %d\n", is_qfsm_hang);
			pr_info("is_qfwd_hang = %d\n", is_qfwd_hang);
			pr_info("-- -- -- -- -- -- --\n");
			pr_info("MTK_QDMA_FSM = 0x%x\n",
				mtk_r32(eth, qdma_base + 0x234));
			pr_info("MTK_QDMA_FWD_CNT = 0x%x\n",
				mtk_r32(eth, qdma_base + 0x308));
			pr_info("MTK_QDMA_FQ_CNT = 0x%x\n",
				mtk_r32(eth, reg_map->qdma.fq_count));
			pr_info("==============================\n");
			err_flag = 1;
		}
	} else {
		err_cnt_qtx = 0;
	}

	if (err_flag)
		return MTK_FE_STOP_TRAFFIC;
	else
		return 0;
}

static u32 mtk_monitor_qdma_rx(struct mtk_eth *eth)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	static u32 err_cnt_qrx;
	static u32 pre_fq_head, pre_fq_tail;
	u32 err_flag = 0;
	u32 qdma_base = reg_map->qdma.glo_cfg - 0x204;

	u32 qrx_fsm = (mtk_r32(eth, qdma_base + 0x234) & 0x1F) == 9;
	u32 fq_head = mtk_r32(eth, reg_map->qdma.fq_head);
	u32 fq_tail = mtk_r32(eth, reg_map->qdma.fq_tail);

	if (qrx_fsm && fq_head == pre_fq_head && fq_tail == pre_fq_tail) {
		err_cnt_qrx++;
		if (err_cnt_qrx >= 3) {
			pr_info("QDMA Rx Info\n");
			pr_info("err_cnt = %d", err_cnt_qrx);
			pr_info("MTK_QDMA_FSM = %d\n",
				mtk_r32(eth, qdma_base + 0x234));
			pr_info("FQ_HEAD = 0x%x\n",
				mtk_r32(eth, reg_map->qdma.fq_head));
			pr_info("FQ_TAIL = 0x%x\n",
				mtk_r32(eth, reg_map->qdma.fq_tail));
			err_flag = 1;
		}
	} else {
		err_cnt_qrx = 0;
	}
	pre_fq_head = fq_head;
	pre_fq_tail = fq_tail;

	if (err_flag)
		return MTK_FE_STOP_TRAFFIC;
	else
		return 0;
}

static u32 mtk_monitor_adma_rx(struct mtk_eth *eth)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	static u32 err_cnt_arx, pre_drx;
	u32 err_flag = 0, cur_drx = 0;

	u32 opq0 = (mtk_r32(eth, reg_map->pse_oq_sta) & 0x1FF) != 0;
	u32 cdm1_fsm = (mtk_r32(eth, MTK_FE_CDM1_FSM) & 0xFFFF0000) != 0;
	u32 cur_stat = ((mtk_r32(eth, reg_map->adma.adma_rx_dbg0) & 0x1F) == 0);
	u32 fifo_rdy = ((mtk_r32(eth, reg_map->adma.adma_rx_dbg0) & 0x40) == 0);
	cur_drx = mtk_r32(eth, reg_map->adma.rx_ptr + 0xC);

	if (opq0 && cdm1_fsm && cur_stat && fifo_rdy &&
	    (cur_drx == pre_drx)) {
		err_cnt_arx++;
		if (err_cnt_arx >= 3) {
			pr_info("ADMA Rx Info\n");
			pr_info("err_cnt = %d", err_cnt_arx);
			pr_info("CDM1_FSM = %d\n",
				mtk_r32(eth, MTK_FE_CDM1_FSM));
			pr_info("MTK_PSE_OQ_STA1 = 0x%x\n",
				mtk_r32(eth, reg_map->pse_oq_sta));
			pr_info("MTK_ADMA_RX_DBG0 = 0x%x\n",
				mtk_r32(eth, reg_map->adma.adma_rx_dbg0));
			pr_info("MTK_ADMA_RX_DBG1 = 0x%x\n",
				mtk_r32(eth, reg_map->adma.adma_rx_dbg0 + 4));
			pr_info("MTK_ADMA_DRX_PTR = 0x%x\n",
				mtk_r32(eth, reg_map->adma.rx_ptr + 0xC));
			pr_info("==============================\n");
			err_flag = 1;
		}
	} else {
		err_cnt_arx = 0;
	}

	pre_drx = cur_drx;
	if (err_flag)
		return MTK_FE_STOP_TRAFFIC;
	else
		return 0;
}

static u32 mtk_monitor_tdma_tx(struct mtk_eth *eth)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	static u32 err_cnt_ttx;
	static u32 pre_ipq10;
	static u32 pre_fsm;
	u32 err_flag = 0;
	u32 cur_fsm = 0;
	u32 tx_busy = 0;
	u32 ipq10;

	ipq10 = mtk_r32(eth, reg_map->pse_iq_sta + 6 * 4) & 0xFFF;
	cur_fsm = (mtk_r32(eth, MTK_FE_CDM6_FSM) & 0x1FFF) != 0;
	tx_busy = ((mtk_r32(eth, MTK_TDMA_GLO_CFG) & 0x2) != 0);

	if (ipq10 && cur_fsm && tx_busy &&
	    cur_fsm == pre_fsm && ipq10 == pre_ipq10) {
		err_cnt_ttx++;
		if (err_cnt_ttx >= 3) {
			pr_info("TDMA Tx Info\n");
			pr_info("err_cnt = %d", err_cnt_ttx);
			pr_info("CDM6_FSM = 0x%x, PRE_CDM6_FSM = 0x%x\n",
				mtk_r32(eth, MTK_FE_CDM6_FSM), pre_fsm);
			pr_info("PSE_IQ_P10 = 0x%x, PRE_PSE_IQ_P10 = 0x%x\n",
				mtk_r32(eth, reg_map->pse_iq_sta + 6 * 4),
				pre_ipq10);
			pr_info("DMA CFG = 0x%x\n",
				mtk_r32(eth, MTK_TDMA_GLO_CFG));
			pr_info("==============================\n");
			err_flag = 1;
		}
	} else {
		err_cnt_ttx = 0;
	}

	pre_fsm = cur_fsm;
	pre_ipq10 = ipq10;

	if (err_flag)
		return MTK_FE_STOP_TRAFFIC;
	else
		return 0;
}

static u32 mtk_monitor_tdma_rx(struct mtk_eth *eth)
{
	static u32 err_cnt_trx;
	static u32 pre_fsm;
	u32 err_flag = 0;
	u32 cur_fsm = 0;
	u32 rx_busy = 0;

	cur_fsm = (mtk_r32(eth, MTK_FE_CDM6_FSM) & 0xFFF0000) != 0;
	rx_busy = ((mtk_r32(eth, MTK_TDMA_GLO_CFG) & 0x8) != 0);

	if (cur_fsm == pre_fsm && cur_fsm != 0 && rx_busy) {
		err_cnt_trx++;
		if (err_cnt_trx >= 3) {
			pr_info("TDMA Rx Info\n");
			pr_info("err_cnt = %d", err_cnt_trx);
			pr_info("CDM6_FSM = %d\n",
				mtk_r32(eth, MTK_FE_CDM6_FSM));
			pr_info("DMA CFG = 0x%x\n",
				mtk_r32(eth, MTK_TDMA_GLO_CFG));
			pr_info("==============================\n");
			err_flag = 1;
		}
	} else {
		err_cnt_trx = 0;
	}

	pre_fsm = cur_fsm;

	if (err_flag)
		return MTK_FE_STOP_TRAFFIC;
	else
		return 0;
}

static u32 mtk_fe_gdm_fsm(struct mtk_eth *eth, int i)
{
	switch (i) {
	case 0:
		return mtk_r32(eth, MTK_FE_GDM1_FSM);
	case 1:
		return mtk_r32(eth, MTK_FE_GDM2_FSM);
	case 2:
		return mtk_r32(eth, MTK_FE_GDM3_FSM);
	default:
		return 0;
	}
}

static u32 mtk_monitor_gdm_rx(struct mtk_eth *eth)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	static u32 gmac_cnt[MTK_MAX_DEVS];
	static u32 gdm_cnt[MTK_MAX_DEVS];
	static u32 pre_fsm[MTK_MAX_DEVS];
	static u32 pre_ipq[MTK_MAX_DEVS];
	u32 gmac_rxcnt[MTK_MAX_DEVS];
	u32 is_gmac_rx[MTK_MAX_DEVS];
	u32 cur_fsm, pse_ipq, err_flag = 0, i;

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		is_gmac_rx[i] = (mtk_r32(eth, MTK_MAC_FSM(i)) & 0xFF0000) !=
				0x10000;
		gmac_rxcnt[i] = mtk_r32(eth, reg_map->gdm1_cnt +
					MTK_GDM_RX_BASE + i * MTK_GDM_CNT_OFFSET);
		if (is_gmac_rx[i] && (gmac_rxcnt[i] == 0)) {
			gmac_cnt[i]++;
			if (gmac_cnt[i] > 4) {
				pr_info("GMAC%d Rx Info\n", i + 1);
				pr_info("err_cnt = %d", gmac_cnt[i]);
				pr_info("GMAC_FSM = 0x%x\n",
					mtk_r32(eth, MTK_MAC_FSM(i)));
				err_flag = 1;
			}
		} else {
			gmac_cnt[i] = 0;
		}
	}

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (i == 0) {
			pse_ipq = (mtk_r32(eth, reg_map->pse_iq_sta) >> 16) &
				  0xFFF;
			cur_fsm = mtk_r32(eth, MTK_FE_GDM1_FSM) & 0xFF;
		} else if (i == 1) {
			pse_ipq = mtk_r32(eth, reg_map->pse_iq_sta + 4) &
				  0xFFF;
			cur_fsm = mtk_r32(eth, MTK_FE_GDM2_FSM) & 0xFF;
		} else {
			pse_ipq = (mtk_r32(eth, reg_map->pse_iq_sta + 7 * 4) >>
				   16) & 0xFFF;
			cur_fsm = mtk_r32(eth, MTK_FE_GDM3_FSM) & 0xFF;
		}

		if (((cur_fsm == pre_fsm[i] && cur_fsm == 0x23) ||
		     (cur_fsm == pre_fsm[i] && cur_fsm == 0x24)) &&
		    (pse_ipq == pre_ipq[i] && pse_ipq != 0x00)) {
			gdm_cnt[i]++;
			if (gdm_cnt[i] >= 3) {
				pr_info("GDM%d Rx Info\n", i + 1);
				pr_info("err_cnt = %d", gdm_cnt[i]);
				pr_info("GDM%d_FSM = %x\n", i + 1,
					mtk_fe_gdm_fsm(eth, i));
				pr_info("==============================\n");
				err_flag = 1;
			}
		} else {
			gdm_cnt[i] = 0;
		}

		pre_fsm[i] = cur_fsm;
		pre_ipq[i] = pse_ipq;
	}

	if (err_flag)
		return MTK_FE_STOP_TRAFFIC;
	else
		return 0;
}

static u32 mtk_monitor_gdm_tx(struct mtk_eth *eth)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	static u32 err_cnt[MTK_MAX_DEVS];
	u32 gmac_txcnt[MTK_MAX_DEVS];
	u32 is_gmac_tx[MTK_MAX_DEVS];
	u32 err_flag = 0, i, pse_opq;

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		is_gmac_tx[i] = (mtk_r32(eth, MTK_MAC_FSM(i)) & 0xFF000000) !=
				0x1000000;
		gmac_txcnt[i] = mtk_r32(eth, reg_map->gdm1_cnt +
					MTK_GDM_TX_BASE + i * MTK_GDM_CNT_OFFSET);
		if (i == 0)
			pse_opq = (mtk_r32(eth, reg_map->pse_oq_sta) >> 16) &
				  0xFFF;
		else if (i == 1)
			pse_opq = mtk_r32(eth, reg_map->pse_oq_sta + 4) &
				  0xFFF;
		else
			pse_opq = (mtk_r32(eth, reg_map->pse_oq_sta + 7 * 4) >>
				   16) & 0xFFF;

		if (is_gmac_tx[i] && (gmac_txcnt[i] == 0) && (pse_opq > 0)) {
			err_cnt[i]++;
			if (err_cnt[i] > 4) {
				pr_info("GMAC%d Tx Info\n", i + 1);
				pr_info("err_cnt = %d", err_cnt[i]);
				pr_info("GMAC_FSM = 0x%x\n",
					mtk_r32(eth, MTK_MAC_FSM(i)));
				err_flag = 1;
			}
		} else {
			err_cnt[i] = 0;
		}
	}

	if (err_flag)
		return MTK_FE_STOP_TRAFFIC;
	else
		return 0;
}

typedef u32 (*mtk_monitor_xdma_func)(struct mtk_eth *eth);

static const mtk_monitor_xdma_func mtk_reset_monitor_func[] = {
	[0] = mtk_monitor_wdma_tx,
	[1] = mtk_monitor_wdma_rx,
	[2] = mtk_monitor_qdma_tx,
	[3] = mtk_monitor_qdma_rx,
	[4] = mtk_monitor_adma_rx,
	[5] = mtk_monitor_tdma_tx,
	[6] = mtk_monitor_tdma_rx,
	[7] = mtk_monitor_gdm_tx,
	[8] = mtk_monitor_gdm_rx,
};

void mtk_dma_monitor_work(struct work_struct *work)
{
	struct delayed_work *del_work = to_delayed_work(work);
	struct mtk_eth *eth =
		container_of(del_work, struct mtk_eth, reset.monitor_work);
	u32 i = 0, ret = 0;
	static bool first_run = true;

	if (first_run) {
		pr_info("mt7988_reset: DMA monitor watchdog started\n");
		first_run = false;
	}

	if (test_bit(MTK_RESETTING, &eth->state))
		goto out;

	for (i = 0; i < ARRAY_SIZE(mtk_reset_monitor_func); i++) {
		ret = (*mtk_reset_monitor_func[i])(eth);
		if ((ret == MTK_FE_START_RESET) ||
		    (ret == MTK_FE_STOP_TRAFFIC)) {
			schedule_work(&eth->pending_work);
			break;
		}
	}

out:
	schedule_delayed_work(&eth->reset.monitor_work,
			      MTK_DMA_MONITOR_TIMEOUT);
}

void mtk_prepare_reset_fe(struct mtk_eth *eth)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	u32 i = 0, val = 0, mcr = 0;

	mtk_w32(eth, 0, MTK_FE_INT_ENABLE);
	mtk_w32(eth, 0, reg_map->adma.irq_mask);
	mtk_w32(eth, 0, reg_map->tx_irq_mask);

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i])
			continue;
		netif_tx_disable(eth->netdev[i]);
	}

	val = mtk_r32(eth, reg_map->qdma.glo_cfg);
	mtk_w32(eth, val & ~(MTK_TX_DMA_EN), reg_map->qdma.glo_cfg);

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		struct mtk_mac *mac = eth->mac[i];

		if (!mac)
			continue;

		if (i != MTK_GMAC1_ID &&
		    mtk_interface_mode_is_xgmii(mac->interface)) {
			mcr = mtk_r32(eth, MTK_XMAC_MCR(mac->id));
			mcr &= 0xfffffff0;
			mcr |= XMAC_MCR_TRX_DISABLE;
			mtk_w32(eth, mcr, MTK_XMAC_MCR(mac->id));
		}

		if (i == MTK_GMAC1_ID ||
		    !mtk_interface_mode_is_xgmii(mac->interface)) {
			mcr = mtk_r32(eth, MTK_MAC_MCR(mac->id));
			mcr &= ~(MAC_MCR_TX_EN | MAC_MCR_RX_EN);
			mtk_w32(eth, mcr, MTK_MAC_MCR(mac->id));
		}
	}

	for (i = 0; i < MTK_MAX_DEVS; i++)
		mtk_gdm_config(eth, i, MTK_GDMA_DROP_ALL);

	val = mtk_r32(eth, reg_map->adma.glo_cfg);
	mtk_w32(eth, val & ~(MTK_RX_DMA_EN), reg_map->adma.glo_cfg);
}

static int mtk_eth_netdevice_event(struct notifier_block *unused,
				   unsigned long event, void *ptr)
{
	switch (event) {
	case MTK_WIFI_RESET_DONE:
	case MTK_FE_STOP_TRAFFIC_DONE:
		mtk_rest_cnt--;
		if (!mtk_rest_cnt)
			mtk_rest_cnt = mtk_wifi_num;
		break;
	case MTK_WIFI_CHIP_ONLINE:
		mtk_wifi_num++;
		mtk_rest_cnt = mtk_wifi_num;
		break;
	case MTK_WIFI_CHIP_OFFLINE:
		mtk_wifi_num--;
		mtk_rest_cnt = mtk_wifi_num;
		break;
	case MTK_FE_STOP_TRAFFIC_DONE_FAIL:
		pr_info("%s rcv done event:%lx\n", __func__, event);
		mtk_rest_cnt = mtk_wifi_num;
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

struct notifier_block mtk_eth_netdevice_nb __read_mostly = {
	.notifier_call = mtk_eth_netdevice_event,
};

int mtk_reset_init(struct mtk_eth *eth)
{
	memset(&eth->reset_event, 0, sizeof(eth->reset_event));
	return register_netdevice_notifier(&mtk_eth_netdevice_nb);
}

void mtk_reset_deinit(struct mtk_eth *eth)
{
	unregister_netdevice_notifier(&mtk_eth_netdevice_nb);
}
