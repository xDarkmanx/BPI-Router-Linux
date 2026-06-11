// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT7988 QDMA HTB offload
 *
 * Hardware tc HTB queue discipline offload via QDMA scheduler.
 * Maps HTB leaf classes to QDMA TX queues (0-15) with per-queue
 * min/max rate shaping via QTX_SCH registers.
 *
 * Rate encoding: rate = MAN * 10^EXP kbps
 *   MAN: 7 bits (1-127)
 *   EXP: 4 bits (0=1K, 1=10K, 2=100K, 3=1M, 4=10M, 5=100M, >=6=1G)
 *
 * Copyright (c) 2024-2026 Semenets V. Pavel <p.semenets@gmail.com>
 */

#include <linux/rhashtable.h>
#include <net/pkt_cls.h>
#include <net/sock.h>

#include "mt7988_eth.h"

#define MTK_HTB_CLASSID_ROOT U32_MAX

struct mtk_htb_node {
	struct rhash_head hnode;
	struct mtk_htb_node *parent;
	u32 classid;
	u16 qid;
	u64 rate;
	u64 ceil;
};

struct mtk_htb {
	struct rhashtable node_table;
	DECLARE_BITMAP(used_qids, MTK_QDMA_NUM_QUEUES);
	struct mtk_eth *eth;
	struct net_device *dev;
	bool active;
};

static const struct rhashtable_params mtk_htb_params = {
	.key_offset = offsetof(struct mtk_htb_node, classid),
	.key_len = sizeof(u32),
	.head_offset = offsetof(struct mtk_htb_node, hnode),
	.automatic_shrinking = true,
};

static struct mtk_htb_node *mtk_htb_node_find(struct mtk_htb *htb, u32 classid)
{
	return rhashtable_lookup_fast(&htb->node_table, &classid, mtk_htb_params);
}

static int mtk_htb_find_free_qid(struct mtk_htb *htb)
{
	int qid;

	qid = find_first_zero_bit(htb->used_qids, MTK_QDMA_NUM_QUEUES);
	if (qid >= MTK_QDMA_NUM_QUEUES)
		return -ENOSPC;

	__set_bit(qid, htb->used_qids);
	return qid;
}

static struct mtk_htb_node *
mtk_htb_node_create(struct mtk_htb *htb, u32 classid, u16 qid,
		     struct mtk_htb_node *parent)
{
	struct mtk_htb_node *node;
	int err;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return ERR_PTR(-ENOMEM);

	node->classid = classid;
	node->qid = qid;
	node->parent = parent;

	err = rhashtable_insert_fast(&htb->node_table, &node->hnode,
				     mtk_htb_params);
	if (err) {
		kfree(node);
		return ERR_PTR(err);
	}

	return node;
}

static void mtk_htb_node_delete(struct mtk_htb *htb, struct mtk_htb_node *node)
{
	if (node->qid < MTK_QDMA_NUM_QUEUES)
		__clear_bit(node->qid, htb->used_qids);

	rhashtable_remove_fast(&htb->node_table, &node->hnode, mtk_htb_params);
	kfree(node);
}

static void mtk_rate_to_man_exp(u64 rate_bytes, u32 *man, u32 *exp)
{
	u64 rate_bps = rate_bytes * 8;
	u64 rate_kbps = div_u64(rate_bps, 1000);
	u32 e;

	if (rate_kbps == 0) {
		*man = 0;
		*exp = 0;
		return;
	}

	for (e = 0; e <= 5; e++) {
		u64 base = 1;
		u64 m;
		u32 i;

		for (i = 0; i < e; i++)
			base *= 10;

		m = div64_u64(rate_kbps, base);
		if (m > 0 && m <= 127) {
			*man = (u32)m;
			*exp = e;
			return;
		}
	}

	if (rate_kbps > 127000000) {
		u64 m = div64_u64(rate_kbps, 1000000000);
		*man = min_t(u64, m, 127);
		*exp = 6;
		return;
	}

	*man = 127;
	*exp = 5;
}

static void mtk_htb_write_qtx_sch(struct mtk_eth *eth, int qid,
				   u64 rate_bytes, u64 ceil_bytes)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	u32 val, ofs, reg;
	u32 man, exp;

	val = MTK_QTX_SCH_LEAKY_BUCKET_SIZE;

	if (rate_bytes > 0) {
		mtk_rate_to_man_exp(rate_bytes, &man, &exp);
		val |= MTK_QTX_SCH_MIN_RATE_EN |
		       FIELD_PREP(MTK_QTX_SCH_MIN_RATE_MAN, man) |
		       FIELD_PREP(MTK_QTX_SCH_MIN_RATE_EXP, exp);
	}

	if (ceil_bytes > 0) {
		mtk_rate_to_man_exp(ceil_bytes, &man, &exp);
		val |= MTK_QTX_SCH_MAX_RATE_EN |
		       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_MAN, man) |
		       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_EXP, exp) |
		       FIELD_PREP(MTK_QTX_SCH_MAX_RATE_WEIGHT, 1);
	}

	ofs = MTK_QTX_OFFSET * qid;
	reg = reg_map->qdma.qtx_sch + ofs;
	mtk_w32(eth, val, reg);
}

static void mtk_htb_clear_qtx_sch(struct mtk_eth *eth, int qid)
{
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	u32 ofs;

	ofs = MTK_QTX_OFFSET * qid;
	mtk_w32(eth, 0, reg_map->qdma.qtx_sch + ofs);
}

static int mtk_htb_root_add(struct mtk_htb *htb, u16 htb_maj_id,
			     u16 htb_defcls, struct netlink_ext_ack *extack)
{
	struct mtk_htb_node *root;
	int err;

	err = rhashtable_init(&htb->node_table, &mtk_htb_params);
	if (err)
		return err;

	root = kzalloc(sizeof(*root), GFP_KERNEL);
	if (!root) {
		rhashtable_destroy(&htb->node_table);
		return -ENOMEM;
	}

	root->classid = MTK_HTB_CLASSID_ROOT;
	root->qid = MTK_QDMA_NUM_QUEUES;

	err = rhashtable_insert_fast(&htb->node_table, &root->hnode,
				     mtk_htb_params);
	if (err) {
		kfree(root);
		rhashtable_destroy(&htb->node_table);
		return err;
	}

	htb->active = true;
	htb->eth->htb_active = true;
	return 0;
}

static int mtk_htb_root_del(struct mtk_htb *htb)
{
	struct mtk_htb_node *root;
	int qid;

	for (qid = 0; qid < MTK_QDMA_NUM_QUEUES; qid++) {
		if (test_bit(qid, htb->used_qids))
			mtk_htb_clear_qtx_sch(htb->eth, qid);
	}

	bitmap_zero(htb->used_qids, MTK_QDMA_NUM_QUEUES);

	root = mtk_htb_node_find(htb, MTK_HTB_CLASSID_ROOT);
	if (root) {
		rhashtable_remove_fast(&htb->node_table, &root->hnode,
				       mtk_htb_params);
		kfree(root);
	}

	rhashtable_destroy(&htb->node_table);
	htb->active = false;
	htb->eth->htb_active = false;
	return 0;
}

static int
mtk_htb_leaf_alloc_queue(struct mtk_htb *htb, u16 classid,
			  u32 parent_classid, u64 rate, u64 ceil,
			  struct netlink_ext_ack *extack)
{
	struct mtk_htb_node *node, *parent;
	int qid;

	parent = mtk_htb_node_find(htb, parent_classid);
	if (!parent)
		return -EINVAL;

	qid = mtk_htb_find_free_qid(htb);
	if (qid < 0) {
		NL_SET_ERR_MSG_MOD(extack, "No free QDMA queues");
		return qid;
	}

	node = mtk_htb_node_create(htb, classid, qid, parent);
	if (IS_ERR(node)) {
		__clear_bit(qid, htb->used_qids);
		return PTR_ERR(node);
	}

	node->rate = rate;
	node->ceil = ceil;

	mtk_htb_write_qtx_sch(htb->eth, qid, rate, ceil);

	return qid;
}

static int
mtk_htb_leaf_to_inner(struct mtk_htb *htb, u16 classid, u16 child_classid,
		       u64 rate, u64 ceil, struct netlink_ext_ack *extack)
{
	struct mtk_htb_node *node, *child;
	u16 qid;

	node = mtk_htb_node_find(htb, classid);
	if (!node)
		return -ENOENT;

	qid = node->qid;

	mtk_htb_clear_qtx_sch(htb->eth, qid);

	rhashtable_remove_fast(&htb->node_table, &node->hnode, mtk_htb_params);
	node->qid = MTK_QDMA_NUM_QUEUES;
	rhashtable_insert_fast(&htb->node_table, &node->hnode, mtk_htb_params);

	child = mtk_htb_node_create(htb, child_classid, qid, node);
	if (IS_ERR(child)) {
		node->qid = qid;
		return PTR_ERR(child);
	}

	child->rate = rate;
	child->ceil = ceil;

	mtk_htb_write_qtx_sch(htb->eth, qid, rate, ceil);

	return 0;
}

static int mtk_htb_leaf_del(struct mtk_htb *htb, u16 *classid,
			     struct netlink_ext_ack *extack)
{
	struct mtk_htb_node *node;

	node = mtk_htb_node_find(htb, *classid);
	if (!node)
		return -ENOENT;

	if (node->qid < MTK_QDMA_NUM_QUEUES)
		mtk_htb_clear_qtx_sch(htb->eth, node->qid);

	*classid = node->classid;
	mtk_htb_node_delete(htb, node);

	return 0;
}

static int mtk_htb_leaf_del_last(struct mtk_htb *htb, u16 classid,
				  struct netlink_ext_ack *extack)
{
	struct mtk_htb_node *node, *parent;
	u16 qid;

	node = mtk_htb_node_find(htb, classid);
	if (!node)
		return -ENOENT;

	qid = node->qid;

	if (qid < MTK_QDMA_NUM_QUEUES)
		mtk_htb_clear_qtx_sch(htb->eth, qid);

	parent = node->parent;
	mtk_htb_node_delete(htb, node);

	if (!parent || parent->classid == MTK_HTB_CLASSID_ROOT)
		return 0;

	parent->qid = qid;
	__set_bit(qid, htb->used_qids);
	mtk_htb_write_qtx_sch(htb->eth, qid, parent->rate, parent->ceil);

	return 0;
}

static int mtk_htb_node_modify(struct mtk_htb *htb, u16 classid,
				u64 rate, u64 ceil,
				struct netlink_ext_ack *extack)
{
	struct mtk_htb_node *node;

	node = mtk_htb_node_find(htb, classid);
	if (!node)
		return -ENOENT;

	node->rate = rate;
	node->ceil = ceil;

	if (node->qid < MTK_QDMA_NUM_QUEUES)
		mtk_htb_write_qtx_sch(htb->eth, node->qid, rate, ceil);

	return 0;
}

static int mtk_htb_leaf_query_queue(struct mtk_htb *htb, u16 classid)
{
	struct mtk_htb_node *node;

	node = mtk_htb_node_find(htb, classid);
	if (!node)
		return -ENOENT;

	if (node->qid >= MTK_QDMA_NUM_QUEUES)
		return -EINVAL;

	return node->qid;
}

int mtk_htb_setup_tc(struct mtk_eth *eth, struct net_device *dev,
		      struct tc_htb_qopt_offload *opt)
{
	struct mtk_htb *htb = eth->htb;
	int res;

	if (!htb && opt->command != TC_HTB_CREATE)
		return -EINVAL;

	switch (opt->command) {
	case TC_HTB_CREATE:
		if (htb)
			return -EEXIST;
		htb = kzalloc(sizeof(*htb), GFP_KERNEL);
		if (!htb)
			return -ENOMEM;
		htb->eth = eth;
		htb->dev = dev;
		res = mtk_htb_root_add(htb, opt->parent_classid,
					opt->classid, opt->extack);
		if (res) {
			kfree(htb);
			return res;
		}
		eth->htb = htb;
		return 0;

	case TC_HTB_DESTROY:
		mtk_htb_root_del(htb);
		kfree(htb);
		eth->htb = NULL;
		return 0;

	case TC_HTB_LEAF_ALLOC_QUEUE:
		res = mtk_htb_leaf_alloc_queue(htb, opt->classid,
						 opt->parent_classid,
						 opt->rate, opt->ceil,
						 opt->extack);
		if (res < 0)
			return res;
		opt->qid = res;
		return 0;

	case TC_HTB_LEAF_TO_INNER:
		return mtk_htb_leaf_to_inner(htb, opt->parent_classid,
					      opt->classid, opt->rate,
					      opt->ceil, opt->extack);

	case TC_HTB_LEAF_DEL:
		return mtk_htb_leaf_del(htb, &opt->classid, opt->extack);

	case TC_HTB_LEAF_DEL_LAST:
	case TC_HTB_LEAF_DEL_LAST_FORCE:
		return mtk_htb_leaf_del_last(htb, opt->classid, opt->extack);

	case TC_HTB_NODE_MODIFY:
		return mtk_htb_node_modify(htb, opt->classid, opt->rate,
					    opt->ceil, opt->extack);

	case TC_HTB_LEAF_QUERY_QUEUE:
		res = mtk_htb_leaf_query_queue(htb, opt->classid);
		if (res < 0)
			return res;
		opt->qid = res;
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}

int mtk_eth_setup_tc(struct net_device *dev, enum tc_setup_type type,
		     void *type_data)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;

	switch (type) {
#if IS_ENABLED(CONFIG_NET_MEDIATEK_MT7988) && !IS_ENABLED(CONFIG_NET_MEDIATEK_HNAT)
	case TC_SETUP_BLOCK:
	case TC_SETUP_FT:
		return mtk_flow_setup_tc(dev, type, type_data);
#endif
	case TC_SETUP_QDISC_HTB:
		return mtk_htb_setup_tc(eth, dev, type_data);
	default:
		return -EOPNOTSUPP;
	}
}
