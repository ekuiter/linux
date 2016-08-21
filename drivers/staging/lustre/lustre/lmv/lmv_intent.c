/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2004, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2015, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#define DEBUG_SUBSYSTEM S_LMV
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <asm/div64.h>
#include <linux/seq_file.h>
#include <linux/namei.h>
#include "../include/lustre_intent.h"
#include "../include/obd_support.h"
#include "../include/lustre/lustre_idl.h"
#include "../include/lustre_lib.h"
#include "../include/lustre_net.h"
#include "../include/lustre_dlm.h"
#include "../include/lustre_mdc.h"
#include "../include/obd_class.h"
#include "../include/lprocfs_status.h"
#include "lmv_internal.h"

static int lmv_intent_remote(struct obd_export *exp, void *lmm,
			     int lmmsize, struct lookup_intent *it,
			     const struct lu_fid *parent_fid, int flags,
			     struct ptlrpc_request **reqp,
			     ldlm_blocking_callback cb_blocking,
			     __u64 extra_lock_flags)
{
	struct obd_device	*obd = exp->exp_obd;
	struct lmv_obd		*lmv = &obd->u.lmv;
	struct ptlrpc_request	*req = NULL;
	struct lustre_handle	plock;
	struct md_op_data	*op_data;
	struct lmv_tgt_desc	*tgt;
	struct mdt_body		*body;
	int			pmode;
	int			rc = 0;

	body = req_capsule_server_get(&(*reqp)->rq_pill, &RMF_MDT_BODY);
	if (!body)
		return -EPROTO;

	LASSERT((body->valid & OBD_MD_MDS));

	/*
	 * Unfortunately, we have to lie to MDC/MDS to retrieve
	 * attributes llite needs and provideproper locking.
	 */
	if (it->it_op & IT_LOOKUP)
		it->it_op = IT_GETATTR;

	/*
	 * We got LOOKUP lock, but we really need attrs.
	 */
	pmode = it->it_lock_mode;
	if (pmode) {
		plock.cookie = it->it_lock_handle;
		it->it_lock_mode = 0;
		it->it_request = NULL;
	}

	LASSERT(fid_is_sane(&body->fid1));

	tgt = lmv_find_target(lmv, &body->fid1);
	if (IS_ERR(tgt)) {
		rc = PTR_ERR(tgt);
		goto out;
	}

	op_data = kzalloc(sizeof(*op_data), GFP_NOFS);
	if (!op_data) {
		rc = -ENOMEM;
		goto out;
	}

	op_data->op_fid1 = body->fid1;
	/* Sent the parent FID to the remote MDT */
	if (parent_fid) {
		/* The parent fid is only for remote open to
		 * check whether the open is from OBF,
		 * see mdt_cross_open
		 */
		LASSERT(it->it_op & IT_OPEN);
		op_data->op_fid2 = *parent_fid;
		/* Add object FID to op_fid3, in case it needs to check stale
		 * (M_CHECK_STALE), see mdc_finish_intent_lock
		 */
		op_data->op_fid3 = body->fid1;
	}

	op_data->op_bias = MDS_CROSS_REF;
	CDEBUG(D_INODE, "REMOTE_INTENT with fid="DFID" -> mds #%d\n",
	       PFID(&body->fid1), tgt->ltd_idx);

	rc = md_intent_lock(tgt->ltd_exp, op_data, lmm, lmmsize, it,
			    flags, &req, cb_blocking, extra_lock_flags);
	if (rc)
		goto out_free_op_data;

	/*
	 * LLite needs LOOKUP lock to track dentry revocation in order to
	 * maintain dcache consistency. Thus drop UPDATE|PERM lock here
	 * and put LOOKUP in request.
	 */
	if (it->it_lock_mode != 0) {
		it->it_remote_lock_handle =
					it->it_lock_handle;
		it->it_remote_lock_mode = it->it_lock_mode;
	}

	if (pmode) {
		it->it_lock_handle = plock.cookie;
		it->it_lock_mode = pmode;
	}

out_free_op_data:
	kfree(op_data);
out:
	if (rc && pmode)
		ldlm_lock_decref(&plock, pmode);

	ptlrpc_req_finished(*reqp);
	*reqp = req;
	return rc;
}

int lmv_revalidate_slaves(struct obd_export *exp, struct mdt_body *mbody,
			  struct lmv_stripe_md *lsm,
			  ldlm_blocking_callback cb_blocking,
			  int extra_lock_flags)
{
	struct obd_device *obd = exp->exp_obd;
	struct lmv_obd *lmv = &obd->u.lmv;
	struct mdt_body *body;
	struct md_op_data *op_data;
	unsigned long size = 0;
	unsigned long nlink = 0;
	__s64 atime = 0;
	__s64 ctime = 0;
	__s64 mtime = 0;
	int rc = 0, i;

	/**
	 * revalidate slaves has some problems, temporarily return,
	 * we may not need that
	 */
	if (lsm->lsm_md_stripe_count <= 1)
		return 0;

	op_data = kzalloc(sizeof(*op_data), GFP_NOFS);
	if (!op_data)
		return -ENOMEM;

	/**
	 * Loop over the stripe information, check validity and update them
	 * from MDS if needed.
	 */
	for (i = 0; i < lsm->lsm_md_stripe_count; i++) {
		struct lookup_intent it = { .it_op = IT_GETATTR };
		struct ptlrpc_request *req = NULL;
		struct lustre_handle *lockh = NULL;
		struct lmv_tgt_desc *tgt = NULL;
		struct inode *inode;
		struct lu_fid fid;

		fid = lsm->lsm_md_oinfo[i].lmo_fid;
		inode = lsm->lsm_md_oinfo[i].lmo_root;
		if (!i) {
			if (mbody) {
				body = mbody;
				goto update;
			} else {
				goto release_lock;
			}
		}

		/*
		 * Prepare op_data for revalidating. Note that @fid2 shluld be
		 * defined otherwise it will go to server and take new lock
		 * which is not needed here.
		 */
		memset(op_data, 0, sizeof(*op_data));
		op_data->op_fid1 = fid;
		op_data->op_fid2 = fid;

		tgt = lmv_locate_mds(lmv, op_data, &fid);
		if (IS_ERR(tgt)) {
			rc = PTR_ERR(tgt);
			goto cleanup;
		}

		CDEBUG(D_INODE, "Revalidate slave "DFID" -> mds #%d\n",
		       PFID(&fid), tgt->ltd_idx);

		rc = md_intent_lock(tgt->ltd_exp, op_data, NULL, 0, &it, 0,
				    &req, cb_blocking, extra_lock_flags);
		if (rc < 0)
			goto cleanup;

		lockh = (struct lustre_handle *)&it.it_lock_handle;
		if (rc > 0 && !req) {
			/* slave inode is still valid */
			CDEBUG(D_INODE, "slave "DFID" is still valid.\n",
			       PFID(&fid));
			rc = 0;
		} else {
			/* refresh slave from server */
			body = req_capsule_server_get(&req->rq_pill,
						      &RMF_MDT_BODY);
			LASSERT(body);
update:
			if (unlikely(body->nlink < 2)) {
				CERROR("%s: nlink %d < 2 corrupt stripe %d "DFID":" DFID"\n",
				       obd->obd_name, body->nlink, i,
				       PFID(&lsm->lsm_md_oinfo[i].lmo_fid),
				       PFID(&lsm->lsm_md_oinfo[0].lmo_fid));

				if (req)
					ptlrpc_req_finished(req);

				if (it.it_lock_mode && lockh) {
					ldlm_lock_decref(lockh, it.it_lock_mode);
					it.it_lock_mode = 0;
				}

				rc = -EIO;
				goto cleanup;
			}

			if (i)
				md_set_lock_data(tgt->ltd_exp, &lockh->cookie,
						 inode, NULL);

			i_size_write(inode, body->size);
			set_nlink(inode, body->nlink);
			LTIME_S(inode->i_atime) = body->atime;
			LTIME_S(inode->i_ctime) = body->ctime;
			LTIME_S(inode->i_mtime) = body->mtime;

			if (req)
				ptlrpc_req_finished(req);
		}
release_lock:
		size += i_size_read(inode);

		if (i != 0)
			nlink += inode->i_nlink - 2;
		else
			nlink += inode->i_nlink;

		atime = LTIME_S(inode->i_atime) > atime ?
				LTIME_S(inode->i_atime) : atime;
		ctime = LTIME_S(inode->i_ctime) > ctime ?
				LTIME_S(inode->i_ctime) : ctime;
		mtime = LTIME_S(inode->i_mtime) > mtime ?
				LTIME_S(inode->i_mtime) : mtime;

		if (it.it_lock_mode && lockh) {
			ldlm_lock_decref(lockh, it.it_lock_mode);
			it.it_lock_mode = 0;
		}

		CDEBUG(D_INODE, "i %d "DFID" size %llu, nlink %u, atime %lu, mtime %lu, ctime %lu.\n",
		       i, PFID(&fid), i_size_read(inode), inode->i_nlink,
		       LTIME_S(inode->i_atime), LTIME_S(inode->i_mtime),
		       LTIME_S(inode->i_ctime));
	}

	/*
	 * update attr of master request.
	 */
	CDEBUG(D_INODE, "Return refreshed attrs: size = %lu nlink %lu atime %llu ctime %llu mtime %llu for " DFID"\n",
	       size, nlink, atime, ctime, mtime,
	       PFID(&lsm->lsm_md_oinfo[0].lmo_fid));

	if (mbody) {
		mbody->atime = atime;
		mbody->ctime = ctime;
		mbody->mtime = mtime;
	}
cleanup:
	kfree(op_data);
	return rc;
}

/*
 * IT_OPEN is intended to open (and create, possible) an object. Parent (pid)
 * may be split dir.
 */
static int lmv_intent_open(struct obd_export *exp, struct md_op_data *op_data,
			   void *lmm, int lmmsize, struct lookup_intent *it,
			   int flags, struct ptlrpc_request **reqp,
			   ldlm_blocking_callback cb_blocking,
			   __u64 extra_lock_flags)
{
	struct obd_device	*obd = exp->exp_obd;
	struct lmv_obd		*lmv = &obd->u.lmv;
	struct lmv_tgt_desc	*tgt;
	struct mdt_body		*body;
	int			rc;

	if (it->it_flags & MDS_OPEN_BY_FID && fid_is_sane(&op_data->op_fid2)) {
		if (op_data->op_mea1) {
			struct lmv_stripe_md *lsm = op_data->op_mea1;
			const struct lmv_oinfo *oinfo;

			oinfo = lsm_name_to_stripe_info(lsm, op_data->op_name,
							op_data->op_namelen);
			if (IS_ERR(oinfo))
				return PTR_ERR(oinfo);
			op_data->op_fid1 = oinfo->lmo_fid;
		}

		tgt = lmv_find_target(lmv, &op_data->op_fid2);
		if (IS_ERR(tgt))
			return PTR_ERR(tgt);

		op_data->op_mds = tgt->ltd_idx;
	} else {
		tgt = lmv_locate_mds(lmv, op_data, &op_data->op_fid1);
		if (IS_ERR(tgt))
			return PTR_ERR(tgt);
	}

	/* If it is ready to open the file by FID, do not need
	 * allocate FID at all, otherwise it will confuse MDT
	 */
	if ((it->it_op & IT_CREAT) &&
	    !(it->it_flags & MDS_OPEN_BY_FID)) {
		/*
		 * For open with IT_CREATE and for IT_CREATE cases allocate new
		 * fid and setup FLD for it.
		 */
		op_data->op_fid3 = op_data->op_fid2;
		rc = lmv_fid_alloc(exp, &op_data->op_fid2, op_data);
		if (rc != 0)
			return rc;
	}

	CDEBUG(D_INODE, "OPEN_INTENT with fid1=" DFID ", fid2=" DFID ", name='%s' -> mds #%d\n",
	       PFID(&op_data->op_fid1),
	       PFID(&op_data->op_fid2), op_data->op_name, tgt->ltd_idx);

	rc = md_intent_lock(tgt->ltd_exp, op_data, lmm, lmmsize, it, flags,
			    reqp, cb_blocking, extra_lock_flags);
	if (rc != 0)
		return rc;
	/*
	 * Nothing is found, do not access body->fid1 as it is zero and thus
	 * pointless.
	 */
	if ((it->it_disposition & DISP_LOOKUP_NEG) &&
	    !(it->it_disposition & DISP_OPEN_CREATE) &&
	    !(it->it_disposition & DISP_OPEN_OPEN))
		return rc;

	body = req_capsule_server_get(&(*reqp)->rq_pill, &RMF_MDT_BODY);
	if (!body)
		return -EPROTO;

	/* Not cross-ref case, just get out of here. */
	if (unlikely((body->valid & OBD_MD_MDS))) {
		rc = lmv_intent_remote(exp, lmm, lmmsize, it, &op_data->op_fid1,
				       flags, reqp, cb_blocking,
				       extra_lock_flags);
		if (rc != 0)
			return rc;

		body = req_capsule_server_get(&(*reqp)->rq_pill, &RMF_MDT_BODY);
		if (!body)
			return -EPROTO;
	}

	return rc;
}

/*
 * Handler for: getattr, lookup and revalidate cases.
 */
static int lmv_intent_lookup(struct obd_export *exp,
			     struct md_op_data *op_data,
			     void *lmm, int lmmsize, struct lookup_intent *it,
			     int flags, struct ptlrpc_request **reqp,
			     ldlm_blocking_callback cb_blocking,
			     __u64 extra_lock_flags)
{
	struct lmv_stripe_md *lsm = op_data->op_mea1;
	struct obd_device      *obd = exp->exp_obd;
	struct lmv_obd	 *lmv = &obd->u.lmv;
	struct lmv_tgt_desc    *tgt = NULL;
	struct mdt_body	*body;
	int		     rc = 0;

	tgt = lmv_locate_mds(lmv, op_data, &op_data->op_fid1);
	if (IS_ERR(tgt))
		return PTR_ERR(tgt);

	if (!fid_is_sane(&op_data->op_fid2))
		fid_zero(&op_data->op_fid2);

	CDEBUG(D_INODE, "LOOKUP_INTENT with fid1="DFID", fid2="DFID", name='%s' -> mds #%d lsm=%p lsm_magic=%x\n",
	       PFID(&op_data->op_fid1), PFID(&op_data->op_fid2),
	       op_data->op_name ? op_data->op_name : "<NULL>",
	       tgt->ltd_idx, lsm, !lsm ? -1 : lsm->lsm_md_magic);

	op_data->op_bias &= ~MDS_CROSS_REF;

	rc = md_intent_lock(tgt->ltd_exp, op_data, lmm, lmmsize, it,
			    flags, reqp, cb_blocking, extra_lock_flags);
	if (rc < 0)
		return rc;

	if (!*reqp) {
		/*
		 * If RPC happens, lsm information will be revalidated
		 * during update_inode process (see ll_update_lsm_md)
		 */
		if (op_data->op_mea2) {
			rc = lmv_revalidate_slaves(exp, NULL, op_data->op_mea2,
						   cb_blocking,
						   extra_lock_flags);
			if (rc != 0)
				return rc;
		}
		return rc;
	} else if (it_disposition(it, DISP_LOOKUP_NEG) && lsm &&
		   lsm->lsm_md_magic == LMV_MAGIC_MIGRATE) {
		/*
		 * For migrating directory, if it can not find the child in
		 * the source directory(master stripe), try the targeting
		 * directory(stripe 1)
		 */
		tgt = lmv_find_target(lmv, &lsm->lsm_md_oinfo[1].lmo_fid);
		if (IS_ERR(tgt))
			return PTR_ERR(tgt);

		ptlrpc_req_finished(*reqp);
		it->it_request = NULL;
		*reqp = NULL;

		CDEBUG(D_INODE, "For migrating dir, try target dir "DFID"\n",
		       PFID(&lsm->lsm_md_oinfo[1].lmo_fid));

		op_data->op_fid1 = lsm->lsm_md_oinfo[1].lmo_fid;
		it->it_disposition &= ~DISP_ENQ_COMPLETE;
		rc = md_intent_lock(tgt->ltd_exp, op_data, lmm, lmmsize, it,
				    flags, reqp, cb_blocking, extra_lock_flags);
	}

	/*
	 * MDS has returned success. Probably name has been resolved in
	 * remote inode. Let's check this.
	 */
	body = req_capsule_server_get(&(*reqp)->rq_pill, &RMF_MDT_BODY);
	if (!body)
		return -EPROTO;

	/* Not cross-ref case, just get out of here. */
	if (unlikely((body->valid & OBD_MD_MDS))) {
		rc = lmv_intent_remote(exp, lmm, lmmsize, it, NULL, flags,
				       reqp, cb_blocking, extra_lock_flags);
		if (rc != 0)
			return rc;
		body = req_capsule_server_get(&(*reqp)->rq_pill, &RMF_MDT_BODY);
		if (!body)
			return -EPROTO;
	}

	return rc;
}

int lmv_intent_lock(struct obd_export *exp, struct md_op_data *op_data,
		    void *lmm, int lmmsize, struct lookup_intent *it,
		    int flags, struct ptlrpc_request **reqp,
		    ldlm_blocking_callback cb_blocking,
		    __u64 extra_lock_flags)
{
	struct obd_device *obd = exp->exp_obd;
	int		rc;

	LASSERT(fid_is_sane(&op_data->op_fid1));

	CDEBUG(D_INODE, "INTENT LOCK '%s' for '%*s' on "DFID"\n",
	       LL_IT2STR(it), op_data->op_namelen, op_data->op_name,
	       PFID(&op_data->op_fid1));

	rc = lmv_check_connect(obd);
	if (rc)
		return rc;

	if (it->it_op & (IT_LOOKUP | IT_GETATTR | IT_LAYOUT))
		rc = lmv_intent_lookup(exp, op_data, lmm, lmmsize, it,
				       flags, reqp, cb_blocking,
				       extra_lock_flags);
	else if (it->it_op & IT_OPEN)
		rc = lmv_intent_open(exp, op_data, lmm, lmmsize, it,
				     flags, reqp, cb_blocking,
				     extra_lock_flags);
	else
		LBUG();
	return rc;
}
