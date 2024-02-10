// SPDX-License-Identifier: GPL-2.0
/*
 *
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gpio/driver.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/regmap.h>

#define GPIOPERPORT				8
#define PORTS					7
#define NGPIO					(GPIOPERPORT * PORTS)

#define PORT_OFFSET				0x8
#define REG_DIRDATA				0x0
#define REG_DIRDATA_DIR_SHIFT	8
#define REG_DIRDATA_DIR_MASK	0xff
#define REG_PUENSEL				0x2

#define PORTD	3

#define gpio_to_port(__gpio) (__gpio >> 3)
#define gpio_to_portoffset(_gpio) (gpio_to_port(_gpio) * PORT_OFFSET)
#define gpio_to_bitmask(_gpio) (BIT(_gpio & 0x7))

struct dragonball_gpio {
	void __iomem		*base;
	struct gpio_chip	gc;
};

static void dragonball_gpio_set(struct gpio_chip *chip, unsigned int offset, int value)
{
	struct dragonball_gpio *gpio = gpiochip_get_data(chip);
	unsigned int port = gpio_to_portoffset(offset);
	u16 mask = gpio_to_bitmask(offset);
	u16 reg;

	reg = readw(gpio->base + port + REG_DIRDATA);
	reg &= ~mask;
	if (value)
		reg |= mask;
	writew(reg, gpio->base + port + REG_DIRDATA);
}

static int dragonball_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	return 0;
}

static int dragonball_gpio_direction_input(struct gpio_chip *chip, unsigned int offset)
{
	struct dragonball_gpio *gpio = gpiochip_get_data(chip);
	unsigned int port = gpio_to_portoffset(offset);
	u16 mask = gpio_to_bitmask(offset) << REG_DIRDATA_DIR_SHIFT;
	u16 reg;

	reg = readw(gpio->base + port + REG_DIRDATA);
	reg &= ~mask;
	writew(reg, gpio->base + port + REG_DIRDATA);

	return 0;
}

static int dragonball_gpio_direction_output(struct gpio_chip *chip, unsigned int offset, int value)
{
	struct dragonball_gpio *gpio = gpiochip_get_data(chip);
	unsigned int port = gpio_to_portoffset(offset);
	u16 mask = gpio_to_bitmask(offset) << REG_DIRDATA_DIR_SHIFT;
	u16 reg;

	reg = readw(gpio->base + port + REG_DIRDATA);
	reg |= mask;
	writew(reg, gpio->base + port + REG_DIRDATA);

	return 0;
}

static int dragonball_gpio_of_xlate(struct gpio_chip *gc,
				const struct of_phandle_args *gpiospec,
				u32 *flags)
{
	int gpionum;
	uint32_t port;
	uint32_t pin;

	if (gpiospec->args_count != 3)
		return -EINVAL;

	port = gpiospec->args[0];
	pin = gpiospec->args[1];

	gpionum = (GPIOPERPORT * port) + pin;
	if(gpionum >= gc->ngpio)
		return -EINVAL;

	if (flags)
		*flags = gpiospec->args[2];

	return gpionum;
}

static int dragonball_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dragonball_gpio *gpio;
	struct gpio_chip *gpiochip;

	gpio = devm_kzalloc(dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	gpio->base = devm_platform_ioremap_resource(pdev, 0);
	if (!gpio->base) {
		dev_err(dev, "failed to allocate device memory\n");
		return -ENOMEM;
	}

	gpiochip = &gpio->gc;

	gpiochip->label = dev_name(dev);
	gpiochip->base = -1;
	gpiochip->ngpio = NGPIO;
	gpiochip->parent = dev;
	gpiochip->owner = THIS_MODULE;
	gpiochip->request = gpiochip_generic_request;
	gpiochip->free = gpiochip_generic_free;
	gpiochip->direction_input = dragonball_gpio_direction_input;
	gpiochip->direction_output = dragonball_gpio_direction_output;
	gpiochip->get = dragonball_gpio_get;
	gpiochip->set = dragonball_gpio_set;
	gpiochip->of_gpio_n_cells = 3;
	gpiochip->of_xlate = dragonball_gpio_of_xlate;

	platform_set_drvdata(pdev, gpio);
	return devm_gpiochip_add_data(dev, gpiochip, gpio);
}

static const struct of_device_id dragonball_gpio_match[] = {
	{ .compatible = "motorola,mc68ez328-gpio" },
	{ },
};

static struct platform_driver dragonball_gpio_driver = {
	.probe		= dragonball_gpio_probe,
	.driver = {
		.name	= "dragonball_gpio",
		.of_match_table = dragonball_gpio_match,
	},
};
module_platform_driver(dragonball_gpio_driver)

MODULE_AUTHOR("Daniel Palmer <daniel@thingy.jp");
MODULE_DESCRIPTION("DragonBall GPIO driver");
MODULE_LICENSE("GPL");
