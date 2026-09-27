/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SOC_ROCKCHIP_IOMMU_H
#define __SOC_ROCKCHIP_IOMMU_H

#include <linux/kconfig.h>
#include <linux/types.h>

struct device;

#if IS_REACHABLE(CONFIG_ROCKCHIP_IOMMU)
bool rockchip_iommu_is_enabled(struct device *dev);
int rockchip_iommu_disable(struct device *dev);
int rockchip_iommu_enable(struct device *dev);
int rockchip_iommu_force_reset(struct device *dev);
void rockchip_iommu_mask_irq(struct device *dev);
#else
static inline bool rockchip_iommu_is_enabled(struct device *dev)
{
	return false;
}
#endif

#endif
