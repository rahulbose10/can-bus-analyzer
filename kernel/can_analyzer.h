/* SPDX-License-Identifier: GPL-2.0 */
/*
 * can_analyzer.h: what the parts of the module share.
 * can_analyzer_rx.c owns the kfifo and the wait queue.
 * can_analyzer_chrdev.c owns /dev/can_analyzer.
 */
#ifndef CAN_ANALYZER_H
#define CAN_ANALYZER_H

#include <linux/types.h>
#include <linux/wait.h>
#include <linux/can.h>

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

#endif
