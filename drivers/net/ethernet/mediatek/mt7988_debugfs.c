// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Felix Fietkau <nbd@nbd.name>
 * Copyright (C) 2018 MediaTek Inc.
 * Copyright (C) 2009-2016 John Crispin <blogic@openwrt.org>
 * Copyright (C) 2009-2016 Felix Fietkau <nbd@openwrt.org>
 * Copyright (C) 2013-2016 Michael Lee <igvtee@gmail.com>
 * Adapted for MT7988 standalone driver by
 * Semenets V. Pavel <p.semenets@gmail.com>
 */

#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/netdevice.h>
#include <net/ipv6.h>

#include "mt7988_eth.h"
#include "mt7988_dma.h"
#include "mt7988_reset.h"
#include "mt7988_debugfs.h"

static u32 hw_lro_agg_num_cnt[MTK_HW_LRO_RING_NUM][MTK_HW_LRO_MAX_AGG_CNT + 1];
static u32 hw_lro_agg_size_cnt[MTK_HW_LRO_RING_NUM][16];
static u32 hw_lro_tot_agg_cnt[MTK_HW_LRO_RING_NUM];
static u32 hw_lro_tot_flush_cnt[MTK_HW_LRO_RING_NUM];
static u32 hw_lro_agg_flush_cnt[MTK_HW_LRO_RING_NUM];
static u32 hw_lro_age_flush_cnt[MTK_HW_LRO_RING_NUM];
static u32 hw_lro_seq_flush_cnt[MTK_HW_LRO_RING_NUM];
static u32 hw_lro_timestamp_flush_cnt[MTK_HW_LRO_RING_NUM];
static u32 hw_lro_norule_flush_cnt[MTK_HW_LRO_RING_NUM];

u32 dbg_show_level;

static struct dentry *mt7988_debugfs_root;

struct mtk_flow_addr_info {
	void *src, *dest;
	u16 *src_port, *dest_port;
	bool ipv6;
};

static const char *mtk_foe_entry_state_str(int state)
{
	static const char * const state_str[] = {
		[MTK_FOE_STATE_INVALID] = "INV",
		[MTK_FOE_STATE_UNBIND] = "UNB",
		[MTK_FOE_STATE_BIND] = "BND",
		[MTK_FOE_STATE_FIN] = "FIN",
	};

	if (state >= ARRAY_SIZE(state_str) || !state_str[state])
		return "UNK";

	return state_str[state];
}

static const char *mtk_foe_pkt_type_str(int type)
{
	static const char * const type_str[] = {
		[MTK_PPE_PKT_TYPE_IPV4_HNAPT] = "IPv4 5T",
		[MTK_PPE_PKT_TYPE_IPV4_ROUTE] = "IPv4 3T",
		[MTK_PPE_PKT_TYPE_IPV4_DSLITE] = "DS-LITE",
		[MTK_PPE_PKT_TYPE_IPV6_ROUTE_3T] = "IPv6 3T",
		[MTK_PPE_PKT_TYPE_IPV6_ROUTE_5T] = "IPv6 5T",
		[MTK_PPE_PKT_TYPE_IPV6_6RD] = "6RD",
	};

	if (type >= ARRAY_SIZE(type_str) || !type_str[type])
		return "UNKNOWN";

	return type_str[type];
}

static void mtk_print_addr(struct seq_file *m, u32 *addr, bool ipv6)
{
	__be32 n_addr[IPV6_ADDR_WORDS];

	if (!ipv6) {
		seq_printf(m, "%pI4h", addr);
		return;
	}

	ipv6_addr_cpu_to_be32(n_addr, addr);
	seq_printf(m, "%pI6", n_addr);
}

static void mtk_print_addr_info(struct seq_file *m,
				struct mtk_flow_addr_info *ai)
{
	mtk_print_addr(m, ai->src, ai->ipv6);
	if (ai->src_port)
		seq_printf(m, ":%d", *ai->src_port);
	seq_printf(m, "->");
	mtk_print_addr(m, ai->dest, ai->ipv6);
	if (ai->dest_port)
		seq_printf(m, ":%d", *ai->dest_port);
}

static int mtk_ppe_debugfs_foe_show(struct seq_file *m, void *private,
				    bool bind)
{
	struct mtk_ppe *ppe = m->private;
	int i;

	for (i = 0; i < MTK_PPE_ENTRIES; i++) {
		struct mtk_foe_entry *entry = mtk_foe_get_entry(ppe, i);
		struct mtk_foe_mac_info *l2;
		struct mtk_flow_addr_info ai = {};
		struct mtk_foe_accounting *acct;
		unsigned char h_source[ETH_ALEN];
		unsigned char h_dest[ETH_ALEN];
		int type, state;
		u32 ib2;

		state = FIELD_GET(MTK_FOE_IB1_STATE, entry->ib1);
		if (!state)
			continue;

		if (bind && state != MTK_FOE_STATE_BIND)
			continue;

		acct = mtk_foe_entry_get_mib(ppe, i, NULL);

		type = mtk_get_ib1_pkt_type(ppe->eth, entry->ib1);
		seq_printf(m, "%05x %s %7s", i,
			   mtk_foe_entry_state_str(state),
			   mtk_foe_pkt_type_str(type));

		switch (type) {
		case MTK_PPE_PKT_TYPE_IPV4_HNAPT:
		case MTK_PPE_PKT_TYPE_IPV4_DSLITE:
			ai.src_port = &entry->ipv4.orig.src_port;
			ai.dest_port = &entry->ipv4.orig.dest_port;
			fallthrough;
		case MTK_PPE_PKT_TYPE_IPV4_ROUTE:
			ai.src = &entry->ipv4.orig.src_ip;
			ai.dest = &entry->ipv4.orig.dest_ip;
			break;
		case MTK_PPE_PKT_TYPE_IPV6_ROUTE_5T:
			ai.src_port = &entry->ipv6.src_port;
			ai.dest_port = &entry->ipv6.dest_port;
			fallthrough;
		case MTK_PPE_PKT_TYPE_IPV6_ROUTE_3T:
		case MTK_PPE_PKT_TYPE_IPV6_6RD:
			ai.src = &entry->ipv6.src_ip;
			ai.dest = &entry->ipv6.dest_ip;
			ai.ipv6 = true;
			break;
		}

		seq_printf(m, " orig=");
		mtk_print_addr_info(m, &ai);

		switch (type) {
		case MTK_PPE_PKT_TYPE_IPV4_HNAPT:
		case MTK_PPE_PKT_TYPE_IPV4_DSLITE:
			ai.src_port = &entry->ipv4.new.src_port;
			ai.dest_port = &entry->ipv4.new.dest_port;
			fallthrough;
		case MTK_PPE_PKT_TYPE_IPV4_ROUTE:
			ai.src = &entry->ipv4.new.src_ip;
			ai.dest = &entry->ipv4.new.dest_ip;
			seq_printf(m, " new=");
			mtk_print_addr_info(m, &ai);
			break;
		}

		if (type >= MTK_PPE_PKT_TYPE_IPV4_DSLITE) {
			l2 = &entry->ipv6.l2;
			ib2 = entry->ipv6.ib2;
		} else {
			l2 = &entry->ipv4.l2;
			ib2 = entry->ipv4.ib2;
		}

		*((__be32 *)h_source) = htonl(l2->src_mac_hi);
		*((__be16 *)&h_source[4]) = htons(l2->src_mac_lo);
		*((__be32 *)h_dest) = htonl(l2->dest_mac_hi);
		*((__be16 *)&h_dest[4]) = htons(l2->dest_mac_lo);

		seq_printf(m, " eth=%pM->%pM etype=%04x"
			      " vlan=%d,%d ib1=%08x ib2=%08x"
			      " packets=%llu bytes=%llu\n",
			   h_source, h_dest, ntohs(l2->etype),
			   l2->vlan1, l2->vlan2, entry->ib1, ib2,
			   acct ? acct->packets : 0, acct ? acct->bytes : 0);
	}

	return 0;
}

static int mtk_ppe_debugfs_foe_all_show(struct seq_file *m, void *private)
{
	return mtk_ppe_debugfs_foe_show(m, private, false);
}
DEFINE_SHOW_ATTRIBUTE(mtk_ppe_debugfs_foe_all);

static int mtk_ppe_debugfs_foe_bind_show(struct seq_file *m, void *private)
{
	return mtk_ppe_debugfs_foe_show(m, private, true);
}
DEFINE_SHOW_ATTRIBUTE(mtk_ppe_debugfs_foe_bind);

int mtk_ppe_debugfs_init(struct mtk_ppe *ppe, int index)
{
	struct dentry *root;

	snprintf(ppe->dirname, sizeof(ppe->dirname), "ppe%d", index);

	root = debugfs_create_dir(ppe->dirname, NULL);
	debugfs_create_file("entries", 0444, root, ppe,
			    &mtk_ppe_debugfs_foe_all_fops);
	debugfs_create_file("bind", 0444, root, ppe,
			    &mtk_ppe_debugfs_foe_bind_fops);

	return 0;
}

static int mt7988_dbg_regs_show(struct seq_file *m, void *private)
{
	struct mtk_eth *eth = m->private;
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	int i;

	seq_puts(m, "   <<DEBUG REG DUMP>>\n");

	seq_printf(m, "| FE_INT_STA    : %08x |\n",
		   mtk_r32(eth, MTK_FE_INT_STATUS));
	seq_printf(m, "| FE_INT_STA2   : %08x |\n",
		   mtk_r32(eth, MTK_FE_INT_STATUS2));
	seq_printf(m, "| PSE_FQFC_CFG  : %08x |\n",
		   mtk_r32(eth, MTK_PSE_FQFC_CFG));

	for (i = 0; i < 8; i++)
		seq_printf(m, "| PSE_IQ_STA%d   : %08x |\n",
			   i + 1, mtk_r32(eth, MTK_PSE_IQ_STA(i)));

	for (i = 0; i < 8; i++)
		seq_printf(m, "| PSE_OQ_STA%d   : %08x |\n",
			   i + 1, mtk_r32(eth, MTK_PSE_OQ_STA(i)));

	seq_printf(m, "| ADMA_CRX_IDX  : %08x |\n",
		   mtk_r32(eth, reg_map->adma.pcrx_ptr));
	seq_printf(m, "| QDMA_CTX_IDX  : %08x |\n",
		   mtk_r32(eth, reg_map->qdma.ctx_ptr));
	seq_printf(m, "| QDMA_DTX_IDX  : %08x |\n",
		   mtk_r32(eth, reg_map->qdma.dtx_ptr));
	seq_printf(m, "| QDMA_FQ_CNT   : %08x |\n",
		   mtk_r32(eth, reg_map->qdma.fq_count));
	seq_printf(m, "| FE_PSE_FREE   : %08x |\n",
		   mtk_r32(eth, MTK_FE_PSE_FREE));
	seq_printf(m, "| FE_DROP_FQ    : %08x |\n",
		   mtk_r32(eth, MTK_FE_DROP_FQ));
	seq_printf(m, "| FE_DROP_FC    : %08x |\n",
		   mtk_r32(eth, MTK_FE_DROP_FC));
	seq_printf(m, "| FE_DROP_PPE   : %08x |\n",
		   mtk_r32(eth, MTK_FE_DROP_PPE));

	for (i = 0; i < MTK_GMAC_ID_MAX; i++) {
		seq_printf(m, "| GDM%d_IG_CTRL  : %08x |\n",
			   i + 1, mtk_r32(eth, MTK_GDMA_FWD_CFG(i)));
		seq_printf(m, "| MAC_P%d_MCR    : %08x |\n",
			   i + 1, mtk_r32(eth, MTK_MAC_MCR(i)));
		seq_printf(m, "| MAC_P%d_FSM    : %08x |\n",
			   i + 1, mtk_r32(eth, MTK_MAC_FSM(i)));
	}

	seq_printf(m, "| FE_CDM1_FSM   : %08x |\n",
		   mtk_r32(eth, MTK_FE_CDM1_FSM));
	seq_printf(m, "| FE_CDM2_FSM   : %08x |\n",
		   mtk_r32(eth, MTK_FE_CDM2_FSM));
	seq_printf(m, "| FE_CDM3_FSM   : %08x |\n",
		   mtk_r32(eth, MTK_FE_CDM3_FSM));
	seq_printf(m, "| FE_CDM4_FSM   : %08x |\n",
		   mtk_r32(eth, MTK_FE_CDM4_FSM));
	seq_printf(m, "| FE_CDM5_FSM   : %08x |\n",
		   mtk_r32(eth, MTK_FE_CDM5_FSM));
	seq_printf(m, "| FE_CDM6_FSM   : %08x |\n",
		   mtk_r32(eth, MTK_FE_CDM6_FSM));
	seq_printf(m, "| FE_GDM1_FSM   : %08x |\n",
		   mtk_r32(eth, MTK_FE_GDM1_FSM));
	seq_printf(m, "| FE_GDM2_FSM   : %08x |\n",
		   mtk_r32(eth, MTK_FE_GDM2_FSM));
	seq_printf(m, "| FE_GDM3_FSM   : %08x |\n",
		   mtk_r32(eth, MTK_FE_GDM3_FSM));

	mtk_w32(eth, 0xffffffff, MTK_FE_INT_STATUS);
	mtk_w32(eth, 0xffffffff, MTK_FE_INT_STATUS2);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt7988_dbg_regs);

static int mt7988_tx_ring_show(struct seq_file *m, void *private)
{
	struct mtk_eth *eth = m->private;
	struct mtk_tx_ring *ring = &eth->tx_ring;
	struct mtk_tx_dma_v2 *txd;
	int i;

	seq_printf(m, "free count = %d\n", atomic_read(&ring->free_count));
	seq_printf(m, "cpu next free: %td\n",
		   (ring->next_free - (struct mtk_tx_dma_v2 *)ring->dma));
	seq_printf(m, "cpu last free: %td\n",
		   (ring->last_free - (struct mtk_tx_dma_v2 *)ring->dma));

	for (i = 0; i < ring->dma_size; i++) {
		dma_addr_t tmp = ring->phys +
				 i * (dma_addr_t)eth->soc->tx.desc_size;
		txd = ring->dma + i * eth->soc->tx.desc_size;
		seq_printf(m, "%d (%pad): %08x %08x %08x %08x %08x %08x %08x %08x\n",
			   i, &tmp, txd->txd1, txd->txd2, txd->txd3, txd->txd4,
			   txd->txd5, txd->txd6, txd->txd7, txd->txd8);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt7988_tx_ring);

static int mt7988_rx_ring_show(struct seq_file *m, void *private)
{
	struct mtk_eth *eth = m->private;
	struct mtk_rx_ring *ring;
	struct mtk_rx_dma_v2 *rxd;
	int i, j;

	for (j = 0; j < MTK_MAX_RX_RING_NUM; j++) {
		ring = &eth->rx_ring[j];
		if (!ring->dma)
			continue;

		seq_printf(m, "[Ring%d] next to read: %d\n", j,
			   NEXT_DESP_IDX(ring->calc_idx, ring->dma_size));
		for (i = 0; i < ring->dma_size; i++) {
			rxd = ring->dma + i * eth->soc->rx.desc_size;
			seq_printf(m, "%d: %08x %08x %08x %08x %08x %08x %08x %08x\n",
				   i, rxd->rxd1, rxd->rxd2, rxd->rxd3, rxd->rxd4,
				   rxd->rxd5, rxd->rxd6, rxd->rxd7, rxd->rxd8);
		}
	}

	if (eth->rx_ring_qdma.dma) {
		ring = &eth->rx_ring_qdma;
		seq_printf(m, "[QDMA Ring] next to read: %d\n",
			   NEXT_DESP_IDX(ring->calc_idx, ring->dma_size));
		for (i = 0; i < ring->dma_size; i++) {
			rxd = ring->dma + i * eth->soc->rx.desc_size;
			seq_printf(m, "%d: %08x %08x %08x %08x %08x %08x %08x %08x\n",
				   i, rxd->rxd1, rxd->rxd2, rxd->rxd3, rxd->rxd4,
				   rxd->rxd5, rxd->rxd6, rxd->rxd7, rxd->rxd8);
		}
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt7988_rx_ring);

static int mt7988_hw_lro_stats_show(struct seq_file *m, void *private)
{
	int i;

	seq_puts(m, "HW LRO statistic dump:\n");

	seq_puts(m, "Cnt:   RING4 | RING5 | RING6 | RING7 | Total\n");
	for (i = 0; i <= MTK_HW_LRO_MAX_AGG_CNT; i++) {
		seq_printf(m, " %2d: %8d %8d %8d %8d %8d\n",
			   i, hw_lro_agg_num_cnt[0][i], hw_lro_agg_num_cnt[1][i],
			   hw_lro_agg_num_cnt[2][i], hw_lro_agg_num_cnt[3][i],
			   hw_lro_agg_num_cnt[0][i] + hw_lro_agg_num_cnt[1][i] +
			   hw_lro_agg_num_cnt[2][i] + hw_lro_agg_num_cnt[3][i]);
	}

	seq_puts(m, "Total agg:   RING4 | RING5 | RING6 | RING7 | Total\n");
	seq_printf(m, "           %8d %8d %8d %8d %8d\n",
		   hw_lro_tot_agg_cnt[0], hw_lro_tot_agg_cnt[1],
		   hw_lro_tot_agg_cnt[2], hw_lro_tot_agg_cnt[3],
		   hw_lro_tot_agg_cnt[0] + hw_lro_tot_agg_cnt[1] +
		   hw_lro_tot_agg_cnt[2] + hw_lro_tot_agg_cnt[3]);

	seq_puts(m, "Total flush: RING4 | RING5 | RING6 | RING7 | Total\n");
	seq_printf(m, "           %8d %8d %8d %8d %8d\n",
		   hw_lro_tot_flush_cnt[0], hw_lro_tot_flush_cnt[1],
		   hw_lro_tot_flush_cnt[2], hw_lro_tot_flush_cnt[3],
		   hw_lro_tot_flush_cnt[0] + hw_lro_tot_flush_cnt[1] +
		   hw_lro_tot_flush_cnt[2] + hw_lro_tot_flush_cnt[3]);

	seq_puts(m, "Avg agg:     RING4 | RING5 | RING6 | RING7 | Total\n");
	seq_printf(m, "           %8d %8d %8d %8d %8d\n",
		   hw_lro_tot_flush_cnt[0] ?
		   hw_lro_tot_agg_cnt[0] / hw_lro_tot_flush_cnt[0] : 0,
		   hw_lro_tot_flush_cnt[1] ?
		   hw_lro_tot_agg_cnt[1] / hw_lro_tot_flush_cnt[1] : 0,
		   hw_lro_tot_flush_cnt[2] ?
		   hw_lro_tot_agg_cnt[2] / hw_lro_tot_flush_cnt[2] : 0,
		   hw_lro_tot_flush_cnt[3] ?
		   hw_lro_tot_agg_cnt[3] / hw_lro_tot_flush_cnt[3] : 0,
		   (hw_lro_tot_flush_cnt[0] + hw_lro_tot_flush_cnt[1] +
		    hw_lro_tot_flush_cnt[2] + hw_lro_tot_flush_cnt[3]) ?
		   ((hw_lro_tot_agg_cnt[0] + hw_lro_tot_agg_cnt[1] +
		     hw_lro_tot_agg_cnt[2] + hw_lro_tot_agg_cnt[3]) /
		    (hw_lro_tot_flush_cnt[0] + hw_lro_tot_flush_cnt[1] +
		     hw_lro_tot_flush_cnt[2] + hw_lro_tot_flush_cnt[3])) : 0);

	seq_puts(m, "HW LRO flush pkt len:\n");
	seq_puts(m, " Length    | RING4  | RING5  | RING6  | RING7  | Total\n");
	for (i = 0; i < 15; i++) {
		seq_printf(m, "%d~%d: %8d %8d %8d %8d %8d\n",
			   i * 5000, (i + 1) * 5000,
			   hw_lro_agg_size_cnt[0][i], hw_lro_agg_size_cnt[1][i],
			   hw_lro_agg_size_cnt[2][i], hw_lro_agg_size_cnt[3][i],
			   hw_lro_agg_size_cnt[0][i] + hw_lro_agg_size_cnt[1][i] +
			   hw_lro_agg_size_cnt[2][i] + hw_lro_agg_size_cnt[3][i]);
	}

	seq_puts(m, "Flush reason:  RING4 | RING5 | RING6 | RING7 | Total\n");
	seq_printf(m, "AGG timeout: %8d %8d %8d %8d %8d\n",
		   hw_lro_agg_flush_cnt[0], hw_lro_agg_flush_cnt[1],
		   hw_lro_agg_flush_cnt[2], hw_lro_agg_flush_cnt[3],
		   hw_lro_agg_flush_cnt[0] + hw_lro_agg_flush_cnt[1] +
		   hw_lro_agg_flush_cnt[2] + hw_lro_agg_flush_cnt[3]);
	seq_printf(m, "AGE timeout: %8d %8d %8d %8d %8d\n",
		   hw_lro_age_flush_cnt[0], hw_lro_age_flush_cnt[1],
		   hw_lro_age_flush_cnt[2], hw_lro_age_flush_cnt[3],
		   hw_lro_age_flush_cnt[0] + hw_lro_age_flush_cnt[1] +
		   hw_lro_age_flush_cnt[2] + hw_lro_age_flush_cnt[3]);
	seq_printf(m, "Not in-seq:  %8d %8d %8d %8d %8d\n",
		   hw_lro_seq_flush_cnt[0], hw_lro_seq_flush_cnt[1],
		   hw_lro_seq_flush_cnt[2], hw_lro_seq_flush_cnt[3],
		   hw_lro_seq_flush_cnt[0] + hw_lro_seq_flush_cnt[1] +
		   hw_lro_seq_flush_cnt[2] + hw_lro_seq_flush_cnt[3]);
	seq_printf(m, "Timestamp:   %8d %8d %8d %8d %8d\n",
		   hw_lro_timestamp_flush_cnt[0],
		   hw_lro_timestamp_flush_cnt[1],
		   hw_lro_timestamp_flush_cnt[2],
		   hw_lro_timestamp_flush_cnt[3],
		   hw_lro_timestamp_flush_cnt[0] +
		   hw_lro_timestamp_flush_cnt[1] +
		   hw_lro_timestamp_flush_cnt[2] +
		   hw_lro_timestamp_flush_cnt[3]);
	seq_printf(m, "No LRO rule: %8d %8d %8d %8d %8d\n",
		   hw_lro_norule_flush_cnt[0], hw_lro_norule_flush_cnt[1],
		   hw_lro_norule_flush_cnt[2], hw_lro_norule_flush_cnt[3],
		   hw_lro_norule_flush_cnt[0] + hw_lro_norule_flush_cnt[1] +
		   hw_lro_norule_flush_cnt[2] + hw_lro_norule_flush_cnt[3]);

	return 0;
}

static ssize_t mt7988_hw_lro_stats_write(struct file *file,
					 const char __user *buffer,
					 size_t count, loff_t *data)
{
	memset(hw_lro_agg_num_cnt, 0, sizeof(hw_lro_agg_num_cnt));
	memset(hw_lro_agg_size_cnt, 0, sizeof(hw_lro_agg_size_cnt));
	memset(hw_lro_tot_agg_cnt, 0, sizeof(hw_lro_tot_agg_cnt));
	memset(hw_lro_tot_flush_cnt, 0, sizeof(hw_lro_tot_flush_cnt));
	memset(hw_lro_agg_flush_cnt, 0, sizeof(hw_lro_agg_flush_cnt));
	memset(hw_lro_age_flush_cnt, 0, sizeof(hw_lro_age_flush_cnt));
	memset(hw_lro_seq_flush_cnt, 0, sizeof(hw_lro_seq_flush_cnt));
	memset(hw_lro_timestamp_flush_cnt, 0,
	       sizeof(hw_lro_timestamp_flush_cnt));
	memset(hw_lro_norule_flush_cnt, 0, sizeof(hw_lro_norule_flush_cnt));
	pr_info("hw lro stats cleared\n");
	return count;
}

static int mt7988_hw_lro_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, mt7988_hw_lro_stats_show, inode->i_private);
}

static const struct file_operations mt7988_hw_lro_stats_fops = {
	.owner = THIS_MODULE,
	.open = mt7988_hw_lro_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = mt7988_hw_lro_stats_write,
	.release = single_release,
};

static int mt7988_reset_event_show(struct seq_file *m, void *private)
{
	struct mtk_eth *eth = m->private;
	struct mtk_reset_event reset_event = eth->reset_event;

	seq_puts(m, "[Event]         [Count]\n");
	seq_printf(m, " FQ Empty:    %d\n",
		   reset_event.count[MTK_EVENT_FQ_EMPTY]);
	seq_printf(m, " TSO Fail:    %d\n",
		   reset_event.count[MTK_EVENT_TSO_FAIL]);
	seq_printf(m, " TSO Illegal: %d\n",
		   reset_event.count[MTK_EVENT_TSO_ILLEGAL]);
	seq_printf(m, " TSO Align:   %d\n",
		   reset_event.count[MTK_EVENT_TSO_ALIGN]);
	seq_printf(m, " RFIFO OV:    %d\n",
		   reset_event.count[MTK_EVENT_RFIFO_OV]);
	seq_printf(m, " RFIFO UF:    %d\n",
		   reset_event.count[MTK_EVENT_RFIFO_UF]);
	seq_printf(m, " Force:       %d\n",
		   reset_event.count[MTK_EVENT_FORCE]);
	seq_puts(m, "----------------------------\n");
	seq_printf(m, " Warm Cnt:    %d\n",
		   reset_event.count[MTK_EVENT_WARM_CNT]);
	seq_printf(m, " Cold Cnt:    %d\n",
		   reset_event.count[MTK_EVENT_COLD_CNT]);
	seq_printf(m, " Total Cnt:   %d\n",
		   reset_event.count[MTK_EVENT_TOTAL_CNT]);

	return 0;
}

static ssize_t mt7988_reset_event_write(struct file *file,
					const char __user *buffer,
					size_t count, loff_t *data)
{
	struct mtk_eth *eth = file->private_data;

	memset(&eth->reset_event, 0, sizeof(struct mtk_reset_event));
	pr_info("reset event counter cleared\n");
	return count;
}

static int mt7988_reset_event_open(struct inode *inode, struct file *file)
{
	return single_open(file, mt7988_reset_event_show, inode->i_private);
}

static const struct file_operations mt7988_reset_event_fops = {
	.owner = THIS_MODULE,
	.open = mt7988_reset_event_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = mt7988_reset_event_write,
	.release = single_release,
};

static int mt7988_gdm_cnt_show(struct seq_file *m, void *private)
{
	struct mtk_eth *eth = m->private;
	u32 mib_base;
	int i;

	seq_puts(m, "+-----------------------------------------------+\n");
	seq_puts(m, "|          <<GDMA MIB>>                         |\n");
	seq_puts(m, "+-----------------------------------------------+\n");

	for (i = 0; i < MTK_GMAC_ID_MAX; i++) {
		mib_base = MTK_GDM1_TX_GBCNT + MTK_STAT_OFFSET_V3 * i;
		seq_printf(m, "\n--- GDM%d ---\n", i + 1);
		seq_printf(m, " RX_GBCNT  : %010u\n", mtk_r32(eth, mib_base));
		seq_printf(m, " RX_GPCNT  : %010u\n", mtk_r32(eth, mib_base + 0x08));
		seq_printf(m, " RX_OERCNT : %010u\n", mtk_r32(eth, mib_base + 0x10));
		seq_printf(m, " RX_FERCNT : %010u\n", mtk_r32(eth, mib_base + 0x14));
		seq_printf(m, " RX_SERCNT : %010u\n", mtk_r32(eth, mib_base + 0x18));
		seq_printf(m, " RX_LERCNT : %010u\n", mtk_r32(eth, mib_base + 0x1C));
		seq_printf(m, " RX_CERCNT : %010u\n", mtk_r32(eth, mib_base + 0x20));
		seq_printf(m, " RX_FCCNT  : %010u\n", mtk_r32(eth, mib_base + 0x24));
		seq_printf(m, " TX_GBCNT  : %010u\n", mtk_r32(eth, mib_base + 0x40));
		seq_printf(m, " TX_GPCNT  : %010u\n", mtk_r32(eth, mib_base + 0x48));
		seq_printf(m, " TX_SKIPCNT: %010u\n", mtk_r32(eth, mib_base + 0x50));
		seq_printf(m, " TX_COLCNT : %010u\n", mtk_r32(eth, mib_base + 0x54));
	}

	seq_puts(m, "+-----------------------------------------------+\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt7988_gdm_cnt);

static int mt7988_qdma_status_show(struct seq_file *m, void *private)
{
	struct mtk_eth *eth = m->private;
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	u32 val;
	int i;

	seq_puts(m, "=== QDMA Status ===\n");
	seq_printf(m, "GLO_CFG    : %08x\n", mtk_r32(eth, reg_map->qdma.glo_cfg));
	seq_printf(m, "CTX_PTR    : %08x\n", mtk_r32(eth, reg_map->qdma.ctx_ptr));
	seq_printf(m, "DTX_PTR    : %08x\n", mtk_r32(eth, reg_map->qdma.dtx_ptr));
	seq_printf(m, "CRX_PTR    : %08x\n", mtk_r32(eth, reg_map->qdma.crx_ptr));
	seq_printf(m, "DRX_PTR    : %08x\n", mtk_r32(eth, reg_map->qdma.drx_ptr));
	seq_printf(m, "FQ_HEAD    : %08x\n", mtk_r32(eth, reg_map->qdma.fq_head));
	seq_printf(m, "FQ_TAIL    : %08x\n", mtk_r32(eth, reg_map->qdma.fq_tail));
	seq_printf(m, "FQ_COUNT   : %08x\n", mtk_r32(eth, reg_map->qdma.fq_count));
	seq_printf(m, "FQ_BLEN    : %08x\n", mtk_r32(eth, reg_map->qdma.fq_blen));
	seq_printf(m, "FC_TH      : %08x\n", mtk_r32(eth, reg_map->qdma.fc_th));
	seq_printf(m, "TX_SCH_RATE: %08x\n",
		   mtk_r32(eth, reg_map->qdma.tx_sch_rate));

	seq_puts(m, "\n=== Per-Queue QTX_CFG/QTX_SCH ===\n");
	for (i = 0; i < MTK_QDMA_NUM_QUEUES; i++) {
		val = mtk_r32(eth, reg_map->qdma.qtx_cfg + i * MTK_QTX_OFFSET);
		seq_printf(m, "Queue%02d CFG: %08x  SCH: %08x\n", i, val,
			   mtk_r32(eth, reg_map->qdma.qtx_sch + i * MTK_QTX_OFFSET));
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt7988_qdma_status);

static int mt7988_adma_status_show(struct seq_file *m, void *private)
{
	struct mtk_eth *eth = m->private;
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	int i;

	seq_puts(m, "=== ADMA Status ===\n");
	seq_printf(m, "GLO_CFG    : %08x\n", mtk_r32(eth, reg_map->adma.glo_cfg));
	seq_printf(m, "RX_CFG     : %08x\n", mtk_r32(eth, reg_map->adma.rx_cfg));
	seq_printf(m, "DELAY_IRQ  : %08x\n",
		   mtk_r32(eth, reg_map->adma.delay_irq));
	seq_printf(m, "IRQ_STATUS : %08x\n",
		   mtk_r32(eth, reg_map->adma.irq_status));
	seq_printf(m, "IRQ_MASK   : %08x\n", mtk_r32(eth, reg_map->adma.irq_mask));
	seq_printf(m, "RSS_GLO_CFG: %08x\n",
		   mtk_r32(eth, reg_map->adma.rss_glo_cfg));

	for (i = 0; i < MTK_MAX_RX_RING_NUM; i++) {
		u32 base = reg_map->adma.rx_ptr + i * MTK_QRX_OFFSET;
		if (!eth->rx_ring[i].dma)
			continue;
		seq_printf(m, "\nRing%d:\n", i);
		seq_printf(m, " BASE_PTR : %08x\n", mtk_r32(eth, base));
		seq_printf(m, " MAX_CNT  : %d\n", eth->rx_ring[i].dma_size);
		seq_printf(m, " CRX_IDX  : %08x\n", mtk_r32(eth, base + 0x08));
		seq_printf(m, " DRX_IDX  : %08x\n", mtk_r32(eth, base + 0x0c));
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt7988_adma_status);

static int mt7988_pse_status_show(struct seq_file *m, void *private)
{
	struct mtk_eth *eth = m->private;
	int i;

	seq_puts(m, "=== PSE Status ===\n");
	seq_printf(m, "PSE_FQFC_CFG : %08x\n", mtk_r32(eth, MTK_PSE_FQFC_CFG));
	seq_printf(m, "PSE_FQFC_CFG2: %08x\n", mtk_r32(eth, PSE_FQFC_CFG2));
	seq_printf(m, "PSE_DROP_CFG : %08x\n", mtk_r32(eth, PSE_DROP_CFG));

	seq_puts(m, "\nPSE Input Queue Status:\n");
	for (i = 0; i < 8; i++)
		seq_printf(m, " IQ_STA%d: %08x\n", i,
			   mtk_r32(eth, MTK_PSE_IQ_STA(i)));

	seq_puts(m, "\nPSE Output Queue Status:\n");
	for (i = 0; i < 8; i++)
		seq_printf(m, " OQ_STA%d: %08x\n", i,
			   mtk_r32(eth, MTK_PSE_OQ_STA(i)));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt7988_pse_status);

static int mt7988_netsys_dump_show(struct seq_file *m, void *private)
{
	struct mtk_eth *eth = m->private;

	seq_puts(m, "=== NetSys FSM Dump ===\n");
	seq_printf(m, "FE_GLO_CFG(0): %08x\n", mtk_r32(eth, MTK_FE_GLO_CFG(0)));
	seq_printf(m, "FE_GLO_CFG(2): %08x\n", mtk_r32(eth, MTK_FE_GLO_CFG(2)));
	seq_printf(m, "FE_GLO_MISC  : %08x\n", mtk_r32(eth, MTK_FE_GLO_MISC));
	seq_printf(m, "FE_CDM1_FSM  : %08x\n", mtk_r32(eth, MTK_FE_CDM1_FSM));
	seq_printf(m, "FE_CDM2_FSM  : %08x\n", mtk_r32(eth, MTK_FE_CDM2_FSM));
	seq_printf(m, "FE_CDM3_FSM  : %08x\n", mtk_r32(eth, MTK_FE_CDM3_FSM));
	seq_printf(m, "FE_CDM4_FSM  : %08x\n", mtk_r32(eth, MTK_FE_CDM4_FSM));
	seq_printf(m, "FE_CDM5_FSM  : %08x\n", mtk_r32(eth, MTK_FE_CDM5_FSM));
	seq_printf(m, "FE_CDM6_FSM  : %08x\n", mtk_r32(eth, MTK_FE_CDM6_FSM));
	seq_printf(m, "FE_GDM1_FSM  : %08x\n", mtk_r32(eth, MTK_FE_GDM1_FSM));
	seq_printf(m, "FE_GDM2_FSM  : %08x\n", mtk_r32(eth, MTK_FE_GDM2_FSM));
	seq_printf(m, "FE_GDM3_FSM  : %08x\n", mtk_r32(eth, MTK_FE_GDM3_FSM));
	seq_printf(m, "FE_PSE_FREE  : %08x\n", mtk_r32(eth, MTK_FE_PSE_FREE));
	seq_printf(m, "FE_DROP_FQ   : %08x\n", mtk_r32(eth, MTK_FE_DROP_FQ));
	seq_printf(m, "FE_DROP_FC   : %08x\n", mtk_r32(eth, MTK_FE_DROP_FC));
	seq_printf(m, "FE_DROP_PPE  : %08x\n", mtk_r32(eth, MTK_FE_DROP_PPE));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt7988_netsys_dump);

static int mt7988_reg_dump_show(struct seq_file *m, void *private)
{
	struct mtk_eth *eth = m->private;
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	int i;

	seq_puts(m, "=== FE Register Dump ===\n");
	seq_printf(m, "FE_GLO_CFG(0): %08x\n", mtk_r32(eth, 0x00));
	seq_printf(m, "FE_GLO_CFG(2): %08x\n", mtk_r32(eth, 0x24));
	seq_printf(m, "RST_GL       : %08x\n", mtk_r32(eth, MTK_RST_GL));
	seq_printf(m, "FE_INT_STATUS: %08x\n", mtk_r32(eth, MTK_FE_INT_STATUS));
	seq_printf(m, "FE_INT_STATUS2: %08x\n", mtk_r32(eth, MTK_FE_INT_STATUS2));
	seq_printf(m, "FE_INT_ENABLE: %08x\n", mtk_r32(eth, MTK_FE_INT_ENABLE));
	seq_printf(m, "FE_INT_GRP   : %08x\n", mtk_r32(eth, MTK_FE_INT_GRP));
	seq_printf(m, "FE_GLO_MISC  : %08x\n", mtk_r32(eth, MTK_FE_GLO_MISC));

	seq_puts(m, "\n=== GDMA Registers ===\n");
	for (i = 0; i < MTK_GMAC_ID_MAX; i++) {
		seq_printf(m, "GDM%d_FWD_CFG : %08x\n", i + 1,
			   mtk_r32(eth, MTK_GDMA_FWD_CFG(i)));
		seq_printf(m, "GDM%d_EG_CTRL : %08x\n", i + 1,
			   mtk_r32(eth, MTK_GDMA_EG_CTRL(i)));
		seq_printf(m, "GDM%d_MAC_ADRL: %08x\n", i + 1,
			   mtk_r32(eth, MTK_GDMA_MAC_ADRL(i)));
		seq_printf(m, "GDM%d_MAC_ADRH: %08x\n", i + 1,
			   mtk_r32(eth, MTK_GDMA_MAC_ADRH(i)));
	}

	seq_puts(m, "\n=== MAC Registers ===\n");
	for (i = 0; i < MTK_GMAC_ID_MAX; i++) {
		seq_printf(m, "MAC%d_MCR     : %08x\n", i + 1,
			   mtk_r32(eth, MTK_MAC_MCR(i)));
		seq_printf(m, "MAC%d_MSR     : %08x\n", i + 1,
			   mtk_r32(eth, MTK_MAC_MSR(i)));
		seq_printf(m, "MAC%d_FSM     : %08x\n", i + 1,
			   mtk_r32(eth, MTK_MAC_FSM(i)));
	}

	seq_puts(m, "\n=== XMAC Registers ===\n");
	for (i = MTK_GMAC2_ID; i < MTK_GMAC_ID_MAX; i++) {
		seq_printf(m, "XMAC%d_MCR    : %08x\n", i,
			   mtk_r32(eth, MTK_XMAC_MCR(i)));
		seq_printf(m, "XMAC%d_STS    : %08x\n", i,
			   mtk_r32(eth, MTK_XGMAC_STS(i)));
	}

	seq_puts(m, "\n=== QDMA Registers ===\n");
	seq_printf(m, "QDMA_GLO_CFG : %08x\n", mtk_r32(eth, reg_map->qdma.glo_cfg));
	seq_printf(m, "QDMA_RST_IDX : %08x\n", mtk_r32(eth, reg_map->qdma.rst_idx));
	seq_printf(m, "QDMA_DELAY_IRQ: %08x\n",
		   mtk_r32(eth, reg_map->qdma.delay_irq));
	seq_printf(m, "QDMA_FC_TH   : %08x\n", mtk_r32(eth, reg_map->qdma.fc_th));
	seq_printf(m, "QDMA_INT_GRP : %08x\n", mtk_r32(eth, reg_map->qdma.int_grp));
	seq_printf(m, "QDMA_HRED    : %08x\n", mtk_r32(eth, reg_map->qdma.hred));

	seq_puts(m, "\n=== ADMA Registers ===\n");
	seq_printf(m, "ADMA_GLO_CFG : %08x\n", mtk_r32(eth, reg_map->adma.glo_cfg));
	seq_printf(m, "ADMA_RST_IDX : %08x\n", mtk_r32(eth, reg_map->adma.rst_idx));
	seq_printf(m, "ADMA_DELAY_IRQ: %08x\n",
		   mtk_r32(eth, reg_map->adma.delay_irq));
	seq_printf(m, "ADMA_IRQ_STATUS: %08x\n",
		   mtk_r32(eth, reg_map->adma.irq_status));
	seq_printf(m, "ADMA_IRQ_MASK: %08x\n", mtk_r32(eth, reg_map->adma.irq_mask));
	seq_printf(m, "ADMA_INT_GRP : %08x\n", mtk_r32(eth, reg_map->adma.int_grp));
	seq_printf(m, "ADMA_INT_GRP2: %08x\n", mtk_r32(eth, reg_map->adma.int_grp2));
	seq_printf(m, "ADMA_INT_GRP3: %08x\n", mtk_r32(eth, reg_map->adma.int_grp3));
	seq_printf(m, "ADMA_LRO_CTRL: %08x\n",
		   mtk_r32(eth, reg_map->adma.lro_ctrl_dw0));
	seq_printf(m, "ADMA_RX_CFG  : %08x\n", mtk_r32(eth, reg_map->adma.rx_cfg));
	seq_printf(m, "ADMA_RSS_GLO : %08x\n",
		   mtk_r32(eth, reg_map->adma.rss_glo_cfg));

	seq_puts(m, "\n=== PSE Registers ===\n");
	seq_printf(m, "PSE_FQFC_CFG : %08x\n", mtk_r32(eth, MTK_PSE_FQFC_CFG));
	seq_printf(m, "PSE_FQFC_CFG2: %08x\n", mtk_r32(eth, PSE_FQFC_CFG2));
	seq_printf(m, "PSE_DROP_CFG : %08x\n", mtk_r32(eth, PSE_DROP_CFG));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt7988_reg_dump);

static int mt7988_hw_lro_settings_show(struct seq_file *m, void *private)
{
	struct mtk_eth *eth = m->private;
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	u32 agg_cnt, agg_time, age_time;
	u32 reg_op1, reg_op2, reg_op3;
	int i;

	seq_puts(m, "HW LRO Ring Settings\n");

	for (i = 0; i < MTK_HW_LRO_RING_NUM; i++) {
		reg_op1 = mtk_r32(eth, MTK_LRO_CTRL_DW1_CFG(i));
		reg_op2 = mtk_r32(eth, MTK_LRO_CTRL_DW2_CFG(i));
		reg_op3 = mtk_r32(eth, MTK_LRO_CTRL_DW3_CFG(i));

		agg_cnt = ((reg_op3 & 0x3) << 6) |
			  ((reg_op2 >> MTK_LRO_RING_AGG_CNT_L_OFFSET) & 0x3f);
		agg_time = (reg_op2 >> MTK_LRO_RING_AGG_TIME_OFFSET) & 0xffff;
		age_time = ((reg_op2 & 0x3f) << 10) |
			   ((reg_op1 >> MTK_LRO_RING_AGE_TIME_L_OFFSET) & 0x3ff);

		seq_printf(m,
			   "Ring[%d]: MAX_AGG_CNT=%d, AGG_TIME=%d, AGE_TIME=%d\n",
			   i + 4, agg_cnt, agg_time, age_time);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt7988_hw_lro_settings);

void mt7988_hw_lro_stats_update(struct mtk_eth *eth, u32 ring_no,
				struct mtk_rx_dma_v2 *rxd)
{
	u32 idx, agg_cnt, agg_size;

	idx = ring_no - 4;
	agg_cnt = RX_DMA_GET_AGG_CNT_V2(rxd->rxd6);

	if (idx >= MTK_HW_LRO_RING_NUM)
		return;

	agg_size = RX_DMA_GET_PLEN0(rxd->rxd2);

	hw_lro_agg_size_cnt[idx][agg_size / 5000]++;
	hw_lro_agg_num_cnt[idx][agg_cnt]++;
	hw_lro_tot_flush_cnt[idx]++;
	hw_lro_tot_agg_cnt[idx] += agg_cnt;
}

void mt7988_hw_lro_flush_stats_update(struct mtk_eth *eth, u32 ring_no,
				      struct mtk_rx_dma_v2 *rxd)
{
	u32 idx, flush_reason;

	idx = ring_no - 4;
	flush_reason = RX_DMA_GET_FLUSH_RSN_V2(rxd->rxd6);

	if (idx >= MTK_HW_LRO_RING_NUM)
		return;

	if ((flush_reason & 0x7) == MTK_HW_LRO_AGG_FLUSH)
		hw_lro_agg_flush_cnt[idx]++;
	else if ((flush_reason & 0x7) == MTK_HW_LRO_AGE_FLUSH)
		hw_lro_age_flush_cnt[idx]++;
	else if ((flush_reason & 0x7) == MTK_HW_LRO_NOT_IN_SEQ_FLUSH)
		hw_lro_seq_flush_cnt[idx]++;
	else if ((flush_reason & 0x7) == MTK_HW_LRO_TIMESTAMP_FLUSH)
		hw_lro_timestamp_flush_cnt[idx]++;
	else if ((flush_reason & 0x7) == MTK_HW_LRO_NON_RULE_FLUSH)
		hw_lro_norule_flush_cnt[idx]++;
}

static int mt7988_rss_ctrl_show(struct seq_file *m, void *private)
{
	struct mtk_eth *eth = m->private;

	seq_printf(m, "RSS_GLO_CFG: %08x\n",
		   mtk_r32(eth, eth->soc->reg_map->adma.rss_glo_cfg));
	return 0;
}

static ssize_t mt7988_rss_ctrl_write(struct file *file,
				     const char __user *buffer,
				     size_t count, loff_t *data)
{
	struct mtk_eth *eth = file->private_data;
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	char buf[32];
	char *p_buf, *p_token;
	long num;
	int ret;
	struct mtk_rss_params *rss_params = &eth->rss_params;
	u32 i;

	if (count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, buffer, count))
		return -EFAULT;

	buf[count] = '\0';
	p_buf = buf;
	p_token = strsep(&p_buf, " \t");
	if (!p_token)
		return -EINVAL;

	ret = kstrtol(p_token, 10, &num);
	if (ret)
		return ret;

	if (num <= 0 || num > MTK_RX_NAPI_NUM)
		return -EINVAL;

	for (i = 0; i < MTK_RSS_MAX_INDIRECTION_TABLE; i++)
		rss_params->indirection_table[i] = i % num;

	for (i = 0; i < MTK_RSS_MAX_INDIRECTION_TABLE / 16; i++)
		mtk_w32(eth, mtk_rss_indr_table(rss_params, i),
			MTK_RSS_INDR_TABLE_DW(i));

	return count;
}

static int mt7988_rss_ctrl_open(struct inode *inode, struct file *file)
{
	return single_open(file, mt7988_rss_ctrl_show, inode->i_private);
}

static const struct file_operations mt7988_rss_ctrl_fops = {
	.owner = THIS_MODULE,
	.open = mt7988_rss_ctrl_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = mt7988_rss_ctrl_write,
	.release = single_release,
};

void mt7988_eth_debugfs_exit(struct mtk_eth *eth)
{
	debugfs_remove_recursive(mt7988_debugfs_root);
	mt7988_debugfs_root = NULL;
}

int mt7988_eth_debugfs_init(struct mtk_eth *eth)
{
	mt7988_debugfs_root = debugfs_create_dir("mt7988_eth", NULL);
	if (!mt7988_debugfs_root)
		return -ENOMEM;

	debugfs_create_file("dbg_regs", 0444, mt7988_debugfs_root, eth,
			    &mt7988_dbg_regs_fops);
	debugfs_create_file("tx_ring", 0444, mt7988_debugfs_root, eth,
			    &mt7988_tx_ring_fops);
	debugfs_create_file("rx_ring", 0444, mt7988_debugfs_root, eth,
			    &mt7988_rx_ring_fops);
	debugfs_create_file("gdm_cnt", 0444, mt7988_debugfs_root, eth,
			    &mt7988_gdm_cnt_fops);
	debugfs_create_file("qdma_status", 0444, mt7988_debugfs_root, eth,
			    &mt7988_qdma_status_fops);
	debugfs_create_file("adma_status", 0444, mt7988_debugfs_root, eth,
			    &mt7988_adma_status_fops);
	debugfs_create_file("pse_status", 0444, mt7988_debugfs_root, eth,
			    &mt7988_pse_status_fops);
	debugfs_create_file("netsys_dump", 0444, mt7988_debugfs_root, eth,
			    &mt7988_netsys_dump_fops);
	debugfs_create_file("reg_dump", 0444, mt7988_debugfs_root, eth,
			    &mt7988_reg_dump_fops);
	debugfs_create_file("hw_lro_stats", 0644, mt7988_debugfs_root, eth,
			    &mt7988_hw_lro_stats_fops);
	debugfs_create_file("hw_lro_settings", 0444, mt7988_debugfs_root, eth,
			    &mt7988_hw_lro_settings_fops);
	debugfs_create_file("reset_event", 0644, mt7988_debugfs_root, eth,
			    &mt7988_reset_event_fops);
	debugfs_create_file("rss_ctrl", 0644, mt7988_debugfs_root, eth,
			    &mt7988_rss_ctrl_fops);
	debugfs_create_u32("show_level", 0644, mt7988_debugfs_root,
			   &dbg_show_level);

	dbg_show_level = 1;

	return 0;
}
