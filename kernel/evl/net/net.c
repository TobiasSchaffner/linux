/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2021 Philippe Gerum  <rpm@xenomai.org>
 */

#include <linux/notifier.h>
#include <linux/netdevice.h>
#include <linux/socket.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/nsproxy.h>
#include <net/net_namespace.h>
#include <evl/factory.h>
#include <evl/uaccess.h>
#include <evl/net/qdisc.h>
#include <evl/net/device.h>
#include <evl/net/input.h>
#include <evl/net/output.h>
#include <evl/net/skb.h>
#include <evl/net/packet.h>
#include <evl/net/ipv4/arp.h>
#include <evl/net/ipv4/route.h>
#include <evl/net/ipv4.h>
#include <evl/net/tap.h>
#include <evl/net.h>

/*
 * Called by the in-band stack to setup the oob state which is going
 * to be maintained by EVL in a network namespace.
 */
void net_init_oob_state(struct net *net)
{
	evl_net_init_packet(net);
	evl_net_init_ipv4(net);
}

/*
 * Converse to net_init_oob_state(), called to cleanup the oob state
 * which is being dismanted by the in-band stack.
 */
void net_cleanup_oob_state(struct net *net)
{
	evl_net_cleanup_ipv4(net);
	evl_net_cleanup_packet(net);
}

static struct notifier_block netdev_notifier = {
	.notifier_call = evl_netdev_event
};

int __init evl_net_init(void)
{
	int ret;

	evl_net_init_tx();

	evl_net_init_qdisc();

	evl_net_init_taps();

	ret = register_netdevice_notifier(&netdev_notifier);
	if (ret)
		goto fail_notifier;

	ret = evl_register_socket_domain(&evl_net_packet);
	if (ret)
		goto fail_packet;

	ret = evl_register_socket_domain(&evl_net_ipv4);
	if (ret)
		goto fail_ipv4;

	/* AF_OOB is given no dedicated socket cache. */
	ret = proto_register(&evl_af_oob_proto, 0);
	if (ret)
		goto fail_proto;

	sock_register(&evl_family_ops);

	return 0;

fail_proto:
	evl_unregister_socket_domain(&evl_net_ipv4);
fail_ipv4:
	evl_unregister_socket_domain(&evl_net_packet);
fail_packet:
	unregister_netdevice_notifier(&netdev_notifier);
fail_notifier:
	evl_net_cleanup_qdisc();

	return ret;
}

void __init evl_net_cleanup(void)
{
	sock_unregister(PF_OOB);
	proto_unregister(&evl_af_oob_proto);
	evl_unregister_socket_domain(&evl_net_packet);
	unregister_netdevice_notifier(&netdev_notifier);
	evl_net_cleanup_qdisc();
}

static long net_ioctl(struct file *filp, unsigned int cmd,
		unsigned long arg)
{
	struct evl_net_devparams devp, __user *u_devp;
	struct evl_net_devfd fdreq, __user *u_fdreq;
	struct net *net = current->nsproxy->net_ns;
	const char __user *u_name;
	struct filename *devname;
	struct net_device *dev;
	long ret;
	int ufd;

	switch (cmd) {
	case EVL_NET_OPENPORT: /* Turn oob port on. */
		u_devp = (typeof(u_devp))arg;
		ret = copy_from_user(&devp, u_devp, sizeof(devp));
		if (ret)
			return -EFAULT;
		u_name = evl_valptr64(devp.name_ptr, const char);
		devname = getname(u_name);
		if (IS_ERR(devname))
			return PTR_ERR(devname);
		dev = dev_get_by_name(net, devname->name);
		putname(devname);
		if (!dev)
			return -EINVAL;
		ret = evl_net_switch_oob_port(dev, &devp);
		dev_put(dev);	/* Drop the ref. obtained from dev_get_by_name() */
		if (ret)
			break;
		/* The port is left open on user-specific errors. */
		ufd = __evl_net_dev_allocfd(dev);
		if (ufd < 0)
			break;
		ret = put_user((__u32)ufd, &u_devp->fd);
		if (ret)
			ret = -EFAULT;
		break;
	case EVL_NET_GETDEVFD:	/* Get a fildes on an oob-enabled device. */
		u_fdreq = (typeof(u_fdreq))arg;
		ret = copy_from_user(&fdreq, u_fdreq, sizeof(fdreq));
		if (ret)
			return -EFAULT;
		u_name = evl_valptr64(fdreq.name_ptr, const char);
		devname = getname(u_name);
		if (IS_ERR(devname))
			return PTR_ERR(devname);
		ufd = evl_net_dev_allocfd(net, devname->name);
		putname(devname);
		if (ufd < 0)
			return ufd;
		ret = put_user((__u32)ufd, &u_fdreq->fd);
		if (ret)
			return -EFAULT;
		break;
	default:
		ret = -ENOTTY;
	}

	return ret;
}

static const struct file_operations net_fops = {
	.unlocked_ioctl	= net_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= compat_ptr_ioctl,
#endif
};

static ssize_t vlans_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return evl_net_show_vlans(buf, PAGE_SIZE);
}

static ssize_t vlans_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	return evl_net_store_vlans(buf, count);
}
static DEVICE_ATTR_RW(vlans);

static ssize_t ipv4_solicit_timeout_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return sysfs_emit(buf, "%d\n", evl_net_ipv4_solicit_timeout);
}

static ssize_t ipv4_solicit_timeout_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	ssize_t ret;
	long value;

	ret = kstrtol(buf, 0, &value);
	if (ret < 0)
		return ret;

	if (value <= 0)
		return -EINVAL;

	evl_net_ipv4_solicit_timeout = value;

	return count;
}
static DEVICE_ATTR_RW(ipv4_solicit_timeout);

static ssize_t ipv4_flush_routes_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct net *net = current->nsproxy->net_ns;

	evl_net_flush_ipv4_routes(net, NULL);

	return count;
}
static DEVICE_ATTR_WO(ipv4_flush_routes);

static ssize_t ipv4_flush_arp_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct net *net = current->nsproxy->net_ns;

	evl_net_flush_arp(net, NULL);

	return count;
}
static DEVICE_ATTR_WO(ipv4_flush_arp);

static struct attribute *net_attrs[] = {
	&dev_attr_vlans.attr,
	&dev_attr_ipv4_solicit_timeout.attr,
	&dev_attr_ipv4_flush_routes.attr,
	&dev_attr_ipv4_flush_arp.attr,
	NULL,
};
ATTRIBUTE_GROUPS(net);

struct evl_factory evl_net_factory = {
	.name	=	"net",
	.fops	=	&net_fops,
	.attrs	=	net_groups,
	.flags	=	EVL_FACTORY_SINGLE,
};
