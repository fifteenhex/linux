// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Daniel Palmer
 */

#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <dt-bindings/interrupt-controller/arm-gic.h>

#define REGOFF_MASK		0x0
#define REGOFF_POLARITY		0x10
#define REGOFF_STATUSCLEAR	0x20
#define IRQSPERREG		16
#define IRQBIT(hwirq)		BIT((hwirq % IRQSPERREG))
#define REGOFF(hwirq)		((hwirq >> 4) * 4)

struct msc313_intc {
	struct irq_domain *domain;
	void __iomem *base;
	struct irq_chip irqchip;
	u8 gicoff;
};

static void msc313_intc_maskunmask(struct msc313_intc *intc, int hwirq, bool mask)
{
	int regoff = REGOFF(hwirq);
	void __iomem *addr = intc->base + REGOFF_MASK + regoff;
	u16 bit = IRQBIT(hwirq);
	u16 reg = readw_relaxed(addr);

	if (mask)
		reg |= bit;
	else
		reg &= ~bit;

	writew_relaxed(reg, addr);
}

static void msc313_intc_mask_irq(struct irq_data *data)
{
	struct msc313_intc *intc = data->chip_data;

	msc313_intc_maskunmask(intc, data->hwirq, true);
	irq_chip_mask_parent(data);
}

static void msc313_intc_unmask_irq(struct irq_data *data)
{
	struct msc313_intc *intc = data->chip_data;

	msc313_intc_maskunmask(intc, data->hwirq, false);
	irq_chip_unmask_parent(data);
}

static int msc313_intc_set_type_irq(struct irq_data *data, unsigned int flow_type)
{
	struct msc313_intc *intc = data->chip_data;
	int irq = data->hwirq;
	int regoff = REGOFF(irq);
	void __iomem *addr = intc->base + REGOFF_POLARITY + regoff;
	u16 bit = IRQBIT(irq);
	u16 reg = readw_relaxed(addr);

	if (flow_type & (IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_LEVEL_HIGH))
		reg &= ~bit;
	else
		reg |= bit;

	writew_relaxed(reg, addr);
	return 0;
}

static void msc313_intc_irq_eoi(struct irq_data *data)
{
	struct msc313_intc *intc = data->chip_data;
	int irq = data->hwirq;
	int regoff = REGOFF(irq);
	void __iomem *addr = intc->base + REGOFF_STATUSCLEAR + regoff;
	u16 bit = IRQBIT(irq);
	u16 reg = readw_relaxed(addr);

	reg |= bit;
	writew_relaxed(reg, addr);
	irq_chip_eoi_parent(data);
}

static int msc313_intc_domain_translate(struct irq_domain *d,
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

static int msc313_intc_domain_alloc(struct irq_domain *domain, unsigned int virq,
				 unsigned int nr_irqs, void *data)
{
	struct irq_fwspec *fwspec = data;
	struct irq_fwspec parent_fwspec;
	struct msc313_intc *intc = domain->host_data;

	if (fwspec->param_count != 2)
		return -EINVAL;

	irq_domain_set_hwirq_and_chip(domain, virq, fwspec->param[0], &intc->irqchip, intc);

	parent_fwspec.fwnode = domain->parent->fwnode;
	parent_fwspec.param[0] = GIC_SPI;
	parent_fwspec.param[1] = fwspec->param[0] + intc->gicoff;
	parent_fwspec.param[2] = fwspec->param[1];
	parent_fwspec.param_count = 3;

	return irq_domain_alloc_irqs_parent(domain, virq, nr_irqs,
					    &parent_fwspec);
}

static const struct irq_domain_ops msc313_intc_domain_ops = {
		.translate = msc313_intc_domain_translate,
		.alloc = msc313_intc_domain_alloc,
		.free = irq_domain_free_irqs_common,
};

static int  msc313_intc_of_init(struct device_node *node,
				   struct device_node *parent,
				   void (*eoi)(struct irq_data *data))
{
	struct irq_domain *domain_parent;
	struct msc313_intc *intc;
	int ret = 0;
	u32 gicoffset, numirqs;

	if (of_property_read_u32(node, "mstar,gic-offset", &gicoffset)) {
		ret = -EINVAL;
		goto out;
	}

	if (of_property_read_u32(node, "mstar,nr-interrupts", &numirqs)) {
		ret = -EINVAL;
		goto out;
	}

	domain_parent = irq_find_host(parent);
	if (!domain_parent) {
		ret = -EINVAL;
		goto out;
	}

	intc = kzalloc(sizeof(*intc), GFP_KERNEL);
	if (!intc) {
		ret = -ENOMEM;
		goto out;
	}

	intc->base = of_iomap(node, 0);
	if (IS_ERR(intc->base)) {
		ret = PTR_ERR(intc->base);
		goto free_intc;
	}

	intc->irqchip.name = node->name;
	intc->irqchip.irq_mask = msc313_intc_mask_irq;
	intc->irqchip.irq_unmask = msc313_intc_unmask_irq;
	intc->irqchip.irq_eoi = eoi;
	intc->irqchip.irq_set_type = msc313_intc_set_type_irq;
	intc->irqchip.irq_retrigger = irq_chip_retrigger_hierarchy;
	intc->irqchip.flags = IRQCHIP_MASK_ON_SUSPEND;

	intc->gicoff = gicoffset;

	intc->domain = irq_domain_add_hierarchy(domain_parent, 0, numirqs, node,
			&msc313_intc_domain_ops, intc);
	if (!intc->domain) {
		ret = -ENOMEM;
		goto unmap;
	}

	return 0;

unmap:
	iounmap(intc->base);
free_intc:
	kfree(intc);
out:
	return ret;
}

static int __init msc313_intc_irq_of_init(struct device_node *node,
				   struct device_node *parent)
{
	return msc313_intc_of_init(node, parent, irq_chip_eoi_parent);
};

static int __init msc313_intc_fiq_of_init(struct device_node *node,
				   struct device_node *parent)
{
	return msc313_intc_of_init(node, parent, msc313_intc_irq_eoi);
};

IRQCHIP_DECLARE(msc313_intc_irq, "mstar,msc313-intc-irq",
		msc313_intc_irq_of_init);
IRQCHIP_DECLARE(mstar_intc_fiq, "mstar,msc313-intc-fiq",
		msc313_intc_fiq_of_init);
