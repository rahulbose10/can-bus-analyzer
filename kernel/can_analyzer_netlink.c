// SPDX-License-Identifier: GPL-2.0
/*
 * can_analyzer_netlink.c: the generic netlink family "can_analyzer".
 * Commands read the statistics and change filter rules. Every error
 * frame is also sent to the "events" multicast group.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <net/genetlink.h>
#include "can_analyzer.h"

static struct genl_family analyzer_family;

static const struct nla_policy analyzer_policy[CAN_ANALYZER_A_MAX + 1] = {
	[CAN_ANALYZER_A_RULE_ID]     = { .type = NLA_U32 },
	[CAN_ANALYZER_A_RULE_MASK]   = { .type = NLA_U32 },
	[CAN_ANALYZER_A_RULE_ACTION] = { .type = NLA_U32 },
};

static int put_stats(struct sk_buff *msg, const struct analyzer_stats_snapshot *st)
{
	if (nla_put_u64_64bit(msg, CAN_ANALYZER_A_FRAMES, st->frames, CAN_ANALYZER_A_PAD) ||
		nla_put_u64_64bit(msg, CAN_ANALYZER_A_FILTERED, st->filtered, CAN_ANALYZER_A_PAD) ||
		nla_put_u64_64bit(msg, CAN_ANALYZER_A_DROPPED, st->dropped, CAN_ANALYZER_A_PAD) ||
		nla_put_u64_64bit(msg, CAN_ANALYZER_A_ERRORS, st->errors, CAN_ANALYZER_A_PAD) ||
		nla_put_u32(msg, CAN_ANALYZER_A_UNREAD, st->unread) ||
		nla_put_u32(msg, CAN_ANALYZER_A_RATE_FPS, st->rate_fps) ||
		nla_put_u32(msg, CAN_ANALYZER_A_PEAK_FPS, st->peak_fps) ||
		nla_put_u32(msg, CAN_ANALYZER_A_LOAD_X10, st->load_x10) ||
		nla_put_u32(msg, CAN_ANALYZER_A_PEAK_LOAD_X10, st->peak_load_x10))
		return -EMSGSIZE;
	return 0;
}

/* Process context. Replies with one message that holds every counter. */
static int analyzer_nl_get_stats(struct sk_buff *skb, struct genl_info *info)
{
	struct analyzer_stats_snapshot st;
	struct sk_buff *msg;
	void *hdr;

	analyzer_stats_snapshot(&st);

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	hdr = genlmsg_put_reply(msg, info, &analyzer_family, 0,
							CAN_ANALYZER_CMD_GET_STATS);
	if (!hdr || put_stats(msg, &st)) {
		nlmsg_free(msg);
		return -EMSGSIZE;
	}
	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);
}

/* Process context. GENL_ADMIN_PERM makes the core check CAP_NET_ADMIN first. */
static int analyzer_nl_rule(struct sk_buff *skb, struct genl_info *info)
{
	struct can_analyzer_rule rule = {};

	if (!info->attrs[CAN_ANALYZER_A_RULE_ID] ||
		!info->attrs[CAN_ANALYZER_A_RULE_MASK])
		return -EINVAL;
	rule.can_id = nla_get_u32(info->attrs[CAN_ANALYZER_A_RULE_ID]);
	rule.mask = nla_get_u32(info->attrs[CAN_ANALYZER_A_RULE_MASK]);
	if (info->attrs[CAN_ANALYZER_A_RULE_ACTION])
		rule.action = nla_get_u32(info->attrs[CAN_ANALYZER_A_RULE_ACTION]);

	if (info->genlhdr->cmd == CAN_ANALYZER_CMD_ADD_RULE)
		return analyzer_filter_add(&rule);
	return analyzer_filter_del(&rule);
}

static const struct genl_small_ops analyzer_ops[] = {
	{
		.cmd   = CAN_ANALYZER_CMD_GET_STATS,
		.doit  = analyzer_nl_get_stats,
	},
	{
		.cmd   = CAN_ANALYZER_CMD_ADD_RULE,
		.doit  = analyzer_nl_rule,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd   = CAN_ANALYZER_CMD_DEL_RULE,
		.doit  = analyzer_nl_rule,
		.flags = GENL_ADMIN_PERM,
	},
};

static const struct genl_multicast_group analyzer_mcgrps[] = {
	{ .name = CAN_ANALYZER_MCGRP_EVENTS },
};

static struct genl_family analyzer_family = {
	.name        = CAN_ANALYZER_GENL_NAME,
	.version     = CAN_ANALYZER_GENL_VERSION,
	.maxattr     = CAN_ANALYZER_A_MAX,
	.policy      = analyzer_policy,
	.module      = THIS_MODULE,
	.small_ops   = analyzer_ops,
	.n_small_ops = ARRAY_SIZE(analyzer_ops),
	.mcgrps      = analyzer_mcgrps,
	.n_mcgrps    = ARRAY_SIZE(analyzer_mcgrps),
};

/* Softirq context, so GFP_ATOMIC. Nothing is built when nobody listens. */
void analyzer_nl_error_event(u32 err_class, u32 ctrl_status, u64 ts_ns)
{
	struct sk_buff *msg;
	void *hdr;

	if (!genl_has_listeners(&analyzer_family, &init_net, 0))
		return;

	msg = genlmsg_new(2 * nla_total_size(sizeof(u32)) +
					  nla_total_size_64bit(sizeof(u64)), GFP_ATOMIC);
	if (!msg)
		return;
	hdr = genlmsg_put(msg, 0, 0, &analyzer_family, 0,
					  CAN_ANALYZER_CMD_ERROR_EVENT);
	if (!hdr ||
		nla_put_u32(msg, CAN_ANALYZER_A_ERR_CLASS, err_class) ||
		nla_put_u32(msg, CAN_ANALYZER_A_CTRL_STATUS, ctrl_status) ||
		nla_put_u64_64bit(msg, CAN_ANALYZER_A_TIMESTAMP, ts_ns,
						  CAN_ANALYZER_A_PAD)) {
		nlmsg_free(msg);
		return;
	}
	genlmsg_end(msg, hdr);
	genlmsg_multicast(&analyzer_family, msg, 0, 0, GFP_ATOMIC);
}

int analyzer_nl_init(void)
{
	return genl_register_family(&analyzer_family);
}

void analyzer_nl_exit(void)
{
	genl_unregister_family(&analyzer_family);
}
