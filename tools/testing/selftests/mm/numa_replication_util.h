/* SPDX-License-Identifier: GPL-2.0 */
/* NUMA replication selftests: opt-in, node placement, per-node counters */
#ifndef __REPL_UTIL_H
#define __REPL_UTIL_H

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kselftest.h"

#ifndef MAP_REPL
#define MAP_REPL 0x200000
#endif

#define PAGE 4096UL
#define MAX_NODES 16

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

static inline long node_stat(int node, const char *name)
{
	char path[64], key[64];
	long val = -1, v;
	FILE *f;

	snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/vmstat",
		 node);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fscanf(f, "%63s %ld", key, &v) == 2)
		if (!strcmp(key, name))
			val = v;
	fclose(f);
	return val;
}

static inline void snap(long *v, const char *name, int nodes)
{
	int n;

	for (n = 0; n < nodes; n++)
		v[n] = node_stat(n, name);
}

#endif
