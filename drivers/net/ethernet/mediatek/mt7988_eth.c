// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 *   Copyright (C) 2009-2016 John Crispin <blogic@openwrt.org>
 *   Copyright (C) 2009-2016 Felix Fietkau <nbd@openwrt.org>
 *   Copyright (C) 2013-2016 Michael Lee <igvtee@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/cpumask.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/jhash.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/pcs/pcs-mtk-lynxi.h>
#include <linux/pcs/pcs-standalone.h>
#include <linux/phy/phy.h>
#include <linux/phylink.h>
#include <linux/pinctrl/devinfo.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/tcp.h>
#include <net/dsa.h>
#include <net/dst_metadata.h>
#include <net/page_pool/helpers.h>

#include "mt7988_dma.h"
#include "mt7988_eth.h"
#include "mt7988_reset.h"

#include "mtk_wed.h"

static int mtk_msg_level = -1;
module_param_named(msg_level, mtk_msg_level, int, 0);
MODULE_PARM_DESC(msg_level, "Message level (-1=defaults,0=none,...,16=all)");

#define MTK_ETHTOOL_STAT(x) \
	{ #x, offsetof(struct mtk_hw_stats, x) / sizeof(u64) }

#define MTK_ETHTOOL_XDP_STAT(x) \
	{ #x, offsetof(struct mtk_hw_stats, xdp_stats.x) / sizeof(u64) }

static const struct mtk_reg_map mt7988_reg_map = {
	.tx_irq_mask		= 0x461c,
	.tx_irq_status		= 0x4618,
	.adma = {
		.rx_ptr		= 0x6900,
		.rx_cnt_cfg	= 0x6904,
		.pcrx_ptr	= 0x6908,
		.glo_cfg	= 0x6a04,
		.rst_idx	= 0x6a08,
		.delay_irq	= 0x6a0c,
		.irq_status	= 0x6a20,
		.irq_mask	= 0x6a28,
		.adma_rx_dbg0	= 0x6a38,
		.int_grp	= 0x6a50,
		.int_grp2	= 0x6a54,
		.int_grp3	= 0x6a58,
		.lro_ctrl_dw0	= 0x6c08,
		.rx_cfg = 0x6a10,
		.lro_alt_score_delta = 0x6c1c,
		.rss_delay_int = 0x6ac0,
		.lro_tx1_dly_int = 0x6a70,
		.lro_tx2_dly_int = 0x6a74,
		.lro_tx3_dly_int = 0x6a78,
		.lro_rx_ring0_dip_dw0 = 0x6c14,
		.lro_rx_ring0_ctrl_dw1 = 0x6c38,
		.lro_rx_ring0_ctrl_dw2 = 0x6c3c,
		.lro_rx_ring0_ctrl_dw3 = 0x6c40,
		.rss_glo_cfg = 0x7000,
	},
	.qdma = {
		.qtx_cfg	= 0x4400,
		.qtx_sch	= 0x4404,
		.rx_ptr		= 0x4500,
		.rx_cnt_cfg	= 0x4504,
		.qcrx_ptr	= 0x4508,
		.glo_cfg	= 0x4604,
		.rst_idx	= 0x4608,
		.delay_irq	= 0x460c,
		.fc_th		= 0x4610,
		.int_grp	= 0x4620,
		.hred		= 0x4644,
		.ctx_ptr	= 0x4700,
		.dtx_ptr	= 0x4704,
		.crx_ptr	= 0x4710,
		.drx_ptr	= 0x4714,
		.fq_head	= 0x4720,
		.fq_tail	= 0x4724,
		.fq_count	= 0x4728,
		.fq_blen	= 0x472c,
		.tx_sch_rate	= 0x4798,
	},
	.gdm1_cnt		= 0x1c00,
	.gdma_to_ppe	= {
		[0]		= 0x3333,
		[1]		= 0x4444,
		[2]		= 0xcccc,
	},
	.ppe_base		= 0x2000,
	.wdma_base = {
		[0]		= 0x4800,
		[1]		= 0x4c00,
		[2]		= 0x5000,
	},
	.pse_iq_sta		= 0x0180,
	.pse_oq_sta		= 0x01a0,
};

/* strings used by ethtool */
static const struct mtk_ethtool_stats {
	char str[ETH_GSTRING_LEN];
	u32 offset;
} mtk_ethtool_stats[] = {
	MTK_ETHTOOL_STAT(tx_bytes),
	MTK_ETHTOOL_STAT(tx_packets),
	MTK_ETHTOOL_STAT(tx_skip),
	MTK_ETHTOOL_STAT(tx_collisions),
	MTK_ETHTOOL_STAT(rx_bytes),
	MTK_ETHTOOL_STAT(rx_packets),
	MTK_ETHTOOL_STAT(rx_overflow),
	MTK_ETHTOOL_STAT(rx_fcs_errors),
	MTK_ETHTOOL_STAT(rx_short_errors),
	MTK_ETHTOOL_STAT(rx_long_errors),
	MTK_ETHTOOL_STAT(rx_checksum_errors),
	MTK_ETHTOOL_STAT(rx_flow_control_packets),
	MTK_ETHTOOL_XDP_STAT(rx_xdp_redirect),
	MTK_ETHTOOL_XDP_STAT(rx_xdp_pass),
	MTK_ETHTOOL_XDP_STAT(rx_xdp_drop),
	MTK_ETHTOOL_XDP_STAT(rx_xdp_tx),
	MTK_ETHTOOL_XDP_STAT(rx_xdp_tx_errors),
	MTK_ETHTOOL_XDP_STAT(tx_xdp_xmit),
	MTK_ETHTOOL_XDP_STAT(tx_xdp_xmit_errors),
};

static const char *const mtk_clks_source_name[] = {
	"ethif",
	"sgmiitop",
	"esw",
	"gp0",
	"gp1",
	"gp2",
	"gp3",
	"xgp1",
	"xgp2",
	"xgp3",
	"crypto",
	"fe",
	"trgpll",
	"sgmii_tx250m",
	"sgmii_rx250m",
	"sgmii_cdr_ref",
	"sgmii_cdr_fb",
	"sgmii2_tx250m",
	"sgmii2_rx250m",
	"sgmii2_cdr_ref",
	"sgmii2_cdr_fb",
	"sgmii_ck",
	"eth2pll",
	"wocpu0",
	"wocpu1",
	"netsys0",
	"netsys1",
	"ethwarp_wocpu2",
	"ethwarp_wocpu1",
	"ethwarp_wocpu0",
	"top_sgm0_sel",
	"top_sgm1_sel",
	"top_eth_gmii_sel",
	"top_eth_refck_50m_sel",
	"top_eth_sys_200m_sel",
	"top_eth_sys_sel",
	"top_eth_xgmii_sel",
	"top_eth_mii_sel",
	"top_netsys_sel",
	"top_netsys_500m_sel",
	"top_netsys_pao_2x_sel",
	"top_netsys_sync_250m_sel",
	"top_netsys_ppefb_250m_sel",
	"top_netsys_warp_sel",
};

void mtk_w32(struct mtk_eth *eth, u32 val, unsigned reg)
{
	__raw_writel(val, eth->base + reg);
}

u32 mtk_r32(struct mtk_eth *eth, unsigned reg)
{
	return __raw_readl(eth->base + reg);
}

u32 mtk_m32(struct mtk_eth *eth, u32 mask, u32 set, unsigned int reg)
{
	u32 val;

	val = mtk_r32(eth, reg);
	val &= ~mask;
	val |= set;
	mtk_w32(eth, val, reg);
	return reg;
}

static int mtk_mdio_busy_wait(struct mtk_eth *eth)
{
	unsigned long t_start = jiffies;

	while (1) {
		if (!(mtk_r32(eth, MTK_PHY_IAC) & PHY_IAC_ACCESS))
			return 0;
		if (time_after(jiffies, t_start + PHY_IAC_TIMEOUT))
			break;
		cond_resched();
	}

	dev_err(eth->dev, "mdio: MDIO timeout\n");
	return -ETIMEDOUT;
}

static int _mtk_mdio_write_c22(struct mtk_eth *eth, u32 phy_addr, u32 phy_reg,
			       u32 write_data)
{
	int ret;

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	mtk_w32(eth,
		PHY_IAC_ACCESS | PHY_IAC_START_C22 | PHY_IAC_CMD_WRITE |
			PHY_IAC_REG(phy_reg) | PHY_IAC_ADDR(phy_addr) |
			PHY_IAC_DATA(write_data),
		MTK_PHY_IAC);

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	return 0;
}

static int _mtk_mdio_write_c45(struct mtk_eth *eth, u32 phy_addr, u32 devad,
			       u32 phy_reg, u32 write_data)
{
	int ret;

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	mtk_w32(eth,
		PHY_IAC_ACCESS | PHY_IAC_START_C45 | PHY_IAC_CMD_C45_ADDR |
			PHY_IAC_REG(devad) | PHY_IAC_ADDR(phy_addr) |
			PHY_IAC_DATA(phy_reg),
		MTK_PHY_IAC);

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	mtk_w32(eth,
		PHY_IAC_ACCESS | PHY_IAC_START_C45 | PHY_IAC_CMD_WRITE |
			PHY_IAC_REG(devad) | PHY_IAC_ADDR(phy_addr) |
			PHY_IAC_DATA(write_data),
		MTK_PHY_IAC);

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	return 0;
}

static int _mtk_mdio_read_c22(struct mtk_eth *eth, u32 phy_addr, u32 phy_reg)
{
	int ret;

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	mtk_w32(eth,
		PHY_IAC_ACCESS | PHY_IAC_START_C22 | PHY_IAC_CMD_C22_READ |
			PHY_IAC_REG(phy_reg) | PHY_IAC_ADDR(phy_addr),
		MTK_PHY_IAC);

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	return mtk_r32(eth, MTK_PHY_IAC) & PHY_IAC_DATA_MASK;
}

static int _mtk_mdio_read_c45(struct mtk_eth *eth, u32 phy_addr, u32 devad,
			      u32 phy_reg)
{
	int ret;

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	mtk_w32(eth,
		PHY_IAC_ACCESS | PHY_IAC_START_C45 | PHY_IAC_CMD_C45_ADDR |
			PHY_IAC_REG(devad) | PHY_IAC_ADDR(phy_addr) |
			PHY_IAC_DATA(phy_reg),
		MTK_PHY_IAC);

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	mtk_w32(eth,
		PHY_IAC_ACCESS | PHY_IAC_START_C45 | PHY_IAC_CMD_C45_READ |
			PHY_IAC_REG(devad) | PHY_IAC_ADDR(phy_addr),
		MTK_PHY_IAC);

	ret = mtk_mdio_busy_wait(eth);
	if (ret < 0)
		return ret;

	return mtk_r32(eth, MTK_PHY_IAC) & PHY_IAC_DATA_MASK;
}

static int mtk_mdio_write_c22(struct mii_bus *bus, int phy_addr, int phy_reg,
			      u16 val)
{
	struct mtk_eth *eth = bus->priv;

	return _mtk_mdio_write_c22(eth, phy_addr, phy_reg, val);
}

static int mtk_mdio_write_c45(struct mii_bus *bus, int phy_addr, int devad,
			      int phy_reg, u16 val)
{
	struct mtk_eth *eth = bus->priv;

	return _mtk_mdio_write_c45(eth, phy_addr, devad, phy_reg, val);
}

static int mtk_mdio_read_c22(struct mii_bus *bus, int phy_addr, int phy_reg)
{
	struct mtk_eth *eth = bus->priv;

	return _mtk_mdio_read_c22(eth, phy_addr, phy_reg);
}

static int mtk_mdio_read_c45(struct mii_bus *bus, int phy_addr, int devad,
			     int phy_reg)
{
	struct mtk_eth *eth = bus->priv;

	return _mtk_mdio_read_c45(eth, phy_addr, devad, phy_reg);
}

static void mtk_setup_bridge_switch(struct mtk_eth *eth)
{
	/* Force Port1 XGMAC Link Up */
	mtk_m32(eth, 0, MTK_XGMAC_FORCE_MODE(MTK_GMAC1_ID),
		MTK_XGMAC_STS(MTK_GMAC1_ID));

	/* Adjust GSW bridge IPG to 11 */
	mtk_m32(eth, GSWTX_IPG_MASK | GSWRX_IPG_MASK,
		(GSW_IPG_11 << GSWTX_IPG_SHIFT) |
			(GSW_IPG_11 << GSWRX_IPG_SHIFT),
		MTK_GSW_CFG);
}

static bool mtk_check_gmac23_idle(struct mtk_mac *mac)
{
	u32 mac_fsm, gdm_fsm;

	mac_fsm = mtk_r32(mac->hw, MTK_MAC_FSM(mac->id));

	switch (mac->id) {
	case MTK_GMAC2_ID:
		gdm_fsm = mtk_r32(mac->hw, MTK_FE_GDM2_FSM);
		break;
	case MTK_GMAC3_ID:
		gdm_fsm = mtk_r32(mac->hw, MTK_FE_GDM3_FSM);
		break;
	default:
		return true;
	};

	if ((mac_fsm & 0xFFFF0000) == 0x01010000 &&
	    (gdm_fsm & 0xFFFF0000) == 0x00000000)
		return true;

	return false;
}

static struct phylink_pcs *mtk_mac_select_pcs(struct phylink_config *config,
					      phy_interface_t interface)
{
	struct mtk_mac *mac =
		container_of(config, struct mtk_mac, phylink_config);

	switch (interface) {
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_2500BASEX:
	case PHY_INTERFACE_MODE_SGMII:
		return mac->sgmii_pcs;
	case PHY_INTERFACE_MODE_5GBASER:
	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_USXGMII:
		return mac->usxgmii_pcs;
	default:
		return NULL;
	}
}

static int mtk_mac_prepare(struct phylink_config *config, unsigned int mode,
			   phy_interface_t iface)
{
	struct mtk_mac *mac =
		container_of(config, struct mtk_mac, phylink_config);
	u32 val;

	if (mac->id != MTK_GMAC1_ID) {
		mtk_m32(mac->hw, XMAC_MCR_TRX_DISABLE, XMAC_MCR_TRX_DISABLE,
			MTK_XMAC_MCR(mac->id));

		val = mtk_r32(mac->hw, MTK_XGMAC_STS(mac->id));
		val |= MTK_XGMAC_FORCE_MODE(mac->id);
		val &= ~MTK_XGMAC_FORCE_LINK(mac->id);
		mtk_w32(mac->hw, val, MTK_XGMAC_STS(mac->id));
	}

	return 0;
}

static void mtk_mac_config(struct phylink_config *config, unsigned int mode,
			   const struct phylink_link_state *state)
{
	struct mtk_mac *mac =
		container_of(config, struct mtk_mac, phylink_config);
	struct mtk_eth *eth = mac->hw;
	int val, err = 0;

	if (mac->interface != state->interface) {
		/* Setup soc pin functions */
		switch (state->interface) {
		case PHY_INTERFACE_MODE_1000BASEX:
		case PHY_INTERFACE_MODE_2500BASEX:
		case PHY_INTERFACE_MODE_SGMII:
			err = mtk_gmac_sgmii_path_setup(eth, mac->id);
			if (err)
				goto init_err;
			break;
		case PHY_INTERFACE_MODE_USXGMII:
		case PHY_INTERFACE_MODE_10GBASER:
		case PHY_INTERFACE_MODE_5GBASER:
			err = mtk_gmac_usxgmii_path_setup(eth, mac->id);
			if (err)
				goto init_err;
			break;
		case PHY_INTERFACE_MODE_INTERNAL:
			if (mac->id == MTK_GMAC2_ID) {
				err = mtk_gmac_2p5gphy_path_setup(eth, mac->id);
				if (err)
					goto init_err;
			}
			break;
		default:
			goto err_phy;
		}
	}

	/* SGMII */
	if (state->interface == PHY_INTERFACE_MODE_SGMII ||
	    phy_interface_mode_is_8023z(state->interface)) {
		/* The path GMAC to SGMII will be enabled once the SGMIISYS is
		 * being setup done.
		 */
		regmap_read(eth->ethsys, ETHSYS_SYSCFG0, &val);

		regmap_update_bits(eth->ethsys, ETHSYS_SYSCFG0,
				   SYSCFG0_SGMII_MASK,
				   ~(u32)SYSCFG0_SGMII_MASK);

		/* Save the syscfg0 value for mac_finish */
		mac->syscfg0 = val;
	} else if (state->interface != PHY_INTERFACE_MODE_USXGMII &&
		   state->interface != PHY_INTERFACE_MODE_10GBASER &&
		   state->interface != PHY_INTERFACE_MODE_5GBASER &&
		   phylink_autoneg_inband(mode)) {
		dev_err(eth->dev,
			"In-band mode not supported in non-SerDes modes!\n");
		return;
	}

	/* Setup gmac */
	if (mtk_interface_mode_is_xgmii(state->interface)) {
		mtk_w32(mac->hw, MTK_GDMA_XGDM_SEL, MTK_GDMA_EG_CTRL(mac->id));
		mtk_w32(mac->hw, MAC_MCR_FORCE_LINK_DOWN, MTK_MAC_MCR(mac->id));

		if (mac->id == MTK_GMAC1_ID)
			mtk_setup_bridge_switch(eth);
	} else {
		mtk_w32(eth, 0, MTK_GDMA_EG_CTRL(mac->id));

		/* FIXME: In current hardware design, we have to reset FE
		 * when switching XGDM to GDM. Therefore, here trigger an SER
		 * to let GDM go back to the initial state.
		 */
		if ((mtk_interface_mode_is_xgmii(mac->interface) ||
		     mac->interface == PHY_INTERFACE_MODE_NA) &&
		    !mtk_check_gmac23_idle(mac) &&
		    !test_bit(MTK_RESETTING, &eth->state))
			schedule_work(&eth->pending_work);
	}

	mac->interface = state->interface;

	return;

err_phy:
	dev_err(eth->dev, "%s: GMAC%d mode %s not supported!\n", __func__,
		mac->id, phy_modes(state->interface));
	return;

init_err:
	dev_err(eth->dev, "%s: GMAC%d mode %s err: %d!\n", __func__, mac->id,
		phy_modes(state->interface), err);
}

static int mtk_mac_finish(struct phylink_config *config, unsigned int mode,
			  phy_interface_t interface)
{
	struct mtk_mac *mac =
		container_of(config, struct mtk_mac, phylink_config);
	struct mtk_eth *eth = mac->hw;
	u32 mcr_cur, mcr_new;

	/* Enable SGMII */
	if (interface == PHY_INTERFACE_MODE_SGMII ||
	    phy_interface_mode_is_8023z(interface))
		regmap_update_bits(eth->ethsys, ETHSYS_SYSCFG0,
				   SYSCFG0_SGMII_MASK, mac->syscfg0);

	/* Setup gmac */
	mcr_cur = mtk_r32(mac->hw, MTK_MAC_MCR(mac->id));
	mcr_new = mcr_cur;
	mcr_new |= MAC_MCR_IPG_CFG | MAC_MCR_FORCE_MODE | MAC_MCR_BACKOFF_EN |
		   MAC_MCR_BACKPR_EN | MAC_MCR_RX_FIFO_CLR_DIS;

	/* Only update control register when needed! */
	if (mcr_new != mcr_cur)
		mtk_w32(mac->hw, mcr_new, MTK_MAC_MCR(mac->id));

	return 0;
}

static void mtk_mac_link_down(struct phylink_config *config, unsigned int mode,
			      phy_interface_t interface)
{
	struct mtk_mac *mac =
		container_of(config, struct mtk_mac, phylink_config);

	if (!mtk_interface_mode_is_xgmii(interface)) {
		mtk_m32(mac->hw,
			MAC_MCR_TX_EN | MAC_MCR_RX_EN | MAC_MCR_FORCE_LINK, 0,
			MTK_MAC_MCR(mac->id));
		mtk_m32(mac->hw, MTK_XGMAC_FORCE_LINK(mac->id), 0,
			MTK_XGMAC_STS(mac->id));
	} else if (mac->id != MTK_GMAC1_ID) {
		mtk_m32(mac->hw, XMAC_MCR_TRX_DISABLE, XMAC_MCR_TRX_DISABLE,
			MTK_XMAC_MCR(mac->id));
	}
}

static void mtk_gdm_mac_link_up(struct mtk_mac *mac, struct phy_device *phy,
				unsigned int mode, phy_interface_t interface,
				int speed, int duplex, bool tx_pause,
				bool rx_pause)
{
	u32 mcr;

	mcr = mtk_r32(mac->hw, MTK_MAC_MCR(mac->id));
	mcr &= ~(MAC_MCR_SPEED_100 | MAC_MCR_SPEED_1000 | MAC_MCR_FORCE_DPX |
		 MAC_MCR_FORCE_TX_FC | MAC_MCR_FORCE_RX_FC);

	/* Configure speed */
	mac->speed = speed;
	switch (speed) {
	case SPEED_2500:
	case SPEED_1000:
		mcr |= MAC_MCR_SPEED_1000;
		break;
	case SPEED_100:
		mcr |= MAC_MCR_SPEED_100;
		break;
	}

	/* Configure duplex */
	if (duplex == DUPLEX_FULL)
		mcr |= MAC_MCR_FORCE_DPX;

	/* Configure pause modes - phylink will avoid these for half duplex */
	if (tx_pause)
		mcr |= MAC_MCR_FORCE_TX_FC;
	if (rx_pause)
		mcr |= MAC_MCR_FORCE_RX_FC;

	mcr |= MAC_MCR_TX_EN | MAC_MCR_RX_EN | MAC_MCR_FORCE_LINK;
	mtk_w32(mac->hw, mcr, MTK_MAC_MCR(mac->id));
}

static void mtk_xgdm_mac_link_up(struct mtk_mac *mac, struct phy_device *phy,
				 unsigned int mode, phy_interface_t interface,
				 int speed, int duplex, bool tx_pause,
				 bool rx_pause)
{
	u32 mcr;

	if (mac->id == MTK_GMAC1_ID)
		return;

	/* Eliminate the interference(before link-up) caused by PHY noise */
	mtk_m32(mac->hw, XMAC_LOGIC_RST, 0, MTK_XMAC_LOGIC_RST(mac->id));
	mdelay(20);
	mtk_m32(mac->hw, XMAC_GLB_CNTCLR, XMAC_GLB_CNTCLR,
		MTK_XMAC_CNT_CTRL(mac->id));

	mtk_m32(mac->hw, MTK_XGMAC_FORCE_LINK(mac->id),
		MTK_XGMAC_FORCE_LINK(mac->id), MTK_XGMAC_STS(mac->id));

	mcr = mtk_r32(mac->hw, MTK_XMAC_MCR(mac->id));
	mcr &= ~(XMAC_MCR_FORCE_TX_FC | XMAC_MCR_FORCE_RX_FC |
		 XMAC_MCR_TRX_DISABLE);
	/* Configure pause modes -
	 * phylink will avoid these for half duplex
	 */
	if (tx_pause)
		mcr |= XMAC_MCR_FORCE_TX_FC;
	if (rx_pause)
		mcr |= XMAC_MCR_FORCE_RX_FC;

	mtk_w32(mac->hw, mcr, MTK_XMAC_MCR(mac->id));
}

static void mtk_mac_link_up(struct phylink_config *config,
			    struct phy_device *phy, unsigned int mode,
			    phy_interface_t interface, int speed, int duplex,
			    bool tx_pause, bool rx_pause)
{
	struct mtk_mac *mac =
		container_of(config, struct mtk_mac, phylink_config);

	if (mtk_interface_mode_is_xgmii(interface))
		mtk_xgdm_mac_link_up(mac, phy, mode, interface, speed, duplex,
				     tx_pause, rx_pause);
	else
		mtk_gdm_mac_link_up(mac, phy, mode, interface, speed, duplex,
				    tx_pause, rx_pause);
}

static const struct phylink_mac_ops mtk_phylink_ops = {
	.mac_select_pcs = mtk_mac_select_pcs,
	.mac_prepare = mtk_mac_prepare,
	.mac_config = mtk_mac_config,
	.mac_finish = mtk_mac_finish,
	.mac_link_down = mtk_mac_link_down,
	.mac_link_up = mtk_mac_link_up,
};

static int mtk_mdio_init(struct mtk_eth *eth)
{
	unsigned int max_clk = 2500000, divider;
	struct device_node *mii_np;
	int ret;
	u32 val;

	mii_np = of_get_child_by_name(eth->dev->of_node, "mdio-bus");
	if (!mii_np) {
		dev_err(eth->dev, "no %s child node found", "mdio-bus");
		return -ENODEV;
	}

	if (!of_device_is_available(mii_np)) {
		ret = -ENODEV;
		goto err_put_node;
	}

	eth->mii_bus = devm_mdiobus_alloc(eth->dev);
	if (!eth->mii_bus) {
		ret = -ENOMEM;
		goto err_put_node;
	}

	eth->mii_bus->name = "mdio";
	eth->mii_bus->read = mtk_mdio_read_c22;
	eth->mii_bus->write = mtk_mdio_write_c22;
	eth->mii_bus->read_c45 = mtk_mdio_read_c45;
	eth->mii_bus->write_c45 = mtk_mdio_write_c45;
	eth->mii_bus->priv = eth;
	eth->mii_bus->parent = eth->dev;

	snprintf(eth->mii_bus->id, MII_BUS_ID_SIZE, "%pOFn", mii_np);

	if (!of_property_read_u32(mii_np, "clock-frequency", &val)) {
		if (val > MDC_MAX_FREQ ||
		    val < MDC_MAX_FREQ / MDC_MAX_DIVIDER) {
			dev_err(eth->dev, "MDIO clock frequency out of range");
			ret = -EINVAL;
			goto err_put_node;
		}
		max_clk = val;
	}
	divider = min_t(unsigned int, DIV_ROUND_UP(MDC_MAX_FREQ, max_clk), 63);

	/* Configure MDC Turbo Mode */
	mtk_m32(eth, 0, MISC_MDC_TURBO, MTK_MAC_MISC_V3);

	/* Configure MDC Divider */
	val = FIELD_PREP(PPSC_MDC_CFG, divider);
	mtk_m32(eth, PPSC_MDC_CFG, val, MTK_PPSC);

	dev_dbg(eth->dev, "MDC is running on %d Hz\n", MDC_MAX_FREQ / divider);

	ret = of_mdiobus_register(eth->mii_bus, mii_np);

err_put_node:
	of_node_put(mii_np);
	return ret;
}

static void mtk_mdio_cleanup(struct mtk_eth *eth)
{
	if (!eth->mii_bus)
		return;

	mdiobus_unregister(eth->mii_bus);
}

static int mtk_set_mac_address(struct net_device *dev, void *p)
{
	int ret = eth_mac_addr(dev, p);
	struct mtk_mac *mac = netdev_priv(dev);
	const char *macaddr = dev->dev_addr;

	if (ret)
		return ret;

	if (unlikely(test_bit(MTK_RESETTING, &mac->hw->state)))
		return -EBUSY;

	spin_lock_bh(&mac->hw->page_lock);
	mtk_w32(mac->hw, (macaddr[0] << 8) | macaddr[1],
		MTK_GDMA_MAC_ADRH(mac->id));
	mtk_w32(mac->hw,
		(macaddr[2] << 24) | (macaddr[3] << 16) | (macaddr[4] << 8) |
			macaddr[5],
		MTK_GDMA_MAC_ADRL(mac->id));
	spin_unlock_bh(&mac->hw->page_lock);

	return 0;
}

void mtk_stats_update_mac(struct mtk_mac *mac)
{
	struct mtk_hw_stats *hw_stats = mac->hw_stats;
	struct mtk_eth *eth = mac->hw;
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	unsigned int offs = hw_stats->reg_offset;
	u64 stats;

	u64_stats_update_begin(&hw_stats->syncp);

	hw_stats->rx_bytes += mtk_r32(mac->hw, reg_map->gdm1_cnt + offs);
	stats = mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x4 + offs);
	if (stats)
		hw_stats->rx_bytes += (stats << 32);
	hw_stats->rx_packets +=
		mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x8 + offs);
	hw_stats->rx_overflow +=
		mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x10 + offs);
	hw_stats->rx_fcs_errors +=
		mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x14 + offs);
	hw_stats->rx_short_errors +=
		mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x18 + offs);
	hw_stats->rx_long_errors +=
		mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x1c + offs);
	hw_stats->rx_checksum_errors +=
		mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x20 + offs);
	hw_stats->rx_flow_control_packets +=
		mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x24 + offs);

	hw_stats->tx_skip += mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x50 + offs);
	hw_stats->tx_collisions +=
		mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x54 + offs);
	hw_stats->tx_bytes += mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x40 + offs);
	stats = mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x44 + offs);
	if (stats)
		hw_stats->tx_bytes += (stats << 32);
	hw_stats->tx_packets +=
		mtk_r32(mac->hw, reg_map->gdm1_cnt + 0x48 + offs);

	u64_stats_update_end(&hw_stats->syncp);
}

static void mtk_get_stats64(struct net_device *dev,
			    struct rtnl_link_stats64 *storage)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_hw_stats *hw_stats = mac->hw_stats;
	unsigned int start;

	if (netif_running(dev) && netif_device_present(dev)) {
		if (spin_trylock_bh(&hw_stats->stats_lock)) {
			mtk_stats_update_mac(mac);
			spin_unlock_bh(&hw_stats->stats_lock);
		}
	}

	do {
		start = u64_stats_fetch_begin(&hw_stats->syncp);
		storage->rx_packets = hw_stats->rx_packets;
		storage->tx_packets = hw_stats->tx_packets;
		storage->rx_bytes = hw_stats->rx_bytes;
		storage->tx_bytes = hw_stats->tx_bytes;
		storage->collisions = hw_stats->tx_collisions;
		storage->rx_length_errors =
			hw_stats->rx_short_errors + hw_stats->rx_long_errors;
		storage->rx_over_errors = hw_stats->rx_overflow;
		storage->rx_crc_errors = hw_stats->rx_fcs_errors;
		storage->rx_errors = hw_stats->rx_checksum_errors;
		storage->tx_aborted_errors = hw_stats->tx_skip;
	} while (u64_stats_fetch_retry(&hw_stats->syncp, start));

	storage->tx_errors = dev->stats.tx_errors;
	storage->rx_dropped = dev->stats.rx_dropped;
	storage->tx_dropped = dev->stats.tx_dropped;
}

static netdev_tx_t mtk_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	struct mtk_tx_ring *ring = &eth->tx_ring;
	struct net_device_stats *stats = &dev->stats;
	bool gso = false;
	int tx_num;

	/* normally we can rely on the stack not calling this more than once,
	 * however we have 2 queues running on the same ring so we need to lock
	 * the ring access
	 */
	spin_lock(&eth->page_lock);

	if (unlikely(test_bit(MTK_RESETTING, &eth->state)))
		goto drop;

	tx_num = mtk_cal_txd_req(eth, skb);
	if (unlikely(atomic_read(&ring->free_count) <= tx_num)) {
		netif_tx_stop_all_queues(dev);
		netif_err(eth, tx_queued, dev,
			  "Tx Ring full when queue awake!\n");
		spin_unlock(&eth->page_lock);
		return NETDEV_TX_BUSY;
	}

	/* TSO: fill MSS info in tcp checksum field */
	if (skb_is_gso(skb)) {
		if (skb_cow_head(skb, 0)) {
			netif_warn(eth, tx_err, dev, "GSO expand head fail.\n");
			goto drop;
		}

		if (skb_shinfo(skb)->gso_type &
		    (SKB_GSO_TCPV4 | SKB_GSO_TCPV6)) {
			gso = true;
			tcp_hdr(skb)->check = htons(skb_shinfo(skb)->gso_size);
		}
	}

	if (mtk_tx_map(skb, dev, tx_num, ring, gso) < 0)
		goto drop;

	if (unlikely(atomic_read(&ring->free_count) <= ring->thresh))
		netif_tx_stop_all_queues(dev);

	spin_unlock(&eth->page_lock);

	return NETDEV_TX_OK;

drop:
	spin_unlock(&eth->page_lock);
	stats->tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static int mtk_xdp_xmit(struct net_device *dev, int num_frame,
			struct xdp_frame **frames, u32 flags)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_hw_stats *hw_stats = mac->hw_stats;
	struct mtk_eth *eth = mac->hw;
	int i, nxmit = 0;

	if (unlikely(flags & ~XDP_XMIT_FLAGS_MASK))
		return -EINVAL;

	for (i = 0; i < num_frame; i++) {
		if (mtk_xdp_submit_frame(eth, frames[i], dev, true))
			break;
		nxmit++;
	}

	u64_stats_update_begin(&hw_stats->syncp);
	hw_stats->xdp_stats.tx_xdp_xmit += nxmit;
	hw_stats->xdp_stats.tx_xdp_xmit_errors += num_frame - nxmit;
	u64_stats_update_end(&hw_stats->syncp);

	return nxmit;
}

static netdev_features_t mtk_fix_features(struct net_device *dev,
					  netdev_features_t features)
{
	if (!(features & NETIF_F_LRO)) {
		struct mtk_mac *mac = netdev_priv(dev);
		int ip_cnt = mtk_hwlro_get_ip_cnt(mac);

		if (ip_cnt) {
			netdev_info(
				dev,
				"RX flow is programmed, LRO should keep on\n");
			features |= NETIF_F_LRO;
		}
	}

	if ((features & NETIF_F_HW_VLAN_CTAG_TX) && netdev_uses_dsa(dev)) {
		netdev_info(
			dev,
			"TX vlan offload cannot be enabled when dsa is attached.\n");
		features &= ~NETIF_F_HW_VLAN_CTAG_TX;
	}

	return features;
}

static int mtk_set_features(struct net_device *dev, netdev_features_t features)
{
	netdev_features_t diff = dev->features ^ features;

	if ((diff & NETIF_F_LRO) && !(features & NETIF_F_LRO))
		mtk_hwlro_netdev_disable(dev);

	return 0;
}

static bool mtk_hw_reset_check(struct mtk_eth *eth)
{
	u32 val = mtk_r32(eth, MTK_FE_INT_STATUS);

	return (val & MTK_FE_INT_FQ_EMPTY) || (val & MTK_FE_INT_RFIFO_UF) ||
	       (val & MTK_FE_INT_RFIFO_OV) || (val & MTK_FE_INT_TSO_FAIL) ||
	       (val & MTK_FE_INT_TSO_ALIGN) || (val & MTK_FE_INT_TSO_ILLEGAL);
}

static void mtk_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;

	if (test_bit(MTK_RESETTING, &eth->state))
		return;

	if (!mtk_hw_reset_check(eth))
		return;

	eth->netdev[mac->id]->stats.tx_errors++;
	netif_err(eth, tx_err, dev, "transmit timed out\n");

	schedule_work(&eth->pending_work);
}

static irqreturn_t mtk_handle_irq_rx(int irq, void *priv)
{
	struct mtk_napi *rx_napi = priv;
	struct mtk_eth *eth = rx_napi->eth;
	struct mtk_rx_ring *ring = rx_napi->rx_ring;

	eth->rx_events++;
	if (unlikely(!(mtk_r32(eth, eth->soc->reg_map->adma.irq_status) &
		       MTK_RX_DONE_INT(ring->ring_no))))
		return IRQ_NONE;

	if (likely(napi_schedule_prep(&rx_napi->napi))) {
		mtk_rx_irq_disable(eth, MTK_RX_DONE_INT(ring->ring_no));
		__napi_schedule(&rx_napi->napi);
	}

	return IRQ_HANDLED;
}

static irqreturn_t mtk_handle_irq_tx(int irq, void *_eth)
{
	struct mtk_eth *eth = _eth;

	eth->tx_events++;
	if (likely(napi_schedule_prep(&eth->tx_napi))) {
		mtk_tx_irq_disable(eth, MTK_TX_DONE_INT);
		__napi_schedule(&eth->tx_napi);
	}

	return IRQ_HANDLED;
}

static irqreturn_t mtk_handle_fe_irq(int irq, void *_eth)
{
	struct mtk_eth *eth = _eth;
	u32 status, val;

	status = mtk_r32(eth, MTK_FE_INT_STATUS);
	pr_info("[%s] Trigger FE Misc ISR: 0x%x\n", __func__, status);

	while (status) {
		val = ffs((unsigned int)status) - 1;
		status &= ~(1 << val);

		if ((val == MTK_EVENT_TSO_FAIL) ||
		    (val == MTK_EVENT_TSO_ILLEGAL) ||
		    (val == MTK_EVENT_TSO_ALIGN) ||
		    (val == MTK_EVENT_RFIFO_OV) ||
		    (val == MTK_EVENT_RFIFO_UF))
			pr_info("[%s] Detect reset event: %s !\n", __func__,
				mtk_reset_event_name[val]);
	}
	mtk_w32(eth, 0xFFFFFFFF, MTK_FE_INT_STATUS);

	return IRQ_HANDLED;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void mtk_poll_controller(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;

	mtk_tx_irq_disable(eth, MTK_TX_DONE_INT);
	mtk_rx_irq_disable(eth, MTK_RX_DONE_INT(0));
	mtk_handle_irq_rx(eth->irq_fe[2], &eth->rx_napi[0]);
	mtk_tx_irq_enable(eth, MTK_TX_DONE_INT);
	mtk_rx_irq_enable(eth, MTK_RX_DONE_INT(0));
}
#endif

void mtk_gdm_config(struct mtk_eth *eth, u32 id, u32 config)
{
	u32 val;

	val = mtk_r32(eth, MTK_GDMA_FWD_CFG(id));

	/* default setup the forward port to send frame to ADMA */
	val &= ~0xffff;

	/* Enable RX checksum */
	val |= MTK_GDMA_ICS_EN | MTK_GDMA_TCS_EN | MTK_GDMA_UCS_EN;

	val |= config;

	if (eth->netdev[id] && netdev_uses_dsa(eth->netdev[id]))
		val |= MTK_GDMA_SPECIAL_TAG;

	mtk_w32(eth, val, MTK_GDMA_FWD_CFG(id));
}

static int mtk_open(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	struct mtk_mac *target_mac;
	int i, err, ppe_num;

	ppe_num = eth->soc->ppe_num;

	err = phylink_of_phy_connect(mac->phylink, mac->of_node, 0);
	if (err) {
		netdev_err(dev, "%s: could not attach PHY: %d\n", __func__,
			   err);
		return err;
	}

	/* we run 2 netdevs on the same dma ring so we only bring it up once */
	if (!refcount_read(&eth->dma_refcnt)) {
		const struct mtk_soc_data *soc = eth->soc;
		u32 gdm_config;

		err = mtk_start_dma(eth);
		if (err) {
			phylink_disconnect_phy(mac->phylink);
			return err;
		}

		for (i = 0; i < ARRAY_SIZE(eth->ppe); i++)
			if (eth->ppe[i]) {
				mtk_ppe_start(eth->ppe[i]);
			}

		for (i = 0; i < MTK_MAX_DEVS; i++) {
			if (!eth->netdev[i])
				continue;

			target_mac = netdev_priv(eth->netdev[i]);
			if (ppe_num >= 3 && target_mac->id == 2) {
				target_mac->ppe_idx = 2;
				gdm_config = soc->reg_map->gdma_to_ppe[2];
			} else if (ppe_num >= 2 && target_mac->id == 1) {
				target_mac->ppe_idx = 1;
				gdm_config = soc->reg_map->gdma_to_ppe[1];
			} else {
				target_mac->ppe_idx = 0;
				gdm_config = soc->reg_map->gdma_to_ppe[0];
			}
			mtk_gdm_config(eth, target_mac->id, gdm_config);
		}
		/* Reset and enable PSE */
		mtk_w32(eth, RST_GL_PSE, MTK_RST_GL);
		mtk_w32(eth, 0, MTK_RST_GL);

		napi_enable(&eth->tx_napi);
		napi_enable(&eth->rx_napi[0].napi);
		mtk_tx_irq_enable(eth, MTK_TX_DONE_INT);
		mtk_rx_irq_enable(eth, MTK_RX_DONE_INT(0));

		if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSS)) {
			for (i = 0; i < MTK_RX_RSS_NUM; i++) {
				napi_enable(
					&eth->rx_napi[MTK_RSS_RING(i)].napi);
				mtk_rx_irq_enable(
					eth, MTK_RX_DONE_INT(MTK_RSS_RING(i)));
			}
		}

		/* hwlro Removed */
		refcount_set(&eth->dma_refcnt, 1);
	} else {
		refcount_inc(&eth->dma_refcnt);
	}

	phylink_start(mac->phylink);
	netif_tx_start_all_queues(dev);

	return 0;
}

static int mtk_stop(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	int i;

	phylink_stop(mac->phylink);

	netif_tx_disable(dev);

	phylink_disconnect_phy(mac->phylink);

	/* only shutdown DMA if this is the last user */
	if (!refcount_dec_and_test(&eth->dma_refcnt))
		return 0;

	for (i = 0; i < MTK_MAX_DEVS; i++)
		mtk_gdm_config(eth, i, MTK_GDMA_DROP_ALL);

	mtk_tx_irq_disable(eth, MTK_TX_DONE_INT);
	mtk_rx_irq_disable(eth, MTK_RX_DONE_INT(0));
	napi_disable(&eth->tx_napi);
	napi_disable(&eth->rx_napi[0].napi);

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSS)) {
		for (i = 0; i < MTK_RX_RSS_NUM; i++) {
			mtk_rx_irq_disable(eth,
					   MTK_RX_DONE_INT(MTK_RSS_RING(i)));
			napi_disable(&eth->rx_napi[MTK_RSS_RING(i)].napi);
		}
	}

	/* БЛОК HW LRO УДАЛЕН */

	cancel_work_sync(&eth->rx_dim.work);
	cancel_work_sync(&eth->tx_dim.work);

	mtk_stop_dma(eth, eth->soc->reg_map->qdma.glo_cfg);
	mtk_stop_dma(eth, eth->soc->reg_map->adma.glo_cfg);

	mtk_dma_free(eth);

	for (i = 0; i < ARRAY_SIZE(eth->ppe); i++)
		if (eth->ppe[i]) {
			mtk_ppe_stop(eth->ppe[i]);
		}

	return 0;
}

static int mtk_xdp_setup(struct net_device *dev, struct bpf_prog *prog,
			 struct netlink_ext_ack *extack)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	struct bpf_prog *old_prog;
	bool need_update;

	if (eth->hwlro) {
		NL_SET_ERR_MSG_MOD(extack, "XDP not supported with HWLRO");
		return -EOPNOTSUPP;
	}

	if (dev->mtu > MTK_PP_MAX_BUF_SIZE) {
		NL_SET_ERR_MSG_MOD(extack, "MTU too large for XDP");
		return -EOPNOTSUPP;
	}

	need_update = !!eth->prog != !!prog;
	if (netif_running(dev) && need_update)
		mtk_stop(dev);

	old_prog = rcu_replace_pointer(eth->prog, prog, lockdep_rtnl_is_held());
	if (old_prog)
		bpf_prog_put(old_prog);

	if (netif_running(dev) && need_update)
		return mtk_open(dev);

	return 0;
}

static int mtk_xdp(struct net_device *dev, struct netdev_bpf *xdp)
{
	switch (xdp->command) {
	case XDP_SETUP_PROG:
		return mtk_xdp_setup(dev, xdp->prog, xdp->extack);
	default:
		return -EINVAL;
	}
}

static void ethsys_reset(struct mtk_eth *eth, u32 reset_bits)
{
	regmap_update_bits(eth->ethsys, ETHSYS_RSTCTRL, reset_bits, reset_bits);

	usleep_range(1000, 1100);
	regmap_update_bits(eth->ethsys, ETHSYS_RSTCTRL, reset_bits,
			   ~reset_bits);
	mdelay(10);
}

static void mtk_clk_disable(struct mtk_eth *eth)
{
	int clk;

	for (clk = MTK_CLK_MAX - 1; clk >= 0; clk--)
		clk_disable_unprepare(eth->clks[clk]);
}

static int mtk_clk_enable(struct mtk_eth *eth)
{
	int clk, ret;

	for (clk = 0; clk < MTK_CLK_MAX; clk++) {
		ret = clk_prepare_enable(eth->clks[clk]);
		if (ret)
			goto err_disable_clks;
	}

	return 0;

err_disable_clks:
	while (--clk >= 0)
		clk_disable_unprepare(eth->clks[clk]);

	return ret;
}

static void mtk_set_mcr_max_rx(struct mtk_mac *mac, u32 val)
{
	u32 mcr_cur, mcr_new;

	mcr_cur = mtk_r32(mac->hw, MTK_MAC_MCR(mac->id));
	mcr_new = mcr_cur & ~MAC_MCR_MAX_RX_MASK;

	if (val <= 1518)
		mcr_new |= MAC_MCR_MAX_RX(MAC_MCR_MAX_RX_1518);
	else if (val <= 1536)
		mcr_new |= MAC_MCR_MAX_RX(MAC_MCR_MAX_RX_1536);
	else if (val <= 1552)
		mcr_new |= MAC_MCR_MAX_RX(MAC_MCR_MAX_RX_1552);
	else
		mcr_new |= MAC_MCR_MAX_RX(MAC_MCR_MAX_RX_2048);

	if (mcr_new != mcr_cur)
		mtk_w32(mac->hw, mcr_new, MTK_MAC_MCR(mac->id));
}

static void mtk_hw_reset(struct mtk_eth *eth)
{
	u32 val;

	regmap_write(eth->ethsys, ETHSYS_FE_RST_CHK_IDLE_EN, 0);

	val = RSTCTRL_PPE0_V3;

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSTCTRL_PPE1))
		val |= RSTCTRL_PPE1_V3;

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSTCTRL_PPE2))
		val |= RSTCTRL_PPE2;

	val |= RSTCTRL_WDMA0 | RSTCTRL_WDMA1 | RSTCTRL_WDMA2;

	ethsys_reset(eth, RSTCTRL_ETH | RSTCTRL_FE | val);

	regmap_write(eth->ethsys, ETHSYS_FE_RST_CHK_IDLE_EN, 0x6f8ff);
}

static u32 mtk_hw_reset_read(struct mtk_eth *eth)
{
	u32 val;

	regmap_read(eth->ethsys, ETHSYS_RSTCTRL, &val);
	return val;
}

static void mtk_hw_warm_reset(struct mtk_eth *eth)
{
	u32 rst_mask, val;

	regmap_update_bits(eth->ethsys, ETHSYS_RSTCTRL, RSTCTRL_FE, RSTCTRL_FE);
	if (readx_poll_timeout_atomic(mtk_hw_reset_read, eth, val,
				      val & RSTCTRL_FE, 1, 1000)) {
		dev_err(eth->dev, "warm reset failed\n");
		mtk_hw_reset(eth);
		return;
	}

	rst_mask = RSTCTRL_ETH | RSTCTRL_PPE0_V3;
	if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSTCTRL_PPE1))
		rst_mask |= RSTCTRL_PPE1_V3;
	if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSTCTRL_PPE2))
		rst_mask |= RSTCTRL_PPE2;

	rst_mask |= RSTCTRL_WDMA0 | RSTCTRL_WDMA1 | RSTCTRL_WDMA2;

	regmap_update_bits(eth->ethsys, ETHSYS_RSTCTRL, rst_mask, rst_mask);

	udelay(1);
	val = mtk_hw_reset_read(eth);
	if (!(val & rst_mask))
		dev_err(eth->dev, "warm reset stage0 failed %08x (%08x)\n", val,
			rst_mask);

	rst_mask |= RSTCTRL_FE;
	regmap_update_bits(eth->ethsys, ETHSYS_RSTCTRL, rst_mask, ~rst_mask);

	udelay(1);
	val = mtk_hw_reset_read(eth);
	if (val & rst_mask)
		dev_err(eth->dev, "warm reset stage1 failed %08x (%08x)\n", val,
			rst_mask);
}

static int mtk_hw_init(struct mtk_eth *eth, bool reset)
{
	u32 dma_mask = ETHSYS_DMA_AG_MAP_ADMA | ETHSYS_DMA_AG_MAP_QDMA |
		       ETHSYS_DMA_AG_MAP_PPE;
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	int i, val, ret;

	if (!reset && test_and_set_bit(MTK_HW_INIT, &eth->state))
		return 0;

	if (!reset) {
		pm_runtime_enable(eth->dev);
		pm_runtime_get_sync(eth->dev);

		ret = mtk_clk_enable(eth);
		if (ret)
			goto err_disable_pm;
	}

	if (eth->ethsys)
		regmap_update_bits(eth->ethsys, ETHSYS_DMA_AG_MAP, dma_mask,
				   of_dma_is_coherent(eth->dma_dev->of_node) *
					   dma_mask);

	msleep(100);

	if (reset)
		mtk_hw_warm_reset(eth);
	else
		mtk_hw_reset(eth);

	/* Set FE to ADMAv2 if necessary */
	val = mtk_r32(eth, MTK_FE_GLO_MISC);
	mtk_w32(eth, val | BIT(4), MTK_FE_GLO_MISC);

	if (eth->pctl) {
		/* Set GE2 driving and slew rate */
		regmap_write(eth->pctl, GPIO_DRV_SEL10, 0xa00);

		/* set GE2 TDSEL */
		regmap_write(eth->pctl, GPIO_OD33_CTRL8, 0x5);

		/* set GE2 TUNE */
		regmap_write(eth->pctl, GPIO_BIAS_CTRL, 0x0);
	}

	/* Set linkdown as the default for each GMAC. Its own MCR would be set
	 * up with the more appropriate value when mtk_mac_config call is being
	 * invoked.
	 */
	for (i = 0; i < MTK_MAX_DEVS; i++) {
		struct net_device *dev = eth->netdev[i];

		if (!dev)
			continue;

		mtk_w32(eth, MAC_MCR_FORCE_LINK_DOWN, MTK_MAC_MCR(i));
		mtk_set_mcr_max_rx(netdev_priv(dev),
				   dev->mtu + MTK_RX_ETH_HLEN);
	}

	/* Indicates CDM to parse the MTK special tag from CPU
	 * which also is working out for untag packets.
	 */
	val = mtk_r32(eth, MTK_CDMQ_IG_CTRL);
	mtk_w32(eth, val | MTK_CDMQ_STAG_EN, MTK_CDMQ_IG_CTRL);

	/* set interrupt delays based on current Net DIM sample */
	mtk_dim_rx(&eth->rx_dim.work);
	mtk_dim_tx(&eth->tx_dim.work);

	/* disable delay and normal interrupt */
	mtk_tx_irq_disable(eth, ~0);
	mtk_rx_irq_disable(eth, ~0);

	/* FE int grouping */
	mtk_w32(eth, MTK_TX_DONE_INT, reg_map->adma.int_grp);
	mtk_w32(eth, eth->soc->rx.irq_done_mask, reg_map->adma.int_grp + 4);
	mtk_w32(eth, MTK_TX_DONE_INT, reg_map->qdma.int_grp);
	mtk_w32(eth, MTK_RX_DONE_INT(0), reg_map->qdma.int_grp + 4);
	mtk_w32(eth, 0x210FFFF2, MTK_FE_INT_GRP);

	/* PSE should not drop port1, port8 and port9 packets */
	mtk_w32(eth, 0x00000302, PSE_DROP_CFG);

	/* GDM and CDM Threshold */
	mtk_w32(eth, 0x00000707, MTK_CDMW0_THRES);
	mtk_w32(eth, 0x00000077, MTK_CDMW1_THRES);

	/* Disable GDM1 RX CRC stripping */
	mtk_m32(eth, MTK_GDMA_STRP_CRC, 0, MTK_GDMA_FWD_CFG(0));

	/* PSE GDM3 MIB counter has incorrect hw default values,
	 * so the driver ought to read clear the values beforehand
	 * in case ethtool retrieve wrong mib values.
	 */
	for (i = 0; i < 0x80; i += 0x4)
		mtk_r32(eth, reg_map->gdm1_cnt + 0x100 + i);

	return 0;

err_disable_pm:
	if (!reset) {
		pm_runtime_put_sync(eth->dev);
		pm_runtime_disable(eth->dev);
	}

	return ret;
}

static int mtk_hw_deinit(struct mtk_eth *eth)
{
	if (!test_and_clear_bit(MTK_HW_INIT, &eth->state))
		return 0;

	mtk_clk_disable(eth);

	pm_runtime_put_sync(eth->dev);
	pm_runtime_disable(eth->dev);

	return 0;
}

static void mtk_uninit(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;

	phylink_disconnect_phy(mac->phylink);
	mtk_tx_irq_disable(eth, ~0);
	mtk_rx_irq_disable(eth, ~0);
}

static int mtk_change_mtu(struct net_device *dev, int new_mtu)
{
	int length = new_mtu + MTK_RX_ETH_HLEN;
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;

	if (rcu_access_pointer(eth->prog) && length > MTK_PP_MAX_BUF_SIZE) {
		netdev_err(dev, "Invalid MTU for XDP mode\n");
		return -EINVAL;
	}

	mtk_set_mcr_max_rx(mac, length);
	WRITE_ONCE(dev->mtu, new_mtu);

	return 0;
}

static int mtk_do_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct mtk_mac *mac = netdev_priv(dev);

	switch (cmd) {
	case SIOCGMIIPHY:
	case SIOCGMIIREG:
	case SIOCSMIIREG:
		return phylink_mii_ioctl(mac->phylink, ifr, cmd);
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static void mtk_prepare_for_reset(struct mtk_eth *eth)
{
	u32 val;
	int i;

	for (i = MTK_GMAC1_ID; i <= MTK_GMAC3_ID; i += 2) {
		val = mtk_r32(eth, MTK_FE_GLO_CFG(i)) |
		      MTK_FE_LINK_DOWN_P(PSE_PPE0_PORT);
		if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSTCTRL_PPE1))
			val |= MTK_FE_LINK_DOWN_P(PSE_PPE1_PORT);
		if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSTCTRL_PPE2))
			val |= MTK_FE_LINK_DOWN_P(PSE_PPE2_PORT);
		mtk_w32(eth, val, MTK_FE_GLO_CFG(i));
	}

	for (i = 0; i < ARRAY_SIZE(eth->ppe); i++)
		mtk_ppe_prepare_reset(eth->ppe[i]);

	mtk_w32(eth, 0, MTK_FE_INT_ENABLE);

	for (i = 0; i < 2; i++) {
		val = mtk_r32(eth, MTK_MAC_MCR(i)) & ~MAC_MCR_FORCE_LINK;
		mtk_w32(eth, val, MTK_MAC_MCR(i));
	}
}

static void mtk_pending_work(struct work_struct *work)
{
	struct mtk_eth *eth = container_of(work, struct mtk_eth, pending_work);
	unsigned long restart = 0;
	u32 val;
	int i;

	rtnl_lock();
	set_bit(MTK_RESETTING, &eth->state);

	mtk_prepare_for_reset(eth);
	mtk_prepare_reset_fe(eth);
	mtk_wed_fe_reset();
	/* Run again reset preliminary configuration in order to avoid any
	 * possible race during FE reset since it can run releasing RTNL lock.
	 */
	mtk_prepare_for_reset(eth);

	/* stop all devices to make sure that dma is properly shut down */
	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i] || !netif_running(eth->netdev[i]))
			continue;

		mtk_stop(eth->netdev[i]);
		__set_bit(i, &restart);
	}

	usleep_range(15000, 16000);

	if (eth->dev->pins)
		pinctrl_select_state(eth->dev->pins->p,
				     eth->dev->pins->default_state);
	mtk_hw_init(eth, true);

	/* restart DMA and enable IRQs */
	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i] || !test_bit(i, &restart))
			continue;

		if (mtk_open(eth->netdev[i])) {
			netif_alert(eth, ifup, eth->netdev[i],
				    "Driver up/down cycle failed\n");
			dev_close(eth->netdev[i]);
		}
	}

	/* set FE PPE ports link up */
	for (i = MTK_GMAC1_ID; i <= MTK_GMAC3_ID; i += 2) {
		val = mtk_r32(eth, MTK_FE_GLO_CFG(i)) &
		      ~MTK_FE_LINK_DOWN_P(PSE_PPE0_PORT);
		if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSTCTRL_PPE1))
			val &= ~MTK_FE_LINK_DOWN_P(PSE_PPE1_PORT);
		if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSTCTRL_PPE2))
			val &= ~MTK_FE_LINK_DOWN_P(PSE_PPE2_PORT);

		mtk_w32(eth, val, MTK_FE_GLO_CFG(i));
	}

	clear_bit(MTK_RESETTING, &eth->state);

	mtk_wed_fe_reset_complete();

	rtnl_unlock();
}

static int mtk_free_dev(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i])
			continue;
		free_netdev(eth->netdev[i]);
	}

	for (i = 0; i < ARRAY_SIZE(eth->dsa_meta); i++) {
		if (!eth->dsa_meta[i])
			break;
		metadata_dst_free(eth->dsa_meta[i]);
	}

	return 0;
}

static int mtk_unreg_dev(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		struct mtk_mac *mac;
		if (!eth->netdev[i])
			continue;
		mac = netdev_priv(eth->netdev[i]);
		unregister_netdev(eth->netdev[i]);
	}

	return 0;
}

static void mtk_sgmii_destroy(struct mtk_eth *eth)
{
	int i;

	for (i = 0; i < MTK_MAX_DEVS; i++)
		mtk_pcs_lynxi_destroy(eth->sgmii_pcs[i]);
}

static int mtk_cleanup(struct mtk_eth *eth)
{
	mtk_sgmii_destroy(eth);
	mtk_unreg_dev(eth);
	mtk_free_dev(eth);
	cancel_work_sync(&eth->pending_work);
	cancel_delayed_work_sync(&eth->reset.monitor_work);

	return 0;
}

static int mtk_get_link_ksettings(struct net_device *ndev,
				  struct ethtool_link_ksettings *cmd)
{
	struct mtk_mac *mac = netdev_priv(ndev);

	if (unlikely(test_bit(MTK_RESETTING, &mac->hw->state)))
		return -EBUSY;

	return phylink_ethtool_ksettings_get(mac->phylink, cmd);
}

static int mtk_set_link_ksettings(struct net_device *ndev,
				  const struct ethtool_link_ksettings *cmd)
{
	struct mtk_mac *mac = netdev_priv(ndev);

	if (unlikely(test_bit(MTK_RESETTING, &mac->hw->state)))
		return -EBUSY;

	return phylink_ethtool_ksettings_set(mac->phylink, cmd);
}

static void mtk_get_drvinfo(struct net_device *dev,
			    struct ethtool_drvinfo *info)
{
	struct mtk_mac *mac = netdev_priv(dev);

	strscpy(info->driver, mac->hw->dev->driver->name, sizeof(info->driver));
	strscpy(info->bus_info, dev_name(mac->hw->dev), sizeof(info->bus_info));
	info->n_stats = ARRAY_SIZE(mtk_ethtool_stats);
}

static u32 mtk_get_msglevel(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);

	return mac->hw->msg_enable;
}

static void mtk_set_msglevel(struct net_device *dev, u32 value)
{
	struct mtk_mac *mac = netdev_priv(dev);

	mac->hw->msg_enable = value;
}

static int mtk_nway_reset(struct net_device *dev)
{
	struct mtk_mac *mac = netdev_priv(dev);

	if (unlikely(test_bit(MTK_RESETTING, &mac->hw->state)))
		return -EBUSY;

	if (!mac->phylink)
		return -ENOTSUPP;

	return phylink_ethtool_nway_reset(mac->phylink);
}

static void mtk_get_strings(struct net_device *dev, u32 stringset, u8 *data)
{
	int i;

	switch (stringset) {
	case ETH_SS_STATS: {
		for (i = 0; i < ARRAY_SIZE(mtk_ethtool_stats); i++) {
			memcpy(data, mtk_ethtool_stats[i].str, ETH_GSTRING_LEN);
			data += ETH_GSTRING_LEN;
		}
		page_pool_ethtool_stats_get_strings(data);
		break;
	}
	default:
		break;
	}
}

static int mtk_get_sset_count(struct net_device *dev, int sset)
{
	switch (sset) {
	case ETH_SS_STATS:
		return ARRAY_SIZE(mtk_ethtool_stats) +
		       page_pool_ethtool_stats_get_count();
	default:
		return -EOPNOTSUPP;
	}
}

static void mtk_ethtool_pp_stats(struct mtk_eth *eth, u64 *data)
{
	struct page_pool_stats stats = {};
	int i;

	for (i = 0; i < ARRAY_SIZE(eth->rx_ring); i++) {
		struct mtk_rx_ring *ring = &eth->rx_ring[i];

		if (!ring->page_pool)
			continue;

		page_pool_get_stats(ring->page_pool, &stats);
	}
	page_pool_ethtool_stats_get(data, &stats);
}

static void mtk_get_ethtool_stats(struct net_device *dev,
				  struct ethtool_stats *stats, u64 *data)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_hw_stats *hwstats = mac->hw_stats;
	u64 *data_src, *data_dst;
	unsigned int start;
	int i;

	if (unlikely(test_bit(MTK_RESETTING, &mac->hw->state)))
		return;

	if (netif_running(dev) && netif_device_present(dev)) {
		if (spin_trylock_bh(&hwstats->stats_lock)) {
			mtk_stats_update_mac(mac);
			spin_unlock_bh(&hwstats->stats_lock);
		}
	}

	data_src = (u64 *)hwstats;

	do {
		data_dst = data;
		start = u64_stats_fetch_begin(&hwstats->syncp);

		for (i = 0; i < ARRAY_SIZE(mtk_ethtool_stats); i++)
			*data_dst++ = *(data_src + mtk_ethtool_stats[i].offset);
		mtk_ethtool_pp_stats(mac->hw, data_dst);
	} while (u64_stats_fetch_retry(&hwstats->syncp, start));
}

static int mtk_get_rxnfc(struct net_device *dev, struct ethtool_rxnfc *cmd,
			 u32 *rule_locs)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	int ret = -EOPNOTSUPP;

	switch (cmd->cmd) {
	case ETHTOOL_GRXRINGS:
		if (dev->hw_features & NETIF_F_LRO) {
			cmd->data = MTK_MAX_RX_RING_NUM;
			ret = 0;
		} else if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSS)) {
			cmd->data = eth->soc->rss_num;
			ret = 0;
		}
		break;
	case ETHTOOL_GRXCLSRLCNT:
		if (dev->hw_features & NETIF_F_LRO) {
			cmd->rule_cnt = mac->hwlro_ip_cnt;
			ret = 0;
		}
		break;
	case ETHTOOL_GRXCLSRULE:
		if (dev->hw_features & NETIF_F_LRO)
			ret = mtk_hwlro_get_fdir_entry(dev, cmd);
		break;
	case ETHTOOL_GRXCLSRLALL:
		if (dev->hw_features & NETIF_F_LRO)
			ret = mtk_hwlro_get_fdir_all(dev, cmd, rule_locs);
		break;
	default:
		break;
	}

	return ret;
}

static int mtk_set_rxnfc(struct net_device *dev, struct ethtool_rxnfc *cmd)
{
	int ret = -EOPNOTSUPP;

	switch (cmd->cmd) {
	case ETHTOOL_SRXCLSRLINS:
		if (dev->hw_features & NETIF_F_LRO)
			ret = mtk_hwlro_add_ipaddr(dev, cmd);
		break;
	case ETHTOOL_SRXCLSRLDEL:
		if (dev->hw_features & NETIF_F_LRO)
			ret = mtk_hwlro_del_ipaddr(dev, cmd);
		break;
	default:
		break;
	}

	return ret;
}

static void mtk_get_pauseparam(struct net_device *dev,
			       struct ethtool_pauseparam *pause)
{
	struct mtk_mac *mac = netdev_priv(dev);

	phylink_ethtool_get_pauseparam(mac->phylink, pause);
}

static int mtk_set_pauseparam(struct net_device *dev,
			      struct ethtool_pauseparam *pause)
{
	struct mtk_mac *mac = netdev_priv(dev);

	return phylink_ethtool_set_pauseparam(mac->phylink, pause);
}

static u32 mtk_get_rxfh_key_size(struct net_device *dev)
{
	return MTK_RSS_HASH_KEYSIZE;
}

static u32 mtk_get_rxfh_indir_size(struct net_device *dev)
{
	return MTK_RSS_MAX_INDIRECTION_TABLE;
}

static int mtk_get_rxfh(struct net_device *dev, struct ethtool_rxfh_param *rxfh)
{
	u32 *indir = rxfh->indir;
	u8 *key = rxfh->key;
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	struct mtk_rss_params *rss_params = &eth->rss_params;
	int i;
	if (rxfh->hfunc)
		rxfh->hfunc = ETH_RSS_HASH_TOP; /* Toeplitz */
	if (key) {
		memcpy(key, rss_params->hash_key, sizeof(rss_params->hash_key));
	}
	if (indir) {
		for (i = 0; i < MTK_RSS_MAX_INDIRECTION_TABLE; i++)
			indir[i] = rss_params->indirection_table[i];
	}
	return 0;
}

static int mtk_set_rxfh(struct net_device *dev, struct ethtool_rxfh_param *rxfh,
			struct netlink_ext_ack *extack)
{
	u32 *indir = rxfh->indir;
	u8 *key = rxfh->key;
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	struct mtk_rss_params *rss_params = &eth->rss_params;
	const struct mtk_reg_map *reg_map = eth->soc->reg_map;
	int i;
	if (rxfh->hfunc != ETH_RSS_HASH_NO_CHANGE &&
	    rxfh->hfunc != ETH_RSS_HASH_TOP)
		return -EOPNOTSUPP;
	if (key) {
		memcpy(rss_params->hash_key, key, sizeof(rss_params->hash_key));
		for (i = 0; i < MTK_RSS_HASH_KEYSIZE / sizeof(u32); i++)
			mtk_w32(eth, rss_params->hash_key[i],
				MTK_RSS_HASH_KEY_DW(i));
	}
	if (indir) {
		for (i = 0; i < MTK_RSS_MAX_INDIRECTION_TABLE; i++)
			rss_params->indirection_table[i] = indir[i];
		for (i = 0; i < MTK_RSS_MAX_INDIRECTION_TABLE / 16; i++)
			mtk_w32(eth, mtk_rss_indr_table(rss_params, i),
				MTK_RSS_INDR_TABLE_DW(i));
	}
	return 0;
}

static u16 mtk_select_queue(struct net_device *dev, struct sk_buff *skb,
			    struct net_device *sb_dev)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;
	unsigned int queue = 0;

	if (eth->htb_active) {
		queue = skb_get_queue_mapping(skb);
		if (queue < dev->num_tx_queues)
			return queue;
	}

	if (netdev_uses_dsa(dev))
		queue = skb_get_queue_mapping(skb) + 3;
	else
		queue = mac->id;

	if (queue >= dev->num_tx_queues)
		queue = 0;

	return queue;
}

static void mtk_get_ringparam(struct net_device *dev,
			      struct ethtool_ringparam *ering,
			      struct kernel_ethtool_ringparam *kernel_ering,
			      struct netlink_ext_ack *extack)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;

	ering->rx_max_pending = MTK_DMA_SIZE(4K);
	ering->rx_pending = eth->rx_ring[0].dma_size;
	ering->tx_max_pending = MTK_DMA_SIZE(4K);
	ering->tx_pending = eth->tx_ring.dma_size;
}

static void mtk_get_channels(struct net_device *dev,
			     struct ethtool_channels *ch)
{
	struct mtk_mac *mac = netdev_priv(dev);
	struct mtk_eth *eth = mac->hw;

	/* У нас 1 базовая RX очередь + 3 RSS очереди = 4 */
	if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSS)) {
		ch->max_rx = MTK_RX_NAPI_NUM;
		ch->rx_count = MTK_RX_NAPI_NUM;
	} else {
		ch->max_rx = 1;
		ch->rx_count = 1;
	}

	/* TX очереди у нас QDMA (16 штук) */
	ch->max_tx = MTK_QDMA_NUM_QUEUES;
	ch->tx_count = MTK_QDMA_NUM_QUEUES;

	/* Combined очереди */
	ch->max_combined = min(ch->max_rx, ch->max_tx);
	ch->combined_count = min(ch->rx_count, ch->tx_count);
}

static const struct ethtool_ops mtk_ethtool_ops = {
	.get_link_ksettings = mtk_get_link_ksettings,
	.set_link_ksettings = mtk_set_link_ksettings,
	.get_drvinfo = mtk_get_drvinfo,
	.get_msglevel = mtk_get_msglevel,
	.set_msglevel = mtk_set_msglevel,
	.nway_reset = mtk_nway_reset,
	.get_link = ethtool_op_get_link,
	.get_strings = mtk_get_strings,
	.get_sset_count = mtk_get_sset_count,
	.get_ethtool_stats = mtk_get_ethtool_stats,
	.get_pauseparam = mtk_get_pauseparam,
	.set_pauseparam = mtk_set_pauseparam,
	.get_rxnfc = mtk_get_rxnfc,
	.set_rxnfc = mtk_set_rxnfc,
	.get_rxfh_key_size = mtk_get_rxfh_key_size,
	.get_rxfh_indir_size = mtk_get_rxfh_indir_size,
	.get_rxfh = mtk_get_rxfh,
	.set_rxfh = mtk_set_rxfh,
	.get_channels = mtk_get_channels,
	.get_ringparam = mtk_get_ringparam,
};

static const struct net_device_ops mtk_netdev_ops = {
	.ndo_uninit = mtk_uninit,
	.ndo_open = mtk_open,
	.ndo_stop = mtk_stop,
	.ndo_start_xmit = mtk_start_xmit,
	.ndo_set_mac_address = mtk_set_mac_address,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_do_ioctl = mtk_do_ioctl,
	.ndo_change_mtu = mtk_change_mtu,
	.ndo_tx_timeout = mtk_tx_timeout,
	.ndo_get_stats64 = mtk_get_stats64,
	.ndo_fix_features = mtk_fix_features,
	.ndo_set_features = mtk_set_features,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller = mtk_poll_controller,
#endif
	.ndo_setup_tc = mtk_eth_setup_tc,
	.ndo_bpf = mtk_xdp,
	.ndo_xdp_xmit = mtk_xdp_xmit,
	.ndo_select_queue = mtk_select_queue,
};

static int mtk_add_mac(struct mtk_eth *eth, struct device_node *np)
{
	const __be32 *_id = of_get_property(np, "reg", NULL);
	phy_interface_t phy_mode;
	struct phylink *phylink;
	struct mtk_mac *mac;
	int id, err;

	if (!_id) {
		dev_err(eth->dev, "missing mac id\n");
		return -EINVAL;
	}

	id = be32_to_cpup(_id);
	if (id >= MTK_MAX_DEVS) {
		dev_err(eth->dev, "%d is not a valid mac id\n", id);
		return -EINVAL;
	}

	if (eth->netdev[id]) {
		dev_err(eth->dev, "duplicate mac id found: %d\n", id);
		return -EINVAL;
	}

	/* ВАЖНО: Указываем 16 TX очередей и 4 RX очереди (MTK_RX_NAPI_NUM) */
	eth->netdev[id] = alloc_etherdev_mqs(sizeof(*mac), MTK_QDMA_NUM_QUEUES,
					     MTK_RX_NAPI_NUM);
	if (!eth->netdev[id]) {
		dev_err(eth->dev, "alloc_etherdev failed\n");
		return -ENOMEM;
	}
	mac = netdev_priv(eth->netdev[id]);
	eth->mac[id] = mac;
	mac->id = id;
	mac->hw = eth;
	mac->of_node = np;
	mac->sgmii_pcs = devm_of_pcs_get(eth->dev, np, 0);
	if (IS_ERR(mac->sgmii_pcs)) {
		if (PTR_ERR(mac->sgmii_pcs) == -ENODEV)
			return -EPROBE_DEFER;

		dev_err(eth->dev, "cannot select SGMII PCS, error %ld\n",
			PTR_ERR(mac->sgmii_pcs));
		return PTR_ERR(mac->sgmii_pcs);
	}

	mac->usxgmii_pcs = devm_of_pcs_get(eth->dev, np, 1);
	if (IS_ERR(mac->usxgmii_pcs)) {
		if (PTR_ERR(mac->usxgmii_pcs) == -ENODEV)
			return -EPROBE_DEFER;

		dev_err(eth->dev, "cannot select USXGMII PCS, error %ld\n",
			PTR_ERR(mac->usxgmii_pcs));
		return PTR_ERR(mac->usxgmii_pcs);
	}

	memset(mac->hwlro_ip, 0, sizeof(mac->hwlro_ip));
	mac->hwlro_ip_cnt = 0;

	mac->hw_stats =
		devm_kzalloc(eth->dev, sizeof(*mac->hw_stats), GFP_KERNEL);
	if (!mac->hw_stats) {
		dev_err(eth->dev, "failed to allocate counter memory\n");
		err = -ENOMEM;
		goto free_netdev;
	}
	spin_lock_init(&mac->hw_stats->stats_lock);
	u64_stats_init(&mac->hw_stats->syncp);
	mac->hw_stats->reg_offset = id * 0x80;

	/* phylink create */
	err = of_get_phy_mode(np, &phy_mode);
	if (err) {
		dev_err(eth->dev, "incorrect phy-mode\n");
		goto free_netdev;
	}

	/* mac config is not set */
	mac->interface = PHY_INTERFACE_MODE_NA;
	mac->speed = SPEED_UNKNOWN;

	mac->phylink_config.dev = &eth->netdev[id]->dev;
	mac->phylink_config.type = PHYLINK_NETDEV;
	mac->phylink_config.mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE |
					       MAC_10 | MAC_100 | MAC_1000 |
					       MAC_2500FD;

	if (id == MTK_GMAC1_ID &&
	    MTK_HAS_CAPS(mac->hw->soc->caps, MTK_ESW_BIT)) {
		mac->phylink_config.mac_capabilities =
			MAC_ASYM_PAUSE | MAC_SYM_PAUSE | MAC_10000FD;
		phy_interface_zero(mac->phylink_config.supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  mac->phylink_config.supported_interfaces);
	} else {
		mac->phylink_config.mac_capabilities |= MAC_5000FD |
							MAC_10000FD;
		__set_bit(PHY_INTERFACE_MODE_SGMII,
			  mac->phylink_config.supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_1000BASEX,
			  mac->phylink_config.supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_2500BASEX,
			  mac->phylink_config.supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_5GBASER,
			  mac->phylink_config.supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_10GBASER,
			  mac->phylink_config.supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_USXGMII,
			  mac->phylink_config.supported_interfaces);
	}

	phylink = phylink_create(&mac->phylink_config,
				 of_fwnode_handle(mac->of_node), phy_mode,
				 &mtk_phylink_ops);
	if (IS_ERR(phylink)) {
		err = PTR_ERR(phylink);
		goto free_netdev;
	}

	mac->phylink = phylink;

	SET_NETDEV_DEV(eth->netdev[id], eth->dev);
	eth->netdev[id]->watchdog_timeo = 5 * HZ;
	eth->netdev[id]->netdev_ops = &mtk_netdev_ops;
	eth->netdev[id]->base_addr = (unsigned long)eth->base;

	eth->netdev[id]->hw_features = eth->soc->hw_features;
	if (eth->hwlro)
		eth->netdev[id]->hw_features |= NETIF_F_LRO;

	eth->netdev[id]->vlan_features = eth->soc->hw_features &
					 ~NETIF_F_HW_VLAN_CTAG_TX;
	eth->netdev[id]->features |= eth->soc->hw_features;
	eth->netdev[id]->ethtool_ops = &mtk_ethtool_ops;

	eth->netdev[id]->irq = eth->irq_fe[0];
	eth->netdev[id]->dev.of_node = np;

	eth->netdev[id]->max_mtu = MTK_MAX_RX_LENGTH_2K - MTK_RX_ETH_HLEN;

	eth->netdev[id]->xdp_features =
		NETDEV_XDP_ACT_BASIC | NETDEV_XDP_ACT_REDIRECT |
		NETDEV_XDP_ACT_NDO_XMIT | NETDEV_XDP_ACT_NDO_XMIT_SG;

	/* Настройка XPS (Transmit Packet Steering) для TX очередей.
	 * Говорим ядру: CPU0 отправляет через Queue 0, CPU1 через Queue 1 и т.д.
	 * Это убирает спинлоки при многопоточном TX.
	 */
	{
		int q;
		for (q = 0; q < MTK_QDMA_NUM_QUEUES; q++) {
			cpumask_t mask;
			cpumask_clear(&mask);
			/* Раскидываем 16 TX очередей по кругу между 4 CPU */
			cpumask_set_cpu(q % MTK_RX_NAPI_NUM, &mask);
			netif_set_xps_queue(eth->netdev[id], &mask, q);
		}
	}

	return 0;

free_netdev:
	free_netdev(eth->netdev[id]);
	return err;
}

static int mtk_mac_assign_address(struct mtk_eth *eth, int i,
				  bool test_defer_only)
{
	int err = of_get_ethdev_address(eth->mac[i]->of_node, eth->netdev[i]);

	if (err == -EPROBE_DEFER)
		return err;

	if (test_defer_only)
		return 0;

	if (err) {
		/* If the mac address is invalid, use random mac address */
		eth_hw_addr_random(eth->netdev[i]);
		dev_err(eth->dev, "generated random MAC address %pM\n",
			eth->netdev[i]);
	}

	return 0;
}

void mtk_eth_set_dma_device(struct mtk_eth *eth, struct device *dma_dev)
{
	struct net_device *dev, *tmp;
	LIST_HEAD(dev_list);
	int i;

	rtnl_lock();

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		dev = eth->netdev[i];

		if (!dev || !(dev->flags & IFF_UP))
			continue;

		list_add_tail(&dev->close_list, &dev_list);
	}

	dev_close_many(&dev_list, false);

	eth->dma_dev = dma_dev;

	list_for_each_entry_safe(dev, tmp, &dev_list, close_list) {
		list_del_init(&dev->close_list);
		dev_open(dev, NULL);
	}

	rtnl_unlock();
}

static int mtk_probe(struct platform_device *pdev)
{
	struct resource *res = NULL;
	struct device_node *mac_np;
	struct mtk_eth *eth;
	int err, i;

	eth = devm_kzalloc(&pdev->dev, sizeof(*eth), GFP_KERNEL);
	if (!eth)
		return -ENOMEM;

	/* Try 36-bit DMA first, fall back to 32-bit */
	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(36));
	if (err) {
		err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (err) {
			dev_err(&pdev->dev, "DMA mask setup failed\n");
			return err;
		}
	}

	eth->soc = of_device_get_match_data(&pdev->dev);

	eth->dev = &pdev->dev;
	eth->dma_dev = &pdev->dev;
	eth->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(eth->base))
		return PTR_ERR(eth->base);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_SRAM)) {
		struct resource *res_sram;

		res_sram = platform_get_resource(pdev, IORESOURCE_MEM, 1);
		if (!res_sram)
			return -EINVAL;

		eth->sram_base = devm_memremap(eth->dev, res_sram->start,
					       resource_size(res_sram),
					       MEMREMAP_WC);
		if (IS_ERR(eth->sram_base))
			return PTR_ERR(eth->sram_base);

		eth->phy_scratch_ring = res_sram->start;
	}

	spin_lock_init(&eth->page_lock);
	spin_lock_init(&eth->tx_irq_lock);
	spin_lock_init(&eth->rx_irq_lock);
	spin_lock_init(&eth->dim_lock);

	eth->rx_dim.mode = DIM_CQ_PERIOD_MODE_START_FROM_EQE;
	INIT_WORK(&eth->rx_dim.work, mtk_dim_rx);
	INIT_DELAYED_WORK(&eth->reset.monitor_work, mtk_dma_monitor_work);

	eth->tx_dim.mode = DIM_CQ_PERIOD_MODE_START_FROM_EQE;
	INIT_WORK(&eth->tx_dim.work, mtk_dim_tx);

	eth->ethsys = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						      "mediatek,ethsys");
	if (IS_ERR(eth->ethsys)) {
		dev_err(&pdev->dev, "no ethsys regmap found\n");
		return PTR_ERR(eth->ethsys);
	}

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_INFRA)) {
		eth->infra = syscon_regmap_lookup_by_phandle(
			pdev->dev.of_node, "mediatek,infracfg");
		if (IS_ERR(eth->infra)) {
			dev_err(&pdev->dev, "no infracfg regmap found\n");
			return PTR_ERR(eth->infra);
		}
	}

	if (of_dma_is_coherent(pdev->dev.of_node)) {
		struct regmap *cci;

		cci = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						      "cci-control-port");
		/* enable CPU/bus coherency */
		if (!IS_ERR(cci))
			regmap_write(cci, 0, 3);
	}

	if (eth->soc->required_pctl) {
		eth->pctl = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							    "mediatek,pctl");
		if (IS_ERR(eth->pctl)) {
			dev_err(&pdev->dev, "no pctl regmap found\n");
			return PTR_ERR(eth->pctl);
		}
	}

	if (eth->soc->offload_version) {
		for (i = 0;; i++) {
			struct device_node *np;
			phys_addr_t wdma_phy;
			u32 wdma_base;

			if (i >= ARRAY_SIZE(eth->soc->reg_map->wdma_base))
				break;

			np = of_parse_phandle(pdev->dev.of_node, "mediatek,wed",
					      i);
			if (!np)
				break;

			wdma_base = eth->soc->reg_map->wdma_base[i];
			wdma_phy = res ? res->start + wdma_base : 0;
			mtk_wed_add_hw(np, eth, eth->base + wdma_base, wdma_phy,
				       i);
		}
	}

	for (i = 0; i < MTK_ADMA_IRQ_NUM; i++)
		eth->irq_adma[i] = platform_get_irq(pdev, i);

	for (i = 0; i < MTK_FE_IRQ_NUM; i++) {
		eth->irq_fe[i] = platform_get_irq(pdev, MTK_ADMA_IRQ_NUM + i);
		if (eth->irq_fe[i] < 0) {
			dev_err(&pdev->dev, "no IRQ%d resource found\n", i);
			err = -ENXIO;
			goto err_wed_exit;
		}
	}

	for (i = 0; i < ARRAY_SIZE(eth->clks); i++) {
		eth->clks[i] = devm_clk_get(eth->dev, mtk_clks_source_name[i]);
		if (IS_ERR(eth->clks[i])) {
			if (PTR_ERR(eth->clks[i]) == -EPROBE_DEFER) {
				err = -EPROBE_DEFER;
				goto err_wed_exit;
			}
			if (eth->soc->required_clks & BIT(i)) {
				dev_err(&pdev->dev, "clock %s not found\n",
					mtk_clks_source_name[i]);
				err = -EINVAL;
				goto err_wed_exit;
			}
			eth->clks[i] = NULL;
		}
	}

	eth->msg_enable = netif_msg_init(mtk_msg_level, MTK_DEFAULT_MSG_ENABLE);
	INIT_WORK(&eth->pending_work, mtk_pending_work);

	err = mtk_hw_init(eth, false);
	if (err)
		goto err_wed_exit;

	eth->hwlro = MTK_HAS_CAPS(eth->soc->caps, MTK_HWLRO);

	for_each_child_of_node(pdev->dev.of_node, mac_np) {
		if (!of_device_is_compatible(mac_np, "mediatek,eth-mac"))
			continue;

		if (!of_device_is_available(mac_np))
			continue;

		err = mtk_add_mac(eth, mac_np);
		if (err) {
			of_node_put(mac_np);
			goto err_deinit_hw;
		}
	}

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i])
			continue;

		err = mtk_mac_assign_address(eth, i, true);
		if (err)
			goto err_deinit_hw;
	}

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i])
			continue;

		err = mtk_mac_assign_address(eth, i, false);
		if (err)
			goto err_deinit_hw;
	}

	err = mtk_napi_init(eth);
	if (err)
		goto err_free_dev;

	/* ==================== ИНТЕРРУПТЫ ==================== */

	/* TX IRQ (SPI 196) - пока на любом CPU (ядро само решит) */
	err = devm_request_irq(eth->dev, eth->irq_fe[1], mtk_handle_irq_tx, 0,
			       dev_name(eth->dev), eth);
	if (err)
		goto err_free_dev;

	/* FE Misc IRQ */
	err = devm_request_irq(eth->dev, eth->irq_fe[2], mtk_handle_fe_irq, 0,
			       dev_name(eth->dev), eth);
	if (err)
		goto err_free_dev;

	/* Базовый RX IRQ (Ring 0 / SPI 189) -> Привязываем к CPU0 */
	err = devm_request_irq(eth->dev, eth->irq_adma[0], mtk_handle_irq_rx,
			       IRQF_SHARED, dev_name(eth->dev),
			       &eth->rx_napi[0]);
	if (err)
		goto err_free_dev;
	irq_set_affinity_hint(eth->irq_adma[0], cpumask_of(0));

	/* RSS RX IRQs (Rings 1, 2, 3 / SPI 190, 191, 192) -> Привязываем к CPU 1, 2, 3 */
	if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSS)) {
		for (i = 0; i < MTK_RX_RSS_NUM; i++) {
			int ring_no = MTK_RSS_RING(i); // 1, 2, 3
			int irq = eth->irq_adma[ring_no];

			err = devm_request_irq(eth->dev, irq, mtk_handle_irq_rx,
					       IRQF_SHARED, dev_name(eth->dev),
					       &eth->rx_napi[ring_no]);
			if (err)
				goto err_free_dev;

			/* Hardcode affinity: Ring N -> CPU N */
			irq_set_affinity_hint(irq, cpumask_of(ring_no));
		}
	}

	/* ==================== КОНЕЦ ИНТЕРРУПТОВ ==================== */

	err = mtk_mdio_init(eth);
	if (err)
		goto err_free_dev;

	if (eth->soc->offload_version) {
		u8 ppe_num = eth->soc->ppe_num;

		ppe_num = min_t(u8, ARRAY_SIZE(eth->ppe), ppe_num);
		for (i = 0; i < ppe_num; i++) {
			u32 ppe_addr = eth->soc->reg_map->ppe_base;

			ppe_addr += (i == 2 ? 0xc00 : i * 0x400);
			eth->ppe[i] =
				mtk_ppe_init(eth, eth->base + ppe_addr, i);

			if (!eth->ppe[i]) {
				err = -ENOMEM;
				goto err_deinit_ppe;
			}
#if !IS_ENABLED(CONFIG_NET_MEDIATEK_HNAT)
			err = mtk_eth_offload_init(eth, i);
			if (err)
				goto err_deinit_ppe;
#endif
		}
	}

	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i])
			continue;

		err = register_netdev(eth->netdev[i]);
		if (err) {
			dev_err(eth->dev, "error bringing up device\n");
			goto err_deinit_ppe;
		} else
			netif_info(eth, probe, eth->netdev[i],
				   "mediatek frame engine at 0x%08lx, irq %d\n",
				   eth->netdev[i]->base_addr, eth->irq_fe[0]);
	}

	/* we run 2 devices on the same DMA ring so we need a dummy device
     * for NAPI to work
     */
	eth->dummy_dev = alloc_netdev_dummy(0);
	if (!eth->dummy_dev) {
		err = -ENOMEM;
		dev_err(eth->dev, "failed to allocated dummy device\n");
		goto err_unreg_netdev;
	}

	/* TX NAPI (пока оставляем 1 на всех) */
	netif_napi_add(eth->dummy_dev, &eth->tx_napi, mtk_napi_tx);

	/* RX NAPI: Строго 4 штуки под наши 4 очереди/прерывания */
	netif_napi_add(eth->dummy_dev, &eth->rx_napi[0].napi, mtk_napi_rx);

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSS)) {
		for (i = 0; i < MTK_RX_RSS_NUM; i++)
			netif_napi_add(eth->dummy_dev,
				       &eth->rx_napi[MTK_RSS_RING(i)].napi,
				       mtk_napi_rx);
	}

	/* Блок HW LRO УДАЛЕН, чтобы не выходить за пределы массива из 4 элементов */

	platform_set_drvdata(pdev, eth);
	schedule_delayed_work(&eth->reset.monitor_work,
			      MTK_DMA_MONITOR_TIMEOUT);

	return 0;

err_unreg_netdev:
	mtk_unreg_dev(eth);
err_deinit_ppe:
	mtk_ppe_deinit(eth);
	mtk_mdio_cleanup(eth);
err_free_dev:
	mtk_free_dev(eth);
err_deinit_hw:
	mtk_hw_deinit(eth);
err_wed_exit:
	mtk_wed_exit();

	return err;
}

static void mtk_remove(struct platform_device *pdev)
{
	struct mtk_eth *eth = platform_get_drvdata(pdev);
	struct mtk_mac *mac;
	int i;

	/* stop all devices to make sure that dma is properly shut down */
	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i])
			continue;
		mtk_stop(eth->netdev[i]);
		mac = netdev_priv(eth->netdev[i]);
		phylink_disconnect_phy(mac->phylink);
	}

	mtk_wed_exit();
	mtk_hw_deinit(eth);

	/* СНАЧАЛА очищаем привязку прерываний к ядрам */
	irq_set_affinity_hint(eth->irq_adma[0], NULL);
	if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSS)) {
		for (i = 0; i < MTK_RX_RSS_NUM; i++)
			irq_set_affinity_hint(eth->irq_adma[MTK_RSS_RING(i)],
					      NULL);
	}

	/* ПОТОМ удаляем NAPI */
	netif_napi_del(&eth->tx_napi);
	netif_napi_del(&eth->rx_napi[0].napi);

	if (MTK_HAS_CAPS(eth->soc->caps, MTK_RSS)) {
		for (i = 0; i < MTK_RX_RSS_NUM; i++)
			netif_napi_del(&eth->rx_napi[MTK_RSS_RING(i)].napi);
	}

	/* Блок HW LRO УДАЛЕН, чтобы не выходить за пределы массива из 4 элементов */

	mtk_cleanup(eth);
	free_netdev(eth->dummy_dev);
	mtk_mdio_cleanup(eth);
}

static const struct mtk_soc_data mt7988_data = {
	.reg_map = &mt7988_reg_map,
	.ana_rgc3 = 0x128,
	.caps = MT7988_CAPS | MTK_HWLRO,
	.hw_features = MTK_HW_FEATURES,
	.required_clks = MT7988_CLKS_BITMAP,
	.required_pctl = false,
	.version = 3,
	.offload_version = 2,
	.ppe_num = 3,
	.hash_offset = 4,
	.has_accounting = true,
	.foe_entry_size = MTK_FOE_ENTRY_V3_SIZE,
	.rss_num = 4,
	.tx = {
		.desc_size = sizeof(struct mtk_tx_dma_v2),
		.dma_max_len = MTK_TX_DMA_BUF_LEN_V2,
		.dma_len_offset = 8,
		.dma_size = MTK_DMA_SIZE(4K),
		.fq_dma_size = MTK_DMA_SIZE(4K),
	},
	.rx = {
		.desc_size = sizeof(struct mtk_rx_dma_v2),
		//.irq_done_mask = MTK_RX_DONE_INT_V2,
		.dma_l4_valid = RX_DMA_L4_VALID_V2,
		.dma_max_len = MTK_TX_DMA_BUF_LEN_V2,
		.dma_len_offset = 8,
		.dma_size = MTK_DMA_SIZE(4K),
	},
};

const struct of_device_id of_mtk_match[] = {
	{ .compatible = "mediatek,mt7988-eth", .data = &mt7988_data },
	{},
};
MODULE_DEVICE_TABLE(of, of_mtk_match);

static struct platform_driver mtk_driver = {
	.probe = mtk_probe,
	.remove_new = mtk_remove,
	.driver = {
		.name = "mt7988_eth",
		.of_match_table = of_mtk_match,
	},
};

module_platform_driver(mtk_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("John Crispin <blogic@openwrt.org>");
MODULE_DESCRIPTION("Ethernet driver for MediaTek SoC");
