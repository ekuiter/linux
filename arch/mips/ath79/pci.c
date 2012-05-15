/*
 *  Atheros AR71XX/AR724X specific PCI setup code
 *
 *  Copyright (C) 2011 René Bolldorf <xsecute@googlemail.com>
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 2 as published
 *  by the Free Software Foundation.
 */

#include <linux/pci.h>
#include <asm/mach-ath79/pci-ath724x.h>

static struct ath724x_pci_data *pci_data;
static int pci_data_size;

void ath724x_pci_add_data(struct ath724x_pci_data *data, int size)
{
	pci_data	= data;
	pci_data_size	= size;
}

int __init pcibios_map_irq(const struct pci_dev *dev, uint8_t slot, uint8_t pin)
{
	unsigned int devfn = dev->devfn;
	int irq = -1;

	if (devfn > pci_data_size - 1)
		return irq;

	irq = pci_data[devfn].irq;

	return irq;
}

int pcibios_plat_dev_init(struct pci_dev *dev)
{
	unsigned int devfn = dev->devfn;

	if (devfn > pci_data_size - 1)
		return PCIBIOS_DEVICE_NOT_FOUND;

	dev->dev.platform_data = pci_data[devfn].pdata;

	return PCIBIOS_SUCCESSFUL;
}
