// SPDX-License-Identifier: GPL-2.0
/*
 * can_analyzer_chrdev.c: /dev/can_analyzer, one text line per frame,
 * and the ioctl calls that manage filter rules.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include "can_analyzer.h"

/* The longest line is about 75 characters. */
#define ANALYZER_LINE_MAX 96

static int analyzer_format(const struct analyzer_record *rec, char *line,
						   size_t size)
{
	unsigned int n = min_t(unsigned int, rec->cf.len, CAN_MAX_DLEN);
	u32 id = (rec->cf.can_id & CAN_EFF_FLAG) ? (rec->cf.can_id & CAN_EFF_MASK)
											 : (rec->cf.can_id & CAN_SFF_MASK);

	return scnprintf(line, size, "t=%llu id=0x%0*x len=%u data=%*ph\n",
					 rec->ts_ns, (rec->cf.can_id & CAN_EFF_FLAG) ? 8 : 3, id,
					 n, (int)n, rec->cf.data);
}

/* One frame per call. Sleeps until a frame arrives unless O_NONBLOCK. */
static ssize_t analyzer_read(struct file *file, char __user *buf,
							 size_t count, loff_t *ppos)
{
	struct analyzer_record rec;
	char line[ANALYZER_LINE_MAX];
	int len;
	int ret;

	if (count < ANALYZER_LINE_MAX)
		return -EINVAL;

	while (!analyzer_rx_pop(&rec)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(analyzer_wq, !analyzer_rx_empty());
		if (ret)
			return ret;
	}

	len = analyzer_format(&rec, line, sizeof(line));
	if (copy_to_user(buf, line, len))
		return -EFAULT;
	return len;
}

/* poll_wait() does not sleep. It only puts us on the queue for later. */
static __poll_t analyzer_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &analyzer_wq, wait);
	return analyzer_rx_empty() ? 0 : (EPOLLIN | EPOLLRDNORM);
}

/*
 * Changing rules needs CAP_NET_ADMIN, listing them is open to all.
 * The list is kzalloc'ed because unused slots are copied to user space.
 */
static long analyzer_ioctl(struct file *file, unsigned int cmd,
						   unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	struct can_analyzer_rule_list *list;
	struct can_analyzer_rule rule;
	long ret;

	switch (cmd) {
	case CAN_ANALYZER_ADD_RULE:
	case CAN_ANALYZER_DEL_RULE:
		if (!capable(CAP_NET_ADMIN))
			return -EPERM;
		if (copy_from_user(&rule, uarg, sizeof(rule)))
			return -EFAULT;
		if (cmd == CAN_ANALYZER_ADD_RULE)
			return analyzer_filter_add(&rule);
		return analyzer_filter_del(&rule);
	case CAN_ANALYZER_LIST_RULES:
		list = kzalloc(sizeof(*list), GFP_KERNEL);
		if (!list)
			return -ENOMEM;
		analyzer_filter_list(list);
		ret = copy_to_user(uarg, list, sizeof(*list)) ? -EFAULT : 0;
		kfree(list);
		return ret;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations analyzer_fops = {
	.owner          = THIS_MODULE,
	.read           = analyzer_read,
	.poll           = analyzer_poll,
	.unlocked_ioctl = analyzer_ioctl,
	.llseek         = noop_llseek,
};

static struct miscdevice analyzer_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "can_analyzer",
	.fops  = &analyzer_fops,
	.mode  = 0444,
};

int analyzer_chrdev_init(void)
{
	return misc_register(&analyzer_misc);
}

void analyzer_chrdev_exit(void)
{
	misc_deregister(&analyzer_misc);
}
