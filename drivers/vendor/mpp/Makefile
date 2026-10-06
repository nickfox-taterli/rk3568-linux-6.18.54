# SPDX-License-Identifier: GPL-2.0

rockchip-mpp-y := mpp_service.o mpp_common.o mpp_iommu.o
rockchip-mpp-$(CONFIG_ROCKCHIP_MPP_RKVDEC2) += mpp_rkvdec2.o mpp_rkvdec2_link.o

rockchip-mpp-$(CONFIG_ROCKCHIP_MPP_VDPU2) += mpp_vdpu2.o
rockchip-mpp-$(CONFIG_ROCKCHIP_MPP_VEPU2) += mpp_vepu2.o
rockchip-mpp-$(CONFIG_ROCKCHIP_MPP_RKVENC) += mpp_rkvenc.o
rockchip-mpp-$(CONFIG_ROCKCHIP_MPP_JPGDEC) += mpp_jpgdec.o
rockchip-mpp-$(CONFIG_ROCKCHIP_MPP_IEP2) += mpp_iep2.o

rockchip-mpp-$(CONFIG_ROCKCHIP_MPP_MAINLINE_DEVFREQ) += mpp_devfreq.o

obj-$(CONFIG_ROCKCHIP_VENDOR_MPP) += rockchip-mpp.o
