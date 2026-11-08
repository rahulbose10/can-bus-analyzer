// SPDX-License-Identifier: GPL-2.0
/*
 * can_analyzer_filter.c: accept and drop rules on CAN IDs.
 * Exact rules live in a hash table, masked rules in a list.
 * The rx path reads both under RCU and never takes a lock.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/hashtable.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include "can_analyzer.h"

struct analyzer_rule {
	struct hlist_node hnode;
	struct list_head lnode;
	u32 id;
	u32 mask;
	u32 action;
	struct rcu_head rcu;
};

static DEFINE_HASHTABLE(exact_rules, 6);
static LIST_HEAD(masked_rules);
static DEFINE_SPINLOCK(rules_lock);
static unsigned int rule_count;

/* The bits that must all match for a rule to be exact. */
static u32 full_mask(u32 can_id)
{
	return (can_id & CAN_EFF_FLAG) ? (CAN_EFF_FLAG | CAN_EFF_MASK)
								   : (CAN_EFF_FLAG | CAN_SFF_MASK);
}

/*
 * CAN_EFF_FLAG is always compared, so a standard rule never matches an
 * extended frame and the other way round.
 */
static int normalize(const struct can_analyzer_rule *in, u32 *id, u32 *mask)
{
	u32 full = full_mask(in->can_id);

	if (in->can_id & ~full)
		return -EINVAL;
	*mask = (in->mask | CAN_EFF_FLAG) & full;
	*id = in->can_id & *mask;
	return 0;
}

/* Caller holds rules_lock. */
static struct analyzer_rule *find_rule(u32 id, u32 mask)
{
	struct analyzer_rule *r;

	if (mask == full_mask(id)) {
		hash_for_each_possible(exact_rules, r, hnode, id)
			if (r->id == id)
				return r;
		return NULL;
	}
	list_for_each_entry(r, &masked_rules, lnode)
		if (r->id == id && r->mask == mask)
			return r;
	return NULL;
}

/*
 * Softirq context, no lock: RCU keeps every rule we look at alive.
 * Exact rules win, then masked rules in the order they were added.
 * With no matching rule the frame is accepted.
 */
bool analyzer_filter_accept(canid_t can_id)
{
	u32 key = can_id & (CAN_EFF_FLAG | CAN_EFF_MASK);
	u32 action = CAN_ANALYZER_ACCEPT;
	struct analyzer_rule *r;

	rcu_read_lock();
	hash_for_each_possible_rcu(exact_rules, r, hnode, key) {
		if (r->id == key) {
			action = r->action;
			goto out;
		}
	}
	list_for_each_entry_rcu(r, &masked_rules, lnode) {
		if ((key & r->mask) == r->id) {
			action = r->action;
			goto out;
		}
	}
out:
	rcu_read_unlock();
	return action == CAN_ANALYZER_ACCEPT;
}

/* Plain spin_lock is enough: the rx path never takes rules_lock. */
int analyzer_filter_add(const struct can_analyzer_rule *in)
{
	struct analyzer_rule *r;
	u32 id, mask;
	int ret;

	if (in->action != CAN_ANALYZER_ACCEPT && in->action != CAN_ANALYZER_DROP)
		return -EINVAL;
	ret = normalize(in, &id, &mask);
	if (ret)
		return ret;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	r->id = id;
	r->mask = mask;
	r->action = in->action;

	spin_lock(&rules_lock);
	if (find_rule(id, mask)) {
		ret = -EEXIST;
	} else if (rule_count >= CAN_ANALYZER_MAX_RULES) {
		ret = -ENOSPC;
	} else {
		if (mask == full_mask(id))
			hash_add_rcu(exact_rules, &r->hnode, id);
		else
			list_add_tail_rcu(&r->lnode, &masked_rules);
		rule_count++;
		r = NULL;
	}
	spin_unlock(&rules_lock);

	kfree(r);
	return ret;
}

/* Unlink under the lock, free only after every RCU reader is done. */
int analyzer_filter_del(const struct can_analyzer_rule *in)
{
	struct analyzer_rule *r;
	u32 id, mask;
	int ret;

	ret = normalize(in, &id, &mask);
	if (ret)
		return ret;

	spin_lock(&rules_lock);
	r = find_rule(id, mask);
	if (r) {
		if (mask == full_mask(id))
			hash_del_rcu(&r->hnode);
		else
			list_del_rcu(&r->lnode);
		rule_count--;
	}
	spin_unlock(&rules_lock);

	if (!r)
		return -ENOENT;
	kfree_rcu(r, rcu);
	return 0;
}

static void fill(struct can_analyzer_rule *out, const struct analyzer_rule *r)
{
	out->can_id = r->id;
	out->mask = r->mask & ~CAN_EFF_FLAG;
	out->action = r->action;
}

void analyzer_filter_list(struct can_analyzer_rule_list *list)
{
	struct analyzer_rule *r;
	unsigned int bkt, n = 0;

	spin_lock(&rules_lock);
	hash_for_each(exact_rules, bkt, r, hnode)
		fill(&list->rules[n++], r);
	list_for_each_entry(r, &masked_rules, lnode)
		fill(&list->rules[n++], r);
	spin_unlock(&rules_lock);
	list->count = n;
}

/* rmmod, after unregister and synchronize_rcu: no reader and no ioctl left. */
void analyzer_filter_exit(void)
{
	struct analyzer_rule *r, *tmp;
	struct hlist_node *htmp;
	unsigned int bkt;

	hash_for_each_safe(exact_rules, bkt, htmp, r, hnode) {
		hash_del(&r->hnode);
		kfree(r);
	}
	list_for_each_entry_safe(r, tmp, &masked_rules, lnode) {
		list_del(&r->lnode);
		kfree(r);
	}
	rule_count = 0;
}
