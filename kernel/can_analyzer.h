/* SPDX-License-Identifier: GPL-2.0 */
/*
 * can_analyzer.h: what the parts of the module share.
 * can_analyzer_rx.c owns the kfifo and the wait queue.
 * can_analyzer_chrdev.c owns /dev/can_analyzer.
 * can_analyzer_filter.c owns the filter rules.
 * can_analyzer_stats.c owns the counters and /proc/can_analyzer/stats.
 */
#ifndef CAN_ANALYZER_H
#define CAN_ANALYZER_H

#include <linux/types.h>
#include <linux/wait.h>
#include <linux/can.h>
#include "can_analyzer_uapi.h"

/* One stored frame: when it arrived and what it was. */
struct analyzer_record {
	u64 ts_ns;
	struct can_frame cf;
};

/* Readers sleep here until a frame is stored. */
extern wait_queue_head_t analyzer_wq;

/* Take the oldest record out of the buffer. Returns 1 if one was taken. */
int analyzer_rx_pop(struct analyzer_record *rec);

/* True when the buffer holds nothing. */
bool analyzer_rx_empty(void);

/* Create and remove /dev/can_analyzer. */
int analyzer_chrdev_init(void);
void analyzer_chrdev_exit(void);

/* Filter rules. accept() runs in softirq, the others come from ioctl. */
bool analyzer_filter_accept(canid_t can_id);
int analyzer_filter_add(const struct can_analyzer_rule *in);
int analyzer_filter_del(const struct can_analyzer_rule *in);
void analyzer_filter_list(struct can_analyzer_rule_list *list);
void analyzer_filter_exit(void);

/* Statistics. frame() and error() run in softirq. */
int analyzer_stats_init(void);
void analyzer_stats_exit(void);
void analyzer_stats_frame(const struct can_frame *cf);
void analyzer_stats_error(const struct can_frame *cf);

/* rx counters for /proc. Process context. */
void analyzer_rx_counters(long *filtered, long *dropped, unsigned int *unread);

#endif
