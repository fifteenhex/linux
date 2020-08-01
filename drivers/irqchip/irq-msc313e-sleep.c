// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Daniel Palmer
 */

#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <dt-bindings/interrupt-controller/arm-gic.h>

#include <soc/mstar/riu.h>

/*
 * pm "sleep" intc
 * This is another interrupt controller that seems to be in the always on
 * power domain and is probably there to deal with interrupts that wake
 * the chip up.
 *
 * The sleep intc is connected to the GIC via the normal irq intc by a single
 * interrupt so here we handle that interrupt with a chained handler and
 * from the status register work out which interrupts to fire in the domain.
 *
 * Note: Only the first two interrupts that come through this controller are
 * controlled (mask, unmask, eoi etc) here. Everything else is passed through and
 * actually controlled by the sleep gpio controller.
 */

#define NR_INTR_SLEEP	32

#define REG_STATUS	0x0

struct msc313e_sleep_intc {
	void __iomem *base;
	struct irq_domain *domain;
};

static void msc313e_sleep_intc_mask_irq(struct irq_data *data)
{
}

static void msc313e_sleep_intc_unmask_irq(struct irq_data *data)
{
}

static void msc313e_sleep_intc_irq_eoi(struct irq_data *data)
{
}

static int msc313e_sleep_intc_set_type_irq(struct irq_data *data, unsigned int flow_type)
{
	return 0;
}

static struct irq_chip msc313e_pm_intc_chip = {
	.name		= "PM-INTC",
	.irq_mask	= msc313e_sleep_intc_mask_irq,
	.irq_unmask	= msc313e_sleep_intc_unmask_irq,
	.irq_eoi	= msc313e_sleep_intc_irq_eoi,
	.irq_set_type	= msc313e_sleep_intc_set_type_irq,
};

static void msc313e_sleep_intc_chainedhandler(struct irq_desc *desc){
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct msc313e_sleep_intc *intc = irq_desc_get_handler_data(desc);
	u32 status;
	unsigned int hwirq;
	unsigned int virq;

	chained_irq_enter(chip, desc);
	status = riu_readl_relaxed(intc->base + REG_STATUS);

	while (status) {
		hwirq = __ffs(status);
		virq = irq_find_mapping(intc->domain, hwirq);
		if (virq)
			generic_handle_irq(virq);
		status &= ~BIT(hwirq);
	}

	chained_irq_exit(chip, desc);
}

static int msc313e_pm_intc_domain_translate(struct irq_domain *d,
					struct irq_fwspec *fwspec,
					unsigned long *hwirq,
					unsigned int *type)
{
	if (!is_of_node(fwspec->fwnode) || fwspec->param_count != 2)
		return -EINVAL;

	*hwirq = fwspec->param[0];
	*type = fwspec->param[1];
	return 0;
}

static int msc313e_pm_intc_domain_alloc(struct irq_domain *domain, unsigned int virq,
				 unsigned int nr_irqs, void *data)
{
	struct irq_fwspec *fwspec = data;
	struct msc313e_intc* intc = domain->host_data;

	if (fwspec->param_count != 2)
		return -EINVAL;

	irq_domain_set_hwirq_and_chip(domain, virq, fwspec->param[0], &msc313e_pm_intc_chip, intc);

	return 0;
}

static const struct irq_domain_ops msc313e_pm_intc_domain_ops = {
		.translate = msc313e_pm_intc_domain_translate,
		.alloc = msc313e_pm_intc_domain_alloc,
		.free = irq_domain_free_irqs_common,
};

static int __init msc313e_sleep_intc_of_init(struct device_node *node,
				   struct device_node *parent)
{
	int gicint;
	struct msc313e_sleep_intc *intc;

	gicint = of_irq_get(node, 0);
	if(gicint == 0){
		return -ENODEV;
	}

	intc = kzalloc(sizeof(*intc), GFP_KERNEL);
	if (!intc)
		return -ENOMEM;

	intc->base = of_iomap(node, 0);
	if (IS_ERR(intc->base))
		return PTR_ERR(intc->base);

	irq_set_chained_handler_and_data(gicint, msc313e_sleep_intc_chainedhandler, intc);

	intc->domain = irq_domain_add_simple(node, 32, 0,
			&msc313e_pm_intc_domain_ops, intc);
	if (!intc->domain) {
		return -ENOMEM;
	}

	return 0;
}

IRQCHIP_DECLARE(mstar_msc313e_sleep_intc, "mstar,msc313e-sleep-intc",
		msc313e_sleep_intc_of_init);
