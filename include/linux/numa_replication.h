/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_NUMA_REPLICATION_H
#define _LINUX_NUMA_REPLICATION_H

#include <linux/mm.h>

#if MAX_NUMNODES < 16
#define REPL_MAX_NODES MAX_NUMNODES
#else
#define REPL_MAX_NODES 16
#endif

struct mm_repl {
	nodemask_t nodes;
	pgd_t *pgds[REPL_MAX_NODES];
	unsigned long window; /* 512GB, 1 PGD */
};

#ifdef CONFIG_NUMA_REPL
#define for_each_repl_pgd(mm, nid, pgd)                             \
	for (nid = first_node((mm)->repl->nodes);                   \
	     nid < MAX_NUMNODES && ((pgd) = (mm)->repl->pgds[nid]); \
	     nid = next_node(nid, (mm)->repl->nodes))

static inline bool repl_supported(void)
{
	return !pgtable_l5_enabled() && !boot_cpu_has(X86_FEATURE_PTI) &&
	       nr_node_ids <= REPL_MAX_NODES;
}

static inline bool repl_enabled(struct mm_struct *mm)
{
	return mm_flags_test(MMF_NUMA_REPL, mm);
}

static inline bool repl_has_pgds(struct mm_struct *mm)
{
	return mm->repl != NULL;
}

static inline pgd_t *repl_get_numa_pgd(struct mm_struct *mm)
{
	pgd_t *pgd;

	if (!repl_has_pgds(mm))
		return mm->pgd;

	pgd = mm->repl->pgds[numa_node_id()];
	return pgd ? pgd : mm->pgd;
}

void repl_mm_init(struct mm_struct *mm);
pgd_t *repl_pgds_alloc(struct mm_struct *mm);
void repl_pgds_free(struct mm_struct *mm);
void repl_p4ds_populate(struct mm_struct *mm, p4d_t *p4d, pud_t *pud,
			unsigned long address);
void repl_p4ds_clear(struct mm_struct *mm, p4d_t *p4d, unsigned long address);

void repl_window_init(struct mm_struct *mm);
void repl_window_refill(struct mm_struct *mm, unsigned long start,
			unsigned long end);
unsigned long repl_get_window_area(struct mm_struct *mm, unsigned long len);
static inline bool addr_is_replicated(struct mm_struct *mm, unsigned long addr)
{
	return mm->repl && mm->repl->window &&
	       addr - mm->repl->window < PGDIR_SIZE;
}
#else
/* clang-format off */
static inline bool repl_supported(void) { return false; }
static inline bool repl_enabled(struct mm_struct *mm) { return false; }
static inline bool repl_has_pgds(struct mm_struct *mm) { return false; }
static inline pgd_t *repl_get_numa_pgd(struct mm_struct *mm) { return mm->pgd; }

static inline void repl_mm_init(struct mm_struct *mm) {}
static inline pgd_t *repl_pgds_alloc(struct mm_struct *mm) { return NULL; }
static inline void repl_pgds_free(struct mm_struct *mm) {}
static inline void repl_p4ds_populate(struct mm_struct *mm, p4d_t *p4d, pud_t *pud,
			unsigned long address) {}
static inline void repl_p4ds_clear(struct mm_struct *mm, p4d_t *p4d, unsigned long address) {}
static inline void repl_window_init(struct mm_struct *mm) {}
static inline void repl_window_refill(struct mm_struct *mm, unsigned long start,
			unsigned long end) {}
static inline unsigned long repl_get_window_area(struct mm_struct *mm, unsigned long len) { return 0; }
static inline bool addr_is_replicated(struct mm_struct *mm, unsigned long addr) { return false; }
/* clang-format on */
#endif

#endif /* _LINUX_NUMA_REPLICATION_H */
