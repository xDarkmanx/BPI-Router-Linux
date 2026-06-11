# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the Mediatek SoCs built-in ethernet macs
#

obj-$(CONFIG_NET_MEDIATEK_SOC) += mtk_eth.o
mtk_eth-y := mtk_eth_soc.o mtk_eth_path.o
ifndef CONFIG_NET_MEDIATEK_HNAT
mtk_eth-y += mtk_ppe.o mtk_ppe_debugfs.o mtk_ppe_offload.o
else
mtk_eth-y += mtk_ppe.o mtk_ppe_debugfs.o
endif
mtk_eth-$(CONFIG_NET_MEDIATEK_SOC_WED) += mtk_wed.o mtk_wed_mcu.o mtk_wed_wo.o
ifdef CONFIG_DEBUG_FS
mtk_eth-$(CONFIG_NET_MEDIATEK_SOC_WED) += mtk_wed_debugfs.o
endif
obj-$(CONFIG_NET_MEDIATEK_SOC_WED) += mtk_wed_ops.o

obj-$(CONFIG_NET_MEDIATEK_MT7988) += mt7988_eth.o
mt7988_eth-y := mt7988_eth.o mt7988_path.o mt7988_dma.o
mt7988_eth-y += mtk_htb.o
ifndef CONFIG_NET_MEDIATEK_HNAT
mt7988_eth-y += mt7988_ppe.o mt7988_debugfs.o mt7988_offload.o
else
mt7988_eth-y += mt7988_ppe.o mt7988_debugfs.o
endif
mt7988_eth-$(CONFIG_NET_MEDIATEK_SOC_WED) += mtk_wed.o mtk_wed_mcu.o mtk_wed_wo.o
ifdef CONFIG_DEBUG_FS
mt7988_eth-$(CONFIG_NET_MEDIATEK_SOC_WED) += mtk_wed_debugfs.o
endif
obj-$(CONFIG_NET_MEDIATEK_STAR_EMAC) += mtk_star_emac.o
obj-$(CONFIG_NET_MEDIATEK_HNAT) += mtk_hnat/
obj-$(CONFIG_NET_AIROHA) += airoha_eth.o
