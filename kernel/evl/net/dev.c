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
 * setup_base_device - Setup a real device for packet diversion.
 *
 * This routine allocates a runtime state for the device, starts the
 * RX/TX kthreads as necessary. May be called for an already set up
 * device, which yields a nop.
 */
static int setup_base_device(struct net_device *real_dev,
			struct evl_net_devparams *p)
{
	struct evl_netdev_stats *stats;
	struct evl_netdev_state *est;
	struct oob_netdev_state *nds;
	struct evl_kthread *kt;
	unsigned int mtu;
	int ret;

	nds = &real_dev->oob_state;
	est = nds->estate;
	if (est) {
		refcount_inc(&est->users);
		return 0;	/* Already set up. */
	}

	est = kzalloc(sizeof(*est), GFP_KERNEL);
	if (est == NULL)
		return -ENOMEM;

	nds->estate = est;
	stats = kzalloc(sizeof(*stats), GFP_KERNEL);
	if (stats == NULL) {
		kfree(est);
		return -ENOMEM;
	}

	nds->stats = stats;

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

	INIT_LIST_HEAD(&est->vlans);
	refcount_set(&est->users, 1);
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

	mutex_init(&nds->bind_lock);
	INIT_LIST_HEAD(&nds->bindings);
	evl_init_crossing(&nds->crossing);

	/*
	 * Tell the in-band stack to divert traffic flowing in from
	 * the real device to netif_deliver_oob().
	 */
	ret = netif_enable_oob_diversion(real_dev);
	if (ret)
		goto fail_enable_diversion;

	return 0;

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
	kfree(est);
	nds->estate = NULL;
	kfree(stats);
	nds->stats = NULL;

	return ret;
}

/*
 * dismantle_base_device - Disable packet diversion on a real device.
 *
 * This routine deallocates the runtime state of a base device,
 * stopping the associated RX/TX kthreads as necessary.
 */
static void dismantle_base_device(struct net_device *real_dev) /* inband, rtnl_lock held */
{
	struct oob_netdev_state *rnds;
	struct evl_netdev_state *est;

	rnds = &real_dev->oob_state;
	est = rnds->estate;

	/*
	 * No more users, we may dismantle the runtime state. Start
	 * with unblocking all the waiters.
	 */
	evl_signal_poll_events(&est->poll_head, POLLERR);
	evl_flush_wait(&est->tx_wait, EVL_T_RMID);
	evl_schedule();

	/*
	 * The order is important: first we stop the TX thread so that
	 * the NIC driver may assume that we won't do any hard xmit
	 * from the oob stage anymore. Then we disable diversion in
	 * the NIC driver before stopping the RX thread, so that we
	 * may assume that no RX won't happen from the oob stage
	 * anymore. In addition, proper synchronization is performed
	 * by both threads to flush the pending traffic.
	 */
	if (est->tx_handler) {
		evl_stop_kthread(est->tx_handler);
		evl_destroy_flag(&est->tx_flag);
	}

	netif_disable_oob_diversion(real_dev);

	evl_stop_kthread(est->rx_handler); /* Stop before destroy the flag. */
	evl_destroy_flag(&est->rx_flag);

	__set_rx_filter(est, NULL);
	evl_net_dev_purge_pool(real_dev);
	/* Once the TX kthread is stopped, qdisc is idle. */
	evl_net_free_qdisc(est->qdisc);
	kfree(est);
	rnds->estate = NULL;
	kfree(rnds->stats);
	rnds->stats = NULL;
}

static bool __put_base_device(struct net_device *real_dev)
{
	struct oob_netdev_state *rnds = &real_dev->oob_state;
	struct evl_netdev_state *est = rnds->estate;

	return refcount_dec_and_test(&est->users);
}

static void put_base_device(struct net_device *real_dev)
{
	if (__put_base_device(real_dev))
		dismantle_base_device(real_dev);
}

/*
 * enable_oob_port - Activate an out-of-band port.
 *
 * @dev - the device we want to enable the oob port on.
 *
 * @dev may be a real device, or a VLAN interface.
 */
static int enable_oob_port(struct net_device *dev,
			struct evl_net_devparams *p) /* inband, rtnl_lock held */
{
	struct evl_netdev_stats *stats;
	struct oob_netdev_state *nds;
	struct net_device *real_dev;
	int ret;

	if (netif_oob_port(dev))
		return 0;	/* Already enabled. */

	real_dev = evl_net_real_dev(dev);
	if (!(real_dev->flags & IFF_UP))
		return -ENETDOWN;

	/* Enable packet diversion on the base device. */
	ret = setup_base_device(real_dev, p);
	if (ret)
		return ret;

	nds = &dev->oob_state;

	if (real_dev == dev)
		goto queue;

	/* This is a VLAN device, set it up too. */

	stats = kzalloc(sizeof(*stats), GFP_KERNEL);
	if (stats == NULL) {
		put_base_device(real_dev);
		return -ENOMEM;
	}

	mutex_init(&nds->bind_lock);
	INIT_LIST_HEAD(&nds->bindings);
	evl_init_crossing(&nds->crossing);
	nds->stats = stats;
	list_add_rcu(&nds->next, &real_dev->oob_state.estate->vlans); /* Guarded by rtnl */
queue:
	netif_enable_oob_port(dev);

	/* Advertise the device to the routing system. */
	ret = evl_net_add_device_route(dev);
	if (ret)
		disable_oob_port(dev);

	return ret;
}

static void drop_bindings(struct oob_netdev_state *nds)
{
	struct evl_socket *esk, *n;

	/* Force unbind any socket bound to the downed device. */
	mutex_lock(&nds->bind_lock);

	list_for_each_entry_safe(esk, n, &nds->bindings, next_binding) {
		list_del_init(&n->next_binding);
		esk->proto->force_unbind(esk);
	}

	mutex_unlock(&nds->bind_lock);
}

/*
 * disable_oob_port - Dectivate an out-of-band port.
 *
 * @dev - the device we want to disable the oob port from.
 *
 * @dev may be a real device, or a VLAN interface.
 */
static void disable_oob_port(struct net_device *dev) /* inband, rtnl_lock held */
{
	struct oob_netdev_state *nds = &dev->oob_state;
	struct net_device *real_dev;

	if (!netif_oob_port(dev))
		return;

	/* Remove the oob-enabled device from the routing system. */
	evl_net_remove_device_route(dev);

	/* Force unbind any socket bound to the downed device. */
	drop_bindings(nds);

	/*
	 * Make sure that no evl_down_crossing() can be issued on the
	 * device after we attempt to pass its crossing. Since we must
	 * find that device with an active port first to do so, turn
	 * the latter off before passing the crossing.
	 */
	netif_disable_oob_port(dev);

	real_dev = evl_net_real_dev(dev);
	if (dev != real_dev) {
		list_del_rcu(&nds->next); /* Guarded by rtnl */
		evl_pass_crossing(&nds->crossing);
		kfree(nds->stats);
		nds->stats = NULL;
		put_base_device(real_dev);
	} else {
		if (__put_base_device(dev)) {
			evl_pass_crossing(&nds->crossing);
			dismantle_base_device(dev);
		}
	}
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
	struct oob_netdev_state *nds;
	struct net_device *dev;

	if (!ifindex)
		return NULL;

	dev = dev_get_by_index(net, ifindex);
	if (!dev)
		return NULL;

	if (!netif_oob_port(dev)) {
		dev_put(dev);
		return NULL;
	}

	nds = &dev->oob_state;
	evl_down_crossing(&nds->crossing);
	dev_put(dev);

	return dev;
}

struct net_device *evl_net_get_dev_by_name(struct net *net, const char *name)
{
	struct oob_netdev_state *nds;
	struct net_device *dev;

	dev = dev_get_by_name(net, name);
	if (!dev)
		return NULL;

	if (!netif_oob_port(dev)) {
		dev_put(dev);
		return NULL;
	}

	nds = &dev->oob_state;
	evl_down_crossing(&nds->crossing);
	dev_put(dev);

	return dev;
}

/**
 * evl_net_find_vlan_dev - find an VLAN device from its id.
 *
 * Only VLAN devices with an active oob port are considered.  May be
 * called from any stage, RCU read-side required.
 */
struct net_device *evl_net_find_vlan_dev(struct net_device *real_dev,
					__be16 vlan_proto, __u16 vlan_id)
{
	struct oob_netdev_state *nds;
	struct net_device *vlan_dev;

	if (!real_dev->oob_state.estate)
		return NULL;	/* Wow. Houston? */

	list_for_each_entry_rcu(nds, &real_dev->oob_state.estate->vlans, next) {
		vlan_dev = container_of(nds, struct net_device, oob_state);
		if (vlan_dev_vlan_proto(vlan_dev) != vlan_proto)
			continue;
		if (vlan_dev_vlan_id(vlan_dev) == vlan_id)
			return vlan_dev;
	}

	return NULL;
}

void evl_net_dev_bind(struct net_device *dev, struct evl_socket *esk)
{
	struct oob_netdev_state *nds = &dev->oob_state;

	if (EVL_WARN_ON(NET, !list_empty(&esk->next_binding)))
		return;

	mutex_lock(&nds->bind_lock);
	list_add(&esk->next_binding, &dev->oob_state.bindings);
	mutex_unlock(&nds->bind_lock);
}

void evl_net_dev_unbind(struct evl_socket *esk, int bound_if)
{
	struct oob_netdev_state *nds;
	struct net_device *dev;

	if (!bound_if) {
		EVL_WARN_ON(NET, !list_empty(&esk->next_binding));
		return;
	}

	if (EVL_WARN_ON(NET, list_empty(&esk->next_binding)))
		return;

	dev = evl_net_get_dev_by_index(sock_net(esk->sk), bound_if);
	if (EVL_WARN_ON(NET, !dev))
		return;

	nds = &dev->oob_state;
	mutex_lock(&nds->bind_lock);
	list_del_init(&esk->next_binding);
	mutex_unlock(&nds->bind_lock);
	evl_net_put_dev(dev);
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
	struct evl_netdev_state *est = evl_net_get_state(dev);

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
__evl_net_filter_rx(struct evl_netdev_state *est, struct sk_buff *skb) /* rcu_read_lock held. */
{
	enum evl_net_rx_action ret = EVL_RX_VLAN;
	struct evl_net_ebpf_filter *filter;

	filter = rcu_dereference(est->rx_filter);
	if (filter)
		ret = bpf_prog_run_clear_cb(filter->prog, skb);

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
	struct evl_net_ebpf_filter *old, *new = NULL;
	struct oob_netdev_state *nds;
	struct net_device *real_dev;
	struct bpf_prog *prog;
	int ret, fd;

	real_dev = evl_net_real_dev(dev);
	if (!netif_oob_diversion(real_dev))
		return -EINVAL;

	ret = raw_get_user(fd, (__s32 *)arg);
	if (ret)
		return -EFAULT;

	nds = &real_dev->oob_state;

	if (fd != -1) {
		prog = bpf_prog_get_type(fd, BPF_PROG_TYPE_SOCKET_FILTER);
		if (IS_ERR(prog)) {
			netdev_warn(real_dev, "invalid out-of-band eBPF program\n");
			return PTR_ERR(prog);
		}
		new = kmalloc(sizeof(*new), GFP_KERNEL);
		if (!new)
			return -ENOMEM;
		new->prog = prog;
		netdev_notice(real_dev, "out-of-band eBPF program installed\n");
	}

	old = __set_rx_filter(nds->estate, new);
	if (old && !new)
		netdev_notice(real_dev, "out-of-band eBPF program removed\n");

	return 0;
}

static int get_dev_stat(struct net_device *dev, struct evl_net_devstat *devs)
{
	struct evl_netdev_stats *stats = evl_net_get_stats(dev);
	struct evl_netdev_state *est = evl_net_get_state(dev);

	if (!(est && stats))
		return -EINVAL;

	memset(devs, 0, sizeof(*devs));
	devs->oob_capable = netdev_is_oob_capable(evl_net_real_dev(dev));
	devs->oob_port = netif_oob_port(dev);
	devs->rx_packets = evl_counter_read_careful(&stats->rx_packets);
	devs->rx_bytes = evl_counter_read_careful(&stats->rx_bytes);
	devs->tx_packets = evl_counter_read_careful(&stats->tx_packets);
	devs->tx_bytes = evl_counter_read_careful(&stats->tx_bytes);
	devs->rx_nomem = evl_counter_read_careful(&stats->rx_nomem);
	devs->tx_nomem = evl_counter_read_careful(&stats->tx_nomem);
	devs->csum_errors = evl_counter_read_careful(&stats->csum_errors);
	devs->skb_size = est->buf_size;
	devs->skb_free = READ_ONCE(est->tx_pages->alloc.count);
	devs->skb_total = est->pool_max;

	return 0;
}

/* in-band hook, called by a driver upon oob memory shortage on RX. */
void netif_rx_nomem_oob(struct net_device *dev)
{
	struct evl_netdev_stats *stats = evl_net_get_stats(dev);

	evl_counter_inc_careful(&stats->tx_nomem);
}
EXPORT_SYMBOL(netif_rx_nomem_oob);

static long netdev_ioctl(struct file *filp, unsigned int cmd,
			unsigned long arg)
{
	struct evl_net_devparams devp = { 0 }, __user *u_devp;
	struct evl_net_devstat devs = { 0 }, __user *u_devs;
	struct net_device *dev = filp->private_data;
	int ret = -ENOTTY;

	switch (cmd) {
	case EVL_NDEVIOC_SETPORT:
		u_devp = (typeof(u_devp))arg;
		if (u_devp) {
			ret = copy_from_user(&devp, u_devp, sizeof(devp));
			if (ret)
				return -EFAULT;
		}
		ret = evl_net_switch_oob_port(dev, u_devp ? &devp : NULL);
		break;
	case EVL_NDEVIOC_SETRXEBPF:
		ret = set_rx_filter(dev, arg);
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

	dev_put(dev);

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

static int dev_allocfd(struct net_device *dev)
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

	/* The caller forwarded us a reference to @dev. */
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
	int fd;

	dev = dev_get_by_name(net, devname);
	if (!dev)
		return -EINVAL;

	fd = dev_allocfd(dev);
	if (fd < 0)
		dev_put(dev);

	/* Otherwise, netdev_release() will put the device back. */

	return fd;
}
