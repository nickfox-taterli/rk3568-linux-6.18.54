// SPDX-License-Identifier: GPL-2.0-only
/* Headless RK3568: mainline devfreq/DFI with the BL31 DDR mailbox ABI.
 * Frequencies come from trained firmware FSPs, never an invented DDR OPP.
 */
#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/devfreq.h>
#include <linux/devfreq-event.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/pm_qos.h>

#define SIP_DRAM 0x82000008
#define SIP_SHARE 0x82000009
#define DDR_PAGE 2
#define DDR_INIT 0
#define DDR_SET 1
#define DDR_GET_RATE 5
#define DDR_VERSION 8
#define DDR_POST 9
#define DDR_MCU_START 12
#define DDR_FREQ_INFO 14

/* Firmware ABI, matching the SDK's share_params. */
struct rk3568_ddr_mailbox {
	u32 hz, lcdc_type, vop, vop_dclk_mode, sr_idle_en, addr_mcu_el3;
	u32 wait_flag1, wait_flag0, complt_hwirq, update_drv_odt_cfg, update_deskew_cfg;
	u32 freq_count, freq_info_mhz[6], wait_mode, vop_scan_line_time_ns;
};
struct rk3568_dmc {
	struct device *dev;
	struct clk *clk;
	struct devfreq *df;
	struct devfreq_event_dev *event;
	struct devfreq_dev_profile profile;
	struct devfreq_simple_ondemand_data policy;
	struct rk3568_ddr_mailbox __iomem *mailbox;
	struct completion complete;
	struct pm_qos_request latency;
	int irq;
	bool event_enabled;
};
static struct arm_smccc_res ddr_smc(u32 arg, u32 cmd)
{
	struct arm_smccc_res res;
	arm_smccc_smc(SIP_DRAM, arg, 0, cmd, 0, 0, 0, 0, &res);
	return res;
}
/* SCMI caches a rate changed through the separate DDR SMC mailbox. */
static unsigned long dmc_rate(struct rk3568_dmc *dmc)
{
	struct arm_smccc_res res = ddr_smc(DDR_PAGE, DDR_GET_RATE);

	return res.a0 ? 0 : res.a1;
}
static int dmc_get_rate(struct device *dev, unsigned long *freq)
{
	*freq = dmc_rate(dev_get_drvdata(dev));
	return *freq ? 0 : -EIO;
}
static irqreturn_t ddr_done(int irq, void *data)
{
	struct rk3568_dmc *dmc = data;
	struct arm_smccc_res res = ddr_smc(DDR_PAGE, DDR_POST);
	if (res.a0)
		dev_err(dmc->dev, "DDR completion failed: %ld\n", (long)res.a0);
	complete(&dmc->complete);
	return IRQ_HANDLED;
}
static int dmc_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct rk3568_dmc *dmc = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	struct arm_smccc_res res;
	unsigned long actual;
	int ret = 0;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);
	if (dmc_rate(dmc) == *freq)
		return 0;
	writel(*freq, &dmc->mailbox->hz);
	writel(1, &dmc->mailbox->wait_flag1);
	writel(1, &dmc->mailbox->wait_flag0);
	wmb();
	res = ddr_smc(DDR_PAGE, DDR_SET);
	dev_dbg(dev, "DDR SET status=%#lx wait=%#lx\n", res.a0, res.a1);
	if (res.a0)
		return -EIO;
	if ((s32)res.a1 == -6) {
		reinit_completion(&dmc->complete);
		enable_irq(dmc->irq);
		cpu_latency_qos_update_request(&dmc->latency, 0);
		res = ddr_smc(0, DDR_MCU_START);
		if (res.a0)
			ret = -EIO;
		else if (!wait_for_completion_timeout(&dmc->complete, msecs_to_jiffies(100)))
			ret = -ETIMEDOUT;
		if (ret)
			ddr_smc(DDR_PAGE, DDR_POST);
		cpu_latency_qos_update_request(&dmc->latency, PM_QOS_DEFAULT_VALUE);
		disable_irq(dmc->irq);
	}
	actual = dmc_rate(dmc);
	if (!ret && actual != *freq)
		ret = -ERANGE;
	dev_info(dev, "DDR request %lu Hz, firmware %lu Hz, ret=%d\n", *freq, actual, ret);
	return ret;
}
static int dmc_status(struct device *dev, struct devfreq_dev_status *stat)
{
	struct rk3568_dmc *dmc = dev_get_drvdata(dev);
	struct devfreq_event_data event;
	int ret = devfreq_event_get_event(dmc->event, &event);
	if (ret)
		return ret;
	stat->current_frequency = dmc_rate(dmc);
	stat->total_time = event.total_count;
	stat->busy_time = min(event.load_count, event.total_count);
	return 0;
}
static void dmc_cleanup(void *data)
{
	struct rk3568_dmc *dmc = data;
	if (dmc->df)
		devfreq_remove_device(dmc->df);
	if (dmc->event_enabled)
		devfreq_event_disable_edev(dmc->event);
	dev_pm_opp_remove_all_dynamic(dmc->dev);
	cpu_latency_qos_remove_request(&dmc->latency);
}
static int dmc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rk3568_dmc *dmc;
	struct arm_smccc_res res;
	u32 count, mhz;
	int ret, i;

	dmc = devm_kzalloc(dev, sizeof(*dmc), GFP_KERNEL);
	if (!dmc)
		return -ENOMEM;
	dmc->dev = dev;
	platform_set_drvdata(pdev, dmc);
	dmc->clk = devm_clk_get(dev, "dmc");
	if (IS_ERR(dmc->clk))
		return dev_err_probe(dev, PTR_ERR(dmc->clk), "DDR SCMI clock\n");
	dmc->event = devfreq_event_get_edev_by_phandle(dev, "devfreq-events", 0);
	if (IS_ERR(dmc->event))
		return dev_err_probe(dev, PTR_ERR(dmc->event), "DDR DFI event\n");
	res = ddr_smc(0, DDR_VERSION);
	if (res.a0 || res.a1 < 0x101)
		return dev_err_probe(dev, -EOPNOTSUPP, "BL31 DDR ABI unavailable\n");
	arm_smccc_smc(SIP_SHARE, 2, DDR_PAGE, 0, 0, 0, 0, 0, &res);
	if (res.a0 || !res.a1 || !IS_ALIGNED(res.a1, PAGE_SIZE))
		return dev_err_probe(dev, -EIO, "BL31 DDR shared pages unavailable\n");
	dev_info(dev, "BL31 DDR ABI, mailbox physical %#lx\n", res.a1);
	dmc->mailbox = devm_ioremap(dev, res.a1, 8192);
	if (!dmc->mailbox)
		return -ENOMEM;
	memset_io(dmc->mailbox, 0, 8192);
	dmc->irq = platform_get_irq(pdev, 0);
	if (dmc->irq < 0)
		return dmc->irq;
	init_completion(&dmc->complete);
	ret = devm_request_irq(dev, dmc->irq, ddr_done, IRQF_NO_AUTOEN, dev_name(dev), dmc);
	if (ret)
		return ret;
	res = ddr_smc(DDR_PAGE, DDR_INIT);
	if (res.a0)
		return dev_err_probe(dev, -EIO, "BL31 DDR init failed\n");
	res = ddr_smc(DDR_PAGE, DDR_FREQ_INFO);
	if (res.a0)
		return -EIO;
	count = readl(&dmc->mailbox->freq_count);
	if (!count || count > 6)
		return -EINVAL;
	ret = devfreq_event_enable_edev(dmc->event);
	if (ret)
		return ret;
	dmc->event_enabled = true;
	cpu_latency_qos_add_request(&dmc->latency, PM_QOS_DEFAULT_VALUE);
	ret = devm_add_action_or_reset(dev, dmc_cleanup, dmc);
	if (ret)
		return ret;
	for (i = 0; i < count; ++i) {
		mhz = readl(&dmc->mailbox->freq_info_mhz[i]);
		if (!mhz || mhz > 3000)
			return -EINVAL;
		ret = dev_pm_opp_add(dev, mhz * 1000000UL, 0);
		if (ret)
			return ret;
		dev_info(dev, "trained DDR FSP %u MHz\n", mhz);
	}
	dmc->profile.initial_freq = dmc_rate(dmc);
	dmc->profile.polling_ms = 100;
	dmc->profile.target = dmc_target;
	dmc->profile.get_dev_status = dmc_status;
	dmc->profile.get_cur_freq = dmc_get_rate;
	dmc->policy.upthreshold = 40;
	dmc->policy.downdifferential = 20;
	/* Keep boot performance until lower FSPs have been checked on this board. */
	dmc->df = devfreq_add_device(dev, &dmc->profile, DEVFREQ_GOV_PERFORMANCE, &dmc->policy);
	if (IS_ERR(dmc->df)) {
		ret = PTR_ERR(dmc->df);
		dmc->df = NULL;
		return ret;
	}
	return 0;
}
static int dmc_suspend(struct device *dev)
{
	struct rk3568_dmc *dmc = dev_get_drvdata(dev);
	int ret = devfreq_suspend_device(dmc->df);

	if (ret)
		return ret;
	ret = devfreq_event_disable_edev(dmc->event);
	if (ret)
		devfreq_resume_device(dmc->df);
	else
		dmc->event_enabled = false;
	return ret;
}
static int dmc_resume(struct device *dev)
{
	struct rk3568_dmc *dmc = dev_get_drvdata(dev);
	struct devfreq_event_data baseline;
	int ret = devfreq_event_enable_edev(dmc->event);

	if (ret)
		return ret;
	dmc->event_enabled = true;
	/* Discard the counter delta across firmware resetting the center. */
	devfreq_event_get_event(dmc->event, &baseline);
	ret = devfreq_resume_device(dmc->df);
	if (ret) {
		devfreq_event_disable_edev(dmc->event);
		dmc->event_enabled = false;
	}
	return ret;
}
static DEFINE_SIMPLE_DEV_PM_OPS(dmc_pm, dmc_suspend, dmc_resume);
static const struct of_device_id dmc_match[] = {
	{ .compatible = "rockchip,rk3568-headless-dmc" }, { }
};
MODULE_DEVICE_TABLE(of, dmc_match);
static struct platform_driver dmc_driver = {
	.probe = dmc_probe,
	.driver = { .name = "rk3568-headless-dmc", .of_match_table = dmc_match, .pm = pm_sleep_ptr(&dmc_pm) },
};
module_platform_driver(dmc_driver);
MODULE_DESCRIPTION("RK3568 headless mainline devfreq/DFI DDR scaling");
MODULE_LICENSE("GPL");
