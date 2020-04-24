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
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/slab.h>
#include <linux/irqchip.h>

#define DRIVER_NAME	"gpio-msc313-pm"

/*
 * MStar PM GPIO
 *
 * 15 - 12 | 11 - 0 |      9       |    8       |    7     |    6    | 5 |    4     | 3 | 2  |  1  |  0
 *    ?    |    0   | INVERTED IN? | INT STATUS | INT TYPE | INT CLR | ? | INT MASK | ? | IN | OUT | OEN
 *         |        |     ro?      |   ro?      |          |   wo    |   |          |   |    |     |
 *
 * bit 9 reacts to the pin being pulled up and down
 *
 * Reset value is 0x0215
 *
 */

#define BIT_OEN		BIT(0)
#define BIT_OUT		BIT(1)
#define BIT_IN		BIT(2)
#define BIT_IRQ_MASK	BIT(4)
#define BIT_IRQ_CLEAR	BIT(6)
#define BIT_IRQ_TYPE	BIT(7)

#define OFF_GPIO0	0x00
#define OFF_GPIO1	0x04
#define OFF_GPIO2	0x08
#define OFF_GPIO3	0x0c
#define OFF_GPIO4	0x10
#define OFF_GPIO5	0x14
#define OFF_GPIO6	0x18
#define OFF_GPIO7	0x1c
#define OFF_GPIO8	0x20
#define OFF_SPI_CZ	0x60
#define OFF_SPI_CK	0x64
#define OFF_SPI_DI	0x68
#define OFF_SPI_DO	0x6c
//#define OFF_IR_IN	0xa0
#define OFF_SPI_HLD	0x114
#define OFF_SD_CZ	0x11c

#define NAME_GPIO0	"pm_gpio0"
#define NAME_GPIO2	"pm_gpio2"
#define NAME_GPIO4	"pm_gpio4"
#define NAME_GPIO5	"pm_gpio5"
#define NAME_GPIO6	"pm_gpio6"
#define NAME_GPIO8	"pm_gpio8"
#define NAME_SPI_CZ	"pm_spi_cz"
#define NAME_SPI_CK	"pm_spi_ck"
#define NAME_SPI_DI	"pm_spi_di"
#define NAME_SPI_DO	"pm_spi_do"
#define NAME_SPI_HLD	"pm_spi_hld"
#define NAME_SD_SDZ	"pm_sd_sdz"

struct info {
	const char **names;
	const unsigned *offsets;
	const unsigned num;
};

#define CHIP_INFO(chipname) static const struct info info_##chipname = { .names = chipname##_names, \
									.offsets = chipname##_offsets, \
									.num = ARRAY_SIZE(chipname##_offsets) \
									}

#ifdef CONFIG_MACH_INFINITY
static const char *msc313_names[] = {
	NAME_GPIO4,
	NAME_SD_SDZ
};

static const unsigned msc313_offsets[] = {
	OFF_GPIO4,
	OFF_SD_CZ
};

CHIP_INFO(msc313);
#endif /* infinity */

#ifdef CONFIG_MACH_MERCURY
static const char *ssc8336_names[] = {
	NAME_GPIO0,
	NAME_GPIO2,
	NAME_GPIO4,
	NAME_GPIO5,
	NAME_GPIO6,
	NAME_GPIO8,
	NAME_SPI_DO,
	NAME_SD_SDZ
};

static const unsigned ssc8336_offsets[] = {
	OFF_GPIO0,
	OFF_GPIO2,
	OFF_GPIO4,
	OFF_GPIO5,
	OFF_GPIO6,
	OFF_GPIO8,
	OFF_SPI_DO,
	OFF_SD_CZ
};

CHIP_INFO(ssc8336);
#endif /* mercury */

struct mstar_pm_gpio {
	struct device *dev;
	void __iomem *base;
	struct fwnode_handle *fwnode;
	struct info *info;
};

static void mstar_pm_gpio_irq_eoi(struct irq_data *data){
	__iomem void *addr = data->chip_data;
	u16 reg = readw_relaxed(addr);
	reg |= BIT_IRQ_CLEAR;
	writew_relaxed(reg, addr);
	irq_chip_eoi_parent(data);
};

static void mstar_pm_gpio_irq_mask(struct irq_data *data){
	__iomem void *addr = data->chip_data;
	u16 reg = readw_relaxed(addr);
	reg |= BIT_IRQ_MASK;
	writew_relaxed(reg, addr);
	irq_chip_mask_parent(data);
};

static void mstar_pm_gpio_irq_unmask(struct irq_data *data){
	__iomem void *addr = data->chip_data;
	u16 reg = readw_relaxed(addr);
	reg &= ~BIT_IRQ_MASK;
	writew_relaxed(reg, addr);
	irq_chip_unmask_parent(data);
}

static int mstar_pm_gpio_irq_set_type(struct irq_data *data, unsigned int flow_type){
	__iomem void *addr = data->chip_data;
	u16 reg = readw_relaxed(addr);
	if(flow_type)
		reg &= BIT_IRQ_TYPE;
	else
		reg |= BIT_IRQ_TYPE;

	writew_relaxed(reg, addr);
	return 0;
}

static struct irq_chip mstar_pm_gpio_irqchip = {
	.name = "PM-GPIO",
	.irq_eoi = mstar_pm_gpio_irq_eoi,
	.irq_mask = mstar_pm_gpio_irq_mask,
	.irq_unmask = mstar_pm_gpio_irq_unmask,
	.irq_set_type = mstar_pm_gpio_irq_set_type,
};

static int mstar_pm_gpio_domain_translate(struct irq_domain *d,
					struct irq_fwspec *fwspec,
					unsigned long *hwirq,
					unsigned int *type)
{
	struct mstar_pm_gpio* pinctrl = d->host_data;

	if (!is_of_node(fwspec->fwnode))
		return -EINVAL;

	if(fwspec->param_count != 3){
		dev_err(pinctrl->dev, "need 3 parameters, got %d", fwspec->param_count);
		return -EINVAL;
	}

	*hwirq = fwspec->param[0];
	*type = fwspec->param[1];

	return 0;
}

static int mstar_pm_gpio_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				 unsigned int nr_irqs, void *data)
{
	struct irq_fwspec *fwspec = data;
	struct mstar_pm_gpio* pinctrl = domain->host_data;
	struct irq_fwspec parent_fwspec;
	__iomem void* addr;

	/* 3 params: first is the gpio offset, second is the parent irq, last is the normal flags */

	if (fwspec->param_count != 3){
		dev_err(pinctrl->dev, "need 3 parameters, got %d", fwspec->param_count);
		return -EINVAL;
	}

	addr = pinctrl->base + pinctrl->info->offsets[fwspec->param[0]];
	irq_domain_set_info(domain, virq, fwspec->param[0],
			&mstar_pm_gpio_irqchip, addr, handle_fasteoi_irq, NULL, NULL);

	parent_fwspec.fwnode = domain->parent->fwnode;
	parent_fwspec.param[0] = fwspec->param[1];
	parent_fwspec.param[1] = fwspec->param[2];
	parent_fwspec.param_count = 2;

	return irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, &parent_fwspec);
}

static const struct irq_domain_ops mstar_pm_gpio_irq_domain_ops = {
		.translate = mstar_pm_gpio_domain_translate,
		.alloc = mstar_pm_gpio_irq_domain_alloc,
		.free = irq_domain_free_irqs_common,
};

static int mstar_pm_gpio_irq_setup(struct platform_device *pdev,
					struct mstar_pm_gpio *pinctrl)
{
	struct irq_domain *domain, *parent_domain;
	struct device_node *parent_node;

	parent_node = of_irq_find_parent(pdev->dev.of_node);
	if (!parent_node)
		return -ENXIO;

	parent_domain = irq_find_host(parent_node);
	of_node_put(parent_node);
	if (!parent_domain)
		return -ENXIO;

	pinctrl->fwnode = of_node_to_fwnode(pdev->dev.of_node);
	domain = irq_domain_add_hierarchy(parent_domain, 0, 32, pdev->dev.of_node,
					&mstar_pm_gpio_irq_domain_ops, pinctrl);

	if (!domain)
		return -ENODEV;

	return 0;
}

static void msc313e_pm_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	struct mstar_pm_gpio *pinctrl = gpiochip_get_data(chip);
	__iomem void *addr = pinctrl->base + pinctrl->info->offsets[offset];
	u16 reg = readw_relaxed(addr);
	if(value)
		reg |= BIT_OUT;
	else
		reg &= ~BIT_OUT;
	writew_relaxed(reg, addr);
}

static int msc313e_pm_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	struct mstar_pm_gpio *pinctrl = gpiochip_get_data(chip);
	u16 reg = readw_relaxed(pinctrl->base + pinctrl->info->offsets[offset]);
	return reg & BIT_IN ? 1 : 0;
}

static int msc313e_pm_gpio_direction_input(struct gpio_chip *chip, unsigned offset)
{
	struct mstar_pm_gpio *pinctrl = gpiochip_get_data(chip);
	__iomem void *addr = pinctrl->base + pinctrl->info->offsets[offset];
	u16 reg = readw_relaxed(addr);
	reg |= BIT_OEN;
	writew_relaxed(reg, addr);
	return 0;
}

static int msc313e_pm_gpio_direction_output(struct gpio_chip *chip, unsigned offset,
					int value)
{
	struct mstar_pm_gpio *pinctrl = gpiochip_get_data(chip);
	__iomem void *addr = pinctrl->base + pinctrl->info->offsets[offset];
	u16 reg = readw_relaxed(addr);
	reg &= ~BIT_OEN;
	writew_relaxed(reg, addr);
	return 0;
}

static int msc313e_pm_gpio_to_irq(struct gpio_chip *chip, unsigned offset)
{
	struct mstar_pm_gpio *pinctrl = gpiochip_get_data(chip);
	int irq = of_irq_get_byname(pinctrl->dev->of_node, pinctrl->info->names[offset]);
	if(irq)
		return irq;
	else {
		dev_info(pinctrl->dev, "no irq for %d(%s)", offset, pinctrl->info->names[offset]);
		return -1;
	}
}

static int mstar_pm_gpio_probe(struct platform_device *pdev)
{
	int ret;
	struct mstar_pm_gpio *pinctrl;
	struct resource *res;
	struct gpio_chip* gpiochip;
	const struct info *match_data;

	match_data = of_device_get_match_data(&pdev->dev);
	if (!match_data)
		return -EINVAL;

	pinctrl = devm_kzalloc(&pdev->dev, sizeof(*pinctrl), GFP_KERNEL);
	if (!pinctrl)
		return -ENOMEM;

	pinctrl->dev = &pdev->dev;
	pinctrl->info = match_data;

	platform_set_drvdata(pdev, pinctrl);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pinctrl->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pinctrl->base))
		return PTR_ERR(pinctrl->base);

	gpiochip = devm_kzalloc(&pdev->dev, sizeof(*gpiochip), GFP_KERNEL);
	if (!gpiochip)
		return -ENOMEM;

	gpiochip->label		   = DRIVER_NAME;
	gpiochip->parent	   = &pdev->dev;
	gpiochip->request	   = gpiochip_generic_request;
	gpiochip->free             = gpiochip_generic_free;
	gpiochip->direction_input  = msc313e_pm_gpio_direction_input;
	gpiochip->get              = msc313e_pm_gpio_get;
	gpiochip->direction_output = msc313e_pm_gpio_direction_output;
	gpiochip->set              = msc313e_pm_gpio_set;
	gpiochip->to_irq	   = msc313e_pm_gpio_to_irq;
	gpiochip->base             = -1;
	gpiochip->ngpio            = pinctrl->info->num;
	gpiochip->names            = pinctrl->info->names;

	ret = mstar_pm_gpio_irq_setup(pdev, pinctrl);
	if(ret)
		return ret;

	ret = gpiochip_add_data(gpiochip, pinctrl);
	if (ret < 0) {
		dev_err(pinctrl->dev,"failed to register gpio chip");
		return ret;
	}

	return ret;
}

static const struct of_device_id mstar_pm_gpio_of_match[] = {
#ifdef CONFIG_MACH_INFINITY
	{
		/* MSC313E PM */
		.compatible	= "mstar,msc313-gpio-pm",
		.data		= &info_msc313,
	},
#endif
#ifdef CONFIG_MACH_MERCURY
	{
		/* SSC8336N PM */
		.compatible	= "mstar,ssc8336-gpio-pm",
		.data		= &info_ssc8336,
	},
#endif
	{ }
};

static struct platform_driver mstar_pm_gpio_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = mstar_pm_gpio_of_match,
	},
	.probe = mstar_pm_gpio_probe,
};

module_platform_driver(mstar_pm_gpio_driver);

MODULE_AUTHOR("Daniel Palmer <daniel@0x0f.com>");
MODULE_DESCRIPTION("pm gpio controller driver for MStar/Sigmastar ARMv7 SoCs");
MODULE_LICENSE("GPL v2");
