/*
 * Copyright(c) 2016 - 2017 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#ifndef __DAX_H__
#define __DAX_H__
struct dax_device;
struct dax_operations;
struct dax_device *alloc_dax(void *private, const char *host,
		const struct dax_operations *ops);
void put_dax(struct dax_device *dax_dev);
bool dax_alive(struct dax_device *dax_dev);
void kill_dax(struct dax_device *dax_dev);
struct dax_device *inode_dax(struct inode *inode);
struct inode *dax_inode(struct dax_device *dax_dev);
void *dax_get_private(struct dax_device *dax_dev);
#endif /* __DAX_H__ */
