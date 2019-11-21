// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Injoinic IP6XXX regulators driver.
 *
 * Copyright (C) 2019 <daniel@thingy.jp>
 *
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/i2c.h>

struct ip6xxx_regulator_data {
	const struct regulator_desc *regulators;
	const int nregulators;
};

static const struct regulator_ops ip6xxx_ops = {
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.list_voltage = regulator_list_voltage_linear,
};

/* IP6303 */
#define IP6303_DC_CTL		0x20
#define IP6303_DC1_VSET		0x21
#define IP6303_DC2_VSET		0x26
#define IP6303_DC3_VSET		0x2b
#define IP6303_LDO_EN		0x40
#define IP6303_LDO3_VSEL	0x43
#define IP6303_LDO4_VSEL	0x44
#define IP6303_LDO5_VSEL	0x45
#define IP6303_LDO6_VSEL	0x46
#define IP6303_LDO7_VSEL	0x47

#define IP6303_DCDC_MIN_UV	600000
#define IP6303_DCDC_STEP_UV	12500
#define IP6303_DCDC_MAX_UV	3600000
#define IP6303_DCDC_VSEL_MASK	0xff
#define IP6303_DC1_EN_MASK	BIT(0)
#define IP6303_DC2_EN_MASK	BIT(1)
#define IP6303_DC3_EN_MASK	BIT(2)

#define IP6303_LDO_MIN_UV	700000
#define IP6303_LDO_STEP_UV	25000
#define IP6303_LDO_MAX_UV	3400000
#define IP6303_LDO_VSEL_MASK	0x7f

/* aka SVCC */
#define IP6303_SLDO1_MIN_UV	2600000
#define IP6303_SLDO1_STEP_UV	100000
#define IP6303_SLDO1_MAX_UV	3300000
#define IP6303_SLDO1_VSEL	0x4D
#define IP6303_SLDO1_VSEL_MASK	0x07

#define IP6303_SLDO2_MIN_UV	700000
#define IP6303_SLDO2_STEP_UV	100000
#define IP6303_SLDO2_MAX_UV	3800000
#define IP6303_SLDO2_VSEL	0x4D
#define IP6303_SLDO2_VSEL_MASK	0xF8

#define IP6303_SLDO2_EN_MASK	BIT(1)
#define IP6303_LDO2_EN_MASK	BIT(2)
#define IP6303_LDO3_EN_MASK	BIT(3)
#define IP6303_LDO4_EN_MASK	BIT(4)
#define IP6303_LDO5_EN_MASK	BIT(5)
#define IP6303_LDO6_EN_MASK	BIT(6)
#define IP6303_LDO7_EN_MASK	BIT(7)

#define IP6XXX_REGULATOR(_name,_id,_vset, _vsetmask, _min,_step,_max, _en, _enmask) { \
				.owner = THIS_MODULE, \
				.type = REGULATOR_VOLTAGE, \
				.ramp_delay = 200, \
				.ops = &ip6xxx_ops, \
				.min_uV =  _min, \
				.uV_step = _step, \
				.n_voltages = ((_max - _min) / _step) + 1, \
				.vsel_mask = _vsetmask, \
				.name = _name, \
				.of_match = of_match_ptr(_name), \
				.regulators_node= of_match_ptr("regulators"), \
				.id = _id, \
				.vsel_reg = _vset, \
				.enable_reg = _en, \
				.enable_mask = _enmask, \
				.enable_val = _enmask, \
				.disable_val = 0, \
			}

#define IP6XXX_DCDC_REGULATOR(_name,_id,_vset,_enmask) IP6XXX_REGULATOR(_name,_id,_vset, \
		IP6303_DCDC_VSEL_MASK, \
		IP6303_DCDC_MIN_UV, \
		IP6303_DCDC_STEP_UV, \
		IP6303_DCDC_MAX_UV, \
		IP6303_DC_CTL, \
		_enmask)

#define IP6XXX_LDO_REGULATOR(_name,_id,_vset, _enmask) IP6XXX_REGULATOR(_name,_id,_vset, \
		IP6303_LDO_VSEL_MASK, \
		IP6303_LDO_MIN_UV, \
		IP6303_LDO_STEP_UV, \
		IP6303_LDO_MAX_UV, \
		IP6303_LDO_EN, \
		_enmask)

static const struct regulator_desc ip6303_regulators[] = {
		IP6XXX_DCDC_REGULATOR("dc1", 0, IP6303_DC1_VSET, IP6303_DC1_EN_MASK),
		IP6XXX_DCDC_REGULATOR("dc2", 1, IP6303_DC2_VSET, IP6303_DC2_EN_MASK),
		IP6XXX_DCDC_REGULATOR("dc3", 2, IP6303_DC3_VSET, IP6303_DC3_EN_MASK),
		IP6XXX_REGULATOR("sldo1", 3, IP6303_SLDO1_VSEL, \
				IP6303_SLDO1_VSEL_MASK, IP6303_SLDO1_MIN_UV, \
				IP6303_SLDO1_STEP_UV,IP6303_SLDO1_MAX_UV, 0, 0),
		IP6XXX_REGULATOR("sldo2", 4, IP6303_SLDO2_VSEL, \
				IP6303_SLDO2_VSEL_MASK, IP6303_SLDO2_MIN_UV, \
				IP6303_SLDO2_STEP_UV,IP6303_SLDO2_MAX_UV, IP6303_LDO_EN, IP6303_SLDO2_EN_MASK),
		IP6XXX_LDO_REGULATOR("ldo3", 5, IP6303_LDO3_VSEL, IP6303_LDO2_EN_MASK),
		IP6XXX_LDO_REGULATOR("ldo4", 6, IP6303_LDO4_VSEL, IP6303_LDO2_EN_MASK),
		IP6XXX_LDO_REGULATOR("ldo5", 7, IP6303_LDO5_VSEL, IP6303_LDO2_EN_MASK),
		IP6XXX_LDO_REGULATOR("ldo6", 8, IP6303_LDO6_VSEL, IP6303_LDO2_EN_MASK),
		IP6XXX_LDO_REGULATOR("ldo7", 9, IP6303_LDO7_VSEL, IP6303_LDO2_EN_MASK),
};

static const struct ip6xxx_regulator_data ip6303_data = {
	.regulators = ip6303_regulators,
	.nregulators = ARRAY_SIZE(ip6303_regulators),
};

/* */

static const struct regmap_config ip6xxx_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int ip6xxx_i2c_probe(struct i2c_client *i2c,
			    const struct i2c_device_id *id)
{
	struct device *dev = &i2c->dev;
	const struct ip6xxx_regulator_data *match_data;
	struct regulator_dev *rdev;
	struct regulator_config config = { 0 };
	int i;

	match_data = of_device_get_match_data(&i2c->dev);
	if (!match_data)
		return -EINVAL;

	config.dev = dev;
	config.of_node = dev->of_node;
	config.regmap = devm_regmap_init_i2c(i2c, &ip6xxx_regmap_config);
	if(IS_ERR(config.regmap)){
		dev_err(dev, "failed to get regmap");
		return PTR_ERR(config.regmap);
	}

	for(i = 0; i < match_data->nregulators; i++){
		rdev = devm_regulator_register(&i2c->dev,
					       &match_data->regulators[i],
					       &config);
		if (IS_ERR(rdev)) {
			dev_err(&i2c->dev, "Failed to register regulator\n");
			return PTR_ERR(rdev);
		}
	}

	return 0;
}

static const struct of_device_id ip6xxx_i2c_of_match[] = {
	{
		.compatible = "injoinic,ip6303-regulator",
		.data = &ip6303_data,
	},
	{ },
};
MODULE_DEVICE_TABLE(of, ip6xxx_i2c_of_match);

static const struct i2c_device_id ip6xxx_i2c_id[] = {
	{ "ip6303", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, i2c_device_id);

static struct i2c_driver ip6xxx_regulator_driver = {
	.driver = {
		.name = "ip6xxx",
		.of_match_table	= of_match_ptr(ip6xxx_i2c_of_match),
	},
	.probe = ip6xxx_i2c_probe,
	.id_table = ip6xxx_i2c_id,
};

module_i2c_driver(ip6xxx_regulator_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Daniel Palmer <daniel@thingy.jp>");
MODULE_DESCRIPTION("Regulator Driver for IP6XXX PMIC");
MODULE_ALIAS("platform:ip6xxx-regulator");
