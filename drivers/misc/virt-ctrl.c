// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cpuidle.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/io.h>
#include <linux/pm.h>

#define VIRT_CTRL_REG_FEATURES	0x00
#define VIRT_CTRL_REG_CMD	0x04

enum {
	CMD_NOOP,
	CMD_RESET,
	CMD_HALT,
	CMD_PANIC,
};

struct virt_ctrl {
	void __iomem *base;
};

static struct virt_ctrl *virt_ctrl = NULL;

static void virt_halt(void)
{
	iowrite32(CMD_HALT, virt_ctrl->base + VIRT_CTRL_REG_CMD);
	local_irq_disable();
	while (1)
		;
}

static int virt_reset(struct notifier_block *nb,
			unsigned long action, void *data)
{
	iowrite32(CMD_RESET, virt_ctrl->base + VIRT_CTRL_REG_CMD);

	return NOTIFY_DONE;
}

static struct notifier_block virt_reboot_nb = {
	.notifier_call = virt_reset,
	.priority = 192,
};

static int virt_ctrl_probe(struct platform_device *pdev)
{
	struct virt_ctrl* ctrl;
	int ret;

	printk("virt probe\n");

	if (virt_ctrl)
		return -EBUSY;

	ctrl = devm_kzalloc(&pdev->dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	ctrl->base = devm_platform_get_and_ioremap_resource(pdev, 0, NULL);
	if (IS_ERR(ctrl->base))
		return PTR_ERR(ctrl->base);

	platform_set_drvdata(pdev, ctrl);

	ret = register_restart_handler(&virt_reboot_nb);
	if (ret)
		return ret;

	pm_power_off = virt_halt;
	virt_ctrl = ctrl;

	return 0;
}

static const struct of_device_id virt_ctrl_of_match_table[] = {
	{ .compatible = "qemu,virt-ctrl" },
	{}
};

static struct platform_driver virt_ctrl_driver = {
	.probe		= virt_ctrl_probe,
	.driver = {
		.name	= "virt-ctrl",
		.of_match_table = virt_ctrl_of_match_table,
	},
};

module_platform_driver(virt_ctrl_driver);

MODULE_DESCRIPTION("QEMU virt-ctrl driver");
MODULE_LICENSE("GPL");
