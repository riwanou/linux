// SPDX-License-Identifier: GPL-2.0
/*
 * NUMA replication fault path: every node reads the last write, whoever
 * made it, alone or with many threads.
 */

#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include "numa_replication_util.h"

#define PER_NODE 2
#define MAX_THREADS (MAX_NODES * PER_NODE)
#define STRESS_PAGES 16
#define STRESS_ITERS 20000

static pthread_barrier_t start;
static char *shared;
static int threads, stop;
static int final[MAX_THREADS][STRESS_PAGES];

/*
 * Steps: "w1" node 1 writes, "rL" the last node reads, "r*" every node reads.
 * Node 0 holds the main tree. Every read must see the last write, and for a
 * file the page cache must hold it after munmap.
 *
 * w0 r*                 main first: a copy on every other node
 * r* w1 r* wL wL w0 r*  read-only first touch, then the writer moves
 *                       replica -> replica -> main; a repeated write
 *                       does not fault
 * wL r1 w1 rL w0 r*     a replica first: main's page lands on L, which
 *                       shares it; w1 makes both stale at once; w0 finds
 *                       the shared page already refreshed by L
 * rL r1 w1 rL wL r1 w1  node 0 never touches the page; the last write
 *                       lives only in node 1's copy until munmap syncs it
 *
 * "m1" moves main's pages to node 1, its tree stays on node 0:
 * r0 mL wL m0 r* w1 r*  main moves alone, L shares it and writes through
 *                       it, main moves off: the sharer goes
 * r* w0 m1 w0 r* w1 r*  writable main lands on node 1's stale copy, which
 *                       goes; node 1 then shares main and writes through it
 * w0 r* wL m1 r* w0 r*  stale main refreshed from L, lands on node 1
 */
static const char *const protocols[] = {
	"w0 r*",
	"r* w1 r* wL wL w0 r*",
	"wL r1 w1 rL w0 r*",
	"rL r1 w1 rL wL r1 w1",
	/* with migration */
	"r0 mL wL m0 r* w1 r*",
	"r* w0 m1 w0 r* w1 r*",
	"w0 r* wL m1 r* w0 r*",
};

static int file_holds(int fd, char want)
{
	unsigned long i;
	char got;

	for (i = 0; i < sz; i += PAGE) {
		if (pread(fd, &got, 1, i) == 1 && got == want)
			continue;
		ksft_print_msg("file page %lu: got %d want %d\n", i / PAGE, got,
			       want);
		return 0;
	}
	return 1;
}

static int test_protocol(const struct backing *b, const char *steps)
{
	char val = 0, *p;
	int fd, ok;

	p = map(b, sz, &fd);
	ok = run_steps(p, steps, &val);
	munmap(p, sz);
	if (fd >= 0) {
		ok = ok && file_holds(fd, val);
		close(fd);
	}
	return ok;
}

/* thread @t owns int @t of every stress page, the others only read it */
static int slot_get(int page, int t)
{
	return __atomic_load_n((int *)(shared + page * PAGE) + t,
			       __ATOMIC_RELAXED);
}

static void slot_set(int page, int t, int v)
{
	__atomic_store_n((int *)(shared + page * PAGE) + t, v,
			 __ATOMIC_RELAXED);
}

static int alloc_check(void)
{
	char *q = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_REPL, -1, 0);
	int ok;

	if (q == MAP_FAILED)
		return 0;
	ok = !q[0];
	q[0] = 1;
	ok &= q[0] == 1;
	munmap(q, 4 * PAGE);
	return ok;
}

static void *stresser(void *arg)
{
	int t = (long)arg, last[STRESS_PAGES][MAX_THREADS] = { 0 };
	unsigned int seed = t;
	int it, page, s, v;

	run_on_node(t % nodes);
	pthread_barrier_wait(&start);
	for (it = 0; it < STRESS_ITERS; it++) {
		if (!(it % 256) && !alloc_check()) {
			ksft_print_msg("thread %d: fresh region broken\n", t);
			return NULL;
		}
		page = rand_r(&seed) % STRESS_PAGES;
		if (!(rand_r(&seed) % 4)) {
			slot_set(page, t, ++final[t][page]);
			continue;
		}
		/* one writer per slot: a value going back is a stale copy */
		for (s = 0; s < threads; s++) {
			v = slot_get(page, s);
			if (v >= last[page][s]) {
				last[page][s] = v;
				continue;
			}
			ksft_print_msg(
				"thread %d page %d slot %d: %d after %d\n", t,
				page, s, v, last[page][s]);
			return NULL;
		}
	}
	return (void *)1;
}

/* main's pages tour the nodes while the stressers fault on them */
static void *migrator(void *arg)
{
	unsigned long n, moved = 0;

	for (n = 0; !__atomic_load_n(&stop, __ATOMIC_RELAXED); n++)
		moved += migrate_main(shared, STRESS_PAGES * PAGE, n % nodes);
	ksft_print_msg("%lu pages migrated\n", moved);
	return (void *)moved;
}

static int stress(const struct backing *b, bool migrate)
{
	pthread_t th[MAX_THREADS], mig;
	int t, n, page, fd, ok = 1;
	void *ret;

	threads = nodes * PER_NODE;
	memset(final, 0, sizeof(final));
	shared = map(b, STRESS_PAGES * PAGE, &fd);
	stop = 0;
	pthread_barrier_init(&start, NULL, threads);
	for (t = 0; t < threads; t++)
		pthread_create(&th[t], NULL, stresser, (void *)(long)t);
	if (migrate)
		pthread_create(&mig, NULL, migrator, NULL);
	for (t = 0; t < threads; t++) {
		pthread_join(th[t], &ret);
		ok &= ret != NULL;
	}
	if (migrate) {
		__atomic_store_n(&stop, 1, __ATOMIC_RELAXED);
		pthread_join(mig, &ret);
		ok &= ret != NULL;
	}
	pthread_barrier_destroy(&start);

	for (n = 0; ok && n < nodes; n++) {
		run_on_node(n);
		for (page = 0; page < STRESS_PAGES; page++)
			for (t = 0; t < threads; t++) {
				if (slot_get(page, t) == final[t][page])
					continue;
				ksft_print_msg(
					"node %d page %d slot %d: %d, want %d\n",
					n, page, t, slot_get(page, t),
					final[t][page]);
				ok = 0;
				goto out;
			}
	}
out:
	unmap(shared, STRESS_PAGES * PAGE, fd);
	return ok;
}

static int test_stress(const struct backing *b, const char *name)
{
	return stress(b, false);
}

static int test_stress_migrate(const struct backing *b, const char *name)
{
	return stress(b, true);
}

/* node 0 holds main, node L touches first */
static int want_node(const struct backing *b, const char *mode, char *page,
		     unsigned long i)
{
	if (!strcmp(mode, "bound"))
		return 0;
	if (!strcmp(mode, "first_touch"))
		return nodes - 1;
	return (b->file ? i : (unsigned long)page / PAGE) % nodes;
}

static int test_main_placement(const struct backing *b, const char *mode)
{
	unsigned long i, n = sz / PAGE;
	void **pages = malloc(n * sizeof(*pages));
	int *status = malloc(n * sizeof(*status));
	char val = 0, *p;
	int fd, ok;

	set_placement(mode);
	p = map(b, sz, &fd);
	madvise(p, sz,
		MADV_RANDOM); /* readahead would place neighbours together */
	ok = run_steps(p, "wL r*", &val);
	for (i = 0; i < n; i++)
		pages[i] = p + i * PAGE;
	/* no target nodes: move_pages only reports the node of main's pages */
	ok = ok && !syscall(SYS_move_pages, 0, n, pages, NULL, status, 0);
	for (i = 0; ok && i < n; i++) {
		if (status[i] == want_node(b, mode, pages[i], i))
			continue;
		ksft_print_msg("%s page %lu: node %d, want %d\n", mode, i,
			       status[i], want_node(b, mode, pages[i], i));
		ok = 0;
	}
	unmap(p, sz, fd);
	free(pages);
	free(status);
	set_placement("bound");
	return ok;
}

int main(int argc, char **argv)
{
	unsigned int b, i;

	repl_test_init(argc, argv);
	ksft_set_plan(ARRAY_SIZE(backings) * (5 + ARRAY_SIZE(protocols)));
	for (b = 0; b < ARRAY_SIZE(backings); b++) {
		for (i = 0; i < ARRAY_SIZE(protocols); i++)
			run(test_protocol, &backings[b], protocols[i]);
		run(test_stress, &backings[b],
		    "threads read, write and allocate");
		run(test_stress_migrate, &backings[b],
		    "threads read, write and allocate while main migrates");
		run(test_main_placement, &backings[b], "bound");
		run(test_main_placement, &backings[b], "first_touch");
		run(test_main_placement, &backings[b], "interleave");
	}
	ksft_finished();
}
