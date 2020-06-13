// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Daniel Palmer
 */

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/slab.h>

#define DRIVER_NAME			"gpio-msc313e"

/*
 * gpio data registers seem to be laid out like this in the gpio block
 *  5   |  4  | 3 | 2 | 1 | 0
 * ~OEN | OUT | 0 | 0 | 0 | IN
 */

#define MSTAR_GPIO_IN  BIT(0)
#define MSTAR_GPIO_OUT BIT(4)
#define MSTAR_GPIO_OEN BIT(5)

#define NAME_FUART_RX	"fuart_rx"
#define NAME_FUART_TX	"fuart_tx"
#define NAME_FUART_CTS	"fuart_cts"
#define NAME_FUART_RTS	"fuart_rts"

#define FUART_NAMES \
	NAME_FUART_RX,\
	NAME_FUART_TX,\
	NAME_FUART_CTS,\
	NAME_FUART_RTS

#define OFF_FUART_RX	0x50
#define OFF_FUART_TX	0x54
#define OFF_FUART_CTS	0x58
#define OFF_FUART_RTS	0x5c

#define FUART_OFFSETS \
	OFF_FUART_RX,\
	OFF_FUART_TX,\
	OFF_FUART_CTS,\
	OFF_FUART_RTS

#define NAME_SD_CLK "sd_clk"
#define NAME_SD_CMD "sd_cmd"
#define NAME_SD_D0 "sd_d0"
#define NAME_SD_D1 "sd_d1"
#define NAME_SD_D2 "sd_d2"
#define NAME_SD_D3 "sd_d3"

#define SD_NAMES \
	NAME_SD_CLK,\
	NAME_SD_CMD,\
	NAME_SD_D0,\
	NAME_SD_D1,\
	NAME_SD_D2,\
	NAME_SD_D3

#define OFF_SD_CLK	0x140
#define OFF_SD_CMD	0x144
#define OFF_SD_D0	0x148
#define OFF_SD_D1	0x14c
#define OFF_SD_D2	0x150
#define OFF_SD_D3	0x154

#define SD_OFFSETS \
	OFF_SD_CLK,\
	OFF_SD_CMD,\
	OFF_SD_D0,\
	OFF_SD_D1,\
	OFF_SD_D2,\
	OFF_SD_D3

#define NAME_SPI0_CZ	"spi0_cz"
#define NAME_SPI0_CK	"spi0_ck"
#define NAME_SPI0_DI	"spi0_di"
#define NAME_SPI0_DO	"spi0_do"

#define SPI0_NAMES \
	NAME_SPI0_CZ,\
	NAME_SPI0_CK,\
	NAME_SPI0_DI,\
	NAME_SPI0_DO

#define OFF_SPI0_CZ	0x1c0
#define OFF_SPI0_CK	0x1c4
#define OFF_SPI0_DI	0x1c8
#define OFF_SPI0_DO	0x1cc

#define SPI0_OFFSETS \
	OFF_SPI0_CZ,\
	OFF_SPI0_CK,\
	OFF_SPI0_DI,\
	OFF_SPI0_DO

struct mstar_gpio_data {
	const char **names;
	const unsigned *offsets;
	const unsigned num;
};

static const char *msc313_names[] = {
		FUART_NAMES,
		"i2c1_scl",
		"i2c1_sda",
		"sr_io2",
		"sr_io3",
		"sr_io4",
		"sr_io5",
		"sr_io6",
		"sr_io7",
		"sr_io8",
		"sr_io9",
		"sr_io10",
		"sr_io11",
		"sr_io12",
		"sr_io13",
		"sr_io14",
		"sr_io15",
		"sr_io16",
		"sr_io17",
		SPI0_NAMES,
		SD_NAMES,
};

static unsigned msc313_offsets[] = {
		FUART_OFFSETS,
		0x188, /* I2C1_SCL	*/
		0x18c, /* I2C1_SDA	*/
		0x88, /* SR_IO2		*/
		0x8c, /* SR_IO3		*/
		0x90, /* SR_IO4		*/
		0x94, /* SR_IO5		*/
		0x98, /* SR_IO6		*/
		0x9c, /* SR_IO7		*/
		0xa0, /* SR_IO8		*/
		0xa4, /* SR_IO9		*/
		0xa8, /* SR_IO10	*/
		0xac, /* SR_IO11	*/
		0xb0, /* SR_IO12	*/
		0xb4, /* SR_IO13	*/
		0xb8, /* SR_IO14	*/
		0xbc, /* SR_IO15	*/
		0xc0, /* SR_IO16	*/
		0xc4, /* SR_IO17	*/
		SPI0_OFFSETS,
		SD_OFFSETS
};

static const struct mstar_gpio_data msc313_data = {
	.names = msc313_names,
	.offsets = msc313_offsets,
	.num = ARRAY_SIZE(msc313_offsets),
};

static const char *ssc8336_names[] = {
		"unknown0",
		FUART_NAMES,
		"sr1_gpio0",
		"sr1_gpio1",
		"sr1_gpio2",
		"sr1_gpio3",
		"sr1_gpio4",
		"lcd_de",
		SPI0_NAMES,
		SD_NAMES,
};

static unsigned ssc8336_offsets[] = {
		0x130, // 70mai lcd rst
		FUART_OFFSETS,
		0xb0, // SR1_GPIO0
		0xb4, // SR1_GPIO1
		0xb8, // SR1_GPIO2
		0xbc, // SR1_GPIO3
		0xc0, // SR1_GPIO4
		0x16c, // LCD_DE - mirrorcam stndby?
		SPI0_OFFSETS,
		SD_OFFSETS,
};

static const struct mstar_gpio_data ssc8336_data = {
	.names = ssc8336_names,
	.offsets = ssc8336_offsets,
	.num = ARRAY_SIZE(ssc8336_offsets),
};

struct msc313e_gpio {
	void __iomem *base;
	const struct mstar_gpio_data *gpio_data;
	int *irqs;
	u8 *saved;
};

static void mstar_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	struct msc313e_gpio *gpio = gpiochip_get_data(chip);
	u8 gpioreg = ioread8(gpio->base + gpio->gpio_data->offsets[offset]);
	if(value)
		gpioreg |= MSTAR_GPIO_OUT;
	else
		gpioreg &= ~MSTAR_GPIO_OUT;
	iowrite8(gpioreg, gpio->base + gpio->gpio_data->offsets[offset]);
}

static int mstar_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	struct msc313e_gpio *gpio = gpiochip_get_data(chip);
	return ioread8(gpio->base + gpio->gpio_data->offsets[offset]) & MSTAR_GPIO_IN;
}

static int mstar_gpio_direction_input(struct gpio_chip *chip, unsigned offset)
{
	struct msc313e_gpio *gpio = gpiochip_get_data(chip);
	u8 gpioreg = ioread8(gpio->base + gpio->gpio_data->offsets[offset]);
	gpioreg |= MSTAR_GPIO_OEN;
	iowrite8(gpioreg, gpio->base + gpio->gpio_data->offsets[offset]);
	return 0;
}

static int mstar_gpio_direction_output(struct gpio_chip *chip, unsigned offset,
					int value)
{
	struct msc313e_gpio *gpio = gpiochip_get_data(chip);
	u8 gpioreg = ioread8(gpio->base + gpio->gpio_data->offsets[offset]);
	gpioreg &= ~MSTAR_GPIO_OEN;
	if(value)
		gpioreg |= MSTAR_GPIO_OUT;
	else
		gpioreg &= ~MSTAR_GPIO_OUT;
	iowrite8(gpioreg, gpio->base + gpio->gpio_data->offsets[offset]);
	return 0;
}

static int mstar_gpio_to_irq(struct gpio_chip *chip, unsigned offset)
{
	struct msc313e_gpio *gpio = gpiochip_get_data(chip);
	return gpio->irqs[offset];
}

static int msc313e_gpio_probe(struct platform_device *pdev)
{
	int i, ret;
	const struct mstar_gpio_data* match_data;
	struct msc313e_gpio *gpio;
	struct resource *res;
	struct gpio_chip* gpiochip;

	match_data = of_device_get_match_data(&pdev->dev);
	if (!match_data)
		return -EINVAL;

	gpio = devm_kzalloc(&pdev->dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	gpio->gpio_data = match_data;

	gpio->irqs = devm_kzalloc(&pdev->dev,
			gpio->gpio_data->num * sizeof(*gpio->irqs), GFP_KERNEL);
	if (!gpio->irqs)
		return -ENOMEM;

	gpio->saved = devm_kzalloc(&pdev->dev,
			gpio->gpio_data->num * sizeof(*gpio->saved), GFP_KERNEL);
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

	gpiochip->label		   = DRIVER_NAME;
	gpiochip->parent	   = &pdev->dev;
	gpiochip->request          = gpiochip_generic_request;
	gpiochip->free             = gpiochip_generic_free;
	gpiochip->direction_input  = mstar_gpio_direction_input;
	gpiochip->get              = mstar_gpio_get;
	gpiochip->direction_output = mstar_gpio_direction_output;
	gpiochip->set              = mstar_gpio_set;
	gpiochip->to_irq	   = mstar_gpio_to_irq;
	gpiochip->base             = -1;
	gpiochip->ngpio            = gpio->gpio_data->num;
	gpiochip->names		   = gpio->gpio_data->names;

	for(i = 0; i < gpiochip->ngpio; i++){
		gpio->irqs[i] = of_irq_get_byname(pdev->dev.of_node, gpio->gpio_data->names[i]);
	}

	ret = gpiochip_add_data(gpiochip, gpio);
	return ret;
}

static const struct of_device_id msc313e_gpio_of_match[] = {
	{
		.compatible	= "mstar,msc313e-gpio",
		.data		= &msc313_data,
	},
	{
		.compatible	= "mstar,ssc8336-gpio",
		.data		= &ssc8336_data,
	},
	{ }
};

static int __maybe_unused msc313e_gpio_suspend(struct device *dev)
{
	struct msc313e_gpio *gpio = dev_get_drvdata(dev);
	int i;
	for(i = 0; i < gpio->gpio_data->num; i++)
		gpio->saved[i] = readb_relaxed(gpio->base +
				gpio->gpio_data->offsets[i]) & (BIT(5) | BIT(4));
	return 0;
}

static int __maybe_unused msc313e_gpio_resume(struct device *dev)
{
	struct msc313e_gpio *gpio = dev_get_drvdata(dev);
	int i;
	for(i = 0; i < gpio->gpio_data->num; i++)
		writeb_relaxed(gpio->saved[i], gpio->base +
				gpio->gpio_data->offsets[i]);
	return 0;
}

static SIMPLE_DEV_PM_OPS(msc313e_gpio_ops, msc313e_gpio_suspend,
			 msc313e_gpio_resume);

static struct platform_driver msc313e_gpio_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = msc313e_gpio_of_match,
		.pm = &msc313e_gpio_ops,
	},
	.probe = msc313e_gpio_probe,
};

static int __init msc313e_gpio_init(void)
{
	return platform_driver_register(&msc313e_gpio_driver);
}
core_initcall(msc313e_gpio_init);

MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
MODULE_DESCRIPTION("gpio controller driver for MStar MSC313E");
MODULE_LICENSE("GPL v2");
