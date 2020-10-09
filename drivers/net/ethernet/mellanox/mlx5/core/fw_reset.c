// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2020, Mellanox Technologies inc.  All rights reserved. */

#include "fw_reset.h"

enum {
	MLX5_FW_RESET_FLAGS_RESET_REQUESTED,
};

struct mlx5_fw_reset {
	struct mlx5_core_dev *dev;
	struct mlx5_nb nb;
	struct workqueue_struct *wq;
	struct work_struct reset_request_work;
	struct work_struct reset_reload_work;
	unsigned long reset_flags;
	struct timer_list timer;
};

static int mlx5_reg_mfrl_set(struct mlx5_core_dev *dev, u8 reset_level,
			     u8 reset_type_sel, u8 sync_resp, bool sync_start)
{
	u32 out[MLX5_ST_SZ_DW(mfrl_reg)] = {};
	u32 in[MLX5_ST_SZ_DW(mfrl_reg)] = {};

	MLX5_SET(mfrl_reg, in, reset_level, reset_level);
	MLX5_SET(mfrl_reg, in, rst_type_sel, reset_type_sel);
	MLX5_SET(mfrl_reg, in, pci_sync_for_fw_update_resp, sync_resp);
	MLX5_SET(mfrl_reg, in, pci_sync_for_fw_update_start, sync_start);

	return mlx5_core_access_reg(dev, in, sizeof(in), out, sizeof(out), MLX5_REG_MFRL, 0, 1);
}

static int mlx5_reg_mfrl_query(struct mlx5_core_dev *dev, u8 *reset_level, u8 *reset_type)
{
	u32 out[MLX5_ST_SZ_DW(mfrl_reg)] = {};
	u32 in[MLX5_ST_SZ_DW(mfrl_reg)] = {};
	int err;

	err = mlx5_core_access_reg(dev, in, sizeof(in), out, sizeof(out), MLX5_REG_MFRL, 0, 0);
	if (err)
		return err;

	if (reset_level)
		*reset_level = MLX5_GET(mfrl_reg, out, reset_level);
	if (reset_type)
		*reset_type = MLX5_GET(mfrl_reg, out, reset_type);

	return 0;
}

int mlx5_fw_reset_query(struct mlx5_core_dev *dev, u8 *reset_level, u8 *reset_type)
{
	return mlx5_reg_mfrl_query(dev, reset_level, reset_type);
}

int mlx5_fw_reset_set_reset_sync(struct mlx5_core_dev *dev, u8 reset_type_sel)
{
	return mlx5_reg_mfrl_set(dev, MLX5_MFRL_REG_RESET_LEVEL3, reset_type_sel, 0, true);
}

int mlx5_fw_reset_set_live_patch(struct mlx5_core_dev *dev)
{
	return mlx5_reg_mfrl_set(dev, MLX5_MFRL_REG_RESET_LEVEL0, 0, 0, false);
}

static void mlx5_sync_reset_reload_work(struct work_struct *work)
{
	struct mlx5_fw_reset *fw_reset = container_of(work, struct mlx5_fw_reset,
						      reset_reload_work);
	struct mlx5_core_dev *dev = fw_reset->dev;

	mlx5_enter_error_state(dev, true);
	mlx5_unload_one(dev, false);
	if (mlx5_health_wait_pci_up(dev)) {
		mlx5_core_err(dev, "reset reload flow aborted, PCI reads still not working\n");
		return;
	}
	mlx5_load_one(dev, false);
}

static void mlx5_stop_sync_reset_poll(struct mlx5_core_dev *dev)
{
	struct mlx5_fw_reset *fw_reset = dev->priv.fw_reset;

	del_timer(&fw_reset->timer);
}

static void mlx5_sync_reset_clear_reset_requested(struct mlx5_core_dev *dev, bool poll_health)
{
	struct mlx5_fw_reset *fw_reset = dev->priv.fw_reset;

	mlx5_stop_sync_reset_poll(dev);
	clear_bit(MLX5_FW_RESET_FLAGS_RESET_REQUESTED, &fw_reset->reset_flags);
	if (poll_health)
		mlx5_start_health_poll(dev);
}

#define MLX5_RESET_POLL_INTERVAL	(HZ / 10)
static void poll_sync_reset(struct timer_list *t)
{
	struct mlx5_fw_reset *fw_reset = from_timer(fw_reset, t, timer);
	struct mlx5_core_dev *dev = fw_reset->dev;
	u32 fatal_error;

	if (!test_bit(MLX5_FW_RESET_FLAGS_RESET_REQUESTED, &fw_reset->reset_flags))
		return;

	fatal_error = mlx5_health_check_fatal_sensors(dev);

	if (fatal_error) {
		mlx5_core_warn(dev, "Got Device Reset\n");
		mlx5_sync_reset_clear_reset_requested(dev, false);
		queue_work(fw_reset->wq, &fw_reset->reset_reload_work);
		return;
	}

	mod_timer(&fw_reset->timer, round_jiffies(jiffies + MLX5_RESET_POLL_INTERVAL));
}

static void mlx5_start_sync_reset_poll(struct mlx5_core_dev *dev)
{
	struct mlx5_fw_reset *fw_reset = dev->priv.fw_reset;

	timer_setup(&fw_reset->timer, poll_sync_reset, 0);
	fw_reset->timer.expires = round_jiffies(jiffies + MLX5_RESET_POLL_INTERVAL);
	add_timer(&fw_reset->timer);
}

static int mlx5_fw_reset_set_reset_sync_ack(struct mlx5_core_dev *dev)
{
	return mlx5_reg_mfrl_set(dev, MLX5_MFRL_REG_RESET_LEVEL3, 0, 1, false);
}

static void mlx5_sync_reset_set_reset_requested(struct mlx5_core_dev *dev)
{
	struct mlx5_fw_reset *fw_reset = dev->priv.fw_reset;

	mlx5_stop_health_poll(dev, true);
	set_bit(MLX5_FW_RESET_FLAGS_RESET_REQUESTED, &fw_reset->reset_flags);
	mlx5_start_sync_reset_poll(dev);
}

static void mlx5_sync_reset_request_event(struct work_struct *work)
{
	struct mlx5_fw_reset *fw_reset = container_of(work, struct mlx5_fw_reset,
						      reset_request_work);
	struct mlx5_core_dev *dev = fw_reset->dev;
	int err;

	mlx5_sync_reset_set_reset_requested(dev);
	err = mlx5_fw_reset_set_reset_sync_ack(dev);
	if (err)
		mlx5_core_warn(dev, "PCI Sync FW Update Reset Ack Failed. Error code: %d\n", err);
	else
		mlx5_core_warn(dev, "PCI Sync FW Update Reset Ack. Device reset is expected.\n");
}

static void mlx5_sync_reset_events_handle(struct mlx5_fw_reset *fw_reset, struct mlx5_eqe *eqe)
{
	struct mlx5_eqe_sync_fw_update *sync_fw_update_eqe;
	u8 sync_event_rst_type;

	sync_fw_update_eqe = &eqe->data.sync_fw_update;
	sync_event_rst_type = sync_fw_update_eqe->sync_rst_state & SYNC_RST_STATE_MASK;
	switch (sync_event_rst_type) {
	case MLX5_SYNC_RST_STATE_RESET_REQUEST:
		queue_work(fw_reset->wq, &fw_reset->reset_request_work);
		break;
	}
}

static int fw_reset_event_notifier(struct notifier_block *nb, unsigned long action, void *data)
{
	struct mlx5_fw_reset *fw_reset = mlx5_nb_cof(nb, struct mlx5_fw_reset, nb);
	struct mlx5_eqe *eqe = data;

	switch (eqe->sub_type) {
	case MLX5_GENERAL_SUBTYPE_PCI_SYNC_FOR_FW_UPDATE_EVENT:
		mlx5_sync_reset_events_handle(fw_reset, eqe);
		break;
	default:
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}

void mlx5_fw_reset_events_start(struct mlx5_core_dev *dev)
{
	struct mlx5_fw_reset *fw_reset = dev->priv.fw_reset;

	MLX5_NB_INIT(&fw_reset->nb, fw_reset_event_notifier, GENERAL_EVENT);
	mlx5_eq_notifier_register(dev, &fw_reset->nb);
}

void mlx5_fw_reset_events_stop(struct mlx5_core_dev *dev)
{
	mlx5_eq_notifier_unregister(dev, &dev->priv.fw_reset->nb);
}

int mlx5_fw_reset_init(struct mlx5_core_dev *dev)
{
	struct mlx5_fw_reset *fw_reset = kzalloc(sizeof(*fw_reset), GFP_KERNEL);

	if (!fw_reset)
		return -ENOMEM;
	fw_reset->wq = create_singlethread_workqueue("mlx5_fw_reset_events");
	if (!fw_reset->wq) {
		kfree(fw_reset);
		return -ENOMEM;
	}

	fw_reset->dev = dev;
	dev->priv.fw_reset = fw_reset;

	INIT_WORK(&fw_reset->reset_request_work, mlx5_sync_reset_request_event);
	INIT_WORK(&fw_reset->reset_reload_work, mlx5_sync_reset_reload_work);

	return 0;
}

void mlx5_fw_reset_cleanup(struct mlx5_core_dev *dev)
{
	struct mlx5_fw_reset *fw_reset = dev->priv.fw_reset;

	destroy_workqueue(fw_reset->wq);
	kfree(dev->priv.fw_reset);
}
