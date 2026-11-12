// SPDX-License-Identifier: GPL-2.0
/*
 * can_latency.c: send frames on one CAN interface and time how long each
 * one takes to come out of /dev/can_analyzer, which listens on the other.
 *
 *   can_latency [count] [ifname]      default: 1000 frames, sent on can0
 *
 * Every time here is CLOCK_MONOTONIC, the same clock as the t= value the
 * module prints, so the three can be subtracted from each other.
 */
#include <fcntl.h>
#include <inttypes.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#define DEV		"/dev/can_analyzer"
#define TEST_ID		0x7a5
#define GAP_US		10000
#define WAIT_MS		1000

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Send only: an empty filter list keeps frames from piling up in this socket. */
static int open_can(const char *ifname)
{
	struct sockaddr_can addr = { .can_family = AF_CAN };
	int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);

	if (s < 0) {
		perror("socket");
		exit(1);
	}
	addr.can_ifindex = if_nametoindex(ifname);
	if (!addr.can_ifindex) {
		fprintf(stderr, "error: no interface %s\n", ifname);
		exit(1);
	}
	setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER, NULL, 0);
	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		exit(1);
	}
	return s;
}

/* Frames left over from earlier tests would be read as if they were ours. */
static void drain(int fd)
{
	char line[128];
	int flags = fcntl(fd, F_GETFL);

	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	while (read(fd, line, sizeof(line)) > 0)
		;
	fcntl(fd, F_SETFL, flags);
}

/* Pull t, id and the sequence number (first 4 data bytes) out of one line. */
static int parse(const char *line, uint64_t *t, unsigned int *id, uint32_t *seq)
{
	unsigned char b[4];
	const char *data;

	if (sscanf(line, "t=%" SCNu64 " id=0x%x", t, id) != 2)
		return -1;
	data = strstr(line, "data=");
	if (!data || sscanf(data + 5, "%hhx %hhx %hhx %hhx",
			    &b[0], &b[1], &b[2], &b[3]) != 4)
		return -1;
	*seq = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
	return 0;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static void report(const char *name, uint64_t *v, int n)
{
	qsort(v, n, sizeof(*v), cmp_u64);
	printf("%-20s %9.1f %9.1f %9.1f %9.1f\n", name, v[0] / 1000.0,
	       v[n / 2] / 1000.0, v[n * 99 / 100] / 1000.0, v[n - 1] / 1000.0);
}

int main(int argc, char **argv)
{
	int count = argc > 1 ? atoi(argv[1]) : 1000;
	const char *ifname = argc > 2 ? argv[2] : "can0";
	uint64_t *to_cb, *to_reader, *total;
	int s, dev, i, got = 0, lost = 0, stale = 0;

	if (count < 1 || count > 1000000) {
		fprintf(stderr, "usage: can_latency [count] [ifname]\n");
		return 2;
	}
	dev = open(DEV, O_RDONLY);
	if (dev < 0) {
		perror(DEV);
		return 1;
	}
	s = open_can(ifname);
	to_cb = calloc(count, sizeof(*to_cb));
	to_reader = calloc(count, sizeof(*to_reader));
	total = calloc(count, sizeof(*total));
	if (!to_cb || !to_reader || !total) {
		fprintf(stderr, "error: out of memory\n");
		return 1;
	}
	drain(dev);

	for (i = 0; i < count; i++) {
		struct can_frame cf = { .can_id = TEST_ID, .len = 8 };
		struct pollfd pfd = { .fd = dev, .events = POLLIN };
		uint32_t seq = i;
		uint64_t t0, t1, t2;

		memcpy(cf.data, &seq, sizeof(seq));
		t0 = now_ns();
		if (write(s, &cf, sizeof(cf)) != sizeof(cf)) {
			perror("write");
			return 1;
		}

		/* Read until our frame shows up, or give up after WAIT_MS. */
		for (;;) {
			char line[128];
			unsigned int id;
			uint32_t got_seq;
			ssize_t n;

			if (poll(&pfd, 1, WAIT_MS) <= 0) {
				lost++;
				break;
			}
			n = read(dev, line, sizeof(line) - 1);
			t2 = now_ns();
			if (n <= 0)
				continue;
			line[n] = '\0';
			if (parse(line, &t1, &id, &got_seq) || id != TEST_ID ||
			    got_seq != seq) {
				stale++;
				continue;
			}
			to_cb[got] = t1 - t0;
			to_reader[got] = t2 - t1;
			total[got] = t2 - t0;
			got++;
			break;
		}
		usleep(GAP_US);
	}

	printf("%d frames sent on %s, %d matched, %d lost, %d other lines\n",
	       count, ifname, got, lost, stale);
	if (!got)
		return 1;
	printf("%-20s %9s %9s %9s %9s   (us)\n", "", "min", "median", "p99", "max");
	report("send to callback", to_cb, got);
	report("callback to reader", to_reader, got);
	report("send to reader", total, got);
	return 0;
}
