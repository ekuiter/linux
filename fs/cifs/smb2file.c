/*
 *   fs/cifs/smb2file.c
 *
 *   Copyright (C) International Business Machines  Corp., 2002, 2011
 *   Author(s): Steve French (sfrench@us.ibm.com),
 *              Pavel Shilovsky ((pshilovsky@samba.org) 2012
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published
 *   by the Free Software Foundation; either version 2.1 of the License, or
 *   (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 *   the GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <asm/div64.h>
#include "cifsfs.h"
#include "cifspdu.h"
#include "cifsglob.h"
#include "cifsproto.h"
#include "cifs_debug.h"
#include "cifs_fs_sb.h"
#include "cifs_unicode.h"
#include "fscache.h"
#include "smb2proto.h"

void
smb2_set_oplock_level(struct cifsInodeInfo *cinode, __u32 oplock)
{
	oplock &= 0xFF;
	if (oplock == SMB2_OPLOCK_LEVEL_EXCLUSIVE) {
		cinode->clientCanCacheAll = true;
		cinode->clientCanCacheRead = true;
		cFYI(1, "Exclusive Oplock granted on inode %p",
		     &cinode->vfs_inode);
	} else if (oplock == SMB2_OPLOCK_LEVEL_II) {
		cinode->clientCanCacheAll = false;
		cinode->clientCanCacheRead = true;
		cFYI(1, "Level II Oplock granted on inode %p",
		    &cinode->vfs_inode);
	} else {
		cinode->clientCanCacheAll = false;
		cinode->clientCanCacheRead = false;
	}
}

int
smb2_open_file(const unsigned int xid, struct cifs_tcon *tcon, const char *path,
	       int disposition, int desired_access, int create_options,
	       struct cifs_fid *fid, __u32 *oplock, FILE_ALL_INFO *buf,
	       struct cifs_sb_info *cifs_sb)
{
	int rc;
	__le16 *smb2_path;
	struct smb2_file_all_info *smb2_data = NULL;

	smb2_path = cifs_convert_path_to_utf16(path, cifs_sb);
	if (smb2_path == NULL) {
		rc = -ENOMEM;
		goto out;
	}

	smb2_data = kzalloc(sizeof(struct smb2_file_all_info) + MAX_NAME * 2,
			    GFP_KERNEL);
	if (smb2_data == NULL) {
		rc = -ENOMEM;
		goto out;
	}

	desired_access |= FILE_READ_ATTRIBUTES;
	*oplock = SMB2_OPLOCK_LEVEL_EXCLUSIVE;

	rc = SMB2_open(xid, tcon, smb2_path, &fid->persistent_fid,
		       &fid->volatile_fid, desired_access, disposition,
		       0, 0, (__u8 *)oplock, smb2_data);
	if (rc)
		goto out;

	if (buf) {
		/* open response does not have IndexNumber field - get it */
		rc = SMB2_get_srv_num(xid, tcon, fid->persistent_fid,
				      fid->volatile_fid,
				      &smb2_data->IndexNumber);
		if (rc) {
			/* let get_inode_info disable server inode numbers */
			smb2_data->IndexNumber = 0;
			rc = 0;
		}
		move_smb2_info_to_cifs(buf, smb2_data);
	}

out:
	kfree(smb2_data);
	kfree(smb2_path);
	return rc;
}

int
smb2_unlock_range(struct cifsFileInfo *cfile, struct file_lock *flock,
		  const unsigned int xid)
{
	int rc = 0, stored_rc;
	unsigned int max_num, num = 0, max_buf;
	struct smb2_lock_element *buf, *cur;
	struct cifs_tcon *tcon = tlink_tcon(cfile->tlink);
	struct cifsInodeInfo *cinode = CIFS_I(cfile->dentry->d_inode);
	struct cifsLockInfo *li, *tmp;
	__u64 length = 1 + flock->fl_end - flock->fl_start;
	struct list_head tmp_llist;

	INIT_LIST_HEAD(&tmp_llist);

	/*
	 * Accessing maxBuf is racy with cifs_reconnect - need to store value
	 * and check it for zero before using.
	 */
	max_buf = tcon->ses->server->maxBuf;
	if (!max_buf)
		return -EINVAL;

	max_num = max_buf / sizeof(struct smb2_lock_element);
	buf = kzalloc(max_num * sizeof(struct smb2_lock_element), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	cur = buf;

	mutex_lock(&cinode->lock_mutex);
	list_for_each_entry_safe(li, tmp, &cfile->llist->locks, llist) {
		if (flock->fl_start > li->offset ||
		    (flock->fl_start + length) <
		    (li->offset + li->length))
			continue;
		if (current->tgid != li->pid)
			continue;
		if (cinode->can_cache_brlcks) {
			/*
			 * We can cache brlock requests - simply remove a lock
			 * from the file's list.
			 */
			list_del(&li->llist);
			cifs_del_lock_waiters(li);
			kfree(li);
			continue;
		}
		cur->Length = cpu_to_le64(li->length);
		cur->Offset = cpu_to_le64(li->offset);
		cur->Flags = cpu_to_le32(SMB2_LOCKFLAG_UNLOCK);
		/*
		 * We need to save a lock here to let us add it again to the
		 * file's list if the unlock range request fails on the server.
		 */
		list_move(&li->llist, &tmp_llist);
		if (++num == max_num) {
			stored_rc = smb2_lockv(xid, tcon,
					       cfile->fid.persistent_fid,
					       cfile->fid.volatile_fid,
					       current->tgid, num, buf);
			if (stored_rc) {
				/*
				 * We failed on the unlock range request - add
				 * all locks from the tmp list to the head of
				 * the file's list.
				 */
				cifs_move_llist(&tmp_llist,
						&cfile->llist->locks);
				rc = stored_rc;
			} else
				/*
				 * The unlock range request succeed - free the
				 * tmp list.
				 */
				cifs_free_llist(&tmp_llist);
			cur = buf;
			num = 0;
		} else
			cur++;
	}
	if (num) {
		stored_rc = smb2_lockv(xid, tcon, cfile->fid.persistent_fid,
				       cfile->fid.volatile_fid, current->tgid,
				       num, buf);
		if (stored_rc) {
			cifs_move_llist(&tmp_llist, &cfile->llist->locks);
			rc = stored_rc;
		} else
			cifs_free_llist(&tmp_llist);
	}
	mutex_unlock(&cinode->lock_mutex);

	kfree(buf);
	return rc;
}
