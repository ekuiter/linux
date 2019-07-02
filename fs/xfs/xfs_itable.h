// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2001 Silicon Graphics, Inc.  All Rights Reserved.
 */
#ifndef __XFS_ITABLE_H__
#define	__XFS_ITABLE_H__

/* In-memory representation of a userspace request for batch inode data. */
struct xfs_ibulk {
	struct xfs_mount	*mp;
	void __user		*ubuffer; /* user output buffer */
	xfs_ino_t		startino; /* start with this inode */
	unsigned int		icount;   /* number of elements in ubuffer */
	unsigned int		ocount;   /* number of records returned */
};

/* Return value that means we want to abort the walk. */
#define XFS_IBULK_ABORT		(XFS_IWALK_ABORT)

/*
 * Advance the user buffer pointer by one record of the given size.  If the
 * buffer is now full, return the appropriate error code.
 */
static inline int
xfs_ibulk_advance(
	struct xfs_ibulk	*breq,
	size_t			bytes)
{
	char __user		*b = breq->ubuffer;

	breq->ubuffer = b + bytes;
	breq->ocount++;
	return breq->ocount == breq->icount ? XFS_IBULK_ABORT : 0;
}

/*
 * Return stat information in bulk (by-inode) for the filesystem.
 */

typedef int (*bulkstat_one_fmt_pf)(struct xfs_ibulk *breq,
		const struct xfs_bstat *bstat);

int xfs_bulkstat_one(struct xfs_ibulk *breq, bulkstat_one_fmt_pf formatter);
int xfs_bulkstat(struct xfs_ibulk *breq, bulkstat_one_fmt_pf formatter);

typedef int (*inumbers_fmt_pf)(
	void			__user *ubuffer, /* buffer to write to */
	const xfs_inogrp_t	*buffer,	/* buffer to read from */
	long			count,		/* # of elements to read */
	long			*written);	/* # of bytes written */

int
xfs_inumbers_fmt(
	void			__user *ubuffer, /* buffer to write to */
	const xfs_inogrp_t	*buffer,	/* buffer to read from */
	long			count,		/* # of elements to read */
	long			*written);	/* # of bytes written */

int					/* error status */
xfs_inumbers(
	xfs_mount_t		*mp,	/* mount point for filesystem */
	xfs_ino_t		*last,	/* last inode returned */
	int			*count,	/* size of buffer/count returned */
	void			__user *buffer, /* buffer with inode info */
	inumbers_fmt_pf		formatter);

/* Temporarily needed while we refactor functions. */
struct xfs_btree_cur;
struct xfs_inobt_rec_incore;
void xfs_bulkstat_ichunk_ra(struct xfs_mount *mp, xfs_agnumber_t agno,
		struct xfs_inobt_rec_incore *irec);
int xfs_bulkstat_grab_ichunk(struct xfs_btree_cur *cur, xfs_agino_t agino,
		int *icount, struct xfs_inobt_rec_incore *irec);

#endif	/* __XFS_ITABLE_H__ */
