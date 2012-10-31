#include <asm/i387.h>

#include "../comedidev.h"
#include "comedi_fc.h"

#include "addi-data/addi_common.h"
#include "addi-data/addi_amcc_s5933.h"

static void fpu_begin(void)
{
	kernel_fpu_begin();
}

static void fpu_end(void)
{
	kernel_fpu_end();
}

#include "addi-data/addi_eeprom.c"
#include "addi-data/hwdrv_apci3200.c"
#include "addi-data/addi_common.c"

static const struct addi_board apci3300_boardtypes[] = {
	{
		.pc_DriverName		= "apci3300",
		.i_VendorId		= PCI_VENDOR_ID_ADDIDATA,
		.i_DeviceId		= 0x3007,
		.i_IorangeBase0		= 128,
		.i_IorangeBase1		= 256,
		.i_IorangeBase2		= 4,
		.i_IorangeBase3		= 4,
		.i_PCIEeprom		= ADDIDATA_EEPROM,
		.pc_EepromChip		= ADDIDATA_S5920,
		.i_NbrAiChannelDiff	= 8,
		.i_AiChannelList	= 8,
		.i_AiMaxdata		= 0x3ffff,
		.pr_AiRangelist		= &range_apci3300_ai,
		.i_NbrDiChannel		= 4,
		.i_NbrDoChannel		= 4,
		.ui_MinAcquisitiontimeNs = 10000,
		.ui_MinDelaytimeNs	= 100000,
		.interrupt		= v_APCI3200_Interrupt,
		.reset			= i_APCI3200_Reset,
		.ai_config		= i_APCI3200_ConfigAnalogInput,
		.ai_read		= i_APCI3200_ReadAnalogInput,
		.ai_write		= i_APCI3200_InsnWriteReleaseAnalogInput,
		.ai_bits		= i_APCI3200_InsnBits_AnalogInput_Test,
		.ai_cmdtest		= i_APCI3200_CommandTestAnalogInput,
		.ai_cmd			= i_APCI3200_CommandAnalogInput,
		.ai_cancel		= i_APCI3200_StopCyclicAcquisition,
		.di_bits		= i_APCI3200_ReadDigitalInput,
		.do_config		= i_APCI3200_ConfigDigitalOutput,
		.do_write		= i_APCI3200_WriteDigitalOutput,
		.do_bits		= i_APCI3200_ReadDigitalOutput,
	},
};

static struct comedi_driver apci3300_driver = {
	.driver_name	= "addi_apci_3300",
	.module		= THIS_MODULE,
	.attach		= i_ADDI_Attach,
	.detach		= i_ADDI_Detach,
	.num_names	= ARRAY_SIZE(apci3300_boardtypes),
	.board_name	= &apci3300_boardtypes[0].pc_DriverName,
	.offset		= sizeof(struct addi_board),
};

static int __devinit apci3300_pci_probe(struct pci_dev *dev,
					const struct pci_device_id *ent)
{
	return comedi_pci_auto_config(dev, &apci3300_driver);
}

static void __devexit apci3300_pci_remove(struct pci_dev *dev)
{
	comedi_pci_auto_unconfig(dev);
}

static DEFINE_PCI_DEVICE_TABLE(apci3300_pci_table) = {
	{ PCI_DEVICE(PCI_VENDOR_ID_ADDIDATA, 0x3007) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, apci3300_pci_table);

static struct pci_driver apci3300_pci_driver = {
	.name		= "addi_apci_3300",
	.id_table	= apci3300_pci_table,
	.probe		= apci3300_pci_probe,
	.remove		= __devexit_p(apci3300_pci_remove),
};
module_comedi_pci_driver(apci3300_driver, apci3300_pci_driver);
