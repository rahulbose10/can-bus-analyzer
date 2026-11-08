// SPDX-License-Identifier: GPL-2.0
/*
 * can_analyzer_chrdev.c: /dev/can_analyzer, one text line per frame.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
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

	return scnprintf(line, size, "t=%llu id=0x%x len=%u data=%*ph\n",
					 rec->ts_ns, id, n, (int)n, rec->cf.data);
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

static const struct file_operations analyzer_fops = {
	.owner  = THIS_MODULE,
	.read   = analyzer_read,
	.poll   = analyzer_poll,
	.llseek = noop_llseek,
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
