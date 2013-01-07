/*
 *
 * Intel Management Engine Interface (Intel MEI) Linux driver
 * Copyright (c) 2003-2012, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <linux/pci.h>
#include <linux/mei.h>

#include "mei_dev.h"
#include "interface.h"

/**
 * mei_reg_read - Reads 32bit data from the mei device
 *
 * @dev: the device structure
 * @offset: offset from which to read the data
 *
 * returns register value (u32)
 */
static inline u32 mei_reg_read(const struct mei_device *dev,
			       unsigned long offset)
{
	return ioread32(dev->mem_addr + offset);
}


/**
 * mei_reg_write - Writes 32bit data to the mei device
 *
 * @dev: the device structure
 * @offset: offset from which to write the data
 * @value: register value to write (u32)
 */
static inline void mei_reg_write(const struct mei_device *dev,
				 unsigned long offset, u32 value)
{
	iowrite32(value, dev->mem_addr + offset);
}

/**
 * mei_hcsr_read - Reads 32bit data from the host CSR
 *
 * @dev: the device structure
 *
 * returns the byte read.
 */
u32 mei_hcsr_read(const struct mei_device *dev)
{
	return mei_reg_read(dev, H_CSR);
}

u32 mei_mecbrw_read(const struct mei_device *dev)
{
	return mei_reg_read(dev, ME_CB_RW);
}
/**
 * mei_mecsr_read - Reads 32bit data from the ME CSR
 *
 * @dev: the device structure
 *
 * returns ME_CSR_HA register value (u32)
 */
u32 mei_mecsr_read(const struct mei_device *dev)
{
	return mei_reg_read(dev, ME_CSR_HA);
}

/**
 * mei_set_csr_register - writes H_CSR register to the mei device,
 * and ignores the H_IS bit for it is write-one-to-zero.
 *
 * @dev: the device structure
 */
void mei_hcsr_set(struct mei_device *dev)
{
	if ((dev->host_hw_state & H_IS) == H_IS)
		dev->host_hw_state &= ~H_IS;
	mei_reg_write(dev, H_CSR, dev->host_hw_state);
	dev->host_hw_state = mei_hcsr_read(dev);
}

/**
 * mei_enable_interrupts - clear and stop interrupts
 *
 * @dev: the device structure
 */
void mei_clear_interrupts(struct mei_device *dev)
{
	if ((dev->host_hw_state & H_IS) == H_IS)
		mei_reg_write(dev, H_CSR, dev->host_hw_state);
}

/**
 * mei_enable_interrupts - enables mei device interrupts
 *
 * @dev: the device structure
 */
void mei_enable_interrupts(struct mei_device *dev)
{
	dev->host_hw_state |= H_IE;
	mei_hcsr_set(dev);
}

/**
 * mei_disable_interrupts - disables mei device interrupts
 *
 * @dev: the device structure
 */
void mei_disable_interrupts(struct mei_device *dev)
{
	dev->host_hw_state &= ~H_IE;
	mei_hcsr_set(dev);
}


/**
 * mei_interrupt_quick_handler - The ISR of the MEI device
 *
 * @irq: The irq number
 * @dev_id: pointer to the device structure
 *
 * returns irqreturn_t
 */
irqreturn_t mei_interrupt_quick_handler(int irq, void *dev_id)
{
	struct mei_device *dev = (struct mei_device *) dev_id;
	u32 csr_reg = mei_hcsr_read(dev);

	if ((csr_reg & H_IS) != H_IS)
		return IRQ_NONE;

	/* clear H_IS bit in H_CSR */
	mei_reg_write(dev, H_CSR, csr_reg);

	return IRQ_WAKE_THREAD;
}

/**
 * mei_hbuf_filled_slots - gets number of device filled buffer slots
 *
 * @device: the device structure
 *
 * returns number of filled slots
 */
static unsigned char mei_hbuf_filled_slots(struct mei_device *dev)
{
	char read_ptr, write_ptr;

	dev->host_hw_state = mei_hcsr_read(dev);

	read_ptr = (char) ((dev->host_hw_state & H_CBRP) >> 8);
	write_ptr = (char) ((dev->host_hw_state & H_CBWP) >> 16);

	return (unsigned char) (write_ptr - read_ptr);
}

/**
 * mei_hbuf_is_empty - checks if host buffer is empty.
 *
 * @dev: the device structure
 *
 * returns true if empty, false - otherwise.
 */
bool mei_hbuf_is_empty(struct mei_device *dev)
{
	return mei_hbuf_filled_slots(dev) == 0;
}

/**
 * mei_hbuf_empty_slots - counts write empty slots.
 *
 * @dev: the device structure
 *
 * returns -1(ESLOTS_OVERFLOW) if overflow, otherwise empty slots count
 */
int mei_hbuf_empty_slots(struct mei_device *dev)
{
	unsigned char filled_slots, empty_slots;

	filled_slots = mei_hbuf_filled_slots(dev);
	empty_slots = dev->hbuf_depth - filled_slots;

	/* check for overflow */
	if (filled_slots > dev->hbuf_depth)
		return -EOVERFLOW;

	return empty_slots;
}

/**
 * mei_write_message - writes a message to mei device.
 *
 * @dev: the device structure
 * @hader: mei HECI header of message
 * @buf: message payload will be written
 *
 * This function returns -EIO if write has failed
 */
int mei_write_message(struct mei_device *dev, struct mei_msg_hdr *header,
		      unsigned char *buf)
{
	unsigned long rem, dw_cnt;
	unsigned long length = header->length;
	u32 *reg_buf = (u32 *)buf;
	int i;
	int empty_slots;

	dev_dbg(&dev->pdev->dev, MEI_HDR_FMT, MEI_HDR_PRM(header));

	empty_slots = mei_hbuf_empty_slots(dev);
	dev_dbg(&dev->pdev->dev, "empty slots = %hu.\n", empty_slots);

	dw_cnt = mei_data2slots(length);
	if (empty_slots < 0 || dw_cnt > empty_slots)
		return -EIO;

	mei_reg_write(dev, H_CB_WW, *((u32 *) header));

	for (i = 0; i < length / 4; i++)
		mei_reg_write(dev, H_CB_WW, reg_buf[i]);

	rem = length & 0x3;
	if (rem > 0) {
		u32 reg = 0;
		memcpy(&reg, &buf[length - rem], rem);
		mei_reg_write(dev, H_CB_WW, reg);
	}

	dev->host_hw_state = mei_hcsr_read(dev);
	dev->host_hw_state |= H_IG;
	mei_hcsr_set(dev);
	dev->me_hw_state = mei_mecsr_read(dev);
	if ((dev->me_hw_state & ME_RDY_HRA) != ME_RDY_HRA)
		return -EIO;

	return 0;
}

/**
 * mei_count_full_read_slots - counts read full slots.
 *
 * @dev: the device structure
 *
 * returns -1(ESLOTS_OVERFLOW) if overflow, otherwise filled slots count
 */
int mei_count_full_read_slots(struct mei_device *dev)
{
	char read_ptr, write_ptr;
	unsigned char buffer_depth, filled_slots;

	dev->me_hw_state = mei_mecsr_read(dev);
	buffer_depth = (unsigned char)((dev->me_hw_state & ME_CBD_HRA) >> 24);
	read_ptr = (char) ((dev->me_hw_state & ME_CBRP_HRA) >> 8);
	write_ptr = (char) ((dev->me_hw_state & ME_CBWP_HRA) >> 16);
	filled_slots = (unsigned char) (write_ptr - read_ptr);

	/* check for overflow */
	if (filled_slots > buffer_depth)
		return -EOVERFLOW;

	dev_dbg(&dev->pdev->dev, "filled_slots =%08x\n", filled_slots);
	return (int)filled_slots;
}

/**
 * mei_read_slots - reads a message from mei device.
 *
 * @dev: the device structure
 * @buffer: message buffer will be written
 * @buffer_length: message size will be read
 */
void mei_read_slots(struct mei_device *dev, unsigned char *buffer,
		    unsigned long buffer_length)
{
	u32 *reg_buf = (u32 *)buffer;

	for (; buffer_length >= sizeof(u32); buffer_length -= sizeof(u32))
		*reg_buf++ = mei_mecbrw_read(dev);

	if (buffer_length > 0) {
		u32 reg = mei_mecbrw_read(dev);
		memcpy(reg_buf, &reg, buffer_length);
	}

	dev->host_hw_state |= H_IG;
	mei_hcsr_set(dev);
}

/**
 * mei_flow_ctrl_creds - checks flow_control credentials.
 *
 * @dev: the device structure
 * @cl: private data of the file object
 *
 * returns 1 if mei_flow_ctrl_creds >0, 0 - otherwise.
 *	-ENOENT if mei_cl is not present
 *	-EINVAL if single_recv_buf == 0
 */
int mei_flow_ctrl_creds(struct mei_device *dev, struct mei_cl *cl)
{
	int i;

	if (!dev->me_clients_num)
		return 0;

	if (cl->mei_flow_ctrl_creds > 0)
		return 1;

	for (i = 0; i < dev->me_clients_num; i++) {
		struct mei_me_client  *me_cl = &dev->me_clients[i];
		if (me_cl->client_id == cl->me_client_id) {
			if (me_cl->mei_flow_ctrl_creds) {
				if (WARN_ON(me_cl->props.single_recv_buf == 0))
					return -EINVAL;
				return 1;
			} else {
				return 0;
			}
		}
	}
	return -ENOENT;
}

/**
 * mei_flow_ctrl_reduce - reduces flow_control.
 *
 * @dev: the device structure
 * @cl: private data of the file object
 * @returns
 *	0 on success
 *	-ENOENT when me client is not found
 *	-EINVAL when ctrl credits are <= 0
 */
int mei_flow_ctrl_reduce(struct mei_device *dev, struct mei_cl *cl)
{
	int i;

	if (!dev->me_clients_num)
		return -ENOENT;

	for (i = 0; i < dev->me_clients_num; i++) {
		struct mei_me_client  *me_cl = &dev->me_clients[i];
		if (me_cl->client_id == cl->me_client_id) {
			if (me_cl->props.single_recv_buf != 0) {
				if (WARN_ON(me_cl->mei_flow_ctrl_creds <= 0))
					return -EINVAL;
				dev->me_clients[i].mei_flow_ctrl_creds--;
			} else {
				if (WARN_ON(cl->mei_flow_ctrl_creds <= 0))
					return -EINVAL;
				cl->mei_flow_ctrl_creds--;
			}
			return 0;
		}
	}
	return -ENOENT;
}


/**
 * mei_other_client_is_connecting - checks if other
 *    client with the same client id is connected.
 *
 * @dev: the device structure
 * @cl: private data of the file object
 *
 * returns 1 if other client is connected, 0 - otherwise.
 */
int mei_other_client_is_connecting(struct mei_device *dev,
				struct mei_cl *cl)
{
	struct mei_cl *cl_pos = NULL;
	struct mei_cl *cl_next = NULL;

	list_for_each_entry_safe(cl_pos, cl_next, &dev->file_list, link) {
		if ((cl_pos->state == MEI_FILE_CONNECTING) &&
			(cl_pos != cl) &&
			cl->me_client_id == cl_pos->me_client_id)
			return 1;

	}
	return 0;
}

