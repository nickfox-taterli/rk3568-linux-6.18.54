// SPDX-License-Identifier: GPL-2.0-only
/* SDK workload clock requests constrained by mainline OPP/QoS/thermal. */
#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/devfreq_cooling.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/pm_opp.h>
#include <linux/pm_qos.h>
#include "mpp_devfreq.h"

struct mpp_mainline_dvfs {
	struct clk *clk;
	struct devfreq *df;
	struct thermal_cooling_device *cooling;
	struct devfreq_dev_profile profile;
	struct dev_pm_qos_request workload_max;
	int opp_token;
	bool opp_ready;
};
static int mpp_dvfs_current(struct device *dev, unsigned long *freq)
{
	struct mpp_dev *mpp = dev_get_drvdata(dev);
	*freq = clk_get_rate(mpp->mainline_dvfs->clk);
	return 0;
}
static int mpp_dvfs_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct dev_pm_opp *opp = devfreq_recommended_opp(dev, freq, flags);
	unsigned long actual;
	int ret;
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);
	ret = dev_pm_opp_set_rate(dev, *freq);
	if (ret)
		return ret;
	mpp_dvfs_current(dev, &actual);
	/* CRU integer dividers may round SDK nominal rates. */
	if (abs((long)actual - (long)*freq) > *freq / 50) {
		dev_err(dev, "MPP OPP %lu Hz, actual %lu Hz\n", *freq, actual);
		return -ERANGE;
	}
	return 0;
}
void mpp_mainline_dvfs_remove(struct mpp_dev *mpp)
{
	struct mpp_mainline_dvfs *dvfs = mpp->mainline_dvfs;
	if (!dvfs)
		return;
	if (dvfs->cooling)
		devfreq_cooling_unregister(dvfs->cooling);
	if (dev_pm_qos_request_active(&dvfs->workload_max))
		dev_pm_qos_remove_request(&dvfs->workload_max);
	if (dvfs->df)
		devfreq_remove_device(dvfs->df);
	if (dvfs->opp_ready)
		dev_pm_opp_of_remove_table(mpp->dev);
	dev_pm_opp_clear_config(dvfs->opp_token);
	mpp->mainline_dvfs = NULL;
}
int mpp_mainline_dvfs_init(struct mpp_dev *mpp)
{
	static const char * const regulators[] = { "mpp", NULL };
	static const char * const clocks[] = { "clk_core", NULL };
	struct dev_pm_opp_config config = {
		.clk_names = clocks, .regulator_names = regulators,
	};
	struct device *dev = mpp->dev;
	struct mpp_mainline_dvfs *dvfs;
	int ret;
	if (!of_property_present(dev->of_node, "operating-points-v2"))
		return 0;
	dvfs = devm_kzalloc(dev, sizeof(*dvfs), GFP_KERNEL);
	if (!dvfs)
		return -ENOMEM;
	dvfs->clk = devm_clk_get(dev, "clk_core");
	if (IS_ERR(dvfs->clk))
		return PTR_ERR(dvfs->clk);
	ret = dev_pm_opp_set_config(dev, &config);
	if (ret < 0)
		return ret;
	dvfs->opp_token = ret;
	mpp->mainline_dvfs = dvfs;
	ret = dev_pm_opp_of_add_table(dev);
	if (ret)
		goto failed;
	dvfs->opp_ready = true;
	ret = dev_pm_qos_add_request(dev, &dvfs->workload_max,
				    DEV_PM_QOS_MAX_FREQUENCY, 300000);
	if (ret < 0)
		goto failed;
	dvfs->profile.initial_freq = clk_get_rate(dvfs->clk);
	dvfs->profile.target = mpp_dvfs_target;
	dvfs->profile.get_cur_freq = mpp_dvfs_current;
	dvfs->df = devfreq_add_device(dev, &dvfs->profile,
				     DEVFREQ_GOV_PERFORMANCE, NULL);
	if (IS_ERR(dvfs->df)) {
		ret = PTR_ERR(dvfs->df);
		dvfs->df = NULL;
		goto failed;
	}
	dvfs->cooling = of_devfreq_cooling_register(dev->of_node, dvfs->df);
	if (IS_ERR(dvfs->cooling)) {
		ret = PTR_ERR(dvfs->cooling);
		dvfs->cooling = NULL;
		goto failed;
	}
	dev_info(dev, "mainline OPP/devfreq/QoS/thermal registered\n");
	return 0;
failed:
	mpp_mainline_dvfs_remove(mpp);
	return dev_err_probe(dev, ret, "mainline MPP devfreq\n");
}
int mpp_mainline_dvfs_rate(struct mpp_dev *mpp, unsigned long rate)
{
	struct mpp_mainline_dvfs *dvfs = mpp->mainline_dvfs;
	int ret;
	if (!dvfs)
		return -EOPNOTSUPP;
	/* The performance governor selects the highest permitted OPP. Both
	 * workload and thermal requests constrain its maximum through QoS. */
	ret = dev_pm_qos_update_request(&dvfs->workload_max, rate / 1000);
	return ret < 0 ? ret : 0;
}
