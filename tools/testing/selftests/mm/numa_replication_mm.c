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

/* pagemap entry of main's pte: bit 63 present, bits 0-54 pfn */
static unsigned long long pagemap_entry(char *p)
{
	unsigned long long e = 0;
	int fd = open("/proc/self/pagemap", O_RDONLY);

	if (fd >= 0) {
		pread(fd, &e, sizeof(e), (unsigned long)p / PAGE * sizeof(e));
		close(fd);
	}
	return e;
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

static int test_writeback(const struct backing *b, const char *name)
{
	char steps[64], *part, *save, val = 0, *p;
	struct statfs fs;
	int fd, ok = 1;

	if (!b->file)
		return -1;
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

/* main is unmapped while node 1's copy holds the last write */
static int test_pageout(const struct backing *b, const char *name)
{
	char val = 0, *p;
	int fd, ok;

	p = map(b, sz, &fd);
	ok = run_steps(p, "w0 r* w1", &val);
	run_on_node(0);
	madvise(p, sz, MADV_PAGEOUT);
	if (pagemap_entry(p) >> 63) {
		unmap(p, sz, fd);
		return -1; /* nothing paged out: no swap for anon or tmpfs */
	}
	print_stat(name);
	ok = ok && run_steps(p, "r* wL r*", &val);
	unmap(p, sz, fd);
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
	return ok;
}

int main(int argc, char **argv)
{
	unsigned int b, i;

	repl_test_init(argc, argv);
	ksft_set_plan(ARRAY_SIZE(backings) *
		      (4 + ARRAY_SIZE(writebacks) + ARRAY_SIZE(pwrites)));
	for (b = 0; b < ARRAY_SIZE(backings); b++) {
		for (i = 0; i < ARRAY_SIZE(writebacks); i++)
			run(test_writeback, &backings[b], writebacks[i]);
		for (i = 0; i < ARRAY_SIZE(pwrites); i++)
			run(test_pwrite, &backings[b], pwrites[i]);
		run(test_pread, &backings[b], "pread");
		run(test_truncate, &backings[b], "truncate");
		run(test_pageout, &backings[b], "pageout");
		run(test_ksm, &backings[b], "ksm");
	}
	ksft_finished();
}
