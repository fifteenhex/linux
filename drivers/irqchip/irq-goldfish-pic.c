// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for MIPS Goldfish Programmable Interrupt Controller.
 *
 * Author: Miodrag Dinic <miodrag.dinic@mips.com>
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

#define GFPIC_NR_IRQS			32

#define GFPIC_REG_IRQ_PENDING		0x04
#define GFPIC_REG_IRQ_DISABLE_ALL	0x08
#define GFPIC_REG_IRQ_DISABLE		0x0c
#define GFPIC_REG_IRQ_ENABLE		0x10

struct goldfish_pic_data {
	void __iomem *base;
	struct irq_domain *irq_domain;
};

static void goldfish_pic_cascade(struct irq_desc *desc)
{
	struct goldfish_pic_data *gfpic = irq_desc_get_handler_data(desc);
	struct irq_chip *host_chip = irq_desc_get_chip(desc);
	u32 pending, hwirq;

	chained_irq_enter(host_chip, desc);

	pending = gf_ioread32(gfpic->base + GFPIC_REG_IRQ_PENDING);
	while (pending) {
		hwirq = __fls(pending);
		generic_handle_domain_irq(gfpic->irq_domain, hwirq);
		pending &= ~(1 << hwirq);
	}

	chained_irq_exit(host_chip, desc);
}

static const struct irq_domain_ops goldfish_irq_domain_ops = {
	.xlate = irq_domain_xlate_onecell,
	.map = irq_map_generic_chip,
};

static int __init goldfish_pic_of_init(struct device_node *of_node,
				       struct device_node *parent)
{
	struct goldfish_pic_data *gfpic;
	struct irq_chip_generic *gc;
	struct irq_chip_type *ct;
	unsigned int parent_irq;
	int ret = 0;

	gfpic = kzalloc(sizeof(*gfpic), GFP_KERNEL);
	if (!gfpic) {
		ret = -ENOMEM;
		goto out_err;
	}

	parent_irq = irq_of_parse_and_map(of_node, 0);
	if (!parent_irq) {
		pr_err("%pOF: Failed to map parent IRQ!\n",
		       of_node);
		ret = -EINVAL;
		goto out_free;
	}

	gfpic->base = of_iomap(of_node, 0);
	if (!gfpic->base) {
		pr_err("%pOF: Failed to map base address!\n",
		       of_node);
		ret = -ENOMEM;
		goto out_unmap_irq;
	}

	/* Mask interrupts. */
	gf_iowrite32(1, gfpic->base + GFPIC_REG_IRQ_DISABLE_ALL);

	gfpic->irq_domain = irq_domain_add_simple(of_node, GFPIC_NR_IRQS, 0,
			&goldfish_irq_domain_ops, NULL);
	if (!gfpic->irq_domain) {
		pr_err("%pOF: Failed to add irqdomain!\n", of_node);
		ret = -ENOMEM;
		goto out_iounmap;
	}

	ret = irq_alloc_domain_generic_chips(gfpic->irq_domain, GFPIC_NR_IRQS, 1, "pic",
					     handle_level_irq, 0, 0, 0);
	if (ret) {
		pr_err("%pOF: Could not allocate generic interrupt chip.\n",
		       of_node);
		goto out_free_domain;
	}

	gc = irq_get_domain_generic_chip(gfpic->irq_domain, 0);

	gc->reg_base = gfpic->base;
	gc->reg_readl = gf_ioread32;
	gc->reg_writel = gf_iowrite32;
	ct = gc->chip_types;
	ct->regs.enable = GFPIC_REG_IRQ_ENABLE;
	ct->regs.disable = GFPIC_REG_IRQ_DISABLE;
	ct->chip.irq_unmask = irq_gc_unmask_enable_reg;
	ct->chip.irq_mask = irq_gc_mask_disable_reg;

	irq_set_chained_handler_and_data(parent_irq,
					 goldfish_pic_cascade, gfpic);

	pr_info("%pOF: Successfully registered.\n",
		of_node);
	return 0;

out_free_domain:
	irq_domain_remove(gfpic->irq_domain);
out_iounmap:
	iounmap(gfpic->base);
out_unmap_irq:
	irq_dispose_mapping(parent_irq);
out_free:
	kfree(gfpic);
out_err:
	pr_err("Failed to initialize! (errno = %d)\n", ret);
	return ret;
}

IRQCHIP_DECLARE(google_gf_pic, "google,goldfish-pic", goldfish_pic_of_init);
