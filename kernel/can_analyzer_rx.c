// SPDX-License-Identifier: GPL-2.0
/*
 * can_analyzer_rx.c: receive hook and frame buffer.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/skbuff.h>
#include <linux/kfifo.h>
#include <linux/spinlock.h>
#include <linux/timekeeping.h>
#include <linux/wait.h>
#include <linux/can.h>
#include <linux/can/core.h>
#include <net/net_namespace.h>
#include "can_analyzer.h"

static char *ifname = "can1";
module_param(ifname, charp, 0444);
MODULE_PARM_DESC(ifname, "CAN interface to listen on (default can1)");

static unsigned int fifo_size = 256;
module_param(fifo_size, uint, 0444);
MODULE_PARM_DESC(fifo_size, "Frames the buffer holds, rounded up to a power of 2 (default 256)");

static DECLARE_KFIFO_PTR(rx_fifo, struct analyzer_record);
static DEFINE_SPINLOCK(rx_lock);
DECLARE_WAIT_QUEUE_HEAD(analyzer_wq);

static struct net_device *analyzer_dev;
static atomic_long_t frames_seen = ATOMIC_LONG_INIT(0);
static atomic_long_t frames_dropped = ATOMIC_LONG_INIT(0);

/*
 * Softirq context. The frame is copied because the skb is freed after
 * we return. A full fifo drops the new frame, it never overwrites.
 */
static void analyzer_rx(struct sk_buff *skb, void *data)
{
	struct analyzer_record rec;
	unsigned int stored;

	if (skb->len < sizeof(struct can_frame))
		return;

	rec.ts_ns = ktime_get_ns();
	memcpy(&rec.cf, skb->data, sizeof(rec.cf));
	atomic_long_inc(&frames_seen);

	spin_lock(&rx_lock);
	stored = kfifo_put(&rx_fifo, rec);
	spin_unlock(&rx_lock);

	if (stored)
		wake_up_interruptible(&analyzer_wq);
	else
		atomic_long_inc(&frames_dropped);
}

/* Called from read(). _bh because the rx callback runs in softirq on this CPU. */
int analyzer_rx_pop(struct analyzer_record *rec)
{
	int ret;

	spin_lock_bh(&rx_lock);
	ret = kfifo_get(&rx_fifo, rec);
	spin_unlock_bh(&rx_lock);
	return ret;
}

/* Called from read() and poll(). */
bool analyzer_rx_empty(void)
{
	bool empty;

	spin_lock_bh(&rx_lock);
	empty = kfifo_is_empty(&rx_fifo);
	spin_unlock_bh(&rx_lock);
	return empty;
}

/* Only at rmmod, when nothing else can touch the fifo, so no lock. */
static void analyzer_drain(void)
{
	struct analyzer_record rec;
	unsigned int n;
	u32 id;

	while (kfifo_get(&rx_fifo, &rec)) {
		n = min_t(unsigned int, rec.cf.len, CAN_MAX_DLEN);
		id = (rec.cf.can_id & CAN_EFF_FLAG) ? (rec.cf.can_id & CAN_EFF_MASK)
											: (rec.cf.can_id & CAN_SFF_MASK);
		pr_info("unread t=%llu ns id=0x%x len=%u data=%*ph\n",
				rec.ts_ns, id, n, (int)n, rec.cf.data);
	}
}

/* The fifo and the device must exist before the callback can fire. */
static int __init analyzer_init(void)
{
	int ret;

	ret = kfifo_alloc(&rx_fifo, fifo_size, GFP_KERNEL);
	if (ret) {
		pr_err("kfifo_alloc(%u) failed: %d\n", fifo_size, ret);
		return ret;
	}

	analyzer_dev = dev_get_by_name(&init_net, ifname);
	if (!analyzer_dev) {
		pr_err("interface %s not found\n", ifname);
		ret = -ENODEV;
		goto err_free;
	}
	if (analyzer_dev->type != ARPHRD_CAN) {
		pr_err("%s is not a CAN interface\n", ifname);
		ret = -EINVAL;
		goto err_put;
	}

	ret = analyzer_chrdev_init();
	if (ret) {
		pr_err("misc_register failed: %d\n", ret);
		goto err_put;
	}

	ret = can_rx_register(&init_net, analyzer_dev, 0, 0, analyzer_rx,
						  NULL, "can_analyzer", NULL);
	if (ret) {
		pr_err("can_rx_register failed: %d\n", ret);
		goto err_chrdev;
	}

	pr_info("listening on %s, buffer holds %u frames, /dev/can_analyzer ready\n",
			ifname, kfifo_size(&rx_fifo));
	return 0;

err_chrdev:
	analyzer_chrdev_exit();
err_put:
	dev_put(analyzer_dev);
err_free:
	kfifo_free(&rx_fifo);
	return ret;
}

/*
 * Reverse of init. Unregister needs the same arguments as register.
 * synchronize_rcu() waits for a callback that may still be running.
 */
static void __exit analyzer_exit(void)
{
	can_rx_unregister(&init_net, analyzer_dev, 0, 0, analyzer_rx, NULL);
	synchronize_rcu();
	analyzer_chrdev_exit();
	dev_put(analyzer_dev);

	pr_info("stopped: %ld seen, %u unread, %ld dropped\n",
			atomic_long_read(&frames_seen), kfifo_len(&rx_fifo),
			atomic_long_read(&frames_dropped));
	analyzer_drain();
	kfifo_free(&rx_fifo);
}

module_init(analyzer_init);
module_exit(analyzer_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rahul Bose");
MODULE_DESCRIPTION("CAN bus analyzer: receive hook, kfifo, /dev/can_analyzer");
