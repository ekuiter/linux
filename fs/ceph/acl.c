/*
 * linux/fs/ceph/acl.c
 *
 * Copyright (C) 2013 Guangliang Zhao, <lucienchao@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <linux/ceph/ceph_debug.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/xattr.h>
#include <linux/posix_acl_xattr.h>
#include <linux/posix_acl.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include "super.h"

static inline void ceph_set_cached_acl(struct inode *inode,
					int type, struct posix_acl *acl)
{
	struct ceph_inode_info *ci = ceph_inode(inode);

	spin_lock(&ci->i_ceph_lock);
	if (__ceph_caps_issued_mask(ci, CEPH_CAP_XATTR_SHARED, 0))
		set_cached_acl(inode, type, acl);
	spin_unlock(&ci->i_ceph_lock);
}

static inline struct posix_acl *ceph_get_cached_acl(struct inode *inode,
							int type)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	struct posix_acl *acl = ACL_NOT_CACHED;

	spin_lock(&ci->i_ceph_lock);
	if (__ceph_caps_issued_mask(ci, CEPH_CAP_XATTR_SHARED, 0))
		acl = get_cached_acl(inode, type);
	spin_unlock(&ci->i_ceph_lock);

	return acl;
}

void ceph_forget_all_cached_acls(struct inode *inode)
{
	forget_all_cached_acls(inode);
}

struct posix_acl *ceph_get_acl(struct inode *inode, int type)
{
	int size;
	const char *name;
	char *value = NULL;
	struct posix_acl *acl;

	if (!IS_POSIXACL(inode))
		return NULL;

	acl = ceph_get_cached_acl(inode, type);
	if (acl != ACL_NOT_CACHED)
		return acl;

	switch (type) {
	case ACL_TYPE_ACCESS:
		name = POSIX_ACL_XATTR_ACCESS;
		break;
	case ACL_TYPE_DEFAULT:
		name = POSIX_ACL_XATTR_DEFAULT;
		break;
	default:
		BUG();
	}

	size = __ceph_getxattr(inode, name, "", 0);
	if (size > 0) {
		value = kzalloc(size, GFP_NOFS);
		if (!value)
			return ERR_PTR(-ENOMEM);
		size = __ceph_getxattr(inode, name, value, size);
	}

	if (size > 0)
		acl = posix_acl_from_xattr(&init_user_ns, value, size);
	else if (size == -ERANGE || size == -ENODATA || size == 0)
		acl = NULL;
	else
		acl = ERR_PTR(-EIO);

	kfree(value);

	if (!IS_ERR(acl))
		ceph_set_cached_acl(inode, type, acl);

	return acl;
}

static int ceph_set_acl(struct dentry *dentry, struct inode *inode,
				struct posix_acl *acl, int type)
{
	int ret = 0, size = 0;
	const char *name = NULL;
	char *value = NULL;
	struct iattr newattrs;
	umode_t new_mode = inode->i_mode, old_mode = inode->i_mode;

	if (acl) {
		ret = posix_acl_valid(acl);
		if (ret < 0)
			goto out;
	}

	switch (type) {
	case ACL_TYPE_ACCESS:
		name = POSIX_ACL_XATTR_ACCESS;
		if (acl) {
			ret = posix_acl_equiv_mode(acl, &new_mode);
			if (ret < 0)
				goto out;
			if (ret == 0)
				acl = NULL;
		}
		break;
	case ACL_TYPE_DEFAULT:
		if (!S_ISDIR(inode->i_mode)) {
			ret = acl ? -EINVAL : 0;
			goto out;
		}
		name = POSIX_ACL_XATTR_DEFAULT;
		break;
	default:
		ret = -EINVAL;
		goto out;
	}

	if (acl) {
		size = posix_acl_xattr_size(acl->a_count);
		value = kmalloc(size, GFP_NOFS);
		if (!value) {
			ret = -ENOMEM;
			goto out;
		}

		ret = posix_acl_to_xattr(&init_user_ns, acl, value, size);
		if (ret < 0)
			goto out_free;
	}

	if (new_mode != old_mode) {
		newattrs.ia_mode = new_mode;
		newattrs.ia_valid = ATTR_MODE;
		ret = ceph_setattr(dentry, &newattrs);
		if (ret)
			goto out_free;
	}

	if (value)
		ret = __ceph_setxattr(dentry, name, value, size, 0);
	else
		ret = __ceph_removexattr(dentry, name);

	if (ret) {
		if (new_mode != old_mode) {
			newattrs.ia_mode = old_mode;
			newattrs.ia_valid = ATTR_MODE;
			ceph_setattr(dentry, &newattrs);
		}
		goto out_free;
	}

	ceph_set_cached_acl(inode, type, acl);

out_free:
	kfree(value);
out:
	return ret;
}

int ceph_init_acl(struct dentry *dentry, struct inode *inode, struct inode *dir)
{
	struct posix_acl *acl = NULL;
	int ret = 0;

	if (!S_ISLNK(inode->i_mode)) {
		if (IS_POSIXACL(dir)) {
			acl = ceph_get_acl(dir, ACL_TYPE_DEFAULT);
			if (IS_ERR(acl)) {
				ret = PTR_ERR(acl);
				goto out;
			}
		}

		if (!acl)
			inode->i_mode &= ~current_umask();
	}

	if (IS_POSIXACL(dir) && acl) {
		if (S_ISDIR(inode->i_mode)) {
			ret = ceph_set_acl(dentry, inode, acl,
						ACL_TYPE_DEFAULT);
			if (ret)
				goto out_release;
		}
		ret = posix_acl_create(&acl, GFP_NOFS, &inode->i_mode);
		if (ret < 0)
			goto out;
		else if (ret > 0)
			ret = ceph_set_acl(dentry, inode, acl, ACL_TYPE_ACCESS);
		else
			cache_no_acl(inode);
	} else {
		cache_no_acl(inode);
	}

out_release:
	posix_acl_release(acl);
out:
	return ret;
}

int ceph_acl_chmod(struct dentry *dentry, struct inode *inode)
{
	struct posix_acl *acl;
	int ret = 0;

	if (S_ISLNK(inode->i_mode)) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	if (!IS_POSIXACL(inode))
		goto out;

	acl = ceph_get_acl(inode, ACL_TYPE_ACCESS);
	if (IS_ERR_OR_NULL(acl)) {
		ret = PTR_ERR(acl);
		goto out;
	}

	ret = posix_acl_chmod(&acl, GFP_KERNEL, inode->i_mode);
	if (ret)
		goto out;
	ret = ceph_set_acl(dentry, inode, acl, ACL_TYPE_ACCESS);
	posix_acl_release(acl);
out:
	return ret;
}

static int ceph_xattr_acl_get(struct dentry *dentry, const char *name,
				void *value, size_t size, int type)
{
	struct posix_acl *acl;
	int ret = 0;

	if (!IS_POSIXACL(dentry->d_inode))
		return -EOPNOTSUPP;

	acl = ceph_get_acl(dentry->d_inode, type);
	if (IS_ERR(acl))
		return PTR_ERR(acl);
	if (acl == NULL)
		return -ENODATA;

	ret = posix_acl_to_xattr(&init_user_ns, acl, value, size);
	posix_acl_release(acl);

	return ret;
}

static int ceph_xattr_acl_set(struct dentry *dentry, const char *name,
			const void *value, size_t size, int flags, int type)
{
	int ret = 0;
	struct posix_acl *acl = NULL;

	if (!inode_owner_or_capable(dentry->d_inode)) {
		ret = -EPERM;
		goto out;
	}

	if (!IS_POSIXACL(dentry->d_inode)) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	if (value) {
		acl = posix_acl_from_xattr(&init_user_ns, value, size);
		if (IS_ERR(acl)) {
			ret = PTR_ERR(acl);
			goto out;
		}

		if (acl) {
			ret = posix_acl_valid(acl);
			if (ret)
				goto out_release;
		}
	}

	ret = ceph_set_acl(dentry, dentry->d_inode, acl, type);

out_release:
	posix_acl_release(acl);
out:
	return ret;
}

const struct xattr_handler ceph_xattr_acl_default_handler = {
	.prefix = POSIX_ACL_XATTR_DEFAULT,
	.flags  = ACL_TYPE_DEFAULT,
	.get    = ceph_xattr_acl_get,
	.set    = ceph_xattr_acl_set,
};

const struct xattr_handler ceph_xattr_acl_access_handler = {
	.prefix = POSIX_ACL_XATTR_ACCESS,
	.flags  = ACL_TYPE_ACCESS,
	.get    = ceph_xattr_acl_get,
	.set    = ceph_xattr_acl_set,
};
