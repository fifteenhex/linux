// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Daniel Palmer <daniel@thingy.jp>
 */

#include <asm/amigaints.h>
#include <linux/bitfield.h>
#include <linux/irqdomain.h>
#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/zorro.h>

/* Offsets etc */
#define MEDIATOR4000_CONTROL			0x0
#define MEDIATOR4000_CONTROL_SIZE		SZ_4
#define MEDIATOR4000_CONTROL_WINDOW		0x0
#define MEDIATOR4000_CONTROL_WINDOW_MSB		GENMASK(31,28)
#define MEDIATOR4000_CONTROL_IRQ		0x4
#define MEDIATOR4000_CONTROL_IRQ_STATUS		GENMASK(3, 0)
#define MEDIATOR4000_CONTROL_IRQ_MASK		GENMASK(7, 4)
#define MEDIATOR4000_PCICONF			0x800000
#define MEDIATOR4000_PCICONF_SIZE		SZ_4M
#define MEDIATOR4000_PCICONF_DEV_STRIDE		SZ_2K
#define MEDIATOR4000_PCICONF_FUNC_STRIDE	SZ_256
#define MEDIATOR4000_PCIIO			0xc00000
#define MEDIATOR4000_PCIIO_SIZE			SZ_1M

/* How the Amiga sees the mediator 4000 */
#define MEDIATOR4000_IRQ	IRQ_AMIGA_PORTS
#define MEDIATOR4000_ID_CONTROL	ZORRO_ID(ELBOX_COMPUTER, 0x21, 0)
#define MEDIATOR4000_ID_WINDOW	ZORRO_ID(ELBOX_COMPUTER, 0x21 | 0x80, 0)

/*
 * There are a few versions of the PCI backplane board,
 * at most there are 6 slots it seems.
 */
#define MEDIATOR4000_MAX_SLOTS 6

#define MEDIATOR4000_PCICONF_DEVFUNC_OFF(priv, devfn)		\
	(priv->config_base +					\
	(MEDIATOR4000_PCICONF_DEV_STRIDE * PCI_SLOT(devfn)) +	\
	(MEDIATOR4000_PCICONF_FUNC_STRIDE * PCI_FUNC(devfn)))

struct pci_mediator4000_priv {
	struct resource pciio_res;
	unsigned long config_base;
	unsigned long setup_base;
	struct irq_domain *irqdomain;
	int irqmap[PCI_NUM_INTX];
};

static int pci_mediator4000_readconfig(struct pci_bus *bus, unsigned int devfn,
				       int where, int size, u32 *value)
{
	struct pci_mediator4000_priv *priv = bus->sysdata;
	unsigned long addr = MEDIATOR4000_PCICONF_DEVFUNC_OFF(priv, devfn) + where;
	u32 v;

	if (PCI_SLOT(devfn) >= MEDIATOR4000_MAX_SLOTS)
		return PCIBIOS_FUNC_NOT_SUPPORTED;

	/* Apparently only reading longs works */
	v = z_readl(addr & ~0x3);

	switch (size) {
	case 4:
		v = le32_to_cpu(v);
		break;
	case 2:
		v = le16_to_cpu(((u16 *)(&v))[(addr & 0x3) >> 1]);
		break;
	case 1:
		v = ((u8 *)&v)[addr & 0x3];
		break;
	default:
		return PCIBIOS_FUNC_NOT_SUPPORTED;
	}

	*value = v;

	return PCIBIOS_SUCCESSFUL;
}

static int pci_mediator4000_writeconfig(struct pci_bus *bus, unsigned int devfn,
					int where, int size, u32 value)
{
	struct pci_mediator4000_priv *priv = bus->sysdata;
	unsigned long addr = MEDIATOR4000_PCICONF_DEVFUNC_OFF(priv, devfn) + where;
	u32 v;

	if (PCI_SLOT(devfn) >= MEDIATOR4000_MAX_SLOTS)
		return PCIBIOS_FUNC_NOT_SUPPORTED;

	/* If its a long just write it */
	if (size == 4) {
		v = cpu_to_le32(value);
		z_writel(v, addr);
		return PCIBIOS_SUCCESSFUL;
	}

	/* Not a long, do RMW */
	v = z_readl(addr & ~0x3);

	switch (size) {
	case 2:
		((u16 *)(&v))[(addr & 0x3) >> 1] = cpu_to_le16((u16)value);
		break;
	case 1:
		((u8 *)(&v))[addr & 0x3] = value;
		break;
	default:
		return PCIBIOS_FUNC_NOT_SUPPORTED;
	}

	z_writel(v, addr & ~0x3);

	return PCIBIOS_SUCCESSFUL;
}

static irqreturn_t pci_mediator4000_irq(int irq, void *dev_id)
{
	struct pci_mediator4000_priv *priv = dev_id;
	unsigned long status;
	int i;
	u8 v;

	v = z_readb(priv->setup_base + MEDIATOR4000_CONTROL_IRQ);
	status = FIELD_GET(MEDIATOR4000_CONTROL_IRQ_STATUS, v);

	for_each_set_bit(i, &status, PCI_NUM_INTX)
		generic_handle_domain_irq(priv->irqdomain, i);

	return IRQ_HANDLED;
}

static int pci_mediator4000_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)
{
	struct pci_mediator4000_priv *priv = dev->bus->sysdata;

	/*
	 * I'm not actually sure how the lines are wired on
	 * mediators with more than 4 slots. My 4 slot board seems to just
	 * have slot number == irq.
	 */
	return priv->irqmap[slot % PCI_NUM_INTX];
}

static struct pci_ops pci_mediator4000_ops = {
	.read = pci_mediator4000_readconfig,
	.write = pci_mediator4000_writeconfig,
};

static struct resource mediator4000_busn = {
	.name = "mediator4000 busn",
	.start = 0,
	.end = 0,
	.flags = IORESOURCE_BUS,
};

static void pci_mediator4000_mask_irq(struct irq_data *d)
{
	struct pci_mediator4000_priv *priv = irq_data_get_irq_chip_data(d);
	u8 v;

	v = z_readb(priv->setup_base + MEDIATOR4000_CONTROL_IRQ);
	v &= ~FIELD_PREP(MEDIATOR4000_CONTROL_IRQ_MASK, BIT(irqd_to_hwirq(d)));
	z_writeb(v, priv->setup_base + MEDIATOR4000_CONTROL_IRQ);
}

static void pci_mediator4000_unmask_irq(struct irq_data *d)
{
	struct pci_mediator4000_priv *priv = irq_data_get_irq_chip_data(d);
	u8 v;

	v = z_readb(priv->setup_base + MEDIATOR4000_CONTROL_IRQ);
	v |= FIELD_PREP(MEDIATOR4000_CONTROL_IRQ_MASK, BIT(irqd_to_hwirq(d)));
	z_writeb(v, priv->setup_base + MEDIATOR4000_CONTROL_IRQ);
}

static struct irq_chip pci_mediator4000_irq_chip = {
	.name = "mediator4000",
	.irq_mask = pci_mediator4000_mask_irq,
	.irq_unmask = pci_mediator4000_unmask_irq,
};

static int pci_mediator4000_irq_map(struct irq_domain *domain, unsigned int irq,
				    irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &pci_mediator4000_irq_chip, handle_level_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops pci_mediator4000_irqdomain_ops = {
	.map = pci_mediator4000_irq_map,
};

static int mediator4000_setup(struct device *dev,
			      struct zorro_dev *m4k_control,
			      struct zorro_dev *m4k_window)
{
	unsigned long control_base = m4k_control->resource.start;
	struct pci_mediator4000_priv *priv;
	struct pci_host_bridge *bridge;
	struct fwnode_handle *fwnode;
	struct resource *res;
	int i, ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ret = devm_request_irq(dev, MEDIATOR4000_IRQ, pci_mediator4000_irq,
			       IRQF_SHARED, "mediator4000", priv);
	if (ret)
		return -ENODEV;

	bridge = devm_pci_alloc_host_bridge(dev, 0);
	if (!bridge)
		return -ENOMEM;

	res = devm_request_mem_region(dev, control_base + MEDIATOR4000_CONTROL,
				      MEDIATOR4000_CONTROL_SIZE, "mediator4000-registers");
	if (!res)
		return -ENOMEM;

	priv->setup_base = res->start;

	/*
	 * Setup the window base, trying to write 0xff into the register showed that
	 * only the top nibble is used so the window address has to be aligned to
	 * 256MB.
	 */
	z_writeb(FIELD_GET(MEDIATOR4000_CONTROL_WINDOW_MSB, ALIGN_DOWN(m4k_window->resource.start,SZ_256M)),
		 priv->setup_base + MEDIATOR4000_CONTROL_WINDOW);

	res = devm_request_mem_region(dev, control_base + MEDIATOR4000_PCICONF,
				 MEDIATOR4000_PCICONF_SIZE, "mediator4000-pci-config");
	if (!res)
		return -ENOMEM;

	priv->config_base = res->start;

	res = devm_request_mem_region(dev, control_base + MEDIATOR4000_PCIIO,
				  MEDIATOR4000_PCIIO_SIZE, "mediator4000-pci-io");
	if (!res)
		return -ENOMEM;

	priv->pciio_res = DEFINE_RES_IO_NAMED(res->start,
					      res->end - res->start,
					      "mediator4000-pci-io");
	res = insert_resource_conflict(&ioport_resource, &priv->pciio_res);
	if (res)
		return -ENOMEM;

	pci_add_resource_offset(&bridge->windows, &priv->pciio_res, priv->pciio_res.start);
	pci_add_resource(&bridge->windows, &m4k_window->resource);
	pci_add_resource(&bridge->windows, &mediator4000_busn);

	/* fixme: PCI DMA can only happen inside the window? How to enforce that? */

	bridge->sysdata = priv;
	bridge->ops = &pci_mediator4000_ops;
	bridge->swizzle_irq = pci_common_swizzle;
	bridge->map_irq = pci_mediator4000_map_irq;

	fwnode = irq_domain_alloc_named_fwnode("mediator4000");
	if (!fwnode)
		return -ENOMEM;

	priv->irqdomain = irq_domain_create_linear(fwnode,
						   PCI_NUM_INTX,
						   &pci_mediator4000_irqdomain_ops,
						   priv);
	if (!priv->irqdomain) {
		ret = -ENOMEM;
		goto err;
	}

	for (i = 0; i < PCI_NUM_INTX; i++)
		priv->irqmap[i] = irq_create_mapping(priv->irqdomain, i);

	ret = pci_host_probe(bridge);
	if (ret)
		goto err;

	return 0;

err:
	irq_domain_free_fwnode(fwnode);
	return ret;
}

static int pci_mediator4000_probe(struct zorro_dev *m4k_control,
				  const struct zorro_device_id *ent)
{
	struct device *dev = &m4k_control->dev;
	struct zorro_dev *m4k_window;

	m4k_window = zorro_find_device(MEDIATOR4000_ID_WINDOW, m4k_control);
	if (!m4k_window) {
		dev_err(&m4k_control->dev, "Could not find window board\n");
		return -ENODEV;
	}

	return mediator4000_setup(dev, m4k_control, m4k_window);
}

static const struct zorro_device_id pci_mediator4000_zorro_tbl[] = {
	{
		.id = MEDIATOR4000_ID_CONTROL,
	},
	{ 0 }
};
MODULE_DEVICE_TABLE(zorro, pci_mediator4000_zorro_tbl);

static struct zorro_driver pci_mediator4000_driver = {
	.name     = "pci_mediator4000",
	.id_table = pci_mediator4000_zorro_tbl,
	.probe    = pci_mediator4000_probe,
};

module_driver(pci_mediator4000_driver,
	      zorro_register_driver,
	      zorro_unregister_driver);
