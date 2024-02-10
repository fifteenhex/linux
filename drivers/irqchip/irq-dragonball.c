// SPDX-License-Identifier: GPL-2.0-or-later
/*
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/goldfish.h>

#define DRAGONBALLINTC_NR_IRQS	32
#define DRAGONBALLINTC_REG_IVR	0x0
#define DRAGONBALLINTC_REG_IMR	0x4
#define DRAGONBALLINTC_REG_ISR	0xc

struct dragonball_intc_data {
	void __iomem *base;
	struct irq_domain *irq_domain;
};

static const struct irq_domain_ops dragonball_irq_domain_ops = {
	.xlate = irq_domain_xlate_onecell,
	.map = irq_map_generic_chip,
};

static void dragonball_intc_cascade(struct irq_desc *desc)
{
	struct dragonball_intc_data *intc = irq_desc_get_handler_data(desc);
	struct irq_chip *host_chip = irq_desc_get_chip(desc);
	u32 pending, hwirq;

	chained_irq_enter(host_chip, desc);

	pending = readl(intc->base + DRAGONBALLINTC_REG_ISR);
	while (pending) {
		hwirq = __fls(pending);
		generic_handle_domain_irq(intc->irq_domain, hwirq);
		pending &= ~(1 << hwirq);
	}

	chained_irq_exit(host_chip, desc);
}

static int __init dragonball_intc_of_init(struct device_node *of_node,
				       struct device_node *parent)
{
	struct dragonball_intc_data *intc;
	struct irq_chip_generic *gc;
	struct irq_chip_type *ct;
	int ret = 0;
	int i;
	unsigned int parent_irqs[7];

	intc = kzalloc(sizeof(*intc), GFP_KERNEL);
	if (!intc) {
		ret = -ENOMEM;
		goto out_err;
	}

	intc->base = of_iomap(of_node, 0);
	if (!intc->base) {
		pr_err("%pOF: Failed to map base address!\n",
		       of_node);
		ret = -ENOMEM;
		goto out_free;
	}

	for (int i = 0; i < ARRAY_SIZE(parent_irqs); i++) {
		parent_irqs[i] = irq_of_parse_and_map(of_node, i);
		if (!parent_irqs[i]) {
			pr_err("%pOF: Failed to map parent IRQ!\n",
				   of_node);
			ret = -EINVAL;
			goto out_free;
		}
		irq_set_chained_handler_and_data(parent_irqs[i],
				dragonball_intc_cascade, intc);
	}

	writeb(0x40, intc->base + DRAGONBALLINTC_REG_IVR);

	intc->irq_domain = irq_domain_add_simple(of_node, DRAGONBALLINTC_NR_IRQS, 0,
			&dragonball_irq_domain_ops, NULL);
	if (!intc->irq_domain) {
		pr_err("%pOF: Failed to add irqdomain!\n", of_node);
		ret = -ENOMEM;
		goto out_iounmap;
	}

	ret = irq_alloc_domain_generic_chips(intc->irq_domain, DRAGONBALLINTC_NR_IRQS, 1, "intc",
					     handle_level_irq, 0, 0, IRQ_GC_INIT_MASK_CACHE);
	if (ret) {
		pr_err("%pOF: Could not allocate generic interrupt chip.\n",
		       of_node);
		goto out_free_domain;
	}

	gc = irq_get_domain_generic_chip(intc->irq_domain, 0);

	gc->reg_base = intc->base;
	gc->reg_readl = ioread32;
	gc->reg_writel = iowrite32;
	ct = gc->chip_types;
	ct->regs.mask = DRAGONBALLINTC_REG_IMR;
	ct->chip.irq_unmask = irq_gc_mask_clr_bit;
	ct->chip.irq_mask = irq_gc_mask_set_bit;

	pr_info("%pOF: Successfully registered.\n",
		of_node);
	return 0;

out_free_domain:
	irq_domain_remove(intc->irq_domain);
out_iounmap:
	iounmap(intc->base);
out_free:
	kfree(intc);
out_err:
	pr_err("Failed to initialize! (errno = %d)\n", ret);
	return ret;
}

IRQCHIP_DECLARE(dragonball, "motorola,mc68ez328-intc", dragonball_intc_of_init);
