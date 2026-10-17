// SPDX-License-Identifier: GPL-2.0
/*
 * can_analyzer_rx.c: Week 1, step 1.
 * Hooks SocketCAN with can_rx_register() and prints every frame it sees.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/skbuff.h>
#include <linux/can.h>
#include <linux/can/core.h>
#include <net/net_namespace.h>

static char *ifname = "can1";
module_param(ifname, charp, 0444);
MODULE_PARM_DESC(ifname, "CAN interface to listen on (default can1)");

static struct net_device *analyzer_dev;
static atomic_long_t frames_seen = ATOMIC_LONG_INIT(0);

/*
 * analyzer_rx: called by SocketCAN for every frame on the interface.
 * Why: this is our hook into the CAN stack.
 * Context: softirq. No sleeping, no mutex.
 */
static void analyzer_rx(struct sk_buff *skb, void *data)
{
    struct can_frame *cf;
    unsigned int n;
    u32 id;

    if (skb->len < sizeof(struct can_frame))
        return;

    cf = (struct can_frame *)skb->data;
    n = min_t(unsigned int, cf->len, CAN_MAX_DLEN);
    id = (cf->can_id & CAN_EFF_FLAG) ? (cf->can_id & CAN_EFF_MASK)
                                     : (cf->can_id & CAN_SFF_MASK);

    atomic_long_inc(&frames_seen);
    pr_info("rx id=0x%x len=%u data=%*ph\n", id, n, (int)n, cf->data);
}

/*
 * analyzer_init: find the interface and register for all data frames.
 * Context: process (insmod). Sleeping is allowed here.
 */
static int __init analyzer_init(void)
{
    int ret;

    analyzer_dev = dev_get_by_name(&init_net, ifname);
    if (!analyzer_dev) {
        pr_err("interface %s not found\n", ifname);
        return -ENODEV;
    }
    if (analyzer_dev->type != ARPHRD_CAN) {
        pr_err("%s is not a CAN interface\n", ifname);
        ret = -EINVAL;
        goto err_put;
    }

    ret = can_rx_register(&init_net, analyzer_dev, 0, 0, analyzer_rx,
                          NULL, "can_analyzer", NULL);
    if (ret) {
        pr_err("can_rx_register failed: %d\n", ret);
        goto err_put;
    }

    pr_info("listening on %s\n", ifname);
    return 0;

err_put:
    dev_put(analyzer_dev);
    return ret;
}

/*
 * analyzer_exit: undo init in reverse order.
 * The arguments to can_rx_unregister must match the register call exactly.
 * Context: process (rmmod).
 */
static void __exit analyzer_exit(void)
{
    can_rx_unregister(&init_net, analyzer_dev, 0, 0, analyzer_rx, NULL);
    synchronize_rcu();
    dev_put(analyzer_dev);
    pr_info("stopped, %ld frames seen\n", atomic_long_read(&frames_seen));
}

module_init(analyzer_init);
module_exit(analyzer_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rahul Bose");
MODULE_DESCRIPTION("CAN bus analyzer, week 1: SocketCAN receive hook");
