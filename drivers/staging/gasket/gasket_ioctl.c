// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2018 Google, Inc. */
#include "gasket.h"
#include "gasket_ioctl.h"
#include "gasket_constants.h"
#include "gasket_core.h"
#include "gasket_interrupt.h"
#include "gasket_logging.h"
#include "gasket_page_table.h"
#include <linux/fs.h>
#include <linux/uaccess.h>

#ifdef GASKET_KERNEL_TRACE_SUPPORT
#define CREATE_TRACE_POINTS
#include <trace/events/gasket_ioctl.h>
#else
#define trace_gasket_ioctl_entry(x, ...)
#define trace_gasket_ioctl_exit(x)
#define trace_gasket_ioctl_integer_data(x)
#define trace_gasket_ioctl_eventfd_data(x, ...)
#define trace_gasket_ioctl_page_table_data(x, ...)
#define trace_gasket_ioctl_config_coherent_allocator(x, ...)
#endif

static uint gasket_ioctl_check_permissions(struct file *filp, uint cmd);
static int gasket_set_event_fd(struct gasket_dev *dev, ulong arg);
static int gasket_read_page_table_size(
	struct gasket_dev *gasket_dev, ulong arg);
static int gasket_read_simple_page_table_size(
	struct gasket_dev *gasket_dev, ulong arg);
static int gasket_partition_page_table(
	struct gasket_dev *gasket_dev, ulong arg);
static int gasket_map_buffers(struct gasket_dev *gasket_dev, ulong arg);
static int gasket_unmap_buffers(struct gasket_dev *gasket_dev, ulong arg);
static int gasket_config_coherent_allocator(
	struct gasket_dev *gasket_dev, ulong arg);

/*
 * standard ioctl dispatch function.
 * @filp: File structure pointer describing this node usage session.
 * @cmd: ioctl number to handle.
 * @arg: ioctl-specific data pointer.
 *
 * Standard ioctl dispatcher; forwards operations to individual handlers.
 */
long gasket_handle_ioctl(struct file *filp, uint cmd, ulong arg)
{
	struct gasket_dev *gasket_dev;
	int retval;

	gasket_dev = (struct gasket_dev *)filp->private_data;
	trace_gasket_ioctl_entry(gasket_dev->dev_info.name, cmd);

	if (gasket_get_ioctl_permissions_cb(gasket_dev)) {
		retval = gasket_get_ioctl_permissions_cb(gasket_dev)(
			filp, cmd, arg);
		if (retval < 0) {
			trace_gasket_ioctl_exit(-EPERM);
			return retval;
		} else if (retval == 0) {
			trace_gasket_ioctl_exit(-EPERM);
			return -EPERM;
		}
	} else if (!gasket_ioctl_check_permissions(filp, cmd)) {
		trace_gasket_ioctl_exit(-EPERM);
		gasket_log_error(gasket_dev, "ioctl cmd=%x noperm.", cmd);
		return -EPERM;
	}

	/* Tracing happens in this switch statement for all ioctls with
	 * an integer argrument, but ioctls with a struct argument
	 * that needs copying and decoding, that tracing is done within
	 * the handler call.
	 */
	switch (cmd) {
	case GASKET_IOCTL_RESET:
		trace_gasket_ioctl_integer_data(arg);
		retval = gasket_reset(gasket_dev, arg);
		break;
	case GASKET_IOCTL_SET_EVENTFD:
		retval = gasket_set_event_fd(gasket_dev, arg);
		break;
	case GASKET_IOCTL_CLEAR_EVENTFD:
		trace_gasket_ioctl_integer_data(arg);
		retval = gasket_interrupt_clear_eventfd(
			gasket_dev->interrupt_data, (int)arg);
		break;
	case GASKET_IOCTL_PARTITION_PAGE_TABLE:
		trace_gasket_ioctl_integer_data(arg);
		retval = gasket_partition_page_table(gasket_dev, arg);
		break;
	case GASKET_IOCTL_NUMBER_PAGE_TABLES:
		trace_gasket_ioctl_integer_data(gasket_dev->num_page_tables);
		if (copy_to_user((void __user *)arg,
				 &gasket_dev->num_page_tables,
				 sizeof(uint64_t)))
			retval = -EFAULT;
		else
			retval = 0;
		break;
	case GASKET_IOCTL_PAGE_TABLE_SIZE:
		retval = gasket_read_page_table_size(gasket_dev, arg);
		break;
	case GASKET_IOCTL_SIMPLE_PAGE_TABLE_SIZE:
		retval = gasket_read_simple_page_table_size(gasket_dev, arg);
		break;
	case GASKET_IOCTL_MAP_BUFFER:
		retval = gasket_map_buffers(gasket_dev, arg);
		break;
	case GASKET_IOCTL_CONFIG_COHERENT_ALLOCATOR:
		retval = gasket_config_coherent_allocator(gasket_dev, arg);
		break;
	case GASKET_IOCTL_UNMAP_BUFFER:
		retval = gasket_unmap_buffers(gasket_dev, arg);
		break;
	case GASKET_IOCTL_CLEAR_INTERRUPT_COUNTS:
		/* Clear interrupt counts doesn't take an arg, so use 0. */
		trace_gasket_ioctl_integer_data(0);
		retval = gasket_interrupt_reset_counts(gasket_dev);
		break;
	default:
		/* If we don't understand the ioctl, the best we can do is trace
		 * the arg.
		 */
		trace_gasket_ioctl_integer_data(arg);
		gasket_log_warn(
			gasket_dev,
			"Unknown ioctl cmd=0x%x not caught by "
			"gasket_is_supported_ioctl",
			cmd);
		retval = -EINVAL;
		break;
	}

	trace_gasket_ioctl_exit(retval);
	return retval;
}

/*
 * Determines if an ioctl is part of the standard Gasket framework.
 * @cmd: The ioctl number to handle.
 *
 * Returns 1 if the ioctl is supported and 0 otherwise.
 */
long gasket_is_supported_ioctl(uint cmd)
{
	switch (cmd) {
	case GASKET_IOCTL_RESET:
	case GASKET_IOCTL_SET_EVENTFD:
	case GASKET_IOCTL_CLEAR_EVENTFD:
	case GASKET_IOCTL_PARTITION_PAGE_TABLE:
	case GASKET_IOCTL_NUMBER_PAGE_TABLES:
	case GASKET_IOCTL_PAGE_TABLE_SIZE:
	case GASKET_IOCTL_SIMPLE_PAGE_TABLE_SIZE:
	case GASKET_IOCTL_MAP_BUFFER:
	case GASKET_IOCTL_UNMAP_BUFFER:
	case GASKET_IOCTL_CLEAR_INTERRUPT_COUNTS:
	case GASKET_IOCTL_CONFIG_COHERENT_ALLOCATOR:
		return 1;
	default:
		return 0;
	}
}

/*
 * Permission checker for Gasket ioctls.
 * @filp: File structure pointer describing this node usage session.
 * @cmd: ioctl number to handle.
 *
 * Standard permissions checker.
 */
static uint gasket_ioctl_check_permissions(struct file *filp, uint cmd)
{
	uint alive, root, device_owner;
	fmode_t read, write;
	struct gasket_dev *gasket_dev = (struct gasket_dev *)filp->private_data;

	alive = (gasket_dev->status == GASKET_STATUS_ALIVE);
	if (!alive) {
		gasket_nodev_error(
			"gasket_ioctl_check_permissions alive %d status %d.",
			alive, gasket_dev->status);
	}

	root = capable(CAP_SYS_ADMIN);
	read = filp->f_mode & FMODE_READ;
	write = filp->f_mode & FMODE_WRITE;
	device_owner = (gasket_dev->dev_info.ownership.is_owned &&
			current->tgid == gasket_dev->dev_info.ownership.owner);

	switch (cmd) {
	case GASKET_IOCTL_RESET:
	case GASKET_IOCTL_CLEAR_INTERRUPT_COUNTS:
		return root || (write && device_owner);

	case GASKET_IOCTL_PAGE_TABLE_SIZE:
	case GASKET_IOCTL_SIMPLE_PAGE_TABLE_SIZE:
	case GASKET_IOCTL_NUMBER_PAGE_TABLES:
		return root || read;

	case GASKET_IOCTL_PARTITION_PAGE_TABLE:
	case GASKET_IOCTL_CONFIG_COHERENT_ALLOCATOR:
		return alive && (root || (write && device_owner));

	case GASKET_IOCTL_MAP_BUFFER:
	case GASKET_IOCTL_UNMAP_BUFFER:
		return alive && (root || (write && device_owner));

	case GASKET_IOCTL_CLEAR_EVENTFD:
	case GASKET_IOCTL_SET_EVENTFD:
		return alive && (root || (write && device_owner));
	}

	return 0; /* unknown permissions */
}

/*
 * Associate an eventfd with an interrupt.
 * @gasket_dev: Pointer to the current gasket_dev we're using.
 * @arg: Pointer to gasket_interrupt_eventfd struct in userspace.
 */
static int gasket_set_event_fd(struct gasket_dev *gasket_dev, ulong arg)
{
	struct gasket_interrupt_eventfd die;

	if (copy_from_user(&die, (void __user *)arg,
			   sizeof(struct gasket_interrupt_eventfd))) {
		return -EFAULT;
	}

	trace_gasket_ioctl_eventfd_data(die.interrupt, die.event_fd);

	return gasket_interrupt_set_eventfd(
		gasket_dev->interrupt_data, die.interrupt, die.event_fd);
}

/*
 * Reads the size of the page table.
 * @gasket_dev: Pointer to the current gasket_dev we're using.
 * @arg: Pointer to gasket_page_table_ioctl struct in userspace.
 */
static int gasket_read_page_table_size(struct gasket_dev *gasket_dev, ulong arg)
{
	int ret = 0;
	struct gasket_page_table_ioctl ibuf;

	if (copy_from_user(&ibuf, (void __user *)arg,
			   sizeof(struct gasket_page_table_ioctl)))
		return -EFAULT;

	if (ibuf.page_table_index >= gasket_dev->num_page_tables)
		return -EFAULT;

	ibuf.size = gasket_page_table_num_entries(
		gasket_dev->page_table[ibuf.page_table_index]);

	trace_gasket_ioctl_page_table_data(
		ibuf.page_table_index, ibuf.size, ibuf.host_address,
		ibuf.device_address);

	if (copy_to_user((void __user *)arg, &ibuf, sizeof(ibuf)))
		return -EFAULT;

	return ret;
}

/*
 * Reads the size of the simple page table.
 * @gasket_dev: Pointer to the current gasket_dev we're using.
 * @arg: Pointer to gasket_page_table_ioctl struct in userspace.
 */
static int gasket_read_simple_page_table_size(
	struct gasket_dev *gasket_dev, ulong arg)
{
	int ret = 0;
	struct gasket_page_table_ioctl ibuf;

	if (copy_from_user(&ibuf, (void __user *)arg,
			   sizeof(struct gasket_page_table_ioctl)))
		return -EFAULT;

	if (ibuf.page_table_index >= gasket_dev->num_page_tables)
		return -EFAULT;

	ibuf.size = gasket_page_table_num_simple_entries(
		gasket_dev->page_table[ibuf.page_table_index]);

	trace_gasket_ioctl_page_table_data(
		ibuf.page_table_index, ibuf.size, ibuf.host_address,
		ibuf.device_address);

	if (copy_to_user((void __user *)arg, &ibuf, sizeof(ibuf)))
		return -EFAULT;

	return ret;
}

/*
 * Sets the boundary between the simple and extended page tables.
 * @gasket_dev: Pointer to the current gasket_dev we're using.
 * @arg: Pointer to gasket_page_table_ioctl struct in userspace.
 */
static int gasket_partition_page_table(struct gasket_dev *gasket_dev, ulong arg)
{
	int ret;
	struct gasket_page_table_ioctl ibuf;
	uint max_page_table_size;

	if (copy_from_user(&ibuf, (void __user *)arg,
			   sizeof(struct gasket_page_table_ioctl)))
		return -EFAULT;

	trace_gasket_ioctl_page_table_data(
		ibuf.page_table_index, ibuf.size, ibuf.host_address,
		ibuf.device_address);

	if (ibuf.page_table_index >= gasket_dev->num_page_tables)
		return -EFAULT;
	max_page_table_size = gasket_page_table_max_size(
		gasket_dev->page_table[ibuf.page_table_index]);

	if (ibuf.size > max_page_table_size) {
		gasket_log_error(
			gasket_dev,
			"Partition request 0x%llx too large, max is 0x%x.",
			ibuf.size, max_page_table_size);
		return -EINVAL;
	}

	mutex_lock(&gasket_dev->mutex);

	ret = gasket_page_table_partition(
		gasket_dev->page_table[ibuf.page_table_index], ibuf.size);
	mutex_unlock(&gasket_dev->mutex);

	return ret;
}

/*
 * Maps a userspace buffer to a device virtual address.
 * @gasket_dev: Pointer to the current gasket_dev we're using.
 * @arg: Pointer to a gasket_page_table_ioctl struct in userspace.
 */
static int gasket_map_buffers(struct gasket_dev *gasket_dev, ulong arg)
{
	struct gasket_page_table_ioctl ibuf;

	if (copy_from_user(&ibuf, (void __user *)arg,
			   sizeof(struct gasket_page_table_ioctl)))
		return -EFAULT;

	trace_gasket_ioctl_page_table_data(
		ibuf.page_table_index, ibuf.size, ibuf.host_address,
		ibuf.device_address);

	if (ibuf.page_table_index >= gasket_dev->num_page_tables)
		return -EFAULT;

	if (gasket_page_table_are_addrs_bad(
		    gasket_dev->page_table[ibuf.page_table_index],
		    ibuf.host_address, ibuf.device_address, ibuf.size))
		return -EINVAL;

	return gasket_page_table_map(
		gasket_dev->page_table[ibuf.page_table_index],
		ibuf.host_address, ibuf.device_address, ibuf.size / PAGE_SIZE);
}

/*
 * Unmaps a userspace buffer from a device virtual address.
 * @gasket_dev: Pointer to the current gasket_dev we're using.
 * @arg: Pointer to a gasket_page_table_ioctl struct in userspace.
 */
static int gasket_unmap_buffers(struct gasket_dev *gasket_dev, ulong arg)
{
	struct gasket_page_table_ioctl ibuf;

	if (copy_from_user(&ibuf, (void __user *)arg,
			   sizeof(struct gasket_page_table_ioctl)))
		return -EFAULT;

	trace_gasket_ioctl_page_table_data(
		ibuf.page_table_index, ibuf.size, ibuf.host_address,
		ibuf.device_address);

	if (ibuf.page_table_index >= gasket_dev->num_page_tables)
		return -EFAULT;

	if (gasket_page_table_is_dev_addr_bad(
		    gasket_dev->page_table[ibuf.page_table_index],
		    ibuf.device_address, ibuf.size))
		return -EINVAL;

	gasket_page_table_unmap(gasket_dev->page_table[ibuf.page_table_index],
				ibuf.device_address, ibuf.size / PAGE_SIZE);

	return 0;
}

/*
 * Tell the driver to reserve structures for coherent allocation, and allocate
 * or free the corresponding memory.
 * @dev: Pointer to the current gasket_dev we're using.
 * @arg: Pointer to a gasket_coherent_alloc_config_ioctl struct in userspace.
 */
static int gasket_config_coherent_allocator(
	struct gasket_dev *gasket_dev, ulong arg)
{
	int ret;
	struct gasket_coherent_alloc_config_ioctl ibuf;

	if (copy_from_user(&ibuf, (void __user *)arg,
			   sizeof(struct gasket_coherent_alloc_config_ioctl)))
		return -EFAULT;

	trace_gasket_ioctl_config_coherent_allocator(
		ibuf.enable, ibuf.size, ibuf.dma_address);

	if (ibuf.page_table_index >= gasket_dev->num_page_tables)
		return -EFAULT;

	if (ibuf.size > PAGE_SIZE * MAX_NUM_COHERENT_PAGES) {
		ibuf.size = PAGE_SIZE * MAX_NUM_COHERENT_PAGES;
		return -ENOMEM;
	}

	if (ibuf.enable == 0) {
		ret = gasket_free_coherent_memory(
			gasket_dev, ibuf.size, ibuf.dma_address,
			ibuf.page_table_index);
	} else {
		ret = gasket_alloc_coherent_memory(
			gasket_dev, ibuf.size, &ibuf.dma_address,
			ibuf.page_table_index);
	}
	if (copy_to_user((void __user *)arg, &ibuf, sizeof(ibuf)))
		return -EFAULT;

	return ret;
}
