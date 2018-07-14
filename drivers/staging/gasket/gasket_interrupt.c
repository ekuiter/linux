// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2018 Google, Inc. */

#include "gasket_interrupt.h"

#include "gasket_constants.h"
#include "gasket_core.h"
#include "gasket_logging.h"
#include "gasket_sysfs.h"
#include <linux/interrupt.h>
#include <linux/version.h>
#ifdef GASKET_KERNEL_TRACE_SUPPORT
#define CREATE_TRACE_POINTS
#include <trace/events/gasket_interrupt.h>
#else
#define trace_gasket_interrupt_event(x, ...)
#endif
/* Retry attempts if the requested number of interrupts aren't available. */
#define MSIX_RETRY_COUNT 3

/* Instance interrupt management data. */
struct gasket_interrupt_data {
	/* The name associated with this interrupt data. */
	const char *name;

	/* Interrupt type. See gasket_interrupt_type in gasket_core.h */
	int type;

	/* The PCI device [if any] associated with the owning device. */
	struct pci_dev *pci_dev;

	/* Set to 1 if MSI-X has successfully been configred, 0 otherwise. */
	int msix_configured;

	/* The number of interrupts requested by the owning device. */
	int num_interrupts;

	/* A pointer to the interrupt descriptor struct for this device. */
	const struct gasket_interrupt_desc *interrupts;

	/* The index of the bar into which interrupts should be mapped. */
	int interrupt_bar_index;

	/* The width of a single interrupt in a packed interrupt register. */
	int pack_width;

	/* offset of wire interrupt registers */
	const struct gasket_wire_interrupt_offsets *wire_interrupt_offsets;

	/*
	 * Design-wise, these elements should be bundled together, but
	 * pci_enable_msix's interface requires that they be managed
	 * individually (requires array of struct msix_entry).
	 */

	/* The number of successfully configured interrupts. */
	int num_configured;

	/* The MSI-X data for each requested/configured interrupt. */
	struct msix_entry *msix_entries;

	/* The eventfd "callback" data for each interrupt. */
	struct eventfd_ctx **eventfd_ctxs;

	/* The number of times each interrupt has been called. */
	ulong *interrupt_counts;

	/* Linux IRQ number. */
	int irq;
};

/* Function definitions. */
static ssize_t interrupt_sysfs_show(
	struct device *device, struct device_attribute *attr, char *buf);

static irqreturn_t gasket_msix_interrupt_handler(int irq, void *dev_id);

/* Structures to display interrupt counts in sysfs. */
enum interrupt_sysfs_attribute_type {
	ATTR_INTERRUPT_COUNTS,
};

static struct gasket_sysfs_attribute interrupt_sysfs_attrs[] = {
	GASKET_SYSFS_RO(
		interrupt_counts, interrupt_sysfs_show, ATTR_INTERRUPT_COUNTS),
	GASKET_END_OF_ATTR_ARRAY,
};

/*
 * Set up device registers for interrupt handling.
 * @gasket_dev: The Gasket information structure for this device.
 *
 * Sets up the device registers with the correct indices for the relevant
 * interrupts.
 */
static void gasket_interrupt_setup(struct gasket_dev *gasket_dev);

/* MSIX init and cleanup. */
static int gasket_interrupt_msix_init(
	struct gasket_interrupt_data *interrupt_data);
static void gasket_interrupt_msix_cleanup(
	struct gasket_interrupt_data *interrupt_data);
static void force_msix_interrupt_unmasking(struct gasket_dev *gasket_dev);

int gasket_interrupt_init(
	struct gasket_dev *gasket_dev, const char *name, int type,
	const struct gasket_interrupt_desc *interrupts,
	int num_interrupts, int pack_width, int bar_index,
	const struct gasket_wire_interrupt_offsets *wire_int_offsets)
{
	int ret;
	struct gasket_interrupt_data *interrupt_data;

	interrupt_data = kzalloc(
		sizeof(struct gasket_interrupt_data), GFP_KERNEL);
	if (!interrupt_data)
		return -ENOMEM;
	gasket_dev->interrupt_data = interrupt_data;
	interrupt_data->name = name;
	interrupt_data->type = type;
	interrupt_data->pci_dev = gasket_dev->pci_dev;
	interrupt_data->num_interrupts = num_interrupts;
	interrupt_data->interrupts = interrupts;
	interrupt_data->interrupt_bar_index = bar_index;
	interrupt_data->pack_width = pack_width;
	interrupt_data->num_configured = 0;
	interrupt_data->wire_interrupt_offsets = wire_int_offsets;

	/* Allocate all dynamic structures. */
	interrupt_data->msix_entries = kcalloc(num_interrupts,
					       sizeof(struct msix_entry),
					       GFP_KERNEL);
	if (!interrupt_data->msix_entries) {
		kfree(interrupt_data);
		return -ENOMEM;
	}

	interrupt_data->eventfd_ctxs = kcalloc(num_interrupts,
					       sizeof(struct eventfd_ctx *),
					       GFP_KERNEL);
	if (!interrupt_data->eventfd_ctxs) {
		kfree(interrupt_data->msix_entries);
		kfree(interrupt_data);
		return -ENOMEM;
	}

	interrupt_data->interrupt_counts = kcalloc(num_interrupts,
						   sizeof(ulong),
						   GFP_KERNEL);
	if (!interrupt_data->interrupt_counts) {
		kfree(interrupt_data->eventfd_ctxs);
		kfree(interrupt_data->msix_entries);
		kfree(interrupt_data);
		return -ENOMEM;
	}

	switch (interrupt_data->type) {
	case PCI_MSIX:
		ret = gasket_interrupt_msix_init(interrupt_data);
		if (ret)
			break;
		force_msix_interrupt_unmasking(gasket_dev);
		break;

	case PCI_MSI:
	case PLATFORM_WIRE:
	default:
		gasket_nodev_error(
			"Cannot handle unsupported interrupt type %d.",
			interrupt_data->type);
		ret = -EINVAL;
	};

	if (ret) {
		/* Failing to setup interrupts will cause the device to report
		 * GASKET_STATUS_LAMED. But it is not fatal.
		 */
		gasket_log_warn(
			gasket_dev, "Couldn't initialize interrupts: %d", ret);
		return 0;
	}

	gasket_interrupt_setup(gasket_dev);
	gasket_sysfs_create_entries(
		gasket_dev->dev_info.device, interrupt_sysfs_attrs);

	return 0;
}

static int gasket_interrupt_msix_init(
	struct gasket_interrupt_data *interrupt_data)
{
	int ret = 1;
	int i;

	for (i = 0; i < interrupt_data->num_interrupts; i++) {
		interrupt_data->msix_entries[i].entry = i;
		interrupt_data->msix_entries[i].vector = 0;
		interrupt_data->eventfd_ctxs[i] = NULL;
	}

	/* Retry MSIX_RETRY_COUNT times if not enough IRQs are available. */
	for (i = 0; i < MSIX_RETRY_COUNT && ret > 0; i++)
		ret = pci_enable_msix_exact(interrupt_data->pci_dev,
					    interrupt_data->msix_entries,
					    interrupt_data->num_interrupts);

	if (ret)
		return ret > 0 ? -EBUSY : ret;
	interrupt_data->msix_configured = 1;

	for (i = 0; i < interrupt_data->num_interrupts; i++) {
		ret = request_irq(
			interrupt_data->msix_entries[i].vector,
			gasket_msix_interrupt_handler, 0, interrupt_data->name,
			interrupt_data);

		if (ret) {
			gasket_nodev_error(
				"Cannot get IRQ for interrupt %d, vector %d; "
				"%d\n",
				i, interrupt_data->msix_entries[i].vector, ret);
			return ret;
		}

		interrupt_data->num_configured++;
	}

	return 0;
}

static void gasket_interrupt_msix_cleanup(
	struct gasket_interrupt_data *interrupt_data)
{
	int i;

	for (i = 0; i < interrupt_data->num_configured; i++)
		free_irq(interrupt_data->msix_entries[i].vector,
			 interrupt_data);
	interrupt_data->num_configured = 0;

	if (interrupt_data->msix_configured)
		pci_disable_msix(interrupt_data->pci_dev);
	interrupt_data->msix_configured = 0;
}

/*
 * On QCM DragonBoard, we exit gasket_interrupt_msix_init() and kernel interrupt
 * setup code with MSIX vectors masked. This is wrong because nothing else in
 * the driver will normally touch the MSIX vectors.
 *
 * As a temporary hack, force unmasking there.
 *
 * TODO: Figure out why QCM kernel doesn't unmask the MSIX vectors, after
 * gasket_interrupt_msix_init(), and remove this code.
 */
static void force_msix_interrupt_unmasking(struct gasket_dev *gasket_dev)
{
	int i;
#define MSIX_VECTOR_SIZE 16
#define MSIX_MASK_BIT_OFFSET 12
#define APEX_BAR2_REG_KERNEL_HIB_MSIX_TABLE 0x46800
	for (i = 0; i < gasket_dev->interrupt_data->num_configured; i++) {
		/* Check if the MSIX vector is unmasked */
		ulong location = APEX_BAR2_REG_KERNEL_HIB_MSIX_TABLE +
				 MSIX_MASK_BIT_OFFSET + i * MSIX_VECTOR_SIZE;
		u32 mask =
			gasket_dev_read_32(
				gasket_dev,
				gasket_dev->interrupt_data->interrupt_bar_index,
				location);
		if (!(mask & 1))
			continue;
		/* Unmask the msix vector (clear 32 bits) */
		gasket_dev_write_32(
			gasket_dev, 0,
			gasket_dev->interrupt_data->interrupt_bar_index,
			location);
	}
#undef MSIX_VECTOR_SIZE
#undef MSIX_MASK_BIT_OFFSET
#undef APEX_BAR2_REG_KERNEL_HIB_MSIX_TABLE
}

int gasket_interrupt_reinit(struct gasket_dev *gasket_dev)
{
	int ret;

	if (!gasket_dev->interrupt_data) {
		gasket_log_error(
			gasket_dev,
			"Attempted to reinit uninitialized interrupt data.");
		return -EINVAL;
	}

	switch (gasket_dev->interrupt_data->type) {
	case PCI_MSIX:
		gasket_interrupt_msix_cleanup(gasket_dev->interrupt_data);
		ret = gasket_interrupt_msix_init(gasket_dev->interrupt_data);
		if (ret)
			break;
		force_msix_interrupt_unmasking(gasket_dev);
		break;

	case PCI_MSI:
	case PLATFORM_WIRE:
	default:
		gasket_nodev_error(
			"Cannot handle unsupported interrupt type %d.",
			gasket_dev->interrupt_data->type);
		ret = -EINVAL;
	};

	if (ret) {
		/* Failing to setup MSIx will cause the device
		 * to report GASKET_STATUS_LAMED, but is not fatal.
		 */
		gasket_log_warn(gasket_dev, "Couldn't init msix: %d", ret);
		return 0;
	}

	gasket_interrupt_setup(gasket_dev);

	return 0;
}

/* See gasket_interrupt.h for description. */
int gasket_interrupt_reset_counts(struct gasket_dev *gasket_dev)
{
	gasket_log_debug(gasket_dev, "Clearing interrupt counts.");
	memset(gasket_dev->interrupt_data->interrupt_counts, 0,
	       gasket_dev->interrupt_data->num_interrupts *
			sizeof(*gasket_dev->interrupt_data->interrupt_counts));
	return 0;
}

/*
 * Set up device registers for interrupt handling.
 * @gasket_dev: The Gasket information structure for this device.
 *
 * Sets up the device registers with the correct indices for the relevant
 * interrupts.
 */
static void gasket_interrupt_setup(struct gasket_dev *gasket_dev)
{
	int i;
	int pack_shift;
	ulong mask;
	ulong value;
	struct gasket_interrupt_data *interrupt_data =
		gasket_dev->interrupt_data;

	if (!interrupt_data) {
		gasket_log_error(
			gasket_dev, "Interrupt data is not initialized.");
		return;
	}

	gasket_log_debug(gasket_dev, "Running interrupt setup.");

	if (interrupt_data->type == PLATFORM_WIRE ||
	    interrupt_data->type == PCI_MSI) {
		/* Nothing needs to be done for platform or PCI devices. */
		return;
	}

	if (interrupt_data->type != PCI_MSIX) {
		gasket_nodev_error(
			"Cannot handle unsupported interrupt type %d.",
			interrupt_data->type);
		return;
	}

	/* Setup the MSIX table. */

	for (i = 0; i < interrupt_data->num_interrupts; i++) {
		/*
		 * If the interrupt is not packed, we can write the index into
		 * the register directly. If not, we need to deal with a read-
		 * modify-write and shift based on the packing index.
		 */
		gasket_log_debug(
			gasket_dev,
			"Setting up interrupt index %d with index 0x%llx and "
			"packing %d",
			interrupt_data->interrupts[i].index,
			interrupt_data->interrupts[i].reg,
			interrupt_data->interrupts[i].packing);
		if (interrupt_data->interrupts[i].packing == UNPACKED) {
			value = interrupt_data->interrupts[i].index;
		} else {
			switch (interrupt_data->interrupts[i].packing) {
			case PACK_0:
				pack_shift = 0;
				break;
			case PACK_1:
				pack_shift = interrupt_data->pack_width;
				break;
			case PACK_2:
				pack_shift = 2 * interrupt_data->pack_width;
				break;
			case PACK_3:
				pack_shift = 3 * interrupt_data->pack_width;
				break;
			default:
				gasket_nodev_error(
					"Found interrupt description with "
					"unknown enum %d.",
					interrupt_data->interrupts[i].packing);
				return;
			}

			mask = ~(0xFFFF << pack_shift);
			value = gasket_dev_read_64(
					gasket_dev,
					interrupt_data->interrupt_bar_index,
					interrupt_data->interrupts[i].reg) &
				mask;
			value |= interrupt_data->interrupts[i].index
				 << pack_shift;
		}
		gasket_dev_write_64(gasket_dev, value,
				    interrupt_data->interrupt_bar_index,
				    interrupt_data->interrupts[i].reg);
	}
}

/* See gasket_interrupt.h for description. */
void gasket_interrupt_cleanup(struct gasket_dev *gasket_dev)
{
	struct gasket_interrupt_data *interrupt_data =
		gasket_dev->interrupt_data;
	/*
	 * It is possible to get an error code from gasket_interrupt_init
	 * before interrupt_data has been allocated, so check it.
	 */
	if (!interrupt_data)
		return;

	switch (interrupt_data->type) {
	case PCI_MSIX:
		gasket_interrupt_msix_cleanup(interrupt_data);
		break;

	case PCI_MSI:
	case PLATFORM_WIRE:
	default:
		gasket_nodev_error(
			"Cannot handle unsupported interrupt type %d.",
			interrupt_data->type);
	};

	kfree(interrupt_data->interrupt_counts);
	kfree(interrupt_data->eventfd_ctxs);
	kfree(interrupt_data->msix_entries);
	kfree(interrupt_data);
	gasket_dev->interrupt_data = NULL;
}

int gasket_interrupt_system_status(struct gasket_dev *gasket_dev)
{
	if (!gasket_dev->interrupt_data) {
		gasket_nodev_info("Interrupt data is null.");
		return GASKET_STATUS_DEAD;
	}

	if (!gasket_dev->interrupt_data->msix_configured) {
		gasket_nodev_info("Interrupt not initialized.");
		return GASKET_STATUS_LAMED;
	}

	if (gasket_dev->interrupt_data->num_configured !=
		gasket_dev->interrupt_data->num_interrupts) {
		gasket_nodev_info("Not all interrupts were configured.");
		return GASKET_STATUS_LAMED;
	}

	return GASKET_STATUS_ALIVE;
}

int gasket_interrupt_set_eventfd(
	struct gasket_interrupt_data *interrupt_data, int interrupt,
	int event_fd)
{
	struct eventfd_ctx *ctx = eventfd_ctx_fdget(event_fd);

	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	if (interrupt < 0 || interrupt >= interrupt_data->num_interrupts)
		return -EINVAL;

	interrupt_data->eventfd_ctxs[interrupt] = ctx;
	return 0;
}

int gasket_interrupt_clear_eventfd(
	struct gasket_interrupt_data *interrupt_data, int interrupt)
{
	if (interrupt < 0 || interrupt >= interrupt_data->num_interrupts)
		return -EINVAL;

	interrupt_data->eventfd_ctxs[interrupt] = NULL;
	return 0;
}

static ssize_t interrupt_sysfs_show(
	struct device *device, struct device_attribute *attr, char *buf)
{
	int i, ret;
	ssize_t written = 0, total_written = 0;
	struct gasket_interrupt_data *interrupt_data;
	struct gasket_dev *gasket_dev;
	struct gasket_sysfs_attribute *gasket_attr;
	enum interrupt_sysfs_attribute_type sysfs_type;

	gasket_dev = gasket_sysfs_get_device_data(device);
	if (!gasket_dev) {
		gasket_nodev_error(
			"No sysfs mapping found for device 0x%p", device);
		return 0;
	}

	gasket_attr = gasket_sysfs_get_attr(device, attr);
	if (!gasket_attr) {
		gasket_nodev_error(
			"No sysfs attr data found for device 0x%p", device);
		gasket_sysfs_put_device_data(device, gasket_dev);
		return 0;
	}

	sysfs_type = (enum interrupt_sysfs_attribute_type)
		gasket_attr->data.attr_type;
	interrupt_data = gasket_dev->interrupt_data;
	switch (sysfs_type) {
	case ATTR_INTERRUPT_COUNTS:
		for (i = 0; i < interrupt_data->num_interrupts; ++i) {
			written =
				scnprintf(buf, PAGE_SIZE - total_written,
					  "0x%02x: %ld\n", i,
					  interrupt_data->interrupt_counts[i]);
			total_written += written;
			buf += written;
		}
		ret = total_written;
		break;
	default:
		gasket_log_error(
			gasket_dev, "Unknown attribute: %s", attr->attr.name);
		ret = 0;
		break;
	}

	gasket_sysfs_put_attr(device, gasket_attr);
	gasket_sysfs_put_device_data(device, gasket_dev);
	return ret;
}

/*
 * MSIX interrupt handler, used with PCI driver.
 */
static irqreturn_t gasket_msix_interrupt_handler(int irq, void *dev_id)
{
	struct eventfd_ctx *ctx;
	struct gasket_interrupt_data *interrupt_data = dev_id;
	int interrupt = -1;
	int i;

	/* If this linear lookup is a problem, we can maintain a map/hash. */
	for (i = 0; i < interrupt_data->num_interrupts; i++) {
		if (interrupt_data->msix_entries[i].vector == irq) {
			interrupt = interrupt_data->msix_entries[i].entry;
			break;
		}
	}
	if (interrupt == -1) {
		gasket_nodev_error("Received unknown irq %d", irq);
		return IRQ_HANDLED;
	}
	trace_gasket_interrupt_event(interrupt_data->name, interrupt);

	ctx = interrupt_data->eventfd_ctxs[interrupt];
	if (ctx)
		eventfd_signal(ctx, 1);

	++(interrupt_data->interrupt_counts[interrupt]);

	return IRQ_HANDLED;
}
