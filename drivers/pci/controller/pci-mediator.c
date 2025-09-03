#include <asm/amigaints.h>
#include <linux/irqdomain.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/zorro.h>

#define MEDIATOR4000_SETUP_OFF		0x0
#define MEDIATOR4000_SETUP_SZ		0x20
#define MEDIATOR4000_SETUP_WINDOW	0x0
#define MEDIATOR4000_SETUP_INTR		0x4
#define MEDIATOR4000_CONF_OFF		0x800000
#define MEDIATOR4000_CONF_SZ		0x400000
#define MEDIATOR4000_IO_OFF		0xC00000
#define MEDIATOR4000_IO_SZ		0x100000

/* window position register */
#define MEDIATOR4000_SETUP_WINDOW_OFF	0x0
/* interrupt setup */
#define MEDIATOR4000_SETUP_INTR_OFF	0x4
#define MEDIATOR4000_WINDOW_SHIFT	0x18

#define	MEDIATOR4000_CONF_DEV_STRIDE	0x800
#define MEDIATOR4000_CONF_FUNC_STRIDE	0x100

#define MEDIATOR4000_CONF_DEVFUNC_OFF(priv, devfn) \
	(priv->config_base +\
	(MEDIATOR4000_CONF_DEV_STRIDE * PCI_SLOT(devfn)) +\
	(MEDIATOR4000_CONF_FUNC_STRIDE * PCI_FUNC(devfn)))

#define MEDIATOR4000_IRQ		IRQ_AMIGA_PORTS

#define MEDIATOR4000_ID_CONTROL		ZORRO_ID(ELBOX_COMPUTER, 0x21, 0)
#define MEDIATOR4000_ID_WINDOW		ZORRO_ID(ELBOX_COMPUTER, 0x21 | 0x80, 0)
#define MEDIATOR4000_MAX_SLOTS		6

struct pci_mediator4000_priv {
	struct resource pci_io_res;
	unsigned long config_base;
	unsigned long setup_base;
	struct irq_domain *irqdomain;
	int irqmap[PCI_NUM_INTX];
};

static int pci_mediator4000_readconfig(struct pci_bus *bus, unsigned int devfn,
	int where, int size, u32 *value)
{
	struct pci_mediator4000_priv *priv = bus->sysdata;
	unsigned long addr = MEDIATOR4000_CONF_DEVFUNC_OFF(priv, devfn) + where;
	u32 v;

	if (PCI_SLOT(devfn) >= MEDIATOR4000_MAX_SLOTS)
		return PCIBIOS_FUNC_NOT_SUPPORTED;

//	printk("%s:%d 0x%x 0x%x (%d)\n", __func__, __LINE__,
//		(unsigned) addr, (unsigned) *value, (int) size);

	/* Apparently reads need to be aligned */
	v = z_readl(addr & ~0x3);

	switch (size) {
	case 4:
		v = le32_to_cpu(v);
		break;
	case 2:
		v = le16_to_cpu(((u16*)(&v))[(addr & 0x3) >> 1]);
		break;
	case 1:
		v = ((u8*)(&v))[addr & 0x3];
		break;
	default:
		return PCIBIOS_FUNC_NOT_SUPPORTED;
	}

	*value = v;

        /* Fix up unconfigured interrupts */
        if (where == PCI_INTERRUPT_PIN && size == 1 && !(*value)) {
		printk("fixing up bad interrupt pin\n");
                *value = 0x01;
	}
        else if (where == PCI_INTERRUPT_LINE && !(*value & 0xff00))
                *value |= 0x0100;

	return PCIBIOS_SUCCESSFUL;
}

static int pci_mediator4000_writeconfig(struct pci_bus *bus, unsigned int devfn,
	int where, int size, u32 value)
{
	struct pci_mediator4000_priv *priv = bus->sysdata;
	unsigned long addr = MEDIATOR4000_CONF_DEVFUNC_OFF(priv, devfn) + where;
	u32 v;

	if (PCI_SLOT(devfn) >= MEDIATOR4000_MAX_SLOTS)
		return PCIBIOS_FUNC_NOT_SUPPORTED;

	/* Aligned, just write it */
	if (size == 4)
	{
		v = cpu_to_le32(value);
		z_writel(v, addr);
		return PCIBIOS_SUCCESSFUL;
	}

	/* Not aligned, do RMW */
	v = z_readl(addr & ~0x3);

	switch (size) {
	case 2:
		((u16*)(&v))[(addr & 0x3) >> 1] = cpu_to_le16((u16)value);
		break;
	case 1:
		((u8*)(&v))[addr & 0x3] = value;
		break;
	default:
		return PCIBIOS_FUNC_NOT_SUPPORTED;
	}

	z_writel(v, addr & ~0x3);

	printk("%s:%d 0x%x 0x%x (%d)\n", __func__, __LINE__, (unsigned) addr, (unsigned) value, size);

	return PCIBIOS_SUCCESSFUL;
}

static void pci_mediator4000_dump_reg(struct pci_mediator4000_priv *priv)
{
	u8 regs[8];

	for (int i = 0; i < ARRAY_SIZE(regs); i++)
		regs[i] = z_readb(priv->setup_base + (i * 4));
	printk("Registers: \n");
	for (int i = 0; i < ARRAY_SIZE(regs); i++)
		printk("0x%x: 0x%02x\n", i, regs[i]);
}

static irqreturn_t pci_mediator4000_irq(int irq, void *dev_id)
{
	struct pci_mediator4000_priv *priv = dev_id;
	u8 v = z_readb(priv->setup_base + MEDIATOR4000_SETUP_INTR);
	int i;

//	printk("%s:%d\n", __func__, __LINE__);
//	pci_mediator4000_dump_reg(priv);

	//chained_irq_enter(irqchip, desc);

	for (i = 0; i < PCI_NUM_INTX; i++) {
		if (v & BIT(i))
			generic_handle_domain_irq(priv->irqdomain, i);
	}

	//chained_irq_exit(irqchip, desc);

	return IRQ_HANDLED;
}

static int pci_mediator4000_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)
{
	struct pci_mediator4000_priv *priv = dev->bus->sysdata;

	return priv->irqmap[pin - 1];
}

static struct pci_ops pci_mediator4000_ops = {
	.read	 = pci_mediator4000_readconfig,
	.write	 = pci_mediator4000_writeconfig,
};

static struct resource mediator4000_busn = {
	.name = "mediator4000 busn",
	.start = 0,
	.end = 6,
	.flags	= IORESOURCE_BUS,
};

static void pci_mediator4000_ack_irq(struct irq_data *d)
{
//	printk("%s:%d\n", __func__, __LINE__);
}

static void pci_mediator4000_mask_irq(struct irq_data *d)
{
	struct pci_mediator4000_priv *priv = irq_data_get_irq_chip_data(d);
	u8 v = z_readb(priv->setup_base + MEDIATOR4000_SETUP_INTR);

	v &= ~(BIT(irqd_to_hwirq(d)) << 4);
	z_writeb(v, priv->setup_base + MEDIATOR4000_SETUP_INTR);
}

static void pci_mediator4000_unmask_irq(struct irq_data *d)
{
	struct pci_mediator4000_priv *priv = irq_data_get_irq_chip_data(d);
	u8 v = z_readb(priv->setup_base + MEDIATOR4000_SETUP_INTR);

	v |= (BIT(irqd_to_hwirq(d)) << 4);
	z_writeb(v, priv->setup_base + MEDIATOR4000_SETUP_INTR);
}

static struct irq_chip pci_mediator4000_irq_chip = {
	.name = "mediator4000",
	.irq_ack = pci_mediator4000_ack_irq,
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
	long unsigned control_base = m4k_control->resource.start;
	struct pci_mediator4000_priv *priv;
	struct pci_host_bridge *bridge;
	struct resource *res;
	int i, err;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	err = devm_request_irq(dev, MEDIATOR4000_IRQ, pci_mediator4000_irq,
			       IRQF_SHARED, "mediator4000", priv);
	if (err)
		return -ENODEV;

	struct fwnode_handle *fwnode = irq_domain_alloc_named_fwnode("mediator4000");
	priv->irqdomain = irq_domain_create_linear(fwnode,
						  PCI_NUM_INTX,
						  &pci_mediator4000_irqdomain_ops,
						  priv);
	if (!priv->irqdomain)
		return -ENOMEM;

	for (i = 0; i < PCI_NUM_INTX; i++) {
		priv->irqmap[i] = irq_create_mapping(priv->irqdomain, i);
		if (!priv->irqmap[i]) {
			printk("failed to create irq mapping\n");
			return -ENODEV;
		}
	}

	bridge = devm_pci_alloc_host_bridge(dev, 0);
	if (!bridge)
		return -ENOMEM;

	res = devm_request_mem_region(dev, control_base + MEDIATOR4000_SETUP_OFF,
				      MEDIATOR4000_SETUP_SZ, "mediator4000-registers");
	if (!res)
		return -ENOMEM;

	priv->setup_base = res->start;
	pci_mediator4000_dump_reg(priv);

	z_writeb(m4k_window->resource.start >> MEDIATOR4000_WINDOW_SHIFT,
		 priv->setup_base + MEDIATOR4000_SETUP_WINDOW);

	pci_mediator4000_dump_reg(priv);

	res = devm_request_mem_region(dev, control_base + MEDIATOR4000_CONF_OFF,
				 MEDIATOR4000_CONF_SZ, "mediator4000-pci-config");
	if (!res)
		return -ENOMEM;

	priv->config_base = res->start;

	res = devm_request_mem_region(dev, control_base + MEDIATOR4000_IO_OFF,
				  MEDIATOR4000_IO_SZ, "mediator4000-pci-io");
	if (!res) {
		printk("Failed to request PCI IO region\n");
		return -ENOMEM;
	}

	priv->pci_io_res.name = "mediator4000-pci-io",
	priv->pci_io_res.flags  = IORESOURCE_IO,
	priv->pci_io_res.start = res->start;
	priv->pci_io_res.end = res->end;
	res = insert_resource_conflict(&ioport_resource, &priv->pci_io_res);
	if (res)
		return -ENOMEM;

	pci_add_resource_offset(&bridge->windows, &priv->pci_io_res, priv->pci_io_res.start);
	pci_add_resource(&bridge->windows, &m4k_window->resource);
	pci_add_resource(&bridge->windows, &mediator4000_busn);

	bridge->sysdata = priv;
	bridge->ops = &pci_mediator4000_ops;
	bridge->swizzle_irq = pci_common_swizzle;
	bridge->map_irq = pci_mediator4000_map_irq;

	return pci_host_probe(bridge);
}

static int pci_mediator4000_probe(struct zorro_dev *m4k_control,
			      const struct zorro_device_id *ent)
{
	struct device *dev = &m4k_control->dev;
	struct zorro_dev *m4k_window;

	m4k_window = zorro_find_device(MEDIATOR4000_ID_WINDOW, m4k_control);
	if (!m4k_window) {
		printk("Could not find window for Mediator 4000\n");
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
MODULE_DEVICE_TABLE(zorro, mediator_zorro_tbl);

static struct zorro_driver pci_mediator4000_driver = {
	.name     = "pci_mediator4000",
	.id_table = pci_mediator4000_zorro_tbl,
	.probe    = pci_mediator4000_probe,
};

module_driver(pci_mediator4000_driver,
	      zorro_register_driver,
	      zorro_unregister_driver);
