#include <linux/etherdevice.h>
#include <linux/if_macvlan.h>
#include <linux/if_macvtap.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/nsproxy.h>
#include <linux/compat.h>
#include <linux/if_tun.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/cache.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/cdev.h>
#include <linux/idr.h>
#include <linux/fs.h>
#include <linux/uio.h>

#include <net/net_namespace.h>
#include <net/rtnetlink.h>
#include <net/sock.h>
#include <linux/virtio_net.h>
#include <linux/skb_array.h>

/*
 * Variables for dealing with macvtaps device numbers.
 */
static dev_t macvtap_major;
#define MACVTAP_NUM_DEVS (1U << MINORBITS)

static const void *macvtap_net_namespace(struct device *d)
{
	struct net_device *dev = to_net_dev(d->parent);
	return dev_net(dev);
}

static struct class macvtap_class = {
	.name = "macvtap",
	.owner = THIS_MODULE,
	.ns_type = &net_ns_type_operations,
	.namespace = macvtap_net_namespace,
};
static struct cdev macvtap_cdev;

#define TUN_OFFLOADS (NETIF_F_HW_CSUM | NETIF_F_TSO_ECN | NETIF_F_TSO | \
		      NETIF_F_TSO6 | NETIF_F_UFO)

static int macvtap_newlink(struct net *src_net,
			   struct net_device *dev,
			   struct nlattr *tb[],
			   struct nlattr *data[])
{
	struct macvlan_dev *vlan = netdev_priv(dev);
	int err;

	INIT_LIST_HEAD(&vlan->queue_list);

	/* Since macvlan supports all offloads by default, make
	 * tap support all offloads also.
	 */
	vlan->tap_features = TUN_OFFLOADS;

	err = netdev_rx_handler_register(dev, macvtap_handle_frame, vlan);
	if (err)
		return err;

	/* Don't put anything that may fail after macvlan_common_newlink
	 * because we can't undo what it does.
	 */
	err = macvlan_common_newlink(src_net, dev, tb, data);
	if (err) {
		netdev_rx_handler_unregister(dev);
		return err;
	}

	return 0;
}

static void macvtap_dellink(struct net_device *dev,
			    struct list_head *head)
{
	netdev_rx_handler_unregister(dev);
	macvtap_del_queues(dev);
	macvlan_dellink(dev, head);
}

static void macvtap_setup(struct net_device *dev)
{
	macvlan_common_setup(dev);
	dev->tx_queue_len = TUN_READQ_SIZE;
}

static struct rtnl_link_ops macvtap_link_ops __read_mostly = {
	.kind		= "macvtap",
	.setup		= macvtap_setup,
	.newlink	= macvtap_newlink,
	.dellink	= macvtap_dellink,
};

static int macvtap_device_event(struct notifier_block *unused,
				unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct macvlan_dev *vlan;
	struct device *classdev;
	dev_t devt;
	int err;
	char tap_name[IFNAMSIZ];

	if (dev->rtnl_link_ops != &macvtap_link_ops)
		return NOTIFY_DONE;

	snprintf(tap_name, IFNAMSIZ, "tap%d", dev->ifindex);
	vlan = netdev_priv(dev);

	switch (event) {
	case NETDEV_REGISTER:
		/* Create the device node here after the network device has
		 * been registered but before register_netdevice has
		 * finished running.
		 */
		err = macvtap_get_minor(vlan);
		if (err)
			return notifier_from_errno(err);

		devt = MKDEV(MAJOR(macvtap_major), vlan->minor);
		classdev = device_create(&macvtap_class, &dev->dev, devt,
					 dev, tap_name);
		if (IS_ERR(classdev)) {
			macvtap_free_minor(vlan);
			return notifier_from_errno(PTR_ERR(classdev));
		}
		err = sysfs_create_link(&dev->dev.kobj, &classdev->kobj,
					tap_name);
		if (err)
			return notifier_from_errno(err);
		break;
	case NETDEV_UNREGISTER:
		/* vlan->minor == 0 if NETDEV_REGISTER above failed */
		if (vlan->minor == 0)
			break;
		sysfs_remove_link(&dev->dev.kobj, tap_name);
		devt = MKDEV(MAJOR(macvtap_major), vlan->minor);
		device_destroy(&macvtap_class, devt);
		macvtap_free_minor(vlan);
		break;
	case NETDEV_CHANGE_TX_QUEUE_LEN:
		if (macvtap_queue_resize(vlan))
			return NOTIFY_BAD;
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block macvtap_notifier_block __read_mostly = {
	.notifier_call	= macvtap_device_event,
};

extern struct file_operations macvtap_fops;
static int macvtap_init(void)
{
	int err;

	err = alloc_chrdev_region(&macvtap_major, 0,
				MACVTAP_NUM_DEVS, "macvtap");
	if (err)
		goto out1;

	cdev_init(&macvtap_cdev, &macvtap_fops);
	err = cdev_add(&macvtap_cdev, macvtap_major, MACVTAP_NUM_DEVS);
	if (err)
		goto out2;

	err = class_register(&macvtap_class);
	if (err)
		goto out3;

	err = register_netdevice_notifier(&macvtap_notifier_block);
	if (err)
		goto out4;

	err = macvlan_link_register(&macvtap_link_ops);
	if (err)
		goto out5;

	return 0;

out5:
	unregister_netdevice_notifier(&macvtap_notifier_block);
out4:
	class_unregister(&macvtap_class);
out3:
	cdev_del(&macvtap_cdev);
out2:
	unregister_chrdev_region(macvtap_major, MACVTAP_NUM_DEVS);
out1:
	return err;
}
module_init(macvtap_init);

extern struct idr minor_idr;
static void macvtap_exit(void)
{
	rtnl_link_unregister(&macvtap_link_ops);
	unregister_netdevice_notifier(&macvtap_notifier_block);
	class_unregister(&macvtap_class);
	cdev_del(&macvtap_cdev);
	unregister_chrdev_region(macvtap_major, MACVTAP_NUM_DEVS);
	idr_destroy(&minor_idr);
}
module_exit(macvtap_exit);

MODULE_ALIAS_RTNL_LINK("macvtap");
MODULE_AUTHOR("Arnd Bergmann <arnd@arndb.de>");
MODULE_LICENSE("GPL");
