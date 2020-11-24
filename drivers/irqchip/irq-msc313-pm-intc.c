// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Daniel Palmer
 */

#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/interrupt.h>

#include <soc/mstar/pmsleep.h>

#define NUM_IRQ		32
#define REG_STATUS	0x0

struct msc313_sleep_intc {
	struct regmap *pmsleep;
};

static void msc313_sleep_intc_mask_irq(struct irq_data *data)
{
}

static void msc313_sleep_intc_unmask_irq(struct irq_data *data)
{
}

static void msc313_sleep_intc_irq_eoi(struct irq_data *data)
{
}

static int msc313_sleep_intc_set_type_irq(struct irq_data *data, unsigned int flow_type)
{
	return 0;
}

static struct irq_chip msc313_pm_intc_chip = {
	.name		= "PM-INTC",
	.irq_mask	= msc313_sleep_intc_mask_irq,
	.irq_unmask	= msc313_sleep_intc_unmask_irq,
	.irq_eoi	= msc313_sleep_intc_irq_eoi,
	.irq_set_type	= msc313_sleep_intc_set_type_irq,
};

static int msc313_sleep_intc_domain_map(struct irq_domain *domain,
		unsigned int irq, irq_hw_number_t hw)
{
	struct msc313_sleep_intc *intc = domain->host_data;

	irq_set_chip_and_handler(irq, &msc313_pm_intc_chip, handle_level_irq);
	irq_set_chip_data(irq, intc);
	irq_set_probe(irq);

	return 0;
}

static const struct irq_domain_ops msc313_pm_intc_domain_ops = {
	.xlate = irq_domain_xlate_twocell,
	.map = msc313_sleep_intc_domain_map,
};

static irqreturn_t msc313_sleep_intc_chainedhandler(int irq, void *data){
	struct irq_domain *domain = data;
	struct msc313_sleep_intc *intc = domain->host_data;
	u32 status;
	unsigned int hwirq, virq, tmp;

	regmap_read(intc->pmsleep, MSTAR_PMSLEEP_INTSTATUS + 4, &tmp);
	status = tmp << 16;
	regmap_read(intc->pmsleep, MSTAR_PMSLEEP_INTSTATUS, &tmp);
	status |= tmp;

	while (status) {
		hwirq = __ffs(status);
		virq = irq_find_mapping(domain, hwirq);
		if (virq)
			generic_handle_irq(virq);
		status &= ~BIT(hwirq);
	}

	return IRQ_HANDLED;
}

static int __init msc313_sleep_intc_of_init(struct device_node *node,
				   struct device_node *parent)
{
	int gicint;
	struct regmap *pmsleep;
	struct msc313_sleep_intc *intc;
	struct irq_domain *domain;
	int ret;

	gicint = of_irq_get(node, 0);
	printk("gicint: %d\n", gicint);
	if (gicint <= 0)
		return gicint;

	pmsleep = syscon_regmap_lookup_by_phandle(node, "mstar,pmsleep");
	if(IS_ERR(pmsleep))
		return PTR_ERR(pmsleep);

	intc = kzalloc(sizeof(*intc), GFP_KERNEL);
	if (!intc)
		return -ENOMEM;

	intc->pmsleep = pmsleep;

	domain = irq_domain_add_linear(node, NUM_IRQ,
			&msc313_pm_intc_domain_ops, intc);
	if (!domain) {
		ret = -ENOMEM;
		goto out_free;
	}

	request_irq(gicint, msc313_sleep_intc_chainedhandler, IRQF_SHARED,
				"pmsleep", domain);

	return 0;

out_free:
	kfree(intc);
	return ret;
}

IRQCHIP_DECLARE(mstar_msc313_sleep_intc, "mstar,msc313-pm-intc",
		msc313_sleep_intc_of_init);
