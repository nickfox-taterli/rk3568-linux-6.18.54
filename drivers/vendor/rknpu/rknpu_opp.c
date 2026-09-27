// SPDX-License-Identifier: GPL-2.0
/* Minimal mainline OPP path for the RK3568 NPU's SCMI clock. */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/pm_opp.h>

#include "rknpu_drv.h"
#include "rknpu_devfreq.h"

int rknpu_devfreq_init(struct rknpu_device *rknpu_dev)
{
	static const char * const clocks[] = { "scmi_clk", NULL };
	static const char * const regulators[] = { "rknpu", NULL };
	struct device *dev = rknpu_dev->dev;
	struct dev_pm_opp_config config = {
		.clk_names = clocks,
		.regulator_names = regulators,
	};
	int ret;
	u8 grade, remark;

	if (!of_property_present(dev->of_node, "operating-points-v2"))
		return 0;
	ret = nvmem_cell_read_u8(dev, "npu-grade", &grade);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read NPU chip grade\n");
	ret = nvmem_cell_read_u8(dev, "npu-grade-remark", &remark);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to read NPU chip grade remark\n");
	if (remark)
		grade = remark;

	ret = dev_pm_opp_set_config(dev, &config);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to configure NPU OPP\n");
	rknpu_dev->opp_config_token = ret;

	ret = dev_pm_opp_of_add_table(dev);
	if (ret) {
		dev_pm_opp_clear_config(rknpu_dev->opp_config_token);
		return dev_err_probe(dev, ret, "failed to read NPU OPP table\n");
	}
	rknpu_dev->opp_table_ready = true;
	/* The BSP's 900 MHz supported-hw mask excludes M and J chip bins. */
	if (grade == 0x0d || grade == 0x0a) {
		ret = dev_pm_opp_disable(dev, 900000000);
		if (ret) {
			rknpu_devfreq_remove(rknpu_dev);
			return dev_err_probe(dev, ret,
					     "failed to exclude 900 MHz OPP\n");
		}
	}
	dev_info(dev, "NPU chip grade %#x; 900 MHz %s\n", grade,
		 grade == 0x0d || grade == 0x0a ? "excluded" : "eligible");
	dev_info(dev, "NPU OPP ready: SCMI %lu Hz, %d uV\n",
		 clk_get_rate(rknpu_dev->clks[0].clk),
		 rknpu_dev->vdd ? regulator_get_voltage(rknpu_dev->vdd) : -ENODEV);
	return 0;
}

void rknpu_devfreq_remove(struct rknpu_device *rknpu_dev)
{
	if (!rknpu_dev->opp_table_ready)
		return;
	rknpu_dev->opp_table_ready = false;
	dev_pm_opp_of_remove_table(rknpu_dev->dev);
	dev_pm_opp_clear_config(rknpu_dev->opp_config_token);
}

int rknpu_devfreq_set_rate(struct rknpu_device *rknpu_dev, unsigned long freq)
{
	struct device *dev = rknpu_dev->dev;
	struct dev_pm_opp *opp;
	unsigned long actual;
	long expected;
	int ret;

	if (!rknpu_dev->opp_table_ready)
		return -EOPNOTSUPP;

	opp = dev_pm_opp_find_freq_exact(dev, freq, true);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);

	ret = rknpu_power_get(rknpu_dev);
	if (ret < 0)
		return ret;

	expected = clk_round_rate(rknpu_dev->clks[0].clk, freq);
	if (expected <= 0) {
		ret = expected ? expected : -ERANGE;
		goto out;
	}
	ret = dev_pm_opp_set_rate(dev, freq);
	actual = clk_get_rate(rknpu_dev->clks[0].clk);
	if (!ret && actual != expected)
		ret = -ERANGE;
	dev_info(dev, "NPU OPP request %lu Hz: rounded %ld Hz, SCMI %lu Hz, %d uV, ret=%d\n",
		 freq, expected, actual,
		 rknpu_dev->vdd ? regulator_get_voltage(rknpu_dev->vdd) : -ENODEV,
		 ret);
out:
	rknpu_power_put(rknpu_dev);
	return ret;
}
