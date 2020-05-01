/*
 *
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <soc/mstar/pmsleep.h>
#include <linux/mfd/syscon.h>
#include <linux/gpio/consumer.h>

static struct regmap *pmsleep;
struct gpio_desc *gpio;

static void mstar_poweroff(void)
{
	/* no idea what this actually does */
	regmap_write(pmsleep, MSTAR_PMSLEEP_REGC8, MSTAR_PMSLEEP_REGC8_MAGIC);
	regmap_write(pmsleep, MSTAR_PMSLEEP_REGCC, MSTAR_PMSLEEP_REGCC_MAGIC);

	/* set wake up source */
	regmap_update_bits(pmsleep, MSTAR_PMSLEEP_WAKEUPSOURCE,
				MSTAR_PMSLEEP_WAKEUPSOURCE_RTC, 0);

	regmap_update_bits(pmsleep, MSTAR_PMSLEEP_REG70,
			MSTAR_PMSLEEP_REG70_ISOEN2GPIO4 |
			MSTAR_PMSLEEP_REG70_LINKWKINT2GPIO4, 0xffff);

	/* unlock powerdown and set the gpio */
	regmap_write(pmsleep, MSTAR_PMSLEEP_PMLOCK,
			MSTAR_PMSLEEP_PMLOCK_UNLOCK);
	regmap_update_bits(pmsleep, MSTAR_PMSLEEP_REG24,
			MSTAR_PMSLEEP_REG24_POWEROFF, MSTAR_PMSLEEP_REG24_POWEROFF);
	gpiod_direction_output(gpio, 1);
	while (1);
}

static int __init mstar_poweroff_probe(struct platform_device *pdev)
{
	int ret = 0;

	pmsleep = syscon_regmap_lookup_by_phandle(pdev->dev.of_node, "mstar,pmsleep");
	if(IS_ERR(pmsleep)){
		ret = PTR_ERR(pmsleep);
		goto out;
	}

	gpio = gpiod_get_from_of_node(pdev->dev.of_node, "gpio", 0, GPIOD_ASIS,
			"powerdown");
	if(IS_ERR(gpio)){
		ret = PTR_ERR(gpio);
		goto out;
	}

	pm_power_off = mstar_poweroff;

out:
	return ret;
}

static int __exit mstar_poweroff_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id mstar_poweroff_of_match[] = {
	{ .compatible = "mstar,msc313-poweroff", },
	{ /*sentinel*/ }
};
MODULE_DEVICE_TABLE(of, mstar_poweroff_of_match);

static struct platform_driver mstar_poweroff_driver = {
	.remove = __exit_p(mstar_poweroff_remove),
	.driver = {
		.name = "mstar-poweroff",
		.of_match_table = mstar_poweroff_of_match,
	},
};
module_platform_driver_probe(mstar_poweroff_driver, mstar_poweroff_probe);

MODULE_AUTHOR("Daniel Palmer");
MODULE_DESCRIPTION("Shutdown driver for MStar SoCs");
MODULE_LICENSE("GPL v2");
