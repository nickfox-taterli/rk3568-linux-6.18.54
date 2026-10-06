/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MPP_MAINLINE_DEVFREQ_H
#define MPP_MAINLINE_DEVFREQ_H
#include "mpp_debug.h"
#include "mpp_common.h"
#if IS_ENABLED(CONFIG_ROCKCHIP_MPP_MAINLINE_DEVFREQ)
int mpp_mainline_dvfs_init(struct mpp_dev *mpp);
void mpp_mainline_dvfs_remove(struct mpp_dev *mpp);
int mpp_mainline_dvfs_rate(struct mpp_dev *mpp, unsigned long rate);
#else
static inline int mpp_mainline_dvfs_init(struct mpp_dev *mpp) { return 0; }
static inline void mpp_mainline_dvfs_remove(struct mpp_dev *mpp) { }
static inline int mpp_mainline_dvfs_rate(struct mpp_dev *mpp, unsigned long rate)
{
	return -EOPNOTSUPP;
}
#endif
#endif
