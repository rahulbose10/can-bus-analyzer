/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * can_analyzer_uapi.h: the ioctl interface, shared with user space.
 * Only fixed-size types, so a user program can include it as well.
 */
#ifndef CAN_ANALYZER_UAPI_H
#define CAN_ANALYZER_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define CAN_ANALYZER_ACCEPT     1
#define CAN_ANALYZER_DROP       2
#define CAN_ANALYZER_MAX_RULES  64

/* Set CAN_EFF_FLAG (0x80000000) in can_id for a 29-bit rule. */
struct can_analyzer_rule {
	__u32 can_id;
	__u32 mask;
	__u32 action;
};

struct can_analyzer_rule_list {
	__u32 count;
	struct can_analyzer_rule rules[CAN_ANALYZER_MAX_RULES];
};

#define CAN_ANALYZER_IOC_MAGIC  0xCA
#define CAN_ANALYZER_ADD_RULE   _IOW(CAN_ANALYZER_IOC_MAGIC, 1, struct can_analyzer_rule)
#define CAN_ANALYZER_DEL_RULE   _IOW(CAN_ANALYZER_IOC_MAGIC, 2, struct can_analyzer_rule)
#define CAN_ANALYZER_LIST_RULES _IOR(CAN_ANALYZER_IOC_MAGIC, 3, struct can_analyzer_rule_list)

#endif
