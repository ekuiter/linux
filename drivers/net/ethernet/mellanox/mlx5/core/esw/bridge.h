/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2021 Mellanox Technologies. */

#ifndef __MLX5_ESW_BRIDGE_H__
#define __MLX5_ESW_BRIDGE_H__

#include <linux/notifier.h>
#include <linux/list.h>
#include "eswitch.h"

struct mlx5_flow_table;
struct mlx5_flow_group;
struct workqueue_struct;

struct mlx5_esw_bridge_offloads {
	struct mlx5_eswitch *esw;
	struct list_head bridges;
	struct notifier_block netdev_nb;
	struct notifier_block nb;
	struct workqueue_struct *wq;

	struct mlx5_flow_table *ingress_ft;
	struct mlx5_flow_group *ingress_mac_fg;
};

struct mlx5_esw_bridge_offloads *mlx5_esw_bridge_init(struct mlx5_eswitch *esw);
void mlx5_esw_bridge_cleanup(struct mlx5_eswitch *esw);
int mlx5_esw_bridge_vport_link(int ifindex, struct mlx5_esw_bridge_offloads *br_offloads,
			       struct mlx5_vport *vport, struct netlink_ext_ack *extack);
int mlx5_esw_bridge_vport_unlink(int ifindex, struct mlx5_esw_bridge_offloads *br_offloads,
				 struct mlx5_vport *vport, struct netlink_ext_ack *extack);
void mlx5_esw_bridge_fdb_create(struct net_device *dev, struct mlx5_eswitch *esw,
				struct mlx5_vport *vport,
				struct switchdev_notifier_fdb_info *fdb_info);
void mlx5_esw_bridge_fdb_remove(struct net_device *dev, struct mlx5_eswitch *esw,
				struct mlx5_vport *vport,
				struct switchdev_notifier_fdb_info *fdb_info);

#endif /* __MLX5_ESW_BRIDGE_H__ */
