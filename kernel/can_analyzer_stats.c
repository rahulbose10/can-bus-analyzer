// SPDX-License-Identifier: GPL-2.0
/*
 * can_analyzer_stats.c: frame and error counters, a once a second rate
 * from an hrtimer, and /proc/can_analyzer/stats.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/hashtable.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timekeeping.h>
#include <linux/can/error.h>
#include "can_analyzer.h"

/* Memory for per-ID counters is bounded: IDs past this are only summed. */
#define MAX_IDS 256

static unsigned int bitrate = 500000;
module_param(bitrate, uint, 0444);
MODULE_PARM_DESC(bitrate, "Bus bit rate for the load estimate (default 500000)");

struct id_stat {
	struct hlist_node node;
	u32 key;
	u64 count;
};

static DEFINE_HASHTABLE(id_stats, 8);
static DEFINE_SPINLOCK(id_lock);
static unsigned int id_count;
static u64 ids_untracked;

static atomic64_t frames_total = ATOMIC64_INIT(0);
static atomic64_t bits_total = ATOMIC64_INIT(0);
static atomic64_t errors_total = ATOMIC64_INIT(0);
static atomic_t err_class[10];
static u8 last_ctrl_status;

static const char * const err_names[10] = {
	"tx_timeout", "lost_arbitration", "controller", "protocol",
	"transceiver", "no_ack", "bus_off", "bus_error", "restarted",
	"counters",
};

/* Only the timer writes these. */
static u64 last_frames, last_bits;
static u32 rate_fps, peak_fps, load_x10, peak_load_x10;

static struct hrtimer rate_timer;
static struct proc_dir_entry *proc_dir;

/*
 * Bits on the wire for one frame plus the 3 bit gap after it.
 * Stuff bits are not counted, so the load is a lower bound.
 */
static u32 frame_bits(const struct can_frame *cf)
{
	u32 n = (cf->can_id & CAN_RTR_FLAG) ? 0 : min_t(u32, cf->len, CAN_MAX_DLEN);

	return ((cf->can_id & CAN_EFF_FLAG) ? 67 : 47) + 8 * n;
}

/* Caller holds id_lock. */
static struct id_stat *find_id(u32 key)
{
	struct id_stat *s;

	hash_for_each_possible(id_stats, s, node, key)
		if (s->key == key)
			return s;
	return NULL;
}

/* Softirq context, every data frame, before filtering. GFP_ATOMIC: no sleeping. */
void analyzer_stats_frame(const struct can_frame *cf)
{
	u32 key = cf->can_id & (CAN_EFF_FLAG | CAN_EFF_MASK);
	struct id_stat *s;

	atomic64_inc(&frames_total);
	atomic64_add(frame_bits(cf), &bits_total);

	spin_lock(&id_lock);
	s = find_id(key);
	if (!s && id_count < MAX_IDS) {
		s = kzalloc(sizeof(*s), GFP_ATOMIC);
		if (s) {
			s->key = key;
			hash_add(id_stats, &s->node, key);
			id_count++;
		}
	}
	if (s)
		s->count++;
	else
		ids_untracked++;
	spin_unlock(&id_lock);
}

/* Softirq context. Each set bit in can_id is one error class. */
void analyzer_stats_error(const struct can_frame *cf)
{
	u32 cls = cf->can_id & CAN_ERR_MASK;
	int i;

	atomic64_inc(&errors_total);
	for (i = 0; i < ARRAY_SIZE(err_class); i++)
		if (cls & BIT(i))
			atomic_inc(&err_class[i]);
	if (cls & CAN_ERR_CRTL)
		WRITE_ONCE(last_ctrl_status, cf->data[1]);
	analyzer_nl_error_event(cls, cf->data[1], ktime_get_ns());
}

/* hrtimer callback, hard interrupt context: arithmetic only, no locks. */
static enum hrtimer_restart rate_tick(struct hrtimer *t)
{
	u64 frames = atomic64_read(&frames_total);
	u64 bits = atomic64_read(&bits_total);
	u32 fps = frames - last_frames;
	u32 x10 = bitrate ? div_u64((bits - last_bits) * 1000, bitrate) : 0;

	last_frames = frames;
	last_bits = bits;
	WRITE_ONCE(rate_fps, fps);
	WRITE_ONCE(load_x10, x10);
	if (fps > peak_fps)
		WRITE_ONCE(peak_fps, fps);
	if (x10 > peak_load_x10)
		WRITE_ONCE(peak_load_x10, x10);

	hrtimer_forward_now(t, ms_to_ktime(1000));
	return HRTIMER_RESTART;
}

void analyzer_stats_snapshot(struct analyzer_stats_snapshot *st)
{
	long filtered, dropped;

	analyzer_rx_counters(&filtered, &dropped, &st->unread);
	st->frames = atomic64_read(&frames_total);
	st->filtered = filtered;
	st->dropped = dropped;
	st->errors = atomic64_read(&errors_total);
	st->rate_fps = READ_ONCE(rate_fps);
	st->peak_fps = READ_ONCE(peak_fps);
	st->load_x10 = READ_ONCE(load_x10);
	st->peak_load_x10 = READ_ONCE(peak_load_x10);
}

static int stats_show(struct seq_file *m, void *v)
{
	struct analyzer_stats_snapshot st;
	unsigned int bkt;
	struct id_stat *s;
	int i;

	analyzer_stats_snapshot(&st);

	seq_printf(m, "frames             %llu\n", st.frames);
	seq_printf(m, "filtered           %llu\n", st.filtered);
	seq_printf(m, "dropped            %llu\n", st.dropped);
	seq_printf(m, "unread             %u\n", st.unread);
	seq_printf(m, "rate_fps           %u\n", st.rate_fps);
	seq_printf(m, "peak_fps           %u\n", st.peak_fps);
	seq_printf(m, "bus_load_pct       %u.%u\n", st.load_x10 / 10, st.load_x10 % 10);
	seq_printf(m, "peak_bus_load_pct  %u.%u\n", st.peak_load_x10 / 10, st.peak_load_x10 % 10);
	seq_printf(m, "bitrate            %u\n", bitrate);
	seq_printf(m, "errors             %llu\n", st.errors);
	for (i = 0; i < ARRAY_SIZE(err_class); i++)
		seq_printf(m, "  %-17s%d\n", err_names[i], atomic_read(&err_class[i]));
	seq_printf(m, "last_ctrl_status   0x%02x\n", READ_ONCE(last_ctrl_status));

	spin_lock_bh(&id_lock);
	seq_printf(m, "ids                %u\n", id_count);
	hash_for_each(id_stats, bkt, s, node) {
		if (s->key & CAN_EFF_FLAG)
			seq_printf(m, "  %08x         %llu\n", s->key & CAN_EFF_MASK, s->count);
		else
			seq_printf(m, "  %03x              %llu\n", s->key, s->count);
	}
	seq_printf(m, "untracked          %llu\n", ids_untracked);
	spin_unlock_bh(&id_lock);
	return 0;
}

int analyzer_stats_init(void)
{
	proc_dir = proc_mkdir("can_analyzer", NULL);
	if (!proc_dir)
		return -ENOMEM;
	if (!proc_create_single("stats", 0444, proc_dir, stats_show)) {
		proc_remove(proc_dir);
		return -ENOMEM;
	}

	hrtimer_init(&rate_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	rate_timer.function = rate_tick;
	hrtimer_start(&rate_timer, ms_to_ktime(1000), HRTIMER_MODE_REL);
	return 0;
}

/* After the rx callbacks are unregistered. proc_remove waits for readers. */
void analyzer_stats_exit(void)
{
	struct hlist_node *tmp;
	struct id_stat *s;
	unsigned int bkt;

	hrtimer_cancel(&rate_timer);
	proc_remove(proc_dir);
	hash_for_each_safe(id_stats, bkt, tmp, s, node) {
		hash_del(&s->node);
		kfree(s);
	}
	id_count = 0;
}
