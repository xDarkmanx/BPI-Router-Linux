# SPDX-License-Identifier: GPL-2.0-only

ccflags-y += -DCONFIG_MEDIATEK_NETSYS_V3
ccflags-y += -Wno-error

obj-$(CONFIG_NET_MEDIATEK_HNAT) += mtkhnat.o
mtkhnat-objs := hnat.o hnat_nf_hook.o hnat_debugfs.o hnat_mcast.o
mtkhnat-$(CONFIG_NET_DSA_MT7530) += hnat_stag.o
