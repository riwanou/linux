/* SPDX-License-Identifier: GPL-2.0 */
/* NUMA replication selftests: opt-in, nodes, and the data test helpers */
#ifndef __NUMA_REPLICATION_UTIL_H
#define __NUMA_REPLICATION_UTIL_H

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <linux/perf_event.h>

#include "kselftest.h"

#ifndef MPOL_MF_MOVE
#define MPOL_MF_MOVE (1 << 1)
#endif

#ifndef MAP_REPL
#define MAP_REPL 0x200000
#endif

#define PAGE 4096UL
#define MAX_NODES 16

static unsigned long sz __maybe_unused = 64UL << 20;
static const char *filter __maybe_unused;
static int nodes __maybe_unused;

struct backing {
	const char *name;
	int file;
};

static const struct backing backings[] = {
	{ "anon", 0 },
	{ "file", 1 },
	{ "shm", 2 },
};

static inline int repl_write(const char *v)
{
	int fd = open("/proc/self/numa_repl", O_WRONLY);
	int ret;

	if (fd < 0)
		return -errno;
	ret = write(fd, v, strlen(v)) < 0 ? -errno : 0;
	close(fd);
	return ret;
}

static inline int node_cpu(int node)
{
	char path[64];
	int cpu = -1;
	FILE *f;

	snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/cpulist",
		 node);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (fscanf(f, "%d", &cpu) != 1)
		cpu = -1;
	fclose(f);
	return cpu;
}

static inline int repl_nodes(void)
{
	int n = 0;

	while (n < MAX_NODES && node_cpu(n) >= 0)
		n++;
	return n;
}

static inline void run_on_node(int node)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(node_cpu(node), &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		ksft_exit_fail_msg("sched_setaffinity node %d: %s\n", node,
				   strerror(errno));
}

/* opt in, then exec ourselves from node 0: only a new mm is replicated */
static inline void repl_reexec(char **argv)
{
	if (getenv("NUMA_REPL_EXECED"))
		return;
	if (repl_write("1"))
		ksft_exit_skip("NUMA replication not available\n");
	setenv("NUMA_REPL_EXECED", "1", 1);
	run_on_node(0);
	execv("/proc/self/exe", argv);
	ksft_exit_fail_msg("execv: %s\n", strerror(errno));
}

static inline int test_file(size_t len)
{
	const char *dir = getenv("REPL_TEST_DIR") ?: "/tmp";
	int fd = open(dir, O_TMPFILE | O_RDWR, 0600);

	if (fd < 0 || ftruncate(fd, len))
		ksft_exit_fail_msg("O_TMPFILE in %s: %s\n", dir,
				   strerror(errno));
	return fd;
}

static inline void write_file(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);

	if (fd < 0 || write(fd, val, strlen(val)) < 0)
		ksft_exit_fail_msg("%s: %s\n", path, strerror(errno));
	close(fd);
}

static inline long read_long(const char *path)
{
	long v = -1;
	FILE *f = fopen(path, "r");

	if (f) {
		if (fscanf(f, "%ld", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

/* count a numa_replication event in this process and its future threads */
static inline int event_open(const char *name)
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_TRACEPOINT,
		.size = sizeof(attr),
		.inherit = 1,
	};
	char path[128];

	snprintf(path, sizeof(path),
		 "/sys/kernel/tracing/events/numa_replication/%s/id", name);
	attr.config = read_long(path);
	return syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);
}

static inline long event_count(int fd)
{
	long long v = -1;

	if (fd >= 0 && read(fd, &v, sizeof(v)) != sizeof(v))
		v = -1;
	if (fd >= 0)
		close(fd);
	return v;
}

static inline void set_placement(const char *mode)
{
	write_file("/sys/kernel/mm/numa_replication/main_placement", mode);
}

static inline char *map(const struct backing *b, unsigned long len, int *fd)
{
	int flags = b->file ? MAP_SHARED : MAP_PRIVATE | MAP_ANONYMOUS;
	char *p;

	*fd = b->file == 2 ? memfd_create("repl", 0) :
	      b->file	   ? test_file(len) :
			     -1;
	if (b->file == 2 && ftruncate(*fd, len))
		ksft_exit_fail_msg("memfd: %s\n", strerror(errno));
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, flags | MAP_REPL, *fd, 0);
	if (p == MAP_FAILED)
		ksft_exit_fail_msg("mmap %s: %s\n", b->name, strerror(errno));
	return p;
}

static inline void unmap(char *p, unsigned long len, int fd)
{
	munmap(p, len);
	if (fd >= 0)
		close(fd);
}

static inline int read_all(char *p, unsigned long off, int n, char want,
			   const char *what)
{
	unsigned long i;

	run_on_node(n);
	for (i = 0; i < sz; i += PAGE) {
		if (p[i + off] == want)
			continue;
		ksft_print_msg(
			"%s: node %d page %lu byte %lu: got %d want %d\n", what,
			n, i / PAGE, off, p[i + off], want);
		return 0;
	}
	return 1;
}

static inline void write_all(char *p, int n, char val)
{
	unsigned long i;

	run_on_node(n);
	for (i = 0; i < sz; i += PAGE)
		p[i] = val;
}

static inline void print_stat(const char *when)
{
	char line[256];
	FILE *f;

	if (strcmp(getenv("REPL_STAT") ?: "", "1"))
		return;
	f = fopen("/proc/self/numa_repl_stat", "r");
	if (!f)
		return;
	if (fgets(line, sizeof(line), f))
		ksft_print_msg("[%s] %s", when, line);
	while (fgets(line, sizeof(line), f))
		ksft_print_msg("%s", line);
	fclose(f);
}

/* move main's pages in [p, p + len) to @node; returns how many got there */
static inline unsigned long migrate_main(char *p, unsigned long len, int node)
{
	unsigned long i, n = len / PAGE, moved = 0;
	void **pages = malloc(n * sizeof(*pages));
	int *to = malloc(n * sizeof(*to));
	int *status = malloc(n * sizeof(*status));

	for (i = 0; i < n; i++) {
		pages[i] = p + i * PAGE;
		to[i] = node;
	}
	syscall(SYS_move_pages, 0, n, pages, to, status, MPOL_MF_MOVE);
	if (!syscall(SYS_move_pages, 0, n, pages, NULL, status, 0))
		for (i = 0; i < n; i++)
			moved += status[i] == node;
	free(pages);
	free(to);
	free(status);
	return moved;
}

/* proactive reclaim through @path: @what is "<size> [swappiness=<n>]" */
static inline void reclaim(const char *path, const char *what)
{
	int fd;

	/* node reclaim skips nodes with little page cache unless told not to */
	write_file("/proc/sys/vm/min_unmapped_ratio", "0");
	write_file("/proc/sys/vm/min_slab_ratio", "0");
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return;
	if (write(fd, what, strlen(what)) < 0 && errno != EAGAIN)
		ksft_print_msg("%s: %s\n", path, strerror(errno));
	close(fd);
}

static inline void reclaim_node(int node, const char *what)
{
	char path[64];

	snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/reclaim",
		 node);
	reclaim(path, what);
}

/* move the whole process into cgroup @name, or back to the root if NULL */
static inline int join_cgroup(const char *name)
{
	char path[128], pid[16];

	if (name) {
		write_file("/sys/fs/cgroup/cgroup.subtree_control", "+memory");
		snprintf(path, sizeof(path), "/sys/fs/cgroup/%s", name);
		mkdir(path, 0755);
	} else {
		snprintf(path, sizeof(path), "/sys/fs/cgroup");
	}
	strcat(path, "/cgroup.procs");
	snprintf(pid, sizeof(pid), "%d", getpid());
	return access(path, W_OK) == 0 && (write_file(path, pid), 1);
}

/*
 * "w1": node 1 writes, "rL": the last node reads, "r*": every node reads,
 * "m1": main's pages move to node 1
 * "x1": node 1's memory is reclaimed
 */
static inline int run_steps(char *p, const char *steps, char *val)
{
	char buf[64], *tok;
	int n, from, to;

	snprintf(buf, sizeof(buf), "%s", steps);
	for (tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
		from = tok[1] == 'L' ? nodes - 1 : tok[1] - '0';
		if (tok[1] == '*')
			from = 0;
		to = tok[1] == '*' ? nodes : from + 1;
		for (n = from; n < to; n++) {
			if (tok[0] == 'w')
				write_all(p, n, ++*val);
			else if (tok[0] == 'r' && !read_all(p, 0, n, *val, tok))
				return 0;
			else if (tok[0] == 'm' &&
				 migrate_main(p, sz, n) != sz / PAGE) {
				ksft_print_msg("main did not move to node %d\n",
					       n);
				return 0;
			} else if (tok[0] == 'x')
				reclaim_node(n, "1G");
		}
		print_stat(tok);
	}
	return 1;
}

/* one TAP result "<backing>: <name>"; @fn returns 1 pass, 0 fail, -1 skip */
static inline void run(int (*fn)(const struct backing *, const char *),
		       const struct backing *b, const char *name)
{
	char full[96];
	int ret = -1;

	snprintf(full, sizeof(full), "%s: %s", b->name, name);
	if (!filter || !strcmp(full, filter))
		ret = fn(b, name);
	if (ret < 0)
		ksft_test_result_skip("%s\n", full);
	else
		ksft_test_result(ret, "%s\n", full);
}

static inline void repl_test_init(int argc, char **argv)
{
	repl_reexec(argv);
	nodes = repl_nodes();
	if (getenv("REPL_SZ"))
		sz = strtoul(getenv("REPL_SZ"), NULL, 0) / PAGE * PAGE ?: PAGE;
	filter = argc > 1 ? argv[1] : NULL;
	ksft_print_header();
	if (nodes < 2)
		ksft_exit_skip("needs at least 2 NUMA nodes\n");
}

#endif
