// SPDX-License-Identifier: GPL-2.0
/* RK3568 SDK ABI with mainline OPP, devfreq and thermal cooling. */
#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/devfreq_cooling.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include "rknpu_drv.h"
#include "rknpu_devfreq.h"

static int npu_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct rknpu_device *npu = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	unsigned long actual;
	int ret;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);
		ret = dev_pm_opp_set_rate(dev, *freq);
	actual = clk_get_rate(npu->clks[0].clk);
	/* SCMI PVTPLL rates can differ slightly from its enumerated PLL bins. */
	if (!ret && (actual < *freq * 98 / 100 || actual > *freq * 102 / 100)) {
		dev_err(dev, "OPP request %lu Hz, SCMI readback %lu Hz\n", *freq, actual);
		return -ERANGE;
	}
	return ret;
}

static int npu_status(struct device *dev, struct devfreq_dev_status *stat)
{
	struct rknpu_device *npu = dev_get_drvdata(dev);
	struct rknpu_job *job;
	unsigned long irqflags;
	u64 now, busy, total;

	/* Core 0 cumulative accounting also includes the currently running job.
	 * The load/debug timer keeps its original one-second counters. */
	spin_lock_irqsave(&npu->irq_lock, irqflags);
	now = ktime_get_ns();
	busy = atomic64_read(&npu->dvfs_busy_ns);
	job = npu->subcore_datas[0].job;
	if (job)
		busy += now - ktime_to_ns(job->hw_recoder_time);
	spin_unlock_irqrestore(&npu->irq_lock, irqflags);
	total = now - npu->dvfs_last_time;
	stat->total_time = max_t(u64, total / 1000, 1);
	stat->busy_time = min_t(u64, (busy > npu->dvfs_last_busy ? busy - npu->dvfs_last_busy : 0) / 1000,
				stat->total_time);
	stat->current_frequency = clk_get_rate(npu->clks[0].clk);
	npu->dvfs_last_time = now;
	npu->dvfs_last_busy = busy;
	return 0;
}

int rknpu_devfreq_init(struct rknpu_device *npu)
{
	static const char * const clocks[] = { "scmi_clk", NULL };
	static const char * const regulators[] = { "rknpu", NULL };
	struct device *dev = npu->dev;
	struct dev_pm_opp_config config = {
		.clk_names = clocks, .regulator_names = regulators,
	};
	struct devfreq_dev_profile *profile = &npu->mainline_profile;
	unsigned long freq;
	struct dev_pm_opp *opp;
	u8 grade, remark;
	int ret;

	if (!of_property_present(dev->of_node, "operating-points-v2"))
		return 0;
	if (npu->config->num_irqs != 1)
		return -EOPNOTSUPP;
	ret = nvmem_cell_read_u8(dev, "npu-grade", &grade);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read NPU grade\n");
	ret = nvmem_cell_read_u8(dev, "npu-grade-remark", &remark);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read NPU remark\n");
	if (remark)
		grade = remark;
	ret = dev_pm_opp_set_config(dev, &config);
	if (ret < 0)
		return ret;
	npu->opp_config_token = ret;
	ret = dev_pm_opp_of_add_table(dev);
	if (ret) {
		dev_pm_opp_clear_config(npu->opp_config_token);
		return ret;
	}
	npu->opp_table_ready = true;
	/* Preserve the existing port's conservative M/J high-rate exclusion. */
	if (grade == 0x0d || grade == 0x0a) {
		freq = 900000000;
		while (!IS_ERR(opp = dev_pm_opp_find_freq_ceil(dev, &freq))) {
			dev_pm_opp_put(opp);
			dev_pm_opp_disable(dev, freq++);
		}
	}
	ret = dev_pm_opp_set_rate(dev, 600000000);
	if (ret)
		goto err;
	atomic64_set(&npu->dvfs_busy_ns, 0);
	npu->dvfs_last_time = ktime_get_ns();
	profile->initial_freq = 600000000;
	profile->polling_ms = 100;
	profile->target = npu_target;
	profile->get_dev_status = npu_status;
	npu->mainline_devfreq = devfreq_add_device(dev, profile,
						 DEVFREQ_GOV_SIMPLE_ONDEMAND, NULL);
	if (IS_ERR(npu->mainline_devfreq)) {
		ret = PTR_ERR(npu->mainline_devfreq);
		npu->mainline_devfreq = NULL;
		goto err;
	}
	ret = dev_pm_qos_add_request(dev, &npu->manual_min,
				    DEV_PM_QOS_MIN_FREQUENCY, 0);
	if (ret < 0)
		goto err;
	ret = dev_pm_qos_add_request(dev, &npu->manual_max,
				    DEV_PM_QOS_MAX_FREQUENCY,
				    PM_QOS_MAX_FREQUENCY_DEFAULT_VALUE);
	if (ret < 0)
		goto err;
	npu->mainline_cooling = of_devfreq_cooling_register(dev->of_node,
							 npu->mainline_devfreq);
	if (IS_ERR(npu->mainline_cooling)) {
		ret = PTR_ERR(npu->mainline_cooling);
		npu->mainline_cooling = NULL;
		goto err;
	}
	dev_info(dev, "mainline devfreq ready, grade %#x, SCMI %lu Hz, %d uV\n",
		 grade, clk_get_rate(npu->clks[0].clk), regulator_get_voltage(npu->vdd));
	return 0;
err:
	rknpu_devfreq_remove(npu);
	return ret;
}

void rknpu_devfreq_remove(struct rknpu_device *npu)
{
	if (npu->mainline_cooling) {
		devfreq_cooling_unregister(npu->mainline_cooling);
		npu->mainline_cooling = NULL;
	}
	if (dev_pm_qos_request_active(&npu->manual_max))
		dev_pm_qos_remove_request(&npu->manual_max);
	if (dev_pm_qos_request_active(&npu->manual_min))
		dev_pm_qos_remove_request(&npu->manual_min);
	if (npu->mainline_devfreq) {
		devfreq_remove_device(npu->mainline_devfreq);
		npu->mainline_devfreq = NULL;
	}
	if (npu->opp_table_ready) {
		npu->opp_table_ready = false;
		dev_pm_opp_of_remove_table(npu->dev);
		dev_pm_opp_clear_config(npu->opp_config_token);
	}
}

int rknpu_devfreq_set_rate(struct rknpu_device *npu, unsigned long freq)
{
	struct dev_pm_opp *opp;
	int ret;

	if (!npu->mainline_devfreq)
		return -EOPNOTSUPP;
	if (freq) {
		opp = dev_pm_opp_find_freq_exact(npu->dev, freq, true);
		if (IS_ERR(opp))
			return PTR_ERR(opp);
		dev_pm_opp_put(opp);
	}
	/* Manual constraints remain subordinate to thermal's maximum QoS.
	 * Writing zero releases them and resumes automatic load control. */
	ret = dev_pm_qos_update_request(&npu->manual_min, freq / 1000);
	if (ret < 0)
		return ret;
	ret = dev_pm_qos_update_request(&npu->manual_max,
			freq ? freq / 1000 : PM_QOS_MAX_FREQUENCY_DEFAULT_VALUE);
	return ret < 0 ? ret : 0;
}

int rknpu_devfreq_runtime_suspend(struct device *dev)
{
	struct rknpu_device *npu = dev_get_drvdata(dev);

	return npu->mainline_devfreq ? devfreq_suspend_device(npu->mainline_devfreq) : 0;
}

int rknpu_devfreq_runtime_resume(struct device *dev)
{
	struct rknpu_device *npu = dev_get_drvdata(dev);

	return npu->mainline_devfreq ? devfreq_resume_device(npu->mainline_devfreq) : 0;
}
