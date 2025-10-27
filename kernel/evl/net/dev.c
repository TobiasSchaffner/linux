/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2020 Philippe Gerum  <rpm@xenomai.org>
 */

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/netdevice.h>
#include <linux/if_vlan.h>
#include <linux/err.h>
#include <linux/rtnetlink.h>
#include <linux/bpf.h>
#include <linux/filter.h>
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <evl/uaccess.h>
#include <evl/sched.h>
#include <evl/thread.h>
#include <evl/crossing.h>
#include <evl/factory.h>
#include <evl/net/socket.h>
#include <evl/net/device.h>
#include <evl/net/skb.h>
#include <evl/net/qdisc.h>
#include <evl/net/input.h>
#include <evl/net/output.h>
#include <evl/net/route.h>
#include <uapi/evl/net/net-abi.h>

/*
 * Since we need an EVL kthread to handle traffic from the out-of-band
 * stage without borrowing CPU time unwisely from random contexts,
 * let's have separate, per-device threads for RX and TX. This gives
 * the best flexibility for leveraging multi-core capabilities on
 * high-bandwidth systems. Kthread priority defaults to 50, chrt is
 * our friend for fine-grained tuning. Unlike the RX kthread which is
 * always created for a device underlying an oob port, the TX one is
 * optional, present only if the NIC driver for that device is
 * oob-capable.
 */
#define KTHREAD_RX_PRIO  50
#define KTHREAD_TX_PRIO  50

/*
 * The default number of I/O pages which should be available on a
 * per-device basis for conveying out-of-band traffic if not specified
 * by an EVL_NET_DEVON request.
 */
#define EVL_DEFAULT_NETDEV_POOLSZ  2048
/*
 * The default fixed payload size available in I/O pages for conveying
 * out-of-band traffic through the device.
 */
#define EVL_DEFAULT_NETDEV_BUFSZ   PAGE_SIZE
/*
 * Max. values for the settings above.
 */
#define EVL_MAX_NETDEV_POOLSZ  32768
#define EVL_MAX_NETDEV_BUFSZ   (PAGE_SIZE * 4)

/*
 * The list of network interfaces (real and virtual devices) which are
 * usable for sending/receiving oob traffic.
 */
static LIST_HEAD(oob_port_list);

static DEFINE_HARD_SPINLOCK(oob_port_lock);

static struct evl_net_ebpf_filter *
__set_rx_filter(struct evl_netdev_state *est,
		struct evl_net_ebpf_filter *filter);

static void disable_oob_port(struct net_device *dev);

static struct evl_kthread *
start_handler_thread(struct net_device *dev,
		void (*fn)(void *arg),
		int prio, const char *type)
{
	struct evl_kthread *kt;
	int ret;

	kt = kzalloc(sizeof(*kt), GFP_KERNEL);
	if (kt == NULL)
		return ERR_PTR(-ENOMEM);

	ret = evl_run_kthread(kt, fn, dev, prio, 0, "%s.%s",
			netdev_name(dev), type);
	if (ret) {
		kfree(kt);
		return ERR_PTR(ret);
	}

	return kt;
}

/*
 * enable_oob_port - @dev is a device which we want to enable as a
 * port for channeling out-of-band traffic. This may be a real device,
 * or a VLAN interface.
 */
static int enable_oob_port(struct net_device *dev,
			struct evl_net_devparams *p) /* inband, rtnl_lock held */
{
	struct oob_netdev_state *rnds, *nds;
	struct evl_netdev_state *pest, *est;
	struct net_device *real_dev;
	struct evl_kthread *kt;
	unsigned long flags;
	unsigned int mtu;
	int ret;

	if (netif_oob_port(dev))
		return 0;	/* Already enabled. */

	/*
	 * If @dev is a VLAN device, diversion is turned on for the
	 * real device it sits on top of, so that the EVL stack is
	 * given a chance to pick the ingress traffic to be routed to
	 * the oob stage. The resources we need are allocated only for
	 * the real device.
	 *
	 * NOTE: the diversion flag is set for a real device only,
	 * _never_ for a VLAN device.
	 */
	real_dev = evl_net_real_dev(dev);
	rnds = &real_dev->oob_state;
	est = pest = rnds->estate;
	if (pest == NULL) {
		est = kzalloc(sizeof(*est), GFP_KERNEL);
		if (est == NULL)
			return -ENOMEM;
		rnds->estate = est;
	}

	nds = &dev->oob_state;
	evl_init_crossing(&nds->crossing);

	if (est->refs++ > 0)	/* Guarded by rtnl_lock. */
		goto queue;

	if (!p->poolsz)
		p->poolsz = EVL_DEFAULT_NETDEV_POOLSZ;

	if (!p->bufsz)
		p->bufsz = EVL_DEFAULT_NETDEV_BUFSZ;

	/*
	 * Use the device MTU as the bare minimum unless this is a
	 * loopback device (those have preposterously large MTUs for
	 * oob usage).
	 */
	mtu = READ_ONCE(real_dev->mtu);
	if (!(real_dev->flags & IFF_LOOPBACK) && p->bufsz < mtu)
		p->bufsz = mtu;

	est->pool_max = p->poolsz;
	est->buf_size = p->bufsz;
	spin_lock_init(&est->filter_lock);
	est->qdisc = evl_net_alloc_qdisc(&evl_net_qdisc_fifo);
	if (IS_ERR(est->qdisc)) {
		ret = PTR_ERR(est->qdisc);
		goto fail_alloc_qdisc;
	}

	ret = evl_net_dev_build_pool(real_dev);
	if (ret)
		goto fail_build_pool;

	evl_net_init_skb_queue(&est->rx_packets);
	INIT_LIST_HEAD(&est->napi_poll);
	raw_spin_lock_init(&est->napi_lock);
	evl_init_flag(&est->rx_flag);

	kt = start_handler_thread(real_dev, evl_net_do_rx,
				KTHREAD_RX_PRIO, "rx");
	if (IS_ERR(kt))
		goto fail_start_rx;

	est->rx_handler = kt;

	/*
	 * We need a TX handler only for oob-capable
	 * devices. Otherwise, the traffic would go through an in-band
	 * Qdisc, like the sched_oob in-band queues.
	 */
	if (netdev_is_oob_capable(real_dev)) {
		evl_init_flag(&est->tx_flag);
		kt = start_handler_thread(real_dev, evl_net_do_tx,
					KTHREAD_TX_PRIO, "tx");
		if (IS_ERR(kt)) {
			ret = PTR_ERR(kt);
			goto fail_start_tx;
		}

		est->tx_handler = kt;
	}

	if (nds != rnds) /* i.e. dev is a VLAN interface. */
		evl_init_crossing(&rnds->crossing);

	/* Divert traffic from the real device. */
	ret = netif_enable_oob_diversion(real_dev);
	if (ret)
		goto fail_enable_diversion;
queue:
	netif_enable_oob_port(dev);

	raw_spin_lock_irqsave(&oob_port_lock, flags);
	list_add(&nds->next, &oob_port_list);
	raw_spin_unlock_irqrestore(&oob_port_lock, flags);

	/* Declare the oob-enabled device to the routing system. */
	ret = evl_net_add_device_route(dev);
	if (ret)
		disable_oob_port(dev);

	return ret;

fail_enable_diversion:
	if (est->tx_handler) {
		evl_stop_kthread(est->tx_handler);
		evl_destroy_flag(&est->tx_flag);
	}
fail_start_tx:
	/*
	 * No skb has flowed yet, no pending recycling op. Likewise,
	 * we cannot have any rxq in the cache or dump lists.
	 */
	evl_stop_kthread(est->rx_handler);
	evl_destroy_flag(&est->rx_flag);
fail_start_rx:
	evl_net_dev_purge_pool(real_dev);
fail_build_pool:
	evl_net_free_qdisc(est->qdisc);
fail_alloc_qdisc:
	if (!pest) {
		kfree(est);
		rnds->estate = NULL;
	}

	return ret;
}

/*
 * disable_oob_port - @dev is a device for which we want to disable
 * the oob port capability (see enable_oob_port()). This may be a real
 * device, or a VLAN interface.
 */
static void disable_oob_port(struct net_device *dev) /* inband, rtnl_lock held */
{
	struct oob_netdev_state *nds, *rnds;
	struct evl_netdev_state *est;
	struct net_device *real_dev;
	unsigned long flags;

	if (!netif_oob_port(dev))
		return;

	/* Remove the oob-enabled device from the routing system. */
	evl_net_remove_device_route(dev);

	/*
	 * Make sure that no evl_down_crossing() can be issued after
	 * we attempt to pass the crossing. Since the former can only
	 * happen as a result of finding the device in the active
	 * list, first unlink the latter _then_ pass the crossing
	 * next.
	 */
	real_dev = evl_net_real_dev(dev);
	nds = &dev->oob_state;
	raw_spin_lock_irqsave(&oob_port_lock, flags);
	list_del(&nds->next);
	raw_spin_unlock_irqrestore(&oob_port_lock, flags);

	/*
	 * Ok, now we may attempt to pass the crossing, waiting until
	 * all in-flight oob operations holding a reference on the
	 * network device acting as an oob port have completed.
	 */
	evl_pass_crossing(&nds->crossing);

	netif_disable_oob_port(dev);

	rnds = &real_dev->oob_state;
	est = rnds->estate;

	if (EVL_WARN_ON(NET, est->refs <= 0))
		return;

	/*
	 * We might have multiple VLAN devices acting as oob ports
	 * sitting on the same real device, so use refcounting.
	 */
	if (--est->refs > 0)	/* Guarded by rtnl_lock. */
		return;

	/*
	 * Last ref. from a port to a real device dropped. Dismantle
	 * the extension for the latter. Start with unblocking all the
	 * waiters.
	 */
	evl_signal_poll_events(&est->poll_head, POLLERR);
	evl_flush_wait(&est->tx_wait, EVL_T_RMID);
	evl_schedule();

	netif_disable_oob_diversion(real_dev);

	evl_stop_kthread(est->rx_handler);
	evl_destroy_flag(&est->rx_flag);
	if (est->tx_handler) {
		evl_stop_kthread(est->tx_handler);
		evl_destroy_flag(&est->tx_flag);
	}

	__set_rx_filter(est, NULL);
	evl_net_dev_purge_pool(real_dev);
	/* Once the TX kthread is stopped, qdisc is idle. */
	evl_net_free_qdisc(est->qdisc);
	kfree(est);
	rnds->estate = NULL;
}

static int switch_oob_port(struct net_device *dev,
			struct evl_net_devparams *p) /* rtnl_lock held, in-band */
{
	int ret = 0;

	/*
	 * Turn on/off oob port for the device. When set, packets
	 * received by the device flowing through the in-band net core
	 * are diverted to netif_deliver_oob().
	 */
	if (p) {
		if (p->poolsz > EVL_MAX_NETDEV_POOLSZ ||
			p->bufsz > EVL_MAX_NETDEV_BUFSZ)
			ret = -EINVAL;
		else
			ret = enable_oob_port(dev, p);
	} else {
		disable_oob_port(dev);
	}

	return ret;
}

int evl_net_switch_oob_port(struct net_device *dev,
			struct evl_net_devparams *p)
{
	struct net *net = dev_net(dev);
	int ret;

	rtnl_net_lock(net);
	ret = switch_oob_port(dev, p);
	rtnl_net_unlock(net);

	return ret;
}
EXPORT_SYMBOL_GPL(evl_net_switch_oob_port);

struct net_device *evl_net_get_dev_by_index(struct net *net, int ifindex)
{
	struct net_device *dev, *ret = NULL;
	struct oob_netdev_state *nds;
	unsigned long flags;

	if (!ifindex)
		return NULL;

	raw_spin_lock_irqsave(&oob_port_lock, flags);

	list_for_each_entry(nds, &oob_port_list, next) {
		dev = container_of(nds, struct net_device, oob_state);
		if (dev_net(dev) == net && dev->ifindex == ifindex) {
			evl_down_crossing(&nds->crossing);
			ret = dev;
			break;
		}
	}

	raw_spin_unlock_irqrestore(&oob_port_lock, flags);

	return ret;
}

struct net_device *evl_net_get_dev_by_flags(struct net *net, int ifflags)
{
	struct net_device *dev, *ret = NULL;
	struct oob_netdev_state *nds;
	unsigned long flags;

	raw_spin_lock_irqsave(&oob_port_lock, flags);

	list_for_each_entry(nds, &oob_port_list, next) {
		dev = container_of(nds, struct net_device, oob_state);
		if (dev_net(dev) == net && dev->flags & ifflags) {
			evl_down_crossing(&nds->crossing);
			ret = dev;
			break;
		}
	}

	raw_spin_unlock_irqrestore(&oob_port_lock, flags);

	return ret;
}

struct net_device *evl_net_get_dev_by_name(struct net *net, const char *name)
{
	struct net_device *dev, *ret = NULL;
	struct oob_netdev_state *nds;
	unsigned long flags;

	raw_spin_lock_irqsave(&oob_port_lock, flags);

	list_for_each_entry(nds, &oob_port_list, next) {
		dev = container_of(nds, struct net_device, oob_state);
		if (dev_net(dev) == net && !strcmp(netdev_name(dev), name)) {
			evl_down_crossing(&nds->crossing);
			ret = dev;
			break;
		}
	}

	raw_spin_unlock_irqrestore(&oob_port_lock, flags);

	return ret;
}

void evl_net_get_dev(struct net_device *dev)
{
	struct oob_netdev_state *nds = &dev->oob_state;

	evl_down_crossing(&nds->crossing);
}

void evl_net_put_dev(struct net_device *dev)
{
	struct oob_netdev_state *nds = &dev->oob_state;

	EVL_WARN_ON(NET, hard_irqs_disabled());

	evl_up_crossing(&nds->crossing);
}

/**
 *	netif_oob_switch_port - turn on oob capability for a network
 *	device.
 *
 *	This call is invoked from the net-sysfs interface in order to
 *	change the state of the oob port of a network device. The
 *	default pool and buffer size are used when enabling.
 *
 *	@dev The network device for which the oob port should be
 *	switched on/off. This may be a real device or a VLAN
 *	interface.
 *
 *	@enabled The new oob port state.
 *
 *	Returns zero on success, an error code otherwise.
 */
int netif_oob_switch_port(struct net_device *dev, bool enabled) /* rtnl_lock held */
{
	struct evl_net_devparams p = {
		.poolsz = 0,
		.bufsz = 0,
	};

	return switch_oob_port(dev, enabled ? &p : NULL);
}

/**
 *	netif_oob_get_port - get the oob diversion state
 *
 *	Returns true if the device is an EVL port for conveying
 *	networking traffic from the out-of-band execution stage.
 */
bool netif_oob_get_port(struct net_device *dev)
{
	return netif_oob_port(dev);
}

ssize_t netif_oob_query_pool(struct net_device *dev, char *buf)
{
	struct evl_netdev_state *est;
	struct net_device *real_dev;

	real_dev = evl_net_real_dev(dev);
	est = real_dev->oob_state.estate;
	if (est == NULL)
		return -ENXIO;

	return sprintf(buf, "%zu %zu\n", est->pool_max, est->buf_size);
}

int evl_netdev_event(struct notifier_block *ev_block,
		unsigned long event, void *ptr) /* rtnl_lock held */
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	switch (event) {
	case NETDEV_UP:
		break;
	case NETDEV_GOING_DOWN:
		if (netif_oob_port(dev)) {
			disable_oob_port(dev);
			evl_net_retire_device(dev);
		}
		break;
	}

	return NOTIFY_DONE;
}

enum evl_net_rx_action
__evl_net_filter_rx(struct evl_netdev_state *est, struct sk_buff *skb)
{
	struct evl_net_ebpf_filter *filter = READ_ONCE(est->rx_filter);
	enum evl_net_rx_action ret = EVL_RX_VLAN;

	if (filter) {
		rcu_read_lock(); /* Recheck under lock. */

		filter = rcu_dereference(est->rx_filter);
		if (filter)
			ret = bpf_prog_run_clear_cb(filter->prog, skb);

		rcu_read_unlock();
	}

	return ret;
}

static void drop_rx_filter(struct rcu_head *rcu)
{
	struct evl_net_ebpf_filter *filter = container_of(rcu, struct evl_net_ebpf_filter, rcu);

	bpf_prog_destroy(filter->prog);
	kfree(filter);
}

static struct evl_net_ebpf_filter *
__set_rx_filter(struct evl_netdev_state *est,
		struct evl_net_ebpf_filter *filter)
{
	struct evl_net_ebpf_filter *old;

	spin_lock_bh(&est->filter_lock);

	old = rcu_dereference_protected(est->rx_filter,
				lockdep_is_held(&est->filter_lock));
	rcu_assign_pointer(est->rx_filter, filter);

	if (filter)
		set_bit(EVL_NETDEV_RX_FILTER_BIT, &est->flags);
	else
		clear_bit(EVL_NETDEV_RX_FILTER_BIT, &est->flags);

	spin_unlock_bh(&est->filter_lock);

	if (old)
		call_rcu(&old->rcu, drop_rx_filter);

	return old;
}

/*
 * set_rx_filter - install an eBPF program module to redirect ingress
 * traffic to the proper network stack, inband or oob.  @dev is a
 * physical interface.
 */
static int set_rx_filter(struct net_device *dev, unsigned long arg)
{
	struct oob_netdev_state *nds = &dev->oob_state;
	struct evl_net_ebpf_filter *old, *new = NULL;
	struct bpf_prog *prog;
	int ret, fd;

	ret = raw_get_user(fd, (__s32 *)arg);
	if (ret)
		return -EFAULT;

	if (fd != -1) {
		prog = bpf_prog_get_type(fd, BPF_PROG_TYPE_SOCKET_FILTER);
		if (IS_ERR(prog)) {
			netdev_warn(dev, "invalid out-of-band eBPF program\n");
			return PTR_ERR(prog);
		}
		new = kmalloc(sizeof(*new), GFP_KERNEL);
		if (!new)
			return -ENOMEM;
		new->prog = prog;
		netdev_notice(dev, "out-of-band eBPF program installed\n");
	}

	old = __set_rx_filter(nds->estate, new);
	if (old && !new)
		netdev_notice(dev, "out-of-band eBPF program removed\n");

	return 0;
}

#define _distance(a, b)	(s32)((a) - (b))

static int get_dev_stat(struct net_device *dev, struct evl_net_devstat *devs)
{
	struct net_device *real_dev = evl_net_real_dev(dev);
	struct evl_netdev_state *est = real_dev->oob_state.estate;
	u64 alloc_count, release_count;

	devs->__flags = 0;	/* Clear this first. */
	devs->oob_capable = netdev_is_oob_capable(real_dev);
	devs->rx_packets = evl_counter_read_careful(&est->stats.rx_packets);
	devs->rx_bytes = evl_counter_read_careful(&est->stats.rx_bytes);
	devs->tx_packets = evl_counter_read_careful(&est->stats.tx_packets);
	devs->tx_bytes = evl_counter_read_careful(&est->stats.tx_bytes);
	devs->rx_nomem = evl_counter_read_careful(&est->stats.rx_nomem);
	devs->tx_nomem = evl_counter_read_careful(&est->stats.tx_nomem);
	devs->csum_errors = evl_counter_read_careful(&est->stats.csum_errors);
	devs->skb_size = est->buf_size;
	alloc_count = evl_counter_read_careful(&est->stats.pool_alloc_count);
	release_count = evl_counter_read_careful(&est->stats.pool_release_count);
	devs->skb_free = est->pool_max - _distance(alloc_count, release_count);
	devs->skb_total = est->pool_max;

	return 0;
}

/* in-band hook, called by a driver upon oob memory shortage on RX. */
void netif_rx_nomem_oob(struct net_device *dev)
{
	struct net_device *real_dev = evl_net_real_dev(dev);
	struct evl_netdev_state *est = real_dev->oob_state.estate;

	evl_counter_inc_careful(&est->stats.tx_nomem);
}
EXPORT_SYMBOL(netif_rx_nomem_oob);

static long netdev_ioctl(struct file *filp, unsigned int cmd,
			unsigned long arg)
{
	struct evl_net_devstat devs = { 0 }, __user *u_devs;
	struct net_device *dev = filp->private_data;
	int ret = -ENOTTY;

	switch (cmd) {
	case EVL_NDEVIOC_SETRXEBPF:
		ret = set_rx_filter(evl_net_real_dev(dev), arg);
		break;
	case EVL_NDEVIOC_SWITCHOFF:
		ret = evl_net_switch_oob_port(dev, NULL);
		break;
	case EVL_NDEVIOC_GETSTAT:
		ret = get_dev_stat(dev, &devs);
		if (ret)
			return ret;
		u_devs = (typeof(u_devs))arg;
		ret = copy_to_user(u_devs, &devs, sizeof(devs));
		if (ret)
			return -EFAULT;
		break;
	}

	return ret;
}

static int netdev_release(struct inode *inode, struct file *filp)
{
	struct net_device *dev = filp->private_data;

	evl_net_put_dev(dev);

	return 0;
}

static const struct file_operations netdev_fops = {
	.open		= stream_open,
	.release	= netdev_release,
	.unlocked_ioctl	= netdev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= compat_ptr_ioctl,
#endif
};

int __evl_net_dev_allocfd(struct net_device *dev)
{
	struct file *filp;
	int ret, fd;

	filp = anon_inode_getfile("[evl-netdev]", &netdev_fops,
				dev, O_RDWR|O_CLOEXEC);
	if (IS_ERR(filp))
		return PTR_ERR(filp);

	fd = get_unused_fd_flags(O_RDWR|O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto fail;
	}

	filp->private_data = dev;

	fd_install(fd, filp);

	return fd;
fail:
	filp_close(filp, current->files);

	return ret;
}

int evl_net_dev_allocfd(struct net *net, const char *devname)
{
	struct net_device *dev;
	int ret;

	dev = evl_net_get_dev_by_name(net, devname);
	if (!dev)
		return -EINVAL;

	ret = __evl_net_dev_allocfd(dev);
	if (ret)
		evl_net_put_dev(dev);

	/*
	 * On success, the reference on @dev will be released by
	 * netdev_release().
	 */

	return ret;
}
