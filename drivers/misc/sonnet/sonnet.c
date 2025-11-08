// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Daniel Palmer <daniel@thingy.jp>
 * 
 * See: https://www.nxp.com/docs/en/product-brief/MPC107TS.pdf
 * 	https://www.nxp.com/docs/en/reference-manual/MPC107UM.pdf
 * 	arch/powerpc/platforms/embedded6xx/mpc10x.h
 */
#include "linux/gfp_types.h"
#include <linux/pci.h>

#define DRIVER_NAME "sonnet"

#define SONNET_MPC107_PCICFG_EUMBBAR	0x78

static const struct pci_device_id sonnet_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_MOTOROLA, PCI_DEVICE_ID_MOTOROLA_MPC107) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, sonnet_ids);

struct sonnet_priv {
	int x;
};

static int sonnet_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct sonnet_priv *priv;
	u32 eummbar;
	int ret;
	u16 tmp;

	printk("%s:%d\n", __func__, __LINE__);

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	pci_read_config_word(pdev, PCI_COMMAND, &tmp);
	printk("%s:%d 0x%08x\n", __func__, __LINE__, (unsigned int) tmp);
	if (tmp & PCI_COMMAND_MEMORY) {
		tmp &= ~PCI_COMMAND_MEMORY;
		pci_write_config_word(pdev, PCI_COMMAND, tmp);
	}

	pci_read_config_word(pdev, PCI_COMMAND, &tmp);
	printk("%s:%d 0x%08x\n", __func__, __LINE__, (unsigned int) tmp);

	printk("%s:%d\n", __func__, __LINE__);
	ret = pci_write_config_dword(pdev, SONNET_MPC107_PCICFG_EUMBBAR, 0xF0000000);
	if (ret)
		return ret;

	ret = pci_read_config_dword(pdev, SONNET_MPC107_PCICFG_EUMBBAR, &eummbar);
	if (ret)
		return ret;

	tmp |= PCI_COMMAND_MEMORY;
	pci_write_config_word(pdev, PCI_COMMAND, tmp);

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	
	printk("%s:%d 0x%08x\n", __func__, __LINE__, (unsigned int) eummbar);


	
	ret = pcim_request_all_regions(pdev, DRIVER_NAME);
	if (ret)
		return ret;

	// todo, not sure what the BAR is for yet..
	//priv->regs = pci_resource_start(pdev, 0);

	pci_set_drvdata(pdev, priv);

	return 0;
}

static struct pci_driver sonnet_driver = {
	.name = DRIVER_NAME,
	.id_table = sonnet_ids,
	.probe = sonnet_probe,
};

module_pci_driver(sonnet_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Daniel Palmer <daniel@thingy.jp>");
MODULE_DESCRIPTION("Sonnet PowerPC on PCI card driver");
