// SPDX-License-Identifier: GPL-2.0-only
/* Local BL31 configuration bridge; suspend itself uses mainline PSCI/PM. */
#include <linux/arm-smccc.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

static int rk3568_pm_probe(struct platform_device *pdev)
{
	static const struct { const char *property; u32 command; } configs[] = {
		{ "rockchip,sleep-mode-config", 1 },
		{ "rockchip,wakeup-config", 2 },
		{ "rockchip,sleep-debug-en", 5 },
	};
	struct arm_smccc_res res;
	u32 value;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(configs); ++i) {
		ret = of_property_read_u32(pdev->dev.of_node, configs[i].property, &value);
		if (ret)
			return ret;
		arm_smccc_smc(0x82000003, configs[i].command, value, 0, 0, 0, 0, 0, &res);
		if (res.a0)
			return dev_err_probe(&pdev->dev, -EIO, "BL31 PM command %u failed: %lx\n",
					     configs[i].command, res.a0);
	}
	/* Terminate the optional firmware GPIO power-control list. */
	arm_smccc_smc(0x82000003, 4, 0, 0xffff, 0, 0, 0, 0, &res);
	if (res.a0)
		return -EIO;
	dev_info(&pdev->dev, "BL31 sleep mode and GPIO0 wake configured; mainline PSCI\n");
	return 0;
}
static const struct of_device_id rk3568_pm_match[] = {
	{ .compatible = "rockchip,rk3568-headless-pm" }, { }
};
MODULE_DEVICE_TABLE(of, rk3568_pm_match);
static struct platform_driver rk3568_pm_driver = {
	.probe = rk3568_pm_probe,
	.driver = { .name = "rk3568-headless-pm", .of_match_table = rk3568_pm_match },
};
module_platform_driver(rk3568_pm_driver);
MODULE_DESCRIPTION("Local RK3568 BL31 sleep configuration for mainline PSCI");
MODULE_LICENSE("GPL");
