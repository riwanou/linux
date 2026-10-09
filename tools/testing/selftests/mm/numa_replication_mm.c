// SPDX-License-Identifier: GPL-2.0
/*
 * NUMA replication against mm paths that move or bypass main's page:
 * write() on a mapped file, truncation, pageout and compaction
 */

#include <sys/vfs.h>
#include <linux/magic.h>

#include "numa_replication_util.h"

/*
 * "|": fsync, then storage holds the last write
 * rL r1 w1 | w1 m1 | w0  copy synced, untouched main; writer refaults after
 *                        the clean; stale main migrates; main writes last
 * r0 mL wL | wL          L shares main's page and writes it, again after clean
 */
static const char *const writebacks[] = {
	"writeback: rL r1 w1 | w1 m1 | w0",
	"writeback: r0 mL wL | wL",
};

/*
 * pwrite sets byte 1 of every page behind the page tables: every node must
 * read it next to the last mmap write in byte 0
 * r*           main clean and up to date: copies only go stale
 * r* mL rL w1  L shares main, node 1's copy holds the last write: main is
 *              synced from it first
 */
static const char *const pwrites[] = {
	"pwrite: r*",
	"pwrite: r* mL rL w1",
};

/*
 * main clean in the swap cache, written through a sharer: only the sharer's
 * pte is dirty, and main must still reach swap at the next pageout
 * r1 w1     the sharer goes at the pageout
 * r1 w1 m0  the migration re-points the sharer first
 */
static const char *const sharer_writes[] = {
	"sharer write: r1 w1",
	"sharer write: r1 w1 m0",
};

/* pagemap bit 63: main's pte maps a page in memory */
static int main_resident(char *p)
{
	unsigned long long e = 0;
	int fd = open("/proc/self/pagemap", O_RDONLY);

	if (fd >= 0) {
		pread(fd, &e, sizeof(e), (unsigned long)p / PAGE * sizeof(e));
		close(fd);
	}
	return e >> 63;
}

/* storage, read with O_DIRECT behind the page cache, holds @want */
static int disk_holds(int fd, char want)
{
	char path[32], *buf;
	unsigned long i;
	int dfd, ok = 1;

	snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
	dfd = open(path, O_RDONLY | O_DIRECT);
	if (dfd < 0 || posix_memalign((void **)&buf, PAGE, PAGE))
		return 0;
	for (i = 0; ok && i < sz; i += PAGE) {
		ok = pread(dfd, buf, PAGE, i) == PAGE && buf[0] == want;
		if (!ok)
			ksft_print_msg("disk page %lu: got %d want %d\n",
				       i / PAGE, buf[0], want);
	}
	free(buf);
	close(dfd);
	return ok;
}

/* @mb of clean page cache on node 0, newer on the LRU than what came before */
static int page_cache_filler(unsigned long mb)
{
	static char buf[1 << 16] = { 1 };
	int fd = test_file(mb << 20);
	unsigned long i;

	run_on_node(0);
	for (i = 0; i < mb << 20; i += sizeof(buf))
		if (write(fd, buf, sizeof(buf)) != sizeof(buf))
			break;
	fsync(fd);
	return fd;
}

static long read_vmstat(const char *name)
{
	FILE *f = fopen("/proc/vmstat", "r");
	char key[64];
	long v = -1;

	while (f && fscanf(f, "%63s %ld", key, &v) == 2 && strcmp(key, name))
		v = -1;
	if (f)
		fclose(f);
	return v;
}

/* every page isolated to move went back or moved: nr_isolated_* never < 0, back to 0 */
static int isolated_balanced(int kmsg)
{
	long anon, file;
	char rec[1024];
	ssize_t len;
	int ok = 1;

	write_file("/proc/sys/vm/stat_refresh", "1");
	while ((len = read(kmsg, rec, sizeof(rec) - 1)) > 0) {
		rec[len] = 0;
		if (!strstr(rec, "vmstat_refresh: nr_isolated"))
			continue;
		ksft_print_msg("%s", strchr(rec, ';') + 1);
		ok = 0;
	}
	anon = read_vmstat("nr_isolated_anon");
	file = read_vmstat("nr_isolated_file");
	ksft_print_msg("nr_isolated anon %ld file %ld\n", anon, file);
	return ok && !anon && !file;
}

/* every write to storage done: a swap slot holds what was paged out */
static void wait_writeback(void)
{
	int i;

	for (i = 0; i < 1000; i++) {
		write_file("/proc/sys/vm/stat_refresh", "1");
		if (!read_vmstat("nr_writeback"))
			return;
		usleep(1000);
	}
}

static int test_writeback(const struct backing *b, const char *name)
{
	char steps[64], *part, *save, val = 0, *p;
	struct statfs fs;
	int fd, ok = 1;

	if (b->file != 1 || !getenv("REPL_TEST_DIR"))
		return -1; /* needs a disk fs: the scripts mount ext4 */
	p = map(b, sz, &fd);
	if (fstatfs(fd, &fs) || fs.f_type == TMPFS_MAGIC) {
		unmap(p, sz, fd);
		return -1; /* tmpfs never writes back */
	}

	snprintf(steps, sizeof(steps), "%s", strchr(name, ':') + 1);
	for (part = strtok_r(steps, "|", &save); ok && part;
	     part = strtok_r(NULL, "|", &save)) {
		ok = run_steps(p, part, &val) && !fsync(fd) &&
		     disk_holds(fd, val);
		if (ok)
			print_stat("fsync");
	}

	unmap(p, sz, fd);
	return ok;
}

/* write() into main's page while node 1's copy holds the last mmap write */
static int test_pwrite(const struct backing *b, const char *name)
{
	char val = 0, c = 100, *p;
	unsigned long i;
	int fd, n, ok;

	if (!b->file)
		return -1;
	p = map(b, sz, &fd);

	ok = run_steps(p, strchr(name, ':') + 1, &val);
	for (i = 0; ok && i < sz; i += PAGE) {
		if (pwrite(fd, &c, 1, i + 1) == 1)
			continue;
		ksft_print_msg("pwrite page %lu: %s\n", i / PAGE,
			       strerror(errno));
		ok = 0;
	}
	print_stat("pwrite");
	for (n = 0; ok && n < nodes; n++)
		ok = read_all(p, 0, n, val, "w1 before pwrite") &&
		     read_all(p, 1, n, c, "pwrite");

	unmap(p, sz, fd);
	return ok;
}

/* read() copies main's folio out while node 1's copy holds the last write */
static int test_pread(const struct backing *b, const char *name)
{
	char val = 0, got = 0, *p;
	unsigned long i;
	int fd, ok;

	if (!b->file)
		return -1;
	p = map(b, sz, &fd);

	ok = run_steps(p, "w0 r* w1", &val);
	for (i = 0; ok && i < sz; i += PAGE) {
		if (pread(fd, &got, 1, i) == 1 && got == val)
			continue;
		ksft_print_msg("pread page %lu: got %d want %d\n", i / PAGE,
			       got, val);
		ok = 0;
	}
	print_stat(name);
	ok = ok && run_steps(p, "r* w2 r*", &val);

	unmap(p, sz, fd);
	return ok;
}

/* main is reclaimed while node 1's replica holds the last write */
static int test_pageout(const struct backing *b, const char *name)
{
	char val = 0, *p;
	int fd, ok;

	p = map(b, sz, &fd);
	ok = run_steps(p, "w0 r* w1", &val);
	msync(p, sz, MS_SYNC);
	run_on_node(0);
	madvise(p, sz, MADV_PAGEOUT);
	if (main_resident(p)) {
		unmap(p, sz, fd);
		return -1; /* nothing paged out: anon needs swap */
	}
	print_stat(name);
	ok = ok && run_steps(p, "r* wL r*", &val);
	unmap(p, sz, fd);
	return ok;
}

static int test_sharer_write(const struct backing *b, const char *name)
{
	char val = 0, *p;
	int fd, ok;

	if (b->file)
		return -1; /* a file or shm main is dirtied on every write */
	set_placement("first_touch");
	p = map(b, sz, &fd);
	ok = run_steps(p, "w1", &val);
	madvise(p, sz, MADV_PAGEOUT);
	if (main_resident(p)) {
		unmap(p, sz, fd);
		set_placement("bound");
		return -1; /* nothing paged out: anon needs swap */
	}
	wait_writeback();
	ok = ok && run_steps(p, strchr(name, ':') + 1, &val);
	madvise(p, sz, MADV_PAGEOUT);
	ok = ok && run_steps(p, "r*", &val);
	unmap(p, sz, fd);
	set_placement("bound");
	return ok;
}

/* truncation zaps main and every copy through the mapping, not munmap */
static int test_truncate(const struct backing *b, const char *name)
{
	char val = 0, *p;
	int fd, ok;

	if (!b->file)
		return -1;
	p = map(b, sz, &fd);
	ok = run_steps(p, "w0 r* w1", &val);
	ok = ok && !ftruncate(fd, 0) && !ftruncate(fd, sz);
	val = 0;
	print_stat(name);
	ok = ok && run_steps(p, "r* w1 r*", &val);
	unmap(p, sz, fd);
	return ok;
}

/* KSM never merges window pages, even identical ones */
static int test_ksm(const struct backing *b, const char *name)
{
	char val = 0, *p;
	unsigned long merged;
	int fd, ok;

	if (b->file)
		return -1;
	p = map(b, sz, &fd);
	ok = run_steps(p, "w0 r*", &val) && !madvise(p, sz, MADV_MERGEABLE);
	write_file("/sys/kernel/mm/ksm/pages_to_scan", "100000");
	write_file("/sys/kernel/mm/ksm/run", "1");
	sleep(1);
	write_file("/sys/kernel/mm/ksm/run", "0");
	print_stat(name);
	merged = read_long("/proc/self/ksm_merging_pages");
	if (merged)
		ksft_print_msg("ksm merged %ld window pages\n", merged);
	ok = ok && !merged;
	ok = ok && run_steps(p, "r* w1 r*", &val);
	unmap(p, sz, fd);
	write_file("/sys/kernel/mm/ksm/run", "2");
	return ok;
}

/* reclaim checks main once: kept while replicas are read, evicted when not */
static int test_aging(const struct backing *b, const char *name)
{
	char val = 0, *p;
	int fd, filler, ok, kept;
	unsigned long saved = sz;

	sz = 16 * PAGE;
	if (b->file != 1 || !getenv("REPL_TEST_DIR"))
		return -1; /* needs a disk fs: the scripts mount ext4 */

	sync();
	write_file("/proc/sys/vm/drop_caches", "1");
	p = map(b, sz, &fd);
	ok = run_steps(p, "w0 r*", &val);
	msync(p, sz, MS_SYNC);
	run_on_node(0);
	madvise(p, sz, MADV_COLD);
	print_stat("main cold");
	filler = page_cache_filler(256);

	reclaim_node(0, "64M swappiness=0");
	kept = main_resident(p);
	print_stat("aging: kept");

	reclaim_node(0, "1G swappiness=0");
	print_stat(name);

	ok = ok && kept && !main_resident(p) && run_steps(p, "r* w1 r*", &val);
	close(filler);
	unmap(p, sz, fd);

	sz = saved;
	return ok;
}

/* memcg reclaim: replicas are charged to the process's cgroup and go with it */
static int test_memcg(const struct backing *b, const char *name)
{
	char val = 0, *p;
	int fd, ok;

	if (!join_cgroup("repl"))
		return -1; /* no cgroup v2 */
	p = map(b, sz, &fd);
	ok = run_steps(p, "w0 r* w1", &val);
	reclaim("/sys/fs/cgroup/repl/memory.reclaim", "1G");
	print_stat(name);
	ok = ok && run_steps(p, "r* w2 r*", &val);
	unmap(p, sz, fd);
	join_cgroup(NULL);
	rmdir("/sys/fs/cgroup/repl");
	return ok;
}

/* pages whose main is in memory on another node than 0 */
static unsigned long away(char *p, unsigned long n)
{
	void **pages = malloc(n * sizeof(*pages));
	int *status = malloc(n * sizeof(*status));
	unsigned long i, off = n;

	for (i = 0; i < n; i++)
		pages[i] = p + i * PAGE;
	if (!syscall(SYS_move_pages, 0, n, pages, NULL, status, 0))
		for (i = 0, off = 0; i < n; i++)
			off += status[i] > 0;
	free(pages);
	free(status);
	return off;
}

/*
 * dynamic: reclaim moves a referenced main to its interleave home; restore
 * brings it back, by the sysfs file or krepld, and the nodes that shared it
 * copy it again
 */
static int test_dynamic(const struct backing *b, const char *name)
{
	const char *interval =
		"/sys/kernel/mm/numa_replication/restore_interval";
	unsigned long i, n = sz / PAGE, moved;
	int fd, filler, install, kmsg, ok;
	char val = 0, *p, pid[16];

	if (!getenv("REPL_TEST_DIR") || n < 2)
		return -1; /* swap and a disk fs from the scripts, a page off node 0 */

	kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	lseek(kmsg, 0, SEEK_END);
	sync();
	write_file("/proc/sys/vm/drop_caches", "1");
	write_file(interval, "0");
	set_placement("dynamic");

	p = map(b, sz, &fd);
	ok = run_steps(p, "w0 r*", &val);
	msync(p, sz, MS_SYNC);

	run_on_node(0);
	madvise(p, sz, MADV_COLD);
	filler = page_cache_filler(256);
	reclaim_node(0,
		     b->file == 1 ? "64M swappiness=0" : "64M swappiness=max");
	close(filler);
	print_stat("reclaimed");
	moved = away(p, n);
	ok = ok && moved && run_steps(p, "r*", &val);

	if (!strcmp(name, "restore")) {
		snprintf(pid, sizeof(pid), "%d", getpid());
		write_file("/sys/kernel/mm/numa_replication/restore", pid);
	} else if (!strcmp(name, "restore: krepld")) {
		write_file(interval, "1");
		for (i = 0; i < 30 && away(p, n); i++)
			usleep(100000);
	}
	if (strcmp(name, "dynamic")) {
		print_stat("restored");
		install = event_open("repl_install");
		ok = ok && !away(p, n) && run_steps(p, "r*", &val);
		ok = event_count(install) == moved && ok;
	}

	ok = ok && run_steps(p, "w1 r*", &val);
	unmap(p, sz, fd);
	ok = isolated_balanced(kmsg) && ok;
	close(kmsg);
	set_placement("bound");
	write_file(interval, "1");
	return ok;
}

int main(int argc, char **argv)
{
	unsigned int b, i;

	repl_test_init(argc, argv);
	ksft_set_plan(ARRAY_SIZE(backings) *
		      (9 + ARRAY_SIZE(writebacks) + ARRAY_SIZE(pwrites) +
		       ARRAY_SIZE(sharer_writes)));
	for (b = 0; b < ARRAY_SIZE(backings); b++) {
		for (i = 0; i < ARRAY_SIZE(writebacks); i++)
			run(test_writeback, &backings[b], writebacks[i]);
		for (i = 0; i < ARRAY_SIZE(pwrites); i++)
			run(test_pwrite, &backings[b], pwrites[i]);
		run(test_pread, &backings[b], "pread");
		run(test_truncate, &backings[b], "truncate");
		run(test_pageout, &backings[b], "pageout");
		for (i = 0; i < ARRAY_SIZE(sharer_writes); i++)
			run(test_sharer_write, &backings[b], sharer_writes[i]);
		run(test_ksm, &backings[b], "ksm");
		run(test_aging, &backings[b], "aging");
		run(test_memcg, &backings[b], "memcg reclaim");
		run(test_dynamic, &backings[b], "dynamic");
		run(test_dynamic, &backings[b], "restore");
		run(test_dynamic, &backings[b], "restore: krepld");
	}
	ksft_finished();
}
