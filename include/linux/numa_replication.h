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
	int main_nid;
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
void repl_free_p4d_range(struct mmu_gather *tlb, unsigned long addr,
			 unsigned long end, unsigned long floor,
			 unsigned long ceiling);

void repl_window_init(struct mm_struct *mm);
void repl_window_refill(struct mm_struct *mm, unsigned long start,
			unsigned long end);
unsigned long repl_get_window_area(struct mm_struct *mm, unsigned long len);
bool repl_mmap_eligible(struct mm_struct *mm, struct file *file,
			unsigned long flags);
bool repl_file_replicated(struct file *file, unsigned long flags);
static inline bool addr_is_replicated(struct mm_struct *mm, unsigned long addr)
{
	return mm->repl && mm->repl->window &&
	       addr - mm->repl->window < PGDIR_SIZE;
}

bool repl_zap_replicas(struct mmu_gather *tlb, struct vm_area_struct *vma,
		       unsigned long addr, pte_t *mainp);
void repl_unmap_main(struct vm_area_struct *vma, unsigned long addr,
		     pte_t *mainp, bool shared_only);
pte_t repl_migrate_done(struct vm_area_struct *vma, unsigned long addr,
			pte_t pte, bool writable);
void repl_mkclean_main(struct vm_area_struct *vma, unsigned long addr,
		       pte_t *mainp);

void repl_sync_folio(struct folio *folio);
void repl_invalidate_folio(struct folio *folio);
void repl_read_folio(struct folio *folio);

vm_fault_t repl_handle_mm_fault(struct vm_area_struct *vma,
				unsigned long address, unsigned int flags);

struct seq_file;
void repl_stat_show(struct seq_file *m, struct mm_struct *mm);
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
static inline void repl_free_p4d_range(struct mmu_gather *tlb, unsigned long addr,
			 unsigned long end, unsigned long floor,
			 unsigned long ceiling) {}
static inline void repl_window_init(struct mm_struct *mm) {}
static inline void repl_window_refill(struct mm_struct *mm, unsigned long start,
			unsigned long end) {}
static inline bool repl_mmap_eligible(struct mm_struct *mm, struct file *file,
			unsigned long flags) { return false; }
static inline unsigned long repl_get_window_area(struct mm_struct *mm, unsigned long len) { return 0; }
static inline bool repl_file_replicated(struct file *file, unsigned long flags) { return false; }
static inline bool addr_is_replicated(struct mm_struct *mm, unsigned long addr) { return false; }

static inline bool repl_zap_replicas(struct mmu_gather *tlb, struct vm_area_struct *vma,
		       unsigned long addr, pte_t *mainp) { return false; }
static inline void repl_unmap_main(struct vm_area_struct *vma, unsigned long addr,
		     pte_t *mainp, bool shared_only) {}
static inline pte_t repl_migrate_done(struct vm_area_struct *vma,
		unsigned long addr, pte_t pte, bool writable) { return pte; }
static inline void repl_mkclean_main(struct vm_area_struct *vma, unsigned long addr,
		       pte_t *mainp) {}

static inline void repl_sync_folio(struct folio *folio) {}
static inline void repl_invalidate_folio(struct folio *folio) {}
static inline void repl_read_folio(struct folio *folio) {}

static inline vm_fault_t repl_handle_mm_fault(struct vm_area_struct *vma,
				unsigned long address, unsigned int flags) { return 0; }
/* clang-format on */
#endif

#endif /* _LINUX_NUMA_REPLICATION_H */
