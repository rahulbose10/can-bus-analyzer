// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>	/* current */


/*
 * hello_init - module entry point
 * Context: process context, running as the insmod process.
 * Return 0 on success; a negative error aborts the load.
 */

static int __init hello_init(void)
{
    pr_info("loaded by pid %d (%s)\n", current->pid, current->comm);
    return 0;
}

/*
 * hello_exit - module teardown
 * Context: process context, running as the rmmod process.
 * Must undo everything hello_init() did, in reverse order.
 */

static void __exit hello_exit(void)
{
    pr_info("unloaded by pid %d (%s)\n", current->pid, current->comm);
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Rahul Bose");
MODULE_DESCRIPTION("Week 0 hello module - proves the build/load/unload cycle");
MODULE_VERSION("0.1");

