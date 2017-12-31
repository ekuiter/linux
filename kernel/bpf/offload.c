/*
 * Copyright (C) 2017 Netronome Systems, Inc.
 *
 * This software is licensed under the GNU General License Version 2,
 * June 1991 as shown in the file COPYING in the top-level directory of this
 * source tree.
 *
 * THE COPYRIGHT HOLDERS AND/OR OTHER PARTIES PROVIDE THE PROGRAM "AS IS"
 * WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE. THE ENTIRE RISK AS TO THE QUALITY AND PERFORMANCE
 * OF THE PROGRAM IS WITH YOU. SHOULD THE PROGRAM PROVE DEFECTIVE, YOU ASSUME
 * THE COST OF ALL NECESSARY SERVICING, REPAIR OR CORRECTION.
 */

#include <linux/bpf.h>
#include <linux/bpf_verifier.h>
#include <linux/bug.h>
#include <linux/list.h>
#include <linux/netdevice.h>
#include <linux/printk.h>
#include <linux/rtnetlink.h>
#include <linux/rwsem.h>

/* Protects bpf_prog_offload_devs and offload members of all progs.
 * RTNL lock cannot be taken when holding this lock.
 */
static DECLARE_RWSEM(bpf_devs_lock);
static LIST_HEAD(bpf_prog_offload_devs);

int bpf_prog_offload_init(struct bpf_prog *prog, union bpf_attr *attr)
{
	struct bpf_dev_offload *offload;

	if (attr->prog_type != BPF_PROG_TYPE_SCHED_CLS &&
	    attr->prog_type != BPF_PROG_TYPE_XDP)
		return -EINVAL;

	if (attr->prog_flags)
		return -EINVAL;

	offload = kzalloc(sizeof(*offload), GFP_USER);
	if (!offload)
		return -ENOMEM;

	offload->prog = prog;

	offload->netdev = dev_get_by_index(current->nsproxy->net_ns,
					   attr->prog_ifindex);
	if (!offload->netdev)
		goto err_free;

	down_write(&bpf_devs_lock);
	if (offload->netdev->reg_state != NETREG_REGISTERED)
		goto err_unlock;
	prog->aux->offload = offload;
	list_add_tail(&offload->offloads, &bpf_prog_offload_devs);
	dev_put(offload->netdev);
	up_write(&bpf_devs_lock);

	return 0;
err_unlock:
	up_write(&bpf_devs_lock);
	dev_put(offload->netdev);
err_free:
	kfree(offload);
	return -EINVAL;
}

static int __bpf_offload_ndo(struct bpf_prog *prog, enum bpf_netdev_command cmd,
			     struct netdev_bpf *data)
{
	struct net_device *netdev = prog->aux->offload->netdev;

	ASSERT_RTNL();

	if (!netdev)
		return -ENODEV;
	if (!netdev->netdev_ops->ndo_bpf)
		return -EOPNOTSUPP;

	data->command = cmd;

	return netdev->netdev_ops->ndo_bpf(netdev, data);
}

int bpf_prog_offload_verifier_prep(struct bpf_verifier_env *env)
{
	struct netdev_bpf data = {};
	int err;

	data.verifier.prog = env->prog;

	rtnl_lock();
	err = __bpf_offload_ndo(env->prog, BPF_OFFLOAD_VERIFIER_PREP, &data);
	if (err)
		goto exit_unlock;

	env->prog->aux->offload->dev_ops = data.verifier.ops;
	env->prog->aux->offload->dev_state = true;
exit_unlock:
	rtnl_unlock();
	return err;
}

int bpf_prog_offload_verify_insn(struct bpf_verifier_env *env,
				 int insn_idx, int prev_insn_idx)
{
	struct bpf_dev_offload *offload;
	int ret = -ENODEV;

	down_read(&bpf_devs_lock);
	offload = env->prog->aux->offload;
	if (offload->netdev)
		ret = offload->dev_ops->insn_hook(env, insn_idx, prev_insn_idx);
	up_read(&bpf_devs_lock);

	return ret;
}

static void __bpf_prog_offload_destroy(struct bpf_prog *prog)
{
	struct bpf_dev_offload *offload = prog->aux->offload;
	struct netdev_bpf data = {};

	/* Caution - if netdev is destroyed before the program, this function
	 * will be called twice.
	 */

	data.offload.prog = prog;

	if (offload->dev_state)
		WARN_ON(__bpf_offload_ndo(prog, BPF_OFFLOAD_DESTROY, &data));

	offload->dev_state = false;
	list_del_init(&offload->offloads);
	offload->netdev = NULL;
}

void bpf_prog_offload_destroy(struct bpf_prog *prog)
{
	struct bpf_dev_offload *offload = prog->aux->offload;

	rtnl_lock();
	down_write(&bpf_devs_lock);
	__bpf_prog_offload_destroy(prog);
	up_write(&bpf_devs_lock);
	rtnl_unlock();

	kfree(offload);
}

static int bpf_prog_offload_translate(struct bpf_prog *prog)
{
	struct netdev_bpf data = {};
	int ret;

	data.offload.prog = prog;

	rtnl_lock();
	ret = __bpf_offload_ndo(prog, BPF_OFFLOAD_TRANSLATE, &data);
	rtnl_unlock();

	return ret;
}

static unsigned int bpf_prog_warn_on_exec(const void *ctx,
					  const struct bpf_insn *insn)
{
	WARN(1, "attempt to execute device eBPF program on the host!");
	return 0;
}

int bpf_prog_offload_compile(struct bpf_prog *prog)
{
	prog->bpf_func = bpf_prog_warn_on_exec;

	return bpf_prog_offload_translate(prog);
}

const struct bpf_prog_ops bpf_offload_prog_ops = {
};

static int bpf_offload_notification(struct notifier_block *notifier,
				    ulong event, void *ptr)
{
	struct net_device *netdev = netdev_notifier_info_to_dev(ptr);
	struct bpf_dev_offload *offload, *tmp;

	ASSERT_RTNL();

	switch (event) {
	case NETDEV_UNREGISTER:
		/* ignore namespace changes */
		if (netdev->reg_state != NETREG_UNREGISTERING)
			break;

		down_write(&bpf_devs_lock);
		list_for_each_entry_safe(offload, tmp, &bpf_prog_offload_devs,
					 offloads) {
			if (offload->netdev == netdev)
				__bpf_prog_offload_destroy(offload->prog);
		}
		up_write(&bpf_devs_lock);
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block bpf_offload_notifier = {
	.notifier_call = bpf_offload_notification,
};

static int __init bpf_offload_init(void)
{
	register_netdevice_notifier(&bpf_offload_notifier);
	return 0;
}

subsys_initcall(bpf_offload_init);
