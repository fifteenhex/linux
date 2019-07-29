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

/* The MSC313 contains two interrupt controllers that are almost identical.
 * The first one handles "FIQ" interrupts and the second handles "IRQ"
 * interrupts. The only differences are the first one only has bits for 32
 * interrupts and needs irqs to be cleared.
 *
 * It's also worth noting that the GIC needs to be configured to disable
 * bypassing the GIC when delivering interrupts from the FIQ controller.
 * Currently this is being done by u-boot.
 *
 * 0x1f201310
 * GIC 96 - 127 - hardware fiq interrupts that go through the intc before the gic
 * mask bits
 * 0x0 0  - 15
 * 0x4 16 - 31
 *
 * polarity
 * 0x10 0 - 15
 * 0x14 16 - 31
 *
 * clear bits
 * 0x20
 * 0x24
 *
 * 0x1f201350
 * GIC 32 - 95 - hardware irq interrupts that go through the intc before the gic
 * mask bits
 * 0x0 - 0 - 15
 * 0x4 - 16 - 31
 * 0x8 - 31 - 47
 * 0xc - 48 - 63
 *
 * polarity
 * 0x10 - 0 - 15
 * 0x14 - 16 - 31
 * 0x18 - 31 - 47
 * 0x1c - 48 - 63
 *
 * raw interrupt status bits -- where did this come from?
 * 0x20
 * 0x24
 * 0x28
 * 0x2c
 */

#define REGOFF_MASK		0x0
#define REGOFF_POLARITY		0x10
#define REGOFF_STATUSCLEAR	0x20
#define BITOFF(hwirq)		(hwirq % 16)
#define REGOFF(hwirq)		((hwirq >> 4) * 4)

struct msc313e_intc {
	void __iomem *base;
	u8 gicoff;
	struct irq_chip *irqchip;
};

static void msc313e_intc_maskunmask_hw_int(struct msc313e_intc *intc, int hwirq, bool mask){
	int bitoff = BITOFF(hwirq);
	int regoff = REGOFF(hwirq);
	__iomem void* addr = intc->base + REGOFF_MASK + regoff;
	u16 reg = readw_relaxed(addr);

	if (mask) {
		reg |= 1 << bitoff;
	} else {
		reg &= ~(1 << bitoff);
	}

	writew_relaxed(reg, addr);
}

static void msc313e_intc_mask_irq(struct irq_data *data)
{
	struct msc313e_intc* intc = data->chip_data;
	msc313e_intc_maskunmask_hw_int(intc, data->hwirq, true);
	irq_chip_mask_parent(data);
}

static void msc313e_intc_unmask_irq(struct irq_data *data)
{
	struct msc313e_intc* intc = data->chip_data;
	msc313e_intc_maskunmask_hw_int(intc, data->hwirq, false);
	irq_chip_unmask_parent(data);
}

static int msc313e_intc_set_type_irq(struct irq_data *data, unsigned int flow_type)
{
	struct msc313e_intc* intc = data->chip_data;
	int irq = data->hwirq;
	int bitoff = BITOFF(irq);
	int regoff = REGOFF(irq);
	__iomem void* addr = intc->base + REGOFF_POLARITY + regoff;
	u16 reg = readw_relaxed(addr);

	if (flow_type & (IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_LEVEL_HIGH)) {
		reg &= ~(1 << bitoff);
	} else {
		reg |= 1 << bitoff;
	}

	writew_relaxed(reg, addr);
	return 0;
}

static void msc313e_intc_irq_eoi(struct irq_data *data){
	struct msc313e_intc* intc = data->chip_data;
	int irq = data->hwirq;
	int bitoff = BITOFF(irq);
	int regoff = REGOFF(irq);
	__iomem void* addr = intc->base + REGOFF_STATUSCLEAR + regoff;
	u16 reg = readw_relaxed(addr);
	reg |= 1 << bitoff;
	writew_relaxed(reg, addr);
	irq_chip_eoi_parent(data);
}

static struct irq_chip msc313e_intc_spi_chip = {
	.name			= "INTC-IRQ",
	.irq_mask		= msc313e_intc_mask_irq,
	.irq_unmask		= msc313e_intc_unmask_irq,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_set_type	= msc313e_intc_set_type_irq,
};

static struct irq_chip msc313e_intc_fiq_chip = {
	.name			= "INTC-FIQ",
	.irq_mask		= msc313e_intc_mask_irq,
	.irq_unmask		= msc313e_intc_unmask_irq,
	.irq_eoi		= msc313e_intc_irq_eoi,
	.irq_retrigger	= irq_chip_retrigger_hierarchy,
	.irq_set_type	= msc313e_intc_set_type_irq,
};

static int msc313e_intc_domain_translate(struct irq_domain *d,
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

static int msc313e_intc_domain_alloc(struct irq_domain *domain, unsigned int virq,
				 unsigned int nr_irqs, void *data)
{
	struct irq_fwspec *fwspec = data;
	struct irq_fwspec parent_fwspec;
	struct msc313e_intc* intc = domain->host_data;

	if (fwspec->param_count != 2)
		return -EINVAL;

	irq_domain_set_hwirq_and_chip(domain, virq, fwspec->param[0], intc->irqchip, intc);

	parent_fwspec.fwnode = domain->parent->fwnode;
	parent_fwspec.param[0] = GIC_SPI;
	parent_fwspec.param[1] = fwspec->param[0] + intc->gicoff;
	parent_fwspec.param[2] = fwspec->param[1];
	parent_fwspec.param_count = 3;

	return irq_domain_alloc_irqs_parent(domain, virq, nr_irqs,
					    &parent_fwspec);
}

static const struct irq_domain_ops msc313e_intc_domain_ops = {
		.translate = msc313e_intc_domain_translate,
		.alloc = msc313e_intc_domain_alloc,
		.free = irq_domain_free_irqs_common,
};

static int __init msc313e_intc_of_init(struct device_node *node,
				   struct device_node *parent, u8 gicoff, u8 numirqs, bool ack,
				   struct irq_chip *irqchip)
{
	struct irq_domain *domain, *domain_parent;
	struct msc313e_intc *intc;

	domain_parent = irq_find_host(parent);
	if (!domain_parent) {
		pr_err("msc313e-intc: interrupt-parent not found\n");
		return -EINVAL;
	}

	intc = kzalloc(sizeof(*intc), GFP_KERNEL);
	if (!intc)
		return -ENOMEM;

	intc->irqchip = irqchip;
	intc->gicoff = gicoff;

	intc->base = of_iomap(node, 0);
	if (IS_ERR(intc->base))
		return PTR_ERR(intc->base);

	domain = irq_domain_add_hierarchy(domain_parent, 0, numirqs, node,
			&msc313e_intc_domain_ops, intc);
	if (!domain) {
		pr_err("msc313e-intc: failed to add irq domain\n");
		return -ENOMEM;
	}

	return 0;
}

static int __init msc313e_intc_spi_of_init(struct device_node *node,
				   struct device_node *parent)
{
	return msc313e_intc_of_init(node, parent, 32, 64, false, &msc313e_intc_spi_chip);
};

static int __init msc313e_intc_fiq_of_init(struct device_node *node,
				   struct device_node *parent)
{
	return msc313e_intc_of_init(node, parent, 96, 32, false, &msc313e_intc_fiq_chip);
};

IRQCHIP_DECLARE(mstar_msc313e_intc_spi, "mstar,msc313e-intc-irq",
		msc313e_intc_spi_of_init);
IRQCHIP_DECLARE(mstar_msc313e_intc_fiq, "mstar,msc313e-intc-fiq",
		msc313e_intc_fiq_of_init);
