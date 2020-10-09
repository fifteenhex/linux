// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Daniel Palmer<daniel@thingy.jp>
 */

#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/gpio/driver.h>

#include <dt-bindings/gpio/msc313-gpio.h>

#define DRIVER_NAME "gpio-msc313"

#define MSC313_GPIO_IN  BIT(0)
#define MSC313_GPIO_OUT BIT(4)
#define MSC313_GPIO_OEN BIT(5)

#define MSC313_GPIO_BITSTOSAVE (MSC313_GPIO_OUT | MSC313_GPIO_OEN)

#define FUART_NAMES			\
	MSC313_PINNAME_FUART_RX,	\
	MSC313_PINNAME_FUART_TX,	\
	MSC313_PINNAME_FUART_CTS,	\
	MSC313_PINNAME_FUART_RTS

#define OFF_FUART_RX	0x50
#define OFF_FUART_TX	0x54
#define OFF_FUART_CTS	0x58
#define OFF_FUART_RTS	0x5c

#define FUART_OFFSETS	\
	OFF_FUART_RX,	\
	OFF_FUART_TX,	\
	OFF_FUART_CTS,	\
	OFF_FUART_RTS

#define SR_NAMES		\
	MSC313_PINNAME_SR_IO2,	\
	MSC313_PINNAME_SR_IO3,	\
	MSC313_PINNAME_SR_IO4,	\
	MSC313_PINNAME_SR_IO5,	\
	MSC313_PINNAME_SR_IO6,	\
	MSC313_PINNAME_SR_IO7,	\
	MSC313_PINNAME_SR_IO8,	\
	MSC313_PINNAME_SR_IO9,	\
	MSC313_PINNAME_SR_IO10,	\
	MSC313_PINNAME_SR_IO11,	\
	MSC313_PINNAME_SR_IO12,	\
	MSC313_PINNAME_SR_IO13,	\
	MSC313_PINNAME_SR_IO14,	\
	MSC313_PINNAME_SR_IO15,	\
	MSC313_PINNAME_SR_IO16,	\
	MSC313_PINNAME_SR_IO17

#define OFF_SR_IO2	0x88
#define OFF_SR_IO3	0x8c
#define OFF_SR_IO4	0x90
#define OFF_SR_IO5	0x94
#define OFF_SR_IO6	0x98
#define OFF_SR_IO7	0x9c
#define OFF_SR_IO8	0xa0
#define OFF_SR_IO9	0xa4
#define OFF_SR_IO10	0xa8
#define OFF_SR_IO11	0xac
#define OFF_SR_IO12	0xb0
#define OFF_SR_IO13	0xb4
#define OFF_SR_IO14	0xb8
#define OFF_SR_IO15	0xbc
#define OFF_SR_IO16	0xc0
#define OFF_SR_IO17	0xc4

#define SR_OFFSETS	\
	OFF_SR_IO2,	\
	OFF_SR_IO3,	\
	OFF_SR_IO4,	\
	OFF_SR_IO5,	\
	OFF_SR_IO6,	\
	OFF_SR_IO7,	\
	OFF_SR_IO8,	\
	OFF_SR_IO9,	\
	OFF_SR_IO10,	\
	OFF_SR_IO11,	\
	OFF_SR_IO12,	\
	OFF_SR_IO13,	\
	OFF_SR_IO14,	\
	OFF_SR_IO15,	\
	OFF_SR_IO16,	\
	OFF_SR_IO17

#define SD_NAMES		\
	MSC313_PINNAME_SD_CLK,	\
	MSC313_PINNAME_SD_CMD,	\
	MSC313_PINNAME_SD_D0,	\
	MSC313_PINNAME_SD_D1,	\
	MSC313_PINNAME_SD_D2,	\
	MSC313_PINNAME_SD_D3

#define OFF_SD_CLK	0x140
#define OFF_SD_CMD	0x144
#define OFF_SD_D0	0x148
#define OFF_SD_D1	0x14c
#define OFF_SD_D2	0x150
#define OFF_SD_D3	0x154

#define SD_OFFSETS	\
	OFF_SD_CLK,	\
	OFF_SD_CMD,	\
	OFF_SD_D0,	\
	OFF_SD_D1,	\
	OFF_SD_D2,	\
	OFF_SD_D3

#define I2C1_NAMES			\
	MSC313_PINNAME_I2C1_SCL,	\
	MSC313_PINNAME_I2C1_SCA

#define OFF_I2C1_SCL	0x188
#define OFF_I2C1_SCA	0x18c

#define I2C1_OFFSETS	\
	OFF_I2C1_SCL,	\
	OFF_I2C1_SCA

#define SPI0_NAMES		\
	MSC313_PINNAME_SPI0_CZ,	\
	MSC313_PINNAME_SPI0_CK,	\
	MSC313_PINNAME_SPI0_DI,	\
	MSC313_PINNAME_SPI0_DO

#define OFF_SPI0_CZ	0x1c0
#define OFF_SPI0_CK	0x1c4
#define OFF_SPI0_DI	0x1c8
#define OFF_SPI0_DO	0x1cc

#define SPI0_OFFSETS	\
	OFF_SPI0_CZ,	\
	OFF_SPI0_CK,	\
	OFF_SPI0_DI,	\
	OFF_SPI0_DO

struct msc313_gpio_data {
	const char * const *names;
	const unsigned int *offsets;
	const unsigned int num;
};

#define MSC313_GPIO_CHIPDATA(_chip) \
static const struct msc313_gpio_data _chip##_data = { \
	.names = _chip##_names, \
	.offsets = _chip##_offsets, \
	.num = ARRAY_SIZE(_chip##_offsets), \
}

#ifdef CONFIG_MACH_INFINITY
static const char * const msc313_names[] = {
	FUART_NAMES,
	SR_NAMES,
	SD_NAMES,
	I2C1_NAMES,
	SPI0_NAMES,
};

static const unsigned int msc313_offsets[] = {
	FUART_OFFSETS,
	SR_OFFSETS,
	SD_OFFSETS,
	I2C1_OFFSETS,
	SPI0_OFFSETS,
};

MSC313_GPIO_CHIPDATA(msc313);
#endif

#ifdef CONFIG_MACH_MERCURY

#define SR1_GPIO_NAMES			\
	SSC8336_PINNAME_SR1_GPIO0,	\
	SSC8336_PINNAME_SR1_GPIO1,	\
	SSC8336_PINNAME_SR1_GPIO2,	\
	SSC8336_PINNAME_SR1_GPIO3,	\
	SSC8336_PINNAME_SR1_GPIO4

#define OFF_SR1_GPIO0	0xb0
#define OFF_SR1_GPIO1	0xb4
#define OFF_SR1_GPIO2	0xb8
#define OFF_SR1_GPIO3	0xbc
#define OFF_SR1_GPIO4	0xc0

#define SR1_GPIO_OFFSETS	\
	OFF_SR1_GPIO0,		\
	OFF_SR1_GPIO1,		\
	OFF_SR1_GPIO2,		\
	OFF_SR1_GPIO3,		\
	OFF_SR1_GPIO4

static const char *ssc8336_names[] = {
	"unknown0",
	FUART_NAMES,
	SR1_GPIO_NAMES,
	"lcd_de",
	SPI0_NAMES,
	SD_NAMES,
};

static unsigned ssc8336_offsets[] = {
	0x130, // 70mai lcd rst
	FUART_OFFSETS,
	SR1_GPIO_OFFSETS,
	0x16c, // LCD_DE - mirrorcam stndby?
	SPI0_OFFSETS,
	SD_OFFSETS,
};

MSC313_GPIO_CHIPDATA(ssc8336);
#endif

struct msc313_gpio {
	void __iomem *base;
	const struct msc313_gpio_data *gpio_data;
	int *irqs;
	u8 *saved;
};

static void msc313_gpio_set(struct gpio_chip *chip, unsigned int offset, int value)
{
	struct msc313_gpio *gpio = gpiochip_get_data(chip);
	u8 gpioreg = readb_relaxed(gpio->base + gpio->gpio_data->offsets[offset]);

	if (value)
		gpioreg |= MSC313_GPIO_OUT;
	else
		gpioreg &= ~MSC313_GPIO_OUT;

	writeb_relaxed(gpioreg, gpio->base + gpio->gpio_data->offsets[offset]);
}

static int msc313_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct msc313_gpio *gpio = gpiochip_get_data(chip);

	return readb_relaxed(gpio->base + gpio->gpio_data->offsets[offset])
			& MSC313_GPIO_IN;
}

static int msc313_gpio_direction_input(struct gpio_chip *chip, unsigned int offset)
{
	struct msc313_gpio *gpio = gpiochip_get_data(chip);
	u8 gpioreg = readb_relaxed(gpio->base + gpio->gpio_data->offsets[offset]);

	gpioreg |= MSC313_GPIO_OEN;
	writeb_relaxed(gpioreg, gpio->base + gpio->gpio_data->offsets[offset]);

	return 0;
}

static int msc313_gpio_direction_output(struct gpio_chip *chip, unsigned int offset, int value)
{
	struct msc313_gpio *gpio = gpiochip_get_data(chip);
	u8 gpioreg = readb_relaxed(gpio->base + gpio->gpio_data->offsets[offset]);

	gpioreg &= ~MSC313_GPIO_OEN;
	if (value)
		gpioreg |= MSC313_GPIO_OUT;
	else
		gpioreg &= ~MSC313_GPIO_OUT;
	writeb_relaxed(gpioreg, gpio->base + gpio->gpio_data->offsets[offset]);

	return 0;
}

static int msc313_gpio_to_irq(struct gpio_chip *chip, unsigned int offset)
{
	struct msc313_gpio *gpio = gpiochip_get_data(chip);

	return gpio->irqs[offset];
}

static int msc313_gpio_probe(struct platform_device *pdev)
{
	int i, ret;
	const struct msc313_gpio_data *match_data;
	struct msc313_gpio *gpio;
	struct resource *res;
	struct gpio_chip *gpiochip;

	match_data = of_device_get_match_data(&pdev->dev);
	if (!match_data)
		return -EINVAL;

	gpio = devm_kzalloc(&pdev->dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	gpio->gpio_data = match_data;

	gpio->irqs = devm_kzalloc(&pdev->dev, gpio->gpio_data->num * sizeof(*gpio->irqs), GFP_KERNEL);
	if (!gpio->irqs)
		return -ENOMEM;

	gpio->saved = devm_kzalloc(&pdev->dev, gpio->gpio_data->num * sizeof(*gpio->saved), GFP_KERNEL);
	if (!gpio->saved)
		return -ENOMEM;

	platform_set_drvdata(pdev, gpio);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	gpio->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(gpio->base))
		return PTR_ERR(gpio->base);

	gpiochip = devm_kzalloc(&pdev->dev, sizeof(*gpiochip), GFP_KERNEL);
	if (!gpiochip)
		return -ENOMEM;

	gpiochip->label = DRIVER_NAME;
	gpiochip->parent = &pdev->dev;
	gpiochip->request = gpiochip_generic_request;
	gpiochip->free = gpiochip_generic_free;
	gpiochip->direction_input = msc313_gpio_direction_input;
	gpiochip->direction_output = msc313_gpio_direction_output;
	gpiochip->get = msc313_gpio_get;
	gpiochip->set = msc313_gpio_set;
	gpiochip->to_irq = msc313_gpio_to_irq;
	gpiochip->base = -1;
	gpiochip->ngpio = gpio->gpio_data->num;
	gpiochip->names = gpio->gpio_data->names;

	for (i = 0; i < gpiochip->ngpio; i++)
		gpio->irqs[i] = of_irq_get_byname(pdev->dev.of_node, gpio->gpio_data->names[i]);

	ret = gpiochip_add_data(gpiochip, gpio);
	return ret;
}

static const struct of_device_id msc313_gpio_of_match[] = {
#ifdef CONFIG_MACH_INFINITY
	{
		.compatible = "mstar,msc313-gpio",
		.data = &msc313_data,
	},
#endif
#ifdef CONFIG_MACH_MERCURY
	{
		.compatible = "mstar,ssc8336-gpio",
		.data = &ssc8336_data,
	},
#endif
	{ }
};

/* The GPIO controller loses the state of the registers when the
 * SoC goes into "DRAM self-refresh low power" mode so we need to
 * save the direction and value before suspending and put it back
 * when resuming.
 */

static int __maybe_unused msc313_gpio_suspend(struct device *dev)
{
	struct msc313_gpio *gpio = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < gpio->gpio_data->num; i++)
		gpio->saved[i] = readb_relaxed(gpio->base + gpio->gpio_data->offsets[i]) & MSC313_GPIO_BITSTOSAVE;

	return 0;
}

static int __maybe_unused msc313_gpio_resume(struct device *dev)
{
	struct msc313_gpio *gpio = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < gpio->gpio_data->num; i++)
		writeb_relaxed(gpio->saved[i], gpio->base + gpio->gpio_data->offsets[i]);

	return 0;
}

static SIMPLE_DEV_PM_OPS(msc313_gpio_ops, msc313_gpio_suspend, msc313_gpio_resume);

static struct platform_driver msc313_gpio_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = msc313_gpio_of_match,
		.pm = &msc313_gpio_ops,
	},
	.probe = msc313_gpio_probe,
};

builtin_platform_driver(msc313_gpio_driver);
