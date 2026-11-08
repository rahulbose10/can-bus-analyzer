// SPDX-License-Identifier: GPL-2.0
/*
 * can_analyzer_tool.c: talk to the can_analyzer module over generic netlink.
 *
 *   can_analyzer_tool --stats
 *   sudo can_analyzer_tool --add-filter <id> <mask> accept|drop
 *   sudo can_analyzer_tool --del-filter <id> <mask>
 *   can_analyzer_tool --watch-errors
 *
 * id and mask are hex. An id written with 8 hex digits is a 29-bit id,
 * the same rule cansend uses.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/can.h>
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include "can_analyzer_uapi.h"

static const char *const err_names[] = {
	"tx_timeout", "lost_arbitration", "controller", "protocol",
	"transceiver", "no_ack", "bus_off", "bus_error", "restarted",
	"counters",
};

static void usage(void)
{
	fprintf(stderr,
			"usage:\n"
			"  can_analyzer_tool --stats\n"
			"  can_analyzer_tool --add-filter <id> <mask> accept|drop\n"
			"  can_analyzer_tool --del-filter <id> <mask>\n"
			"  can_analyzer_tool --watch-errors\n");
	exit(2);
}

static uint32_t parse_hex(const char *text, int is_id)
{
	const char *digits = text;
	char *end;
	unsigned long value;

	if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
		digits += 2;
	value = strtoul(digits, &end, 16);
	if (*digits == '\0' || *end != '\0')
		usage();
	if (is_id && strlen(digits) == 8)
		value |= CAN_EFF_FLAG;
	return (uint32_t)value;
}

/* Open a generic netlink socket and look up our family by name. */
static struct nl_sock *open_family(int *family)
{
	struct nl_sock *sk = nl_socket_alloc();

	if (!sk || genl_connect(sk) < 0) {
		fprintf(stderr, "error: cannot open a netlink socket\n");
		exit(1);
	}
	*family = genl_ctrl_resolve(sk, CAN_ANALYZER_GENL_NAME);
	if (*family < 0) {
		fprintf(stderr, "error: netlink family %s not found, is the module loaded?\n",
				CAN_ANALYZER_GENL_NAME);
		exit(1);
	}
	return sk;
}

static struct nl_msg *new_msg(int family, uint8_t cmd)
{
	struct nl_msg *msg = nlmsg_alloc();

	if (!msg || !genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, family, 0, 0,
							 cmd, CAN_ANALYZER_GENL_VERSION)) {
		fprintf(stderr, "error: out of memory\n");
		exit(1);
	}
	return msg;
}

static uint64_t get_u64(struct nlattr **tb, int attr)
{
	return tb[attr] ? nla_get_u64(tb[attr]) : 0;
}

static uint32_t get_u32(struct nlattr **tb, int attr)
{
	return tb[attr] ? nla_get_u32(tb[attr]) : 0;
}

/* Same names and layout as the top of /proc/can_analyzer/stats. */
static int stats_cb(struct nl_msg *msg, void *arg)
{
	struct nlattr *tb[CAN_ANALYZER_A_MAX + 1];
	uint32_t load, peak;

	(void)arg;
	if (genlmsg_parse(nlmsg_hdr(msg), 0, tb, CAN_ANALYZER_A_MAX, NULL) < 0)
		return NL_SKIP;
	load = get_u32(tb, CAN_ANALYZER_A_LOAD_X10);
	peak = get_u32(tb, CAN_ANALYZER_A_PEAK_LOAD_X10);

	printf("frames             %" PRIu64 "\n", get_u64(tb, CAN_ANALYZER_A_FRAMES));
	printf("filtered           %" PRIu64 "\n", get_u64(tb, CAN_ANALYZER_A_FILTERED));
	printf("dropped            %" PRIu64 "\n", get_u64(tb, CAN_ANALYZER_A_DROPPED));
	printf("unread             %" PRIu32 "\n", get_u32(tb, CAN_ANALYZER_A_UNREAD));
	printf("rate_fps           %" PRIu32 "\n", get_u32(tb, CAN_ANALYZER_A_RATE_FPS));
	printf("peak_fps           %" PRIu32 "\n", get_u32(tb, CAN_ANALYZER_A_PEAK_FPS));
	printf("bus_load_pct       %" PRIu32 ".%" PRIu32 "\n", load / 10, load % 10);
	printf("peak_bus_load_pct  %" PRIu32 ".%" PRIu32 "\n", peak / 10, peak % 10);
	printf("errors             %" PRIu64 "\n", get_u64(tb, CAN_ANALYZER_A_ERRORS));
	return NL_OK;
}

static int do_stats(void)
{
	struct nl_sock *sk;
	struct nl_msg *msg;
	int family, err;

	sk = open_family(&family);
	msg = new_msg(family, CAN_ANALYZER_CMD_GET_STATS);
	nl_socket_modify_cb(sk, NL_CB_VALID, NL_CB_CUSTOM, stats_cb, NULL);
	err = nl_send_auto(sk, msg);
	nlmsg_free(msg);
	if (err >= 0)
		err = nl_recvmsgs_default(sk);
	nl_socket_free(sk);
	if (err < 0) {
		fprintf(stderr, "error: %s\n", nl_geterror(err));
		return 1;
	}
	return 0;
}

/* Add or delete one rule and wait for the kernel's answer. */
static int do_rule(uint8_t cmd, uint32_t id, uint32_t mask, uint32_t action)
{
	struct nl_sock *sk;
	struct nl_msg *msg;
	int family, err;

	sk = open_family(&family);
	msg = new_msg(family, cmd);
	if (nla_put_u32(msg, CAN_ANALYZER_A_RULE_ID, id) < 0 ||
		nla_put_u32(msg, CAN_ANALYZER_A_RULE_MASK, mask) < 0 ||
		(action && nla_put_u32(msg, CAN_ANALYZER_A_RULE_ACTION, action) < 0)) {
		fprintf(stderr, "error: message too large\n");
		return 1;
	}
	err = nl_send_auto(sk, msg);
	nlmsg_free(msg);
	if (err >= 0)
		err = nl_wait_for_ack(sk);
	nl_socket_free(sk);
	if (err < 0) {
		fprintf(stderr, "error: %s\n", nl_geterror(err));
		return 1;
	}
	return 0;
}

static int event_cb(struct nl_msg *msg, void *arg)
{
	struct nlattr *tb[CAN_ANALYZER_A_MAX + 1];
	uint32_t cls;
	unsigned int i;
	const char *sep = "";

	(void)arg;
	if (genlmsg_hdr(nlmsg_hdr(msg))->cmd != CAN_ANALYZER_CMD_ERROR_EVENT)
		return NL_SKIP;
	if (genlmsg_parse(nlmsg_hdr(msg), 0, tb, CAN_ANALYZER_A_MAX, NULL) < 0)
		return NL_SKIP;

	cls = get_u32(tb, CAN_ANALYZER_A_ERR_CLASS);
	printf("t=%" PRIu64 " ctrl=0x%02" PRIx32 " class=",
		   get_u64(tb, CAN_ANALYZER_A_TIMESTAMP),
		   get_u32(tb, CAN_ANALYZER_A_CTRL_STATUS));
	for (i = 0; i < sizeof(err_names) / sizeof(err_names[0]); i++) {
		if (cls & (1U << i)) {
			printf("%s%s", sep, err_names[i]);
			sep = ",";
		}
	}
	printf("\n");
	fflush(stdout);
	return NL_OK;
}

/* Join the events multicast group and print every error frame. */
static int do_watch(void)
{
	struct nl_sock *sk;
	int family, group, err;

	sk = open_family(&family);
	group = genl_ctrl_resolve_grp(sk, CAN_ANALYZER_GENL_NAME,
								  CAN_ANALYZER_MCGRP_EVENTS);
	if (group < 0) {
		fprintf(stderr, "error: %s\n", nl_geterror(group));
		return 1;
	}
	nl_socket_disable_seq_check(sk);
	nl_socket_modify_cb(sk, NL_CB_VALID, NL_CB_CUSTOM, event_cb, NULL);
	err = nl_socket_add_membership(sk, group);
	if (err < 0) {
		fprintf(stderr, "error: %s\n", nl_geterror(err));
		return 1;
	}
	printf("watching CAN error frames, Ctrl+C to stop\n");
	fflush(stdout);
	for (;;) {
		err = nl_recvmsgs_default(sk);
		if (err < 0) {
			fprintf(stderr, "error: %s\n", nl_geterror(err));
			return 1;
		}
	}
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--stats"))
		return do_stats();
	if (argc == 2 && !strcmp(argv[1], "--watch-errors"))
		return do_watch();
	if (argc == 5 && !strcmp(argv[1], "--add-filter")) {
		uint32_t action;

		if (!strcmp(argv[4], "accept"))
			action = CAN_ANALYZER_ACCEPT;
		else if (!strcmp(argv[4], "drop"))
			action = CAN_ANALYZER_DROP;
		else
			usage();
		return do_rule(CAN_ANALYZER_CMD_ADD_RULE, parse_hex(argv[2], 1),
					   parse_hex(argv[3], 0), action);
	}
	if (argc == 4 && !strcmp(argv[1], "--del-filter"))
		return do_rule(CAN_ANALYZER_CMD_DEL_RULE, parse_hex(argv[2], 1),
					   parse_hex(argv[3], 0), 0);
	usage();
	return 2;
}
