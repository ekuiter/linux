/*
 * in-kernel handling for sie intercepts
 *
 * Copyright IBM Corp. 2008, 2009
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (version 2 only)
 * as published by the Free Software Foundation.
 *
 *    Author(s): Carsten Otte <cotte@de.ibm.com>
 *               Christian Borntraeger <borntraeger@de.ibm.com>
 */

#include <linux/kvm_host.h>
#include <linux/errno.h>
#include <linux/pagemap.h>

#include <asm/kvm_host.h>

#include "kvm-s390.h"
#include "gaccess.h"
#include "trace.h"
#include "trace-s390.h"


static const intercept_handler_t instruction_handlers[256] = {
	[0x01] = kvm_s390_handle_01,
	[0x82] = kvm_s390_handle_lpsw,
	[0x83] = kvm_s390_handle_diag,
	[0xae] = kvm_s390_handle_sigp,
	[0xb2] = kvm_s390_handle_b2,
	[0xb6] = kvm_s390_handle_stctl,
	[0xb7] = kvm_s390_handle_lctl,
	[0xb9] = kvm_s390_handle_b9,
	[0xe5] = kvm_s390_handle_e5,
	[0xeb] = kvm_s390_handle_eb,
};

static int handle_noop(struct kvm_vcpu *vcpu)
{
	switch (vcpu->arch.sie_block->icptcode) {
	case 0x0:
		vcpu->stat.exit_null++;
		break;
	case 0x10:
		vcpu->stat.exit_external_request++;
		break;
	case 0x14:
		vcpu->stat.exit_external_interrupt++;
		break;
	default:
		break; /* nothing */
	}
	return 0;
}

static int handle_stop(struct kvm_vcpu *vcpu)
{
	int rc = 0;

	vcpu->stat.exit_stop_request++;
	spin_lock_bh(&vcpu->arch.local_int.lock);

	trace_kvm_s390_stop_request(vcpu->arch.local_int.action_bits);

	if (vcpu->arch.local_int.action_bits & ACTION_STOP_ON_STOP) {
		atomic_set_mask(CPUSTAT_STOPPED,
				&vcpu->arch.sie_block->cpuflags);
		vcpu->arch.local_int.action_bits &= ~ACTION_STOP_ON_STOP;
		VCPU_EVENT(vcpu, 3, "%s", "cpu stopped");
		rc = -EOPNOTSUPP;
	}

	if (vcpu->arch.local_int.action_bits & ACTION_STORE_ON_STOP) {
		vcpu->arch.local_int.action_bits &= ~ACTION_STORE_ON_STOP;
		/* store status must be called unlocked. Since local_int.lock
		 * only protects local_int.* and not guest memory we can give
		 * up the lock here */
		spin_unlock_bh(&vcpu->arch.local_int.lock);
		rc = kvm_s390_vcpu_store_status(vcpu,
						KVM_S390_STORE_STATUS_NOADDR);
		if (rc >= 0)
			rc = -EOPNOTSUPP;
	} else
		spin_unlock_bh(&vcpu->arch.local_int.lock);
	return rc;
}

static int handle_validity(struct kvm_vcpu *vcpu)
{
	int viwhy = vcpu->arch.sie_block->ipb >> 16;

	vcpu->stat.exit_validity++;
	trace_kvm_s390_intercept_validity(vcpu, viwhy);
	WARN_ONCE(true, "kvm: unhandled validity intercept 0x%x\n", viwhy);
	return -EOPNOTSUPP;
}

static int handle_instruction(struct kvm_vcpu *vcpu)
{
	intercept_handler_t handler;

	vcpu->stat.exit_instruction++;
	trace_kvm_s390_intercept_instruction(vcpu,
					     vcpu->arch.sie_block->ipa,
					     vcpu->arch.sie_block->ipb);
	handler = instruction_handlers[vcpu->arch.sie_block->ipa >> 8];
	if (handler)
		return handler(vcpu);
	return -EOPNOTSUPP;
}

static void __extract_prog_irq(struct kvm_vcpu *vcpu,
			       struct kvm_s390_pgm_info *pgm_info)
{
	memset(pgm_info, 0, sizeof(struct kvm_s390_pgm_info));
	pgm_info->code = vcpu->arch.sie_block->iprcc;

	switch (vcpu->arch.sie_block->iprcc & ~PGM_PER) {
	case PGM_AFX_TRANSLATION:
	case PGM_ASX_TRANSLATION:
	case PGM_EX_TRANSLATION:
	case PGM_LFX_TRANSLATION:
	case PGM_LSTE_SEQUENCE:
	case PGM_LSX_TRANSLATION:
	case PGM_LX_TRANSLATION:
	case PGM_PRIMARY_AUTHORITY:
	case PGM_SECONDARY_AUTHORITY:
	case PGM_SPACE_SWITCH:
		pgm_info->trans_exc_code = vcpu->arch.sie_block->tecmc;
		break;
	case PGM_ALEN_TRANSLATION:
	case PGM_ALE_SEQUENCE:
	case PGM_ASTE_INSTANCE:
	case PGM_ASTE_SEQUENCE:
	case PGM_ASTE_VALIDITY:
	case PGM_EXTENDED_AUTHORITY:
		pgm_info->exc_access_id = vcpu->arch.sie_block->eai;
		break;
	case PGM_ASCE_TYPE:
	case PGM_PAGE_TRANSLATION:
	case PGM_REGION_FIRST_TRANS:
	case PGM_REGION_SECOND_TRANS:
	case PGM_REGION_THIRD_TRANS:
	case PGM_SEGMENT_TRANSLATION:
		pgm_info->trans_exc_code = vcpu->arch.sie_block->tecmc;
		pgm_info->exc_access_id  = vcpu->arch.sie_block->eai;
		pgm_info->op_access_id  = vcpu->arch.sie_block->oai;
		break;
	case PGM_MONITOR:
		pgm_info->mon_class_nr = vcpu->arch.sie_block->mcn;
		pgm_info->mon_code = vcpu->arch.sie_block->tecmc;
		break;
	case PGM_DATA:
		pgm_info->data_exc_code = vcpu->arch.sie_block->dxc;
		break;
	case PGM_PROTECTION:
		pgm_info->trans_exc_code = vcpu->arch.sie_block->tecmc;
		pgm_info->exc_access_id  = vcpu->arch.sie_block->eai;
		break;
	default:
		break;
	}

	if (vcpu->arch.sie_block->iprcc & PGM_PER) {
		pgm_info->per_code = vcpu->arch.sie_block->perc;
		pgm_info->per_atmid = vcpu->arch.sie_block->peratmid;
		pgm_info->per_address = vcpu->arch.sie_block->peraddr;
		pgm_info->per_access_id = vcpu->arch.sie_block->peraid;
	}
}

#define per_event(vcpu) (vcpu->arch.sie_block->iprcc & PGM_PER)

static int handle_prog(struct kvm_vcpu *vcpu)
{
	struct kvm_s390_pgm_info pgm_info;
	struct kvm_s390_itdb *itdb;
	int rc;

	vcpu->stat.exit_program_interruption++;

	if (guestdbg_enabled(vcpu) && per_event(vcpu)) {
		kvm_s390_handle_per_event(vcpu);
		/* the interrupt might have been filtered out completely */
		if (vcpu->arch.sie_block->iprcc == 0)
			return 0;
	}

	/* Restore ITDB to Program-Interruption TDB in guest memory */
	if (!IS_TE_ENABLED(vcpu) || !IS_ITDB_VALID(vcpu))
		goto skip_itdb;
	if (current->thread.per_flags & PER_FLAG_NO_TE)
		goto skip_itdb;
	itdb = (struct kvm_s390_itdb *)vcpu->arch.sie_block->itdba;
	rc = write_guest_lc(vcpu, TDB_ADDR, itdb, sizeof(*itdb));
	if (rc)
		return rc;
	memset(itdb, 0, sizeof(*itdb));
skip_itdb:
	trace_kvm_s390_intercept_prog(vcpu, vcpu->arch.sie_block->iprcc);
	__extract_prog_irq(vcpu, &pgm_info);

	return kvm_s390_inject_prog_irq(vcpu, &pgm_info);
}

static int handle_instruction_and_prog(struct kvm_vcpu *vcpu)
{
	int rc, rc2;

	vcpu->stat.exit_instr_and_program++;
	rc = handle_instruction(vcpu);
	rc2 = handle_prog(vcpu);

	if (rc == -EOPNOTSUPP)
		vcpu->arch.sie_block->icptcode = 0x04;
	if (rc)
		return rc;
	return rc2;
}

static const intercept_handler_t intercept_funcs[] = {
	[0x00 >> 2] = handle_noop,
	[0x04 >> 2] = handle_instruction,
	[0x08 >> 2] = handle_prog,
	[0x0C >> 2] = handle_instruction_and_prog,
	[0x10 >> 2] = handle_noop,
	[0x14 >> 2] = handle_noop,
	[0x18 >> 2] = handle_noop,
	[0x1C >> 2] = kvm_s390_handle_wait,
	[0x20 >> 2] = handle_validity,
	[0x28 >> 2] = handle_stop,
};

int kvm_handle_sie_intercept(struct kvm_vcpu *vcpu)
{
	intercept_handler_t func;
	u8 code = vcpu->arch.sie_block->icptcode;

	if (code & 3 || (code >> 2) >= ARRAY_SIZE(intercept_funcs))
		return -EOPNOTSUPP;
	func = intercept_funcs[code >> 2];
	if (func)
		return func(vcpu);
	return -EOPNOTSUPP;
}
