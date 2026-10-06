// SPDX-License-Identifier: GPL-2.0
/* NUMA replication opt-in: /proc/self/numa_repl */

#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "numa_replication_util.h"

#define SZ (2UL << 20)
#define WINDOW_SIZE (1UL << 39) /* one PGD entry, 4-level paging */
static unsigned long window;

/* end of the [repl] piece containing addr, 0 if none */
static unsigned long placeholder_at(unsigned long addr)
{
	unsigned long s, e, end = 0;
	char line[256];
	FILE *f = fopen("/proc/self/maps", "r");

	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%lx-%lx", &s, &e) == 2 && addr >= s &&
		    addr < e && strstr(line, "[repl]"))
			end = e;
	fclose(f);
	return end;
}

static void dump_maps(bool window_only)
{
	unsigned long s, e;
	char line[256];
	FILE *f = fopen("/proc/self/maps", "r");

	ksft_print_msg("[vma list (%s)]\n", window_only ? "window" : "all");
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%lx-%lx", &s, &e) == 2 &&
		    (!window_only || (e > window && s < window + WINDOW_SIZE)))
			ksft_print_msg("%s", line);
	fclose(f);
}

static void check_window_whole(int test)
{
	if (placeholder_at(window) == window + WINDOW_SIZE)
		return;
	dump_maps(true);
	ksft_exit_fail_msg("test %d left the window fragmented\n", test);
}

static void vma_result(int ok, const char *name)
{
	if (!ok)
		dump_maps(false);
	ksft_test_result(ok, "%s", name);
}

static unsigned long mmap_repl(size_t len)
{
	return (unsigned long)mmap(NULL, len, PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS | MAP_REPL, -1,
				   0);
}

static int in_window(unsigned long p)
{
	return p >= window && p < window + WINDOW_SIZE;
}

static void test_cannot_opt_out(void)
{
	ksft_test_result(repl_write("0") == -EPERM, "cannot opt out\n");
}

static void test_window_aligned(void)
{
	vma_result(window % WINDOW_SIZE == 0, "window is pgd aligned\n");
}

static void test_hint_lands_outside(void)
{
	void *p = mmap((void *)(window + 4096), 4096, PROT_READ,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	vma_result(p != MAP_FAILED && !in_window((unsigned long)p),
		   "hint in window lands outside\n");
	munmap(p, 4096);
}

static void test_fork(void)
{
	unsigned long a = mmap_repl(SZ);
	int status;
	pid_t pid = fork();

	if (pid == 0)
		_exit(placeholder_at(window) ||
		      msync((void *)a, SZ, MS_ASYNC) == 0);
	waitpid(pid, &status, 0);
	vma_result(WEXITSTATUS(status) == 0,
		   "fork child has no window and no MAP_REPL mapping\n");
	munmap((void *)a, SZ);
}

static void test_repl_contiguous(void)
{
	unsigned long a = mmap_repl(SZ), b = mmap_repl(SZ);

	vma_result(in_window(a) && b == a + SZ,
		   "MAP_REPL mappings are contiguous in window\n");
	munmap((void *)a, SZ);
	munmap((void *)b, SZ);
}

static void test_hole(void)
{
	unsigned long a = mmap_repl(SZ), b = mmap_repl(SZ), c = mmap_repl(SZ),
		      big, small;
	int refilled;

	munmap((void *)b, SZ);
	refilled = placeholder_at(b);
	big = mmap_repl(2 * SZ);
	small = mmap_repl(SZ / 2);
	vma_result(
		refilled && big != b && small == b &&
			placeholder_at(b + SZ / 2),
		"hole is refilled, skipped when too small, reused when it fits\n");
	munmap((void *)a, SZ);
	munmap((void *)c, SZ);
	munmap((void *)big, 2 * SZ);
	munmap((void *)small, SZ / 2);
}

static void test_holes_merge(void)
{
	unsigned long a = mmap_repl(SZ), b = mmap_repl(SZ), c = mmap_repl(SZ),
		      d = mmap_repl(SZ), e;

	munmap((void *)b, SZ);
	munmap((void *)c, SZ);
	e = mmap_repl(2 * SZ);
	vma_result(e == b, "two adjacent holes become one placeholder\n");
	munmap((void *)a, SZ);
	munmap((void *)d, SZ);
	munmap((void *)e, 2 * SZ);
}

static void test_mremap_refused(void)
{
	unsigned long a = mmap_repl(SZ);
	void *p = mremap((void *)a, SZ, 2 * SZ, MREMAP_MAYMOVE);

	vma_result(p == MAP_FAILED && errno == EFAULT && !placeholder_at(a),
		   "mremap of a window mapping is refused\n");
	munmap((void *)a, SZ);
}

static void test_eligible(void)
{
	int fd = test_file(SZ);
	unsigned long anon = mmap_repl(SZ);
	unsigned long shared = (unsigned long)mmap(
		NULL, SZ, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_REPL, fd, 0);
	unsigned long priv = (unsigned long)mmap(NULL, SZ,
						 PROT_READ | PROT_WRITE,
						 MAP_PRIVATE | MAP_REPL, fd, 0);

	vma_result(
		in_window(anon) && in_window(shared) && !in_window(priv),
		"private anon and shared file in window, private file out\n");
	munmap((void *)anon, SZ);
	munmap((void *)shared, SZ);
	munmap((void *)priv, SZ);
	close(fd);
}

static void (*tests[])(void) = {
	test_cannot_opt_out, test_window_aligned,  test_hint_lands_outside,
	test_fork,	     test_repl_contiguous, test_hole,
	test_holes_merge,    test_mremap_refused,  test_eligible,
};

int main(int argc, char **argv)
{
	unsigned long w;
	int i;

	repl_reexec(argv);
	ksft_print_header();
	for (w = 0; w < (128UL << 40); w += WINDOW_SIZE)
		if (placeholder_at(w))
			window = w;
	if (!window)
		ksft_exit_fail_msg("no [repl] in /proc/self/maps\n");

	ksft_set_plan(ARRAY_SIZE(tests));
	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		tests[i]();
		check_window_whole(i + 1);
	}
	ksft_finished();
}
